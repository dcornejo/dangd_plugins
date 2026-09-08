// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/** @file Self-restoring transaction diagnostic for an isolated FRR instance. */

#include "frr_config.h"
#include "frr_transaction.h"
#include "mgmtd_session.h"
#include "mgmtd_transport.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <unistd.h>

namespace {

using dang::plugins::frr::mgmtd::Session;

std::unique_ptr<Session> Open(std::string_view socket,
                              std::atomic<std::uint64_t>* next,
                              std::string* error) {
  auto transport = dang::plugins::frr::mgmtd::Transport::Connect(
      socket, std::chrono::seconds(5), error);
  if (!transport) return nullptr;
  return Session::Open(std::move(transport), next->fetch_add(1),
                       "dangd-frr-isolated-mutation", error);
}

std::optional<std::string> Read(std::string_view socket,
                                std::string_view xpath,
                                std::atomic<std::uint64_t>* next,
                                std::string* error) {
  auto session = Open(socket, next, error);
  if (!session) return std::nullopt;
  auto value = session->GetRunningConfiguration(xpath, error);
  std::string close_error;
  if (!session->Close(&close_error)) {
    if (error && error->empty()) *error = std::move(close_error);
    return std::nullopt;
  }
  return value;
}

}  // namespace

int main(int argc, char** argv) {
  if ((argc != 6 && argc != 8) ||
      std::string_view(argv[1]) != "--allow-isolated-test") {
    std::cerr << "usage: frr-mgmtd-mutation-check --allow-isolated-test "
                 "SOCKET XPATH AFTER_XML EXPECTED_TEXT "
                 "[OPERATIONAL_XPATH EXPECTED_OPERATIONAL_TEXT]\n";
    return 2;
  }
  const std::string socket = argv[2];
  const std::filesystem::path socket_path(socket);
  if (socket_path.filename() != "mgmtd_fe.sock" ||
      !socket_path.parent_path().filename().string().starts_with(
          "dangd-test-")) {
    std::cerr << "refusing mutation outside a dangd-test FRR pathspace\n";
    return 2;
  }
  const std::string xpath = argv[3];
  const std::string after = argv[4];
  const std::string expected_text = argv[5];
  std::atomic<std::uint64_t> next(
      static_cast<std::uint64_t>(getpid()) << 32u | 1u);
  std::string error;
  auto before = Read(socket, xpath, &next, &error);
  if (!before) {
    std::cerr << "read before: " << error << '\n';
    return 1;
  }
  dang::plugins::frr::SessionFactory factory =
      [&](std::string* failure)
          -> std::unique_ptr<dang::plugins::frr::mgmtd::SessionOperations> {
        return Open(socket, &next, failure);
      };
  dang::plugins::frr::FrrTransaction transaction(
      std::move(factory), {{xpath, before->empty()
                                      ? std::nullopt
                                      : std::optional<std::string>(*before),
                            after}});
  if (!transaction.Validate(&error)) {
    std::cerr << "mutation: " << error << '\n';
    return 1;
  }
  if (!transaction.Apply(&error)) {
    const std::string apply_error = error;
    if (transaction.applied()) {
      std::string rollback_error;
      if (!transaction.Rollback(&rollback_error)) {
        std::cerr << "mutation: " << apply_error
                  << "; rollback: " << rollback_error << '\n';
        return 1;
      }
      std::cerr << "mutation rejected after commit and restored: "
                << apply_error << '\n';
      return 3;
    }
    std::cerr << "mutation: " << apply_error << '\n';
    return 1;
  }
  auto observed = Read(socket, xpath, &next, &error);
  const bool applied = observed && !expected_text.empty() &&
      observed->find(expected_text) != std::string::npos;
  bool operational = true;
  std::optional<std::string> operational_xml;
  if (argc == 8) {
    auto session = Open(socket, &next, &error);
    if (session) {
      operational_xml = session->GetOperationalData(argv[6], &error);
      std::string close_error;
      if (!session->Close(&close_error)) {
        if (error.empty()) error = std::move(close_error);
        operational_xml.reset();
      }
    }
    operational = operational_xml && argv[7][0] != '\0' &&
        operational_xml->find(argv[7]) != std::string::npos;
  }

  // Restoration is attempted regardless of observation success. A caller must
  // treat any nonzero result as requiring inspection of the disposable daemon.
  const bool restored = transaction.Rollback(&error);
  auto final = Read(socket, xpath, &next, &error);
  auto equivalent = final
      ? dang::plugins::frr::EquivalentConfigurationRoot(
            before->empty() ? std::nullopt
                            : std::optional<std::string>(*before),
            final->empty() ? std::nullopt
                           : std::optional<std::string>(*final),
            &error)
      : std::nullopt;
  if (!applied || !operational || !restored || !equivalent || !*equivalent) {
    std::cerr << "round trip failed"
              << " (applied=" << (applied ? "yes" : "no")
              << ", restored=" << (restored ? "yes" : "no")
              << ", operational=" << (operational ? "yes" : "no")
              << ", equivalent="
              << (equivalent ? (*equivalent ? "yes" : "no") : "unavailable")
              << "): "
              << (error.empty() ? "running state did not match" : error)
              << "\nbefore: " << (before->empty() ? "<empty>" : *before)
              << "\napplied readback: "
              << (observed ? (observed->empty() ? "<empty>" : *observed)
                           : "<unavailable>")
              << "\noperational readback: "
              << (operational_xml
                      ? (operational_xml->empty() ? "<empty>" : *operational_xml)
                      : (argc == 8 ? "<unavailable>" : "<not requested>"))
              << "\nfinal readback: "
              << (final ? (final->empty() ? "<empty>" : *final)
                        : "<unavailable>")
              << '\n';
    return 1;
  }
  std::cout << "validated, committed, observed";
  if (argc == 8) std::cout << ", read operational state";
  std::cout << ", and restored " << xpath << '\n';
  return 0;
}

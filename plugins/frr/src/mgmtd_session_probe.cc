// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/** @file Live diagnostic that creates and cleanly destroys one mgmtd session. */

#include "mgmtd_session.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#include <unistd.h>

int main(int argc, char** argv) {
  const std::string_view mode = argc >= 3 ? argv[2] : "";
  if (argc < 2 || argc > 5 ||
      (argc >= 3 && mode != "--operational" && mode != "--running" &&
       mode != "--rpc" && mode != "--notify" && mode != "--replace") ||
      (mode == "--running" && argc != 4) ||
      (mode == "--notify" && argc != 4) ||
      (mode == "--replace" && argc != 5) ||
      (mode == "--rpc" && (argc < 4 || argc > 5))) {
    std::cerr <<
        "usage: frr-mgmtd-session-check SOCKET "
        "[--operational [XPATH] | --running XPATH | --rpc XPATH [XML] | "
        "--notify XPATH | --replace XPATH XML]\n";
    return 2;
  }
  if (mode == "--replace") {
    const std::filesystem::path socket_path(argv[1]);
    const std::filesystem::path parent = socket_path.parent_path();
    std::error_code filesystem_error;
    const auto parent_status = std::filesystem::symlink_status(
        parent, filesystem_error);
    const auto socket_status = std::filesystem::status(
        socket_path, filesystem_error);
    const std::filesystem::path grandparent =
        std::filesystem::weakly_canonical(parent.parent_path(),
                                           filesystem_error);
    const bool recognized_frr_run_directory =
        grandparent == "/var/run/frr" ||
        grandparent == "/usr/local/var/run/frr";
    if (filesystem_error || socket_path.filename() != "mgmtd_fe.sock" ||
        !parent.filename().string().starts_with("dang-notify-") ||
        !recognized_frr_run_directory ||
        std::filesystem::is_symlink(parent_status) ||
        !std::filesystem::is_directory(parent_status) ||
        !std::filesystem::is_socket(socket_status)) {
      std::cerr <<
          "refusing mutation outside a real disposable dang-notify FRR "
          "pathspace\n";
      return 2;
    }
  }
  std::string error;
  auto transport = dang::plugins::frr::mgmtd::Transport::Connect(
      argv[1], std::chrono::seconds(2), &error);
  if (!transport) {
    std::cerr << error << '\n';
    return 1;
  }
  const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
  std::uint64_t client_id = static_cast<std::uint64_t>(ticks) ^
      static_cast<std::uint64_t>(getpid());
  if (client_id == 0) client_id = 1;
  auto session = dang::plugins::frr::mgmtd::Session::Open(
      std::move(transport), client_id, "dangd-frr-session-check", &error);
  if (!session) {
    std::cerr << error << '\n';
    return 1;
  }
  const std::uint64_t session_id = session->id();
  std::optional<std::string> operational;
  if (argc >= 3) {
    if (mode == "--replace") {
      if (!session->LockCandidate(&error) ||
          !session->ReplaceCandidate(argv[3], argv[4], &error) ||
          !session->ValidateCandidate(&error) ||
          !session->ApplyCandidate(&error) ||
          !session->UnlockCandidate(&error)) {
        std::cerr << error << '\n';
        return 1;
      }
    } else if (mode == "--notify") {
      const std::string_view selector = argv[3];
      if (!session->SelectNotifications(
              std::span<const std::string_view>(&selector, 1), &error)) {
        std::cerr << error << '\n';
        return 1;
      }
      bool timed_out = false;
      auto event = session->NextNotification(&timed_out, &error);
      if (!event) {
        std::cerr << (timed_out ? "notification wait timed out" : error) << '\n';
        return 1;
      }
      operational = event->xpath + "\n" + event->xml;
    } else if (mode == "--running")
      operational = session->GetRunningConfiguration(argv[3], &error);
    else if (mode == "--rpc")
      operational = session->InvokeRpc(argv[3], argc == 5 ? argv[4] : "",
                                       &error);
    else
      operational = session->GetOperationalData(argc == 4 ? argv[3] : "/*",
                                                &error);
    if (mode != "--replace" && !operational) {
      std::cerr << error << '\n';
      (void)session->Close(&error);
      return 1;
    }
  }
  if (!session->Close(&error)) {
    std::cerr << error << '\n';
    return 1;
  }
  if (operational)
    std::cout << *operational << '\n';
  else
    std::cout << "created and destroyed mgmtd session " << session_id << '\n';
  return 0;
}

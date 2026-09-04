// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/** @file Live diagnostic that creates and cleanly destroys one mgmtd session. */

#include "mgmtd_session.h"

#include <chrono>
#include <cstdint>
#include <iostream>

#include <unistd.h>

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: frr-mgmtd-session-check SOCKET\n";
    return 2;
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
  if (!session->Close(&error)) {
    std::cerr << error << '\n';
    return 1;
  }
  std::cout << "created and destroyed mgmtd session " << session_id << '\n';
  return 0;
}

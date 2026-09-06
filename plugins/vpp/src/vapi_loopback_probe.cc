// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/** @file Live VAPI loopback create, enable, and rollback validation. */

#include "plugins/vpp/src/loopback_transaction.h"
#include "plugins/vpp/src/vapi_vpp_client.h"

#include <iostream>
#include <string>

int main(int argc, char** argv) {
  const std::string socket_path =
      argc > 1 ? argv[1] : "/run/vpp/api.sock";
  std::string error;
  auto client = dang::vpp::VapiVppClient::Connect(socket_path, &error);
  if (!client) {
    std::cerr << error << '\n';
    return 1;
  }
  dang::vpp::LoopbackTransaction transaction(client.get());
  if (!transaction.Apply(&error)) {
    std::cerr << error << '\n';
    return 1;
  }
  std::cout << "created and enabled " << transaction.created()->name << '\n';
  if (!transaction.Rollback(&error)) {
    std::cerr << "rollback failed: " << error << '\n';
    return 1;
  }
  std::cout << "restored pre-test state\n";
}

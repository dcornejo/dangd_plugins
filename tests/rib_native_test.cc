// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "plugins/rib/src/platform_executor.h"

#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
  if (argc != 6 && argc != 7) {
    std::cerr << "usage: rib_native_test linux|freebsd install|delete "
                 "RIB PREFIX INTERFACE [GATEWAY]\n";
    return 2;
  }
  const std::string_view platform_name(argv[1]);
  const std::string_view operation(argv[2]);
  if ((platform_name != "linux" && platform_name != "freebsd") ||
      (operation != "install" && operation != "delete"))
    return 2;
  dang::rib::Route route;
  route.routing_instance = "native-test";
  route.rib = argv[3];
  route.address_family = std::string_view(argv[4]).find(':') ==
                                 std::string_view::npos
                             ? "ipv4"
                             : "ipv6";
  route.destination = argv[4];
  route.interface = argv[5];
  if (argc == 7) route.gateway = argv[6];
  route.preference = 10;
  const dang::rib::Change change{
      operation == "install" ? dang::rib::ChangeKind::kInstall
                             : dang::rib::ChangeKind::kDelete,
      route};
  const auto result = dang::rib::ExecuteChanges(
      platform_name == "linux" ? dang::rib::NativePlatform::kLinux
                               : dang::rib::NativePlatform::kFreeBsd,
      {change});
  if (result.ok) return 0;
  std::cerr << result.error << " (" << result.error_path << ")\n";
  for (const auto& failure : result.rollback_failures)
    std::cerr << "rollback: " << failure << '\n';
  return 1;
}

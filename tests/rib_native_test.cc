// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "plugins/rib/src/platform_executor.h"
#include "plugins/rib/src/route_observer.h"

#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

int main(int argc, char** argv) {
  if (argc < 6 || argc > 7) {
    std::cerr << "usage: rib_native_test linux|freebsd install|delete "
                 "RIB PREFIX INTERFACE [GATEWAY]\n"
                 "       rib_native_test linux observe-multipath "
                 "RIB PREFIX INTERFACE INTERFACE\n";
    return 2;
  }
  const std::string_view platform_name(argv[1]);
  const std::string_view operation(argv[2]);
  if ((platform_name != "linux" && platform_name != "freebsd") ||
      (operation != "install" && operation != "delete" &&
       operation != "observe-multipath"))
    return 2;
  if (operation == "observe-multipath") {
    if (platform_name != "linux" || argc != 7) return 2;
    std::vector<dang::rib::ObservedRoute> routes;
    std::string error;
    if (!dang::rib::ObserveLinuxRoutes(&routes, &error)) {
      std::cerr << error << '\n';
      return 1;
    }
    std::set<std::string> interfaces;
    std::vector<dang::rib::ObservedRoute> matching;
    for (const auto& route : routes) {
      if (route.route.rib != argv[3] || route.route.destination != argv[4])
        continue;
      if (route.route.interface) interfaces.insert(*route.route.interface);
      matching.push_back(route);
    }
    const std::string operational =
        dang::rib::SerializeOperationalRoutes(matching);
    const bool ok = matching.size() == 2U && interfaces.size() == 2U &&
        interfaces.contains(argv[5]) && interfaces.contains(argv[6]) &&
        operational.find(argv[5]) != std::string::npos &&
        operational.find(argv[6]) != std::string::npos;
    if (!ok)
      std::cerr << "multipath observation mismatch: " << operational << '\n';
    return ok ? 0 : 1;
  }
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

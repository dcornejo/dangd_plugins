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
                 "       rib_native_test linux|freebsd "
                 "install-special|delete-special|observe-special "
                 "RIB PREFIX discard|discard-with-error\n"
                 "       rib_native_test linux observe-multipath "
                 "RIB PREFIX INTERFACE INTERFACE\n"
                 "       rib_native_test linux observe-nexthop-group "
                 "RIB PREFIX INTERFACE INTERFACE\n"
                 "       rib_native_test freebsd observe-weighted-multipath "
                 "RIB PREFIX INTERFACE INTERFACE\n";
    return 2;
  }
  const std::string_view platform_name(argv[1]);
  const std::string_view operation(argv[2]);
  if ((platform_name != "linux" && platform_name != "freebsd") ||
      (operation != "install" && operation != "delete" &&
       operation != "install-special" &&
       operation != "delete-special" &&
       operation != "observe-special" &&
       operation != "observe-multipath" &&
       operation != "observe-nexthop-group" &&
       operation != "observe-weighted-multipath"))
    return 2;
  if (operation == "observe-special") {
    if (argc != 6 ||
        (std::string_view(argv[5]) != "discard" &&
         std::string_view(argv[5]) != "discard-with-error"))
      return 2;
    std::vector<dang::rib::ObservedRoute> routes;
    std::string error;
    const bool observed = platform_name == "linux"
        ? dang::rib::ObserveLinuxRoutes(&routes, &error)
        : dang::rib::ObserveFreeBsdRoutesForFib(
              static_cast<std::uint32_t>(std::stoul(argv[3])), &routes,
              &error);
    if (!observed) {
      std::cerr << error << '\n';
      return 1;
    }
    for (const auto& route : routes) {
      if (route.route.rib == argv[3] && route.route.destination == argv[4] &&
          route.route.special == argv[5] && route.mutable_route) {
        const std::string xml =
            dang::rib::SerializeOperationalRoutes({route});
        if (xml.find("<special>" + std::string(argv[5]) + "</special>") !=
            std::string::npos)
          return 0;
      }
    }
    std::cerr << "special route observation mismatch\n";
    return 1;
  }
  if (operation == "observe-multipath" ||
      operation == "observe-nexthop-group" ||
      operation == "observe-weighted-multipath") {
    if (argc != 7 ||
        (operation == "observe-weighted-multipath"
             ? platform_name != "freebsd"
             : platform_name != "linux"))
      return 2;
    std::vector<dang::rib::ObservedRoute> routes;
    std::string error;
    const bool observed = platform_name == "linux"
        ? dang::rib::ObserveLinuxRoutes(&routes, &error)
        : dang::rib::ObserveFreeBsdRoutesForFib(
              static_cast<std::uint32_t>(std::stoul(argv[3])), &routes,
              &error);
    if (!observed) {
      std::cerr << error << '\n';
      return 1;
    }
    std::set<std::string> interfaces;
    std::multiset<std::uint32_t> weights;
    std::vector<dang::rib::ObservedRoute> matching;
    for (const auto& route : routes) {
      if (route.route.rib != argv[3] || route.route.destination != argv[4])
        continue;
      if (route.route.interface) interfaces.insert(*route.route.interface);
      if (route.weight) weights.insert(*route.weight);
      matching.push_back(route);
    }
    const std::string operational =
        dang::rib::SerializeOperationalRoutes(matching);
    bool mutability_ok = true;
    const bool expected_mutable = operation != "observe-nexthop-group";
    for (const auto& route : matching) {
      if (route.mutable_route != expected_mutable) mutability_ok = false;
    }
    const bool ok = matching.size() == 2U && interfaces.size() == 2U &&
        interfaces.contains(argv[5]) && interfaces.contains(argv[6]) &&
        weights == (operation == "observe-multipath"
                        ? std::multiset<std::uint32_t>{1U, 1U}
                        : std::multiset<std::uint32_t>{2U, 3U}) &&
        operational.find(argv[5]) != std::string::npos &&
        operational.find(argv[6]) != std::string::npos && mutability_ok;
    if (!ok)
      std::cerr << "multipath path or weight observation mismatch: "
                << operational << '\n';
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
  const bool special = operation == "install-special" ||
                       operation == "delete-special";
  if (special) {
    if (argc != 6 ||
        (std::string_view(argv[5]) != "discard" &&
         std::string_view(argv[5]) != "discard-with-error"))
      return 2;
    route.special = argv[5];
  } else {
    route.interface = argv[5];
    if (argc == 7) route.gateway = argv[6];
  }
  route.preference = 10;
  const dang::rib::Change change{
      operation == "install" || operation == "install-special"
          ? dang::rib::ChangeKind::kInstall
          : dang::rib::ChangeKind::kDelete,
      route};
  const auto result = dang::rib::ExecuteChanges(
      platform_name == "linux" ? dang::rib::NativePlatform::kLinux
                               : dang::rib::NativePlatform::kFreeBsd,
      {change});
  if (result.ok &&
      (operation == "install" || operation == "install-special")) {
    std::vector<dang::rib::ObservedRoute> routes;
    std::string observation_error;
    const bool observed = platform_name == "linux"
        ? dang::rib::ObserveLinuxRoutes(&routes, &observation_error)
        : dang::rib::ObserveFreeBsdRoutesForFib(
              static_cast<std::uint32_t>(std::stoul(argv[3])), &routes,
              &observation_error);
    if (!observed) {
      std::cerr << observation_error << '\n';
      return 1;
    }
    for (const auto& candidate : routes)
      if (candidate.route.rib == route.rib &&
          candidate.route.destination == route.destination &&
          candidate.route.preference == route.preference)
        return 0;
    std::cerr << "installed route preference did not round-trip: expected "
              << route.preference << '\n';
    return 1;
  }
  if (result.ok) return 0;
  std::cerr << result.error << " (" << result.error_path << ")\n";
  for (const auto& failure : result.rollback_failures)
    std::cerr << "rollback: " << failure << '\n';
  return 1;
}

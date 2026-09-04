// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "plugins/rib/src/platform_command.h"

#include <charconv>

namespace dang::rib {
namespace {

bool SafeFibAndInterface(const Route& route, std::string* error,
                         std::string* path) {
  unsigned fib = 0;
  const auto parsed = std::from_chars(route.rib.data(),
                                      route.rib.data() + route.rib.size(), fib);
  if (parsed.ec != std::errc{} || parsed.ptr != route.rib.data() + route.rib.size()) {
    *error = "FreeBSD backend currently requires a numeric RIB name (FIB number)";
    *path = "/ietf-i2rs-rib:routing-instance/rib-list/name";
    return false;
  }
  if (route.interface && (route.interface->empty() || route.interface->front() == '-')) {
    *error = "unsafe outgoing interface name";
    *path = "/ietf-i2rs-rib:routing-instance/rib-list/route-list/nexthop";
    return false;
  }
  return true;
}

}  // namespace

bool BuildFreeBsdCommands(const std::vector<Change>& changes,
                          std::vector<NativeCommand>* commands,
                          std::string* error, std::string* error_path) {
  if (!commands || !error || !error_path) return false;
  commands->clear();
  for (const Change& change : changes) {
    if (!SafeFibAndInterface(change.route, error, error_path)) return false;
    NativeCommand command;
    command.arguments = {"route", "-n",
                         change.kind == ChangeKind::kDelete ? "delete" : "add",
                         change.route.address_family == "ipv4" ? "-inet" : "-inet6",
                         "-fib", change.route.rib, change.route.destination};
    if (change.route.gateway) {
      command.arguments.push_back(*change.route.gateway);
      if (change.route.interface)
        command.arguments.insert(command.arguments.end(),
                                 {"-ifp", *change.route.interface});
    } else if (change.route.interface) {
      // Ethernet interface routes require the interface's local address as
      // the route(8) gateway argument. Resolving that address belongs in the
      // later observed-state adapter; guessing from only an interface-ref can
      // select the wrong address on a multihomed interface.
      *error = "FreeBSD interface-only nexthops require native interface "
               "address resolution";
      *error_path =
          "/ietf-i2rs-rib:routing-instance/rib-list/route-list/nexthop";
      commands->clear();
      return false;
    }
    commands->push_back(std::move(command));
  }
  return true;
}

}  // namespace dang::rib

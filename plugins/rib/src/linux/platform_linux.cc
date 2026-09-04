// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/** @file Linux iproute2 mapping for the portable RFC 8431 route plan. */

#include "plugins/rib/src/platform_command.h"

#include <charconv>

namespace dang::rib {
namespace {

bool SafeTableAndInterface(const Route& route, std::string* error,
                           std::string* path) {
  unsigned table = 0;
  const auto parsed = std::from_chars(route.rib.data(),
                                      route.rib.data() + route.rib.size(), table);
  if (parsed.ec != std::errc{} || parsed.ptr != route.rib.data() + route.rib.size() ||
      table == 0) {
    *error = "Linux backend currently requires a positive numeric RIB name";
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

bool BuildLinuxCommands(const std::vector<Change>& changes,
                        std::vector<NativeCommand>* commands,
                        std::string* error, std::string* error_path) {
  if (!commands || !error || !error_path) return false;
  commands->clear();
  for (const Change& change : changes) {
    if (!SafeTableAndInterface(change.route, error, error_path)) return false;
    NativeCommand command;
    command.arguments = {"ip", change.route.address_family == "ipv4" ? "-4" : "-6",
                         "route",
                         change.kind == ChangeKind::kDelete ? "delete" : "replace",
                         change.route.destination, "table", change.route.rib};
    if (change.route.gateway)
      command.arguments.insert(command.arguments.end(), {"via", *change.route.gateway});
    if (change.route.interface)
      command.arguments.insert(command.arguments.end(), {"dev", *change.route.interface});
    if (change.kind == ChangeKind::kInstall)
      command.arguments.insert(command.arguments.end(),
                               {"metric", std::to_string(change.route.preference),
                                "proto", "static"});
    commands->push_back(std::move(command));
  }
  return true;
}

}  // namespace dang::rib

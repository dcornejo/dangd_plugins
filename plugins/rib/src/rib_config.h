// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANG_PLUGINS_RIB_CONFIG_H_
#define DANG_PLUGINS_RIB_CONFIG_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dang::rib {

/**
 * A deliberately small, portable projection of one RFC 8431 route.
 *
 * The first backend slice accepts destination-prefix IPv4 and IPv6 routes
 * with a base nexthop expressed as a gateway, interface, or both. Keeping the
 * projection narrower than the YANG tree makes unsupported forwarding
 * semantics fail during prepare instead of being silently approximated by a
 * host routing API.
 */
struct Route {
  std::string routing_instance;
  std::string rib;
  std::string address_family;
  std::uint64_t index = 0;
  std::string destination;
  std::optional<std::string> gateway;
  std::optional<std::string> interface;
  std::uint32_t preference = 0;
  bool local_only = false;

  bool operator==(const Route&) const = default;
};

struct Config {
  std::vector<Route> routes;
};

enum class ChangeKind { kDelete, kInstall };

struct Change {
  ChangeKind kind = ChangeKind::kInstall;
  Route route;
};

/** Extracts the supported RFC 8431 subtree from a complete datastore. */
[[nodiscard]] bool ParseConfig(const char* xml, Config* config,
                               std::string* error, std::string* error_path);

/**
 * Produces a stable replacement plan. Deletions precede installations so a
 * changed route never temporarily competes with its obsolete incarnation.
 */
[[nodiscard]] std::vector<Change> PlanChanges(const Config& before,
                                               const Config& proposed);

/** Human-readable transaction evidence used by tests and dry-run logging. */
[[nodiscard]] std::string Describe(const Change& change);

}  // namespace dang::rib

#endif  // DANG_PLUGINS_RIB_CONFIG_H_

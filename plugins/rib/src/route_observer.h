// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANG_PLUGINS_RIB_ROUTE_OBSERVER_H_
#define DANG_PLUGINS_RIB_ROUTE_OBSERVER_H_

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "plugins/rib/src/rib_config.h"
#include "plugins/rib/src/rib_persistence.h"

namespace dang::rib {

/** One route read from the host forwarding plane. */
struct ObservedRoute {
  ObservedRoute() = default;
  ObservedRoute(const Route& value, bool is_installed)
      : route(value), installed(is_installed) {}

  Route route;
  bool installed = true;
  /** False when observation is safe but native mutation is not implemented. */
  bool mutable_route = true;
  /** RFC 8431 reason when the native state exposes one without inference. */
  std::optional<std::string> reason;
};

/**
 * Tracks a native RIB snapshot and reports additions, removals, and changes.
 *
 * The first observation establishes a quiet baseline. Managed changes update
 * an established baseline immediately so their later kernel observation does
 * not generate a duplicate event.
 */
class RouteChangeTracker {
 public:
  [[nodiscard]] std::vector<ObservedRoute> Observe(
      const std::vector<ObservedRoute>& routes);
  void ApplyManaged(const Route& route, bool installed);

 private:
  using Key = std::tuple<std::string, std::string, std::string, std::string,
                         std::string, std::string>;
  std::map<Key, ObservedRoute> routes_;
  bool initialized_ = false;
};

struct NexthopResolutionChange {
  PersistentNexthop nexthop;
  bool resolved = false;
};

/** Derives reusable-nexthop resolution transitions from installed bindings. */
class NexthopResolutionTracker {
 public:
  [[nodiscard]] std::vector<NexthopResolutionChange> Observe(
      const PersistentRegistry& registry,
      const std::vector<ObservedRoute>& routes);

 private:
  std::map<std::pair<std::string, std::uint32_t>, bool> states_;
};

/** Reads Linux routes with rtnetlink. */
[[nodiscard]] bool ObserveLinuxRoutes(std::vector<ObservedRoute>* routes,
                                      std::string* error);

/** Reads FreeBSD routes with the routing sysctl API. */
[[nodiscard]] bool ObserveFreeBsdRoutes(std::vector<ObservedRoute>* routes,
                                        std::string* error);

/** Reads one numbered FreeBSD FIB with the routing sysctl API. */
[[nodiscard]] bool ObserveFreeBsdRoutesForFib(
    std::uint32_t fib, std::vector<ObservedRoute>* routes,
    std::string* error);

/**
 * Serializes a partial RFC 8431 operational-data subtree.
 *
 * Durable RIB registrations are supplied with the native route inventory so a
 * successful rib-add remains visible even before its first route or nexthop.
 */
[[nodiscard]] std::string SerializeOperationalRoutes(
    const std::vector<ObservedRoute>& routes,
    const PersistentRegistry& registry = {});

/** Serializes one RFC 8431 route-change event without an RFC 5277 wrapper. */
[[nodiscard]] std::string SerializeRouteChange(const Route& route,
                                               bool installed,
                                               std::string_view reason = {});

/** Serializes one RFC 8431 reusable-nexthop resolution transition. */
[[nodiscard]] std::string SerializeNexthopResolutionChange(
    const PersistentNexthop& nexthop, bool resolved);

}  // namespace dang::rib

#endif  // DANG_PLUGINS_RIB_ROUTE_OBSERVER_H_

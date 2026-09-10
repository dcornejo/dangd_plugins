// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANG_PLUGINS_RIB_ROUTE_OBSERVER_H_
#define DANG_PLUGINS_RIB_ROUTE_OBSERVER_H_

#include <string>
#include <map>
#include <tuple>
#include <vector>

#include "plugins/rib/src/rib_config.h"

namespace dang::rib {

/** One route read from the host forwarding plane. */
struct ObservedRoute {
  Route route;
  bool installed = true;
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
  using Key = std::tuple<std::string, std::string, std::string>;
  std::map<Key, ObservedRoute> routes_;
  bool initialized_ = false;
};

/** Reads Linux routes with rtnetlink. */
[[nodiscard]] bool ObserveLinuxRoutes(std::vector<ObservedRoute>* routes,
                                      std::string* error);

/** Reads FreeBSD routes with the routing sysctl API. */
[[nodiscard]] bool ObserveFreeBsdRoutes(std::vector<ObservedRoute>* routes,
                                        std::string* error);

/** Serializes a partial RFC 8431 operational-data subtree. */
[[nodiscard]] std::string SerializeOperationalRoutes(
    const std::vector<ObservedRoute>& routes,
    const std::vector<std::tuple<std::string, std::string, std::uint32_t>>&
        nexthops = {});

/** Serializes one RFC 8431 route-change event without an RFC 5277 wrapper. */
[[nodiscard]] std::string SerializeRouteChange(const Route& route,
                                               bool installed);

}  // namespace dang::rib

#endif  // DANG_PLUGINS_RIB_ROUTE_OBSERVER_H_

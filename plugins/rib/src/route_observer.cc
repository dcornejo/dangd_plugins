// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/** @file RFC 8431 XML serialization shared by native route observers. */

#include "plugins/rib/src/route_observer.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

namespace dang::rib {
namespace {

std::string Escape(std::string_view value) {
  std::string result;
  for (const char character : value) {
    switch (character) {
      case '&': result += "&amp;"; break;
      case '<': result += "&lt;"; break;
      case '>': result += "&gt;"; break;
      case '\"': result += "&quot;"; break;
      case '\'': result += "&apos;"; break;
      default: result += character;
    }
  }
  return result;
}

/** Emits exactly one case of the RFC 8431 nexthop-base choice. */
void EmitBaseNexthop(std::ostringstream& xml, bool ipv4,
                     const std::optional<std::string>& gateway,
                     const std::optional<std::string>& interface,
                     const std::optional<std::string>& special = {}) {
  const char* address = ipv4 ? "ipv4-address" : "ipv6-address";
  if (special) {
    xml << "<special>" << Escape(*special) << "</special>";
  } else if (gateway && interface) {
    const char* combined = ipv4 ? "egress-interface-ipv4-address"
                                : "egress-interface-ipv6-address";
    xml << '<' << combined << "><outgoing-interface>"
        << Escape(*interface) << "</outgoing-interface><" << address << '>'
        << Escape(*gateway) << "</" << address << "></" << combined << '>';
  } else if (gateway) {
    xml << '<' << address << '>' << Escape(*gateway) << "</" << address << '>';
  } else if (interface) {
    xml << "<outgoing-interface>" << Escape(*interface)
        << "</outgoing-interface>";
  }
}

auto RouteIdentity(const Route& route) {
  return std::tuple{route.rib, route.address_family, route.destination,
                    route.gateway.value_or(""), route.interface.value_or(""),
                    route.special.value_or("")};
}

bool EquivalentObservedRoute(const ObservedRoute& left,
                             const ObservedRoute& right) {
  Route normalized_left = left.route;
  Route normalized_right = right.route;
  // route-index is a caller-owned key for managed RPCs but a deterministic
  // synthetic key for native observations.  It must not turn confirmation of
  // the same route into a second notification.
  normalized_left.index = 0;
  normalized_right.index = 0;
  // A platform may omit the default single-path weight. Treat that as one so
  // managed confirmation does not generate a duplicate native notification.
  return normalized_left == normalized_right &&
         left.weight.value_or(1U) == right.weight.value_or(1U);
}

using RibKey = std::pair<std::string, std::string>;

struct OperationalProjection {
  std::vector<ObservedRoute> routes;
  std::map<RibKey, std::vector<std::uint32_t>> synthetic_ids;
};

std::uint64_t StableRouteIndex(const Route& route) {
  const std::string key = route.rib + "|" + route.address_family + "|" +
                          route.destination + "|" +
                          std::to_string(route.preference) + "|" +
                          (route.local_only ? "1" : "0");
  std::uint64_t hash = 1469598103934665603ULL;
  for (const char byte : key) {
    hash ^= static_cast<unsigned char>(byte);
    hash *= 1099511628211ULL;
  }
  return hash;
}

bool SameBaseNexthop(const Route& route, const PersistentNexthop& nexthop) {
  return route.gateway == nexthop.gateway &&
         route.interface == nexthop.interface &&
         route.special == nexthop.special;
}

/** Restores a modeled list key and reusable identity after native readback. */
void RestoreModeledIdentity(ObservedRoute* observed,
                            const PersistentRegistry& registry,
                            const std::vector<Route>& configured_routes) {
  if (!observed || !observed->route.load_balance.empty()) return;
  Route& route = observed->route;
  const auto same_route = [&](const Route& candidate) {
    return candidate.load_balance.empty() &&
           candidate.routing_instance == route.routing_instance &&
           candidate.rib == route.rib &&
           candidate.address_family == route.address_family &&
           candidate.destination == route.destination &&
           candidate.preference == route.preference &&
           candidate.local_only == route.local_only &&
           candidate.gateway == route.gateway &&
           candidate.interface == route.interface &&
           candidate.special == route.special;
  };
  const auto configured = std::ranges::find_if(configured_routes, same_route);
  if (configured != configured_routes.end() &&
      std::ranges::find_if(std::next(configured), configured_routes.end(),
                           same_route) == configured_routes.end()) {
    route.index = configured->index;
    route.nexthop_ref = configured->nexthop_ref;
    return;
  }

  const PersistentRouteBinding* matched = nullptr;
  for (const PersistentRouteBinding& binding : registry.bindings) {
    if (binding.rib != route.rib ||
        binding.address_family != route.address_family ||
        binding.destination != route.destination)
      continue;
    const auto nexthop = std::ranges::find_if(
        registry.nexthops, [&](const PersistentNexthop& value) {
          return value.rib == binding.rib && value.id == binding.nexthop_id;
        });
    if (nexthop == registry.nexthops.end() ||
        !SameBaseNexthop(route, *nexthop))
      continue;
    if (matched) return;
    matched = &binding;
  }
  if (matched) {
    route.index = matched->route_index;
    route.nexthop_ref = matched->nexthop_id;
  }
}

/** Collapses representable native ECMP paths into one modeled weighted route. */
OperationalProjection ProjectWeightedRoutes(
    const std::vector<ObservedRoute>& input,
    const PersistentRegistry& registry,
    const std::vector<Route>& configured_routes) {
  using GroupKey =
      std::tuple<std::string, std::string, std::string, std::string,
                 std::uint32_t, bool>;
  std::map<GroupKey, std::vector<ObservedRoute>> groups;
  OperationalProjection result;
  std::uint64_t next_synthetic = std::max<std::uint64_t>(registry.next_id, 1U);
  for (const PersistentNexthop& nexthop : registry.nexthops)
    next_synthetic =
        std::max(next_synthetic, static_cast<std::uint64_t>(nexthop.id) + 1U);

  for (ObservedRoute observed : input) {
    RestoreModeledIdentity(&observed, registry, configured_routes);
    const Route& route = observed.route;
    if (!observed.weight || *observed.weight < 1U ||
        *observed.weight > 99U || route.special ||
        !route.load_balance.empty()) {
      result.routes.push_back(observed);
      continue;
    }
    groups[{route.routing_instance, route.rib, route.address_family,
            route.destination, route.preference, route.local_only}]
        .push_back(observed);
  }

  for (auto& [key, members] : groups) {
    if (members.size() < 2U) {
      result.routes.insert(result.routes.end(), members.begin(), members.end());
      continue;
    }
    std::ranges::sort(members, {}, [](const ObservedRoute& member) {
      return std::tie(member.route.gateway, member.route.interface,
                      member.route.special);
    });

    std::vector<WeightedNexthop> projected;
    std::set<std::uint32_t> used_ids;
    std::optional<std::uint64_t> configured_index;
    bool durable_ids = true;
    for (const ObservedRoute& member : members) {
      const auto binding = std::ranges::find_if(
          registry.bindings, [&](const PersistentRouteBinding& candidate) {
            if (candidate.rib != member.route.rib ||
                candidate.address_family != member.route.address_family ||
                candidate.destination != member.route.destination ||
                used_ids.contains(candidate.nexthop_id))
              return false;
            const auto nexthop = std::ranges::find_if(
                registry.nexthops, [&](const PersistentNexthop& value) {
                  return value.rib == candidate.rib &&
                         value.id == candidate.nexthop_id;
                });
            return nexthop != registry.nexthops.end() &&
                   SameBaseNexthop(member.route, *nexthop);
          });
      if (binding == registry.bindings.end() ||
          (configured_index && *configured_index != binding->route_index)) {
        durable_ids = false;
        break;
      }
      configured_index = binding->route_index;
      used_ids.insert(binding->nexthop_id);
      projected.push_back(
          {.id = binding->nexthop_id,
           .gateway = member.route.gateway,
           .interface = member.route.interface,
           .weight = static_cast<std::uint8_t>(*member.weight)});
    }

    if (!durable_ids) {
      projected.clear();
      configured_index.reset();
      if (next_synthetic > std::numeric_limits<std::uint32_t>::max()) {
        result.routes.insert(result.routes.end(), members.begin(),
                             members.end());
        continue;
      }
      const std::uint64_t remaining_ids =
          static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) -
          next_synthetic + 1U;
      if (members.size() > remaining_ids) {
        result.routes.insert(result.routes.end(), members.begin(),
                             members.end());
        continue;
      }
      const RibKey rib_key{std::get<1>(key), std::get<2>(key)};
      // External routes have no datastore identity. Allocate deterministic,
      // snapshot-local IDs solely so their weighted native state can be
      // represented by the RFC 8431 reference-based operational schema.
      for (const ObservedRoute& member : members) {
        const auto id = static_cast<std::uint32_t>(next_synthetic++);
        result.synthetic_ids[rib_key].push_back(id);
        projected.push_back(
            {.id = id,
             .gateway = member.route.gateway,
             .interface = member.route.interface,
             .weight = static_cast<std::uint8_t>(*member.weight)});
      }
    }
    std::ranges::sort(projected, {}, &WeightedNexthop::id);

    ObservedRoute combined = members.front();
    combined.route.index = configured_index.value_or(
        StableRouteIndex(combined.route));
    combined.route.gateway.reset();
    combined.route.interface.reset();
    combined.route.nexthop_ref.reset();
    combined.route.special.reset();
    combined.route.load_balance = std::move(projected);
    combined.installed = std::ranges::any_of(
        members, [](const ObservedRoute& member) { return member.installed; });
    combined.mutable_route = std::ranges::all_of(
        members,
        [](const ObservedRoute& member) { return member.mutable_route; });
    combined.weight.reset();
    const bool same_reason = std::ranges::all_of(
        members, [&](const ObservedRoute& member) {
          return member.reason == members.front().reason;
        });
    if (!same_reason) combined.reason.reset();
    result.routes.push_back(std::move(combined));
  }
  return result;
}

}  // namespace

std::vector<ObservedRoute> RouteChangeTracker::Observe(
    const std::vector<ObservedRoute>& routes) {
  std::map<Key, ObservedRoute> next;
  for (const ObservedRoute& route : routes)
    next[RouteIdentity(route.route)] = route;
  if (!initialized_) {
    routes_ = std::move(next);
    initialized_ = true;
    return {};
  }
  std::vector<ObservedRoute> changes;
  for (const auto& [key, previous] : routes_)
    if (!next.contains(key)) {
      ObservedRoute removed = previous;
      removed.installed = false;
      // Disappearance from an inventory does not reveal why the route was
      // removed. Do not carry an earlier status reason into a new event.
      removed.reason.reset();
      changes.push_back(std::move(removed));
    }
  for (const auto& [key, current] : next) {
    const auto previous = routes_.find(key);
    if (previous == routes_.end() ||
        !EquivalentObservedRoute(previous->second, current) ||
        previous->second.installed != current.installed) {
      ObservedRoute change = current;
      if (previous != routes_.end() &&
          previous->second.installed != current.installed)
        change.reason = current.installed ? "resolved-nexthop"
                                          : "unresolved-nexthop";
      changes.push_back(std::move(change));
    }
  }
  routes_ = std::move(next);
  return changes;
}

void RouteChangeTracker::ApplyManaged(const Route& route, bool installed) {
  if (!initialized_) return;
  const Key key = RouteIdentity(route);
  if (installed) routes_[key] = ObservedRoute(route, true);
  else routes_.erase(key);
}

std::vector<NexthopResolutionChange> NexthopResolutionTracker::Observe(
    const PersistentRegistry& registry,
    const std::vector<ObservedRoute>& routes) {
  std::set<std::pair<std::string, std::uint32_t>> resolved;
  for (const PersistentRouteBinding& binding : registry.bindings) {
    const auto nexthop = std::ranges::find_if(
        registry.nexthops, [&](const PersistentNexthop& candidate) {
          return candidate.rib == binding.rib &&
                 candidate.id == binding.nexthop_id;
        });
    if (nexthop == registry.nexthops.end()) continue;
    const bool installed = std::ranges::any_of(
        routes, [&](const ObservedRoute& observed) {
          const Route& route = observed.route;
          return observed.installed && route.rib == binding.rib &&
                 route.address_family == binding.address_family &&
                 route.destination == binding.destination &&
                 (!nexthop->gateway || route.gateway == nexthop->gateway) &&
                 (!nexthop->interface ||
                  route.interface == nexthop->interface) &&
                 (!nexthop->special || route.special == nexthop->special);
        });
    if (installed) resolved.emplace(binding.rib, binding.nexthop_id);
  }

  std::map<std::pair<std::string, std::uint32_t>, bool> next;
  std::vector<NexthopResolutionChange> changes;
  for (const PersistentNexthop& nexthop : registry.nexthops) {
    const auto key = std::make_pair(nexthop.rib, nexthop.id);
    const bool current = resolved.contains(key);
    next[key] = current;
    const auto previous = states_.find(key);
    // Object creation alone is not a resolution change. A newly observed
    // object is reported only when it is already backed by an installed route.
    if ((previous == states_.end() && current) ||
        (previous != states_.end() && previous->second != current))
      changes.push_back({nexthop, current});
  }
  states_ = std::move(next);
  return changes;
}

std::string SerializeOperationalRoutes(
    const std::vector<ObservedRoute>& input,
    const PersistentRegistry& registry,
    const std::vector<Route>& configuration_routes) {
  OperationalProjection projection =
      ProjectWeightedRoutes(input, registry, configuration_routes);
  std::vector<ObservedRoute>& routes = projection.routes;
  std::ranges::sort(routes, {}, [](const ObservedRoute& value) {
    return std::tie(value.route.routing_instance, value.route.rib,
                    value.route.address_family, value.route.index);
  });
  std::ostringstream xml;
  xml << "<data xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">"
         "<routing-instance xmlns=\"urn:ietf:params:xml:ns:yang:ietf-i2rs-rib\">"
         "<name>default</name>";
  std::string current_rib;
  std::string current_family;
  std::map<RibKey, std::vector<std::uint32_t>> ids;
  std::set<RibKey> registered_ribs;
  for (const PersistentRib& rib : registry.ribs)
    registered_ribs.emplace(rib.name, rib.address_family);
  for (const PersistentNexthop& nexthop : registry.nexthops) {
    std::string family = nexthop.address_family.value_or("");
    if (family.empty()) {
      const auto registered = std::ranges::find_if(
          registry.ribs, [&](const PersistentRib& rib) {
            return rib.name == nexthop.rib;
          });
      if (registered != registry.ribs.end())
        family = registered->address_family;
    }
    if (family.empty()) {
      for (const ObservedRoute& observed : routes)
        if (observed.route.rib == nexthop.rib) {
          if (family.empty()) family = observed.route.address_family;
          else if (family != observed.route.address_family) { family.clear(); break; }
        }
    }
    // An interface-only nexthop has no intrinsic family. Publish it only when
    // the containing native RIB supplies one unambiguous family.
    if (!family.empty())
      ids[{nexthop.rib, family}].push_back(nexthop.id);
  }
  for (const auto& [key, values] : projection.synthetic_ids)
    ids[key].insert(ids[key].end(), values.begin(), values.end());
  for (auto& [key, values] : ids) {
    std::ranges::sort(values);
    values.erase(std::unique(values.begin(), values.end()), values.end());
  }
  std::set<RibKey> emitted;
  const auto emit_ids = [&](const RibKey& key) {
    if (const auto found = ids.find(key); found != ids.end())
      for (const std::uint32_t id : found->second)
        xml << "<nexthop-list><nexthop-member-id>" << id
            << "</nexthop-member-id></nexthop-list>";
    emitted.insert(key);
  };
  for (const ObservedRoute& observed : routes) {
    const Route& route = observed.route;
    if (route.rib != current_rib || route.address_family != current_family) {
      if (!current_rib.empty()) {
        emit_ids({current_rib, current_family});
        xml << "</rib-list>";
      }
      current_rib = route.rib;
      current_family = route.address_family;
      xml << "<rib-list><name>" << Escape(route.rib)
          << "</name><address-family>" << route.address_family
          << "-address-family"
          << "</address-family>";
    }
    const bool ipv4 = route.address_family == "ipv4";
    xml << "<route-list><route-index>" << route.index << "</route-index><match><"
        << (ipv4 ? "ipv4><dest-ipv4-prefix>" : "ipv6><dest-ipv6-prefix>")
        << Escape(route.destination)
        << (ipv4 ? "</dest-ipv4-prefix></ipv4>" : "</dest-ipv6-prefix></ipv6>")
        << "</match><nexthop>";
    if (route.nexthop_ref) {
      const auto definition = std::ranges::find_if(
          registry.nexthops, [&](const PersistentNexthop& candidate) {
            return candidate.rib == route.rib &&
                   candidate.id == *route.nexthop_ref;
          });
      if (definition != registry.nexthops.end())
        xml << "<nexthop-id>" << definition->id
            << "</nexthop-id><sharing-flag>"
            << (definition->sharable ? "true" : "false")
            << "</sharing-flag>";
    }
    if (route.load_balance.empty()) {
      xml << "<nexthop-base>";
      EmitBaseNexthop(xml, ipv4, route.gateway, route.interface, route.special);
      xml << "</nexthop-base>";
    } else {
      xml << "<nexthop-lb>";
      for (const WeightedNexthop& member : route.load_balance)
        xml << "<nexthop-list><nexthop-member-id>" << member.id
            << "</nexthop-member-id><nexthop-lb-weight>"
            << static_cast<unsigned>(member.weight)
            << "</nexthop-lb-weight></nexthop-list>";
      xml << "</nexthop-lb>";
    }
    xml << "</nexthop><route-status><route-state>"
        << (observed.installed ? "active" : "inactive")
        << "</route-state>"
        << "<route-installed-state>"
        << (observed.installed ? "installed" : "uninstalled")
        << "</route-installed-state>";
    if (observed.reason)
      xml << "<route-reason>" << Escape(*observed.reason)
          << "</route-reason>";
    xml << "</route-status><route-attributes>"
        << "<route-preference>" << route.preference << "</route-preference>"
        << "<local-only>" << (route.local_only ? "true" : "false")
        << "</local-only></route-attributes></route-list>";
  }
  if (!current_rib.empty()) {
    emit_ids({current_rib, current_family});
    xml << "</rib-list>";
  }
  std::set<RibKey> remaining_ribs = registered_ribs;
  for (const auto& entry : ids) remaining_ribs.insert(entry.first);
  for (const RibKey& key : remaining_ribs) {
    if (emitted.contains(key)) continue;
    xml << "<rib-list><name>" << Escape(key.first)
        << "</name><address-family>" << key.second
        << "-address-family</address-family>";
    if (const auto values = ids.find(key); values != ids.end())
      for (const std::uint32_t id : values->second)
        xml << "<nexthop-list><nexthop-member-id>" << id
            << "</nexthop-member-id></nexthop-list>";
    xml << "</rib-list>";
  }
  xml << "</routing-instance></data>";
  return xml.str();
}

std::string SerializeRouteChange(const Route& route, bool installed,
                                 std::string_view reason) {
  const bool ipv4 = route.address_family == "ipv4";
  std::ostringstream xml;
  xml << "<route-change xmlns=\"urn:ietf:params:xml:ns:yang:ietf-i2rs-rib\">"
      << "<rib-name>" << Escape(route.rib) << "</rib-name>"
      << "<address-family>" << Escape(route.address_family)
      << "-address-family</address-family><route-index>" << route.index
      << "</route-index>"
      << "<match><" << (ipv4 ? "ipv4><dest-ipv4-prefix>"
                                 : "ipv6><dest-ipv6-prefix>")
      << Escape(route.destination)
      << (ipv4 ? "</dest-ipv4-prefix></ipv4>"
               : "</dest-ipv6-prefix></ipv6>")
      << "</match><route-installed-state>"
      << (installed ? "installed" : "uninstalled")
      << "</route-installed-state><route-state>"
      << (installed ? "active" : "inactive")
      << "</route-state>";
  if (!reason.empty())
    xml << "<route-change-reasons><route-change-reason>" << Escape(reason)
        << "</route-change-reason></route-change-reasons>";
  xml << "</route-change>";
  return xml.str();
}

std::string SerializeNexthopResolutionChange(
    const PersistentNexthop& nexthop, bool resolved) {
  std::ostringstream xml;
  xml << "<nexthop-resolution-status-change xmlns=\""
         "urn:ietf:params:xml:ns:yang:ietf-i2rs-rib\"><nexthop>"
      << "<nexthop-id>" << nexthop.id << "</nexthop-id><sharing-flag>"
      << (nexthop.sharable ? "true" : "false")
      << "</sharing-flag><nexthop-base>";
  const bool ipv4 = nexthop.address_family
      ? *nexthop.address_family == "ipv4"
      : !nexthop.gateway || nexthop.gateway->find(':') == std::string::npos;
  EmitBaseNexthop(xml, ipv4, nexthop.gateway, nexthop.interface,
                  nexthop.special);
  xml << "</nexthop-base></nexthop><nexthop-state>"
      << (resolved ? "resolved" : "unresolved")
      << "</nexthop-state></nexthop-resolution-status-change>";
  return xml.str();
}

}  // namespace dang::rib

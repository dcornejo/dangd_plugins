// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/** @file RFC 8431 XML serialization shared by native route observers. */

#include "plugins/rib/src/route_observer.h"

#include <algorithm>
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

bool EquivalentObservedRoute(const Route& left, const Route& right) {
  Route normalized_left = left;
  Route normalized_right = right;
  // route-index is a caller-owned key for managed RPCs but a deterministic
  // synthetic key for native observations.  It must not turn confirmation of
  // the same route into a second notification.
  normalized_left.index = 0;
  normalized_right.index = 0;
  return normalized_left == normalized_right;
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
      changes.push_back(std::move(removed));
    }
  for (const auto& [key, current] : next) {
    const auto previous = routes_.find(key);
    if (previous == routes_.end() ||
        !EquivalentObservedRoute(previous->second.route, current.route) ||
        previous->second.installed != current.installed)
      changes.push_back(current);
  }
  routes_ = std::move(next);
  return changes;
}

void RouteChangeTracker::ApplyManaged(const Route& route, bool installed) {
  if (!initialized_) return;
  const Key key = RouteIdentity(route);
  if (installed) routes_[key] = ObservedRoute{route, true};
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
                  route.interface == nexthop->interface);
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
    const std::vector<std::tuple<std::string, std::string, std::uint32_t>>&
        nexthops) {
  std::vector<ObservedRoute> routes = input;
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
  using RibKey = std::pair<std::string, std::string>;
  std::map<RibKey, std::vector<std::uint32_t>> ids;
  for (const auto& [rib, supplied_family, id] : nexthops) {
    std::string family = supplied_family;
    if (family.empty()) {
      for (const ObservedRoute& observed : routes)
        if (observed.route.rib == rib) {
          if (family.empty()) family = observed.route.address_family;
          else if (family != observed.route.address_family) { family.clear(); break; }
        }
    }
    // An interface-only nexthop has no intrinsic family. Publish it only when
    // the containing native RIB supplies one unambiguous family.
    if (!family.empty()) ids[{rib, family}].push_back(id);
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
        << "</match><nexthop><nexthop-base>";
    EmitBaseNexthop(xml, ipv4, route.gateway, route.interface, route.special);
    xml << "</nexthop-base></nexthop><route-status><route-state>"
        << (observed.installed ? "active" : "inactive")
        << "</route-state>"
        << "<route-installed-state>"
        << (observed.installed ? "installed" : "uninstalled")
        << "</route-installed-state></route-status><route-attributes>"
        << "<route-preference>" << route.preference << "</route-preference>"
        << "<local-only>" << (route.local_only ? "true" : "false")
        << "</local-only></route-attributes></route-list>";
  }
  if (!current_rib.empty()) {
    emit_ids({current_rib, current_family});
    xml << "</rib-list>";
  }
  for (const auto& [key, values] : ids) {
    if (emitted.contains(key)) continue;
    xml << "<rib-list><name>" << Escape(key.first)
        << "</name><address-family>" << key.second
        << "-address-family</address-family>";
    for (const std::uint32_t id : values)
      xml << "<nexthop-list><nexthop-member-id>" << id
          << "</nexthop-member-id></nexthop-list>";
    xml << "</rib-list>";
  }
  xml << "</routing-instance></data>";
  return xml.str();
}

std::string SerializeRouteChange(const Route& route, bool installed) {
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
      << "</route-state></route-change>";
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
  const bool ipv4 = !nexthop.gateway || nexthop.gateway->find(':') == std::string::npos;
  EmitBaseNexthop(xml, ipv4, nexthop.gateway, nexthop.interface);
  xml << "</nexthop-base></nexthop><nexthop-state>"
      << (resolved ? "resolved" : "unresolved")
      << "</nexthop-state></nexthop-resolution-status-change>";
  return xml.str();
}

}  // namespace dang::rib

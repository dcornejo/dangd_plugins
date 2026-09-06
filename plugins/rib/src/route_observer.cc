// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/** @file RFC 8431 XML serialization shared by native route observers. */

#include "plugins/rib/src/route_observer.h"

#include <algorithm>
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

}  // namespace

std::string SerializeOperationalRoutes(const std::vector<ObservedRoute>& input) {
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
  for (const ObservedRoute& observed : routes) {
    const Route& route = observed.route;
    if (route.rib != current_rib || route.address_family != current_family) {
      if (!current_rib.empty()) xml << "</rib-list>";
      current_rib = route.rib;
      current_family = route.address_family;
      xml << "<rib-list><name>" << Escape(route.rib)
          << "</name><address-family>" << route.address_family
          << "</address-family>";
    }
    const bool ipv4 = route.address_family == "ipv4";
    xml << "<route-list><route-index>" << route.index << "</route-index><match><"
        << (ipv4 ? "ipv4><dest-ipv4-prefix>" : "ipv6><dest-ipv6-prefix>")
        << Escape(route.destination)
        << (ipv4 ? "</dest-ipv4-prefix></ipv4>" : "</dest-ipv6-prefix></ipv6>")
        << "</match><nexthop><nexthop-base>";
    if (route.gateway)
      xml << '<' << (ipv4 ? "ipv4-address" : "ipv6-address") << '>'
          << Escape(*route.gateway) << "</"
          << (ipv4 ? "ipv4-address" : "ipv6-address") << '>';
    if (route.interface)
      xml << "<outgoing-interface>" << Escape(*route.interface)
          << "</outgoing-interface>";
    xml << "</nexthop-base></nexthop><route-status><route-state>active</route-state>"
        << "<route-installed-state>"
        << (observed.installed ? "installed" : "uninstalled")
        << "</route-installed-state></route-status><route-attributes>"
        << "<route-preference>" << route.preference << "</route-preference>"
        << "<local-only>" << (route.local_only ? "true" : "false")
        << "</local-only></route-attributes></route-list>";
  }
  if (!current_rib.empty()) xml << "</rib-list>";
  xml << "</routing-instance></data>";
  return xml.str();
}

}  // namespace dang::rib

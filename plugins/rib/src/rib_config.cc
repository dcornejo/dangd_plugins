// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/**
 * @file
 * Parses the supported RFC 8431 static-route projection and produces a stable
 * host-operation plan.  Strict rejection of unsupported route forms prevents
 * the native backends from approximating model semantics unexpectedly.
 */

#include "plugins/rib/src/rib_config.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <memory>
#include <set>
#include <sstream>
#include <string_view>
#include <tuple>

#include <libxml/parser.h>
#include <libxml/tree.h>

namespace dang::rib {
namespace {

constexpr std::string_view kNamespace =
    "urn:ietf:params:xml:ns:yang:ietf-i2rs-rib";

bool Is(xmlNodePtr node, std::string_view name) {
  return node && node->type == XML_ELEMENT_NODE && node->ns && node->ns->href &&
         name == reinterpret_cast<const char*>(node->name) &&
         kNamespace == reinterpret_cast<const char*>(node->ns->href);
}

xmlNodePtr Child(xmlNodePtr parent, std::string_view name) {
  for (xmlNodePtr node = parent ? parent->children : nullptr; node;
       node = node->next)
    if (Is(node, name)) return node;
  return nullptr;
}

std::vector<xmlNodePtr> Children(xmlNodePtr parent, std::string_view name) {
  std::vector<xmlNodePtr> result;
  for (xmlNodePtr node = parent ? parent->children : nullptr; node;
       node = node->next)
    if (Is(node, name)) result.push_back(node);
  return result;
}

std::optional<std::string> Text(xmlNodePtr node) {
  if (!node) return std::nullopt;
  xmlChar* raw = xmlNodeGetContent(node);
  if (!raw) return std::nullopt;
  std::string value(reinterpret_cast<const char*>(raw));
  xmlFree(raw);
  return value;
}

bool Fail(std::string message, std::string path, std::string* error,
          std::string* error_path) {
  *error = std::move(message);
  *error_path = std::move(path);
  return false;
}

xmlNodePtr FindRoutingInstance(xmlDocPtr document) {
  xmlNodePtr root = xmlDocGetRootElement(document);
  if (Is(root, "routing-instance")) return root;
  for (xmlNodePtr node = root ? root->children : nullptr; node;
       node = node->next)
    if (Is(node, "routing-instance")) return node;
  return nullptr;
}

std::string LocalIdentity(std::string value) {
  const std::size_t colon = value.find(':');
  value = colon == std::string::npos ? value : value.substr(colon + 1);
  if (value == "ipv4-address-family") return "ipv4";
  if (value == "ipv6-address-family") return "ipv6";
  return value;
}

bool ParseUnsigned(xmlNodePtr node, std::uint64_t* output) {
  const auto value = Text(node);
  if (!value) return false;
  const auto parsed = std::from_chars(value->data(),
                                      value->data() + value->size(), *output);
  return parsed.ec == std::errc{} &&
         parsed.ptr == value->data() + value->size();
}

bool ParseBoolean(xmlNodePtr node, bool* output) {
  const auto value = Text(node);
  if (!value) return false;
  if (*value == "true" || *value == "1") { *output = true; return true; }
  if (*value == "false" || *value == "0") { *output = false; return true; }
  return false;
}

auto Key(const Route& route) {
  // RFC 8431 identifies a route by its containing instance/RIB and route-index;
  // forwarding attributes are values, so changing one replaces the same key.
  return std::tie(route.routing_instance, route.rib, route.index);
}

}  // namespace

bool ParseConfig(const char* xml, Config* config, std::string* error,
                 std::string* error_path, const NexthopResolver& resolver) {
  if (!xml || !config || !error || !error_path) return false;
  xmlDocPtr raw = xmlReadMemory(xml, static_cast<int>(std::strlen(xml)),
                                "datastore.xml", nullptr,
                                XML_PARSE_NONET | XML_PARSE_NOBLANKS |
                                    XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
  std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> document(raw, xmlFreeDoc);
  if (!document)
    return Fail("cannot parse complete datastore XML",
                "/ietf-i2rs-rib:routing-instance", error, error_path);
  *config = {};
  xmlNodePtr instance = FindRoutingInstance(document.get());
  if (!instance) return true;
  const std::string instance_name = Text(Child(instance, "name")).value_or("");
  if (instance_name.empty())
    return Fail("routing-instance name is required",
                "/ietf-i2rs-rib:routing-instance/name", error, error_path);
  if (instance_name != "default")
    return Fail(
        "the native RIB backend supports only routing-instance 'default'; "
        "VRF or VNET instance mapping is not implemented",
        "/ietf-i2rs-rib:routing-instance/name", error, error_path);

  std::set<std::tuple<std::string, std::uint64_t>> route_keys;
  for (xmlNodePtr rib : Children(instance, "rib-list")) {
    const std::string rib_name = Text(Child(rib, "name")).value_or("");
    const std::string family =
        LocalIdentity(Text(Child(rib, "address-family")).value_or(""));
    if (rib_name.empty() || (family != "ipv4" && family != "ipv6"))
      return Fail("the initial backend supports only named IPv4 and IPv6 RIBs",
                  "/ietf-i2rs-rib:routing-instance/rib-list/address-family",
                  error, error_path);
    for (xmlNodePtr route_node : Children(rib, "route-list")) {
      Route route;
      route.routing_instance = instance_name;
      route.rib = rib_name;
      route.address_family = family;
      if (!ParseUnsigned(Child(route_node, "route-index"), &route.index) ||
          !route_keys.emplace(rib_name, route.index).second)
        return Fail("route-index is missing, invalid, or duplicated",
                    "/ietf-i2rs-rib:routing-instance/rib-list/route-list/route-index",
                    error, error_path);

      xmlNodePtr match = Child(route_node, "match");
      xmlNodePtr ip = Child(match, family);
      const std::string prefix_name =
          family == "ipv4" ? "dest-ipv4-prefix" : "dest-ipv6-prefix";
      route.destination = Text(Child(ip, prefix_name)).value_or("");
      if (route.destination.empty())
        return Fail("only destination-prefix IP routes are currently supported",
                    "/ietf-i2rs-rib:routing-instance/rib-list/route-list/match",
                    error, error_path);

      xmlNodePtr base = Child(Child(route_node, "nexthop"), "nexthop-base");
      if (!base)
        return Fail("only base nexthops are currently supported",
                    "/ietf-i2rs-rib:routing-instance/rib-list/route-list/nexthop",
                    error, error_path);
      route.gateway = Text(Child(base, family == "ipv4" ? "ipv4-address"
                                                        : "ipv6-address"));
      route.interface = Text(Child(base, "outgoing-interface"));
      if (const auto special = Text(Child(base, "special")); special)
        route.special = LocalIdentity(*special);
      if (xmlNodePtr combined = Child(
              base, family == "ipv4" ? "egress-interface-ipv4-address"
                                      : "egress-interface-ipv6-address")) {
        route.gateway = Text(Child(combined, family == "ipv4" ? "ipv4-address"
                                                              : "ipv6-address"));
        route.interface = Text(Child(combined, "outgoing-interface"));
      }
      if (xmlNodePtr reference = Child(base, "nexthop-ref")) {
        std::uint64_t id = 0;
        if (route.special || route.gateway || route.interface || !resolver ||
            !ParseUnsigned(reference, &id) || id > UINT32_MAX ||
            !resolver(rib_name, static_cast<std::uint32_t>(id), &route.gateway,
                      &route.interface, &route.special))
          return Fail("nexthop-ref does not identify a registered nexthop in this RIB",
                      "/ietf-i2rs-rib:routing-instance/rib-list/route-list/nexthop/nexthop-base/nexthop-ref",
                      error, error_path);
        route.nexthop_ref = static_cast<std::uint32_t>(id);
      }
      if (route.special && (*route.special != "discard" &&
                            *route.special != "discard-with-error"))
        return Fail(*route.special == "receive"
                        ? "receive routes are kernel-owned and read-only"
                        : "the special nexthop is not supported by the "
                          "portable backend",
                    "/ietf-i2rs-rib:routing-instance/rib-list/route-list/"
                    "nexthop/nexthop-base/special",
                    error, error_path);
      if (route.special && (route.gateway || route.interface))
        return Fail("a special nexthop cannot include another base nexthop",
                    "/ietf-i2rs-rib:routing-instance/rib-list/route-list/nexthop/nexthop-base",
                    error, error_path);
      if (!route.special && !route.gateway && !route.interface)
        return Fail("base nexthop requires an IP gateway, outgoing interface, "
                    "or supported special identity",
                    "/ietf-i2rs-rib:routing-instance/rib-list/route-list/nexthop/nexthop-base",
                    error, error_path);

      xmlNodePtr attributes = Child(route_node, "route-attributes");
      std::uint64_t preference = 0;
      if (!ParseUnsigned(Child(attributes, "route-preference"), &preference) ||
          preference > UINT32_MAX)
        return Fail("route-preference is required and must fit uint32",
                    "/ietf-i2rs-rib:routing-instance/rib-list/route-list/route-attributes/route-preference",
                    error, error_path);
      route.preference = static_cast<std::uint32_t>(preference);
      if (!ParseBoolean(Child(attributes, "local-only"), &route.local_only))
        return Fail("local-only is required and must be boolean",
                    "/ietf-i2rs-rib:routing-instance/rib-list/route-list/route-attributes/local-only",
                    error, error_path);
      if (route.local_only)
        return Fail(
            "the portable native backend cannot configure local-only routes; "
            "kernel-owned receive routes are published as read-only state",
            "/ietf-i2rs-rib:routing-instance/rib-list/route-list/route-attributes/local-only",
            error, error_path);
      config->routes.push_back(std::move(route));
    }
  }
  // Canonical ordering keeps plans deterministic regardless of XML sibling
  // order and makes dry-run/test output stable.
  std::ranges::sort(config->routes, {}, Key);
  return true;
}

std::vector<Change> PlanChanges(const Config& before, const Config& proposed) {
  std::vector<Change> deletions;
  std::vector<Change> installations;
  for (const Route& old_route : before.routes) {
    const auto found = std::ranges::find_if(
        proposed.routes, [&](const Route& route) { return Key(route) == Key(old_route); });
    if (found == proposed.routes.end() || *found != old_route)
      deletions.push_back({ChangeKind::kDelete, old_route});
  }
  for (const Route& new_route : proposed.routes) {
    const auto found = std::ranges::find_if(
        before.routes, [&](const Route& route) { return Key(route) == Key(new_route); });
    if (found == before.routes.end() || *found != new_route)
      installations.push_back({ChangeKind::kInstall, new_route});
  }
  deletions.insert(deletions.end(), installations.begin(), installations.end());
  return deletions;
}

std::string Describe(const Change& change) {
  std::ostringstream output;
  output << (change.kind == ChangeKind::kDelete ? "delete" : "install")
         << ' ' << change.route.address_family << " route "
         << change.route.destination << " in RIB " << change.route.rib;
  if (change.route.gateway) output << " via " << *change.route.gateway;
  if (change.route.interface) output << " dev " << *change.route.interface;
  if (change.route.special) output << " special " << *change.route.special;
  output << " preference " << change.route.preference;
  if (change.route.local_only) output << " local-only";
  return output.str();
}

}  // namespace dang::rib

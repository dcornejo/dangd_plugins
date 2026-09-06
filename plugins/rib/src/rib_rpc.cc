// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/** @file Parsing and execution for imperative RFC 8431 RIB operations. */

#include "plugins/rib/src/rib_rpc.h"

#include <cstring>
#include <charconv>
#include <limits>
#include <memory>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

#include <libxml/parser.h>
#include <libxml/tree.h>

#if defined(__FreeBSD__)
#include <sys/sysctl.h>
#endif

#include "plugins/rib/src/rib_config.h"

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

std::string Text(xmlNodePtr node) {
  if (!node) return {};
  xmlChar* raw = xmlNodeGetContent(node);
  if (!raw) return {};
  std::string value(reinterpret_cast<const char*>(raw));
  xmlFree(raw);
  return value;
}

bool Boolean(xmlNodePtr node) {
  const std::string value = Text(node);
  return value == "true" || value == "1";
}

bool Unsigned(xmlNodePtr node, std::uint32_t* output) {
  const std::string value = Text(node);
  if (value.empty() || !output) return false;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(),
                                      *output);
  return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
}

bool Inventory(NativePlatform platform, const RouteObserver& supplied,
               std::vector<ObservedRoute>* routes, std::string* error) {
  if (supplied) return supplied(routes, error);
  return platform == NativePlatform::kLinux
             ? ObserveLinuxRoutes(routes, error)
             : ObserveFreeBsdRoutes(routes, error);
}

std::string Output(unsigned success,
                   const std::vector<std::pair<std::uint64_t, unsigned>>& failed,
                   bool details) {
  std::ostringstream xml;
  xml << "<success-count xmlns=\"" << kNamespace << "\">" << success
      << "</success-count><failed-count xmlns=\"" << kNamespace << "\">"
      << failed.size() << "</failed-count>";
  if (details && !failed.empty()) {
    xml << "<failure-detail xmlns=\"" << kNamespace << "\">";
    for (const auto& [index, code] : failed)
      xml << "<failed-routes><route-index>" << index
          << "</route-index><error-code>" << code
          << "</error-code></failed-routes>";
    xml << "</failure-detail>";
  }
  return xml.str();
}

std::string BooleanOutput(bool result, std::string_view reason = {}) {
  std::ostringstream xml;
  xml << "<result xmlns=\"" << kNamespace << "\">"
      << (result ? "true" : "false") << "</result>";
  if (!reason.empty()) {
    xml << "<reason xmlns=\"" << kNamespace << "\">";
    for (const char character : reason) {
      if (character == '&') xml << "&amp;";
      else if (character == '<') xml << "&lt;";
      else if (character == '>') xml << "&gt;";
      else xml << character;
    }
    xml << "</reason>";
  }
  return xml.str();
}

std::string NexthopOutput(std::uint32_t id) {
  std::ostringstream xml;
  xml << BooleanOutput(true) << "<nexthop-id xmlns=\"" << kNamespace
      << "\">" << id << "</nexthop-id>";
  return xml.str();
}

bool ParseBaseNexthop(xmlNodePtr root, NexthopRegistry::Entry* entry,
                      std::string* reason) {
  xmlNodePtr base = Child(root, "nexthop-base");
  if (!base) { *reason = "only a base nexthop is supported"; return false; }
  entry->gateway = Text(Child(base, "ipv4-address"));
  if (entry->gateway->empty()) entry->gateway = Text(Child(base, "ipv6-address"));
  entry->interface = Text(Child(base, "outgoing-interface"));
  for (const std::string_view combined : {"egress-interface-ipv4-address",
                                          "egress-interface-ipv6-address"}) {
    if (xmlNodePtr pair = Child(base, combined)) {
      entry->interface = Text(Child(pair, "outgoing-interface"));
      entry->gateway = Text(Child(pair, combined == "egress-interface-ipv4-address"
                                            ? "ipv4-address" : "ipv6-address"));
    }
  }
  if (entry->gateway->empty()) entry->gateway.reset();
  if (entry->interface->empty()) entry->interface.reset();
  if (!entry->gateway && !entry->interface) {
    *reason = "the base nexthop requires an IP address or outgoing interface";
    return false;
  }
  return true;
}

bool NumericRib(std::string_view name, NativePlatform platform) {
  std::uint32_t value = 0;
  const auto parsed = std::from_chars(name.data(), name.data() + name.size(),
                                      value);
  if (parsed.ec != std::errc{} || parsed.ptr != name.data() + name.size())
    return false;
  if (platform == NativePlatform::kLinux) return value != 0U;
#if defined(__FreeBSD__)
  unsigned fib_count = 0;
  size_t size = sizeof(fib_count);
  return sysctlbyname("net.fibs", &fib_count, &size, nullptr, 0) == 0 &&
         value < fib_count;
#else
  return true;  // FreeBSD behavior is exercised only in a FreeBSD build.
#endif
}

}  // namespace

std::optional<std::uint32_t> NexthopRegistry::Add(Entry entry) {
  std::lock_guard lock(mutex_);
  for (std::uint64_t attempts = 0;
       attempts < std::numeric_limits<std::uint32_t>::max(); ++attempts) {
    const std::uint32_t id = next_id_++;
    if (next_id_ == 0) next_id_ = 1;
    if (entries_.emplace(std::make_pair(entry.rib, id), entry).second) return id;
  }
  return std::nullopt;
}

bool NexthopRegistry::Remove(const std::string& rib, std::uint32_t id) {
  std::lock_guard lock(mutex_);
  return entries_.erase({rib, id}) == 1;
}

bool NexthopRegistry::Resolve(const std::string& rib, std::uint32_t id,
                              std::optional<std::string>* gateway,
                              std::optional<std::string>* interface) {
  if (!gateway || !interface) return false;
  std::lock_guard lock(mutex_);
  const auto found = entries_.find({rib, id});
  if (found == entries_.end()) return false;
  *gateway = found->second.gateway;
  *interface = found->second.interface;
  return true;
}

bool InvokeRouteAdd(NativePlatform platform, const char* input_xml,
                    std::string* output_xml, std::string* error,
                    std::string* error_path, const CommandRunner& runner,
                    const NexthopResolver& resolver) {
  if (!input_xml || !output_xml || !error || !error_path) return false;
  xmlDocPtr raw = xmlReadMemory(input_xml, static_cast<int>(std::strlen(input_xml)),
                                "route-add.xml", nullptr,
                                XML_PARSE_NONET | XML_PARSE_NOBLANKS |
                                    XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
  std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> input(raw, xmlFreeDoc);
  xmlNodePtr root = input ? xmlDocGetRootElement(input.get()) : nullptr;
  if (!Is(root, "route-add")) {
    *error = "route-add input is not RFC 8431 XML";
    *error_path = "/ietf-i2rs-rib:route-add";
    return false;
  }
  const std::string rib_name = Text(Child(root, "rib-name"));
  xmlNodePtr routes_node = Child(root, "routes");
  if (rib_name.empty() || !routes_node) {
    *error = "route-add requires rib-name and routes";
    *error_path = "/ietf-i2rs-rib:route-add";
    return false;
  }
  const bool details = Boolean(Child(root, "return-failure-detail"));
  unsigned success = 0;
  std::vector<std::pair<std::uint64_t, unsigned>> failed;
  for (xmlNodePtr node = routes_node->children; node; node = node->next) {
    if (!Is(node, "route-list")) continue;
    // Reuse the configuration parser by constructing the equivalent RIB
    // context around this route. This keeps RPC and datastore validation from
    // drifting as the supported route projection grows.
    xmlDocPtr synthetic_raw = xmlNewDoc(BAD_CAST "1.0");
    std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> synthetic(synthetic_raw,
                                                            xmlFreeDoc);
    xmlNodePtr instance = xmlNewNode(nullptr, BAD_CAST "routing-instance");
    xmlNsPtr ns = xmlNewNs(instance, BAD_CAST kNamespace.data(), nullptr);
    xmlSetNs(instance, ns);
    xmlDocSetRootElement(synthetic.get(), instance);
    xmlNewTextChild(instance, ns, BAD_CAST "name", BAD_CAST "default");
    xmlNodePtr rib = xmlNewChild(instance, ns, BAD_CAST "rib-list", nullptr);
    xmlNewTextChild(rib, ns, BAD_CAST "name", BAD_CAST rib_name.c_str());
    const bool ipv6 = Child(Child(node, "match"), "ipv6") != nullptr;
    xmlNewTextChild(rib, ns, BAD_CAST "address-family",
                    BAD_CAST(ipv6 ? "ipv6" : "ipv4"));
    xmlAddChild(rib, xmlDocCopyNode(node, synthetic.get(), 1));
    xmlChar* serialized = nullptr;
    int serialized_size = 0;
    xmlDocDumpMemory(synthetic.get(), &serialized, &serialized_size);
    Config config;
    std::string parse_error;
    std::string parse_path;
    const bool parsed = serialized && ParseConfig(
        reinterpret_cast<const char*>(serialized), &config, &parse_error,
        &parse_path, resolver);
    if (serialized) xmlFree(serialized);
    std::uint64_t index = 0;
    const std::string index_text = Text(Child(node, "route-index"));
    try { index = std::stoull(index_text); } catch (...) { index = 0; }
    if (!parsed || config.routes.size() != 1) {
      failed.emplace_back(index, 3U);
      continue;
    }
    ExecutionResult result = ExecuteChanges(
        platform, {{ChangeKind::kInstall, config.routes.front()}}, runner);
    if (result.ok) ++success;
    else failed.emplace_back(index, 0U);
  }
  *output_xml = Output(success, failed, details);
  return true;
}

bool InvokeRouteDelete(NativePlatform platform, const char* input_xml,
                       std::string* output_xml, std::string* error,
                       std::string* error_path, const CommandRunner& runner,
                       const RouteObserver& supplied_observer) {
  if (!input_xml || !output_xml || !error || !error_path) return false;
  xmlDocPtr raw = xmlReadMemory(input_xml, static_cast<int>(std::strlen(input_xml)),
                                "route-delete.xml", nullptr,
                                XML_PARSE_NONET | XML_PARSE_NOBLANKS |
                                    XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
  std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> input(raw, xmlFreeDoc);
  xmlNodePtr root = input ? xmlDocGetRootElement(input.get()) : nullptr;
  if (!Is(root, "route-delete")) {
    *error = "route-delete input is not RFC 8431 XML";
    *error_path = "/ietf-i2rs-rib:route-delete";
    return false;
  }
  const std::string rib_name = Text(Child(root, "rib-name"));
  xmlNodePtr routes_node = Child(root, "routes");
  if (rib_name.empty() || !routes_node) {
    *error = "route-delete requires rib-name and routes";
    *error_path = "/ietf-i2rs-rib:route-delete";
    return false;
  }
  std::vector<ObservedRoute> observed;
  std::string observe_error;
  const bool observed_ok = Inventory(platform, supplied_observer, &observed,
                                     &observe_error);
  if (!observed_ok) {
    *error = "cannot read host RIB: " + observe_error;
    *error_path = "/ietf-i2rs-rib:route-delete/routes";
    return false;
  }
  const bool details = Boolean(Child(root, "return-failure-detail"));
  unsigned success = 0;
  std::vector<std::pair<std::uint64_t, unsigned>> failed;
  for (xmlNodePtr node = routes_node->children; node; node = node->next) {
    if (!Is(node, "route-list")) continue;
    std::uint64_t index = 0;
    try { index = std::stoull(Text(Child(node, "route-index"))); }
    catch (...) { failed.emplace_back(0U, 3U); continue; }
    xmlNodePtr match = Child(node, "match");
    xmlNodePtr family = Child(match, "ipv4");
    bool ipv6 = false;
    if (!family) { family = Child(match, "ipv6"); ipv6 = true; }
    const std::string destination = Text(Child(
        family, ipv6 ? "dest-ipv6-prefix" : "dest-ipv4-prefix"));
    if (destination.empty()) { failed.emplace_back(index, 3U); continue; }
    std::vector<const ObservedRoute*> matches;
    for (const ObservedRoute& candidate : observed)
      if (candidate.route.rib == rib_name &&
          candidate.route.address_family == (ipv6 ? "ipv6" : "ipv4") &&
          candidate.route.destination == destination)
        matches.push_back(&candidate);
    if (matches.empty()) { failed.emplace_back(index, 2U); continue; }
    if (matches.size() != 1U) { failed.emplace_back(index, 0U); continue; }
    Route route = matches.front()->route;
    route.index = index;
    const ExecutionResult result = ExecuteChanges(
        platform, {{ChangeKind::kDelete, std::move(route)}}, runner);
    if (result.ok) ++success;
    else failed.emplace_back(index, 0U);
  }
  *output_xml = Output(success, failed, details);
  return true;
}

bool InvokeRouteUpdate(NativePlatform platform, const char* input_xml,
                       std::string* output_xml, std::string* error,
                       std::string* error_path, const CommandRunner& runner,
                       const RouteObserver& observer,
                       const NexthopResolver& resolver) {
  if (!input_xml || !output_xml || !error || !error_path) return false;
  xmlDocPtr raw = xmlReadMemory(input_xml, static_cast<int>(std::strlen(input_xml)),
                                "route-update.xml", nullptr,
                                XML_PARSE_NONET | XML_PARSE_NOBLANKS |
                                    XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
  std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> input(raw, xmlFreeDoc);
  xmlNodePtr root = input ? xmlDocGetRootElement(input.get()) : nullptr;
  if (!Is(root, "route-update")) {
    *error = "route-update input is not RFC 8431 XML";
    *error_path = "/ietf-i2rs-rib:route-update";
    return false;
  }
  const std::string rib_name = Text(Child(root, "rib-name"));
  xmlNodePtr input_routes = Child(root, "input-routes");
  if (rib_name.empty() || !input_routes) {
    *error = "only prefix-matched route-update is currently supported";
    *error_path = "/ietf-i2rs-rib:route-update/input-routes";
    return false;
  }
  std::vector<ObservedRoute> observed;
  std::string observe_error;
  if (!Inventory(platform, observer, &observed, &observe_error)) {
    *error = "cannot read host RIB: " + observe_error;
    *error_path = "/ietf-i2rs-rib:route-update/input-routes";
    return false;
  }
  const bool details = Boolean(Child(root, "return-failure-detail"));
  unsigned success = 0;
  std::vector<std::pair<std::uint64_t, unsigned>> failed;
  for (xmlNodePtr node = input_routes->children; node; node = node->next) {
    if (!Is(node, "route-list")) continue;
    std::uint64_t index = 0;
    try { index = std::stoull(Text(Child(node, "route-index"))); }
    catch (...) { failed.emplace_back(0U, 3U); continue; }
    xmlNodePtr match = Child(node, "match");
    xmlNodePtr family = Child(match, "ipv4");
    bool ipv6 = false;
    if (!family) { family = Child(match, "ipv6"); ipv6 = true; }
    const std::string destination = Text(Child(
        family, ipv6 ? "dest-ipv6-prefix" : "dest-ipv4-prefix"));
    if (destination.empty()) { failed.emplace_back(index, 3U); continue; }
    std::vector<const ObservedRoute*> matches;
    for (const ObservedRoute& candidate : observed)
      if (candidate.route.rib == rib_name &&
          candidate.route.address_family == (ipv6 ? "ipv6" : "ipv4") &&
          candidate.route.destination == destination)
        matches.push_back(&candidate);
    if (matches.empty()) { failed.emplace_back(index, 2U); continue; }
    if (matches.size() != 1U) { failed.emplace_back(index, 0U); continue; }
    Route replacement = matches.front()->route;
    replacement.index = index;
    if (xmlNodePtr updated = Child(node, "updated-nexthop")) {
      xmlNodePtr base = Child(updated, "nexthop-base");
      if (!base) { failed.emplace_back(index, 3U); continue; }
      replacement.gateway = Text(Child(base, ipv6 ? "ipv6-address"
                                                   : "ipv4-address"));
      replacement.interface = Text(Child(base, "outgoing-interface"));
      if (xmlNodePtr combined = Child(
              base, ipv6 ? "egress-interface-ipv6-address"
                         : "egress-interface-ipv4-address")) {
        replacement.gateway = Text(Child(combined, ipv6 ? "ipv6-address"
                                                         : "ipv4-address"));
        replacement.interface = Text(Child(combined, "outgoing-interface"));
      }
      if (xmlNodePtr reference = Child(base, "nexthop-ref")) {
        std::uint32_t id = 0;
        if (!resolver || !Unsigned(reference, &id) ||
            !resolver(rib_name, id, &replacement.gateway,
                      &replacement.interface)) {
          failed.emplace_back(index, 2U);
          continue;
        }
      }
      if (replacement.gateway->empty()) replacement.gateway.reset();
      if (replacement.interface->empty()) replacement.interface.reset();
      if (!replacement.gateway && !replacement.interface) {
        failed.emplace_back(index, 3U); continue;
      }
    } else if (xmlNodePtr attributes = Child(node, "updated-route-attr")) {
      bool local_only = false;
      const std::string local = Text(Child(attributes, "local-only"));
      if (!Unsigned(Child(attributes, "route-preference"),
                    &replacement.preference) ||
          (local != "true" && local != "1" && local != "false" &&
           local != "0")) {
        failed.emplace_back(index, 3U); continue;
      }
      local_only = local == "true" || local == "1";
      replacement.local_only = local_only;
    } else {
      failed.emplace_back(index, 3U); continue;
    }
    const Route original = matches.front()->route;
    const ExecutionResult result = ExecuteChanges(
        platform, {{ChangeKind::kDelete, original},
                   {ChangeKind::kInstall, replacement}}, runner);
    if (result.ok) ++success;
    else failed.emplace_back(index, 0U);
  }
  *output_xml = Output(success, failed, details);
  return true;
}

bool InvokeRibAdd(NativePlatform platform, const char* input_xml,
                  std::string* output_xml, std::string* error,
                  std::string* error_path) {
  if (!input_xml || !output_xml || !error || !error_path) return false;
  xmlDocPtr raw = xmlReadMemory(input_xml, static_cast<int>(std::strlen(input_xml)),
                                "rib-add.xml", nullptr,
                                XML_PARSE_NONET | XML_PARSE_NOBLANKS |
                                    XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
  std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> input(raw, xmlFreeDoc);
  xmlNodePtr root = input ? xmlDocGetRootElement(input.get()) : nullptr;
  if (!Is(root, "rib-add")) {
    *error = "rib-add input is not RFC 8431 XML";
    *error_path = "/ietf-i2rs-rib:rib-add";
    return false;
  }
  const std::string name = Text(Child(root, "name"));
  const std::string family = Text(Child(root, "address-family"));
  if (name.empty() || (family != "ipv4" && family != "ipv6")) {
    *error = "rib-add requires a name and IPv4 or IPv6 address family";
    *error_path = "/ietf-i2rs-rib:rib-add";
    return false;
  }
  if (!NumericRib(name, platform)) {
    *output_xml = BooleanOutput(false, "the platform requires a numeric RIB/FIB name");
    return true;
  }
  if (Boolean(Child(root, "ip-rpf-check"))) {
    *output_xml = BooleanOutput(false, "IP RPF checks are not implemented by this provider");
    return true;
  }
  // Linux tables spring into existence with their first route. FreeBSD FIBs
  // are boot-time objects. In both cases a successful reply means the numeric
  // namespace is acceptable, not that a new kernel object was allocated.
  *output_xml = BooleanOutput(true);
  return true;
}

bool InvokeRibDelete(NativePlatform platform, const char* input_xml,
                     std::string* output_xml, std::string* error,
                     std::string* error_path, const CommandRunner& runner,
                     const RouteObserver& observer) {
  if (!input_xml || !output_xml || !error || !error_path) return false;
  xmlDocPtr raw = xmlReadMemory(input_xml, static_cast<int>(std::strlen(input_xml)),
                                "rib-delete.xml", nullptr,
                                XML_PARSE_NONET | XML_PARSE_NOBLANKS |
                                    XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
  std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> input(raw, xmlFreeDoc);
  xmlNodePtr root = input ? xmlDocGetRootElement(input.get()) : nullptr;
  if (!Is(root, "rib-delete")) {
    *error = "rib-delete input is not RFC 8431 XML";
    *error_path = "/ietf-i2rs-rib:rib-delete";
    return false;
  }
  const std::string name = Text(Child(root, "name"));
  if (name.empty()) {
    *error = "rib-delete requires a RIB name";
    *error_path = "/ietf-i2rs-rib:rib-delete/name";
    return false;
  }
  if (!NumericRib(name, platform)) {
    *output_xml = BooleanOutput(false, "the platform requires a numeric RIB/FIB name");
    return true;
  }
  std::vector<ObservedRoute> observed;
  std::string observe_error;
  if (!Inventory(platform, observer, &observed, &observe_error)) {
    *error = "cannot read host RIB: " + observe_error;
    *error_path = "/ietf-i2rs-rib:rib-delete/name";
    return false;
  }
  std::vector<Change> deletions;
  for (const ObservedRoute& route : observed)
    if (route.route.rib == name)
      deletions.push_back({ChangeKind::kDelete, route.route});
  const ExecutionResult result = ExecuteChanges(platform, deletions, runner);
  if (!result.ok) {
    *output_xml = BooleanOutput(false, result.error);
    return true;
  }
  *output_xml = BooleanOutput(true);
  return true;
}

bool InvokeNexthopAdd(NexthopRegistry* registry, const char* input_xml,
                       std::string* output_xml, std::string* error,
                       std::string* error_path) {
  if (!registry || !input_xml || !output_xml || !error || !error_path) return false;
  xmlDocPtr raw = xmlReadMemory(input_xml, static_cast<int>(std::strlen(input_xml)),
                                "nh-add.xml", nullptr, XML_PARSE_NONET |
                                XML_PARSE_NOBLANKS | XML_PARSE_NOERROR |
                                XML_PARSE_NOWARNING);
  std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> input(raw, xmlFreeDoc);
  xmlNodePtr root = input ? xmlDocGetRootElement(input.get()) : nullptr;
  if (!Is(root, "nh-add")) {
    *error = "nh-add input is not RFC 8431 XML";
    *error_path = "/ietf-i2rs-rib:nh-add";
    return false;
  }
  NexthopRegistry::Entry entry;
  entry.rib = Text(Child(root, "rib-name"));
  if (entry.rib.empty()) {
    *error = "nh-add requires a RIB name";
    *error_path = "/ietf-i2rs-rib:nh-add/rib-name";
    return false;
  }
  if (Child(root, "nexthop-id")) {
    *output_xml = BooleanOutput(false, "nexthop-id is allocated by nh-add");
    return true;
  }
  const std::string sharing = Text(Child(root, "sharing-flag"));
  if (!sharing.empty() && sharing != "true" && sharing != "1" &&
      sharing != "false" && sharing != "0") {
    *output_xml = BooleanOutput(false, "sharing-flag is not a boolean");
    return true;
  }
  entry.sharable = sharing == "true" || sharing == "1";
  std::string reason;
  if (!ParseBaseNexthop(root, &entry, &reason)) {
    *output_xml = BooleanOutput(false, reason);
    return true;
  }
  const auto id = registry->Add(std::move(entry));
  *output_xml = id ? NexthopOutput(*id)
                   : BooleanOutput(false, "the nexthop identifier space is exhausted");
  return true;
}

bool InvokeNexthopDelete(NexthopRegistry* registry, const char* input_xml,
                          std::string* output_xml, std::string* error,
                          std::string* error_path) {
  if (!registry || !input_xml || !output_xml || !error || !error_path) return false;
  xmlDocPtr raw = xmlReadMemory(input_xml, static_cast<int>(std::strlen(input_xml)),
                                "nh-delete.xml", nullptr, XML_PARSE_NONET |
                                XML_PARSE_NOBLANKS | XML_PARSE_NOERROR |
                                XML_PARSE_NOWARNING);
  std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> input(raw, xmlFreeDoc);
  xmlNodePtr root = input ? xmlDocGetRootElement(input.get()) : nullptr;
  if (!Is(root, "nh-delete")) {
    *error = "nh-delete input is not RFC 8431 XML";
    *error_path = "/ietf-i2rs-rib:nh-delete";
    return false;
  }
  const std::string rib = Text(Child(root, "rib-name"));
  std::uint32_t id = 0;
  if (rib.empty() || !Unsigned(Child(root, "nexthop-id"), &id)) {
    *error = "nh-delete requires a RIB name and nexthop identifier";
    *error_path = "/ietf-i2rs-rib:nh-delete";
    return false;
  }
  *output_xml = registry->Remove(rib, id)
                    ? BooleanOutput(true)
                    : BooleanOutput(false,
                          "the nexthop identifier does not exist in this RIB");
  return true;
}

}  // namespace dang::rib

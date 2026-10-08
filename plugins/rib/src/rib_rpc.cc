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

std::string LocalIdentity(std::string value) {
  const std::size_t colon = value.find(':');
  return colon == std::string::npos ? value : value.substr(colon + 1U);
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
  const std::string special = LocalIdentity(Text(Child(base, "special")));
  if (!special.empty()) entry->special = special;
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
  if (entry->special && *entry->special != "discard" &&
      *entry->special != "discard-with-error") {
    *reason = *entry->special == "receive"
        ? "receive nexthops are kernel-owned and read-only"
        : "the special nexthop is not supported by the portable backend";
    return false;
  }
  if (entry->special && (entry->gateway || entry->interface)) {
    *reason = "a special nexthop cannot include another base nexthop";
    return false;
  }
  if (!entry->special && !entry->gateway && !entry->interface) {
    *reason = "the base nexthop requires an IP address, outgoing interface, "
              "or supported special identity";
    return false;
  }
  if (entry->gateway)
    entry->address_family = entry->gateway->find(':') == std::string::npos
                                ? "ipv4" : "ipv6";
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

std::optional<Route> NativeRoute(const Route& route,
                                 const RibNameResolver& resolver) {
  Route result = route;
  if (resolver) {
    const auto native = resolver(route.rib, route.address_family);
    if (!native) return std::nullopt;
    result.rib = *native;
  }
  return result;
}

bool PersistRegistryChange(NativePlatform platform, NexthopRegistry* registry,
                           const PersistentRegistry& before,
                           const RegistryWriter& writer,
                           const std::vector<Change>& compensation,
                           const CommandRunner& runner, std::string_view operation,
                           std::string* error, std::string* error_path) {
  if (!registry || !writer) return true;
  const PersistentRegistry after = registry->PersistentState();
  if (after == before) return true;
  std::string persistence_error;
  if (writer(after, &persistence_error)) return true;

  std::string failure = "cannot persist " + std::string(operation) +
                        " registry change: " + persistence_error;
  if (!compensation.empty()) {
    const ExecutionResult rollback =
        ExecuteChanges(platform, compensation, runner);
    if (!rollback.ok) {
      failure += "; native compensation failed: " + rollback.error;
      for (const auto& item : rollback.rollback_failures)
        failure += "; compensation rollback failed: " + item;
    }
  }
  std::string restore_error;
  if (!registry->ReplacePersistentState(before, &restore_error))
    failure += "; registry rollback failed: " + restore_error;
  *error = std::move(failure);
  *error_path = "/ietf-i2rs-rib:" + std::string(operation);
  return false;
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

NexthopRegistry::RegisterRibResult NexthopRegistry::RegisterRib(
    const std::string& rib, const std::string& family) {
  std::lock_guard lock(mutex_);
  const auto [found, inserted] = rib_families_.emplace(rib, family);
  if (inserted) return RegisterRibResult::kRegistered;
  return found->second == family ? RegisterRibResult::kExisting
                                 : RegisterRibResult::kConflict;
}

std::optional<std::string> NexthopRegistry::RibFamily(const std::string& rib) {
  std::lock_guard lock(mutex_);
  const auto found = rib_families_.find(rib);
  return found == rib_families_.end() ? std::nullopt
                                      : std::optional(found->second);
}

NexthopRegistry::RemoveResult NexthopRegistry::Remove(
    const std::string& rib, std::uint32_t id) {
  std::lock_guard lock(mutex_);
  const auto key = std::make_pair(rib, id);
  if (!entries_.contains(key)) return RemoveResult::kMissing;
  if (references_.contains(key)) return RemoveResult::kInUse;
  entries_.erase(key);
  return RemoveResult::kRemoved;
}

bool NexthopRegistry::Retain(const std::string& rib, std::uint32_t id) {
  std::lock_guard lock(mutex_);
  const auto key = std::make_pair(rib, id);
  if (!entries_.contains(key)) return false;
  ++references_[key];
  return true;
}

void NexthopRegistry::Release(const std::string& rib, std::uint32_t id) {
  std::lock_guard lock(mutex_);
  const auto key = std::make_pair(rib, id);
  const auto found = references_.find(key);
  if (found == references_.end()) return;
  if (--found->second == 0) references_.erase(found);
}

void NexthopRegistry::BindRoute(
    const Route& route, std::optional<std::uint32_t> reserved_reference) {
  std::lock_guard lock(mutex_);
  const auto route_key = std::make_tuple(route.rib, route.address_family,
                                         route.destination, route.index);
  const auto old = route_references_.find(route_key);
  if (old != route_references_.end()) {
    const auto reference_key = std::make_pair(route.rib, old->second);
    const auto count = references_.find(reference_key);
    if (count != references_.end() && --count->second == 0)
      references_.erase(count);
    route_references_.erase(old);
  }
  if (reserved_reference)
    route_references_.emplace(route_key, *reserved_reference);
}

void NexthopRegistry::ForgetRoute(const Route& route) {
  BindRoute(route, std::nullopt);
}

void NexthopRegistry::ForgetRib(const std::string& rib) {
  std::lock_guard lock(mutex_);
  for (auto route = route_references_.begin(); route != route_references_.end();) {
    if (std::get<0>(route->first) != rib) { ++route; continue; }
    const auto key = std::make_pair(rib, route->second);
    const auto count = references_.find(key);
    if (count != references_.end() && --count->second == 0)
      references_.erase(count);
    route = route_references_.erase(route);
  }
  rib_families_.erase(rib);
}

std::optional<std::uint32_t> NexthopRegistry::RouteReference(
    const Route& route) {
  std::lock_guard lock(mutex_);
  const auto found = route_references_.find(
      std::make_tuple(route.rib, route.address_family, route.destination,
                      route.index));
  return found == route_references_.end()
             ? std::nullopt : std::optional<std::uint32_t>(found->second);
}

bool NexthopRegistry::ReplaceConfigurationReferences(
    const std::vector<std::pair<std::string, std::uint32_t>>& requested) {
  std::lock_guard lock(mutex_);
  std::map<std::pair<std::string, std::uint32_t>, std::size_t> replacement;
  for (const auto& reference : requested) {
    if (!entries_.contains(reference)) return false;
    ++replacement[reference];
  }
  for (const auto& [reference, count] : configuration_references_) {
    auto total = references_.find(reference);
    if (total == references_.end() || total->second < count) return false;
    total->second -= count;
    if (total->second == 0) references_.erase(total);
  }
  for (const auto& [reference, count] : replacement)
    references_[reference] += count;
  configuration_references_ = std::move(replacement);
  return true;
}

bool NexthopRegistry::ReplaceConfigurationRouteBindings(
    const std::vector<Route>& routes) {
  std::lock_guard lock(mutex_);
  std::map<std::tuple<std::string, std::string, std::string, std::uint64_t>,
           std::uint32_t> replacement;
  for (const Route& route : routes) {
    if (!route.nexthop_ref) continue;
    if (!entries_.contains({route.rib, *route.nexthop_ref}) ||
        !replacement.emplace(
            std::make_tuple(route.rib, route.address_family, route.destination,
                            route.index),
            *route.nexthop_ref).second)
      return false;
  }
  configuration_route_references_ = std::move(replacement);
  return true;
}

bool NexthopRegistry::Resolve(const std::string& rib, std::uint32_t id,
                              std::optional<std::string>* gateway,
                              std::optional<std::string>* interface,
                              std::optional<std::string>* special) {
  if (!gateway || !interface || !special) return false;
  std::lock_guard lock(mutex_);
  const auto found = entries_.find({rib, id});
  if (found == entries_.end()) return false;
  *gateway = found->second.gateway;
  *interface = found->second.interface;
  *special = found->second.special;
  return true;
}

PersistentRegistry NexthopRegistry::PersistentState() {
  std::lock_guard lock(mutex_);
  PersistentRegistry state;
  state.next_id = next_id_;
  state.ribs.reserve(rib_families_.size());
  for (const auto& [name, family] : rib_families_)
    state.ribs.push_back({name, family});
  state.nexthops.reserve(entries_.size());
  for (const auto& [key, entry] : entries_) {
    state.nexthops.push_back({key.first, key.second, entry.gateway,
                              entry.interface, entry.address_family,
                              entry.sharable, entry.special});
  }
  state.bindings.reserve(route_references_.size());
  for (const auto& [route, id] : route_references_) {
    state.bindings.push_back(
        {std::get<0>(route), std::get<1>(route), std::get<2>(route),
         std::get<3>(route), id});
  }
  return state;
}

PersistentRegistry NexthopRegistry::ResolutionState() {
  std::lock_guard lock(mutex_);
  PersistentRegistry state;
  state.next_id = next_id_;
  for (const auto& [name, family] : rib_families_)
    state.ribs.push_back({name, family});
  for (const auto& [key, entry] : entries_)
    state.nexthops.push_back({key.first, key.second, entry.gateway,
                              entry.interface, entry.address_family,
                              entry.sharable, entry.special});
  for (const auto& [route, id] : route_references_)
    state.bindings.push_back(
        {std::get<0>(route), std::get<1>(route), std::get<2>(route),
         std::get<3>(route), id});
  for (const auto& [route, id] : configuration_route_references_)
    state.bindings.push_back(
        {std::get<0>(route), std::get<1>(route), std::get<2>(route),
         std::get<3>(route), id});
  return state;
}

bool NexthopRegistry::RestorePersistentState(const PersistentRegistry& state,
                                             std::string* error) {
  if (!error) return false;
  if (state.next_id == 0) {
    *error = "registry next-id must be nonzero";
    return false;
  }
  std::map<std::pair<std::string, std::uint32_t>, Entry> entries;
  std::map<std::string, std::string> ribs;
  for (const auto& item : state.ribs) {
    if (item.name.empty() ||
        (item.address_family != "ipv4" && item.address_family != "ipv6") ||
        !ribs.emplace(item.name, item.address_family).second) {
      *error = "registry contains an invalid or duplicate RIB";
      return false;
    }
  }
  for (const auto& item : state.nexthops) {
    const bool supported_special =
        !item.special || *item.special == "discard" ||
        *item.special == "discard-with-error";
    const bool valid_shape =
        !item.special || (!item.gateway && !item.interface &&
                          item.address_family.has_value());
    if (item.rib.empty() || item.id == 0 || !supported_special ||
        !valid_shape ||
        (ribs.contains(item.rib) && item.address_family &&
         ribs.at(item.rib) != *item.address_family) ||
        !entries.emplace(std::make_pair(item.rib, item.id),
                         Entry{item.rib, item.gateway, item.interface,
                               item.address_family, item.sharable,
                               item.special})
             .second) {
      *error = "registry contains an invalid, conflicting, or duplicate nexthop";
      return false;
    }
  }
  std::map<std::tuple<std::string, std::string, std::string, std::uint64_t>,
           std::uint32_t> bindings;
  std::map<std::pair<std::string, std::uint32_t>, std::size_t> references;
  for (const auto& item : state.bindings) {
    const auto nexthop = std::make_pair(item.rib, item.nexthop_id);
    const auto route =
        std::make_tuple(item.rib, item.address_family, item.destination,
                        item.route_index);
    if (!entries.contains(nexthop) || item.address_family.empty() ||
        item.destination.empty() || !bindings.emplace(route, item.nexthop_id).second) {
      *error = "registry contains an invalid route binding";
      return false;
    }
    ++references[nexthop];
  }

  std::lock_guard lock(mutex_);
  if (!entries_.empty() || !rib_families_.empty() || !references_.empty() ||
      !configuration_references_.empty() || !route_references_.empty() ||
      !configuration_route_references_.empty()) {
    *error = "persistent state can only restore an empty registry";
    return false;
  }
  entries_ = std::move(entries);
  rib_families_ = std::move(ribs);
  references_ = std::move(references);
  route_references_ = std::move(bindings);
  next_id_ = state.next_id;
  return true;
}

bool NexthopRegistry::ReplacePersistentState(const PersistentRegistry& state,
                                             std::string* error) {
  NexthopRegistry candidate;
  if (!candidate.RestorePersistentState(state, error)) return false;
  std::scoped_lock lock(mutex_, candidate.mutex_);
  for (const auto& [reference, count] : configuration_references_) {
    if (!candidate.entries_.contains(reference)) {
      *error = "checkpoint omits a datastore-referenced nexthop";
      return false;
    }
    candidate.references_[reference] += count;
  }
  for (const auto& [route, id] : configuration_route_references_) {
    if (!candidate.entries_.contains({std::get<0>(route), id})) {
      *error = "checkpoint omits a datastore-bound nexthop";
      return false;
    }
    candidate.configuration_route_references_[route] = id;
  }
  entries_ = std::move(candidate.entries_);
  rib_families_ = std::move(candidate.rib_families_);
  references_ = std::move(candidate.references_);
  route_references_ = std::move(candidate.route_references_);
  configuration_route_references_ =
      std::move(candidate.configuration_route_references_);
  next_id_ = candidate.next_id_;
  return true;
}

bool InvokeRouteAdd(NativePlatform platform, const char* input_xml,
                    std::string* output_xml, std::string* error,
                    std::string* error_path, const CommandRunner& runner,
                    const NexthopResolver& resolver,
                    NexthopRegistry* registry, const RegistryWriter& writer,
                    const RouteEventSink& events,
                    const RibNameResolver& native_rib) {
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
  const PersistentRegistry before =
      registry ? registry->PersistentState() : PersistentRegistry{};
  unsigned success = 0;
  std::vector<std::pair<std::uint64_t, unsigned>> failed;
  std::vector<Route> installed;
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
    Route route = config.routes.front();
    const auto native_route = NativeRoute(route, native_rib);
    if (!native_route) {
      failed.emplace_back(index, 2U);
      continue;
    }
    if (route.nexthop_ref &&
        (!registry || !registry->Retain(route.rib, *route.nexthop_ref))) {
      failed.emplace_back(index, 2U); continue;
    }
    ExecutionResult result = ExecuteChanges(
        platform, {{ChangeKind::kInstall, *native_route}}, runner);
    if (result.ok) {
      if (registry) registry->BindRoute(route, route.nexthop_ref);
      installed.push_back(route);
      ++success;
    } else {
      if (route.nexthop_ref)
        registry->Release(route.rib, *route.nexthop_ref);
      failed.emplace_back(index, 0U);
    }
  }
  std::vector<Change> compensation;
  for (auto route = installed.rbegin(); route != installed.rend(); ++route)
    compensation.push_back(
        {ChangeKind::kDelete, *NativeRoute(*route, native_rib)});
  if (!PersistRegistryChange(platform, registry, before, writer, compensation,
                             runner, "route-add", error, error_path))
    return false;
  if (events)
    for (const Route& route : installed) events(route, true);
  *output_xml = Output(success, failed, details);
  return true;
}

bool InvokeRouteDelete(NativePlatform platform, const char* input_xml,
                       std::string* output_xml, std::string* error,
                       std::string* error_path, const CommandRunner& runner,
                       const RouteObserver& supplied_observer,
                       NexthopRegistry* registry,
                       const RegistryWriter& writer,
                       const RouteEventSink& events,
                       const RibNameResolver& native_rib) {
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
  const PersistentRegistry before =
      registry ? registry->PersistentState() : PersistentRegistry{};
  unsigned success = 0;
  std::vector<std::pair<std::uint64_t, unsigned>> failed;
  std::vector<Route> deleted;
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
    if (!matches.front()->mutable_route) {
      failed.emplace_back(index, 0U);
      continue;
    }
    Route route = matches.front()->route;
    route.index = index;
    const auto native_route = NativeRoute(route, native_rib);
    if (!native_route) { failed.emplace_back(index, 2U); continue; }
    const ExecutionResult result = ExecuteChanges(
        platform, {{ChangeKind::kDelete, *native_route}}, runner);
    if (result.ok) {
      if (registry) registry->ForgetRoute(route);
      deleted.push_back(route);
      ++success;
    }
    else failed.emplace_back(index, 0U);
  }
  std::vector<Change> compensation;
  for (auto route = deleted.rbegin(); route != deleted.rend(); ++route)
    compensation.push_back(
        {ChangeKind::kInstall, *NativeRoute(*route, native_rib)});
  if (!PersistRegistryChange(platform, registry, before, writer, compensation,
                             runner, "route-delete", error, error_path))
    return false;
  if (events)
    for (const Route& route : deleted) events(route, false);
  *output_xml = Output(success, failed, details);
  return true;
}

bool InvokeRouteUpdate(NativePlatform platform, const char* input_xml,
                       std::string* output_xml, std::string* error,
                       std::string* error_path, const CommandRunner& runner,
                       const RouteObserver& observer,
                       const NexthopResolver& resolver,
                       NexthopRegistry* registry,
                       const RegistryWriter& writer,
                       const RouteEventSink& events,
                       const RibNameResolver& native_rib) {
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
  const PersistentRegistry before =
      registry ? registry->PersistentState() : PersistentRegistry{};
  unsigned success = 0;
  std::vector<std::pair<std::uint64_t, unsigned>> failed;
  std::vector<std::pair<Route, Route>> replacements;
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
    if (!matches.front()->mutable_route) {
      failed.emplace_back(index, 0U);
      continue;
    }
    Route replacement = matches.front()->route;
    replacement.index = index;
    if (registry)
      replacement.nexthop_ref = registry->RouteReference(replacement);
    if (xmlNodePtr updated = Child(node, "updated-nexthop")) {
      xmlNodePtr base = Child(updated, "nexthop-base");
      if (!base) { failed.emplace_back(index, 3U); continue; }
      replacement.gateway.reset();
      replacement.interface.reset();
      replacement.nexthop_ref.reset();
      replacement.special.reset();
      replacement.gateway = Text(Child(base, ipv6 ? "ipv6-address"
                                                   : "ipv4-address"));
      replacement.interface = Text(Child(base, "outgoing-interface"));
      const std::string special = LocalIdentity(Text(Child(base, "special")));
      if (!special.empty()) replacement.special = special;
      if (xmlNodePtr combined = Child(
              base, ipv6 ? "egress-interface-ipv6-address"
                         : "egress-interface-ipv4-address")) {
        replacement.gateway = Text(Child(combined, ipv6 ? "ipv6-address"
                                                         : "ipv4-address"));
        replacement.interface = Text(Child(combined, "outgoing-interface"));
      }
      if (replacement.gateway->empty()) replacement.gateway.reset();
      if (replacement.interface->empty()) replacement.interface.reset();
      if (xmlNodePtr reference = Child(base, "nexthop-ref")) {
        std::uint32_t id = 0;
        if (replacement.special || replacement.gateway ||
            replacement.interface || !resolver || !Unsigned(reference, &id) ||
            !resolver(rib_name, id, &replacement.gateway,
                      &replacement.interface, &replacement.special)) {
          failed.emplace_back(index, 2U);
          continue;
        }
        replacement.nexthop_ref = id;
      } else {
        replacement.nexthop_ref.reset();
      }
      if (replacement.special &&
          (*replacement.special != "discard" &&
           *replacement.special != "discard-with-error")) {
        failed.emplace_back(index, 3U); continue;
      }
      if (replacement.special &&
          (replacement.gateway || replacement.interface)) {
        failed.emplace_back(index, 3U); continue;
      }
      if (!replacement.special && !replacement.gateway &&
          !replacement.interface) {
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
      if (local_only) {
        failed.emplace_back(index, 3U);
        continue;
      }
      replacement.local_only = local_only;
    } else {
      failed.emplace_back(index, 3U); continue;
    }
    const Route original = matches.front()->route;
    const auto native_original = NativeRoute(original, native_rib);
    const auto native_replacement = NativeRoute(replacement, native_rib);
    if (!native_original || !native_replacement) {
      failed.emplace_back(index, 2U);
      continue;
    }
    if (replacement.nexthop_ref &&
        (!registry || !registry->Retain(rib_name, *replacement.nexthop_ref))) {
      failed.emplace_back(index, 2U); continue;
    }
    const ExecutionResult result = ExecuteChanges(
        platform, {{ChangeKind::kDelete, *native_original},
                   {ChangeKind::kInstall, *native_replacement}}, runner);
    if (result.ok) {
      if (registry) registry->BindRoute(replacement, replacement.nexthop_ref);
      replacements.emplace_back(original, replacement);
      ++success;
    } else {
      if (replacement.nexthop_ref)
        registry->Release(rib_name, *replacement.nexthop_ref);
      failed.emplace_back(index, 0U);
    }
  }
  std::vector<Change> compensation;
  for (auto replacement = replacements.rbegin();
       replacement != replacements.rend(); ++replacement) {
    compensation.push_back({ChangeKind::kDelete, replacement->second});
    compensation.back().route = *NativeRoute(replacement->second, native_rib);
    compensation.push_back(
        {ChangeKind::kInstall,
         *NativeRoute(replacement->first, native_rib)});
  }
  if (!PersistRegistryChange(platform, registry, before, writer, compensation,
                             runner, "route-update", error, error_path))
    return false;
  if (events)
    for (const auto& [original, replacement] : replacements) {
      events(original, false);
      events(replacement, true);
    }
  *output_xml = Output(success, failed, details);
  return true;
}

bool InvokeRibAdd(NativePlatform platform, const char* input_xml,
                  std::string* output_xml, std::string* error,
                  std::string* error_path, NexthopRegistry* registry,
                  const RegistryWriter& writer,
                  const RibNameResolver& native_rib) {
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
  std::string family = Text(Child(root, "address-family"));
  if (family == "ipv4-address-family") family = "ipv4";
  if (family == "ipv6-address-family") family = "ipv6";
  if (name.empty() || (family != "ipv4" && family != "ipv6")) {
    *error = "rib-add requires a name and IPv4 or IPv6 address family";
    *error_path = "/ietf-i2rs-rib:rib-add";
    return false;
  }
  const auto native_name = native_rib ? native_rib(name, family)
                                      : std::optional<std::string>(name);
  if (!native_name || !NumericRib(*native_name, platform)) {
    *output_xml = BooleanOutput(false, "the platform requires a numeric RIB/FIB name");
    return true;
  }
  if (Boolean(Child(root, "ip-rpf-check"))) {
    *output_xml = BooleanOutput(false, "IP RPF checks are not implemented by this provider");
    return true;
  }
  if (registry) {
    const PersistentRegistry before = registry->PersistentState();
    switch (registry->RegisterRib(name, family)) {
      case NexthopRegistry::RegisterRibResult::kConflict:
        *output_xml = BooleanOutput(
            false, "the RIB name is already registered with another address family");
        return true;
      case NexthopRegistry::RegisterRibResult::kRegistered:
        if (writer) {
          std::string persistence_error;
          if (!writer(registry->PersistentState(), &persistence_error)) {
            std::string restore_error;
            if (!registry->ReplacePersistentState(before, &restore_error))
              persistence_error += "; registry rollback failed: " + restore_error;
            *error = "cannot persist RIB family: " + persistence_error;
            *error_path = "/ietf-i2rs-rib:rib-add";
            return false;
          }
        }
        break;
      case NexthopRegistry::RegisterRibResult::kExisting:
        break;
    }
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
                     const RouteObserver& observer,
                     NexthopRegistry* registry,
                     const RegistryWriter& writer,
                     const RouteEventSink& events,
                     const RibNameResolver& native_rib) {
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
  const auto native_name = native_rib ? native_rib(name, "")
                                      : std::optional<std::string>(name);
  if (!native_name || !NumericRib(*native_name, platform)) {
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
  for (const ObservedRoute& route : observed)
    if (route.route.rib == name && !route.mutable_route) {
      *output_xml = BooleanOutput(
          false, "the RIB contains native route types this provider cannot mutate");
      return true;
    }
  std::vector<Change> deletions;
  for (const ObservedRoute& route : observed)
    if (route.route.rib == name) {
      const auto native_route = NativeRoute(route.route, native_rib);
      if (!native_route) {
        *output_xml = BooleanOutput(false, "the RIB has no platform mapping");
        return true;
      }
      deletions.push_back({ChangeKind::kDelete, *native_route});
    }
  const ExecutionResult result = ExecuteChanges(platform, deletions, runner);
  if (!result.ok) {
    *output_xml = BooleanOutput(false, result.error);
    return true;
  }
  *output_xml = BooleanOutput(true);
  const PersistentRegistry before =
      registry ? registry->PersistentState() : PersistentRegistry{};
  if (registry) registry->ForgetRib(name);
  std::vector<Change> compensation;
  for (auto deletion = deletions.rbegin(); deletion != deletions.rend();
       ++deletion)
    compensation.push_back({ChangeKind::kInstall, deletion->route});
  if (!PersistRegistryChange(platform, registry, before, writer, compensation,
                             runner, "rib-delete", error, error_path))
    return false;
  if (events)
    for (const ObservedRoute& route : observed)
      if (route.route.rib == name) events(route.route, false);
  return true;
}

bool InvokeNexthopAdd(NexthopRegistry* registry, const char* input_xml,
                       std::string* output_xml, std::string* error,
                       std::string* error_path,
                       const RegistryWriter& writer,
                       const RibNameResolver& native_rib) {
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
  // An outgoing interface or special identity does not encode an IP family.
  // Require the modeled RIB context established by rib-add instead of
  // guessing from host state.
  if (!entry.address_family) {
    entry.address_family = registry->RibFamily(entry.rib);
    if (!entry.address_family) {
      *output_xml = BooleanOutput(
          false, "family-neutral nexthop requires a prior rib-add address family");
      return true;
    }
  }
  // Validate the modeled identity before allocation or durability. Otherwise
  // nh-add could acknowledge state that the startup mapping guard rejects.
  if (native_rib && !native_rib(entry.rib, *entry.address_family)) {
    *output_xml = BooleanOutput(
        false, "RIB name has no mapping for the nexthop address family");
    return true;
  }
  const PersistentRegistry before = registry->PersistentState();
  const auto id = registry->Add(std::move(entry));
  if (id && writer) {
    std::string persistence_error;
    if (!writer(registry->PersistentState(), &persistence_error)) {
      std::string restore_error;
      if (!registry->ReplacePersistentState(before, &restore_error))
        persistence_error += "; registry rollback failed: " + restore_error;
      *error = "cannot persist reusable nexthop: " + persistence_error;
      *error_path = "/ietf-i2rs-rib:nh-add";
      return false;
    }
  }
  *output_xml = id ? NexthopOutput(*id)
                   : BooleanOutput(false, "the nexthop identifier space is exhausted");
  return true;
}

bool InvokeNexthopDelete(NexthopRegistry* registry, const char* input_xml,
                          std::string* output_xml, std::string* error,
                          std::string* error_path,
                          const RegistryWriter& writer) {
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
  const PersistentRegistry before = registry->PersistentState();
  switch (registry->Remove(rib, id)) {
    case NexthopRegistry::RemoveResult::kRemoved:
      if (writer) {
        std::string persistence_error;
        if (!writer(registry->PersistentState(), &persistence_error)) {
          std::string restore_error;
          if (!registry->ReplacePersistentState(before, &restore_error))
            persistence_error += "; registry rollback failed: " + restore_error;
          *error = "cannot persist nexthop deletion: " + persistence_error;
          *error_path = "/ietf-i2rs-rib:nh-delete";
          return false;
        }
      }
      *output_xml = BooleanOutput(true); break;
    case NexthopRegistry::RemoveResult::kInUse:
      *output_xml = BooleanOutput(false, "the nexthop is referenced by a route"); break;
    case NexthopRegistry::RemoveResult::kMissing:
      *output_xml = BooleanOutput(false,
          "the nexthop identifier does not exist in this RIB"); break;
  }
  return true;
}

}  // namespace dang::rib

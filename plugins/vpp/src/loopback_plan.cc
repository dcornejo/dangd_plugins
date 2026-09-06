// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/** @file Parsing and ordered native-operation planning for VPP loopbacks. */

#include "plugins/vpp/src/loopback_plan.h"

#include <charconv>
#include <cstring>
#include <memory>
#include <string_view>

#include <libxml/parser.h>
#include <libxml/tree.h>

namespace dang::vpp {
namespace {
constexpr std::string_view kNamespace = "urn:dang:vpp:interfaces";

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
  std::string result(reinterpret_cast<const char*>(raw));
  xmlFree(raw);
  return result;
}

bool Fail(const std::string& message, const std::string& path,
          std::string* error, std::string* error_path) {
  *error = message;
  *error_path = path;
  return false;
}

std::string Path(uint32_t instance) {
  return "/dang-vpp-interfaces:vpp-interfaces/loopback[instance='" +
         std::to_string(instance) + "']";
}

}  // namespace

bool ParseLoopbackConfiguration(const char* xml,
                                LoopbackConfigurationMap* configuration,
                                std::string* error,
                                std::string* error_path) {
  if (!xml || !configuration || !error || !error_path) return false;
  error->clear();
  error_path->clear();
  xmlDocPtr raw = xmlReadMemory(xml, static_cast<int>(std::strlen(xml)),
      "vpp-interfaces.xml", nullptr, XML_PARSE_NONET | XML_PARSE_NOBLANKS |
      XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
  std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> document(raw, xmlFreeDoc);
  if (!document)
    return Fail("cannot parse VPP interface configuration",
                "/dang-vpp-interfaces:vpp-interfaces", error, error_path);
  configuration->clear();
  xmlNodePtr root = xmlDocGetRootElement(document.get());
  xmlNodePtr interfaces = Is(root, "vpp-interfaces")
      ? root : Child(root, "vpp-interfaces");
  if (!interfaces) return true;
  for (xmlNodePtr node = interfaces->children; node; node = node->next) {
    if (!Is(node, "loopback")) continue;
    const std::string instance_text = Text(Child(node, "instance"));
    uint32_t instance = 0;
    const auto parsed = std::from_chars(instance_text.data(),
                                        instance_text.data() + instance_text.size(),
                                        instance);
    if (instance_text.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != instance_text.data() + instance_text.size())
      return Fail("VPP loopback instance is invalid",
                  "/dang-vpp-interfaces:vpp-interfaces/loopback/instance",
                  error, error_path);
    const std::string enabled_text = Text(Child(node, "enabled"));
    if (!enabled_text.empty() && enabled_text != "true" &&
        enabled_text != "false")
      return Fail("VPP loopback enabled value is invalid", Path(instance),
                  error, error_path);
    const LoopbackConfiguration value{
        .instance = instance, .enabled = enabled_text == "true"};
    if (!configuration->emplace(instance, value).second)
      return Fail("VPP loopback instance is duplicated", Path(instance),
                  error, error_path);
  }
  return true;
}

std::vector<LoopbackOperation> PlanLoopbackChanges(
    const LoopbackConfigurationMap& before,
    const LoopbackConfigurationMap& proposed) {
  std::vector<LoopbackOperation> result;
  // Deactivate before deletion so traffic stops before the object disappears.
  for (const auto& [instance, old_value] : before) {
    if (proposed.contains(instance)) continue;
    if (old_value.enabled)
      result.push_back({LoopbackOperationKind::kSetAdminState, instance, false,
                        Path(instance)});
    result.push_back({LoopbackOperationKind::kDelete, instance, false,
                      Path(instance)});
  }
  // Create all new objects before any activation operation is emitted.
  for (const auto& [instance, new_value] : proposed) {
    if (!before.contains(instance))
      result.push_back({LoopbackOperationKind::kCreate, instance, false,
                        Path(instance)});
  }
  for (const auto& [instance, new_value] : proposed) {
    const auto old = before.find(instance);
    if ((old == before.end() && new_value.enabled) ||
        (old != before.end() && old->second.enabled != new_value.enabled))
      result.push_back({LoopbackOperationKind::kSetAdminState, instance,
                        new_value.enabled, Path(instance)});
  }
  return result;
}

}  // namespace dang::vpp

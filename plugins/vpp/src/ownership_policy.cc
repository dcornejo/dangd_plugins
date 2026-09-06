// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/** @file Schema-adjacent parsing and live safety validation for VPP claims. */

#include "plugins/vpp/src/ownership_policy.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <set>
#include <string_view>

#include <libxml/parser.h>
#include <libxml/tree.h>

namespace dang::vpp {
namespace {
constexpr std::string_view kNamespace = "urn:dang:vpp:interface-ownership";

bool Is(xmlNodePtr node, std::string_view name) {
  return node && node->type == XML_ELEMENT_NODE && node->ns && node->ns->href &&
      name == reinterpret_cast<const char*>(node->name) &&
      kNamespace == reinterpret_cast<const char*>(node->ns->href);
}
xmlNodePtr Child(xmlNodePtr parent, std::string_view name) {
  for (xmlNodePtr node = parent ? parent->children : nullptr; node; node = node->next)
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
std::string Lower(std::string value) {
  std::ranges::transform(value, value.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return value;
}
bool Fail(std::string text, std::string path, std::string* error,
          std::string* error_path) {
  *error = std::move(text); *error_path = std::move(path); return false;
}
}  // namespace

bool ParseOwnershipPolicy(const char* xml, std::vector<DevicePolicy>* policy,
                          std::string* error, std::string* error_path) {
  if (!xml || !policy || !error || !error_path) return false;
  xmlDocPtr raw = xmlReadMemory(xml, static_cast<int>(std::strlen(xml)),
      "vpp-ownership.xml", nullptr, XML_PARSE_NONET | XML_PARSE_NOBLANKS |
      XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
  std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> document(raw, xmlFreeDoc);
  if (!document) return Fail("cannot parse ownership configuration",
      "/dang-vpp-interface-ownership:vpp-interface-ownership", error, error_path);
  policy->clear();
  xmlNodePtr root = xmlDocGetRootElement(document.get());
  xmlNodePtr ownership = Is(root, "vpp-interface-ownership") ? root :
      Child(root, "vpp-interface-ownership");
  if (!ownership) return true;
  std::set<std::string> keys;
  for (xmlNodePtr node = ownership->children; node; node = node->next) {
    if (!Is(node, "device")) continue;
    DevicePolicy value;
    value.pci_address = Lower(Text(Child(node, "pci-address")));
    value.expected_mac_address = Lower(Text(Child(node, "expected-mac-address")));
    value.expected_vendor_device = Lower(Text(Child(node, "expected-vendor-device")));
    const std::string owner = Text(Child(node, "owner"));
    value.vpp_owner = owner == "vpp";
    if (value.pci_address.empty() || value.expected_mac_address.empty() ||
        value.expected_vendor_device.empty() ||
        (!owner.empty() && owner != "host" && owner != "vpp") ||
        !keys.insert(value.pci_address).second)
      return Fail("ownership device identity is incomplete, invalid, or duplicated",
          "/dang-vpp-interface-ownership:vpp-interface-ownership/device", error,
          error_path);
    policy->push_back(std::move(value));
  }
  return true;
}

bool ValidateOwnershipPolicy(const std::vector<DevicePolicy>& policy,
                             const std::vector<InterfaceEvidence>& inventory,
                             std::string* error, std::string* error_path) {
  if (!error || !error_path) return false;
  for (const DevicePolicy& request : policy) {
    if (!request.vpp_owner) continue;
    const auto found = std::ranges::find(inventory, request.pci_address,
        &InterfaceEvidence::pci_address);
    const std::string path =
        "/dang-vpp-interface-ownership:vpp-interface-ownership/device[pci-address='" +
        request.pci_address + "']";
    if (found == inventory.end())
      return Fail("allowlisted PCI function is not a Linux network interface", path,
                  error, error_path);
    if (Lower(found->mac_address) != request.expected_mac_address ||
        Lower(found->vendor_device) != request.expected_vendor_device)
      return Fail("live hardware identity does not match the allowlist", path,
                  error, error_path);
    if (found->carries_management_session || found->carries_default_route)
      return Fail("management-path interfaces cannot be transferred to VPP", path,
                  error, error_path);
    if (!found->eligible())
      return Fail("interface eligibility cannot be established safely", path,
                  error, error_path);
  }
  return true;
}

}  // namespace dang::vpp

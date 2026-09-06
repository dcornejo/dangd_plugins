// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "plugins/ip_management/src/platform_config.h"

#include <charconv>
#include <set>

#include <pugixml.hpp>

namespace dangd::ip_management {
namespace {

std::string_view LocalName(std::string_view name) {
  const auto colon = name.find(':');
  return colon == std::string_view::npos ? name : name.substr(colon + 1);
}

pugi::xml_node Child(const pugi::xml_node parent, std::string_view name) {
  for (const pugi::xml_node child : parent.children())
    if (LocalName(child.name()) == name) return child;
  return {};
}

bool ReadUnsigned(const pugi::xml_node node, unsigned* result) {
  if (!node || !result) return false;
  const std::string text = node.text().as_string();
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), *result);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

void ReadFamily(const pugi::xml_node family, bool ipv6,
                InterfaceConfig* result, bool* valid) {
  if (!family || !result || !valid) return;
  unsigned mtu = 0;
  if (const pugi::xml_node mtu_node = Child(family, "mtu")) {
    if (!ReadUnsigned(mtu_node, &mtu)) {
      *valid = false;
    } else if (ipv6) {
      result->ipv6_mtu = mtu;
    } else {
      result->ipv4_mtu = mtu;
    }
  }
  for (const pugi::xml_node entry : family.children()) {
    const std::string_view name = LocalName(entry.name());
    if (name == "address") {
      const pugi::xml_node ip = Child(entry, "ip");
      unsigned length = 0;
      if (ip && ReadUnsigned(Child(entry, "prefix-length"), &length)) {
        result->addresses.push_back(
            {ip.text().as_string(), length, ipv6});
      } else {
        *valid = false;
      }
    } else if (name == "neighbor") {
      const pugi::xml_node ip = Child(entry, "ip");
      const pugi::xml_node link_layer = Child(entry, "link-layer-address");
      if (ip && link_layer) {
        result->neighbors.push_back(
            {ip.text().as_string(), link_layer.text().as_string(), ipv6});
      } else {
        *valid = false;
      }
    }
  }
}

void ReadInterfaces(const pugi::xml_node parent,
                    std::vector<InterfaceConfig>* interfaces, bool* valid) {
  for (const pugi::xml_node root : parent.children()) {
    if (LocalName(root.name()) != "interfaces") {
      ReadInterfaces(root, interfaces, valid);
      continue;
    }
    for (const pugi::xml_node node : root.children()) {
      if (LocalName(node.name()) != "interface") continue;
      const pugi::xml_node name = Child(node, "name");
      if (!name) {
        *valid = false;
        continue;
      }
      InterfaceConfig interface;
      interface.name = name.text().as_string();
      if (const pugi::xml_node enabled = Child(node, "enabled"))
        interface.enabled = std::string_view(enabled.text().as_string()) == "true";
      ReadFamily(Child(node, "ipv4"), false, &interface, valid);
      ReadFamily(Child(node, "ipv6"), true, &interface, valid);
      interfaces->push_back(std::move(interface));
    }
  }
}

}  // namespace

bool ParsePlatformConfig(std::string_view xml,
                         std::vector<InterfaceConfig>* interfaces,
                         std::string* error) {
  if (!interfaces) return false;
  interfaces->clear();
  pugi::xml_document document;
  const auto parsed = document.load_buffer(xml.data(), xml.size());
  if (!parsed) {
    if (error) *error = std::string("cannot parse configuration: ") + parsed.description();
    return false;
  }
  bool valid = true;
  ReadInterfaces(document, interfaces, &valid);
  if (!valid) {
    if (error) *error = "configuration contains incomplete or invalid IP data";
    interfaces->clear();
    return false;
  }
  std::set<std::string> interface_names;
  for (const InterfaceConfig& interface : *interfaces) {
    if (interface.name.empty() || interface.name.front() == '-') {
      if (error) *error = "interface name cannot be empty or begin with '-'";
      interfaces->clear();
      return false;
    }
    if (!interface_names.insert(interface.name).second) {
      if (error) *error = "interface names must be unique";
      interfaces->clear();
      return false;
    }
    if ((interface.ipv4_mtu &&
         (*interface.ipv4_mtu < 68 || *interface.ipv4_mtu > 65535)) ||
        (interface.ipv6_mtu && *interface.ipv6_mtu < 1280)) {
      if (error) *error = "interface MTU is outside the RFC 8344 range";
      interfaces->clear();
      return false;
    }
    std::set<std::pair<bool, std::string>> addresses;
    for (const AddressConfig& address : interface.addresses) {
      const unsigned maximum = address.ipv6 ? 128u : 32u;
      if (address.address.empty() || address.prefix_length > maximum ||
          !addresses.emplace(address.ipv6, address.address).second) {
        if (error) *error = "invalid or duplicate interface address";
        interfaces->clear();
        return false;
      }
    }
    std::set<std::pair<bool, std::string>> neighbors;
    for (const NeighborConfig& neighbor : interface.neighbors) {
      if (neighbor.address.empty() || neighbor.link_layer_address.empty() ||
          !neighbors.emplace(neighbor.ipv6, neighbor.address).second) {
        if (error) *error = "invalid or duplicate interface neighbor";
        interfaces->clear();
        return false;
      }
    }
  }
  return true;
}

}  // namespace dangd::ip_management

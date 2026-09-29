// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#pragma once

/**
 * @file
 * Portable Kea process-instance identity validation and operational XML.
 * Keeping this logic independent of the plugin callback makes the externally
 * visible instance contract testable without a running Kea service.
 */

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace dang::plugins::kea {

/** Returns whether an identifier is safe for services, paths, and XML text. */
inline bool ValidInstanceId(std::string_view value) {
  const auto alphanumeric = [](char character) {
    return (character >= 'a' && character <= 'z') ||
        (character >= 'A' && character <= 'Z') ||
        (character >= '0' && character <= '9');
  };
  if (value.empty() || value.size() > 64 || !alphanumeric(value.front()))
    return false;
  return std::all_of(value.begin() + 1, value.end(), [&](char character) {
    return alphanumeric(character) || character == '_' || character == '-' ||
        character == '.';
  });
}

/**
 * Builds the read-only identity tree for one independently persisted process.
 *
 * Each family flag is false for DHCPv4 and true for DHCPv6. Callers preserve
 * their process-stable target order, which is IPv4 before IPv6 in the plugin.
 * The identifier has already passed ValidInstanceId(), so it cannot introduce
 * markup and does not need general-purpose XML escaping.
 */
inline std::string BuildInstanceOperationalXml(
    std::string_view instance_id, const std::vector<bool>& dhcp6_families,
    const std::vector<std::string>& versions = {}) {
  std::string xml =
      "<kea-instance xmlns=\"urn:dang:kea:instance\"><instance-id>";
  xml += instance_id;
  xml += "</instance-id>";
  for (bool dhcp6 : dhcp6_families) {
    xml += "<address-family>";
    xml += dhcp6 ? "dhcpv6" : "dhcpv4";
    xml += "</address-family>";
  }
  if (versions.size() == dhcp6_families.size()) {
    for (std::size_t index = 0; index < versions.size(); ++index) {
      if (versions[index].empty()) continue;
      xml += "<daemon><address-family>";
      xml += dhcp6_families[index] ? "dhcpv6" : "dhcpv4";
      xml += "</address-family><version>";
      // ReadDaemonVersion accepts digits and dots only, so version text cannot
      // introduce markup into this generated operational fragment.
      xml += versions[index];
      xml += "</version></daemon>";
    }
  }
  xml += "</kea-instance>";
  return xml;
}

}  // namespace dang::plugins::kea

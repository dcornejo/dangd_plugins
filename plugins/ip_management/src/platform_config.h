// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANGD_PLUGINS_IP_MANAGEMENT_PLATFORM_CONFIG_H_
#define DANGD_PLUGINS_IP_MANAGEMENT_PLATFORM_CONFIG_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dangd::ip_management {

struct AddressConfig {
  /** Canonical textual address without a zone suffix. */
  std::string address;
  /** CIDR prefix width, bounded to 32 or 128 by the parser. */
  unsigned prefix_length = 0;
  /** True for IPv6 and false for IPv4. */
  bool ipv6 = false;
  auto operator<=>(const AddressConfig&) const = default;
};

/** One configured IPv4 ARP or IPv6 neighbor-cache entry. */
struct NeighborConfig {
  /** IPv4 or IPv6 neighbor address without a zone suffix. */
  std::string address;
  /** Model-supplied link-layer address for a permanent neighbor entry. */
  std::string link_layer_address;
  /** True for an IPv6 neighbor and false for an IPv4 ARP entry. */
  bool ipv6 = false;
  auto operator<=>(const NeighborConfig&) const = default;
};

struct InterfaceConfig {
  /** Kernel interface name used only after native API resolution. */
  std::string name;
  /** Requested administrative state, or absent when it is not configured. */
  std::optional<bool> enabled;
  /** Requested IPv4 packet MTU as defined by RFC 8344. */
  std::optional<unsigned> ipv4_mtu;
  /** Requested IPv6 packet MTU as defined by RFC 8344. */
  std::optional<unsigned> ipv6_mtu;
  /** Explicit IPv4 and IPv6 addresses owned by the configuration. */
  std::vector<AddressConfig> addresses;
  /** Explicit permanent ARP and neighbor-discovery cache entries. */
  std::vector<NeighborConfig> neighbors;
};

// Extracts the subset of RFC 8343/8344 configuration implemented by the native
// examples. Unknown model nodes remain the responsibility of other plugins.
bool ParsePlatformConfig(std::string_view xml,
                         std::vector<InterfaceConfig>* interfaces,
                         std::string* error);

}  // namespace dangd::ip_management

#endif  // DANGD_PLUGINS_IP_MANAGEMENT_PLATFORM_CONFIG_H_

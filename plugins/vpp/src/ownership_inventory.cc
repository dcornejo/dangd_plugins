// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/** @file Read-only Linux management-path and PCI ownership discovery. */

#include "plugins/vpp/src/ownership_inventory.h"

#include <ifaddrs.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace dang::vpp {
namespace {
std::string ReadLine(const std::filesystem::path& path) {
  std::ifstream input(path);
  std::string value;
  std::getline(input, value);
  return value;
}

std::string PciAddress(std::filesystem::path path) {
  std::error_code error;
  path = std::filesystem::canonical(path, error);
  for (; !error && !path.empty(); path = path.parent_path()) {
    const std::string name = path.filename().string();
    if (name.size() == 12 && name[4] == ':' && name[7] == ':' && name[10] == '.')
      return name;
    if (path == path.root_path()) break;
  }
  return {};
}

std::set<std::string> DefaultRouteInterfaces() {
  std::set<std::string> result;
  std::ifstream ipv4("/proc/net/route");
  std::string line;
  std::getline(ipv4, line);
  while (std::getline(ipv4, line)) {
    std::istringstream fields(line);
    std::string interface, destination;
    fields >> interface >> destination;
    if (destination == "00000000") result.insert(interface);
  }
  std::ifstream ipv6("/proc/net/ipv6_route");
  while (std::getline(ipv6, line)) {
    std::istringstream fields(line);
    std::string destination, prefix, source, source_prefix, gateway, metric,
        reference_count, use, flags, interface;
    fields >> destination >> prefix >> source >> source_prefix >> gateway >>
        metric >> reference_count >> use >> flags >> interface;
    // Linux includes an unreachable ::/0 sentinel on loopback with the maximum
    // metric. It is not a forwarding default route and must not be mistaken for
    // management-path evidence.
    if (destination == std::string(32, '0') && prefix == "00" &&
        metric != "ffffffff" && !interface.empty())
      result.insert(interface);
  }
  return result;
}

std::string ManagementLocalAddress() {
  const char* connection = std::getenv("SSH_CONNECTION");
  if (!connection) return {};
  std::istringstream fields(connection);
  std::string remote, remote_port, local;
  fields >> remote >> remote_port >> local;
  return local;
}
}  // namespace

bool DiscoverInterfaces(std::vector<InterfaceEvidence>* interfaces,
                        std::string* error) {
  if (!interfaces || !error) return false;
  interfaces->clear();
  const auto defaults = DefaultRouteInterfaces();
  const std::string management = ManagementLocalAddress();
  std::set<std::string> management_interfaces;
  ifaddrs* raw = nullptr;
  if (getifaddrs(&raw) != 0) { *error = "cannot enumerate interface addresses"; return false; }
  for (const ifaddrs* address = raw; address; address = address->ifa_next) {
    if (!address->ifa_addr || management.empty()) continue;
    char text[INET6_ADDRSTRLEN]{};
    const void* source = address->ifa_addr->sa_family == AF_INET
        ? static_cast<const void*>(&reinterpret_cast<const sockaddr_in*>(address->ifa_addr)->sin_addr)
        : address->ifa_addr->sa_family == AF_INET6
            ? static_cast<const void*>(&reinterpret_cast<const sockaddr_in6*>(address->ifa_addr)->sin6_addr)
            : nullptr;
    if (source && inet_ntop(address->ifa_addr->sa_family, source, text, sizeof(text)) && management == text)
      management_interfaces.insert(address->ifa_name);
  }
  freeifaddrs(raw);
  struct if_nameindex* names = if_nameindex();
  if (!names) { *error = "cannot enumerate interface names"; return false; }
  for (struct if_nameindex* item = names; item->if_index; ++item) {
    const std::filesystem::path base = std::filesystem::path("/sys/class/net") / item->if_name;
    InterfaceEvidence value;
    value.name = item->if_name;
    value.pci_address = PciAddress(base / "device");
    value.mac_address = ReadLine(base / "address");
    std::error_code driver_error;
    const auto driver = std::filesystem::canonical(base / "device/driver", driver_error);
    if (!driver_error) value.driver = driver.filename().string();
    value.carries_default_route = defaults.contains(value.name);
    value.carries_management_session = management_interfaces.contains(value.name);
    interfaces->push_back(std::move(value));
  }
  if_freenameindex(names);
  return true;
}

}  // namespace dang::vpp

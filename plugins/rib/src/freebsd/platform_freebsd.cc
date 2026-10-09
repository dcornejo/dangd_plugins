// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/** @file FreeBSD validation, route(8) test mapping, and route observation. */

#include "plugins/rib/src/platform_command.h"
#include "plugins/rib/src/route_observer.h"

#include <algorithm>
#include <charconv>
#include <limits>

#if defined(__FreeBSD__)
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/sysctl.h>

#include <cerrno>
#include <cstring>
#endif

namespace dang::rib {
namespace {

bool ResolveInterfaceAddress(const std::string& interface,
                             const std::string& address_family,
                             std::string* address, std::string* error) {
#if !defined(__FreeBSD__)
  (void)interface;
  (void)address_family;
  (void)address;
  *error = "native FreeBSD interface address resolution is unavailable";
  return false;
#else
  ifaddrs* raw = nullptr;
  if (getifaddrs(&raw) != 0) {
    *error = "cannot enumerate interface addresses: " +
             std::string(std::strerror(errno));
    return false;
  }
  std::vector<std::string> candidates;
  const int family = address_family == "ipv4" ? AF_INET : AF_INET6;
  for (const ifaddrs* item = raw; item; item = item->ifa_next) {
    if (!item->ifa_name || interface != item->ifa_name || !item->ifa_addr ||
        item->ifa_addr->sa_family != family)
      continue;
    const void* bytes = nullptr;
    if (family == AF_INET) {
      const auto* value = reinterpret_cast<const sockaddr_in*>(item->ifa_addr);
      if (value->sin_addr.s_addr == INADDR_ANY) continue;
      bytes = &value->sin_addr;
    } else {
      const auto* value = reinterpret_cast<const sockaddr_in6*>(item->ifa_addr);
      if (IN6_IS_ADDR_UNSPECIFIED(&value->sin6_addr) ||
          IN6_IS_ADDR_MULTICAST(&value->sin6_addr) ||
          IN6_IS_ADDR_LINKLOCAL(&value->sin6_addr))
        continue;
      bytes = &value->sin6_addr;
    }
    char text[INET6_ADDRSTRLEN]{};
    if (inet_ntop(family, bytes, text, sizeof(text)))
      candidates.emplace_back(text);
  }
  freeifaddrs(raw);
  std::sort(candidates.begin(), candidates.end());
  candidates.erase(std::unique(candidates.begin(), candidates.end()),
                   candidates.end());
  if (candidates.size() != 1U) {
    *error = candidates.empty()
                 ? "outgoing interface has no usable local " + address_family +
                       " address"
                 : "outgoing interface has multiple usable local " +
                       address_family + " addresses";
    return false;
  }
  *address = candidates.front();
  return true;
#endif
}

bool SafeFibAndInterface(const Route& route, std::string* error,
                         std::string* path) {
  if (!route.load_balance.empty() &&
      (route.gateway || route.interface || route.nexthop_ref || route.special)) {
    *error = "FreeBSD load-balance route cannot include a base nexthop";
    *path = "/ietf-i2rs-rib:routing-instance/rib-list/route-list/nexthop";
    return false;
  }
  for (const WeightedNexthop& member : route.load_balance) {
    if ((!member.gateway && !member.interface) || member.weight < 1U ||
        member.weight > 99U) {
      *error =
          "FreeBSD load-balance member is incomplete or has invalid weight";
      *path = "/ietf-i2rs-rib:routing-instance/rib-list/route-list/nexthop/"
              "nexthop-lb/nexthop-list";
      return false;
    }
    if (member.interface &&
        (member.interface->empty() || member.interface->front() == '-')) {
      *error = "unsafe load-balance outgoing interface name";
      *path = "/ietf-i2rs-rib:routing-instance/rib-list/route-list/nexthop/"
              "nexthop-lb/nexthop-list/nexthop-member-id";
      return false;
    }
  }
  if (route.local_only) {
    *error = "FreeBSD cannot safely map a configured RFC 8431 local-only route";
    *path = "/ietf-i2rs-rib:routing-instance/rib-list/route-list/"
            "route-attributes/local-only";
    return false;
  }
  unsigned fib = 0;
  const auto parsed = std::from_chars(route.rib.data(),
                                      route.rib.data() + route.rib.size(), fib);
  if (parsed.ec != std::errc{} || parsed.ptr != route.rib.data() + route.rib.size()) {
    *error = "FreeBSD backend currently requires a numeric RIB name (FIB number)";
    *path = "/ietf-i2rs-rib:routing-instance/rib-list/name";
    return false;
  }
  if (route.interface && (route.interface->empty() || route.interface->front() == '-')) {
    *error = "unsafe outgoing interface name";
    *path = "/ietf-i2rs-rib:routing-instance/rib-list/route-list/nexthop";
    return false;
  }
  if (route.special && *route.special != "discard" &&
      *route.special != "discard-with-error") {
    *error = "FreeBSD route has an unsupported special nexthop";
    *path = "/ietf-i2rs-rib:routing-instance/rib-list/route-list/nexthop/"
            "nexthop-base/special";
    return false;
  }
  if (route.special && (route.gateway || route.interface)) {
    *error = "FreeBSD special route cannot include another nexthop";
    *path = "/ietf-i2rs-rib:routing-instance/rib-list/route-list/nexthop/"
            "nexthop-base";
    return false;
  }
  return true;
}

}  // namespace

bool BuildFreeBsdCommands(const std::vector<Change>& changes,
                          std::vector<NativeCommand>* commands,
                          std::string* error, std::string* error_path,
                          const InterfaceAddressResolver& supplied_resolver) {
  if (!commands || !error || !error_path) return false;
  commands->clear();
  for (const Change& change : changes) {
    if (!SafeFibAndInterface(change.route, error, error_path)) return false;
    if (!change.route.load_balance.empty()) {
      *error = "FreeBSD route(8) test adapter cannot express one atomic "
               "load-balance change";
      *error_path =
          "/ietf-i2rs-rib:routing-instance/rib-list/route-list/nexthop/"
          "nexthop-lb";
      commands->clear();
      return false;
    }
    NativeCommand command;
    command.arguments = {"route", "-n",
                         change.kind == ChangeKind::kDelete ? "delete" : "add",
                         change.route.address_family == "ipv4" ? "-inet" : "-inet6",
                         "-fib", change.route.rib, change.route.destination};
    if (change.route.special)
      command.arguments.push_back(*change.route.special == "discard"
                                      ? "-blackhole"
                                      : "-reject");
    if (change.route.gateway) {
      command.arguments.push_back(*change.route.gateway);
      if (change.route.interface)
        command.arguments.insert(command.arguments.end(),
                                 {"-ifp", *change.route.interface});
    } else if (change.route.interface) {
      // route(8) represents an Ethernet interface route using one of the
      // interface's local addresses as its gateway argument. Never guess on a
      // multihomed interface: require exactly one usable address in the route
      // family, excluding automatic IPv6 link-local addresses.
      std::string local_address;
      const auto& resolver = supplied_resolver
                                 ? supplied_resolver
                                 : InterfaceAddressResolver(
                                       ResolveInterfaceAddress);
      if (!resolver(*change.route.interface, change.route.address_family,
                    &local_address, error)) {
        *error_path =
            "/ietf-i2rs-rib:routing-instance/rib-list/route-list/nexthop";
        commands->clear();
        return false;
      }
      command.arguments.push_back(local_address);
      command.arguments.insert(command.arguments.end(),
                               {"-ifp", *change.route.interface});
    }
    commands->push_back(std::move(command));
  }
  return true;
}

bool ValidateFreeBsdChanges(const std::vector<Change>& changes,
                            std::string* error, std::string* error_path) {
  if (!error || !error_path) return false;
  for (const Change& change : changes) {
    if (!SafeFibAndInterface(change.route, error, error_path)) return false;
  }
  return true;
}

bool ObserveFreeBsdRoutes(std::vector<ObservedRoute>* routes,
                          std::string* error) {
  return ObserveFreeBsdRoutesForFib(0, routes, error);
}

bool ObserveFreeBsdRoutesForFib(std::uint32_t fib,
                                std::vector<ObservedRoute>* routes,
                                std::string* error) {
#if !defined(__FreeBSD__)
  (void)fib;
  (void)routes;
  if (error) *error = "FreeBSD route observation is unavailable on this host";
  return false;
#else
  if (!routes || !error) return false;
  routes->clear();
  if (fib > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
    *error = "FreeBSD FIB number exceeds the routing sysctl range";
    return false;
  }
  int mib[] = {CTL_NET, PF_ROUTE, 0, AF_UNSPEC, NET_RT_DUMP,
               static_cast<int>(fib)};
  size_t length = 0;
  if (sysctl(mib, 6, nullptr, &length, nullptr, 0) < 0) {
    *error = std::strerror(errno); return false;
  }
  std::vector<char> buffer(length);
  if (length && sysctl(mib, 6, buffer.data(), &length, nullptr, 0) < 0) {
    *error = std::strerror(errno); return false;
  }
  const auto aligned = [](const sockaddr* address) {
    return address->sa_len ? 1U + ((address->sa_len - 1U) | (sizeof(long) - 1U))
                           : sizeof(long);
  };
  for (char* cursor = buffer.data(); cursor < buffer.data() + length;) {
    const auto* message = reinterpret_cast<const rt_msghdr*>(cursor);
    if (message->rtm_msglen == 0) break;
    cursor += message->rtm_msglen;
    if (message->rtm_version != RTM_VERSION || !(message->rtm_flags & RTF_UP) ||
        (message->rtm_flags & RTF_LLINFO)) continue;
    const sockaddr* addresses[RTAX_MAX]{};
    const char* address_cursor = reinterpret_cast<const char*>(message + 1);
    for (int index = 0; index < RTAX_MAX; ++index) {
      if (!(message->rtm_addrs & (1 << index))) continue;
      addresses[index] = reinterpret_cast<const sockaddr*>(address_cursor);
      address_cursor += aligned(addresses[index]);
    }
    const sockaddr* destination = addresses[RTAX_DST];
    if (!destination || (destination->sa_family != AF_INET && destination->sa_family != AF_INET6))
      continue;
    const bool ipv4 = destination->sa_family == AF_INET;
    const unsigned bits = ipv4 ? 32U : 128U;
    unsigned prefix = (message->rtm_flags & RTF_HOST) ? bits : 0U;
    if (!(message->rtm_flags & RTF_HOST) && addresses[RTAX_NETMASK]) {
      const auto* bytes = reinterpret_cast<const unsigned char*>(addresses[RTAX_NETMASK]) + 2;
      const size_t byte_count = addresses[RTAX_NETMASK]->sa_len > 2
                                    ? addresses[RTAX_NETMASK]->sa_len - 2U : 0U;
      for (size_t i = 0; i < byte_count; ++i)
        prefix += static_cast<unsigned>(__builtin_popcount(bytes[i]));
    }
    const void* destination_bytes = ipv4
        ? static_cast<const void*>(&reinterpret_cast<const sockaddr_in*>(destination)->sin_addr)
        : static_cast<const void*>(&reinterpret_cast<const sockaddr_in6*>(destination)->sin6_addr);
    char text[INET6_ADDRSTRLEN]{};
    if (!inet_ntop(destination->sa_family, destination_bytes, text, sizeof(text))) continue;
    ObservedRoute observed;
    Route& route = observed.route;
    route.routing_instance = "default";
    route.rib = std::to_string(fib);
    route.address_family = ipv4 ? "ipv4" : "ipv6";
    route.destination = std::string(text) + "/" + std::to_string(prefix);
    // FreeBSD keeps the route metric and ECMP path weight as distinct native
    // values. NL_RTA_PRIORITY round-trips through rmx_metric; rmx_weight must
    // not be exposed as RFC 8431 route-preference.
    if (message->rtm_rmx.rmx_metric >
        std::numeric_limits<std::uint32_t>::max())
      continue;
    route.preference =
        static_cast<std::uint32_t>(message->rtm_rmx.rmx_metric);
    if (message->rtm_rmx.rmx_weight >
        std::numeric_limits<std::uint32_t>::max())
      continue;
    // rmx_weight is independent of route preference and identifies this
    // precise path's share when FreeBSD returns an ECMP member.
    observed.weight =
        static_cast<std::uint32_t>(message->rtm_rmx.rmx_weight);
#if defined(RTF_LOCAL)
    // RTF_LOCAL is set for destinations owned by the host.  Do not infer this
    // from RTF_HOST: a host route may still point at a remote peer.
    route.local_only = (message->rtm_flags & RTF_LOCAL) != 0;
    if (route.local_only) route.special = "receive";
#endif
#if defined(RTF_BLACKHOLE)
    if (message->rtm_flags & RTF_BLACKHOLE) route.special = "discard";
#endif
#if defined(RTF_REJECT)
    if (message->rtm_flags & RTF_REJECT)
      route.special = "discard-with-error";
#endif
    // Local receive routes remain kernel-owned. Blackhole and reject routes
    // have exact reversible route-netlink encodings.
    observed.mutable_route = !route.special || *route.special != "receive";
    if (message->rtm_index) {
      char interface_name[IF_NAMESIZE]{};
      if (if_indextoname(message->rtm_index, interface_name)) route.interface = interface_name;
    }
    const sockaddr* gateway = addresses[RTAX_GATEWAY];
    if (gateway && gateway->sa_family == destination->sa_family) {
      const void* gateway_bytes = ipv4
          ? static_cast<const void*>(&reinterpret_cast<const sockaddr_in*>(gateway)->sin_addr)
          : static_cast<const void*>(&reinterpret_cast<const sockaddr_in6*>(gateway)->sin6_addr);
      if (inet_ntop(gateway->sa_family, gateway_bytes, text, sizeof(text))) route.gateway = text;
    }
    if (route.special) {
      route.gateway.reset();
      route.interface.reset();
    }
    const std::string key = route.rib + "|" + route.address_family + "|" +
                            route.destination + "|" + route.gateway.value_or("") +
                            "|" + route.interface.value_or("") + "|" +
                            route.special.value_or("");
    std::uint64_t hash = 1469598103934665603ULL;
    for (const char byte : key) {
      hash ^= static_cast<unsigned char>(byte);
      hash *= 1099511628211ULL;
    }
    route.index = hash;
    routes->push_back(std::move(observed));
  }
  return true;
#endif
}

}  // namespace dang::rib

// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/** @file Linux iproute2 mapping for the portable RFC 8431 route plan. */

#include "plugins/rib/src/platform_command.h"
#include "plugins/rib/src/route_observer.h"

#include <charconv>

#if defined(__linux__)
#include <arpa/inet.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <set>
#endif

namespace dang::rib {
namespace {

#if defined(__linux__)
/** One path nested in Linux's RTA_MULTIPATH route attribute. */
struct LinuxNexthop {
  std::optional<std::string> gateway;
  unsigned interface_index = 0;
  bool installed = true;
};
#endif

bool SafeTableAndInterface(const Route& route, std::string* error,
                           std::string* path) {
  unsigned table = 0;
  const auto parsed = std::from_chars(route.rib.data(),
                                      route.rib.data() + route.rib.size(), table);
  if (parsed.ec != std::errc{} || parsed.ptr != route.rib.data() + route.rib.size() ||
      table == 0) {
    *error = "Linux backend currently requires a positive numeric RIB name";
    *path = "/ietf-i2rs-rib:routing-instance/rib-list/name";
    return false;
  }
  if (route.interface && (route.interface->empty() || route.interface->front() == '-')) {
    *error = "unsafe outgoing interface name";
    *path = "/ietf-i2rs-rib:routing-instance/rib-list/route-list/nexthop";
    return false;
  }
  return true;
}

}  // namespace

bool BuildLinuxCommands(const std::vector<Change>& changes,
                        std::vector<NativeCommand>* commands,
                        std::string* error, std::string* error_path) {
  if (!commands || !error || !error_path) return false;
  commands->clear();
  for (const Change& change : changes) {
    if (!SafeTableAndInterface(change.route, error, error_path)) return false;
    NativeCommand command;
    command.arguments = {"ip", change.route.address_family == "ipv4" ? "-4" : "-6",
                         "route",
                         change.kind == ChangeKind::kDelete ? "delete" : "replace",
                         change.route.destination, "table", change.route.rib};
    if (change.route.gateway)
      command.arguments.insert(command.arguments.end(), {"via", *change.route.gateway});
    if (change.route.interface)
      command.arguments.insert(command.arguments.end(), {"dev", *change.route.interface});
    if (change.kind == ChangeKind::kInstall)
      command.arguments.insert(command.arguments.end(),
                               {"metric", std::to_string(change.route.preference),
                                "proto", "static"});
    commands->push_back(std::move(command));
  }
  return true;
}

bool ObserveLinuxRoutes(std::vector<ObservedRoute>* routes,
                        std::string* error) {
#if !defined(__linux__)
  (void)routes;
  if (error) *error = "Linux route observation is unavailable on this host";
  return false;
#else
  if (!routes || !error) return false;
  routes->clear();
  const int socket_fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
  if (socket_fd < 0) { *error = std::strerror(errno); return false; }
  struct Request { nlmsghdr header; rtmsg route; } request{};
  request.header.nlmsg_len = NLMSG_LENGTH(sizeof(rtmsg));
  request.header.nlmsg_type = RTM_GETROUTE;
  request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
  request.header.nlmsg_seq = 1;
  request.route.rtm_family = AF_UNSPEC;
  if (send(socket_fd, &request, request.header.nlmsg_len, 0) < 0) {
    *error = std::strerror(errno); close(socket_fd); return false;
  }
  std::array<char, 32768> buffer{};
  bool done = false;
  while (!done) {
    const ssize_t received = recv(socket_fd, buffer.data(), buffer.size(), 0);
    if (received < 0) {
      if (errno == EINTR) continue;
      *error = std::strerror(errno); close(socket_fd); return false;
    }
    if (received == 0) { *error = "unexpected end of netlink dump"; close(socket_fd); return false; }
    int remaining = static_cast<int>(received);
    for (nlmsghdr* header = reinterpret_cast<nlmsghdr*>(buffer.data());
         NLMSG_OK(header, remaining); header = NLMSG_NEXT(header, remaining)) {
      if (header->nlmsg_type == NLMSG_DONE) { done = true; break; }
      if (header->nlmsg_type == NLMSG_ERROR) {
        const auto* failure = reinterpret_cast<const nlmsgerr*>(NLMSG_DATA(header));
        if (failure->error == 0) { done = true; break; }
        *error = std::strerror(-failure->error); close(socket_fd); return false;
      }
      if (header->nlmsg_type != RTM_NEWROUTE) continue;
      const auto* info = reinterpret_cast<const rtmsg*>(NLMSG_DATA(header));
      if (info->rtm_family != AF_INET && info->rtm_family != AF_INET6) continue;
      if (info->rtm_type != RTN_UNICAST && info->rtm_type != RTN_LOCAL &&
          info->rtm_type != RTN_BLACKHOLE &&
          info->rtm_type != RTN_UNREACHABLE &&
          info->rtm_type != RTN_PROHIBIT)
        continue;
      ObservedRoute observed;
      Route& route = observed.route;
      route.routing_instance = "default";
      route.address_family = info->rtm_family == AF_INET ? "ipv4" : "ipv6";
      route.rib = std::to_string(info->rtm_table);
      route.preference = 0;
      // RT_SCOPE_HOST identifies routes whose destinations are local to this
      // host (for example, addresses in Linux's local table).  It is the
      // kernel fact corresponding to RFC 8431 local-only; link scope does not
      // imply local-only because connected prefixes still forward off-host.
      route.local_only = info->rtm_scope == RT_SCOPE_HOST ||
                         info->rtm_type == RTN_LOCAL;
      if (info->rtm_type == RTN_LOCAL)
        route.special = "receive";
      else if (info->rtm_type == RTN_BLACKHOLE)
        route.special = "discard";
      else if (info->rtm_type == RTN_UNREACHABLE ||
               info->rtm_type == RTN_PROHIBIT)
        route.special = "discard-with-error";
      observed.mutable_route = info->rtm_type == RTN_UNICAST;
      std::array<unsigned char, 16> destination{};
      unsigned table = info->rtm_table;
      unsigned interface_index = 0;
      std::uint32_t metric = 0;
      std::vector<LinuxNexthop> multipath;
      int attributes_length = static_cast<int>(RTM_PAYLOAD(header));
      for (rtattr* attribute = RTM_RTA(info); RTA_OK(attribute, attributes_length);
           attribute = RTA_NEXT(attribute, attributes_length)) {
        if (attribute->rta_type == RTA_DST)
          std::memcpy(destination.data(), RTA_DATA(attribute),
                      info->rtm_family == AF_INET ? 4U : 16U);
        else if (attribute->rta_type == RTA_GATEWAY) {
          char address[INET6_ADDRSTRLEN]{};
          if (inet_ntop(info->rtm_family, RTA_DATA(attribute), address,
                        sizeof(address))) route.gateway = address;
        } else if (attribute->rta_type == RTA_OIF)
          std::memcpy(&interface_index, RTA_DATA(attribute), sizeof(interface_index));
        else if (attribute->rta_type == RTA_PRIORITY)
          std::memcpy(&metric, RTA_DATA(attribute), sizeof(metric));
        else if (attribute->rta_type == RTA_TABLE)
          std::memcpy(&table, RTA_DATA(attribute), sizeof(table));
        else if (attribute->rta_type == RTA_MULTIPATH) {
          int nexthops_length = static_cast<int>(RTA_PAYLOAD(attribute));
          for (rtnexthop* nexthop =
                   reinterpret_cast<rtnexthop*>(RTA_DATA(attribute));
               RTNH_OK(nexthop, nexthops_length);
               nexthops_length -= RTNH_ALIGN(nexthop->rtnh_len),
                          nexthop = RTNH_NEXT(nexthop)) {
            LinuxNexthop path;
            path.interface_index =
                static_cast<unsigned>(nexthop->rtnh_ifindex);
            path.installed = (nexthop->rtnh_flags & RTNH_F_DEAD) == 0;
            int nested_length = static_cast<int>(nexthop->rtnh_len) -
                                static_cast<int>(sizeof(*nexthop));
            for (rtattr* nested = RTNH_DATA(nexthop);
                 RTA_OK(nested, nested_length);
                 nested = RTA_NEXT(nested, nested_length)) {
              if (nested->rta_type != RTA_GATEWAY) continue;
              char gateway[INET6_ADDRSTRLEN]{};
              if (inet_ntop(info->rtm_family, RTA_DATA(nested), gateway,
                            sizeof(gateway)))
                path.gateway = gateway;
            }
            multipath.push_back(std::move(path));
          }
        }
      }
      char address[INET6_ADDRSTRLEN]{};
      if (!inet_ntop(info->rtm_family, destination.data(), address, sizeof(address))) continue;
      route.destination = std::string(address) + "/" + std::to_string(info->rtm_dst_len);
      route.rib = std::to_string(table);
      route.preference = metric;
      const auto resolve_interface_name =
          [](unsigned index) -> std::optional<std::string> {
        if (index == 0) return std::nullopt;
        char name[IF_NAMESIZE]{};
        if (!if_indextoname(index, name)) return std::nullopt;
        return name;
      };
      route.interface = resolve_interface_name(interface_index);
      if (route.special) {
        route.gateway.reset();
        route.interface.reset();
      }
      const auto append = [&](ObservedRoute value) {
        // FNV-1a gives a stable RFC 8431 list key without claiming that the
        // kernel supplies a native route identifier. Including each native
        // path keeps parallel ECMP members distinct.
        Route& candidate = value.route;
        const std::string key = candidate.rib + "|" + candidate.address_family +
            "|" + candidate.destination + "|" +
            candidate.gateway.value_or("") + "|" +
            candidate.interface.value_or("") + "|" +
            candidate.special.value_or("");
        std::uint64_t hash = 1469598103934665603ULL;
        for (const char byte : key) {
          hash ^= static_cast<unsigned char>(byte);
          hash *= 1099511628211ULL;
        }
        candidate.index = hash;
        routes->push_back(std::move(value));
      };
      if (!multipath.empty() && !route.special) {
        std::set<std::pair<std::optional<std::string>,
                           std::optional<std::string>>> emitted_paths;
        for (const LinuxNexthop& path : multipath) {
          ObservedRoute member = observed;
          member.installed = path.installed;
          member.route.gateway = path.gateway;
          member.route.interface = resolve_interface_name(path.interface_index);
          // A kernel nexthop object referenced only by ID cannot yet be
          // expanded into the RFC base-nexthop choice. Never emit an empty,
          // schema-invalid nexthop while that support remains absent.
          if (!member.route.gateway && !member.route.interface) continue;
          if (!emitted_paths.emplace(member.route.gateway,
                                     member.route.interface).second)
            continue;
          append(std::move(member));
        }
      } else if (route.special || route.gateway || route.interface) {
        append(std::move(observed));
      }
    }
  }
  close(socket_fd);
  return true;
#endif
}

}  // namespace dang::rib

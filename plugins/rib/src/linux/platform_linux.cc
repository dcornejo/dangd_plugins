// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/** @file Linux rtnetlink mutation and observation for RFC 8431 routes. */

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
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
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

/** Bounded request storage for one route and its portable attributes. */
struct LinuxRouteRequest {
  nlmsghdr header;
  rtmsg route;
  std::array<std::byte, 512> attributes;
};

bool AddAttribute(LinuxRouteRequest* request, unsigned short type,
                  const void* data, std::size_t size, std::string* error) {
  const std::size_t offset = NLMSG_ALIGN(request->header.nlmsg_len);
  const std::size_t attribute_length = RTA_LENGTH(size);
  const std::size_t required = offset + RTA_ALIGN(attribute_length);
  if (required > sizeof(*request) ||
      attribute_length > std::numeric_limits<unsigned short>::max()) {
    *error = "Linux route netlink request exceeds its attribute bound";
    return false;
  }
  auto* attribute =
      reinterpret_cast<rtattr*>(reinterpret_cast<std::byte*>(request) + offset);
  attribute->rta_type = type;
  attribute->rta_len = static_cast<unsigned short>(attribute_length);
  if (size != 0U) std::memcpy(RTA_DATA(attribute), data, size);
  request->header.nlmsg_len = static_cast<std::uint32_t>(required);
  return true;
}

bool ParseDestination(const Route& route, int* family, unsigned* prefix,
                      std::array<unsigned char, 16>* address,
                      std::string* error) {
  if (route.address_family == "ipv4") {
    *family = AF_INET;
  } else if (route.address_family == "ipv6") {
    *family = AF_INET6;
  } else {
    *error = "Linux route has an unsupported address family";
    return false;
  }
  const std::size_t separator = route.destination.rfind('/');
  if (separator == std::string::npos || separator == 0U ||
      separator + 1U == route.destination.size()) {
    *error = "Linux route destination is not an address prefix";
    return false;
  }
  const char* first = route.destination.data() + separator + 1U;
  const char* last = route.destination.data() + route.destination.size();
  const auto parsed = std::from_chars(first, last, *prefix);
  const unsigned maximum = *family == AF_INET ? 32U : 128U;
  if (parsed.ec != std::errc{} || parsed.ptr != last || *prefix > maximum) {
    *error = "Linux route destination has an invalid prefix length";
    return false;
  }
  const std::string text = route.destination.substr(0, separator);
  if (inet_pton(*family, text.c_str(), address->data()) != 1) {
    *error = "Linux route destination has an invalid address";
    return false;
  }
  return true;
}

bool SendAcknowledgedRouteRequest(LinuxRouteRequest* request,
                                  std::string* error) {
  const int socket_fd =
      socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
  if (socket_fd < 0) {
    *error = "cannot open Linux route netlink socket: " +
             std::string(std::strerror(errno));
    return false;
  }
  const auto close_socket = [&]() { close(socket_fd); };
  timeval timeout{.tv_sec = 5, .tv_usec = 0};
  if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                 sizeof(timeout)) != 0) {
    *error = "cannot bound Linux route acknowledgement: " +
             std::string(std::strerror(errno));
    close_socket();
    return false;
  }
  sockaddr_nl local{};
  local.nl_family = AF_NETLINK;
  if (bind(socket_fd, reinterpret_cast<const sockaddr*>(&local),
           sizeof(local)) != 0) {
    *error = "cannot bind Linux route netlink socket: " +
             std::string(std::strerror(errno));
    close_socket();
    return false;
  }
  static std::atomic<std::uint32_t> sequence{1U};
  request->header.nlmsg_seq = sequence.fetch_add(1U);
  sockaddr_nl kernel{};
  kernel.nl_family = AF_NETLINK;
  const ssize_t sent =
      sendto(socket_fd, request, request->header.nlmsg_len, 0,
             reinterpret_cast<const sockaddr*>(&kernel), sizeof(kernel));
  if (sent < 0 || static_cast<std::size_t>(sent) != request->header.nlmsg_len) {
    *error = "cannot send Linux route netlink request: " +
             std::string(std::strerror(errno));
    close_socket();
    return false;
  }
  std::array<char, 8192> response{};
  while (true) {
    const ssize_t received =
        recv(socket_fd, response.data(), response.size(), 0);
    if (received < 0) {
      if (errno == EINTR) continue;
      *error = "cannot receive Linux route acknowledgement: " +
               std::string(std::strerror(errno));
      close_socket();
      return false;
    }
    if (received == 0) {
      *error = "Linux route netlink socket closed before acknowledgement";
      close_socket();
      return false;
    }
    int remaining = static_cast<int>(received);
    for (nlmsghdr* header = reinterpret_cast<nlmsghdr*>(response.data());
         NLMSG_OK(header, remaining); header = NLMSG_NEXT(header, remaining)) {
      if (header->nlmsg_seq != request->header.nlmsg_seq) continue;
      if (header->nlmsg_type != NLMSG_ERROR) continue;
      if (header->nlmsg_len < NLMSG_LENGTH(sizeof(nlmsgerr))) {
        *error = "Linux route netlink acknowledgement is truncated";
        close_socket();
        return false;
      }
      const auto* acknowledgement =
          reinterpret_cast<const nlmsgerr*>(NLMSG_DATA(header));
      if (acknowledgement->error == 0) {
        close_socket();
        return true;
      }
      *error = "Linux kernel rejected route change: " +
               std::string(std::strerror(-acknowledgement->error));
      close_socket();
      return false;
    }
  }
}
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

bool ValidateLinuxChanges(const std::vector<Change>& changes,
                          std::string* error, std::string* error_path) {
  if (!error || !error_path) return false;
  for (const Change& change : changes) {
    if (!SafeTableAndInterface(change.route, error, error_path)) return false;
  }
  return true;
}

bool ApplyLinuxRouteChange(const Change& change, std::string* error) {
#if !defined(__linux__)
  (void)change;
  if (error) *error = "Linux route mutation is unavailable on this host";
  return false;
#else
  if (!error) return false;
  std::string path;
  if (!SafeTableAndInterface(change.route, error, &path)) return false;

  unsigned table = 0;
  const auto parsed =
      std::from_chars(change.route.rib.data(),
                      change.route.rib.data() + change.route.rib.size(), table);
  if (parsed.ec != std::errc{} ||
      parsed.ptr != change.route.rib.data() + change.route.rib.size()) {
    *error = "Linux route has an invalid table number";
    return false;
  }
  int family = AF_UNSPEC;
  unsigned prefix = 0;
  std::array<unsigned char, 16> destination{};
  if (!ParseDestination(change.route, &family, &prefix, &destination, error))
    return false;

  LinuxRouteRequest request{};
  request.header.nlmsg_len = NLMSG_LENGTH(sizeof(rtmsg));
  request.header.nlmsg_type =
      change.kind == ChangeKind::kDelete ? RTM_DELROUTE : RTM_NEWROUTE;
  request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
  if (change.kind == ChangeKind::kInstall)
    request.header.nlmsg_flags |= NLM_F_CREATE | NLM_F_REPLACE;
  request.route.rtm_family = static_cast<unsigned char>(family);
  request.route.rtm_dst_len = static_cast<unsigned char>(prefix);
  request.route.rtm_protocol =
      change.kind == ChangeKind::kInstall ? RTPROT_STATIC : RTPROT_UNSPEC;
  request.route.rtm_scope =
      change.route.gateway ? RT_SCOPE_UNIVERSE : RT_SCOPE_LINK;
  request.route.rtm_type = RTN_UNICAST;
  if (table <= std::numeric_limits<unsigned char>::max()) {
    request.route.rtm_table = static_cast<unsigned char>(table);
  } else {
    request.route.rtm_table = RT_TABLE_UNSPEC;
    const std::uint32_t table_attribute = table;
    if (!AddAttribute(&request, RTA_TABLE, &table_attribute,
                      sizeof(table_attribute), error))
      return false;
  }
  const std::size_t address_size = family == AF_INET ? 4U : 16U;
  if (!AddAttribute(&request, RTA_DST, destination.data(), address_size, error))
    return false;
  if (change.route.gateway) {
    std::array<unsigned char, 16> gateway{};
    if (inet_pton(family, change.route.gateway->c_str(), gateway.data()) != 1) {
      *error = "Linux route gateway has an invalid address";
      return false;
    }
    if (!AddAttribute(&request, RTA_GATEWAY, gateway.data(), address_size,
                      error))
      return false;
  }
  if (change.route.interface) {
    errno = 0;
    const unsigned interface_index =
        if_nametoindex(change.route.interface->c_str());
    if (interface_index == 0U) {
      *error = "Linux route outgoing interface does not exist";
      if (errno != 0) *error += ": " + std::string(std::strerror(errno));
      return false;
    }
    const std::uint32_t interface_attribute = interface_index;
    if (!AddAttribute(&request, RTA_OIF, &interface_attribute,
                      sizeof(interface_attribute), error))
      return false;
  }
  if (change.kind == ChangeKind::kInstall) {
    const std::uint32_t preference = change.route.preference;
    if (!AddAttribute(&request, RTA_PRIORITY, &preference, sizeof(preference),
                      error))
      return false;
  }
  return SendAcknowledgedRouteRequest(&request, error);
#endif
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

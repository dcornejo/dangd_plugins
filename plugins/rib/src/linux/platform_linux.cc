// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/** @file Linux rtnetlink mutation and observation for RFC 8431 routes. */

#include "plugins/rib/src/platform_command.h"
#include "plugins/rib/src/route_observer.h"

#include <charconv>

#if defined(__linux__)
#include <arpa/inet.h>
#include <linux/netlink.h>
#include <linux/nexthop.h>
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
#include <map>
#include <set>
#include <tuple>
#endif

namespace dang::rib {
namespace {

#if defined(__linux__)
static_assert(offsetof(nexthop_grp, weight) == 4U);
static_assert(sizeof(nexthop_grp) >= 6U);

/** One native path from RTA_MULTIPATH or an expanded nexthop object. */
struct LinuxNexthop {
  std::optional<std::string> gateway;
  std::optional<std::string> special;
  unsigned interface_index = 0;
  bool installed = true;
  std::uint32_t weight = 1;
};

/** One member reference and its exact positive kernel selection weight. */
struct LinuxNexthopGroupMember {
  std::uint32_t id = 0;
  std::uint32_t weight = 1;
};

/** One Linux persistent nexthop object or group returned by route netlink. */
struct LinuxNexthopObject {
  int family = AF_UNSPEC;
  unsigned flags = 0;
  std::optional<std::string> gateway;
  std::optional<std::string> special;
  unsigned interface_index = 0;
  std::vector<LinuxNexthopGroupMember> members;
  bool unsupported = false;
};

using LinuxNexthopObjects = std::map<std::uint32_t, LinuxNexthopObject>;

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

/** Reads the kernel nexthop-object registry used by route RTA_NH_ID values. */
bool ReadLinuxNexthopObjects(LinuxNexthopObjects* objects,
                             std::string* error) {
  objects->clear();
  const int socket_fd =
      socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
  if (socket_fd < 0) {
    *error = "cannot open Linux nexthop netlink socket: " +
             std::string(std::strerror(errno));
    return false;
  }
  const auto close_socket = [&]() { close(socket_fd); };
  struct Request {
    nlmsghdr header;
    nhmsg nexthop;
  } request{};
  request.header.nlmsg_len = NLMSG_LENGTH(sizeof(nhmsg));
  request.header.nlmsg_type = RTM_GETNEXTHOP;
  request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
  request.header.nlmsg_seq = 1U;
  request.nexthop.nh_family = AF_UNSPEC;
  if (send(socket_fd, &request, request.header.nlmsg_len, 0) < 0) {
    *error = "cannot request Linux nexthop objects: " +
             std::string(std::strerror(errno));
    close_socket();
    return false;
  }

  std::array<char, 32768> buffer{};
  bool done = false;
  while (!done) {
    const ssize_t received = recv(socket_fd, buffer.data(), buffer.size(), 0);
    if (received < 0) {
      if (errno == EINTR) continue;
      *error = "cannot receive Linux nexthop objects: " +
               std::string(std::strerror(errno));
      close_socket();
      return false;
    }
    if (received == 0) {
      *error = "Linux nexthop dump ended before completion";
      close_socket();
      return false;
    }
    int remaining = static_cast<int>(received);
    for (nlmsghdr* header = reinterpret_cast<nlmsghdr*>(buffer.data());
         NLMSG_OK(header, remaining); header = NLMSG_NEXT(header, remaining)) {
      if (header->nlmsg_seq != request.header.nlmsg_seq) continue;
      if (header->nlmsg_type == NLMSG_DONE) {
        if ((header->nlmsg_flags & NLM_F_DUMP_INTR) != 0U) {
          *error = "Linux nexthop dump was interrupted by a registry change";
          close_socket();
          return false;
        }
        done = true;
        break;
      }
      if (header->nlmsg_type == NLMSG_ERROR) {
        if (header->nlmsg_len < NLMSG_LENGTH(sizeof(nlmsgerr))) {
          *error = "Linux nexthop dump returned a truncated error";
          close_socket();
          return false;
        }
        const auto* failure =
            reinterpret_cast<const nlmsgerr*>(NLMSG_DATA(header));
        if (failure->error == 0) {
          done = true;
          break;
        }
        const int code = -failure->error;
        // Kernels predating the nexthop-object API have no objects to expand;
        // retain the historical route observation behavior on those hosts.
        if (code == EOPNOTSUPP || code == EINVAL) {
          objects->clear();
          close_socket();
          return true;
        }
        *error = "Linux kernel rejected nexthop dump: " +
                 std::string(std::strerror(code));
        close_socket();
        return false;
      }
      if (header->nlmsg_type != RTM_NEWNEXTHOP ||
          header->nlmsg_len < NLMSG_LENGTH(sizeof(nhmsg)))
        continue;
      const auto* info = reinterpret_cast<const nhmsg*>(NLMSG_DATA(header));
      LinuxNexthopObject object;
      object.family = info->nh_family;
      object.flags = info->nh_flags;
      std::uint32_t id = 0;
      int attributes_length =
          static_cast<int>(NLMSG_PAYLOAD(header, sizeof(nhmsg)));
      for (rtattr* attribute = reinterpret_cast<rtattr*>(
               reinterpret_cast<char*>(const_cast<nhmsg*>(info)) +
               NLMSG_ALIGN(sizeof(nhmsg)));
           RTA_OK(attribute, attributes_length);
           attribute = RTA_NEXT(attribute, attributes_length)) {
        const std::size_t payload = RTA_PAYLOAD(attribute);
        if (attribute->rta_type == NHA_ID && payload >= sizeof(id)) {
          std::memcpy(&id, RTA_DATA(attribute), sizeof(id));
        } else if (attribute->rta_type == NHA_OIF &&
                   payload >= sizeof(std::uint32_t)) {
          std::uint32_t index = 0;
          std::memcpy(&index, RTA_DATA(attribute), sizeof(index));
          object.interface_index = index;
        } else if (attribute->rta_type == NHA_GATEWAY) {
          char address[INET6_ADDRSTRLEN]{};
          if ((object.family == AF_INET && payload >= 4U) ||
              (object.family == AF_INET6 && payload >= 16U)) {
            if (inet_ntop(object.family, RTA_DATA(attribute), address,
                          sizeof(address)))
              object.gateway = address;
          } else {
            object.unsupported = true;
          }
        } else if (attribute->rta_type == NHA_GROUP) {
          if (payload % sizeof(nexthop_grp) != 0U) {
            object.unsupported = true;
            continue;
          }
          const auto* group =
              reinterpret_cast<const nexthop_grp*>(RTA_DATA(attribute));
          for (std::size_t index = 0; index < payload / sizeof(*group);
               ++index) {
            // NHA_GROUP encodes weight minus one in two adjacent bytes. The
            // second byte was formerly reserved, so reading the wire layout
            // also remains compatible with kernels limited to 8-bit weights.
            const auto* encoded = reinterpret_cast<const unsigned char*>(
                &group[index]);
            const std::uint32_t weight_minus_one =
                static_cast<std::uint32_t>(encoded[4]) |
                (static_cast<std::uint32_t>(encoded[5]) << 8U);
            object.members.push_back(
                {.id = group[index].id, .weight = weight_minus_one + 1U});
          }
        } else if (attribute->rta_type == NHA_BLACKHOLE) {
          object.special = "discard";
        } else if (attribute->rta_type == NHA_ENCAP ||
                   attribute->rta_type == NHA_ENCAP_TYPE ||
                   attribute->rta_type == NHA_FDB) {
          object.unsupported = true;
        }
      }
      if (id != 0U) (*objects)[id] = std::move(object);
    }
  }
  close_socket();
  return true;
}

/** Recursively expands one simple or grouped object into RFC base nexthops. */
bool ExpandLinuxNexthopObject(std::uint32_t id, int route_family,
                              const LinuxNexthopObjects& objects,
                              std::set<std::uint32_t>* visiting,
                              std::vector<LinuxNexthop>* paths) {
  const auto found = objects.find(id);
  if (found == objects.end() || found->second.unsupported ||
      !visiting->insert(id).second)
    return false;
  const LinuxNexthopObject& object = found->second;
  const std::size_t first_path = paths->size();
  bool valid = true;
  if (!object.members.empty()) {
    for (const LinuxNexthopGroupMember& member : object.members) {
      const std::size_t member_first_path = paths->size();
      if (!ExpandLinuxNexthopObject(member.id, route_family, objects, visiting,
                                    paths)) {
        valid = false;
        break;
      }
      for (std::size_t index = member_first_path; index < paths->size();
           ++index) {
        const std::uint64_t weighted =
            static_cast<std::uint64_t>((*paths)[index].weight) * member.weight;
        if (weighted > std::numeric_limits<std::uint32_t>::max()) {
          valid = false;
          break;
        }
        (*paths)[index].weight = static_cast<std::uint32_t>(weighted);
      }
      if (!valid) break;
    }
  } else if ((object.family == AF_UNSPEC || object.family == route_family) &&
             (object.gateway || object.interface_index != 0U ||
              object.special)) {
    paths->push_back({.gateway = object.gateway,
                      .special = object.special,
                      .interface_index = object.interface_index,
                      .installed =
                          (object.flags & (RTNH_F_DEAD | RTNH_F_LINKDOWN)) ==
                          0U});
  } else {
    valid = false;
  }
  if ((object.flags & (RTNH_F_DEAD | RTNH_F_LINKDOWN)) != 0U)
    for (std::size_t index = first_path; index < paths->size(); ++index)
      (*paths)[index].installed = false;
  visiting->erase(id);
  if (!valid) paths->resize(first_path);
  return valid;
}
#endif

bool SafeTableAndInterface(const Route& route, std::string* error,
                           std::string* path) {
  if (!route.load_balance.empty()) {
    *error = "Linux weighted load-balance mutation is not implemented yet";
    *path = "/ietf-i2rs-rib:routing-instance/rib-list/route-list/nexthop/"
            "nexthop-lb";
    return false;
  }
  if (route.local_only) {
    *error = "Linux cannot safely map a configured RFC 8431 local-only route";
    *path = "/ietf-i2rs-rib:routing-instance/rib-list/route-list/"
            "route-attributes/local-only";
    return false;
  }
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
  if (route.special && *route.special != "discard" &&
      *route.special != "discard-with-error") {
    *error = "Linux route has an unsupported special nexthop";
    *path = "/ietf-i2rs-rib:routing-instance/rib-list/route-list/nexthop/"
            "nexthop-base/special";
    return false;
  }
  if (route.special && (route.gateway || route.interface)) {
    *error = "Linux special route cannot include another nexthop";
    *path = "/ietf-i2rs-rib:routing-instance/rib-list/route-list/nexthop/"
            "nexthop-base";
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
    command.arguments = {
        "ip", change.route.address_family == "ipv4" ? "-4" : "-6",
        "route", change.kind == ChangeKind::kDelete
                     ? "delete"
                     : change.kind == ChangeKind::kAdd ? "add" : "replace"};
    if (change.route.special)
      command.arguments.push_back(*change.route.special == "discard"
                                      ? "blackhole"
                                      : "unreachable");
    command.arguments.insert(command.arguments.end(),
                             {change.route.destination, "table", change.route.rib});
    if (change.route.gateway)
      command.arguments.insert(command.arguments.end(), {"via", *change.route.gateway});
    if (change.route.interface)
      command.arguments.insert(command.arguments.end(), {"dev", *change.route.interface});
    if (change.kind != ChangeKind::kDelete)
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
  else if (change.kind == ChangeKind::kAdd)
    request.header.nlmsg_flags |= NLM_F_CREATE | NLM_F_EXCL;
  request.route.rtm_family = static_cast<unsigned char>(family);
  request.route.rtm_dst_len = static_cast<unsigned char>(prefix);
  request.route.rtm_protocol =
      change.kind != ChangeKind::kDelete ? RTPROT_STATIC : RTPROT_UNSPEC;
  request.route.rtm_scope =
      (change.route.gateway || change.route.special) ? RT_SCOPE_UNIVERSE
                                                     : RT_SCOPE_LINK;
  request.route.rtm_type =
      !change.route.special
          ? RTN_UNICAST
          : *change.route.special == "discard" ? RTN_BLACKHOLE
                                                : RTN_UNREACHABLE;
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
  if (change.kind != ChangeKind::kDelete) {
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
  LinuxNexthopObjects nexthop_objects;
  if (!ReadLinuxNexthopObjects(&nexthop_objects, error)) return false;
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
      // Receive routes are kernel-owned. Blackhole and reject routes have an
      // exact reversible mapping in the portable backend.
      observed.mutable_route =
          info->rtm_type == RTN_UNICAST || info->rtm_type == RTN_BLACKHOLE ||
          info->rtm_type == RTN_UNREACHABLE || info->rtm_type == RTN_PROHIBIT;
      std::array<unsigned char, 16> destination{};
      unsigned table = info->rtm_table;
      unsigned interface_index = 0;
      std::uint32_t metric = 0;
      std::optional<std::uint32_t> nexthop_id;
      bool from_nexthop_object = false;
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
        else if (attribute->rta_type == RTA_NH_ID &&
                 RTA_PAYLOAD(attribute) >= sizeof(std::uint32_t)) {
          std::uint32_t id = 0;
          std::memcpy(&id, RTA_DATA(attribute), sizeof(id));
          nexthop_id = id;
        }
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
            // rtnh_hops stores the positive route weight minus one.
            path.weight =
                static_cast<std::uint32_t>(nexthop->rtnh_hops) + 1U;
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
      from_nexthop_object = nexthop_id.has_value();
      if (nexthop_id) {
        std::set<std::uint32_t> visiting;
        std::vector<LinuxNexthop> expanded;
        if (!ExpandLinuxNexthopObject(*nexthop_id, info->rtm_family,
                                      nexthop_objects, &visiting, &expanded))
          continue;
        multipath = std::move(expanded);
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
        std::set<std::tuple<std::optional<std::string>,
                            std::optional<std::string>,
                            std::optional<std::string>>> emitted_paths;
        std::vector<ObservedRoute> members;
        bool representable = true;
        for (const LinuxNexthop& path : multipath) {
          ObservedRoute member = observed;
          member.installed = path.installed;
          member.weight = path.weight;
          if (!path.installed) member.reason = "unresolved-nexthop";
          // The RFC base view cannot retain the Linux object ID or group
          // topology needed to recreate this route during rollback. Keep it
          // observable, but never approximate an imperative mutation.
          if (from_nexthop_object) member.mutable_route = false;
          member.route.gateway = path.gateway;
          member.route.interface = resolve_interface_name(path.interface_index);
          member.route.special = path.special;
          if (member.route.special) {
            member.route.gateway.reset();
            member.route.interface.reset();
          }
          // A kernel nexthop object referenced only by ID cannot yet be
          // represented if it uses encapsulation or another unsupported
          // object type. Never emit an empty, schema-invalid nexthop.
          if (!member.route.gateway && !member.route.interface &&
              !member.route.special)
            continue;
          if (!emitted_paths.emplace(member.route.gateway,
                                     member.route.interface,
                                     member.route.special).second) {
            // Two paths with the same RFC base identity cannot receive
            // distinct list keys without inventing model state. Omit the
            // entire route instead of silently dropping one native weight.
            representable = false;
            break;
          }
          members.push_back(std::move(member));
        }
        if (representable)
          for (ObservedRoute& member : members) append(std::move(member));
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

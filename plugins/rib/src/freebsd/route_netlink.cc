// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/** @file FreeBSD route mutation through the acknowledged route-netlink ABI. */

#include "plugins/rib/src/platform_command.h"

#if defined(__FreeBSD__)
#include <arpa/inet.h>
#include <net/if.h>
#include <netlink/netlink.h>
#include <netlink/route/common.h>
#include <netlink/route/route.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#endif

namespace dang::rib {
namespace {

#if defined(__FreeBSD__)
/** Fixed upper bound for one route and every attribute emitted by the plugin. */
struct FreeBsdRouteRequest {
  nlmsghdr header;
  rtmsg route;
  std::array<std::byte, 512> attributes;
};

bool AddAttribute(FreeBsdRouteRequest* request, std::uint16_t type,
                  const void* data, std::size_t size, std::string* error) {
  const std::size_t offset = NLMSG_ALIGN(request->header.nlmsg_len);
  const std::size_t attribute_length = NLA_HDRLEN + size;
  const std::size_t required = offset + NLA_ALIGN(attribute_length);
  if (required > sizeof(*request) ||
      attribute_length > std::numeric_limits<std::uint16_t>::max()) {
    *error = "FreeBSD route netlink request exceeds its attribute bound";
    return false;
  }
  auto* attribute = reinterpret_cast<nlattr*>(
      reinterpret_cast<std::byte*>(request) + offset);
  attribute->nla_type = type;
  attribute->nla_len = static_cast<std::uint16_t>(attribute_length);
  if (size != 0U) std::memcpy(attribute + 1, data, size);
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
    *error = "FreeBSD route has an unsupported address family";
    return false;
  }
  const std::size_t separator = route.destination.rfind('/');
  if (separator == std::string::npos || separator == 0U ||
      separator + 1U == route.destination.size()) {
    *error = "FreeBSD route destination is not an address prefix";
    return false;
  }
  const char* first = route.destination.data() + separator + 1U;
  const char* last = route.destination.data() + route.destination.size();
  const auto parsed = std::from_chars(first, last, *prefix);
  const unsigned maximum = *family == AF_INET ? 32U : 128U;
  if (parsed.ec != std::errc{} || parsed.ptr != last || *prefix > maximum) {
    *error = "FreeBSD route destination has an invalid prefix length";
    return false;
  }
  const std::string text = route.destination.substr(0, separator);
  if (inet_pton(*family, text.c_str(), address->data()) != 1) {
    *error = "FreeBSD route destination has an invalid address";
    return false;
  }
  return true;
}

bool SendAcknowledgedRouteRequest(FreeBsdRouteRequest* request,
                                  std::string* error) {
  const int socket_fd =
      socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
  if (socket_fd < 0) {
    *error = "cannot open FreeBSD route netlink socket: " +
             std::string(std::strerror(errno));
    return false;
  }
  const auto close_socket = [&]() { close(socket_fd); };
  timeval timeout{.tv_sec = 5, .tv_usec = 0};
  if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                 sizeof(timeout)) != 0) {
    *error = "cannot bound FreeBSD route acknowledgement: " +
             std::string(std::strerror(errno));
    close_socket();
    return false;
  }
  static std::atomic<std::uint32_t> sequence{1U};
  request->header.nlmsg_seq = sequence.fetch_add(1U);
  const ssize_t sent =
      send(socket_fd, request, request->header.nlmsg_len, 0);
  if (sent < 0 || static_cast<std::size_t>(sent) != request->header.nlmsg_len) {
    *error = "cannot send FreeBSD route netlink request: " +
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
      *error = "cannot receive FreeBSD route acknowledgement: " +
               std::string(std::strerror(errno));
      close_socket();
      return false;
    }
    if (received == 0) {
      *error = "FreeBSD route netlink socket closed before acknowledgement";
      close_socket();
      return false;
    }
    std::size_t remaining = static_cast<std::size_t>(received);
    for (nlmsghdr* header = reinterpret_cast<nlmsghdr*>(response.data());
         NLMSG_OK(header, remaining); header = NLMSG_NEXT(header, remaining)) {
      if (header->nlmsg_seq != request->header.nlmsg_seq ||
          header->nlmsg_type != NLMSG_ERROR)
        continue;
      if (header->nlmsg_len < NLMSG_LENGTH(sizeof(nlmsgerr))) {
        *error = "FreeBSD route netlink acknowledgement is truncated";
        close_socket();
        return false;
      }
      const auto* acknowledgement =
          reinterpret_cast<const nlmsgerr*>(NLMSG_DATA(header));
      if (acknowledgement->error == 0) {
        close_socket();
        return true;
      }
      // FreeBSD reports positive errno values. Accept a negative value too so
      // an ABI-compatible kernel cannot turn a useful error into "Unknown".
      const int code = acknowledgement->error < 0
                           ? -acknowledgement->error
                           : acknowledgement->error;
      *error = "FreeBSD kernel rejected route change: " +
               std::string(std::strerror(code));
      close_socket();
      return false;
    }
  }
}
#endif

}  // namespace

bool ApplyFreeBsdRouteChange(const Change& change, std::string* error) {
#if !defined(__FreeBSD__)
  (void)change;
  if (error) *error = "FreeBSD route mutation is unavailable on this host";
  return false;
#else
  if (!error) return false;
  std::string path;
  if (!ValidateFreeBsdChanges({change}, error, &path)) return false;

  const auto apply_member = [&](ChangeKind kind,
                                const WeightedNexthop* member,
                                bool append, std::string* why) {
    unsigned fib = 0;
    const auto parsed = std::from_chars(
        change.route.rib.data(),
        change.route.rib.data() + change.route.rib.size(), fib);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != change.route.rib.data() + change.route.rib.size()) {
      *why = "FreeBSD route has an invalid FIB number";
      return false;
    }
    int family = AF_UNSPEC;
    unsigned prefix = 0;
    std::array<unsigned char, 16> destination{};
    if (!ParseDestination(change.route, &family, &prefix, &destination, why))
      return false;

    const std::optional<std::string>& gateway =
        member ? member->gateway : change.route.gateway;
    const std::optional<std::string>& interface =
        member ? member->interface : change.route.interface;
    FreeBsdRouteRequest request{};
    request.header.nlmsg_len = NLMSG_LENGTH(sizeof(rtmsg));
    request.header.nlmsg_type =
        kind == ChangeKind::kDelete ? RTM_DELROUTE : RTM_NEWROUTE;
    request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    if (kind == ChangeKind::kInstall)
      request.header.nlmsg_flags |=
          NLM_F_CREATE | (append ? NLM_F_APPEND : NLM_F_REPLACE);
    else if (kind == ChangeKind::kAdd)
      request.header.nlmsg_flags |=
          NLM_F_CREATE | (append ? NLM_F_APPEND : NLM_F_EXCL);
    request.route.rtm_family = static_cast<unsigned char>(family);
    request.route.rtm_dst_len = static_cast<unsigned char>(prefix);
    request.route.rtm_table = 0;  // FreeBSD requires RTA_TABLE for one FIB.
    request.route.rtm_protocol =
        kind != ChangeKind::kDelete ? RTPROT_STATIC : RTPROT_UNSPEC;
    request.route.rtm_scope =
        (gateway || change.route.special) ? RT_SCOPE_UNIVERSE : RT_SCOPE_LINK;
    request.route.rtm_type =
        !change.route.special
            ? RTN_UNICAST
            : *change.route.special == "discard" ? RTN_BLACKHOLE
                                                  : RTN_PROHIBIT;

    const std::size_t address_size = family == AF_INET ? 4U : 16U;
    if (!AddAttribute(&request, NL_RTA_DST, destination.data(), address_size,
                      why))
      return false;
    const std::uint32_t table_attribute = fib;
    if (!AddAttribute(&request, NL_RTA_TABLE, &table_attribute,
                      sizeof(table_attribute), why))
      return false;

    // A base-route delete intentionally selects by FIB and destination. A
    // load-balance delete must include one member's gateway/interface so the
    // kernel removes that path without collapsing its siblings.
    if (kind != ChangeKind::kDelete || member) {
      if (gateway) {
        std::array<unsigned char, 16> encoded{};
        if (inet_pton(family, gateway->c_str(), encoded.data()) != 1) {
          *why = "FreeBSD route gateway has an invalid address";
          return false;
        }
        if (!AddAttribute(&request, NL_RTA_GATEWAY, encoded.data(),
                          address_size, why))
          return false;
      }
      if (change.route.special) {
        // FreeBSD represents blackhole and reject routes through the matching
        // loopback gateway. This native detail is not exposed in RFC 8431.
        std::array<unsigned char, 16> encoded{};
        const char* loopback = family == AF_INET ? "127.0.0.1" : "::1";
        if (inet_pton(family, loopback, encoded.data()) != 1 ||
            !AddAttribute(&request, NL_RTA_GATEWAY, encoded.data(),
                          address_size, why)) {
          if (why->empty()) *why = "cannot encode the FreeBSD loopback gateway";
          return false;
        }
      } else if (interface) {
        errno = 0;
        const unsigned index = if_nametoindex(interface->c_str());
        if (index == 0U) {
          *why = "FreeBSD route outgoing interface does not exist";
          if (errno != 0) *why += ": " + std::string(std::strerror(errno));
          return false;
        }
        const std::uint32_t value = index;
        if (!AddAttribute(&request, NL_RTA_OIF, &value, sizeof(value), why))
          return false;
      }
      if (kind != ChangeKind::kDelete) {
        const std::uint32_t preference = change.route.preference;
        if (!AddAttribute(&request, NL_RTA_PRIORITY, &preference,
                          sizeof(preference), why))
          return false;
        if (member) {
          const std::uint32_t weight = member->weight;
          if (!AddAttribute(&request, NL_RTA_WEIGHT, &weight, sizeof(weight),
                            why))
            return false;
        }
      }
    }
    return SendAcknowledgedRouteRequest(&request, why);
  };

  if (change.route.load_balance.empty())
    return apply_member(change.kind, nullptr, false, error);

  std::size_t completed = 0;
  for (; completed < change.route.load_balance.size(); ++completed) {
    const std::size_t index = change.kind == ChangeKind::kDelete
                                  ? change.route.load_balance.size() - completed - 1U
                                  : completed;
    const bool append = change.kind != ChangeKind::kDelete && completed != 0U;
    if (apply_member(change.kind, &change.route.load_balance[index], append,
                     error))
      continue;

    // Keep one modeled route atomic even though FreeBSD mutates each ECMP path
    // separately. Reverse every completed member before reporting failure.
    const std::string primary = *error;
    std::string rollback_error;
    while (completed > 0U) {
      --completed;
      const std::size_t restored_index =
          change.kind == ChangeKind::kDelete
              ? change.route.load_balance.size() - completed - 1U
              : completed;
      const ChangeKind inverse = change.kind == ChangeKind::kDelete
                                     ? ChangeKind::kInstall
                                     : ChangeKind::kDelete;
      const bool restore_append = inverse != ChangeKind::kDelete;
      std::string member_error;
      if (!apply_member(inverse, &change.route.load_balance[restored_index],
                        restore_append, &member_error)) {
        if (!rollback_error.empty()) rollback_error += "; ";
        rollback_error += member_error;
      }
    }
    *error = primary;
    if (!rollback_error.empty())
      *error += "; load-balance rollback failed: " + rollback_error;
    return false;
  }
  return true;
#endif
}

}  // namespace dang::rib

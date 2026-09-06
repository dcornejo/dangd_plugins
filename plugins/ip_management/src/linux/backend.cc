// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "plugins/ip_management/src/platform_backend.h"
#include "plugins/ip_management/src/platform_config.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <ifaddrs.h>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <linux/if_addr.h>
#include <linux/if_link.h>
#include <linux/neighbour.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <pugixml.hpp>

namespace dangd::ip_management {
namespace {

constexpr std::size_t kMessageBytes = 1024;

struct LiveAddress {
  std::string address;
  std::string status;
  unsigned prefix_length = 0;
  bool ipv6 = false;
};

struct LiveNeighbor {
  std::string address;
  std::string link_layer_address;
  std::string origin;
  std::string state;
  bool ipv6 = false;
  bool router = false;
};

const InterfaceConfig* Find(const std::vector<InterfaceConfig>& values,
                            const std::string& name) {
  const auto found = std::ranges::find(values, name, &InterfaceConfig::name);
  return found == values.end() ? nullptr : &*found;
}

bool AddAttribute(nlmsghdr* header, std::size_t capacity, std::uint16_t type,
                  const void* data, std::size_t size) {
  const std::size_t attribute_size = RTA_LENGTH(size);
  const std::size_t offset = NLMSG_ALIGN(header->nlmsg_len);
  if (offset + RTA_ALIGN(attribute_size) > capacity) return false;
  auto* attribute = reinterpret_cast<rtattr*>(
      reinterpret_cast<std::byte*>(header) + offset);
  attribute->rta_type = type;
  attribute->rta_len = static_cast<unsigned short>(attribute_size);
  if (size) std::memcpy(RTA_DATA(attribute), data, size);
  header->nlmsg_len = static_cast<std::uint32_t>(
      offset + RTA_ALIGN(attribute_size));
  return true;
}

class RouteSocket {
 public:
  RouteSocket() {
    descriptor_ = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (descriptor_ < 0) {
      error_ = errno;
      return;
    }
    sockaddr_nl local{};
    local.nl_family = AF_NETLINK;
    if (bind(descriptor_, reinterpret_cast<sockaddr*>(&local), sizeof(local))) {
      error_ = errno;
      close(descriptor_);
      descriptor_ = -1;
    }
  }
  ~RouteSocket() {
    if (descriptor_ >= 0) close(descriptor_);
  }
  RouteSocket(const RouteSocket&) = delete;
  RouteSocket& operator=(const RouteSocket&) = delete;

  bool valid(std::string* error) const {
    if (descriptor_ >= 0) return true;
    if (error) *error = "cannot open rtnetlink socket: " +
                        std::string(std::strerror(error_));
    return false;
  }

  bool Request(nlmsghdr* request, std::string_view description,
               std::string* error) {
    request->nlmsg_seq = ++sequence_;
    sockaddr_nl kernel{};
    kernel.nl_family = AF_NETLINK;
    iovec part{request, request->nlmsg_len};
    msghdr message{};
    message.msg_name = &kernel;
    message.msg_namelen = sizeof(kernel);
    message.msg_iov = &part;
    message.msg_iovlen = 1;
    if (sendmsg(descriptor_, &message, 0) < 0) {
      if (error) *error = std::string(description) + ": " +
                          std::strerror(errno);
      return false;
    }
    std::array<std::byte, 8192> response{};
    while (true) {
      sockaddr_nl sender{};
      iovec response_part{response.data(), response.size()};
      msghdr response_message{};
      response_message.msg_name = &sender;
      response_message.msg_namelen = sizeof(sender);
      response_message.msg_iov = &response_part;
      response_message.msg_iovlen = 1;
      const ssize_t received = recvmsg(descriptor_, &response_message, 0);
      if (received < 0 && errno == EINTR) continue;
      if (received <= 0) {
        if (error) *error = std::string(description) + ": " +
                            (received ? std::strerror(errno)
                                      : "netlink peer closed");
        return false;
      }
      if (response_message.msg_flags & MSG_TRUNC) {
        if (error) *error = "neighbor dump exceeded the receive buffer";
        return false;
      }
      if (sender.nl_pid != 0) continue;
      int remaining = static_cast<int>(received);
      for (auto* header = reinterpret_cast<nlmsghdr*>(response.data());
           NLMSG_OK(header, remaining); header = NLMSG_NEXT(header, remaining)) {
        if (header->nlmsg_seq != sequence_ || header->nlmsg_type != NLMSG_ERROR)
          continue;
        if (NLMSG_PAYLOAD(header, 0) < sizeof(nlmsgerr)) {
          if (error) *error = std::string(description) +
                              ": truncated rtnetlink acknowledgement";
          return false;
        }
        const auto* reply =
            reinterpret_cast<const nlmsgerr*>(NLMSG_DATA(header));
        if (!reply->error) return true;
        if (error) *error = std::string(description) + ": " +
                            std::strerror(-reply->error);
        return false;
      }
    }
  }

  bool Neighbors(unsigned interface_index, std::vector<LiveNeighbor>* result,
                 std::string* error) {
    struct {
      nlmsghdr header;
      ndmsg body;
    } request{};
    request.header.nlmsg_len = NLMSG_LENGTH(sizeof(ndmsg));
    request.header.nlmsg_type = RTM_GETNEIGH;
    request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    request.header.nlmsg_seq = ++sequence_;
    request.body.ndm_family = AF_UNSPEC;
    sockaddr_nl kernel{};
    kernel.nl_family = AF_NETLINK;
    iovec part{&request, request.header.nlmsg_len};
    msghdr message{};
    message.msg_name = &kernel;
    message.msg_namelen = sizeof(kernel);
    message.msg_iov = &part;
    message.msg_iovlen = 1;
    if (sendmsg(descriptor_, &message, 0) < 0) {
      if (error) *error = "dump neighbors: " +
                          std::string(std::strerror(errno));
      return false;
    }
    std::array<std::byte, 16384> response{};
    while (true) {
      sockaddr_nl sender{};
      iovec response_part{response.data(), response.size()};
      msghdr response_message{};
      response_message.msg_name = &sender;
      response_message.msg_namelen = sizeof(sender);
      response_message.msg_iov = &response_part;
      response_message.msg_iovlen = 1;
      const ssize_t received = recvmsg(descriptor_, &response_message, 0);
      if (received < 0 && errno == EINTR) continue;
      if (received <= 0) {
        if (error) *error = "dump neighbors: " + std::string(
            received ? std::strerror(errno) : "netlink peer closed");
        return false;
      }
      if (sender.nl_pid != 0) continue;
      int remaining = static_cast<int>(received);
      for (auto* header = reinterpret_cast<nlmsghdr*>(response.data());
           NLMSG_OK(header, remaining); header = NLMSG_NEXT(header, remaining)) {
        if (header->nlmsg_seq != sequence_) continue;
        if (header->nlmsg_flags & NLM_F_DUMP_INTR) {
          if (error) *error = "neighbor dump was interrupted by a kernel change";
          return false;
        }
        if (header->nlmsg_type == NLMSG_DONE) return true;
        if (header->nlmsg_type == NLMSG_ERROR) {
          if (error) {
            if (NLMSG_PAYLOAD(header, 0) < sizeof(nlmsgerr)) {
              *error = "truncated neighbor dump error";
            } else {
              const auto* reply = reinterpret_cast<const nlmsgerr*>(
                  NLMSG_DATA(header));
              *error = reply->error
                  ? "kernel rejected neighbor dump: " +
                        std::string(std::strerror(-reply->error))
                  : "unexpected neighbor dump acknowledgement";
            }
          }
          return false;
        }
        if (header->nlmsg_type != RTM_NEWNEIGH ||
            NLMSG_PAYLOAD(header, 0) < sizeof(ndmsg))
          continue;
        const auto* neighbor = reinterpret_cast<const ndmsg*>(
            NLMSG_DATA(header));
        if (neighbor->ndm_ifindex != static_cast<int>(interface_index) ||
            (neighbor->ndm_family != AF_INET &&
             neighbor->ndm_family != AF_INET6))
          continue;
        const void* destination = nullptr;
        std::size_t destination_size = 0;
        const std::byte* link_layer = nullptr;
        std::size_t link_layer_size = 0;
        int attribute_bytes = static_cast<int>(
            NLMSG_PAYLOAD(header, sizeof(ndmsg)));
        for (auto* attribute = reinterpret_cast<rtattr*>(
                 reinterpret_cast<std::byte*>(const_cast<ndmsg*>(neighbor)) +
                 NLMSG_ALIGN(sizeof(ndmsg)));
             RTA_OK(attribute, attribute_bytes);
             attribute = RTA_NEXT(attribute, attribute_bytes)) {
          if (attribute->rta_type == NDA_DST) {
            destination = RTA_DATA(attribute);
            destination_size = RTA_PAYLOAD(attribute);
          } else if (attribute->rta_type == NDA_LLADDR) {
            link_layer = reinterpret_cast<const std::byte*>(RTA_DATA(attribute));
            link_layer_size = RTA_PAYLOAD(attribute);
          }
        }
        const std::size_t expected = neighbor->ndm_family == AF_INET
            ? sizeof(in_addr) : sizeof(in6_addr);
        if (!destination || destination_size != expected || !link_layer ||
            link_layer_size == 0)
          continue;
        char address[INET6_ADDRSTRLEN]{};
        if (!inet_ntop(neighbor->ndm_family, destination, address,
                       sizeof(address)))
          continue;
        std::ostringstream hardware;
        hardware << std::hex;
        for (std::size_t index = 0; index < link_layer_size; ++index) {
          if (index) hardware << ':';
          hardware.width(2);
          hardware.fill('0');
          hardware << static_cast<unsigned>(
              std::to_integer<unsigned char>(link_layer[index]));
        }
        std::string state;
        if (neighbor->ndm_state & NUD_REACHABLE) state = "reachable";
        else if (neighbor->ndm_state & NUD_STALE) state = "stale";
        else if (neighbor->ndm_state & NUD_DELAY) state = "delay";
        else if (neighbor->ndm_state & NUD_PROBE) state = "probe";
        else if (neighbor->ndm_state & NUD_INCOMPLETE) state = "incomplete";
        result->push_back({address, hardware.str(),
                           neighbor->ndm_state & NUD_PERMANENT
                               ? "static" : "dynamic",
                           state, neighbor->ndm_family == AF_INET6,
                           (neighbor->ndm_flags & NTF_ROUTER) != 0});
      }
    }
  }

  bool Addresses(unsigned interface_index, std::vector<LiveAddress>* result,
                 std::string* error) {
    struct {
      nlmsghdr header;
      ifaddrmsg body;
    } request{};
    request.header.nlmsg_len = NLMSG_LENGTH(sizeof(ifaddrmsg));
    request.header.nlmsg_type = RTM_GETADDR;
    request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    request.header.nlmsg_seq = ++sequence_;
    request.body.ifa_family = AF_UNSPEC;
    sockaddr_nl kernel{};
    kernel.nl_family = AF_NETLINK;
    iovec part{&request, request.header.nlmsg_len};
    msghdr message{};
    message.msg_name = &kernel;
    message.msg_namelen = sizeof(kernel);
    message.msg_iov = &part;
    message.msg_iovlen = 1;
    if (sendmsg(descriptor_, &message, 0) < 0) {
      if (error) *error = "dump addresses: " +
                          std::string(std::strerror(errno));
      return false;
    }
    std::array<std::byte, 16384> response{};
    while (true) {
      const ssize_t received = recv(descriptor_, response.data(),
                                    response.size(), 0);
      if (received < 0 && errno == EINTR) continue;
      if (received <= 0) {
        if (error) *error = "dump addresses: " + std::string(
            received ? std::strerror(errno) : "netlink peer closed");
        return false;
      }
      int remaining = static_cast<int>(received);
      for (auto* header = reinterpret_cast<nlmsghdr*>(response.data());
           NLMSG_OK(header, remaining); header = NLMSG_NEXT(header, remaining)) {
        if (header->nlmsg_seq != sequence_) continue;
        if (header->nlmsg_flags & NLM_F_DUMP_INTR) {
          if (error) *error = "address dump was interrupted by a kernel change";
          return false;
        }
        if (header->nlmsg_type == NLMSG_DONE) return true;
        if (header->nlmsg_type == NLMSG_ERROR) {
          if (error) *error = "kernel rejected address dump";
          return false;
        }
        if (header->nlmsg_type != RTM_NEWADDR ||
            NLMSG_PAYLOAD(header, 0) < sizeof(ifaddrmsg))
          continue;
        const auto* address = reinterpret_cast<const ifaddrmsg*>(
            NLMSG_DATA(header));
        if (address->ifa_index != interface_index ||
            (address->ifa_family != AF_INET &&
             address->ifa_family != AF_INET6))
          continue;
        const void* binary = nullptr;
        const void* local = nullptr;
        std::size_t binary_size = 0;
        std::size_t local_size = 0;
        std::uint32_t flags = address->ifa_flags;
        int attribute_bytes = static_cast<int>(
            NLMSG_PAYLOAD(header, sizeof(ifaddrmsg)));
        for (auto* attribute = reinterpret_cast<rtattr*>(
                 reinterpret_cast<std::byte*>(const_cast<ifaddrmsg*>(address)) +
                 NLMSG_ALIGN(sizeof(ifaddrmsg)));
             RTA_OK(attribute, attribute_bytes);
             attribute = RTA_NEXT(attribute, attribute_bytes)) {
          if (attribute->rta_type == IFA_ADDRESS) {
            binary = RTA_DATA(attribute);
            binary_size = RTA_PAYLOAD(attribute);
          } else if (attribute->rta_type == IFA_LOCAL) {
            local = RTA_DATA(attribute);
            local_size = RTA_PAYLOAD(attribute);
          } else if (attribute->rta_type == IFA_FLAGS &&
                     RTA_PAYLOAD(attribute) == sizeof(flags)) {
            std::memcpy(&flags, RTA_DATA(attribute), sizeof(flags));
          }
        }
        const std::size_t expected = address->ifa_family == AF_INET
            ? sizeof(in_addr) : sizeof(in6_addr);
        if (local && local_size == expected) {
          binary = local;
          binary_size = local_size;
        }
        if (!binary || binary_size != expected) continue;
        char text[INET6_ADDRSTRLEN]{};
        if (!inet_ntop(address->ifa_family, binary, text, sizeof(text)))
          continue;
        std::string status = "preferred";
        if (flags & IFA_F_DADFAILED) status = "duplicate";
        else if (flags & IFA_F_OPTIMISTIC) status = "optimistic";
        else if (flags & IFA_F_TENTATIVE) status = "tentative";
        else if (flags & IFA_F_DEPRECATED) status = "deprecated";
        result->push_back({text, status, address->ifa_prefixlen,
                           address->ifa_family == AF_INET6});
      }
    }
  }

 private:
  int descriptor_ = -1;
  int error_ = 0;
  std::uint32_t sequence_ = 0;
};

bool InterfaceIndex(std::string_view name, unsigned* index,
                    std::string* error) {
  const std::string copied(name);
  *index = if_nametoindex(copied.c_str());
  if (*index) return true;
  if (error) *error = "cannot resolve interface " + copied + ": " +
                      std::strerror(errno);
  return false;
}

bool AddressRequest(RouteSocket* socket, std::string_view interface,
                    const AddressConfig& address, bool add,
                    std::string* error) {
  unsigned index = 0;
  if (!InterfaceIndex(interface, &index, error)) return false;
  std::array<std::byte, kMessageBytes> storage{};
  auto* header = reinterpret_cast<nlmsghdr*>(storage.data());
  header->nlmsg_len = NLMSG_LENGTH(sizeof(ifaddrmsg));
  header->nlmsg_type = add ? RTM_NEWADDR : RTM_DELADDR;
  header->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK |
      (add ? NLM_F_CREATE | NLM_F_EXCL : 0);
  auto* body = reinterpret_cast<ifaddrmsg*>(NLMSG_DATA(header));
  body->ifa_family = address.ipv6 ? AF_INET6 : AF_INET;
  body->ifa_prefixlen = static_cast<unsigned char>(address.prefix_length);
  body->ifa_scope = RT_SCOPE_UNIVERSE;
  body->ifa_index = index;
  std::array<std::byte, sizeof(in6_addr)> binary{};
  const int family = address.ipv6 ? AF_INET6 : AF_INET;
  if (inet_pton(family, address.address.c_str(), binary.data()) != 1) {
    if (error) *error = "invalid address " + address.address;
    return false;
  }
  const std::size_t size = address.ipv6 ? sizeof(in6_addr) : sizeof(in_addr);
  if (!AddAttribute(header, storage.size(), IFA_LOCAL, binary.data(), size) ||
      !AddAttribute(header, storage.size(), IFA_ADDRESS, binary.data(), size)) {
    if (error) *error = "rtnetlink address request is too large";
    return false;
  }
  return socket->Request(header, add ? "add address" : "delete address", error);
}

std::optional<std::array<std::byte, 6>> EthernetAddress(
    std::string_view text) {
  std::array<std::byte, 6> result{};
  unsigned values[6]{};
  char trailing = 0;
  const std::string copied(text);
  if (std::sscanf(copied.c_str(), "%x:%x:%x:%x:%x:%x%c", &values[0],
                  &values[1], &values[2], &values[3], &values[4], &values[5],
                  &trailing) != 6)
    return std::nullopt;
  for (std::size_t index = 0; index < result.size(); ++index) {
    if (values[index] > 255) return std::nullopt;
    result[index] = static_cast<std::byte>(values[index]);
  }
  return result;
}

bool NeighborRequest(RouteSocket* socket, std::string_view interface,
                     const NeighborConfig& neighbor, bool add,
                     std::string* error) {
  unsigned index = 0;
  if (!InterfaceIndex(interface, &index, error)) return false;
  const auto link_layer = EthernetAddress(neighbor.link_layer_address);
  if (!link_layer) {
    if (error) *error = "unsupported link-layer address " +
                        neighbor.link_layer_address;
    return false;
  }
  std::array<std::byte, kMessageBytes> storage{};
  auto* header = reinterpret_cast<nlmsghdr*>(storage.data());
  header->nlmsg_len = NLMSG_LENGTH(sizeof(ndmsg));
  header->nlmsg_type = add ? RTM_NEWNEIGH : RTM_DELNEIGH;
  header->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK |
      (add ? NLM_F_CREATE | NLM_F_REPLACE : 0);
  auto* body = reinterpret_cast<ndmsg*>(NLMSG_DATA(header));
  body->ndm_family = neighbor.ipv6 ? AF_INET6 : AF_INET;
  body->ndm_ifindex = static_cast<int>(index);
  body->ndm_state = NUD_PERMANENT;
  std::array<std::byte, sizeof(in6_addr)> destination{};
  const int family = neighbor.ipv6 ? AF_INET6 : AF_INET;
  if (inet_pton(family, neighbor.address.c_str(), destination.data()) != 1) {
    if (error) *error = "invalid neighbor address " + neighbor.address;
    return false;
  }
  const std::size_t size = neighbor.ipv6 ? sizeof(in6_addr) : sizeof(in_addr);
  if (!AddAttribute(header, storage.size(), NDA_DST, destination.data(), size) ||
      (add && !AddAttribute(header, storage.size(), NDA_LLADDR,
                            link_layer->data(), link_layer->size()))) {
    if (error) *error = "rtnetlink neighbor request is too large";
    return false;
  }
  return socket->Request(header, add ? "add neighbor" : "delete neighbor",
                         error);
}

bool LinkRequest(RouteSocket* socket, std::string_view interface,
                 std::optional<bool> enabled, std::optional<unsigned> mtu,
                 std::string* error) {
  unsigned index = 0;
  if (!InterfaceIndex(interface, &index, error)) return false;
  std::array<std::byte, kMessageBytes> storage{};
  auto* header = reinterpret_cast<nlmsghdr*>(storage.data());
  header->nlmsg_len = NLMSG_LENGTH(sizeof(ifinfomsg));
  header->nlmsg_type = RTM_NEWLINK;
  header->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
  auto* body = reinterpret_cast<ifinfomsg*>(NLMSG_DATA(header));
  body->ifi_family = AF_UNSPEC;
  body->ifi_index = static_cast<int>(index);
  if (enabled) {
    body->ifi_change = IFF_UP;
    if (*enabled) body->ifi_flags = IFF_UP;
  }
  if (mtu && !AddAttribute(header, storage.size(), IFLA_MTU, &*mtu,
                           sizeof(*mtu))) {
    if (error) *error = "rtnetlink link request is too large";
    return false;
  }
  return socket->Request(header, "set interface attributes", error);
}

struct LinkSnapshot {
  bool enabled = false;
  bool running = false;
  unsigned mtu = 0;
};

std::optional<LinkSnapshot> ReadLink(std::string_view interface,
                                     std::string* error) {
  const int descriptor = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (descriptor < 0) {
    if (error) *error = "cannot open interface ioctl socket: " +
                        std::string(std::strerror(errno));
    return std::nullopt;
  }
  ifreq request{};
  const std::string copied(interface);
  if (copied.size() >= sizeof(request.ifr_name)) {
    close(descriptor);
    if (error) *error = "interface name exceeds the Linux kernel limit";
    return std::nullopt;
  }
  std::memcpy(request.ifr_name, copied.c_str(), copied.size() + 1);
  if (ioctl(descriptor, SIOCGIFFLAGS, &request) != 0) {
    if (error) *error = "cannot read interface flags for " + copied + ": " +
                        std::strerror(errno);
    close(descriptor);
    return std::nullopt;
  }
  const bool enabled = (request.ifr_flags & IFF_UP) != 0;
  const bool running = (request.ifr_flags & IFF_RUNNING) != 0;
  if (ioctl(descriptor, SIOCGIFMTU, &request) != 0) {
    if (error) *error = "cannot read interface MTU for " + copied + ": " +
                        std::strerror(errno);
    close(descriptor);
    return std::nullopt;
  }
  const unsigned mtu = static_cast<unsigned>(request.ifr_mtu);
  close(descriptor);
  return LinkSnapshot{enabled, running, mtu};
}

std::string_view LocalName(std::string_view name) {
  const std::size_t colon = name.find(':');
  return colon == std::string_view::npos ? name : name.substr(colon + 1);
}

pugi::xml_node Child(const pugi::xml_node& parent, std::string_view local) {
  for (const pugi::xml_node child : parent.children())
    if (LocalName(child.name()) == local) return child;
  return {};
}

unsigned PrefixLength(const sockaddr* mask) {
  if (!mask) return 0;
  const std::byte* bytes = nullptr;
  std::size_t size = 0;
  if (mask->sa_family == AF_INET) {
    bytes = reinterpret_cast<const std::byte*>(
        &reinterpret_cast<const sockaddr_in*>(mask)->sin_addr);
    size = sizeof(in_addr);
  } else if (mask->sa_family == AF_INET6) {
    bytes = reinterpret_cast<const std::byte*>(
        &reinterpret_cast<const sockaddr_in6*>(mask)->sin6_addr);
    size = sizeof(in6_addr);
  }
  unsigned bits = 0;
  for (std::size_t index = 0; index < size; ++index)
    bits += static_cast<unsigned>(__builtin_popcount(
        std::to_integer<unsigned char>(bytes[index])));
  return bits;
}

bool AppendAddresses(const std::string& name, pugi::xml_node entry,
                     unsigned mtu, std::string* error) {
  unsigned interface_index = 0;
  if (!InterfaceIndex(name, &interface_index, error)) return false;
  RouteSocket socket;
  if (!socket.valid(error)) return false;
  std::vector<LiveAddress> addresses;
  if (!socket.Addresses(interface_index, &addresses, error)) return false;
  pugi::xml_node ipv4;
  pugi::xml_node ipv6;
  for (const LiveAddress& value : addresses) {
    pugi::xml_node* family_node = value.ipv6 ? &ipv6 : &ipv4;
    if (!*family_node) {
      *family_node = entry.append_child(value.ipv6 ? "ipv6" : "ipv4");
      // RFC 8344 deliberately gives IPv4 MTU a uint16 representation. Linux
      // loopback commonly reports 65536, so omit that unrepresentable value
      // instead of publishing invalid instance data.
      if (value.ipv6 || mtu <= 65535)
        family_node->append_child("mtu").text() = mtu;
    }
    pugi::xml_node address = family_node->append_child("address");
    address.append_child("ip").text() = value.address.c_str();
    address.append_child("prefix-length").text() = value.prefix_length;
    address.append_child("origin").text() = "other";
    address.append_child("status").text() = value.status.c_str();
  }
  return true;
}

bool AppendNeighbors(const std::string& name, pugi::xml_node entry,
                     std::string* error) {
  unsigned interface_index = 0;
  if (!InterfaceIndex(name, &interface_index, error)) return false;
  RouteSocket socket;
  if (!socket.valid(error)) return false;
  std::vector<LiveNeighbor> neighbors;
  if (!socket.Neighbors(interface_index, &neighbors, error)) return false;
  pugi::xml_node ipv4 = Child(entry, "ipv4");
  pugi::xml_node ipv6 = Child(entry, "ipv6");
  for (const LiveNeighbor& value : neighbors) {
    pugi::xml_node* family = value.ipv6 ? &ipv6 : &ipv4;
    if (!*family) *family = entry.append_child(value.ipv6 ? "ipv6" : "ipv4");
    pugi::xml_node neighbor = family->append_child("neighbor");
    neighbor.append_child("ip").text() = value.address.c_str();
    neighbor.append_child("link-layer-address").text() =
        value.link_layer_address.c_str();
    neighbor.append_child("origin").text() = value.origin.c_str();
    if (value.ipv6) {
      if (value.router) neighbor.append_child("is-router");
      if (!value.state.empty())
        neighbor.append_child("state").text() = value.state.c_str();
    }
  }
  return true;
}

std::string InterfaceType(const std::string& name) {
  const int descriptor = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (descriptor < 0) return "iana-if-type:other";
  ifreq request{};
  if (name.size() >= sizeof(request.ifr_name)) {
    close(descriptor);
    return "iana-if-type:other";
  }
  std::memcpy(request.ifr_name, name.c_str(), name.size() + 1);
  const bool read = ioctl(descriptor, SIOCGIFHWADDR, &request) == 0;
  close(descriptor);
  if (!read) return "iana-if-type:other";
  if (request.ifr_hwaddr.sa_family == ARPHRD_LOOPBACK)
    return "iana-if-type:softwareLoopback";
  if (request.ifr_hwaddr.sa_family == ARPHRD_ETHER)
    return "iana-if-type:ethernetCsmacd";
  return "iana-if-type:other";
}

std::string BootTime() {
  timespec boot_elapsed{};
  timespec now{};
  if (clock_gettime(CLOCK_BOOTTIME, &boot_elapsed) != 0 ||
      clock_gettime(CLOCK_REALTIME, &now) != 0)
    return "1970-01-01T00:00:00Z";
  const std::time_t boot = now.tv_sec - boot_elapsed.tv_sec;
  std::tm utc{};
  if (!gmtime_r(&boot, &utc)) return "1970-01-01T00:00:00Z";
  char text[32]{};
  if (!std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc))
    return "1970-01-01T00:00:00Z";
  return text;
}

bool AppendStatistics(const std::string& name, pugi::xml_node entry,
                      std::string* error) {
  ifaddrs* values = nullptr;
  if (getifaddrs(&values) != 0) {
    if (error) *error = "cannot enumerate Linux interface statistics: " +
                        std::string(std::strerror(errno));
    return false;
  }
  const rtnl_link_stats* counters = nullptr;
  for (const ifaddrs* value = values; value; value = value->ifa_next) {
    if (value->ifa_addr && value->ifa_addr->sa_family == AF_PACKET &&
        name == value->ifa_name && value->ifa_data) {
      counters = static_cast<const rtnl_link_stats*>(value->ifa_data);
      break;
    }
  }
  if (counters) {
    pugi::xml_node statistics = entry.append_child("statistics");
    statistics.append_child("discontinuity-time").text() = BootTime().c_str();
    statistics.append_child("in-octets").text() = counters->rx_bytes;
    statistics.append_child("in-unicast-pkts").text() = counters->rx_packets;
    statistics.append_child("in-discards").text() = counters->rx_dropped;
    statistics.append_child("in-errors").text() = counters->rx_errors;
    statistics.append_child("out-octets").text() = counters->tx_bytes;
    statistics.append_child("out-unicast-pkts").text() = counters->tx_packets;
    statistics.append_child("out-discards").text() = counters->tx_dropped;
    statistics.append_child("out-errors").text() = counters->tx_errors;
  }
  freeifaddrs(values);
  return true;
}

struct KernelInterfaceState {
  std::vector<AddressConfig> addresses;
  std::vector<NeighborConfig> neighbors;
};

bool ReadKernelState(const std::string& name, KernelInterfaceState* state,
                     std::string* error) {
  ifaddrs* values = nullptr;
  if (getifaddrs(&values) != 0) {
    if (error) *error = "cannot enumerate Linux interface addresses: " +
                        std::string(std::strerror(errno));
    return false;
  }
  for (const ifaddrs* value = values; value; value = value->ifa_next) {
    if (!value->ifa_addr || name != value->ifa_name) continue;
    const int family = value->ifa_addr->sa_family;
    if (family != AF_INET && family != AF_INET6) continue;
    char text[INET6_ADDRSTRLEN]{};
    const void* binary = family == AF_INET
        ? static_cast<const void*>(&reinterpret_cast<const sockaddr_in*>(
              value->ifa_addr)->sin_addr)
        : static_cast<const void*>(&reinterpret_cast<const sockaddr_in6*>(
              value->ifa_addr)->sin6_addr);
    if (inet_ntop(family, binary, text, sizeof(text)))
      state->addresses.push_back(
          {text, PrefixLength(value->ifa_netmask), family == AF_INET6});
  }
  freeifaddrs(values);
  unsigned interface_index = 0;
  if (!InterfaceIndex(name, &interface_index, error)) return false;
  RouteSocket socket;
  if (!socket.valid(error)) return false;
  std::vector<LiveNeighbor> live_neighbors;
  if (!socket.Neighbors(interface_index, &live_neighbors, error)) return false;
  for (const LiveNeighbor& value : live_neighbors)
    state->neighbors.push_back(
        {value.address, value.link_layer_address, value.ipv6});
  return true;
}

template <typename Value>
const Value* FindByAddress(const std::vector<Value>& values,
                           const Value& target) {
  const auto found = std::ranges::find_if(values, [&](const Value& value) {
    return value.ipv6 == target.ipv6 && value.address == target.address;
  });
  return found == values.end() ? nullptr : &*found;
}

std::optional<unsigned> Mtu(const InterfaceConfig& interface,
                            std::string* error) {
  if (interface.ipv4_mtu && interface.ipv6_mtu &&
      interface.ipv4_mtu != interface.ipv6_mtu) {
    if (error) *error = "Linux requires equal IPv4 and IPv6 link MTUs on " +
                        interface.name;
    return std::nullopt;
  }
  return interface.ipv4_mtu ? interface.ipv4_mtu : interface.ipv6_mtu;
}

struct Operation {
  enum class Kind { kAddress, kNeighbor, kLink } kind;
  std::string interface;
  std::optional<AddressConfig> address;
  std::optional<NeighborConfig> neighbor;
  std::optional<bool> enabled;
  std::optional<unsigned> mtu;
  bool add = false;

  bool Run(RouteSocket* socket, std::string* error) const {
    if (kind == Kind::kAddress)
      return AddressRequest(socket, interface, *address, add, error);
    if (kind == Kind::kNeighbor)
      return NeighborRequest(socket, interface, *neighbor, add, error);
    return LinkRequest(socket, interface, enabled, mtu, error);
  }
};

class LinuxBackend final : public PlatformBackend {
 public:
  bool Reconcile(std::string_view before_xml, std::string_view desired_xml,
                 std::string* error) override {
    if (before_xml == rollback_before_ && desired_xml == rollback_desired_) {
      const bool restored = RunOperations(rollback_operations_, error);
      if (restored) Commit();
      return restored;
    }
    std::vector<InterfaceConfig> before;
    std::vector<InterfaceConfig> desired;
    if (!ParsePlatformConfig(before_xml, &before, error) ||
        !ParsePlatformConfig(desired_xml, &desired, error))
      return false;
    std::vector<std::pair<std::string, KernelInterfaceState>> kernel_states;
    for (const InterfaceConfig& interface : desired) {
      KernelInterfaceState state;
      if (!ReadKernelState(interface.name, &state, error)) return false;
      kernel_states.push_back({interface.name, std::move(state)});
    }
    for (const InterfaceConfig& interface : before) {
      if (std::ranges::find(kernel_states, interface.name,
                            &decltype(kernel_states)::value_type::first) !=
          kernel_states.end())
        continue;
      KernelInterfaceState state;
      if (!ReadKernelState(interface.name, &state, error)) return false;
      kernel_states.push_back({interface.name, std::move(state)});
    }
    const auto kernel = [&](const std::string& name)
        -> const KernelInterfaceState& {
      return std::ranges::find(kernel_states, name,
                               &decltype(kernel_states)::value_type::first)
          ->second;
    };
    std::vector<std::pair<Operation, Operation>> operations;
    for (const InterfaceConfig& old_interface : before) {
      const InterfaceConfig* replacement = Find(desired, old_interface.name);
      for (const NeighborConfig& value : old_interface.neighbors)
        if (!replacement || std::ranges::find(replacement->neighbors, value) ==
                                replacement->neighbors.end()) {
          const NeighborConfig* observed =
              FindByAddress(kernel(old_interface.name).neighbors, value);
          if (observed)
            operations.push_back(
                {{Operation::Kind::kNeighbor, old_interface.name, {}, *observed,
                  {}, {}, false},
                 {Operation::Kind::kNeighbor, old_interface.name, {}, *observed,
                  {}, {}, true}});
        }
      for (const AddressConfig& value : old_interface.addresses)
        if (!replacement || std::ranges::find(replacement->addresses, value) ==
                                replacement->addresses.end()) {
          const AddressConfig* observed =
              FindByAddress(kernel(old_interface.name).addresses, value);
          if (observed)
            operations.push_back(
                {{Operation::Kind::kAddress, old_interface.name, *observed, {},
                  {}, {}, false},
                 {Operation::Kind::kAddress, old_interface.name, *observed, {},
                  {}, {}, true}});
        }
    }
    for (const InterfaceConfig& interface : desired) {
      const InterfaceConfig* old = Find(before, interface.name);
      for (const AddressConfig& value : interface.addresses) {
        const AddressConfig* observed =
            FindByAddress(kernel(interface.name).addresses, value);
        const bool old_will_remove = old && FindByAddress(old->addresses, value) &&
            std::ranges::find(old->addresses, value) == old->addresses.end();
        if (observed && *observed != value && !old_will_remove)
          operations.push_back({{Operation::Kind::kAddress, interface.name,
                                 *observed, {}, {}, {}, false},
                                {Operation::Kind::kAddress, interface.name,
                                 *observed, {}, {}, {}, true}});
        if (!observed || *observed != value)
          operations.push_back({{Operation::Kind::kAddress, interface.name,
                                 value, {}, {}, {}, true},
                                {Operation::Kind::kAddress, interface.name,
                                 value, {}, {}, {}, false}});
      }
      for (const NeighborConfig& value : interface.neighbors) {
        const NeighborConfig* observed =
            FindByAddress(kernel(interface.name).neighbors, value);
        if (!observed || *observed != value)
          operations.push_back({{Operation::Kind::kNeighbor, interface.name,
                                 {}, value, {}, {}, true},
                                {Operation::Kind::kNeighbor, interface.name,
                                 {}, observed ? *observed : value, {}, {},
                                 observed != nullptr}});
      }
      const auto desired_mtu = Mtu(interface, error);
      if ((interface.ipv4_mtu || interface.ipv6_mtu) && !desired_mtu)
        return false;
      std::optional<LinkSnapshot> snapshot;
      if (interface.enabled || desired_mtu)
        snapshot = ReadLink(interface.name, error);
      if ((interface.enabled || desired_mtu) && !snapshot) return false;
      const bool enabled_changed = interface.enabled &&
          snapshot->enabled != *interface.enabled;
      const bool mtu_changed = desired_mtu && snapshot->mtu != *desired_mtu;
      if (enabled_changed || mtu_changed)
        operations.push_back(
            {{Operation::Kind::kLink, interface.name, {}, {},
              enabled_changed ? interface.enabled : std::nullopt,
              mtu_changed ? desired_mtu : std::nullopt, false},
             {Operation::Kind::kLink, interface.name, {}, {},
              enabled_changed ? std::optional(snapshot->enabled) : std::nullopt,
              mtu_changed ? std::optional(snapshot->mtu) : std::nullopt, false}});
    }
    if (!RunOperations(operations, error)) return false;
    rollback_before_ = std::string(desired_xml);
    rollback_desired_ = std::string(before_xml);
    rollback_operations_.clear();
    rollback_operations_.reserve(operations.size());
    for (auto operation = operations.rbegin(); operation != operations.rend();
         ++operation)
      rollback_operations_.push_back({operation->second, operation->first});
    return true;
  }

  void Commit() override {
    rollback_before_.clear();
    rollback_desired_.clear();
    rollback_operations_.clear();
  }

  bool OperationalXml(std::string_view configuration_xml, std::string* output,
                      std::string* error) override {
    pugi::xml_document configuration;
    const pugi::xml_parse_result parsed = configuration.load_buffer(
        configuration_xml.data(), configuration_xml.size());
    if (!parsed) {
      if (error) *error = "cannot parse applied configuration for live state";
      return false;
    }
    pugi::xml_document state;
    pugi::xml_node root = state.append_child("interfaces-state");
    root.append_attribute("xmlns") =
        "urn:ietf:params:xml:ns:yang:ietf-interfaces";
    root.append_attribute("xmlns:iana-if-type") =
        "urn:ietf:params:xml:ns:yang:iana-if-type";
    std::vector<std::pair<std::string, std::string>> configured_types;
    for (const pugi::xml_node top : configuration.document_element().children()) {
      if (LocalName(top.name()) != "interfaces") continue;
      for (const pugi::xml_node configured : top.children()) {
        if (LocalName(configured.name()) != "interface") continue;
        const pugi::xml_node name_node = Child(configured, "name");
        const pugi::xml_node type_node = Child(configured, "type");
        if (name_node && type_node)
          configured_types.push_back(
              {name_node.text().as_string(), type_node.text().as_string()});
      }
    }
    struct if_nameindex* interfaces = if_nameindex();
    if (!interfaces) {
      if (error) *error = "cannot enumerate Linux interfaces: " +
                          std::string(std::strerror(errno));
      return false;
    }
    for (const struct if_nameindex* interface = interfaces;
         interface->if_index != 0 && interface->if_name; ++interface) {
      const std::string name = interface->if_name;
      const auto link = ReadLink(name, error);
      if (!link) {
        if_freenameindex(interfaces);
        return false;
      }
      const auto configured = std::ranges::find(
          configured_types, name, &decltype(configured_types)::value_type::first);
      const std::string type = configured == configured_types.end()
          ? InterfaceType(name) : configured->second;
      pugi::xml_node entry = root.append_child("interface");
      entry.append_child("name").text() = name.c_str();
      entry.append_child("type").text() = type.c_str();
      entry.append_child("admin-status").text() =
          link->enabled ? "up" : "down";
      entry.append_child("oper-status").text() =
          link->running ? "up" : (link->enabled ? "dormant" : "down");
      if (!AppendAddresses(name, entry, link->mtu, error) ||
          !AppendNeighbors(name, entry, error) ||
          !AppendStatistics(name, entry, error)) {
        if_freenameindex(interfaces);
        return false;
      }
    }
    if_freenameindex(interfaces);
    std::ostringstream serialized;
    state.print(serialized, "", pugi::format_raw);
    *output = serialized.str();
    return true;
  }

 private:
  static bool RunOperations(
      const std::vector<std::pair<Operation, Operation>>& operations,
      std::string* error) {
    RouteSocket socket;
    if (!socket.valid(error)) return false;
    std::vector<std::size_t> completed;
    for (std::size_t index = 0; index < operations.size(); ++index) {
      if (operations[index].first.Run(&socket, error)) {
        completed.push_back(index);
        continue;
      }
      const std::string failure = error ? *error : "rtnetlink operation failed";
      std::vector<std::string> rollback_failures;
      for (auto rollback = completed.rbegin(); rollback != completed.rend();
           ++rollback) {
        std::string rollback_error;
        if (!operations[*rollback].second.Run(&socket, &rollback_error))
          rollback_failures.push_back(std::move(rollback_error));
      }
      if (error) {
        *error = failure;
        if (!rollback_failures.empty()) {
          *error += "; rollback incomplete: ";
          for (std::size_t failure_index = 0;
               failure_index < rollback_failures.size(); ++failure_index) {
            if (failure_index) *error += ", ";
            *error += rollback_failures[failure_index];
          }
        }
      }
      return false;
    }
    return true;
  }

  std::string rollback_before_;
  std::string rollback_desired_;
  std::vector<std::pair<Operation, Operation>> rollback_operations_;
};

}  // namespace

std::unique_ptr<PlatformBackend> MakePlatformBackend() {
  return std::make_unique<LinuxBackend>();
}

}  // namespace dangd::ip_management

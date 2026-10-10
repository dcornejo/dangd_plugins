// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "plugins/ip_management/src/platform_backend.h"
#include "plugins/ip_management/src/platform_config.h"
#include "plugins/ip_management/src/freebsd/address_status.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
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
#include <net/if.h>
#include <net/if_types.h>
#include <netlink/netlink.h>
#include <netlink/route/common.h>
#include <netlink/route/neigh.h>
#include <netinet/in.h>
#include <netinet6/in6_var.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <pugixml.hpp>

namespace dangd::ip_management {
namespace {

const InterfaceConfig* Find(const std::vector<InterfaceConfig>& values,
                            const std::string& name) {
  const auto found = std::ranges::find(values, name, &InterfaceConfig::name);
  return found == values.end() ? nullptr : &*found;
}

bool CopyName(std::string_view name, char (&destination)[IFNAMSIZ],
              std::string* error) {
  if (name.empty() || name.size() >= sizeof(destination)) {
    if (error) *error = "interface name exceeds the FreeBSD kernel limit";
    return false;
  }
  std::memcpy(destination, name.data(), name.size());
  destination[name.size()] = '\0';
  return true;
}

class IoctlSocket {
 public:
  explicit IoctlSocket(int family) {
    descriptor_ = socket(family, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (descriptor_ < 0) error_ = errno;
  }
  ~IoctlSocket() {
    if (descriptor_ >= 0) close(descriptor_);
  }
  IoctlSocket(const IoctlSocket&) = delete;
  IoctlSocket& operator=(const IoctlSocket&) = delete;

  bool valid(std::string* error) const {
    if (descriptor_ >= 0) return true;
    if (error) *error = "cannot open interface ioctl socket: " +
                        std::string(std::strerror(error_));
    return false;
  }

  bool Call(unsigned long request, void* argument,
            std::string_view description, std::string* error) const {
    if (ioctl(descriptor_, request, argument) == 0) return true;
    if (error) *error = std::string(description) + ": " +
                        std::strerror(errno);
    return false;
  }

 private:
  int descriptor_ = -1;
  int error_ = 0;
};

sockaddr_in Ipv4Address(std::string_view text, std::string* error) {
  sockaddr_in result{};
  result.sin_len = sizeof(result);
  result.sin_family = AF_INET;
  const std::string copied(text);
  if (inet_pton(AF_INET, copied.c_str(), &result.sin_addr) != 1 && error)
    *error = "invalid IPv4 address " + copied;
  return result;
}

sockaddr_in Ipv4Mask(unsigned prefix) {
  sockaddr_in result{};
  result.sin_len = sizeof(result);
  result.sin_family = AF_INET;
  const std::uint32_t bits = prefix == 0 ? 0 :
      UINT32_MAX << static_cast<unsigned>(32 - prefix);
  result.sin_addr.s_addr = htonl(bits);
  return result;
}

sockaddr_in6 Ipv6Address(std::string_view text, std::string* error) {
  sockaddr_in6 result{};
  result.sin6_len = sizeof(result);
  result.sin6_family = AF_INET6;
  const std::string copied(text);
  if (inet_pton(AF_INET6, copied.c_str(), &result.sin6_addr) != 1 && error)
    *error = "invalid IPv6 address " + copied;
  return result;
}

sockaddr_in6 Ipv6Mask(unsigned prefix) {
  sockaddr_in6 result{};
  result.sin6_len = sizeof(result);
  result.sin6_family = AF_INET6;
  unsigned remaining = prefix;
  for (std::uint8_t& byte : result.sin6_addr.s6_addr) {
    if (remaining >= 8) {
      byte = UINT8_MAX;
      remaining -= 8;
    } else if (remaining != 0) {
      byte = static_cast<std::uint8_t>(UINT8_MAX << (8 - remaining));
      remaining = 0;
    }
  }
  return result;
}

bool AddressRequest(std::string_view interface, const AddressConfig& address,
                    bool add, std::string* error) {
  IoctlSocket socket(address.ipv6 ? AF_INET6 : AF_INET);
  if (!socket.valid(error)) return false;
  const std::string target = std::string(interface);
  if (address.ipv6) {
    if (add) {
      in6_aliasreq request{};
      if (!CopyName(interface, request.ifra_name, error)) return false;
      request.ifra_addr = Ipv6Address(address.address, error);
      request.ifra_prefixmask = Ipv6Mask(address.prefix_length);
      request.ifra_lifetime.ia6t_vltime = UINT32_MAX;
      request.ifra_lifetime.ia6t_pltime = UINT32_MAX;
      return socket.Call(SIOCAIFADDR_IN6, &request,
                         "add IPv6 address on " + target, error);
    }
    in6_ifreq request{};
    if (!CopyName(interface, request.ifr_name, error)) return false;
    request.ifr_addr = Ipv6Address(address.address, error);
    return socket.Call(SIOCDIFADDR_IN6, &request,
                       "delete IPv6 address on " + target, error);
  }
  if (add) {
    ifaliasreq request{};
    if (!CopyName(interface, request.ifra_name, error)) return false;
    const sockaddr_in value = Ipv4Address(address.address, error);
    const sockaddr_in mask = Ipv4Mask(address.prefix_length);
    std::memcpy(&request.ifra_addr, &value, sizeof(value));
    std::memcpy(&request.ifra_mask, &mask, sizeof(mask));
    return socket.Call(SIOCAIFADDR, &request,
                       "add IPv4 address on " + target, error);
  }
  ifreq request{};
  if (!CopyName(interface, request.ifr_name, error)) return false;
  const sockaddr_in value = Ipv4Address(address.address, error);
  std::memcpy(&request.ifr_addr, &value, sizeof(value));
  return socket.Call(SIOCDIFADDR, &request,
                     "delete IPv4 address on " + target, error);
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

bool AddNetlinkAttribute(nlmsghdr* header, std::size_t capacity,
                         std::uint16_t type, const void* data,
                         std::size_t size) {
  const std::size_t attribute_size = NLA_HDRLEN + size;
  const std::size_t offset = NLMSG_ALIGN(header->nlmsg_len);
  if (offset + NLA_ALIGN(attribute_size) > capacity) return false;
  auto* attribute = reinterpret_cast<nlattr*>(
      reinterpret_cast<std::byte*>(header) + offset);
  attribute->nla_type = type;
  attribute->nla_len = static_cast<std::uint16_t>(attribute_size);
  if (size) std::memcpy(attribute + 1, data, size);
  header->nlmsg_len = static_cast<std::uint32_t>(
      offset + NLA_ALIGN(attribute_size));
  return true;
}

bool NeighborRequest(std::string_view interface,
                     const NeighborConfig& neighbor, bool add,
                     std::string* error) {
  const std::string target(interface);
  const unsigned index = if_nametoindex(target.c_str());
  if (index == 0) {
    if (error) *error = "resolve interface " + target + ": " +
                        std::strerror(errno);
    return false;
  }
  const auto link_layer = EthernetAddress(neighbor.link_layer_address);
  if (!link_layer) {
    if (error) *error = "unsupported link-layer address " +
                        neighbor.link_layer_address + " on " + target;
    return false;
  }
  std::array<std::byte, sizeof(in6_addr)> destination{};
  const int family = neighbor.ipv6 ? AF_INET6 : AF_INET;
  if (inet_pton(family, neighbor.address.c_str(), destination.data()) != 1) {
    if (error) *error = "invalid neighbor address " + neighbor.address +
                        " on " + target;
    return false;
  }

  const int descriptor = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC,
                                NETLINK_ROUTE);
  if (descriptor < 0) {
    if (error) *error = "open FreeBSD route netlink socket: " +
                        std::string(std::strerror(errno));
    return false;
  }
  sockaddr_nl local{};
  local.nl_len = sizeof(local);
  local.nl_family = AF_NETLINK;
  if (bind(descriptor, reinterpret_cast<sockaddr*>(&local), sizeof(local))) {
    if (error) *error = "bind FreeBSD route netlink socket: " +
                        std::string(std::strerror(errno));
    close(descriptor);
    return false;
  }
  std::array<std::byte, 1024> request{};
  auto* header = reinterpret_cast<nlmsghdr*>(request.data());
  header->nlmsg_len = NLMSG_LENGTH(sizeof(ndmsg));
  header->nlmsg_type = add ? RTM_NEWNEIGH : RTM_DELNEIGH;
  header->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK |
      (add ? NLM_F_CREATE | NLM_F_REPLACE : 0);
  header->nlmsg_seq = 1;
  auto* body = reinterpret_cast<ndmsg*>(NLMSG_DATA(header));
  body->ndm_family = static_cast<std::uint8_t>(family);
  body->ndm_ifindex = static_cast<std::int32_t>(index);
  body->ndm_state = NUD_PERMANENT;
  const std::size_t destination_size = neighbor.ipv6 ? sizeof(in6_addr) :
                                                        sizeof(in_addr);
  const bool attributes_added =
      AddNetlinkAttribute(header, request.size(), NDA_DST, destination.data(),
                          destination_size) &&
      (!add || AddNetlinkAttribute(header, request.size(), NDA_LLADDR,
                                    link_layer->data(), link_layer->size()));
  sockaddr_nl kernel{};
  kernel.nl_len = sizeof(kernel);
  kernel.nl_family = AF_NETLINK;
  if (!attributes_added ||
      sendto(descriptor, header, header->nlmsg_len, 0,
             reinterpret_cast<sockaddr*>(&kernel), sizeof(kernel)) < 0) {
    if (error) *error = "send FreeBSD neighbor request for " + neighbor.address +
                        " on " + target + ": " + std::strerror(errno);
    close(descriptor);
    return false;
  }
  std::array<std::byte, 4096> response{};
  ssize_t received = -1;
  do received = recv(descriptor, response.data(), response.size(), 0);
  while (received < 0 && errno == EINTR);
  const int receive_error = errno;
  close(descriptor);
  if (received < static_cast<ssize_t>(NLMSG_LENGTH(sizeof(nlmsgerr)))) {
    if (error) *error = std::string(add ? "add" : "delete") + " neighbor " +
        neighbor.address + " on " + target + ": " +
        (received < 0 ? std::strerror(receive_error) :
                        "truncated netlink reply");
    return false;
  }
  const auto* reply_header =
      reinterpret_cast<const nlmsghdr*>(response.data());
  if (reply_header->nlmsg_type != NLMSG_ERROR ||
      reply_header->nlmsg_seq != header->nlmsg_seq) {
    if (error) *error = "unexpected FreeBSD neighbor acknowledgement for " +
                        neighbor.address + " on " + target;
    return false;
  }
  const auto* reply =
      reinterpret_cast<const nlmsgerr*>(NLMSG_DATA(reply_header));
  if (reply->error == 0) return true;
  if (error) *error = std::string(add ? "add" : "delete") + " neighbor " +
      neighbor.address + " on " + target + ": " +
      std::strerror(reply->error);
  return false;
}

struct LiveNeighbor {
  std::string address;
  std::string link_layer_address;
  std::string origin;
  std::string state;
  bool ipv6 = false;
  bool router = false;
};

bool DumpNeighbors(unsigned interface_index, std::vector<LiveNeighbor>* result,
                   std::string* error) {
  const int descriptor = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC,
                                NETLINK_ROUTE);
  if (descriptor < 0) {
    if (error) *error = "open FreeBSD route netlink socket: " +
                        std::string(std::strerror(errno));
    return false;
  }
  sockaddr_nl local{};
  local.nl_len = sizeof(local);
  local.nl_family = AF_NETLINK;
  if (bind(descriptor, reinterpret_cast<sockaddr*>(&local), sizeof(local))) {
    if (error) *error = "bind FreeBSD route netlink socket: " +
                        std::string(std::strerror(errno));
    close(descriptor);
    return false;
  }
  struct {
    nlmsghdr header;
    ndmsg body;
  } request{};
  request.header.nlmsg_len = NLMSG_LENGTH(sizeof(ndmsg));
  request.header.nlmsg_type = RTM_GETNEIGH;
  request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
  request.header.nlmsg_seq = 1;
  request.body.ndm_family = AF_UNSPEC;
  sockaddr_nl kernel{};
  kernel.nl_len = sizeof(kernel);
  kernel.nl_family = AF_NETLINK;
  if (sendto(descriptor, &request, request.header.nlmsg_len, 0,
             reinterpret_cast<sockaddr*>(&kernel), sizeof(kernel)) < 0) {
    if (error) *error = "dump FreeBSD neighbors: " +
                        std::string(std::strerror(errno));
    close(descriptor);
    return false;
  }
  std::array<std::byte, 16384> response{};
  while (true) {
    const ssize_t received = recv(descriptor, response.data(), response.size(), 0);
    if (received < 0 && errno == EINTR) continue;
    if (received <= 0) {
      if (error) *error = "dump FreeBSD neighbors: " + std::string(
          received ? std::strerror(errno) : "netlink peer closed");
      close(descriptor);
      return false;
    }
    std::size_t remaining = static_cast<std::size_t>(received);
    for (auto* header = reinterpret_cast<nlmsghdr*>(response.data());
         NLMSG_OK(header, remaining); header = NLMSG_NEXT(header, remaining)) {
      if (header->nlmsg_seq != request.header.nlmsg_seq) continue;
      if (header->nlmsg_flags & NLM_F_DUMP_INTR) {
        if (error) *error = "FreeBSD neighbor dump was interrupted";
        close(descriptor);
        return false;
      }
      if (header->nlmsg_type == NLMSG_DONE) {
        close(descriptor);
        return true;
      }
      if (header->nlmsg_type == NLMSG_ERROR) {
        if (error) *error = "kernel rejected FreeBSD neighbor dump";
        close(descriptor);
        return false;
      }
      if (header->nlmsg_type != RTM_NEWNEIGH ||
          NLMSG_PAYLOAD(header, 0) < sizeof(ndmsg))
        continue;
      const auto* neighbor =
          reinterpret_cast<const ndmsg*>(NLMSG_DATA(header));
      if (neighbor->ndm_ifindex != static_cast<std::int32_t>(interface_index) ||
          (neighbor->ndm_family != AF_INET && neighbor->ndm_family != AF_INET6))
        continue;
      const void* destination = nullptr;
      std::size_t destination_size = 0;
      const std::byte* link_layer = nullptr;
      std::size_t link_layer_size = 0;
      std::size_t attribute_bytes = NLMSG_PAYLOAD(header, sizeof(ndmsg));
      const auto* attribute = reinterpret_cast<const nlattr*>(
          reinterpret_cast<const std::byte*>(neighbor) +
          NLMSG_ALIGN(sizeof(ndmsg)));
      while (attribute_bytes >= sizeof(nlattr) &&
             attribute->nla_len >= NLA_HDRLEN &&
             attribute->nla_len <= attribute_bytes) {
        const std::size_t payload = attribute->nla_len - NLA_HDRLEN;
        const void* data = attribute + 1;
        if ((attribute->nla_type & NLA_TYPE_MASK) == NDA_DST) {
          destination = data;
          destination_size = payload;
        } else if ((attribute->nla_type & NLA_TYPE_MASK) == NDA_LLADDR) {
          link_layer = reinterpret_cast<const std::byte*>(data);
          link_layer_size = payload;
        }
        const std::size_t step = NLA_ALIGN(attribute->nla_len);
        if (step > attribute_bytes) break;
        attribute_bytes -= step;
        attribute = reinterpret_cast<const nlattr*>(
            reinterpret_cast<const std::byte*>(attribute) + step);
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
                         (neighbor->ndm_state & NUD_PERMANENT) ||
                                 (neighbor->ndm_flags & NTF_STICKY)
                             ? "static" : "dynamic",
                         state, neighbor->ndm_family == AF_INET6,
                         (neighbor->ndm_flags & NTF_ROUTER) != 0});
    }
  }
}

struct LinkSnapshot {
  bool enabled = false;
  bool running = false;
  unsigned mtu = 0;
};

std::optional<LinkSnapshot> ReadLink(std::string_view interface,
                                     std::string* error) {
  IoctlSocket socket(AF_INET);
  if (!socket.valid(error)) return std::nullopt;
  ifreq request{};
  if (!CopyName(interface, request.ifr_name, error)) return std::nullopt;
  if (!socket.Call(SIOCGIFFLAGS, &request,
                   "read flags on " + std::string(interface), error))
    return std::nullopt;
  const bool enabled = (request.ifr_flags & IFF_UP) != 0;
  const bool running = (request.ifr_flags & IFF_RUNNING) != 0;
  if (!socket.Call(SIOCGIFMTU, &request,
                   "read MTU on " + std::string(interface), error))
    return std::nullopt;
  return LinkSnapshot{enabled, running, static_cast<unsigned>(request.ifr_mtu)};
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

std::string AddressStatus(std::string_view interface,
                          const sockaddr_in6& address) {
  IoctlSocket socket(AF_INET6);
  if (!socket.valid(nullptr)) return "unknown";
  in6_ifreq request{};
  if (!CopyName(interface, request.ifr_name, nullptr)) return "unknown";
  request.ifr_addr = address;
  if (!socket.Call(SIOCGIFAFLAG_IN6, &request, "read IPv6 address flags",
                   nullptr))
    return "unknown";
  return std::string(FreeBsdAddressStatus(request.ifr_ifru.ifru_flags6));
}

bool AppendAddresses(const std::string& name, pugi::xml_node entry,
                     unsigned mtu, std::string* error) {
  ifaddrs* values = nullptr;
  if (getifaddrs(&values) != 0) {
    if (error) *error = "cannot enumerate FreeBSD interface addresses: " +
                        std::string(std::strerror(errno));
    return false;
  }
  pugi::xml_node ipv4;
  pugi::xml_node ipv6;
  for (const ifaddrs* value = values; value; value = value->ifa_next) {
    if (!value->ifa_addr || name != value->ifa_name) continue;
    const int family = value->ifa_addr->sa_family;
    if (family != AF_INET && family != AF_INET6) continue;
    pugi::xml_node* family_node = family == AF_INET ? &ipv4 : &ipv6;
    if (!*family_node) {
      *family_node = entry.append_child(family == AF_INET ? "ipv4" : "ipv6");
      family_node->append_attribute("xmlns") =
          "urn:ietf:params:xml:ns:yang:ietf-ip";
      if (family == AF_INET6 || mtu <= 65535)
        family_node->append_child("mtu").text() = mtu;
    }
    char text[INET6_ADDRSTRLEN]{};
    const void* binary = family == AF_INET
        ? static_cast<const void*>(&reinterpret_cast<const sockaddr_in*>(
              value->ifa_addr)->sin_addr)
        : static_cast<const void*>(&reinterpret_cast<const sockaddr_in6*>(
              value->ifa_addr)->sin6_addr);
    if (!inet_ntop(family, binary, text, sizeof(text))) continue;
    pugi::xml_node address = family_node->append_child("address");
    address.append_child("ip").text() = text;
    address.append_child("prefix-length").text() =
        PrefixLength(value->ifa_netmask);
    address.append_child("origin").text() = "other";
    if (family == AF_INET6)
      address.append_child("status").text() =
          AddressStatus(name, *reinterpret_cast<const sockaddr_in6*>(
                                  value->ifa_addr)).c_str();
  }
  freeifaddrs(values);
  return true;
}

bool AppendNeighbors(const std::string& name, pugi::xml_node entry,
                     std::string* error) {
  const unsigned interface_index = if_nametoindex(name.c_str());
  if (!interface_index) {
    if (error) *error = "resolve interface " + name + ": " +
                        std::strerror(errno);
    return false;
  }
  std::vector<LiveNeighbor> neighbors;
  if (!DumpNeighbors(interface_index, &neighbors, error)) return false;
  pugi::xml_node ipv4 = Child(entry, "ipv4");
  pugi::xml_node ipv6 = Child(entry, "ipv6");
  for (const LiveNeighbor& value : neighbors) {
    pugi::xml_node* family = value.ipv6 ? &ipv6 : &ipv4;
    if (!*family) {
      *family = entry.append_child(value.ipv6 ? "ipv6" : "ipv4");
      family->append_attribute("xmlns") =
          "urn:ietf:params:xml:ns:yang:ietf-ip";
    }
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

std::optional<if_data> ReadInterfaceData(const std::string& name,
                                         std::string* error) {
  IoctlSocket socket(AF_INET);
  if (!socket.valid(error)) return std::nullopt;
  ifreq request{};
  if_data data{};
  if (!CopyName(name, request.ifr_name, error)) return std::nullopt;
  request.ifr_data = reinterpret_cast<caddr_t>(&data);
  if (!socket.Call(SIOCGIFDATA, &request,
                   "read interface data on " + name, error))
    return std::nullopt;
  return data;
}

std::string InterfaceType(const if_data& data) {
  if (data.ifi_type == IFT_LOOP) return "iana-if-type:softwareLoopback";
  if (data.ifi_type == IFT_ETHER) return "iana-if-type:ethernetCsmacd";
  return "iana-if-type:other";
}

std::string DiscontinuityTime(const if_data& data) {
  timeval boot{};
  std::size_t size = sizeof(boot);
  if (sysctlbyname("kern.boottime", &boot, &size, nullptr, 0) != 0)
    return "1970-01-01T00:00:00Z";
  const std::time_t discontinuity = boot.tv_sec + data.ifi_epoch;
  std::tm utc{};
  if (!gmtime_r(&discontinuity, &utc)) return "1970-01-01T00:00:00Z";
  char text[32]{};
  if (!std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc))
    return "1970-01-01T00:00:00Z";
  return text;
}

void AppendStatistics(const if_data& data, pugi::xml_node entry) {
  pugi::xml_node statistics = entry.append_child("statistics");
  statistics.append_child("discontinuity-time").text() =
      DiscontinuityTime(data).c_str();
  statistics.append_child("in-octets").text() = data.ifi_ibytes;
  statistics.append_child("in-unicast-pkts").text() =
      data.ifi_ipackets >= data.ifi_imcasts
          ? data.ifi_ipackets - data.ifi_imcasts : 0;
  statistics.append_child("in-multicast-pkts").text() = data.ifi_imcasts;
  statistics.append_child("in-discards").text() =
      static_cast<std::uint32_t>(data.ifi_iqdrops);
  statistics.append_child("in-errors").text() =
      static_cast<std::uint32_t>(data.ifi_ierrors);
  statistics.append_child("in-unknown-protos").text() =
      static_cast<std::uint32_t>(data.ifi_noproto);
  statistics.append_child("out-octets").text() = data.ifi_obytes;
  statistics.append_child("out-unicast-pkts").text() =
      data.ifi_opackets >= data.ifi_omcasts
          ? data.ifi_opackets - data.ifi_omcasts : 0;
  statistics.append_child("out-multicast-pkts").text() = data.ifi_omcasts;
  statistics.append_child("out-discards").text() =
      static_cast<std::uint32_t>(data.ifi_oqdrops);
  statistics.append_child("out-errors").text() =
      static_cast<std::uint32_t>(data.ifi_oerrors);
}

struct KernelInterfaceState {
  std::vector<AddressConfig> addresses;
  std::vector<NeighborConfig> neighbors;
};

bool ReadKernelState(const std::string& name, KernelInterfaceState* state,
                     std::string* error) {
  ifaddrs* values = nullptr;
  if (getifaddrs(&values) != 0) {
    if (error) *error = "cannot enumerate FreeBSD interface addresses: " +
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
  const unsigned interface_index = if_nametoindex(name.c_str());
  if (!interface_index) {
    if (error) *error = "resolve interface " + name + ": " +
                        std::strerror(errno);
    return false;
  }
  std::vector<LiveNeighbor> live_neighbors;
  if (!DumpNeighbors(interface_index, &live_neighbors, error)) return false;
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

bool MtuRequest(std::string_view interface, unsigned mtu, std::string* error) {
  IoctlSocket socket(AF_INET);
  if (!socket.valid(error)) return false;
  ifreq request{};
  if (!CopyName(interface, request.ifr_name, error)) return false;
  request.ifr_mtu = static_cast<int>(mtu);
  return socket.Call(SIOCSIFMTU, &request,
                     "set MTU on " + std::string(interface), error);
}

bool EnabledRequest(std::string_view interface, bool enabled,
                    std::string* error) {
  IoctlSocket socket(AF_INET);
  if (!socket.valid(error)) return false;
  ifreq request{};
  if (!CopyName(interface, request.ifr_name, error)) return false;
  if (!socket.Call(SIOCGIFFLAGS, &request,
                   "read flags on " + std::string(interface), error))
    return false;
  if (enabled) request.ifr_flags |= IFF_UP;
  else request.ifr_flags &= static_cast<short>(~IFF_UP);
  return socket.Call(SIOCSIFFLAGS, &request,
                     "set flags on " + std::string(interface), error);
}

std::optional<unsigned> Mtu(const InterfaceConfig& interface,
                            std::string* error) {
  if (interface.ipv4_mtu && interface.ipv6_mtu &&
      interface.ipv4_mtu != interface.ipv6_mtu) {
    if (error) *error = "FreeBSD requires equal IPv4 and IPv6 link MTUs on " +
                        interface.name;
    return std::nullopt;
  }
  const auto result = interface.ipv4_mtu ? interface.ipv4_mtu :
                                           interface.ipv6_mtu;
  if (result && *result > static_cast<unsigned>(INT_MAX)) {
    if (error) *error = "MTU exceeds the FreeBSD kernel integer range on " +
                        interface.name;
    return std::nullopt;
  }
  return result;
}

struct Operation {
  enum class Kind { kAddress, kEnabled, kMtu, kNeighbor } kind;
  std::string interface;
  std::optional<AddressConfig> address;
  std::optional<bool> enabled;
  std::optional<unsigned> mtu;
  bool add = false;
  std::optional<NeighborConfig> neighbor;

  bool Run(std::string* error) const {
    if (kind == Kind::kAddress)
      return AddressRequest(interface, *address, add, error);
    if (kind == Kind::kEnabled)
      return EnabledRequest(interface, *enabled, error);
    if (kind == Kind::kNeighbor)
      return NeighborRequest(interface, *neighbor, add, error);
    return MtuRequest(interface, *mtu, error);
  }
};

class FreeBsdBackend final : public PlatformBackend {
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
                {{Operation::Kind::kNeighbor, old_interface.name, {}, {}, {},
                  false, *observed},
                 {Operation::Kind::kNeighbor, old_interface.name, {}, {}, {},
                  true, *observed}});
        }
      for (const AddressConfig& value : old_interface.addresses)
        if (!replacement || std::ranges::find(replacement->addresses, value) ==
                                replacement->addresses.end()) {
          const AddressConfig* observed =
              FindByAddress(kernel(old_interface.name).addresses, value);
          if (observed)
            operations.push_back(
                {{Operation::Kind::kAddress, old_interface.name, *observed, {},
                  {}, false, {}},
                 {Operation::Kind::kAddress, old_interface.name, *observed, {},
                  {}, true, {}}});
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
                                 *observed, {}, {}, false, {}},
                                {Operation::Kind::kAddress, interface.name,
                                 *observed, {}, {}, true, {}}});
        if (!observed || *observed != value)
          operations.push_back({{Operation::Kind::kAddress, interface.name,
                                 value, {}, {}, true, {}},
                                {Operation::Kind::kAddress, interface.name,
                                 value, {}, {}, false, {}}});
      }
      for (const NeighborConfig& value : interface.neighbors) {
        const NeighborConfig* observed =
            FindByAddress(kernel(interface.name).neighbors, value);
        if (!observed || *observed != value)
          operations.push_back({{Operation::Kind::kNeighbor, interface.name,
                                 {}, {}, {}, true, value},
                                {Operation::Kind::kNeighbor, interface.name,
                                 {}, {}, {}, observed != nullptr,
                                 observed ? *observed : value}});
      }
      const auto desired_mtu = Mtu(interface, error);
      if ((interface.ipv4_mtu || interface.ipv6_mtu) && !desired_mtu)
        return false;
      const bool needs_link = interface.enabled || desired_mtu;
      const auto snapshot = needs_link ? ReadLink(interface.name, error) :
                                         std::optional<LinkSnapshot>{};
      if (needs_link && !snapshot) return false;
      const bool enabled_changed = interface.enabled &&
          snapshot->enabled != *interface.enabled;
      const bool mtu_changed = desired_mtu && snapshot->mtu != *desired_mtu;
      if (mtu_changed)
        operations.push_back(
            {{Operation::Kind::kMtu, interface.name, {}, {}, desired_mtu, false,
              {}},
             {Operation::Kind::kMtu, interface.name, {}, {}, snapshot->mtu,
              false, {}}});
      if (enabled_changed) {
        std::pair<Operation, Operation> change{
            {Operation::Kind::kEnabled, interface.name, {}, interface.enabled,
             {}, false, {}},
            {Operation::Kind::kEnabled, interface.name, {}, snapshot->enabled,
             {}, false, {}}};
        if (*interface.enabled) operations.push_back(std::move(change));
        else operations.insert(operations.begin(), std::move(change));
      }
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
    pugi::xml_node wrapper = state.append_child("data");
    wrapper.append_attribute("xmlns") =
        "urn:ietf:params:xml:ns:netconf:base:1.0";
    pugi::xml_node root = wrapper.append_child("interfaces-state");
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
      if (error) *error = "cannot enumerate FreeBSD interfaces: " +
                          std::string(std::strerror(errno));
      return false;
    }
    for (const struct if_nameindex* interface = interfaces;
         interface->if_index != 0 && interface->if_name; ++interface) {
      const std::string name = interface->if_name;
      const auto link = ReadLink(name, error);
      const auto data = ReadInterfaceData(name, error);
      if (!link || !data) {
        if_freenameindex(interfaces);
        return false;
      }
      const auto configured = std::ranges::find(
          configured_types, name, &decltype(configured_types)::value_type::first);
      const std::string type = configured == configured_types.end()
          ? InterfaceType(*data) : configured->second;
      pugi::xml_node entry = root.append_child("interface");
      entry.append_child("name").text() = name.c_str();
      entry.append_child("type").text() = type.c_str();
      entry.append_child("admin-status").text() =
          link->enabled ? "up" : "down";
      entry.append_child("oper-status").text() =
          link->running ? "up" : (link->enabled ? "dormant" : "down");
      if (!AppendAddresses(name, entry, link->mtu, error) ||
          !AppendNeighbors(name, entry, error)) {
        if_freenameindex(interfaces);
        return false;
      }
      AppendStatistics(*data, entry);
    }
    if_freenameindex(interfaces);
    // Publish the RFC 8343 NMDA interface inventory as well as the deprecated
    // compatibility tree. Cross-model leafrefs such as RFC 8431's
    // outgoing-interface target /interfaces/interface/name, including for
    // system-created interfaces that are absent from intended configuration.
    pugi::xml_node nmda = wrapper.prepend_child("interfaces");
    nmda.append_attribute("xmlns") =
        "urn:ietf:params:xml:ns:yang:ietf-interfaces";
    nmda.append_attribute("xmlns:iana-if-type") =
        "urn:ietf:params:xml:ns:yang:iana-if-type";
    for (const pugi::xml_node entry : root.children("interface"))
      nmda.append_copy(entry);
    std::ostringstream serialized;
    state.print(serialized, "", pugi::format_raw);
    *output = serialized.str();
    return true;
  }

 private:
  static bool RunOperations(
      const std::vector<std::pair<Operation, Operation>>& operations,
      std::string* error) {
    std::vector<std::size_t> completed;
    for (std::size_t index = 0; index < operations.size(); ++index) {
      if (operations[index].first.Run(error)) {
        completed.push_back(index);
        continue;
      }
      const std::string failure = error ? *error : "interface operation failed";
      std::vector<std::string> rollback_failures;
      for (auto rollback = completed.rbegin(); rollback != completed.rend();
           ++rollback) {
        std::string rollback_error;
        if (!operations[*rollback].second.Run(&rollback_error))
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
  return std::make_unique<FreeBsdBackend>();
}

}  // namespace dangd::ip_management

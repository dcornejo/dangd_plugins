// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "plugins/ip_management/src/platform_backend.h"
#include "plugins/ip_management/src/freebsd/address_status.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

namespace dangd::ip_management {
namespace {

const char* TestInterface() {
  return std::getenv("DANG_PRIVILEGED_IP_INTERFACE");
}

int InterfaceMtu(const char* name) {
  const int descriptor = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (descriptor < 0) return -1;
  ifreq request{};
  std::strncpy(request.ifr_name, name, sizeof(request.ifr_name) - 1);
  const int result = ioctl(descriptor, SIOCGIFMTU, &request);
  close(descriptor);
  return result == 0 ? request.ifr_mtu : -1;
}

bool AddressExists(const char* interface, const char* expected) {
  const int family = std::strchr(expected, ':') ? AF_INET6 : AF_INET;
  ifaddrs* values = nullptr;
  if (getifaddrs(&values) != 0) return false;
  bool found = false;
  for (const ifaddrs* value = values; value; value = value->ifa_next) {
    if (!value->ifa_addr || std::strcmp(interface, value->ifa_name) != 0 ||
        value->ifa_addr->sa_family != family)
      continue;
    char text[INET6_ADDRSTRLEN]{};
    const void* address = family == AF_INET
        ? static_cast<const void*>(&reinterpret_cast<const sockaddr_in*>(
              value->ifa_addr)->sin_addr)
        : static_cast<const void*>(&reinterpret_cast<const sockaddr_in6*>(
              value->ifa_addr)->sin6_addr);
    if (inet_ntop(family, address, text, sizeof(text)) &&
        std::strcmp(text, expected) == 0)
      found = true;
  }
  freeifaddrs(values);
  return found;
}

std::string Configuration(const char* interface, const char* address,
                          int mtu = 0) {
  std::string result = "<config><interfaces><interface><name>";
  result += interface;
  result += "</name><type>iana-if-type:ethernetCsmacd</type><ipv4>";
  if (mtu) result += "<mtu>" + std::to_string(mtu) + "</mtu>";
  if (address && *address) {
    result += "<address><ip>";
    result += address;
    result += "</ip><prefix-length>32</prefix-length></address>";
  }
  result += "</ipv4><ipv6><address><ip>2001:db8::123</ip>"
      "<prefix-length>128</prefix-length></address>";
  if (mtu) result += "<mtu>" + std::to_string(mtu) + "</mtu>";
  result += "</ipv6></interface></interfaces></config>";
  return result;
}

std::string NeighborConfiguration(const char* interface) {
  return "<config><interfaces><interface><name>" + std::string(interface) +
      "</name><type>iana-if-type:ethernetCsmacd</type>"
      "<ipv4><address><ip>198.51.100.123</ip>"
      "<prefix-length>24</prefix-length></address>"
      "<neighbor><ip>198.51.100.200</ip>"
      "<link-layer-address>02:00:00:00:00:c8</link-layer-address>"
      "</neighbor></ipv4>"
      "<ipv6><address><ip>2001:db8:1::123</ip>"
      "<prefix-length>64</prefix-length></address>"
      "<neighbor><ip>2001:db8:1::200</ip>"
      "<link-layer-address>02:00:00:00:00:c9</link-layer-address>"
      "</neighbor></ipv6></interface></interfaces></config>";
}

TEST(FreeBsdIpBackendTest, AppliesAndRollsBackAddressAndObservedMtu) {
  const char* interface = TestInterface();
  if (!interface)
    GTEST_SKIP() << "set DANG_PRIVILEGED_IP_INTERFACE to a disposable interface";
  const int original_mtu = InterfaceMtu(interface);
  ASSERT_GT(original_mtu, 576);
  const std::string configured =
      Configuration(interface, "198.51.100.123", original_mtu - 1);
  auto backend = MakePlatformBackend();
  std::string error;
  ASSERT_TRUE(backend->Reconcile("<config/>", configured, &error)) << error;
  EXPECT_TRUE(AddressExists(interface, "198.51.100.123"));
  EXPECT_TRUE(AddressExists(interface, "2001:db8::123"));
  EXPECT_EQ(InterfaceMtu(interface), original_mtu - 1);
  ASSERT_TRUE(backend->Reconcile(configured, "<config/>", &error)) << error;
  EXPECT_FALSE(AddressExists(interface, "198.51.100.123"));
  EXPECT_FALSE(AddressExists(interface, "2001:db8::123"));
  EXPECT_EQ(InterfaceMtu(interface), original_mtu);
}

TEST(FreeBsdIpBackendTest, CompensatesAfterLaterOperationFails) {
  const char* interface = TestInterface();
  if (!interface)
    GTEST_SKIP() << "set DANG_PRIVILEGED_IP_INTERFACE to a disposable interface";
  std::string desired = "<config><interfaces>";
  desired += "<interface><name>" + std::string(interface) +
      "</name><ipv4><address><ip>198.51.100.124</ip>"
      "<prefix-length>32</prefix-length></address></ipv4></interface>";
  desired +=
      "<interface><name>dang-missing0</name><ipv4><address>"
      "<ip>198.51.100.125</ip><prefix-length>32</prefix-length>"
      "</address></ipv4></interface></interfaces></config>";
  auto backend = MakePlatformBackend();
  std::string error;
  EXPECT_FALSE(backend->Reconcile("<config/>", desired, &error));
  EXPECT_NE(error.find("dang-missing0"), std::string::npos) << error;
  EXPECT_FALSE(AddressExists(interface, "198.51.100.124"));
}

TEST(FreeBsdIpBackendTest, AppliesPublishesRepairsAndRollsBackStaticNeighbor) {
  const char* interface = TestInterface();
  if (!interface)
    GTEST_SKIP() << "set DANG_PRIVILEGED_IP_INTERFACE to a disposable interface";
  const std::string configured = NeighborConfiguration(interface);
  auto backend = MakePlatformBackend();
  std::string error;
  ASSERT_TRUE(backend->Reconcile("<config/>", configured, &error)) << error;
  std::string state;
  ASSERT_TRUE(backend->OperationalXml(configured, &state, &error)) << error;
  EXPECT_NE(state.find("<name>" + std::string(interface) + "</name>"),
            std::string::npos) << state;
  EXPECT_NE(state.find("<admin-status>up</admin-status>"), std::string::npos)
      << state;
  EXPECT_NE(state.find("<oper-status>up</oper-status>"), std::string::npos)
      << state;
  EXPECT_NE(state.find("<statistics>"), std::string::npos) << state;
  EXPECT_NE(state.find("<discontinuity-time>"), std::string::npos) << state;
  EXPECT_NE(state.find("<in-octets>"), std::string::npos) << state;
  EXPECT_NE(state.find("<out-octets>"), std::string::npos) << state;
  std::string peer = interface;
  ASSERT_FALSE(peer.empty());
  peer.back() = peer.back() == 'a' ? 'b' : 'a';
  EXPECT_NE(state.find("<name>" + peer + "</name>"), std::string::npos) << state;
  EXPECT_NE(state.find("<type>iana-if-type:ethernetCsmacd</type>"),
            std::string::npos) << state;
  EXPECT_NE(state.find("<mtu>" + std::to_string(InterfaceMtu(interface)) +
                       "</mtu>"),
            std::string::npos) << state;
  EXPECT_NE(state.find("<ip>198.51.100.123</ip>"), std::string::npos) << state;
  EXPECT_NE(state.find("<prefix-length>24</prefix-length>"), std::string::npos)
      << state;
  EXPECT_NE(state.find("<ip>198.51.100.200</ip>"), std::string::npos) << state;
  EXPECT_NE(state.find(
                "<link-layer-address>02:00:00:00:00:c8</link-layer-address>"),
            std::string::npos) << state;
  EXPECT_NE(state.find("<origin>static</origin>"), std::string::npos) << state;
  EXPECT_NE(state.find("<ip>2001:db8:1::123</ip>"), std::string::npos) << state;
  EXPECT_NE(state.find("<ip>2001:db8:1::200</ip>"), std::string::npos) << state;
  EXPECT_NE(state.find("<status>preferred</status>"), std::string::npos)
      << state;
  EXPECT_NE(state.find(
                "<link-layer-address>02:00:00:00:00:c9</link-layer-address>"),
            std::string::npos) << state;
  backend->Commit();
  auto external = MakePlatformBackend();
  ASSERT_TRUE(external->Reconcile(configured, "<config/>", &error)) << error;
  external->Commit();
  EXPECT_FALSE(AddressExists(interface, "198.51.100.123"));
  EXPECT_FALSE(AddressExists(interface, "2001:db8:1::123"));
  ASSERT_TRUE(backend->Reconcile(configured, configured, &error)) << error;
  state.clear();
  ASSERT_TRUE(backend->OperationalXml(configured, &state, &error)) << error;
  EXPECT_NE(state.find("<ip>198.51.100.123</ip>"), std::string::npos) << state;
  EXPECT_NE(state.find("<ip>198.51.100.200</ip>"), std::string::npos) << state;
  EXPECT_NE(state.find("<ip>2001:db8:1::123</ip>"), std::string::npos) << state;
  EXPECT_NE(state.find("<ip>2001:db8:1::200</ip>"), std::string::npos) << state;
  backend->Commit();
  ASSERT_TRUE(backend->Reconcile(configured, "<config/>", &error)) << error;
}

TEST(FreeBsdIpBackendTest, MapsIpv6AddressStatusFlags) {
  EXPECT_EQ(FreeBsdAddressStatus(0), "preferred");
  EXPECT_EQ(FreeBsdAddressStatus(IN6_IFF_TENTATIVE), "tentative");
  EXPECT_EQ(FreeBsdAddressStatus(IN6_IFF_DUPLICATED), "duplicate");
  EXPECT_EQ(FreeBsdAddressStatus(IN6_IFF_DEPRECATED), "deprecated");
  EXPECT_EQ(FreeBsdAddressStatus(IN6_IFF_DETACHED), "inaccessible");
  EXPECT_EQ(FreeBsdAddressStatus(IN6_IFF_DUPLICATED | IN6_IFF_TENTATIVE),
            "duplicate");
}

TEST(FreeBsdIpBackendTest, PublishesLiveDuplicateIpv6DadStatus) {
  const char* interface =
      std::getenv("DANG_PRIVILEGED_DUPLICATE_IP_INTERFACE");
  const char* address = std::getenv("DANG_PRIVILEGED_DUPLICATE_IP_ADDRESS");
  if (!interface || !address)
    GTEST_SKIP() << "set duplicate-IP interface and address after inducing "
                    "DAD from an external network stack";

  auto backend = MakePlatformBackend();
  std::string state;
  std::string error;
  ASSERT_TRUE(backend->OperationalXml("<config/>", &state, &error)) << error;
  const std::size_t interface_start =
      state.find("<name>" + std::string(interface) + "</name>");
  ASSERT_NE(interface_start, std::string::npos) << state;
  const std::size_t interface_end = state.find("</interface>", interface_start);
  ASSERT_NE(interface_end, std::string::npos) << state;
  const std::string_view published(state.data() + interface_start,
                                   interface_end - interface_start);
  EXPECT_NE(published.find("<ip>" + std::string(address) + "</ip>"),
            std::string_view::npos)
      << published;
  EXPECT_NE(published.find("<status>duplicate</status>"),
            std::string_view::npos)
      << published;
}

}  // namespace
}  // namespace dangd::ip_management

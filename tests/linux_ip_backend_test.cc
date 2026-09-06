// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "plugins/ip_management/src/platform_backend.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <chrono>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>

#include <gtest/gtest.h>

namespace dangd::ip_management {
namespace {

constexpr std::string_view kAddress = "198.51.100.123";

bool AddressExists(std::string_view expected) {
  ifaddrs* addresses = nullptr;
  if (getifaddrs(&addresses) != 0) return false;
  bool found = false;
  for (const ifaddrs* value = addresses; value; value = value->ifa_next) {
    if (!value->ifa_addr || value->ifa_addr->sa_family != AF_INET) continue;
    char text[INET_ADDRSTRLEN]{};
    const auto* address =
        reinterpret_cast<const sockaddr_in*>(value->ifa_addr);
    if (inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text)) &&
        expected == text)
      found = true;
  }
  freeifaddrs(addresses);
  return found;
}

bool PrivilegedTestsEnabled() {
  return std::getenv("DANG_RUN_PRIVILEGED_IP_TESTS") != nullptr;
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

std::string Ipv6Configuration(std::string_view interface) {
  return "<config><interfaces><interface><name>" + std::string(interface) +
      "</name><type>iana-if-type:ethernetCsmacd</type>"
      "<ipv6><address><ip>2001:db8:2::1</ip>"
      "<prefix-length>64</prefix-length></address>"
      "</ipv6></interface></interfaces></config>";
}

TEST(LinuxIpBackendTest, AppliesRepairsAndRemovesAddressThroughRtnetlink) {
  if (!PrivilegedTestsEnabled())
    GTEST_SKIP() << "set DANG_RUN_PRIVILEGED_IP_TESTS=1 in an isolated netns";
  constexpr const char* empty = "<config/>";
  constexpr const char* configured =
      "<config><interfaces><interface><name>lo</name><ipv4><address>"
      "<ip>198.51.100.123</ip><prefix-length>32</prefix-length>"
      "</address></ipv4></interface></interfaces></config>";
  auto backend = MakePlatformBackend();
  std::string error;
  ASSERT_TRUE(backend->Reconcile(empty, configured, &error)) << error;
  EXPECT_TRUE(AddressExists(kAddress));
  backend->Commit();
  auto external = MakePlatformBackend();
  ASSERT_TRUE(external->Reconcile(configured, empty, &error)) << error;
  external->Commit();
  EXPECT_FALSE(AddressExists(kAddress));
  ASSERT_TRUE(backend->Reconcile(configured, configured, &error)) << error;
  EXPECT_TRUE(AddressExists(kAddress));
  backend->Commit();
  ASSERT_TRUE(backend->Reconcile(configured, empty, &error)) << error;
  EXPECT_FALSE(AddressExists(kAddress));
}

TEST(LinuxIpBackendTest, CompensatesAfterLaterOperationFails) {
  if (!PrivilegedTestsEnabled())
    GTEST_SKIP() << "set DANG_RUN_PRIVILEGED_IP_TESTS=1 in an isolated netns";
  constexpr const char* desired =
      "<config><interfaces>"
      "<interface><name>lo</name><ipv4><address>"
      "<ip>198.51.100.123</ip><prefix-length>32</prefix-length>"
      "</address></ipv4></interface>"
      "<interface><name>dang-missing0</name><ipv4><address>"
      "<ip>198.51.100.124</ip><prefix-length>32</prefix-length>"
      "</address></ipv4></interface>"
      "</interfaces></config>";
  auto backend = MakePlatformBackend();
  std::string error;
  EXPECT_FALSE(backend->Reconcile("<config/>", desired, &error));
  EXPECT_NE(error.find("dang-missing0"), std::string::npos) << error;
  EXPECT_FALSE(AddressExists(kAddress));
}

TEST(LinuxIpBackendTest, RollbackRestoresObservedMtu) {
  if (!PrivilegedTestsEnabled())
    GTEST_SKIP() << "set DANG_RUN_PRIVILEGED_IP_TESTS=1 in an isolated netns";
  const int original_mtu = InterfaceMtu("lo");
  ASSERT_GT(original_mtu, 1280);
  const int changed_mtu = original_mtu - 1;
  const std::string configured =
      "<config><interfaces><interface><name>lo</name><ipv4><mtu>" +
      std::to_string(changed_mtu) +
      "</mtu></ipv4></interface></interfaces></config>";
  auto backend = MakePlatformBackend();
  std::string error;
  ASSERT_TRUE(backend->Reconcile("<config/>", configured, &error)) << error;
  EXPECT_EQ(InterfaceMtu("lo"), changed_mtu);
  ASSERT_TRUE(backend->Reconcile(configured, "<config/>", &error)) << error;
  EXPECT_EQ(InterfaceMtu("lo"), original_mtu);
}

TEST(LinuxIpBackendTest, PublishesLiveLinkAndAddressState) {
  constexpr const char* configured =
      "<config><interfaces><interface><name>lo</name>"
      "<type>iana-if-type:softwareLoopback</type>"
      "</interface></interfaces></config>";
  auto backend = MakePlatformBackend();
  std::string state;
  std::string error;
  ASSERT_TRUE(backend->OperationalXml(configured, &state, &error)) << error;
  EXPECT_NE(state.find("<name>lo</name>"), std::string::npos) << state;
  EXPECT_NE(state.find("<admin-status>up</admin-status>"), std::string::npos)
      << state;
  EXPECT_NE(state.find("<oper-status>up</oper-status>"), std::string::npos)
      << state;
  EXPECT_NE(state.find("<ip>127.0.0.1</ip>"), std::string::npos) << state;
  EXPECT_NE(state.find("<prefix-length>8</prefix-length>"), std::string::npos)
      << state;
  EXPECT_NE(state.find("<status>preferred</status>"), std::string::npos)
      << state;
  EXPECT_NE(state.find("<mtu>"), std::string::npos) << state;
  EXPECT_NE(state.find("<statistics>"), std::string::npos) << state;
  EXPECT_NE(state.find("<discontinuity-time>"), std::string::npos) << state;
  EXPECT_NE(state.find("<in-octets>"), std::string::npos) << state;
  EXPECT_NE(state.find("<out-octets>"), std::string::npos) << state;
  if (PrivilegedTestsEnabled()) {
    EXPECT_NE(state.find("<name>eth0</name>"), std::string::npos) << state;
    EXPECT_NE(state.find("<type>iana-if-type:ethernetCsmacd</type>"),
              std::string::npos) << state;
  }
}

TEST(LinuxIpBackendTest, PublishesAndRepairsConfiguredKernelNeighbor) {
  if (!PrivilegedTestsEnabled())
    GTEST_SKIP() << "set DANG_RUN_PRIVILEGED_IP_TESTS=1 in an isolated netns";
  constexpr const char* configured =
      "<config><interfaces><interface><name>eth0</name>"
      "<type>iana-if-type:ethernetCsmacd</type><ipv4><neighbor>"
      "<ip>198.51.100.200</ip>"
      "<link-layer-address>02:00:00:00:00:c8</link-layer-address>"
      "</neighbor></ipv4></interface></interfaces></config>";
  auto backend = MakePlatformBackend();
  std::string error;
  ASSERT_TRUE(backend->Reconcile("<config/>", configured, &error)) << error;
  std::string state;
  ASSERT_TRUE(backend->OperationalXml(configured, &state, &error)) << error;
  EXPECT_NE(state.find("<ip>198.51.100.200</ip>"), std::string::npos) << state;
  EXPECT_NE(state.find(
                "<link-layer-address>02:00:00:00:00:c8</link-layer-address>"),
            std::string::npos) << state;
  EXPECT_NE(state.find("<origin>static</origin>"), std::string::npos) << state;
  backend->Commit();
  auto external = MakePlatformBackend();
  ASSERT_TRUE(external->Reconcile(configured, "<config/>", &error)) << error;
  external->Commit();
  ASSERT_TRUE(backend->Reconcile(configured, configured, &error)) << error;
  state.clear();
  ASSERT_TRUE(backend->OperationalXml(configured, &state, &error)) << error;
  EXPECT_NE(state.find("<ip>198.51.100.200</ip>"), std::string::npos) << state;
  backend->Commit();
  ASSERT_TRUE(backend->Reconcile(configured, "<config/>", &error)) << error;
}

TEST(LinuxIpBackendTest, PublishesDuplicateIpv6AddressStatus) {
  if (!PrivilegedTestsEnabled())
    GTEST_SKIP() << "set DANG_RUN_PRIVILEGED_IP_TESTS=1 in an isolated netns";
  const std::string first = Ipv6Configuration("eth0");
  const std::string second = Ipv6Configuration("eth1");
  auto first_backend = MakePlatformBackend();
  auto second_backend = MakePlatformBackend();
  std::string error;
  ASSERT_TRUE(first_backend->Reconcile("<config/>", first, &error)) << error;
  first_backend->Commit();
  ASSERT_TRUE(second_backend->Reconcile("<config/>", second, &error)) << error;
  second_backend->Commit();
  std::string state;
  for (int attempt = 0; attempt < 100; ++attempt) {
    ASSERT_TRUE(first_backend->OperationalXml(first, &state, &error)) << error;
    if (state.find("<status>duplicate</status>") != std::string::npos) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  EXPECT_NE(state.find("<status>duplicate</status>"), std::string::npos)
      << state;
  ASSERT_TRUE(second_backend->Reconcile(second, "<config/>", &error)) << error;
  ASSERT_TRUE(first_backend->Reconcile(first, "<config/>", &error)) << error;
}

}  // namespace
}  // namespace dangd::ip_management

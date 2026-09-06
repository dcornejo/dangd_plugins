// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "plugins/ip_management/src/platform_config.h"

#include <gtest/gtest.h>

namespace dangd::ip_management {
namespace {

TEST(IpManagementPlatformTest, ExtractsNamespacedIpv4AndIpv6Intent) {
  constexpr std::string_view xml = R"xml(
    <config xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
      <if:interfaces xmlns:if="urn:ietf:params:xml:ns:yang:ietf-interfaces"
                     xmlns:ip="urn:ietf:params:xml:ns:yang:ietf-ip">
        <if:interface><if:name>em0</if:name><if:enabled>true</if:enabled>
          <ip:ipv4><ip:mtu>1500</ip:mtu>
            <ip:address><ip:ip>192.0.2.4</ip:ip>
            <ip:prefix-length>24</ip:prefix-length></ip:address></ip:ipv4>
          <ip:ipv6><ip:mtu>1480</ip:mtu>
            <ip:address><ip:ip>2001:db8::4</ip:ip>
              <ip:prefix-length>64</ip:prefix-length></ip:address>
            <ip:neighbor><ip:ip>2001:db8::1</ip:ip>
              <ip:link-layer-address>02:00:00:00:00:01</ip:link-layer-address>
            </ip:neighbor>
          </ip:ipv6>
        </if:interface>
      </if:interfaces>
    </config>)xml";
  std::vector<InterfaceConfig> interfaces;
  std::string error;
  ASSERT_TRUE(ParsePlatformConfig(xml, &interfaces, &error)) << error;
  ASSERT_EQ(interfaces.size(), 1u);
  EXPECT_EQ(interfaces[0].name, "em0");
  EXPECT_EQ(interfaces[0].enabled, true);
  EXPECT_EQ(interfaces[0].ipv4_mtu, 1500u);
  EXPECT_EQ(interfaces[0].ipv6_mtu, 1480u);
  EXPECT_EQ(interfaces[0].addresses,
            (std::vector<AddressConfig>{{"192.0.2.4", 24, false},
                                        {"2001:db8::4", 64, true}}));
  EXPECT_EQ(interfaces[0].neighbors,
            (std::vector<NeighborConfig>{{"2001:db8::1",
                                          "02:00:00:00:00:01", true}}));
}

TEST(IpManagementPlatformTest, RejectsMalformedConfiguration) {
  std::vector<InterfaceConfig> interfaces;
  std::string error;
  EXPECT_FALSE(ParsePlatformConfig("<config>", &interfaces, &error));
  EXPECT_NE(error.find("cannot parse configuration"), std::string::npos);
}

TEST(IpManagementPlatformTest, RejectsOptionLikeInterfaceName) {
  constexpr std::string_view xml =
      "<interfaces><interface><name>--help</name></interface></interfaces>";
  std::vector<InterfaceConfig> interfaces;
  std::string error;
  EXPECT_FALSE(ParsePlatformConfig(xml, &interfaces, &error));
  EXPECT_NE(error.find("begin with '-'"), std::string::npos);
}

TEST(IpManagementPlatformTest, RejectsInvalidMtuAndDuplicateNeighbors) {
  constexpr std::string_view invalid_mtu = R"xml(
    <interfaces><interface><name>em0</name>
      <ipv6><mtu>1279</mtu></ipv6>
    </interface></interfaces>)xml";
  std::vector<InterfaceConfig> interfaces;
  std::string error;
  EXPECT_FALSE(ParsePlatformConfig(invalid_mtu, &interfaces, &error));
  EXPECT_NE(error.find("MTU"), std::string::npos);

  constexpr std::string_view duplicate_neighbor = R"xml(
    <interfaces><interface><name>em0</name><ipv4>
      <neighbor><ip>192.0.2.1</ip><link-layer-address>00:00:5e:00:53:01</link-layer-address></neighbor>
      <neighbor><ip>192.0.2.1</ip><link-layer-address>00:00:5e:00:53:02</link-layer-address></neighbor>
    </ipv4></interface></interfaces>)xml";
  EXPECT_FALSE(ParsePlatformConfig(duplicate_neighbor, &interfaces, &error));
  EXPECT_NE(error.find("duplicate interface neighbor"), std::string::npos);
}

TEST(IpManagementPlatformTest, RejectsIncompleteModeledIpData) {
  constexpr std::string_view xml = R"xml(
    <interfaces><interface><name>em0</name><ipv4>
      <address><ip>192.0.2.1</ip><prefix-length>not-a-number</prefix-length></address>
    </ipv4></interface></interfaces>)xml";
  std::vector<InterfaceConfig> interfaces;
  std::string error;
  EXPECT_FALSE(ParsePlatformConfig(xml, &interfaces, &error));
  EXPECT_NE(error.find("incomplete or invalid IP data"), std::string::npos);
  EXPECT_TRUE(interfaces.empty());
}

}  // namespace
}  // namespace dangd::ip_management

// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "plugins/ntp/src/ntp_config.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>

namespace dang::ntp {
namespace {

constexpr std::string_view kBefore = R"xml(
<config xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"
        xmlns:n="urn:ietf:params:xml:ns:yang:ietf-ntp">
  <n:ntp>
    <n:authentication>
      <n:auth-enabled>true</n:auth-enabled>
      <n:authentication-keys>
        <n:keyid>7</n:keyid>
        <n:algorithm>n:hmac-sha-256</n:algorithm>
        <n:key><n:hexadecimal-string>01:23:45:67</n:hexadecimal-string></n:key>
        <n:istrusted>true</n:istrusted>
      </n:authentication-keys>
    </n:authentication>
    <n:access-rules><n:access-rule>
      <n:access-mode>n:server-access-mode</n:access-mode>
      <n:acl>ntp-clients</n:acl>
    </n:access-rule></n:access-rules>
    <n:unicast-configuration>
      <n:address>192.0.2.10</n:address><n:type>n:uc-server</n:type>
      <n:authentication><n:keyid>7</n:keyid></n:authentication>
      <n:iburst>true</n:iburst>
    </n:unicast-configuration>
  </n:ntp>
</config>)xml";

constexpr std::string_view kProposed = R"xml(
<config xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"
        xmlns:n="urn:ietf:params:xml:ns:yang:ietf-ntp">
  <n:ntp>
    <n:port>1123</n:port>
    <n:refclock-master><n:master-stratum>8</n:master-stratum></n:refclock-master>
    <n:authentication>
      <n:auth-enabled>true</n:auth-enabled>
      <n:authentication-keys>
        <n:keyid>8</n:keyid>
        <n:algorithm>n:aes-cmac</n:algorithm>
        <n:key><n:hexadecimal-string>89:ab:cd:ef</n:hexadecimal-string></n:key>
        <n:istrusted>true</n:istrusted>
      </n:authentication-keys>
    </n:authentication>
    <n:access-rules><n:access-rule>
      <n:access-mode>n:peer-access-mode</n:access-mode>
      <n:acl>trusted-peers</n:acl>
    </n:access-rule></n:access-rules>
    <n:unicast-configuration>
      <n:address>2001:db8::10</n:address><n:type>n:uc-peer</n:type>
      <n:authentication><n:keyid>8</n:keyid></n:authentication>
      <n:source>test0</n:source><n:prefer>true</n:prefer>
      <n:minpoll>4</n:minpoll><n:maxpoll>8</n:maxpoll><n:version>3</n:version>
    </n:unicast-configuration>
  </n:ntp>
</config>)xml";

bool Parse(std::string_view xml, Config* config, std::string* error,
           std::string* path) {
  const std::string owned(xml);
  return ParseConfig(owned.c_str(), config, error, path);
}

TEST(NtpConfigTest, ParsesSupportedConfigurationAndDefaults) {
  Config config;
  std::string error;
  std::string path;
  ASSERT_TRUE(Parse(kBefore, &config, &error, &path)) << path << ": " << error;
  ASSERT_TRUE(config.present);
  EXPECT_EQ(config.port, 123);
  EXPECT_FALSE(config.refclock_master_stratum);
  ASSERT_EQ(config.authentication_keys.size(), 1U);
  EXPECT_EQ(config.authentication_keys[0].algorithm, "hmac-sha-256");
  EXPECT_EQ(config.authentication_keys[0].hexadecimal, "01:23:45:67");
  ASSERT_EQ(config.access_rules.size(), 1U);
  EXPECT_EQ(config.access_rules[0].mode, "server-access-mode");
  ASSERT_EQ(config.unicast.size(), 1U);
  EXPECT_EQ(config.unicast[0].type, "uc-server");
  EXPECT_TRUE(config.unicast[0].iburst);
  EXPECT_EQ(config.unicast[0].common.minpoll, 6);
  EXPECT_EQ(config.unicast[0].common.maxpoll, 10);
  EXPECT_EQ(config.unicast[0].common.version, 4U);
  EXPECT_EQ(config.unicast[0].common.key_id, 7U);
}

TEST(NtpConfigTest, EmptySnapshotMeansDisabled) {
  Config config;
  std::string error;
  std::string path;
  ASSERT_TRUE(Parse("<config/>", &config, &error, &path));
  EXPECT_FALSE(config.present);
  EXPECT_TRUE(config.unicast.empty());
}

TEST(NtpConfigTest, RejectsRfc7317AndRfc9249AtTheSameTime) {
  constexpr std::string_view xml = R"xml(
    <config xmlns:n="urn:ietf:params:xml:ns:yang:ietf-ntp"
      xmlns:s="urn:ietf:params:xml:ns:yang:ietf-system">
      <n:ntp/><s:system><s:ntp/></s:system>
    </config>)xml";
  Config config;
  std::string error;
  std::string path;
  EXPECT_FALSE(Parse(xml, &config, &error, &path));
  EXPECT_EQ(path, "/ietf-ntp:ntp");
  EXPECT_NE(error.find("mutually exclusive"), std::string::npos);
}

TEST(NtpConfigTest, RejectsUnknownAuthenticationReference) {
  constexpr std::string_view xml = R"xml(
    <ntp xmlns="urn:ietf:params:xml:ns:yang:ietf-ntp">
      <unicast-configuration><address>192.0.2.1</address>
        <type>uc-server</type><authentication><keyid>42</keyid></authentication>
      </unicast-configuration>
    </ntp>)xml";
  Config config;
  std::string error;
  std::string path;
  EXPECT_FALSE(Parse(xml, &config, &error, &path));
  EXPECT_EQ(path, "/ietf-ntp:ntp/unicast-configuration/authentication/keyid");
}

TEST(NtpConfigTest, RejectsDeprecatedSecretAtExactPath) {
  constexpr std::string_view xml = R"xml(
    <ntp xmlns="urn:ietf:params:xml:ns:yang:ietf-ntp">
      <authentication><authentication-keys><keyid>1</keyid>
        <algorithm>hmac-sha-256</algorithm>
        <key><keystring>do-not-accept</keystring></key>
      </authentication-keys></authentication>
    </ntp>)xml";
  Config config;
  std::string error;
  std::string path;
  EXPECT_FALSE(Parse(xml, &config, &error, &path));
  EXPECT_EQ(path,
            "/ietf-ntp:ntp/authentication/authentication-keys/key/"
            "keystring");
}

TEST(NtpConfigTest, RejectsUnimplementedInterfaceModes) {
  constexpr std::string_view xml = R"xml(
    <ntp xmlns="urn:ietf:params:xml:ns:yang:ietf-ntp">
      <interfaces><interface><name>test0</name><broadcast-client/></interface>
      </interfaces>
    </ntp>)xml";
  Config config;
  std::string error;
  std::string path;
  EXPECT_FALSE(Parse(xml, &config, &error, &path));
  EXPECT_EQ(path, "/ietf-ntp:ntp/interfaces");
}

TEST(NtpConfigTest, RejectsAugmentedConfigurationAtItsPath) {
  constexpr std::string_view xml = R"xml(
    <ntp xmlns="urn:ietf:params:xml:ns:yang:ietf-ntp"
         xmlns:v="urn:vendor:ntp">
      <v:hardware-clock-selector>clock0</v:hardware-clock-selector>
    </ntp>)xml";
  Config config;
  std::string error;
  std::string path;
  EXPECT_FALSE(Parse(xml, &config, &error, &path));
  EXPECT_EQ(path, "/ietf-ntp:ntp/hardware-clock-selector");
}

TEST(NtpConfigTest, RejectsMalformedAddressBeforeNativePlanning) {
  constexpr std::string_view xml = R"xml(
    <ntp xmlns="urn:ietf:params:xml:ns:yang:ietf-ntp">
      <unicast-configuration><address>not-an-address</address>
        <type>uc-server</type></unicast-configuration>
    </ntp>)xml";
  Config config;
  std::string error;
  std::string path;
  EXPECT_FALSE(Parse(xml, &config, &error, &path));
  EXPECT_EQ(path, "/ietf-ntp:ntp/unicast-configuration");
}

TEST(NtpConfigTest, PlansPrerequisitesBeforeNewAssociationAndRedactsKeys) {
  Config before;
  Config proposed;
  std::string error;
  std::string path;
  ASSERT_TRUE(Parse(kBefore, &before, &error, &path));
  ASSERT_TRUE(Parse(kProposed, &proposed, &error, &path));
  const auto plan = PlanChanges(before, proposed);
  ASSERT_EQ(plan.size(), 8U);
  EXPECT_EQ(plan[0].id, "stop-ntp-for-reconfiguration");
  EXPECT_EQ(plan[1].id, "remove-unicast-0");
  EXPECT_NE(
      std::ranges::find(plan[1].dependencies, "stop-ntp-for-reconfiguration"),
      plan[1].dependencies.end());
  EXPECT_EQ(plan[2].id, "configure-authentication");
  EXPECT_EQ(plan[3].id, "configure-access-rules");
  EXPECT_EQ(plan[4].id, "configure-listener-port");
  EXPECT_EQ(plan[5].id, "configure-reference-clock");
  EXPECT_EQ(plan[6].id, "configure-unicast-0");
  EXPECT_NE(std::ranges::find(plan[6].dependencies, "configure-authentication"),
            plan[6].dependencies.end());
  EXPECT_NE(std::ranges::find(plan[6].dependencies, "configure-access-rules"),
            plan[6].dependencies.end());
  EXPECT_NE(std::ranges::find(plan[6].dependencies, "configure-listener-port"),
            plan[6].dependencies.end());
  EXPECT_EQ(plan[7].id, "restart-ntp-after-reconfiguration");
  EXPECT_NE(std::ranges::find(plan[7].dependencies, "configure-unicast-0"),
            plan[7].dependencies.end());
  const std::string descriptions = [&] {
    std::string joined;
    for (const auto& action : plan) joined += action.description;
    return joined;
  }();
  EXPECT_EQ(descriptions.find("01:23:45:67"), std::string::npos);
  EXPECT_EQ(descriptions.find("89:ab:cd:ef"), std::string::npos);
}

TEST(NtpConfigTest, NoChangeProducesNoActions) {
  Config config;
  std::string error;
  std::string path;
  ASSERT_TRUE(Parse(kBefore, &config, &error, &path));
  EXPECT_TRUE(PlanChanges(config, config).empty());
}

TEST(NtpConfigTest, ServiceTransitionsBracketConfigurationChanges) {
  Config enabled;
  std::string error;
  std::string path;
  ASSERT_TRUE(Parse(kBefore, &enabled, &error, &path));
  const Config disabled;

  const auto stop = PlanChanges(enabled, disabled);
  ASSERT_FALSE(stop.empty());
  EXPECT_EQ(stop.front().id, "disable-ntp");
  ASSERT_GE(stop.size(), 2U);
  EXPECT_NE(std::ranges::find(stop[1].dependencies, "disable-ntp"),
            stop[1].dependencies.end());

  const auto start = PlanChanges(disabled, enabled);
  ASSERT_FALSE(start.empty());
  EXPECT_EQ(start.back().id, "enable-ntp");
  EXPECT_NE(
      std::ranges::find(start.back().dependencies, "configure-authentication"),
      start.back().dependencies.end());
  EXPECT_NE(std::ranges::find(start.back().dependencies, "configure-unicast-0"),
            start.back().dependencies.end());
}

TEST(NtpConfigTest, RestartWaitsForARemovalOnlyChange) {
  Config before;
  std::string error;
  std::string path;
  ASSERT_TRUE(Parse(kBefore, &before, &error, &path));
  Config proposed = before;
  proposed.unicast.clear();

  const auto plan = PlanChanges(before, proposed);
  ASSERT_EQ(plan.size(), 3U);
  EXPECT_EQ(plan[0].id, "stop-ntp-for-reconfiguration");
  EXPECT_EQ(plan[1].id, "remove-unicast-0");
  EXPECT_EQ(plan[2].id, "restart-ntp-after-reconfiguration");
  EXPECT_NE(std::ranges::find(plan[2].dependencies, "remove-unicast-0"),
            plan[2].dependencies.end());
}

TEST(NtpConfigTest, SystemOrderedListsProduceAStableNoop) {
  constexpr std::string_view first = R"xml(
    <ntp xmlns="urn:ietf:params:xml:ns:yang:ietf-ntp">
      <unicast-configuration><address>192.0.2.2</address>
        <type>uc-server</type></unicast-configuration>
      <unicast-configuration><address>192.0.2.1</address>
        <type>uc-server</type></unicast-configuration>
    </ntp>)xml";
  constexpr std::string_view second = R"xml(
    <ntp xmlns="urn:ietf:params:xml:ns:yang:ietf-ntp">
      <unicast-configuration><address>192.0.2.1</address>
        <type>uc-server</type></unicast-configuration>
      <unicast-configuration><address>192.0.2.2</address>
        <type>uc-server</type></unicast-configuration>
    </ntp>)xml";
  Config before;
  Config proposed;
  std::string error;
  std::string path;
  ASSERT_TRUE(Parse(first, &before, &error, &path));
  ASSERT_TRUE(Parse(second, &proposed, &error, &path));
  EXPECT_EQ(before, proposed);
  EXPECT_TRUE(PlanChanges(before, proposed).empty());
}

}  // namespace
}  // namespace dang::ntp

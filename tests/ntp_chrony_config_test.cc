// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/** @file Tests exact, side-effect-free RFC 9249 to chrony translation. */

#include <gtest/gtest.h>

#include <string>

#include "plugins/ntp/src/chrony_config.h"

namespace dang::ntp {
namespace {

Config ExampleConfig() {
  Config config;
  config.present = true;
  config.port = 1123;
  config.refclock_master_stratum = 8;
  config.authentication_present = true;
  config.authentication_enabled = true;
  config.authentication_keys = {
      {.id = 7,
       .algorithm = "aes-cmac",
       .hexadecimal = "01:23:45:67:89:ab:cd:ef:01:23:45:67:89:ab:cd:ef",
       .trusted = true},
      {.id = 8,
       .algorithm = "aes-cmac",
       .hexadecimal = "00112233445566778899aabbccddeeff",
       .trusted = true}};
  config.unicast = {{.address = "192.0.2.10",
                     .type = "uc-server",
                     .prefer = true,
                     .burst = true,
                     .iburst = true,
                     .source_interface = std::nullopt,
                     .common = {.minpoll = 4,
                                .maxpoll = 8,
                                .port = 1123,
                                .version = 4,
                                .key_id = 7}},
                    {.address = "2001:db8::10",
                     .type = "uc-peer",
                     .source_interface = std::nullopt,
                     .common = {.minpoll = 6,
                                .maxpoll = 10,
                                .port = 123,
                                .version = 3,
                                .key_id = 8}}};
  return config;
}

TEST(NtpChronyConfigTest, RendersDeterministicFragmentAndRootOnlyKeyImage) {
  ChronyConfigImage image;
  std::string error;
  std::string path;
  ASSERT_TRUE(RenderChronyConfig(
      ExampleConfig(), "/etc/chrony/dangd-rfc9249.keys", &image, &error, &path))
      << path << ": " << error;
  EXPECT_EQ(image.fragment,
            "# Managed by the dangd RFC 9249 plugin. Do not edit.\n"
            "port 1123\n"
            "local stratum 8\n"
            "keyfile /etc/chrony/dangd-rfc9249.keys\n"
            "server 192.0.2.10 minpoll 4 maxpoll 8 version 4 port 1123 key 7 "
            "prefer burst iburst\n"
            "peer 2001:db8::10 minpoll 6 maxpoll 10 version 3 key 8\n");
  EXPECT_EQ(image.key_file,
            "7 AES128 HEX:0123456789ABCDEF0123456789ABCDEF\n"
            "8 AES128 HEX:00112233445566778899AABBCCDDEEFF\n");
  EXPECT_EQ(image.fragment.find("012345"), std::string::npos);
}

TEST(NtpChronyConfigTest, DisabledConfigurationProducesNoManagedFiles) {
  ChronyConfigImage image{.fragment = "old", .key_file = "secret"};
  std::string error;
  std::string path;
  EXPECT_TRUE(RenderChronyConfig({}, "/unused", &image, &error, &path));
  EXPECT_TRUE(image.fragment.empty());
  EXPECT_TRUE(image.key_file.empty());
}

TEST(NtpChronyConfigTest, RejectsHmacRatherThanSubstitutingALegacyMac) {
  for (const char* algorithm :
       {"hmac-sha-256", "hmac-sha-384", "hmac-sha-512"}) {
    Config config = ExampleConfig();
    config.authentication_keys[0].algorithm = algorithm;
    ChronyConfigImage image;
    std::string error;
    std::string path;
    EXPECT_FALSE(RenderChronyConfig(config, "/keys", &image, &error, &path));
    EXPECT_EQ(path,
              "/ietf-ntp:ntp/authentication/authentication-keys/algorithm");
    EXPECT_TRUE(image.fragment.empty());
    EXPECT_TRUE(image.key_file.empty());
  }
}

TEST(NtpChronyConfigTest, RejectsKeyFileDirectiveInjection) {
  ChronyConfigImage image;
  std::string error;
  std::string path;
  EXPECT_FALSE(RenderChronyConfig(ExampleConfig(), "/keys\nallow all", &image,
                                  &error, &path));
  EXPECT_EQ(path, "/ietf-ntp:ntp");
}

TEST(NtpChronyConfigTest, RejectsAssociationSpecificSourceInterface) {
  Config config = ExampleConfig();
  config.unicast[0].source_interface = "test0";
  ChronyConfigImage image;
  std::string error;
  std::string path;
  EXPECT_FALSE(RenderChronyConfig(config, "/keys", &image, &error, &path));
  EXPECT_EQ(path, "/ietf-ntp:ntp/unicast-configuration/source");
  EXPECT_NE(error.find("daemon-wide"), std::string::npos);
}

TEST(NtpChronyConfigTest, RejectsBurstOptionsOnSymmetricPeer) {
  Config config = ExampleConfig();
  config.unicast[1].iburst = true;
  ChronyConfigImage image;
  std::string error;
  std::string path;
  EXPECT_FALSE(RenderChronyConfig(config, "/keys", &image, &error, &path));
  EXPECT_EQ(path, "/ietf-ntp:ntp/unicast-configuration/iburst");
}

TEST(NtpChronyConfigTest, RejectsUntrustedAssociationKey) {
  Config config = ExampleConfig();
  config.authentication_keys[0].trusted = false;
  ChronyConfigImage image;
  std::string error;
  std::string path;
  EXPECT_FALSE(RenderChronyConfig(config, "/keys", &image, &error, &path));
  EXPECT_EQ(path, "/ietf-ntp:ntp/unicast-configuration/authentication/keyid");
}

TEST(NtpChronyConfigTest, RejectsAmbiguousAesCmacKeyLengthWithoutLeakingIt) {
  Config config = ExampleConfig();
  config.authentication_keys[1].hexadecimal = "deadbeef";
  ChronyConfigImage image;
  std::string error;
  std::string path;
  EXPECT_FALSE(RenderChronyConfig(config, "/keys", &image, &error, &path));
  EXPECT_EQ(path,
            "/ietf-ntp:ntp/authentication/authentication-keys/key/"
            "hexadecimal-string");
  EXPECT_EQ(error.find("deadbeef"), std::string::npos);
}

TEST(NtpChronyConfigTest, RejectsStratumChronyCannotRepresent) {
  Config config = ExampleConfig();
  config.refclock_master_stratum = 16;
  ChronyConfigImage image;
  std::string error;
  std::string path;
  EXPECT_FALSE(RenderChronyConfig(config, "/keys", &image, &error, &path));
  EXPECT_EQ(path, "/ietf-ntp:ntp/refclock-master/master-stratum");
}

TEST(NtpChronyConfigTest, RejectsAccessRulesUntilAclOwnershipExists) {
  Config config = ExampleConfig();
  config.access_rules.push_back(
      {.mode = "server-access-mode", .acl = "clients"});
  ChronyConfigImage image;
  std::string error;
  std::string path;
  EXPECT_FALSE(RenderChronyConfig(config, "/keys", &image, &error, &path));
  EXPECT_EQ(path, "/ietf-ntp:ntp/access-rules");
}

}  // namespace
}  // namespace dang::ntp

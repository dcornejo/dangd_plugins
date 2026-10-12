// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0
/** @file Checks the exclusive wrapper boundary and secret-free failures. */
#include <gtest/gtest.h>

#include "plugins/ntp/src/chrony_preflight.h"
namespace dang::ntp {
TEST(ChronyPreflight, RequiresExactlyOneLiteralInclude) {
  std::string error;
  EXPECT_TRUE(ValidateChronyWrapper(
      "# wrapper\npidfile /run/ntp.pid\ninclude /managed.conf\n",
      "/managed.conf", false, &error));
  EXPECT_FALSE(ValidateChronyWrapper("pidfile /run/ntp.pid\n", "/managed.conf",
                                     false, &error));
  EXPECT_FALSE(
      ValidateChronyWrapper("include /managed.conf\ninclude /managed.conf\n",
                            "/managed.conf", false, &error));
  EXPECT_FALSE(ValidateChronyWrapper("include /etc/*.conf\n", "/managed.conf",
                                     false, &error));
}
TEST(ChronyPreflight, RefusesUnmanagedPolicySourcesAndSecretArguments) {
  std::string error;
  for (const char* directive :
       {"server 192.0.2.1", "pool time.example", "sourcedir /run/dhcp",
        "confdir /etc/chrony/conf.d", "keyfile /secret-key-file", "port 123",
        "local stratum 8", "allow all"}) {
    EXPECT_FALSE(ValidateChronyWrapper(
        std::string("include /managed.conf\n") + directive, "/managed.conf",
        false, &error));
    EXPECT_EQ(error.find("secret-key-file"), std::string::npos);
  }
}
TEST(ChronyPreflight, AllowsOnlyExplicitLoopbackTestNetworking) {
  std::string error;
  const char* text =
      "bindaddress 127.0.0.1\nallow 127.0.0.1\ncmdport 0\ninclude "
      "/managed.conf\n";
  EXPECT_TRUE(ValidateChronyWrapper(text, "/managed.conf", true, &error));
  EXPECT_FALSE(ValidateChronyWrapper(text, "/managed.conf", false, &error));
  EXPECT_FALSE(
      ValidateChronyWrapper("bindaddress 0.0.0.0\ninclude /managed.conf\n",
                            "/managed.conf", true, &error));
}
}  // namespace dang::ntp

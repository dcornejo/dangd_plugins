// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0
/** @file Real-filesystem trust checks and strict daemon invocation policy. */
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "plugins/ntp/src/chrony_deployment.h"
namespace dang::ntp {
namespace {
class ChronyDeploymentTest : public testing::Test {
 protected:
  void SetUp() override {
    char pattern[] = "/tmp/dang-ntp-trust-XXXXXX";
    const char* created = mkdtemp(pattern);
    ASSERT_NE(created, nullptr);
    root = std::filesystem::canonical(created).string();
    {
      std::ofstream out(root + "/main.conf");
      out << "wrapper";
    }
    {
      std::ofstream out(root + "/managed.keys");
      out << "private";
    }
    ASSERT_EQ(chmod((root + "/managed.keys").c_str(), 0600), 0);
    ASSERT_EQ(chmod((root + "/main.conf").c_str(), 0644), 0);
  }
  void TearDown() override { std::filesystem::remove_all(root); }
  std::string root, error, contents;
};
TEST_F(ChronyDeploymentTest, ReadsWrapperFromTrustedTree) {
  ASSERT_TRUE(ValidateChronyTree(root, geteuid(), &contents, &error)) << error;
  EXPECT_EQ(contents, "wrapper");
}
TEST_F(ChronyDeploymentTest, RefusesWritableDirectoryAndPublicKeys) {
  ASSERT_EQ(chmod(root.c_str(), 0777), 0);
  EXPECT_FALSE(ValidateChronyTree(root, geteuid(), &contents, &error));
  ASSERT_EQ(chmod(root.c_str(), 0700), 0);
  ASSERT_EQ(chmod((root + "/managed.keys").c_str(), 0644), 0);
  EXPECT_FALSE(ValidateChronyTree(root, geteuid(), &contents, &error));
}
TEST_F(ChronyDeploymentTest, RefusesSymbolicAndHardLinkedWrapper) {
  ASSERT_EQ(unlink((root + "/main.conf").c_str()), 0);
  ASSERT_EQ(symlink("managed.keys", (root + "/main.conf").c_str()), 0);
  EXPECT_FALSE(ValidateChronyTree(root, geteuid(), &contents, &error));
  ASSERT_EQ(unlink((root + "/main.conf").c_str()), 0);
  ASSERT_EQ(
      link((root + "/managed.keys").c_str(), (root + "/main.conf").c_str()), 0);
  EXPECT_FALSE(ValidateChronyTree(root, geteuid(), &contents, &error));
}
TEST_F(ChronyDeploymentTest, RefusesUnexpectedOwnerAndExecutablePermissions) {
  const uid_t other = geteuid() == 0 ? 1 : 0;
  EXPECT_FALSE(ValidateChronyTree(root, other, &contents, &error));
  const std::string binary = root + "/binary";
  {
    std::ofstream out(binary);
    out << "binary";
  }
  ASSERT_EQ(chmod(binary.c_str(), 0755), 0);
  EXPECT_TRUE(ValidateChronyBinary(binary, geteuid(), &error));
  ASSERT_EQ(chmod(binary.c_str(), 0777), 0);
  EXPECT_FALSE(ValidateChronyBinary(binary, geteuid(), &error));
}
TEST(ChronyInvocation, RequiresExactExecutableWrapperAndClockIsolation) {
  const std::string binary = "/run/test-chronyd", wrapper = "/test/main.conf";
  std::vector<std::string> arguments{binary, "-n", "-x",   "-u",
                                     "root", "-f", wrapper};
  std::string error;
  EXPECT_TRUE(ValidateChronyInvocation(binary, arguments, binary, wrapper, true,
                                       &error));
  arguments.back() = "/unexpected.conf";
  EXPECT_FALSE(ValidateChronyInvocation(binary, arguments, binary, wrapper,
                                        true, &error));
  arguments.back() = wrapper;
  arguments.push_back("secret-directive");
  EXPECT_FALSE(ValidateChronyInvocation(binary, arguments, binary, wrapper,
                                        true, &error));
  EXPECT_EQ(error.find("secret-directive"), std::string::npos);
  arguments.pop_back();
  arguments.erase(arguments.begin() + 2);
  EXPECT_FALSE(ValidateChronyInvocation(binary, arguments, binary, wrapper,
                                        true, &error));
}
TEST(ChronyServicePolicy, RejectsHooksOverridesAndClockPrivileges) {
  native::UnitPolicy policy;
  policy.fragment_path = "/run/systemd/system/test.service";
  policy.type = "notify";
  policy.restart = "no";
  policy.no_new_privileges = true;
  std::string error;
  EXPECT_TRUE(ValidateChronyUnitPolicy(policy, policy.fragment_path, &error));
  for (auto field :
       {&native::UnitPolicy::auxiliary_commands, &native::UnitPolicy::drop_ins,
        &native::UnitPolicy::environment,
        &native::UnitPolicy::filesystem_override,
        &native::UnitPolicy::privileged_start_flags}) {
    auto altered = policy;
    altered.*field = true;
    EXPECT_FALSE(
        ValidateChronyUnitPolicy(altered, policy.fragment_path, &error));
  }
  policy.capability_bounding_set = std::uint64_t{1} << 25;
  EXPECT_FALSE(ValidateChronyUnitPolicy(policy, policy.fragment_path, &error));
}
}  // namespace
}  // namespace dang::ntp

// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "plugins/system/src/platform.h"

namespace dang::system {
namespace {

class ScopedPowerGuard {
 public:
  ScopedPowerGuard() {
    if (const char* value = std::getenv("DANG_SYSTEM_ALLOW_POWER"))
      previous_ = value;
  }
  ~ScopedPowerGuard() {
    if (previous_)
      setenv("DANG_SYSTEM_ALLOW_POWER", previous_->c_str(), 1);
    else
      unsetenv("DANG_SYSTEM_ALLOW_POWER");
  }

 private:
  std::optional<std::string> previous_;
};

TEST(SystemPlatformPolicyTest, RejectsPowerOperationBeforeNativeApiCall) {
  ScopedPowerGuard guard;
  unsetenv("DANG_SYSTEM_ALLOW_POWER");
  bool called = false;
  std::string error;

  EXPECT_FALSE(
      RequestPowerOperation(true, &error, [&called](bool, std::string*) {
        called = true;
        return true;
      }));
  EXPECT_FALSE(called);
  EXPECT_NE(error.find("disabled"), std::string::npos) << error;
}

TEST(SystemPlatformPolicyTest, RoutesRestartAndPowerOffToNativeApi) {
  ScopedPowerGuard guard;
  setenv("DANG_SYSTEM_ALLOW_POWER", "1", 1);
  std::vector<bool> operations;
  const PowerOperator capture = [&operations](bool restart, std::string*) {
    operations.push_back(restart);
    return true;
  };
  std::string error;

  EXPECT_TRUE(RequestPowerOperation(true, &error, capture));
  EXPECT_TRUE(RequestPowerOperation(false, &error, capture));
  EXPECT_EQ(operations, (std::vector<bool>{true, false}));
}

TEST(SystemPlatformPolicyTest, PreservesNativeApiFailureDiagnostic) {
  ScopedPowerGuard guard;
  setenv("DANG_SYSTEM_ALLOW_POWER", "1", 1);
  std::string error;

  EXPECT_FALSE(
      RequestPowerOperation(false, &error, [](bool, std::string* native_error) {
        *native_error = "injected reboot API failure";
        return false;
      }));
  EXPECT_EQ(error, "injected reboot API failure");
}

TEST(SystemPlatformPolicyTest, RoutesNtpStateToNativeApi) {
  std::vector<bool> operations;
  const NtpServiceOperator capture =
      [&operations](bool enabled, std::string*) {
        operations.push_back(enabled);
        return true;
      };
  std::string error;

  EXPECT_TRUE(RequestNtpServiceOperation(true, &error, capture));
  EXPECT_TRUE(RequestNtpServiceOperation(false, &error, capture));
  EXPECT_EQ(operations, (std::vector<bool>{true, false}));
}

TEST(SystemPlatformPolicyTest, PreservesNtpApiFailureDiagnostic) {
  std::string error;
  EXPECT_FALSE(RequestNtpServiceOperation(
      true, &error, [](bool, std::string* native_error) {
        *native_error = "injected NTP API failure";
        return false;
      }));
  EXPECT_EQ(error, "injected NTP API failure");
}

}  // namespace
}  // namespace dang::system

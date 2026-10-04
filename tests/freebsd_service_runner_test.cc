// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <signal.h>
#include <sys/wait.h>

#include <gtest/gtest.h>

#include <string>

#include "plugins/system/src/freebsd/service_runner.h"

namespace dang::system {
namespace {

TEST(FreeBsdServiceRunnerTest, AcceptsOnlyZeroExitStatus) {
  std::string error;
  EXPECT_TRUE(InterpretFreeBsdServiceStatus(W_EXITCODE(0, 0), &error));
  EXPECT_FALSE(InterpretFreeBsdServiceStatus(W_EXITCODE(7, 0), &error));
  EXPECT_NE(error.find("status 7"), std::string::npos) << error;
}

TEST(FreeBsdServiceRunnerTest, ReportsTerminatingSignal) {
  std::string error;
  EXPECT_FALSE(InterpretFreeBsdServiceStatus(SIGTERM, &error));
  EXPECT_NE(error.find("signal"), std::string::npos) << error;
  EXPECT_NE(error.find(std::to_string(SIGTERM)), std::string::npos) << error;
}

}  // namespace
}  // namespace dang::system

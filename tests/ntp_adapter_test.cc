// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/** @file Tests daemon selection and fail-closed rollback checkpoints. */

#include "plugins/ntp/src/ntp_adapter.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace dang::ntp {
namespace {

AdapterDescriptor Select(Platform platform, std::string_view requested) {
  AdapterDescriptor descriptor;
  std::string error;
  std::string path;
  EXPECT_TRUE(SelectAdapter(platform, requested, &descriptor, &error, &path))
      << path << ": " << error;
  return descriptor;
}

TEST(NtpAdapterTest, SelectsOnlyThePlatformDefaultDaemon) {
  EXPECT_EQ(Select(Platform::kLinux, "auto").daemon, Daemon::kChrony);
  EXPECT_EQ(Select(Platform::kLinux, "chrony").daemon, Daemon::kChrony);
  EXPECT_EQ(Select(Platform::kFreeBsd, "auto").daemon, Daemon::kNtpd);
  EXPECT_EQ(Select(Platform::kFreeBsd, "ntpd").daemon, Daemon::kNtpd);
}

TEST(NtpAdapterTest, RejectsCrossPlatformAndUnknownDaemons) {
  AdapterDescriptor descriptor;
  std::string error;
  std::string path;
  EXPECT_FALSE(
      SelectAdapter(Platform::kLinux, "ntpd", &descriptor, &error, &path));
  EXPECT_EQ(path, "/ietf-ntp:ntp");
  EXPECT_NE(error.find("not supported on Linux"), std::string::npos);

  error.clear();
  path.clear();
  EXPECT_FALSE(
      SelectAdapter(Platform::kFreeBsd, "chrony", &descriptor, &error, &path));
  EXPECT_EQ(path, "/ietf-ntp:ntp");
  EXPECT_NE(error.find("not supported on FreeBSD"), std::string::npos);

  EXPECT_FALSE(SelectAdapter(Platform::kLinux, "vendor-daemon", &descriptor,
                             &error, &path));
}

TEST(NtpAdapterTest, UnsupportedBuildHasNoImplicitNativeClaim) {
  const AdapterDescriptor descriptor = Select(Platform::kUnsupported, "auto");
  EXPECT_EQ(descriptor.daemon, Daemon::kUnavailable);
  EXPECT_FALSE(descriptor.configuration_ready);
  EXPECT_FALSE(descriptor.operational_ready);
}

TEST(NtpAdapterTest, NativeFactoryMatchesBuildPlatformAndFailsClosed) {
  std::string error;
  std::string path;
  std::unique_ptr<NativeAdapter> adapter =
      CreateNativeAdapter("auto", &error, &path);
  ASSERT_TRUE(adapter) << path << ": " << error;
  EXPECT_EQ(adapter->descriptor().platform, NativePlatform());
  EXPECT_FALSE(adapter->descriptor().configuration_ready);
  EXPECT_FALSE(adapter->descriptor().operational_ready);

  Config before;
  Config proposed;
  proposed.present = true;
  PlannedAction action{.id = "configure-listener-port",
                       .description = "configure the listener",
                       .dependencies = {}};
  AdapterCheckpoint checkpoint;
  EXPECT_FALSE(adapter->ApplyAction(before, proposed, action, &checkpoint,
                                    &error, &path));
  EXPECT_EQ(checkpoint.action_id, action.id);
  EXPECT_FALSE(checkpoint.applied);
  EXPECT_TRUE(checkpoint.before_image.empty());
  EXPECT_EQ(path, "/ietf-ntp:ntp/port");
  EXPECT_NE(error.find("not implemented"), std::string::npos);

  error.clear();
  path.clear();
  EXPECT_FALSE(adapter->RollbackAction(before, proposed, action, &checkpoint,
                                       &error, &path));
  EXPECT_EQ(path, "/ietf-ntp:ntp/port");
  EXPECT_NE(error.find("no acknowledged native before-image"),
            std::string::npos);

  std::string xml = "must be cleared";
  EXPECT_FALSE(adapter->OperationalXml(&xml, &error, &path));
  EXPECT_TRUE(xml.empty());
  EXPECT_EQ(path, "/ietf-ntp:ntp/clock-state");
}

}  // namespace
}  // namespace dang::ntp

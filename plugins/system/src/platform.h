// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANG_PLUGINS_SYSTEM_PLATFORM_H_
#define DANG_PLUGINS_SYSTEM_PLATFORM_H_

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "plugins/system/src/system_config.h"

namespace dang::system {

/** OS-specific files and NTP service commands used by the portable layer. */
struct PlatformLayout {
  std::filesystem::path ntp_configuration;
  std::filesystem::path hostname_configuration;
  std::string ntp_reload_command;
  std::string ntp_stop_command;
};

/** Native layout selected by the Linux or FreeBSD translation unit. */
[[nodiscard]] PlatformLayout NativePlatformLayout();

/** Complete recoverable state for a regular file, symlink, or absent path. */
struct FileSnapshot {
  std::filesystem::path path;
  bool existed = false;
  bool symbolic_link = false;
  std::filesystem::path link_target;
  std::string contents;
};

/** Platform transaction state retained from prepare through release. */
struct PreparedPlatform {
  Config before;
  Config proposed;
  std::filesystem::path root;
  PlatformLayout layout;
  std::vector<FileSnapshot> files;
  std::string old_hostname;
  bool applied = false;
};

[[nodiscard]] bool PreparePlatform(const Config& before, const Config& proposed,
                                   PreparedPlatform* prepared,
                                   std::string* error, std::string* path);
/** Checks paths, values, and command availability without changing the host. */
[[nodiscard]] bool ValidatePlatform(const PreparedPlatform& prepared,
                                    std::string* error, std::string* path);
[[nodiscard]] bool ApplyPlatform(PreparedPlatform* prepared, std::string* error,
                                 std::string* path);
/** Restores all captured files and runtime values after a completed apply. */
[[nodiscard]] bool RollbackPlatform(PreparedPlatform* prepared,
                                    std::string* error, std::string* path);

/** RFC 7317 system-state XML populated from uname(2), clock_gettime(2). */
[[nodiscard]] std::string OperationalStateXml();

/** Implements set-current-datetime after parsing its RFC 3339 input leaf. */
[[nodiscard]] bool SetCurrentDatetime(std::string_view xml, std::string* error);
/** Injectable native power boundary used to test policy without rebooting. */
using PowerOperator = std::function<bool(bool restart, std::string* error)>;

/** Requests orderly restart or power-off through the native service manager. */
[[nodiscard]] bool NativePowerOperation(bool restart, std::string* error);

/** Enforces the deployment guard before invoking a native power operation. */
[[nodiscard]] bool RequestPowerOperation(
    bool restart, std::string* error,
    const PowerOperator& power_operator = NativePowerOperation);

}  // namespace dang::system

#endif  // DANG_PLUGINS_SYSTEM_PLATFORM_H_

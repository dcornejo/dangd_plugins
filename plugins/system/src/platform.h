// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANG_PLUGINS_SYSTEM_PLATFORM_H_
#define DANG_PLUGINS_SYSTEM_PLATFORM_H_

#include "plugins/system/src/system_config.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace dang::system {

struct PlatformLayout {
  std::filesystem::path ntp_configuration;
  std::filesystem::path hostname_configuration;
  std::string ntp_reload_command;
  std::string ntp_stop_command;
  std::string restart_command;
  std::string shutdown_command;
};

/** Native layout selected by the Linux or FreeBSD translation unit. */
[[nodiscard]] PlatformLayout NativePlatformLayout();

struct FileSnapshot {
  std::filesystem::path path;
  bool existed = false;
  bool symbolic_link = false;
  std::filesystem::path link_target;
  std::string contents;
};

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
[[nodiscard]] bool ValidatePlatform(const PreparedPlatform& prepared,
                                    std::string* error, std::string* path);
[[nodiscard]] bool ApplyPlatform(PreparedPlatform* prepared, std::string* error,
                                 std::string* path);
[[nodiscard]] bool RollbackPlatform(PreparedPlatform* prepared,
                                    std::string* error, std::string* path);

/** RFC 7317 system-state XML populated from uname(2), clock_gettime(2). */
[[nodiscard]] std::string OperationalStateXml();

[[nodiscard]] bool SetCurrentDatetime(std::string_view xml,
                                      std::string* error);
[[nodiscard]] bool RequestPowerOperation(bool restart, std::string* error);

}  // namespace dang::system

#endif  // DANG_PLUGINS_SYSTEM_PLATFORM_H_

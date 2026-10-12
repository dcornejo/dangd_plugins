// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0
#ifndef DANG_PLUGINS_NTP_CHRONY_PROCESS_H_
#define DANG_PLUGINS_NTP_CHRONY_PROCESS_H_
#include <sys/types.h>

#include <cstdint>
#include <string>
#include <vector>
namespace dang::ntp {
/**
 * Pin a live Linux process with pidfd_open, compare its executable inode,
 * exact NUL-delimited argv and all four UIDs through a retained proc directory,
 * and reject exit during observation. No process is signalled or spawned.
 */
bool ValidateChronyProcess(std::uint32_t pid, const std::string& executable,
                           const std::vector<std::string>& arguments,
                           uid_t owner, std::string* error);
}  // namespace dang::ntp
#endif

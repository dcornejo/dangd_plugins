// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANG_PLUGINS_RIB_PLATFORM_EXECUTOR_H_
#define DANG_PLUGINS_RIB_PLATFORM_EXECUTOR_H_

#include <functional>
#include <string>
#include <vector>

#include "plugins/rib/src/platform_command.h"

namespace dang::rib {

enum class NativePlatform { kLinux, kFreeBsd };

using CommandRunner =
    std::function<bool(const NativeCommand&, std::string* error)>;

struct ExecutionResult {
  bool ok = false;
  std::string error;
  std::string error_path;
  std::vector<std::string> rollback_failures;
};

/** Executes argv directly with posix_spawnp(3), never through a shell. */
[[nodiscard]] bool RunNativeCommand(const NativeCommand& command,
                                    std::string* error);

/**
 * Applies a prepared plan and compensates completed changes in reverse order.
 * A caller-provided runner makes every failure boundary deterministic in unit
 * tests; production passes RunNativeCommand.
 */
[[nodiscard]] ExecutionResult ExecuteChanges(
    NativePlatform platform, const std::vector<Change>& changes,
    const CommandRunner& runner = RunNativeCommand);

}  // namespace dang::rib

#endif  // DANG_PLUGINS_RIB_PLATFORM_EXECUTOR_H_

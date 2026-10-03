// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANG_PLUGINS_RIB_PLATFORM_EXECUTOR_H_
#define DANG_PLUGINS_RIB_PLATFORM_EXECUTOR_H_

#include <functional>
#include <string>
#include <vector>

#include "plugins/rib/src/platform_command.h"

namespace dang::rib {

/** Supported host command dialects; selected at plugin build time. */
enum class NativePlatform { kLinux, kFreeBsd };

/** Injectable argv boundary used to exercise partial failures safely. */
using CommandRunner =
    std::function<bool(const NativeCommand&, std::string* error)>;

/** Apply outcome, primary failure, and any failures while compensating it. */
struct ExecutionResult {
  bool ok = false;
  std::string error;
  std::string error_path;
  std::vector<std::string> rollback_failures;
};

/**
 * Applies a prepared plan and compensates completed changes in reverse order.
 * A caller-provided argv runner makes every failure boundary deterministic in
 * portable unit tests. With no runner, production uses acknowledged route
 * netlink on Linux and FreeBSD and does not create a child process.
 */
[[nodiscard]] ExecutionResult ExecuteChanges(NativePlatform platform,
                                             const std::vector<Change>& changes,
                                             const CommandRunner& runner = {});

}  // namespace dang::rib

#endif  // DANG_PLUGINS_RIB_PLATFORM_EXECUTOR_H_

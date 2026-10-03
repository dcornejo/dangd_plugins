// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/**
 * @file
 * Executes prepared RIB changes and compensates a partial failure in reverse
 * order. Production mutations use acknowledged route netlink on both supported
 * systems; the argv mapping remains only as an injectable unit-test adapter.
 */

#include "plugins/rib/src/platform_executor.h"

namespace dang::rib {
namespace {

bool Commands(NativePlatform platform, const std::vector<Change>& changes,
              std::vector<NativeCommand>* commands, std::string* error,
              std::string* path) {
  return platform == NativePlatform::kLinux
             ? BuildLinuxCommands(changes, commands, error, path)
             : BuildFreeBsdCommands(changes, commands, error, path);
}

bool Validate(NativePlatform platform, const std::vector<Change>& changes,
              std::string* error, std::string* path) {
  return platform == NativePlatform::kLinux
             ? ValidateLinuxChanges(changes, error, path)
             : ValidateFreeBsdChanges(changes, error, path);
}

Change Inverse(const Change& change) {
  return {change.kind == ChangeKind::kDelete ? ChangeKind::kInstall
                                             : ChangeKind::kDelete,
          change.route};
}

bool RunNativeChange(NativePlatform platform, const Change& change,
                     std::string* error) {
  return platform == NativePlatform::kLinux
             ? ApplyLinuxRouteChange(change, error)
             : ApplyFreeBsdRouteChange(change, error);
}

}  // namespace

ExecutionResult ExecuteChanges(NativePlatform platform,
                               const std::vector<Change>& changes,
                               const CommandRunner& runner) {
  ExecutionResult result;
  std::vector<NativeCommand> commands;
  if (runner) {
    if (!Commands(platform, changes, &commands, &result.error,
                  &result.error_path))
      return result;
  } else if (!Validate(platform, changes, &result.error, &result.error_path)) {
    return result;
  }
  std::size_t completed = 0;
  for (; completed < changes.size(); ++completed) {
    std::string command_error;
    const bool changed =
        runner ? runner(commands[completed], &command_error)
               : RunNativeChange(platform, changes[completed], &command_error);
    if (changed) continue;
    result.error = "route change failed: " + command_error;
    result.error_path = "/ietf-i2rs-rib:routing-instance/rib-list/route-list";
    break;
  }
  if (completed == changes.size()) {
    result.ok = true;
    return result;
  }

  while (completed > 0) {
    --completed;
    const Change inverse = Inverse(changes[completed]);
    std::string rollback_error;
    bool restored = false;
    if (runner) {
      std::vector<NativeCommand> compensation;
      std::string build_error;
      std::string build_path;
      if (!Commands(platform, {inverse}, &compensation, &build_error,
                    &build_path)) {
        result.rollback_failures.push_back(build_error);
        continue;
      }
      restored = runner(compensation.front(), &rollback_error);
    } else {
      restored = RunNativeChange(platform, inverse, &rollback_error);
    }
    if (!restored)
      result.rollback_failures.push_back(Describe(inverse) + ": " +
                                         rollback_error);
  }
  return result;
}

}  // namespace dang::rib

// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/**
 * @file
 * Executes prepared RIB changes and compensates a partial failure in reverse
 * order. Linux production mutations use acknowledged rtnetlink. FreeBSD still
 * passes fixed argv directly to posix_spawnp(3); neither path invokes a shell.
 */

#include "plugins/rib/src/platform_executor.h"

#include <spawn.h>
#include <sys/wait.h>

#include <cerrno>
#include <cstring>
#include <sstream>

extern char** environ;

namespace dang::rib {
namespace {

bool Commands(NativePlatform platform, const std::vector<Change>& changes,
              std::vector<NativeCommand>* commands, std::string* error,
              std::string* path) {
  return platform == NativePlatform::kLinux
             ? BuildLinuxCommands(changes, commands, error, path)
             : BuildFreeBsdCommands(changes, commands, error, path);
}

Change Inverse(const Change& change) {
  return {change.kind == ChangeKind::kDelete ? ChangeKind::kInstall
                                             : ChangeKind::kDelete,
          change.route};
}

bool RunNativeChange(NativePlatform platform, const Change& change,
                     const NativeCommand& command, std::string* error) {
  if (platform == NativePlatform::kLinux)
    return ApplyLinuxRouteChange(change, error);
  return RunNativeCommand(command, error);
}

}  // namespace

bool RunNativeCommand(const NativeCommand& command, std::string* error) {
  if (!error || command.arguments.empty() || command.arguments.front().empty())
    return false;
  std::vector<char*> arguments;
  arguments.reserve(command.arguments.size() + 1);
  for (const std::string& argument : command.arguments)
    arguments.push_back(const_cast<char*>(argument.c_str()));
  arguments.push_back(nullptr);

  pid_t child = -1;
  const int spawned = posix_spawnp(&child, arguments[0], nullptr, nullptr,
                                   arguments.data(), environ);
  if (spawned != 0) {
    *error = "cannot start " + command.arguments.front() + ": " +
             std::strerror(spawned);
    return false;
  }
  int status = 0;
  while (waitpid(child, &status, 0) < 0) {
    if (errno == EINTR) continue;
    *error =
        "cannot wait for route command: " + std::string(std::strerror(errno));
    return false;
  }
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return true;
  std::ostringstream message;
  message << command.arguments.front() << " failed";
  if (WIFEXITED(status))
    message << " with exit status " << WEXITSTATUS(status);
  else if (WIFSIGNALED(status))
    message << " after signal " << WTERMSIG(status);
  *error = message.str();
  return false;
}

ExecutionResult ExecuteChanges(NativePlatform platform,
                               const std::vector<Change>& changes,
                               const CommandRunner& runner) {
  ExecutionResult result;
  std::vector<NativeCommand> commands;
  if (!Commands(platform, changes, &commands, &result.error,
                &result.error_path))
    return result;
  std::size_t completed = 0;
  for (; completed < commands.size(); ++completed) {
    std::string command_error;
    const bool changed =
        runner ? runner(commands[completed], &command_error)
               : RunNativeChange(platform, changes[completed],
                                 commands[completed], &command_error);
    if (changed) continue;
    result.error = "route change failed: " + command_error;
    result.error_path = "/ietf-i2rs-rib:routing-instance/rib-list/route-list";
    break;
  }
  if (completed == commands.size()) {
    result.ok = true;
    return result;
  }

  while (completed > 0) {
    --completed;
    const Change inverse = Inverse(changes[completed]);
    std::vector<NativeCommand> compensation;
    std::string build_error;
    std::string build_path;
    if (!Commands(platform, {inverse}, &compensation, &build_error,
                  &build_path)) {
      result.rollback_failures.push_back(build_error);
      continue;
    }
    std::string rollback_error;
    const bool restored =
        runner ? runner(compensation.front(), &rollback_error)
               : RunNativeChange(platform, inverse, compensation.front(),
                                 &rollback_error);
    if (!restored)
      result.rollback_failures.push_back(Describe(inverse) + ": " +
                                         rollback_error);
  }
  return result;
}

}  // namespace dang::rib

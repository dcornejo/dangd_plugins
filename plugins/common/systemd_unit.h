// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0
#ifndef DANG_PLUGINS_COMMON_SYSTEMD_UNIT_H_
#define DANG_PLUGINS_COMMON_SYSTEMD_UNIT_H_
#include <string>
#include <string_view>
#include <vector>
namespace dang::native {
enum class UnitOperation { kStart, kStop, kReloadOrRestart };
/** Configured command returned by the native systemd Service interface. */
struct UnitCommand {
  std::string executable;
  std::vector<std::string> arguments;
  bool ignore_failure = false;
};
/** Read structured ExecStart commands without invoking a command-line tool. */
bool SystemdExecStart(std::string_view unit, std::vector<UnitCommand>* commands,
                      std::string* error);
/** Read stable active/inactive/failed state; reject transitional states. */
bool SystemdUnitActive(std::string_view unit, bool* active, std::string* error);
/** Await the exact job and observed state; failure may still mutate service. */
bool ChangeSystemdUnit(std::string_view unit, UnitOperation operation,
                       std::string* error);
}  // namespace dang::native
#endif

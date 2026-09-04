// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANG_PLUGINS_RIB_PLATFORM_COMMAND_H_
#define DANG_PLUGINS_RIB_PLATFORM_COMMAND_H_

#include <string>
#include <vector>

#include "plugins/rib/src/rib_config.h"

namespace dang::rib {

/** An argv vector, never a shell command string. */
struct NativeCommand {
  std::vector<std::string> arguments;
};

[[nodiscard]] bool BuildLinuxCommands(const std::vector<Change>& changes,
                                      std::vector<NativeCommand>* commands,
                                      std::string* error,
                                      std::string* error_path);

[[nodiscard]] bool BuildFreeBsdCommands(const std::vector<Change>& changes,
                                        std::vector<NativeCommand>* commands,
                                        std::string* error,
                                        std::string* error_path);

}  // namespace dang::rib

#endif  // DANG_PLUGINS_RIB_PLATFORM_COMMAND_H_

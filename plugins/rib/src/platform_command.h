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

/** Maps a checked portable plan to Linux iproute2 argv vectors. */
[[nodiscard]] bool BuildLinuxCommands(const std::vector<Change>& changes,
                                      std::vector<NativeCommand>* commands,
                                      std::string* error,
                                      std::string* error_path);

/** Maps a checked portable plan to FreeBSD route(8) argv vectors. */
[[nodiscard]] bool BuildFreeBsdCommands(const std::vector<Change>& changes,
                                        std::vector<NativeCommand>* commands,
                                        std::string* error,
                                        std::string* error_path);

}  // namespace dang::rib

#endif  // DANG_PLUGINS_RIB_PLATFORM_COMMAND_H_

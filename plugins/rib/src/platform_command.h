// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANG_PLUGINS_RIB_PLATFORM_COMMAND_H_
#define DANG_PLUGINS_RIB_PLATFORM_COMMAND_H_

#include <functional>
#include <string>
#include <vector>

#include "plugins/rib/src/rib_config.h"

namespace dang::rib {

/** An argv vector, never a shell command string. */
struct NativeCommand {
  std::vector<std::string> arguments;
};

/** Resolves one unambiguous local address for an interface and IP family. */
using InterfaceAddressResolver = std::function<bool(
    const std::string& interface, const std::string& address_family,
    std::string* address, std::string* error)>;

/** Maps a checked Linux plan to deterministic argv for validation/unit tests. */
[[nodiscard]] bool BuildLinuxCommands(const std::vector<Change>& changes,
                                      std::vector<NativeCommand>* commands,
                                      std::string* error,
                                      std::string* error_path);

/** Applies one Linux route change through rtnetlink and waits for its ACK. */
[[nodiscard]] bool ApplyLinuxRouteChange(const Change& change,
                                         std::string* error);

/** Maps a checked portable plan to FreeBSD route(8) argv vectors. */
[[nodiscard]] bool BuildFreeBsdCommands(
    const std::vector<Change>& changes, std::vector<NativeCommand>* commands,
    std::string* error, std::string* error_path,
    const InterfaceAddressResolver& resolver = {});

}  // namespace dang::rib

#endif  // DANG_PLUGINS_RIB_PLATFORM_COMMAND_H_

// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANG_PLUGINS_RIB_RIB_RPC_H_
#define DANG_PLUGINS_RIB_RIB_RPC_H_

#include <functional>
#include <string>

#include "plugins/rib/src/platform_executor.h"
#include "plugins/rib/src/route_observer.h"

namespace dang::rib {

/** Executes the supported RFC 8431 route-add RPC and returns its output XML. */
[[nodiscard]] bool InvokeRouteAdd(NativePlatform platform,
                                  const char* input_xml,
                                  std::string* output_xml,
                                  std::string* error,
                                  std::string* error_path,
                                  const CommandRunner& runner = RunNativeCommand);

/** Injectable route inventory used to resolve route-delete prefix requests. */
using RouteObserver =
    std::function<bool(std::vector<ObservedRoute>*, std::string*)>;

/** Executes RFC 8431 route-delete against unambiguous observed routes. */
[[nodiscard]] bool InvokeRouteDelete(
    NativePlatform platform, const char* input_xml, std::string* output_xml,
    std::string* error, std::string* error_path,
    const CommandRunner& runner = RunNativeCommand,
    const RouteObserver& observer = {});

/** Updates prefix-selected routes with a base nexthop or route attributes. */
[[nodiscard]] bool InvokeRouteUpdate(
    NativePlatform platform, const char* input_xml, std::string* output_xml,
    std::string* error, std::string* error_path,
    const CommandRunner& runner = RunNativeCommand,
    const RouteObserver& observer = {});

}  // namespace dang::rib

#endif  // DANG_PLUGINS_RIB_RIB_RPC_H_

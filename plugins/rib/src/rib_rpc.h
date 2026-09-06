// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANG_PLUGINS_RIB_RIB_RPC_H_
#define DANG_PLUGINS_RIB_RIB_RPC_H_

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>

#include "plugins/rib/src/platform_executor.h"
#include "plugins/rib/src/route_observer.h"

namespace dang::rib {

/** Process-local, thread-safe store for RFC 8431 reusable nexthops. */
class NexthopRegistry {
 public:
  struct Entry {
    std::string rib;
    std::optional<std::string> gateway;
    std::optional<std::string> interface;
    bool sharable = false;
  };
  [[nodiscard]] std::optional<std::uint32_t> Add(Entry entry);
  [[nodiscard]] bool Remove(const std::string& rib, std::uint32_t id);
  [[nodiscard]] bool Resolve(const std::string& rib, std::uint32_t id,
                             std::optional<std::string>* gateway,
                             std::optional<std::string>* interface);

 private:
  std::mutex mutex_;
  std::map<std::pair<std::string, std::uint32_t>, Entry> entries_;
  std::uint32_t next_id_ = 1;
};

/** Executes the supported RFC 8431 route-add RPC and returns its output XML. */
[[nodiscard]] bool InvokeRouteAdd(NativePlatform platform,
                                  const char* input_xml,
                                  std::string* output_xml,
                                  std::string* error,
                                  std::string* error_path,
                                  const CommandRunner& runner = RunNativeCommand,
                                  const NexthopResolver& resolver = {});

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
    const RouteObserver& observer = {},
    const NexthopResolver& resolver = {});

/** Validates availability of a native RIB/FIB for the rib-add RPC. */
[[nodiscard]] bool InvokeRibAdd(NativePlatform platform, const char* input_xml,
                                std::string* output_xml, std::string* error,
                                std::string* error_path);

/** Atomically removes every observed route from the selected native RIB/FIB. */
[[nodiscard]] bool InvokeRibDelete(
    NativePlatform platform, const char* input_xml, std::string* output_xml,
    std::string* error, std::string* error_path,
    const CommandRunner& runner = RunNativeCommand,
    const RouteObserver& observer = {});

/** Allocates and retains a portable base nexthop for nh-add. */
[[nodiscard]] bool InvokeNexthopAdd(NexthopRegistry* registry,
                                    const char* input_xml,
                                    std::string* output_xml,
                                    std::string* error,
                                    std::string* error_path);

/** Removes a previously allocated nexthop for nh-delete. */
[[nodiscard]] bool InvokeNexthopDelete(NexthopRegistry* registry,
                                       const char* input_xml,
                                       std::string* output_xml,
                                       std::string* error,
                                       std::string* error_path);

}  // namespace dang::rib

#endif  // DANG_PLUGINS_RIB_RIB_RPC_H_

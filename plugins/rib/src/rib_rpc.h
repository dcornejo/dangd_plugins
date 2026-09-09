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
#include <tuple>

#include "plugins/rib/src/platform_executor.h"
#include "plugins/rib/src/rib_persistence.h"
#include "plugins/rib/src/route_observer.h"

namespace dang::rib {

/** Process-local, thread-safe store for RFC 8431 reusable nexthops. */
class NexthopRegistry {
 public:
  enum class RemoveResult { kRemoved, kMissing, kInUse };
  struct Entry {
    std::string rib;
    std::optional<std::string> gateway;
    std::optional<std::string> interface;
    std::optional<std::string> address_family;
    bool sharable = false;
  };
  [[nodiscard]] std::optional<std::uint32_t> Add(Entry entry);
  [[nodiscard]] RemoveResult Remove(const std::string& rib, std::uint32_t id);
  [[nodiscard]] bool Retain(const std::string& rib, std::uint32_t id);
  void Release(const std::string& rib, std::uint32_t id);
  /** Commits a route binding, consuming one prior Retain for a new reference. */
  void BindRoute(const Route& route,
                 std::optional<std::uint32_t> reserved_reference);
  /** Releases and removes the reference associated with one native route. */
  void ForgetRoute(const Route& route);
  /** Releases all imperative route bindings in a deleted RIB. */
  void ForgetRib(const std::string& rib);
  [[nodiscard]] std::optional<std::uint32_t> RouteReference(
      const Route& route);
  /** Atomically replaces reference counts owned by the applied datastore. */
  [[nodiscard]] bool ReplaceConfigurationReferences(
      const std::vector<std::pair<std::string, std::uint32_t>>& references);
  /** Returns a consistent RIB/family/identifier view for operational output. */
  [[nodiscard]] std::vector<std::tuple<std::string, std::string, std::uint32_t>>
  Snapshot();
  [[nodiscard]] bool Resolve(const std::string& rib, std::uint32_t id,
                             std::optional<std::string>* gateway,
                             std::optional<std::string>* interface);
  /** Captures every durable object, allocation cursor, and route binding. */
  [[nodiscard]] PersistentRegistry PersistentState();
  /** Replaces an empty process registry with validated durable state. */
  [[nodiscard]] bool RestorePersistentState(const PersistentRegistry& state,
                                            std::string* error);

 private:
  std::mutex mutex_;
  std::map<std::pair<std::string, std::uint32_t>, Entry> entries_;
  std::map<std::pair<std::string, std::uint32_t>, std::size_t> references_;
  std::map<std::pair<std::string, std::uint32_t>, std::size_t>
      configuration_references_;
  std::map<std::tuple<std::string, std::string, std::string>, std::uint32_t>
      route_references_;
  std::uint32_t next_id_ = 1;
};

/** Executes the supported RFC 8431 route-add RPC and returns its output XML. */
[[nodiscard]] bool InvokeRouteAdd(NativePlatform platform,
                                  const char* input_xml,
                                  std::string* output_xml,
                                  std::string* error,
                                  std::string* error_path,
                                  const CommandRunner& runner = RunNativeCommand,
                                  const NexthopResolver& resolver = {},
                                  NexthopRegistry* registry = nullptr);

/** Injectable route inventory used to resolve route-delete prefix requests. */
using RouteObserver =
    std::function<bool(std::vector<ObservedRoute>*, std::string*)>;

/** Executes RFC 8431 route-delete against unambiguous observed routes. */
[[nodiscard]] bool InvokeRouteDelete(
    NativePlatform platform, const char* input_xml, std::string* output_xml,
    std::string* error, std::string* error_path,
    const CommandRunner& runner = RunNativeCommand,
    const RouteObserver& observer = {}, NexthopRegistry* registry = nullptr);

/** Updates prefix-selected routes with a base nexthop or route attributes. */
[[nodiscard]] bool InvokeRouteUpdate(
    NativePlatform platform, const char* input_xml, std::string* output_xml,
    std::string* error, std::string* error_path,
    const CommandRunner& runner = RunNativeCommand,
    const RouteObserver& observer = {},
    const NexthopResolver& resolver = {},
    NexthopRegistry* registry = nullptr);

/** Validates availability of a native RIB/FIB for the rib-add RPC. */
[[nodiscard]] bool InvokeRibAdd(NativePlatform platform, const char* input_xml,
                                std::string* output_xml, std::string* error,
                                std::string* error_path);

/** Atomically removes every observed route from the selected native RIB/FIB. */
[[nodiscard]] bool InvokeRibDelete(
    NativePlatform platform, const char* input_xml, std::string* output_xml,
    std::string* error, std::string* error_path,
    const CommandRunner& runner = RunNativeCommand,
    const RouteObserver& observer = {}, NexthopRegistry* registry = nullptr);

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

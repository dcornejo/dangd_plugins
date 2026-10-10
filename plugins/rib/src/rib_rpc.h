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
#include <vector>

#include "plugins/rib/src/platform_executor.h"
#include "plugins/rib/src/rib_persistence.h"
#include "plugins/rib/src/route_observer.h"

namespace dang::rib {

/** One atomic view used to correlate native routes with applied identities. */
struct OperationalRegistryState {
  PersistentRegistry registry;
  std::vector<Route> configuration_routes;
};

/** Process-local, thread-safe store for RFC 8431 reusable nexthops. */
class NexthopRegistry {
 public:
  enum class RegisterRibResult { kRegistered, kExisting, kConflict };
  enum class RemoveResult { kRemoved, kMissing, kInUse };
  struct Entry {
    std::string rib;
    std::optional<std::string> gateway;
    std::optional<std::string> interface;
    std::optional<std::string> address_family;
    bool sharable = false;
    std::optional<std::string> special;
  };
  [[nodiscard]] std::optional<std::uint32_t> Add(Entry entry);
  /** Records the modeled family used by family-neutral nexthops. */
  [[nodiscard]] RegisterRibResult RegisterRib(const std::string& rib,
                                              const std::string& family);
  [[nodiscard]] std::optional<std::string> RibFamily(const std::string& rib);
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
  /** Replaces datastore route bindings used only for live resolution state. */
  [[nodiscard]] bool ReplaceConfigurationRouteBindings(
      const std::vector<Route>& routes);
  [[nodiscard]] bool Resolve(const std::string& rib, std::uint32_t id,
                             std::optional<std::string>* gateway,
                             std::optional<std::string>* interface,
                             std::optional<std::string>* special);
  /** Captures every durable object, allocation cursor, and route binding. */
  [[nodiscard]] PersistentRegistry PersistentState();
  /** Returns durable state plus transient datastore route bindings. */
  [[nodiscard]] PersistentRegistry ResolutionState();
  /** Atomically adds applied datastore route identities to resolution state. */
  [[nodiscard]] OperationalRegistryState OperationalState();
  /** Replaces an empty process registry with validated durable state. */
  [[nodiscard]] bool RestorePersistentState(const PersistentRegistry& state,
                                            std::string* error);
  /** Restores a checkpoint while preserving datastore-owned references. */
  [[nodiscard]] bool ReplacePersistentState(const PersistentRegistry& state,
                                            std::string* error);

 private:
  std::mutex mutex_;
  std::map<std::pair<std::string, std::uint32_t>, Entry> entries_;
  std::map<std::string, std::string> rib_families_;
  std::map<std::pair<std::string, std::uint32_t>, std::size_t> references_;
  std::map<std::pair<std::string, std::uint32_t>, std::size_t>
      configuration_references_;
  std::map<std::tuple<std::string, std::string, std::string, std::uint64_t>,
           std::uint32_t>
      route_references_;
  /**
   * Datastore routes may retain several reusable nexthops through the RFC 8431
   * weighted load-balance list, so this index deliberately permits duplicate
   * route keys with distinct nexthop identifiers.
   */
  std::multimap<
      std::tuple<std::string, std::string, std::string, std::uint64_t>,
      std::uint32_t>
      configuration_route_references_;
  /** Applied datastore routes retained only until the next reconciliation. */
  std::vector<Route> configuration_routes_;
  std::uint32_t next_id_ = 1;
};

/** Makes one complete registry state durable before an RPC is acknowledged. */
using RegistryWriter =
    std::function<bool(const PersistentRegistry&, std::string*)>;

/** Receives a route-change only after its native and durable work succeeds. */
using RouteEventSink = std::function<void(const Route&, bool installed)>;
using RibNameResolver =
    std::function<std::optional<std::string>(const std::string&,
                                             const std::string&)>;
/** Injectable route inventory used by imperative route operations. */
using RouteObserver =
    std::function<bool(std::vector<ObservedRoute>*, std::string*)>;

/** Executes the supported RFC 8431 route-add RPC and returns its output XML. */
[[nodiscard]] bool InvokeRouteAdd(NativePlatform platform,
                                  const char* input_xml,
                                  std::string* output_xml,
                                  std::string* error,
                                  std::string* error_path,
                                  const CommandRunner& runner = {},
                                  const NexthopResolver& resolver = {},
                                  NexthopRegistry* registry = nullptr,
                                  const RegistryWriter& writer = {},
                                  const RouteEventSink& events = {},
                                  const RibNameResolver& native_rib = {},
                                  const RouteObserver& observer = {});

/** Executes RFC 8431 route-delete against unambiguous observed routes. */
[[nodiscard]] bool InvokeRouteDelete(
    NativePlatform platform, const char* input_xml, std::string* output_xml,
    std::string* error, std::string* error_path,
    const CommandRunner& runner = {},
    const RouteObserver& observer = {}, NexthopRegistry* registry = nullptr,
    const RegistryWriter& writer = {}, const RouteEventSink& events = {},
    const RibNameResolver& native_rib = {});

/** Updates prefix-selected routes with a base nexthop or route attributes. */
[[nodiscard]] bool InvokeRouteUpdate(
    NativePlatform platform, const char* input_xml, std::string* output_xml,
    std::string* error, std::string* error_path,
    const CommandRunner& runner = {},
    const RouteObserver& observer = {},
    const NexthopResolver& resolver = {},
    NexthopRegistry* registry = nullptr,
    const RegistryWriter& writer = {}, const RouteEventSink& events = {},
    const RibNameResolver& native_rib = {});

/** Validates availability of a native RIB/FIB for the rib-add RPC. */
[[nodiscard]] bool InvokeRibAdd(NativePlatform platform, const char* input_xml,
                                std::string* output_xml, std::string* error,
                                std::string* error_path,
                                NexthopRegistry* registry = nullptr,
                                const RegistryWriter& writer = {},
                                const RibNameResolver& native_rib = {});

/** Atomically removes every observed route from the selected native RIB/FIB. */
[[nodiscard]] bool InvokeRibDelete(
    NativePlatform platform, const char* input_xml, std::string* output_xml,
    std::string* error, std::string* error_path,
    const CommandRunner& runner = {},
    const RouteObserver& observer = {}, NexthopRegistry* registry = nullptr,
    const RegistryWriter& writer = {}, const RouteEventSink& events = {},
    const RibNameResolver& native_rib = {});

/** Allocates and retains a portable base nexthop for nh-add. */
[[nodiscard]] bool InvokeNexthopAdd(NexthopRegistry* registry,
                                    const char* input_xml,
                                    std::string* output_xml,
                                    std::string* error,
                                    std::string* error_path,
                                    const RegistryWriter& writer = {},
                                    const RibNameResolver& native_rib = {});

/** Removes a previously allocated nexthop for nh-delete. */
[[nodiscard]] bool InvokeNexthopDelete(NexthopRegistry* registry,
                                       const char* input_xml,
                                       std::string* output_xml,
                                       std::string* error,
                                       std::string* error_path,
                                       const RegistryWriter& writer = {});

}  // namespace dang::rib

#endif  // DANG_PLUGINS_RIB_RIB_RPC_H_

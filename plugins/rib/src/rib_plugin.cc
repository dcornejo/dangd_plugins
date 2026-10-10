// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/** @file Transactional dangd adapter for the supported RFC 8431 route slice. */

#include "dangd/plugin_api.h"
#include "plugins/rib/src/platform_command.h"
#include "plugins/rib/src/platform_executor.h"
#include "plugins/rib/src/rib_config.h"
#include "plugins/rib/src/rib_persistence.h"
#include "plugins/rib/src/rib_mapping.h"
#include "plugins/rib/src/rib_rpc.h"
#include "plugins/rib/src/route_observer.h"
#include "rib_model_sources.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#if defined(__FreeBSD__)
#include <sys/sysctl.h>
#endif

namespace {
using namespace dang::rib;

#if defined(__linux__)
constexpr NativePlatform kPlatform = NativePlatform::kLinux;
#elif defined(__FreeBSD__)
constexpr NativePlatform kPlatform = NativePlatform::kFreeBsd;
#else
#error "The RFC 8431 plugin supports only Linux and FreeBSD"
#endif

struct Context {
  NexthopRegistry nexthops;
  RibMapping rib_mapping;
  std::once_flag load_once;
  std::mutex rpc_mutex;
  std::mutex notification_mutex;
  struct PendingNotification {
    std::string name;
    std::string xml;
  };
  std::deque<PendingNotification> notifications;
  RouteChangeTracker route_changes;
  NexthopResolutionTracker nexthop_resolutions;
  std::filesystem::path registry_path;
  std::filesystem::path mapping_path;
  std::string load_error;
  bool loaded = false;
};
using Reference = std::pair<std::string, std::uint32_t>;
struct Prepared {
  std::vector<Change> changes;
  std::vector<Change> native_changes;
  std::vector<Route> before_routes;
  std::vector<Route> proposed_routes;
  std::vector<Reference> before_references;
  std::vector<Reference> proposed_references;
  NexthopRegistry* registry = nullptr;
  bool applied = false;
};
Context plugin_context;
bool EnsureRegistry(Context* owner, std::string* error) {
  std::call_once(owner->load_once, [owner] {
    const char* configured = std::getenv("DANG_RIB_REGISTRY_FILE");
    owner->registry_path = configured && *configured
                               ? configured
                               : "/var/lib/dangd/rib-nexthops.json";
    const char* configured_mapping = std::getenv("DANG_RIB_MAP_FILE");
    owner->mapping_path = configured_mapping && *configured_mapping
                              ? configured_mapping
                              : std::filesystem::path{};
    PersistentRegistry state;
    owner->loaded = LoadRibMapping(owner->mapping_path, &owner->rib_mapping,
                                   &owner->load_error) &&
                    LoadRegistry(owner->registry_path, &state,
                                 &owner->load_error) &&
                    ValidateRegistryRibMappings(
                        state, owner->rib_mapping, kPlatform,
                        &owner->load_error) &&
                    owner->nexthops.RestorePersistentState(
                        state, &owner->load_error);
  });
  if (!owner->loaded && error) *error = owner->load_error;
  return owner->loaded;
}

std::optional<std::vector<Change>> NativeChanges(
    const RibMapping& mapping, const std::vector<Change>& changes,
    std::string* error) {
  std::vector<Change> result = changes;
  for (Change& change : result) {
    const auto native = mapping.ToNative(change.route.rib, kPlatform,
                                         change.route.address_family);
    if (!native) {
      if (error)
        *error = "RIB name '" + change.route.rib +
                 "' has no mapping for this platform";
      return std::nullopt;
    }
    change.route.rib = *native;
  }
  return result;
}

void ModelObservedRoutes(const RibMapping& mapping,
                         std::vector<ObservedRoute>* routes) {
  for (ObservedRoute& route : *routes)
    route.route.rib = mapping.ToModeled(route.route.rib, kPlatform,
                                       route.route.address_family);
}

RibNameResolver NativeRib(Context* owner) {
  return [owner](const std::string& name, const std::string& family) {
    return owner->rib_mapping.ToNative(name, kPlatform, family);
  };
}

RouteObserver Observer(Context* owner) {
  return [owner](std::vector<ObservedRoute>* routes, std::string* error) {
    bool ok = false;
    if (kPlatform == NativePlatform::kLinux) {
      ok = ObserveLinuxRoutes(routes, error);
    } else {
      routes->clear();
#if defined(__FreeBSD__)
      unsigned fib_count = 0;
      size_t size = sizeof(fib_count);
      if (sysctlbyname("net.fibs", &fib_count, &size, nullptr, 0) != 0 ||
          fib_count == 0U) {
        *error = "cannot enumerate FreeBSD FIBs";
        return false;
      }
      ok = true;
      for (std::uint32_t fib = 0; fib < fib_count; ++fib) {
        std::vector<ObservedRoute> observed;
        if (!ObserveFreeBsdRoutesForFib(fib, &observed, error)) {
          ok = false;
          break;
        }
        routes->insert(routes->end(), std::make_move_iterator(observed.begin()),
                       std::make_move_iterator(observed.end()));
      }
#endif
    }
    if (ok) ModelObservedRoutes(owner->rib_mapping, routes);
    return ok;
  };
}
RegistryWriter Writer(Context* owner) {
  return [owner](const PersistentRegistry& state, std::string* error) {
    return SaveRegistry(owner->registry_path, state, error);
  };
}
NexthopResolver Resolver(Context* owner) {
  return [owner](const std::string& rib, std::uint32_t id,
                 std::optional<std::string>* gateway,
                 std::optional<std::string>* interface,
                 std::optional<std::string>* special) {
    return owner->nexthops.Resolve(rib, id, gateway, interface, special);
  };
}
std::vector<Reference> References(const Config& config) {
  std::vector<Reference> result;
  for (const Route& route : config.routes) {
    if (route.nexthop_ref) result.emplace_back(route.rib, *route.nexthop_ref);
    for (const WeightedNexthop& member : route.load_balance)
      result.emplace_back(route.rib, member.id);
  }
  return result;
}
bool RetainAll(NexthopRegistry* registry, const std::vector<Reference>& refs) {
  std::size_t retained = 0;
  for (; retained < refs.size(); ++retained)
    if (!registry->Retain(refs[retained].first, refs[retained].second)) break;
  if (retained == refs.size()) return true;
  while (retained > 0) {
    --retained;
    registry->Release(refs[retained].first, refs[retained].second);
  }
  return false;
}
void ReleaseAll(NexthopRegistry* registry, const std::vector<Reference>& refs) {
  for (const auto& [rib, id] : refs) registry->Release(rib, id);
}
thread_local std::string message;
thread_local std::string path;
thread_local std::string operational_xml;
thread_local std::string rpc_output_xml;
thread_local std::string notification_xml;
thread_local std::string notification_name;

constexpr std::size_t kMaximumPendingNotifications = 1024;
RouteEventSink EventSink(Context* owner) {
  return [owner](const Route& route, bool installed) {
    std::lock_guard lock(owner->notification_mutex);
    owner->route_changes.ApplyManaged(route, installed);
    if (owner->notifications.size() >= kMaximumPendingNotifications) return;
    owner->notifications.push_back(
        {"route-change", SerializeRouteChange(route, installed)});
  };
}

int Fail(DangPluginErrorV1* error, std::string text, std::string where = {}) {
  message = std::move(text); path = std::move(where);
  if (error) { error->message = message.c_str(); error->instance_path = path.empty() ? nullptr : path.c_str(); }
  return 0;
}
size_t SourceCount(void*) { return 2; }
int SourceAt(void*, size_t index, DangYangSourceV1* out, DangPluginErrorV1* error) {
  static constexpr const char* kRibFeatures[]{"nexthop-load-balance"};
  static const DangYangSourceV1 sources[]{
    {"ietf-i2rs-rib", "2018-09-13", kIetfI2rsRibYang, std::strlen(kIetfI2rsRibYang), "RFC 8431", DANG_YANG_IMPLEMENTED_V1, kRibFeatures, 1},
    {"ietf-interfaces", "2018-02-20", kIetfInterfacesYang, std::strlen(kIetfInterfacesYang), "RFC 8343", DANG_YANG_IMPORT_ONLY_V1, nullptr, 0}};
  if (!out || index >= 2) return Fail(error, "RIB YANG source index is invalid");
  *out = sources[index]; return 1;
}
size_t DependencyCount(void*) { return 0; }
const char* DependencyAt(void*, size_t) { return nullptr; }

int Prepare(void* raw_context, const DangTransactionV1* tx, void** out, DangPluginErrorV1* error) {
  if (!tx || !tx->before_xml || !tx->proposed_xml || !out) return Fail(error, "RIB transaction input is incomplete");
  std::string load_error;
  if (!EnsureRegistry(static_cast<Context*>(raw_context), &load_error))
    return Fail(error, "cannot load reusable nexthop registry: " + load_error,
                "/ietf-i2rs-rib:routing-instance");
  Config before, proposed; std::string why, where;
  const auto resolver = Resolver(static_cast<Context*>(raw_context));
  if (!ParseConfig(tx->before_xml, &before, &why, &where, resolver)) return Fail(error, why, where);
  if (!ParseConfig(tx->proposed_xml, &proposed, &why, &where, resolver)) return Fail(error, why, where);
  auto* owner = static_cast<Context*>(raw_context);
  const std::vector<Change> changes = PlanChanges(before, proposed);
  auto native_changes = NativeChanges(owner->rib_mapping, changes, &why);
  if (!native_changes)
    return Fail(error, why,
                "/ietf-i2rs-rib:routing-instance/rib-list/name");
  auto* prepared = new (std::nothrow) Prepared{
      changes, std::move(*native_changes), before.routes, proposed.routes,
      References(before), References(proposed),
      &owner->nexthops};
  if (!prepared) return Fail(error, "cannot retain RIB transaction plan");
  if (!RetainAll(prepared->registry, prepared->before_references)) {
    delete prepared;
    return Fail(error, "cannot reserve a referenced nexthop",
                "/ietf-i2rs-rib:routing-instance/rib-list/route-list/nexthop");
  }
  if (!RetainAll(prepared->registry, prepared->proposed_references)) {
    ReleaseAll(prepared->registry, prepared->before_references);
    delete prepared;
    return Fail(error, "cannot reserve a referenced nexthop",
                "/ietf-i2rs-rib:routing-instance/rib-list/route-list/nexthop");
  }
  *out = prepared; return 1;
}
int Validate(void*, void* raw, DangPluginErrorV1* error) {
  auto* prepared = static_cast<Prepared*>(raw); if (!prepared) return Fail(error, "RIB transaction plan is missing");
  std::string why, where;
  const bool ok = kPlatform == NativePlatform::kLinux
                      ? ValidateLinuxChanges(prepared->native_changes, &why,
                                             &where)
                      : ValidateFreeBsdChanges(prepared->native_changes, &why,
                                               &where);
  return ok ? 1 : Fail(error, why, where);
}
int Apply(void*, void* raw, DangPluginErrorV1* error) {
  auto* prepared = static_cast<Prepared*>(raw); if (!prepared) return Fail(error, "RIB transaction plan is missing");
  if (prepared->applied) return 1;
  auto result = ExecuteChanges(kPlatform, prepared->native_changes);
  if (!result.ok) return Fail(error, result.error, result.error_path);
  prepared->applied = true; return 1;
}
int Rollback(void*, void* raw, DangPluginErrorV1* error) {
  auto* prepared = static_cast<Prepared*>(raw); if (!prepared) return Fail(error, "RIB transaction plan is missing");
  if (!prepared->applied) return 1;
  std::vector<Change> inverse;
  for (auto i = prepared->native_changes.rbegin(); i != prepared->native_changes.rend(); ++i)
    inverse.push_back({i->kind == ChangeKind::kDelete ? ChangeKind::kInstall : ChangeKind::kDelete, i->route});
  auto result = ExecuteChanges(kPlatform, inverse);
  if (!result.ok) return Fail(error, result.error, result.error_path);
  if (!prepared->registry->ReplaceConfigurationReferences(
          prepared->before_references))
    return Fail(error, "cannot restore datastore nexthop references");
  if (!prepared->registry->ReplaceConfigurationRouteBindings(
          prepared->before_routes))
    return Fail(error, "cannot restore datastore nexthop route bindings");
  prepared->applied = false; return 1;
}
void Release(void*, void* raw) {
  auto* prepared = static_cast<Prepared*>(raw);
  if (!prepared) return;
  ReleaseAll(prepared->registry, prepared->before_references);
  ReleaseAll(prepared->registry, prepared->proposed_references);
  delete prepared;
}
size_t ActionCount(void*, void* raw) { return raw ? 1 : 0; }
int ActionAt(void*, void* raw, size_t index, DangHardwareActionV1* action, DangPluginErrorV1* error) {
  if (!raw || !action || index) return Fail(error, "RIB transaction action is unavailable");
  *action = {"routes", "/ietf-i2rs-rib:routing-instance", DANG_HARDWARE_NORMAL_V1, nullptr, 0}; return 1;
}
int ApplyAction(void* context, void* raw, const char* id, DangPluginErrorV1* error) {
  return id && std::string_view(id) == "routes" ? Apply(context, raw, error) : Fail(error, "RIB action ID is unknown");
}
int RollbackAction(void* context, void* raw, const char* id, DangPluginErrorV1* error) {
  return id && std::string_view(id) == "routes" ? Rollback(context, raw, error) : Fail(error, "RIB action ID is unknown");
}
int Invoke(void* raw_context, const DangOperationV1* operation,
           DangOperationResultV1* result, DangPluginErrorV1* error) {
  if (!operation || !operation->module_name || !operation->operation_name ||
      !result)
    return Fail(error, "RIB RPC input is incomplete", "/ietf-i2rs-rib:routing-instance");
  if (std::string_view(operation->module_name) != "ietf-i2rs-rib")
    return Fail(error, "RIB RPC module is not implemented", "/");
  auto* owner = static_cast<Context*>(raw_context);
  std::string load_error;
  if (!EnsureRegistry(owner, &load_error))
    return Fail(error, "cannot load reusable nexthop registry: " + load_error,
                "/ietf-i2rs-rib:routing-instance");
  std::lock_guard rpc_lock(owner->rpc_mutex);
  const std::string rpc_path = "/ietf-i2rs-rib:" +
                               std::string(operation->operation_name);
  std::string why;
  std::string where;
  bool invoked = false;
  if (std::string_view(operation->operation_name) == "route-add")
    invoked = InvokeRouteAdd(kPlatform, operation->input_xml, &rpc_output_xml,
                             &why, &where, CommandRunner{},
                             Resolver(static_cast<Context*>(raw_context)),
                             &owner->nexthops, Writer(owner), EventSink(owner),
                             NativeRib(owner), Observer(owner));
  else if (std::string_view(operation->operation_name) == "route-delete")
    invoked = InvokeRouteDelete(kPlatform, operation->input_xml,
                                &rpc_output_xml, &why, &where,
                                CommandRunner{}, Observer(owner),
                                &owner->nexthops, Writer(owner), EventSink(owner),
                                NativeRib(owner));
  else if (std::string_view(operation->operation_name) == "route-update")
    invoked = InvokeRouteUpdate(kPlatform, operation->input_xml,
                                &rpc_output_xml, &why, &where,
                                CommandRunner{}, Observer(owner),
                                Resolver(static_cast<Context*>(raw_context)),
                                &owner->nexthops, Writer(owner), EventSink(owner),
                                NativeRib(owner));
  else if (std::string_view(operation->operation_name) == "rib-add")
    invoked = InvokeRibAdd(kPlatform, operation->input_xml, &rpc_output_xml,
                           &why, &where, &owner->nexthops, Writer(owner),
                           NativeRib(owner));
  else if (std::string_view(operation->operation_name) == "rib-delete")
    invoked = InvokeRibDelete(kPlatform, operation->input_xml, &rpc_output_xml,
                              &why, &where, CommandRunner{}, Observer(owner),
                              &owner->nexthops, Writer(owner), EventSink(owner),
                              NativeRib(owner));
  else if (std::string_view(operation->operation_name) == "nh-add")
    invoked = InvokeNexthopAdd(
        &owner->nexthops, operation->input_xml,
        &rpc_output_xml, &why, &where, Writer(owner), NativeRib(owner));
  else if (std::string_view(operation->operation_name) == "nh-delete")
    invoked = InvokeNexthopDelete(
        &owner->nexthops, operation->input_xml,
        &rpc_output_xml, &why, &where, Writer(owner));
  else
    return Fail(error, "RFC 8431 operation is not implemented", rpc_path);
  if (!invoked)
    return Fail(error, why, where.empty() ? rpc_path : where);
  result->output_xml = rpc_output_xml.c_str();
  return 1;
}
int Operational(void* raw_context, DangOperationalDataV2* out, DangPluginErrorV1* error) {
  if (!out) return Fail(error, "RIB operational output is missing");
  std::string load_error;
  if (!EnsureRegistry(static_cast<Context*>(raw_context), &load_error))
    return Fail(error, "cannot load reusable nexthop registry: " + load_error,
                "/ietf-i2rs-rib:routing-instance");
  std::vector<ObservedRoute> routes;
  std::string why;
  auto* owner = static_cast<Context*>(raw_context);
  // Imperative RPCs mutate the kernel and durable registry as one operation.
  // Hold their epoch across both observations so an operational reply cannot
  // combine a pre-route kernel view with a post-rib-add registry snapshot.
  std::lock_guard rpc_lock(owner->rpc_mutex);
  const bool ok = Observer(owner)(&routes, &why);
  if (!ok) return Fail(error, "cannot read host RIB: " + why,
                       "/ietf-i2rs-rib:routing-instance/rib-list");
  const OperationalRegistryState state = owner->nexthops.OperationalState();
  operational_xml = SerializeOperationalRoutes(
      routes, state.registry, state.configuration_routes);
  *out = {operational_xml.c_str(), 0}; return 1;
}
int Reconcile(void* raw_context, void* raw_prepared, const char* current_xml,
              DangAppliedConfigurationV1* result, DangPluginErrorV1* error) {
  if (!raw_context || !current_xml || !result)
    return Fail(error, "RIB reconciliation input is incomplete");
  Config current;
  std::string why;
  std::string where;
  auto* owner = static_cast<Context*>(raw_context);
  if (!EnsureRegistry(owner, &why))
    return Fail(error, "cannot load reusable nexthop registry: " + why,
                "/ietf-i2rs-rib:routing-instance");
  if (!ParseConfig(current_xml, &current, &why, &where, Resolver(owner)))
    return Fail(error, why, where);
  if (!owner->nexthops.ReplaceConfigurationReferences(References(current)))
    return Fail(error, "cannot establish applied nexthop references",
                "/ietf-i2rs-rib:routing-instance/rib-list/route-list/nexthop");
  if (!owner->nexthops.ReplaceConfigurationRouteBindings(current.routes))
    return Fail(error, "cannot establish applied nexthop route bindings",
                "/ietf-i2rs-rib:routing-instance/rib-list/route-list/nexthop");
  auto* prepared = static_cast<Prepared*>(raw_prepared);
  if (prepared && prepared->applied) {
    const auto events = EventSink(owner);
    for (const Change& change : prepared->changes)
      events(change.route, change.kind == ChangeKind::kInstall);
  }
  *result = {current_xml, nullptr, 0};
  return 1;
}
size_t ResourceCount(void*) { return 1; }
const char* ResourceAt(void*, size_t index) { return index == 0 ? "routing" : nullptr; }

int NextNotification(void* raw_context, DangNotificationV1* event,
                     DangPluginErrorV1* error) {
  if (!raw_context || !event)
    return Fail(error, "RIB notification output is missing", "/");
  auto* owner = static_cast<Context*>(raw_context);
  std::string observe_error;
  std::vector<ObservedRoute> routes;
  // Keep native observation and registry-derived resolution state in one
  // imperative-RPC epoch so neither baseline can capture half a transaction.
  std::lock_guard rpc_lock(owner->rpc_mutex);
  const bool observed = Observer(owner)(&routes, &observe_error);
  if (!observed) {
    (void)Fail(error, "cannot observe RIB notifications: " + observe_error,
               "/ietf-i2rs-rib:routing-instance/rib-list");
    return -1;
  }
  const OperationalRegistryState state = owner->nexthops.OperationalState();
  const std::vector<ObservedRoute> modeled_routes =
      ProjectOperationalRoutes(routes, state.registry,
                               state.configuration_routes);
  std::lock_guard lock(owner->notification_mutex);
  for (const ObservedRoute& change :
       owner->route_changes.Observe(modeled_routes)) {
    if (owner->notifications.size() >= kMaximumPendingNotifications) break;
    owner->notifications.push_back({
        "route-change",
        SerializeRouteChange(change.route, change.installed,
                             change.reason.value_or(""))});
  }
  for (const NexthopResolutionChange& change :
       owner->nexthop_resolutions.Observe(state.registry, routes)) {
    if (owner->notifications.size() >= kMaximumPendingNotifications) break;
    owner->notifications.push_back(
        {"nexthop-resolution-status-change",
         SerializeNexthopResolutionChange(change.nexthop, change.resolved)});
  }
  if (owner->notifications.empty()) return 0;
  notification_name = std::move(owner->notifications.front().name);
  notification_xml = std::move(owner->notifications.front().xml);
  owner->notifications.pop_front();
  *event = {.stream_name = "NETCONF",
            .module_name = "ietf-i2rs-rib",
            .notification_name = notification_name.c_str(),
            .content_xml = notification_xml.c_str(),
            .instance_path = "",
            .default_deny_all = 0};
  return 1;
}

const DangPluginV8 kPlugin{.v7 = {.v6 = {.v5 = {.v4 = {.v3 = {.v2 = {.v1 = {
  DANG_PLUGIN_ABI_V8, "dang-rib", &plugin_context, SourceCount, SourceAt,
  DependencyCount, DependencyAt, Prepare, Validate, Apply, Rollback, Release,
  nullptr}, .invoke = Invoke}, .get_operational_data = nullptr},
  .hardware_action_count = ActionCount, .hardware_action_at = ActionAt,
  .apply_hardware_action = ApplyAction, .rollback_hardware_action = RollbackAction},
  .get_operational_data_v2 = Operational}, .reconcile_applied_configuration = Reconcile},
  .resource_domain_count = ResourceCount, .resource_domain_at = ResourceAt},
  .next_notification = NextNotification};
}

extern "C" const DangPluginV8* dang_plugin_init_v8() { return &kPlugin; }

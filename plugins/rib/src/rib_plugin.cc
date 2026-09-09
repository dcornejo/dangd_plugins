// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/** @file Transactional dangd adapter for the supported RFC 8431 route slice. */

#include "dangd/plugin_api.h"
#include "plugins/rib/src/platform_command.h"
#include "plugins/rib/src/platform_executor.h"
#include "plugins/rib/src/rib_config.h"
#include "plugins/rib/src/rib_persistence.h"
#include "plugins/rib/src/rib_rpc.h"
#include "plugins/rib/src/route_observer.h"
#include "rib_model_sources.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <vector>

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
  std::once_flag load_once;
  std::mutex rpc_mutex;
  std::filesystem::path registry_path;
  std::string load_error;
  bool loaded = false;
};
using Reference = std::pair<std::string, std::uint32_t>;
struct Prepared {
  std::vector<Change> changes;
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
    PersistentRegistry state;
    owner->loaded = LoadRegistry(owner->registry_path, &state,
                                 &owner->load_error) &&
                    owner->nexthops.RestorePersistentState(
                        state, &owner->load_error);
  });
  if (!owner->loaded && error) *error = owner->load_error;
  return owner->loaded;
}
RegistryWriter Writer(Context* owner) {
  return [owner](const PersistentRegistry& state, std::string* error) {
    return SaveRegistry(owner->registry_path, state, error);
  };
}
NexthopResolver Resolver(Context* owner) {
  return [owner](const std::string& rib, std::uint32_t id,
                 std::optional<std::string>* gateway,
                 std::optional<std::string>* interface) {
    return owner->nexthops.Resolve(rib, id, gateway, interface);
  };
}
std::vector<Reference> References(const Config& config) {
  std::vector<Reference> result;
  for (const Route& route : config.routes)
    if (route.nexthop_ref) result.emplace_back(route.rib, *route.nexthop_ref);
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

int Fail(DangPluginErrorV1* error, std::string text, std::string where = {}) {
  message = std::move(text); path = std::move(where);
  if (error) { error->message = message.c_str(); error->instance_path = path.empty() ? nullptr : path.c_str(); }
  return 0;
}
size_t SourceCount(void*) { return 2; }
int SourceAt(void*, size_t index, DangYangSourceV1* out, DangPluginErrorV1* error) {
  static const DangYangSourceV1 sources[]{
    {"ietf-i2rs-rib", "2018-09-13", kIetfI2rsRibYang, std::strlen(kIetfI2rsRibYang), "RFC 8431", DANG_YANG_IMPLEMENTED_V1, nullptr, 0},
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
  auto* prepared = new (std::nothrow) Prepared{
      PlanChanges(before, proposed), References(before), References(proposed),
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
  std::vector<NativeCommand> commands; std::string why, where;
  const bool ok = kPlatform == NativePlatform::kLinux ? BuildLinuxCommands(prepared->changes, &commands, &why, &where) : BuildFreeBsdCommands(prepared->changes, &commands, &why, &where);
  return ok ? 1 : Fail(error, why, where);
}
int Apply(void*, void* raw, DangPluginErrorV1* error) {
  auto* prepared = static_cast<Prepared*>(raw); if (!prepared) return Fail(error, "RIB transaction plan is missing");
  if (prepared->applied) return 1;
  auto result = ExecuteChanges(kPlatform, prepared->changes);
  if (!result.ok) return Fail(error, result.error, result.error_path);
  prepared->applied = true; return 1;
}
int Rollback(void*, void* raw, DangPluginErrorV1* error) {
  auto* prepared = static_cast<Prepared*>(raw); if (!prepared) return Fail(error, "RIB transaction plan is missing");
  if (!prepared->applied) return 1;
  std::vector<Change> inverse;
  for (auto i = prepared->changes.rbegin(); i != prepared->changes.rend(); ++i)
    inverse.push_back({i->kind == ChangeKind::kDelete ? ChangeKind::kInstall : ChangeKind::kDelete, i->route});
  auto result = ExecuteChanges(kPlatform, inverse);
  if (!result.ok) return Fail(error, result.error, result.error_path);
  if (!prepared->registry->ReplaceConfigurationReferences(
          prepared->before_references))
    return Fail(error, "cannot restore datastore nexthop references");
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
                             &why, &where, RunNativeCommand,
                             Resolver(static_cast<Context*>(raw_context)),
                             &owner->nexthops, Writer(owner));
  else if (std::string_view(operation->operation_name) == "route-delete")
    invoked = InvokeRouteDelete(kPlatform, operation->input_xml,
                                &rpc_output_xml, &why, &where,
                                RunNativeCommand, {},
                                &owner->nexthops, Writer(owner));
  else if (std::string_view(operation->operation_name) == "route-update")
    invoked = InvokeRouteUpdate(kPlatform, operation->input_xml,
                                &rpc_output_xml, &why, &where,
                                RunNativeCommand, {},
                                Resolver(static_cast<Context*>(raw_context)),
                                &owner->nexthops, Writer(owner));
  else if (std::string_view(operation->operation_name) == "rib-add")
    invoked = InvokeRibAdd(kPlatform, operation->input_xml, &rpc_output_xml,
                           &why, &where, &owner->nexthops, Writer(owner));
  else if (std::string_view(operation->operation_name) == "rib-delete")
    invoked = InvokeRibDelete(kPlatform, operation->input_xml, &rpc_output_xml,
                              &why, &where, RunNativeCommand, {},
                              &owner->nexthops, Writer(owner));
  else if (std::string_view(operation->operation_name) == "nh-add")
    invoked = InvokeNexthopAdd(
        &owner->nexthops, operation->input_xml,
        &rpc_output_xml, &why, &where, Writer(owner));
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
  const bool ok = kPlatform == NativePlatform::kLinux
                      ? ObserveLinuxRoutes(&routes, &why)
                      : ObserveFreeBsdRoutes(&routes, &why);
  if (!ok) return Fail(error, "cannot read host RIB: " + why,
                       "/ietf-i2rs-rib:routing-instance/rib-list");
  operational_xml = SerializeOperationalRoutes(
      routes, static_cast<Context*>(raw_context)->nexthops.Snapshot());
  *out = {operational_xml.c_str(), 0}; return 1;
}
int Reconcile(void* raw_context, void*, const char* current_xml,
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
  *result = {current_xml, nullptr, 0};
  return 1;
}
size_t ResourceCount(void*) { return 1; }
const char* ResourceAt(void*, size_t index) { return index == 0 ? "routing" : nullptr; }

const DangPluginV7 kPlugin{.v6 = {.v5 = {.v4 = {.v3 = {.v2 = {.v1 = {
  DANG_PLUGIN_ABI_V7, "dang-rib", &plugin_context, SourceCount, SourceAt,
  DependencyCount, DependencyAt, Prepare, Validate, Apply, Rollback, Release,
  nullptr}, .invoke = Invoke}, .get_operational_data = nullptr},
  .hardware_action_count = ActionCount, .hardware_action_at = ActionAt,
  .apply_hardware_action = ApplyAction, .rollback_hardware_action = RollbackAction},
  .get_operational_data_v2 = Operational}, .reconcile_applied_configuration = Reconcile},
  .resource_domain_count = ResourceCount, .resource_domain_at = ResourceAt};
}

extern "C" const DangPluginV7* dang_plugin_init_v7() { return &kPlugin; }

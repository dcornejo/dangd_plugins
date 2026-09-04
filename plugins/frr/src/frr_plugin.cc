// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/**
 * @file
 * dangd ABI adapter for FRR's native YANG schemas and mgmtd transaction API.
 * The context owns immutable schema text for the plugin lifetime; prepared
 * objects retain before/proposed subtrees until release so rollback remains
 * possible after an otherwise successful apply.
 */

#include "dangd/plugin_api.h"

#include "frr_config.h"
#include "frr_operational.h"
#include "frr_transaction.h"
#include "mgmtd_session.h"
#include "mgmtd_transport.h"
#include "schema_inventory.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr std::string_view kRoutingNamespace =
    "http://frrouting.org/yang/routing";
constexpr std::string_view kZebraNamespace =
    "http://frrouting.org/yang/zebra";
constexpr char kMonitoringModel[] = R"yang(module dang-frr-monitoring {
  yang-version 1.1;
  namespace "urn:dang:plugins:frr:monitoring";
  prefix dfm;
  revision 2026-09-04 {
    description "FRR provider health notifications.";
  }
  notification configuration-drift {
    description
      "The running FRR configuration differs from the last state reconciled
       after a successful dangd commit.";
    leaf datastore-path { type string; mandatory true; }
    leaf reason { type string; mandatory true; }
  }
})yang";

struct Context {
  std::vector<dang::plugins::frr::YangSchema> sources;
  std::vector<std::string> source_uris;
  std::string socket_path = "/var/run/frr/mgmtd_fe.sock";
  std::chrono::milliseconds timeout{2000};
  std::atomic<std::uint64_t> next_client{1};
  std::mutex running_mutex;
  std::optional<std::vector<std::optional<std::string>>> expected_running;
  std::chrono::milliseconds drift_poll_interval{1000};
  std::mutex notification_mutex;
  std::deque<std::string> pending_drift_paths;
  std::set<std::string> reported_drift_paths;
  // Declared after every object used by the thread so destruction joins it
  // before those objects are released.
  std::jthread drift_watcher;
  std::string initialization_error;

  Context() {
    dang::plugins::frr::SchemaInventoryOptions options;
    if (const char* directory = std::getenv("DANG_FRR_YANG_DIR");
        directory && *directory)
      options.explicit_directory = directory;
    if (const char* socket = std::getenv("DANG_FRR_MGMTD_SOCKET");
        socket && *socket)
      socket_path = socket;
    if (const char* milliseconds = std::getenv("DANG_FRR_TIMEOUT_MS");
        milliseconds && *milliseconds) {
      long long parsed = 0;
      const std::string_view text(milliseconds);
      const auto conversion =
          std::from_chars(text.data(), text.data() + text.size(), parsed);
      if (conversion.ec != std::errc{} || conversion.ptr != text.data() + text.size() ||
          parsed <= 0 || parsed > 60000) {
        initialization_error =
            "DANG_FRR_TIMEOUT_MS must be an integer from 1 through 60000";
        return;
      }
      timeout = std::chrono::milliseconds(parsed);
    }
    if (const char* milliseconds = std::getenv("DANG_FRR_DRIFT_POLL_MS");
        milliseconds && *milliseconds) {
      long long parsed = 0;
      const std::string_view text(milliseconds);
      const auto conversion =
          std::from_chars(text.data(), text.data() + text.size(), parsed);
      if (conversion.ec != std::errc{} ||
          conversion.ptr != text.data() + text.size() || parsed < 100 ||
          parsed > 60000) {
        initialization_error =
            "DANG_FRR_DRIFT_POLL_MS must be an integer from 100 through 60000";
        return;
      }
      drift_poll_interval = std::chrono::milliseconds(parsed);
    }
    auto inventory =
        dang::plugins::frr::DiscoverSchemaInventory(options, &initialization_error);
    if (!inventory) return;
    auto closure = dang::plugins::frr::ResolveImportClosure(
        *inventory, {"frr-routing", "frr-zebra", "frr-staticd"},
        &initialization_error);
    if (!closure) return;
    for (const std::size_t index : *closure) {
      source_uris.push_back((*inventory)[index].path.string());
      sources.push_back(std::move((*inventory)[index]));
    }
    const auto namespace_for = [&](std::string_view module) -> std::string_view {
      for (const auto& source : sources)
        if (source.module_name == module) return source.namespace_uri;
      return {};
    };
    if (namespace_for("frr-routing") != kRoutingNamespace ||
        namespace_for("frr-zebra") != kZebraNamespace) {
      initialization_error = "installed FRR roots have unexpected namespaces";
      sources.clear();
      source_uris.clear();
      return;
    }
    std::optional<std::string> library_xml;
    if (const char* path = std::getenv("DANG_FRR_YANG_LIBRARY_FILE");
        path && *path) {
      std::error_code size_error;
      const auto bytes = std::filesystem::file_size(path, size_error);
      if (size_error || bytes == 0 || bytes > options.maximum_total_bytes) {
        initialization_error =
            "DANG_FRR_YANG_LIBRARY_FILE size is invalid";
        return;
      }
      std::ifstream input(path, std::ios::binary);
      std::string contents{std::istreambuf_iterator<char>(input), {}};
      if ((!input.good() && !input.eof()) || contents.empty()) {
        initialization_error =
            "cannot read DANG_FRR_YANG_LIBRARY_FILE";
        return;
      }
      library_xml = std::move(contents);
    } else {
      auto transport = dang::plugins::frr::mgmtd::Transport::Connect(
          socket_path, timeout, &initialization_error);
      if (!transport) return;
      auto session = dang::plugins::frr::mgmtd::Session::Open(
          std::move(transport), next_client.fetch_add(1), "dangd-frr-discovery",
          &initialization_error);
      if (!session) return;
      library_xml = session->GetOperationalData(
          "/ietf-yang-library:yang-library", &initialization_error);
      std::string close_error;
      if (!session->Close(&close_error) && initialization_error.empty())
        initialization_error = std::move(close_error);
      if (!library_xml || !initialization_error.empty()) return;
    }
    if (!dang::plugins::frr::ApplyRuntimeYangLibrary(
            *library_xml, &sources, &initialization_error))
      return;
    source_uris.emplace_back("embedded:dang-frr-monitoring");
    sources.push_back({.module_name = "dang-frr-monitoring",
                       .revision = "2026-09-04",
                       .namespace_uri = "urn:dang:plugins:frr:monitoring",
                       .imports = {},
                       .enabled_features = {},
                       .source = kMonitoringModel,
                       .path = {}});
  }
};

struct Prepared {
  std::unique_ptr<dang::plugins::frr::FrrTransaction> transaction;
};

Context context;
thread_local std::string callback_error;
thread_local std::string callback_path;
thread_local std::string operational_xml;
thread_local std::string reconciled_xml;
thread_local std::string rpc_output_xml;
thread_local std::vector<const char*> callback_features;

const std::vector<dang::plugins::frr::RootDescriptor>& RootDescriptors() {
  static const std::vector<dang::plugins::frr::RootDescriptor> descriptors{
      {"frr-routing", std::string(kRoutingNamespace), "routing",
       "/frr-routing:routing"},
      {"frr-zebra", std::string(kZebraNamespace), "zebra",
       "/frr-zebra:zebra"}};
  return descriptors;
}

int Fail(DangPluginErrorV1* error, std::string message,
         std::string path = "/frr-routing:routing") {
  callback_error = std::move(message);
  callback_path = std::move(path);
  if (error) {
    error->message = callback_error.c_str();
    error->instance_path = callback_path.c_str();
  }
  return 0;
}

std::size_t SourceCount(void* raw) {
  return static_cast<Context*>(raw)->sources.size();
}

int SourceAt(void* raw, std::size_t index, DangYangSourceV1* output,
             DangPluginErrorV1* error) {
  auto* owner = static_cast<Context*>(raw);
  if (!output || index >= owner->sources.size())
    return Fail(error, "FRR YANG source index is out of range", "/");
  const auto& source = owner->sources[index];
  callback_features.clear();
  for (const std::string& feature : source.enabled_features)
    callback_features.push_back(feature.c_str());
  static const std::set<std::string> implemented{
      "dang-frr-monitoring", "frr-routing", "frr-zebra", "frr-staticd"};
  *output = {.module_name = source.module_name.c_str(),
             .revision = source.revision.c_str(),
             .source = source.source.data(),
             .source_size = source.source.size(),
             .source_uri = owner->source_uris[index].c_str(),
             .role = implemented.contains(source.module_name)
                 ? DANG_YANG_IMPLEMENTED_V1
                 : DANG_YANG_IMPORT_ONLY_V1,
             .enabled_features = callback_features.empty()
                 ? nullptr
                 : callback_features.data(),
             .enabled_feature_count = callback_features.size()};
  return 1;
}

std::size_t DependencyCount(void*) { return 0; }
const char* DependencyAt(void*, std::size_t) { return nullptr; }

std::unique_ptr<dang::plugins::frr::mgmtd::Session> OpenConcreteSession(
    Context* owner, std::string* error) {
  auto transport = dang::plugins::frr::mgmtd::Transport::Connect(
      owner->socket_path, owner->timeout, error);
  if (!transport) return nullptr;
  std::uint64_t client = owner->next_client.fetch_add(1);
  if (client == 0) client = owner->next_client.fetch_add(1);
  return dang::plugins::frr::mgmtd::Session::Open(
      std::move(transport), client, "dangd-frr", error);
}

std::unique_ptr<dang::plugins::frr::mgmtd::SessionOperations> OpenSession(
    Context* owner, std::string* error) {
  return OpenConcreteSession(owner, error);
}

std::optional<std::vector<std::optional<std::string>>> ReadRunningRoots(
    dang::plugins::frr::mgmtd::Session* session, std::string* error) {
  std::vector<std::optional<std::string>> roots;
  for (const auto& descriptor : RootDescriptors()) {
    auto xml = session->GetRunningConfiguration(descriptor.xpath, error);
    if (!xml) return std::nullopt;
    roots.push_back(xml->empty() ? std::nullopt
                                : std::optional<std::string>(std::move(*xml)));
  }
  return roots;
}

void StartDriftWatcher(Context* owner) {
  if (owner->drift_watcher.joinable()) return;
  owner->drift_watcher = std::jthread([owner](std::stop_token stop) {
    while (!stop.stop_requested()) {
      std::this_thread::sleep_for(owner->drift_poll_interval);
      if (stop.stop_requested()) break;
      std::optional<std::vector<std::optional<std::string>>> expected;
      {
        std::lock_guard lock(owner->running_mutex);
        expected = owner->expected_running;
      }
      if (!expected) continue;
      std::string ignored_error;
      auto session = OpenConcreteSession(owner, &ignored_error);
      if (!session) continue;
      auto observed = ReadRunningRoots(session.get(), &ignored_error);
      std::string close_error;
      const bool closed = session->Close(&close_error);
      if (!observed || !closed || observed->size() != expected->size()) continue;
      for (std::size_t index = 0; index < expected->size(); ++index) {
        auto equivalent = dang::plugins::frr::EquivalentConfigurationRoot(
            (*expected)[index], (*observed)[index], &ignored_error);
        if (!equivalent || *equivalent) continue;
        const std::string& path = RootDescriptors()[index].xpath;
        std::lock_guard lock(owner->notification_mutex);
        if (owner->reported_drift_paths.insert(path).second)
          owner->pending_drift_paths.push_back(path);
      }
    }
  });
}

int Prepare(void* raw, const DangTransactionV1* transaction, void** output,
            DangPluginErrorV1* error) {
  auto* owner = static_cast<Context*>(raw);
  if (!transaction || !transaction->before_xml || !transaction->proposed_xml ||
      !output)
    return Fail(error, "FRR transaction snapshots are incomplete");
  std::string extraction_error;
  std::string extraction_path;
  auto roots = dang::plugins::frr::ExtractConfigurationRoots(
      transaction->before_xml, transaction->proposed_xml,
      RootDescriptors(),
      &extraction_error, &extraction_path);
  if (!roots)
    return Fail(error, extraction_error,
                extraction_path.empty() ? "/" : extraction_path);
  auto prepared = std::make_unique<Prepared>();
  prepared->transaction = std::make_unique<dang::plugins::frr::FrrTransaction>(
      [owner](std::string* session_error) {
        return OpenSession(owner, session_error);
      },
      std::move(*roots));
  *output = prepared.release();
  return 1;
}

int Validate(void*, void* raw, DangPluginErrorV1* error) {
  auto* prepared = static_cast<Prepared*>(raw);
  if (!prepared) return Fail(error, "FRR prepared transaction is missing");
  std::string failure;
  if (!prepared->transaction->Validate(&failure)) return Fail(error, failure);
  return 1;
}

int Apply(void*, void* raw, DangPluginErrorV1* error) {
  auto* prepared = static_cast<Prepared*>(raw);
  if (!prepared) return Fail(error, "FRR prepared transaction is missing");
  std::string failure;
  if (!prepared->transaction->Apply(&failure)) return Fail(error, failure);
  return 1;
}

int Rollback(void*, void* raw, DangPluginErrorV1* error) {
  auto* prepared = static_cast<Prepared*>(raw);
  if (!prepared) return Fail(error, "FRR prepared transaction is missing");
  std::string failure;
  if (!prepared->transaction->Rollback(&failure)) return Fail(error, failure);
  return 1;
}

void Release(void*, void* raw) { delete static_cast<Prepared*>(raw); }

int Operational(void* raw, DangOperationalDataV1* result,
                DangPluginErrorV1* error) {
  auto* owner = static_cast<Context*>(raw);
  if (!result) return Fail(error, "FRR operational output is missing", "/");
  auto transport = dang::plugins::frr::mgmtd::Transport::Connect(
      owner->socket_path, owner->timeout, &callback_error);
  if (!transport) return Fail(error, callback_error, "/");
  std::uint64_t client = owner->next_client.fetch_add(1);
  if (client == 0) client = owner->next_client.fetch_add(1);
  auto session = dang::plugins::frr::mgmtd::Session::Open(
      std::move(transport), client, "dangd-frr-operational", &callback_error);
  if (!session) return Fail(error, callback_error, "/");
  std::vector<std::string> fragments;
  auto zebra = session->GetOperationalData("/frr-zebra:zebra", &callback_error);
  if (zebra && !zebra->empty()) fragments.push_back(std::move(*zebra));
  for (const auto& [xpath, descriptor] : {
           std::pair{
               "/frr-interface:lib",
               dang::plugins::frr::AugmentedList{
                   "http://frrouting.org/yang/interface", "lib", "interface",
                   {"name", "vrf"}}},
           std::pair{
               "/frr-vrf:lib",
               dang::plugins::frr::AugmentedList{
                   "http://frrouting.org/yang/vrf", "lib", "vrf", {"name"}}}}) {
    if (!zebra) break;
    auto imported = session->GetOperationalData(xpath, &callback_error);
    if (!imported) {
      zebra.reset();
      break;
    }
    if (imported->empty()) continue;
    auto filtered = dang::plugins::frr::ExtractZebraAugments(
        *imported, descriptor, &callback_error);
    if (!filtered) {
      zebra.reset();
      break;
    }
    if (!filtered->empty()) fragments.push_back(std::move(*filtered));
  }
  std::unique_lock running_lock(owner->running_mutex);
  auto running = zebra ? ReadRunningRoots(session.get(), &callback_error)
                       : std::nullopt;
  std::string close_error;
  const bool closed = session->Close(&close_error);
  if (!zebra || !running)
    return Fail(error, callback_error, "/frr-routing:routing");
  if (!closed) return Fail(error, close_error, "/");
  if (owner->expected_running) {
    for (std::size_t index = 0; index < owner->expected_running->size();
         ++index) {
      auto equivalent = dang::plugins::frr::EquivalentConfigurationRoot(
          (*owner->expected_running)[index], (*running)[index], &callback_error);
      if (!equivalent)
        return Fail(error, callback_error, RootDescriptors()[index].xpath);
      if (!*equivalent)
        return Fail(error, "FRR running configuration changed outside dangd",
                    RootDescriptors()[index].xpath);
    }
  }
  operational_xml = dang::plugins::frr::OperationalDocument(fragments);
  result->data_xml = operational_xml.c_str();
  return 1;
}

int Reconcile(void* raw, void* prepared_raw, const char* current_xml,
              DangAppliedConfigurationV1* result,
              DangPluginErrorV1* error) {
  auto* owner = static_cast<Context*>(raw);
  auto* prepared = static_cast<Prepared*>(prepared_raw);
  if (!prepared || !prepared->transaction->applied() || !current_xml || !result)
    return Fail(error, "FRR reconciliation input is incomplete");
  auto session = OpenConcreteSession(owner, &callback_error);
  if (!session) return Fail(error, callback_error, "/");
  std::unique_lock running_lock(owner->running_mutex);
  auto observed = ReadRunningRoots(session.get(), &callback_error);
  std::string close_error;
  const bool closed = session->Close(&close_error);
  if (!observed)
    return Fail(error, callback_error, "/");
  if (!closed) return Fail(error, close_error, "/");
  std::string reconciliation_path;
  auto reconciled = dang::plugins::frr::ReconcileConfigurationRoots(
      current_xml, RootDescriptors(), *observed, &callback_error,
      &reconciliation_path);
  if (!reconciled)
    return Fail(error, callback_error,
                reconciliation_path.empty() ? "/" : reconciliation_path);
  owner->expected_running = std::move(*observed);
  {
    std::lock_guard notification_lock(owner->notification_mutex);
    owner->pending_drift_paths.clear();
    owner->reported_drift_paths.clear();
  }
  StartDriftWatcher(owner);
  reconciled_xml = std::move(*reconciled);
  result->applied_xml = reconciled_xml.c_str();
  result->outcomes = nullptr;
  result->outcome_count = 0;
  return 1;
}

int Invoke(void* raw, const DangOperationV1* operation,
           DangOperationResultV1* result, DangPluginErrorV1* error) {
  auto* owner = static_cast<Context*>(raw);
  if (!operation || !operation->module_name || !operation->operation_name ||
      !result)
    return Fail(error, "FRR RPC input is incomplete", "/");
  if (std::string_view(operation->module_name) != "frr-zebra")
    return Fail(error, "FRR RPC module is not implemented", "/");
  const std::string xpath = "/frr-zebra:" +
      std::string(operation->operation_name);
  auto session = OpenConcreteSession(owner, &callback_error);
  if (!session) return Fail(error, callback_error, xpath);
  auto output = session->InvokeRpc(
      xpath, operation->input_xml ? operation->input_xml : "", &callback_error);
  std::string close_error;
  const bool closed = session->Close(&close_error);
  if (!output) return Fail(error, callback_error, xpath);
  if (!closed) return Fail(error, close_error, xpath);
  rpc_output_xml = std::move(*output);
  result->output_xml = rpc_output_xml.c_str();
  return 1;
}

std::size_t HardwareActionCount(void*, void*) { return 0; }
int UnsupportedHardwareAction(void*, void*, std::size_t,
                              DangHardwareActionV1*, DangPluginErrorV1* error) {
  return Fail(error, "FRR exposes no fine-grained hardware actions");
}
int UnsupportedApplyAction(void*, void*, const char*, DangPluginErrorV1* error) {
  return Fail(error, "FRR exposes no fine-grained hardware actions");
}

std::size_t ResourceCount(void*) { return 1; }
const char* ResourceAt(void*, std::size_t index) {
  return index == 0 ? "routing" : nullptr;
}

int NextNotification(void* raw, DangNotificationV1* event,
                     DangPluginErrorV1* error) {
  auto* owner = static_cast<Context*>(raw);
  if (!event) return Fail(error, "FRR notification output is missing", "/");
  std::lock_guard lock(owner->notification_mutex);
  if (owner->pending_drift_paths.empty()) return 0;
  callback_path = std::move(owner->pending_drift_paths.front());
  owner->pending_drift_paths.pop_front();
  operational_xml =
      "<configuration-drift xmlns=\"urn:dang:plugins:frr:monitoring\">"
      "<datastore-path>" + callback_path + "</datastore-path>"
      "<reason>FRR running configuration changed outside dangd</reason>"
      "</configuration-drift>";
  *event = {.stream_name = "NETCONF",
            .module_name = "dang-frr-monitoring",
            .notification_name = "configuration-drift",
            .content_xml = operational_xml.c_str(),
            .instance_path = "",
            .default_deny_all = 0};
  return 1;
}

const DangPluginV8 kPlugin{
    .v7 = {.v6 = {.v5 = {.v4 = {.v3 = {.v2 = {.v1 = {
                                            .abi_version = DANG_PLUGIN_ABI_V8,
                                            .plugin_name = "dang-frr",
                                            .context = &context,
                                            .yang_source_count = SourceCount,
                                            .yang_source_at = SourceAt,
                                            .dependency_count = DependencyCount,
                                            .dependency_at = DependencyAt,
                                            .prepare = Prepare,
                                            .validate = Validate,
                                            .apply = Apply,
                                            .rollback = Rollback,
                                            .release = Release,
                                            .destroy = nullptr},
                                        .invoke = Invoke},
                                .get_operational_data = Operational},
                        .hardware_action_count = HardwareActionCount,
                        .hardware_action_at = UnsupportedHardwareAction,
                        .apply_hardware_action = UnsupportedApplyAction,
                        .rollback_hardware_action = UnsupportedApplyAction},
                .get_operational_data_v2 = nullptr},
           .reconcile_applied_configuration = Reconcile},
           .resource_domain_count = ResourceCount,
           .resource_domain_at = ResourceAt},
    .next_notification = NextNotification};

}  // namespace

extern "C" const DangPluginV8* dang_plugin_init_v8() {
  return context.initialization_error.empty() ? &kPlugin : nullptr;
}

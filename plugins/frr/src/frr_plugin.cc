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
#include "frr_transaction.h"
#include "mgmtd_session.h"
#include "mgmtd_transport.h"
#include "schema_inventory.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view kRoutingNamespace =
    "http://frrouting.org/yang/routing";
constexpr std::string_view kZebraNamespace =
    "http://frrouting.org/yang/zebra";

struct Context {
  std::vector<dang::plugins::frr::YangSchema> sources;
  std::vector<std::string> source_uris;
  std::string socket_path = "/var/run/frr/mgmtd_fe.sock";
  std::chrono::milliseconds timeout{2000};
  std::atomic<std::uint64_t> next_client{1};
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
    }
  }
};

struct Prepared {
  std::unique_ptr<dang::plugins::frr::FrrTransaction> transaction;
};

Context context;
thread_local std::string callback_error;
thread_local std::string callback_path;
thread_local std::string operational_xml;

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
  static const std::set<std::string> implemented{
      "frr-routing", "frr-zebra", "frr-staticd"};
  *output = {.module_name = source.module_name.c_str(),
             .revision = source.revision.c_str(),
             .source = source.source.data(),
             .source_size = source.source.size(),
             .source_uri = owner->source_uris[index].c_str(),
             .role = implemented.contains(source.module_name)
                 ? DANG_YANG_IMPLEMENTED_V1
                 : DANG_YANG_IMPORT_ONLY_V1,
             .enabled_features = nullptr,
             .enabled_feature_count = 0};
  return 1;
}

std::size_t DependencyCount(void*) { return 0; }
const char* DependencyAt(void*, std::size_t) { return nullptr; }

std::unique_ptr<dang::plugins::frr::mgmtd::SessionOperations> OpenSession(
    Context* owner, std::string* error) {
  auto transport = dang::plugins::frr::mgmtd::Transport::Connect(
      owner->socket_path, owner->timeout, error);
  if (!transport) return nullptr;
  std::uint64_t client = owner->next_client.fetch_add(1);
  if (client == 0) client = owner->next_client.fetch_add(1);
  return dang::plugins::frr::mgmtd::Session::Open(
      std::move(transport), client, "dangd-frr", error);
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
      {{"frr-routing", std::string(kRoutingNamespace), "routing",
        "/frr-routing:routing"},
       {"frr-zebra", std::string(kZebraNamespace), "zebra",
        "/frr-zebra:zebra"}},
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
  // Publish only the implemented top-level zebra state. A broad /* request
  // also returns mgmtd's own YANG-library and imported-module state, which this
  // provider does not own and which would collide with dangd core providers.
  auto xml = session->GetOperationalData("/frr-zebra:zebra", &callback_error);
  std::string close_error;
  const bool closed = session->Close(&close_error);
  if (!xml)
    return Fail(error, callback_error, "/frr-routing:routing");
  if (!closed) return Fail(error, close_error, "/");
  operational_xml = xml->empty()
      ? "<data xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\"/>"
      : std::move(*xml);
  result->data_xml = operational_xml.c_str();
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

const DangPluginV7 kPlugin{
    .v6 = {.v5 = {.v4 = {.v3 = {.v2 = {.v1 = {
                                            .abi_version = DANG_PLUGIN_ABI_V7,
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
                                        .invoke = nullptr},
                                .get_operational_data = Operational},
                        .hardware_action_count = HardwareActionCount,
                        .hardware_action_at = UnsupportedHardwareAction,
                        .apply_hardware_action = UnsupportedApplyAction,
                        .rollback_hardware_action = UnsupportedApplyAction},
                .get_operational_data_v2 = nullptr},
           .reconcile_applied_configuration = nullptr},
    .resource_domain_count = ResourceCount,
    .resource_domain_at = ResourceAt};

}  // namespace

extern "C" const DangPluginV7* dang_plugin_init_v7() {
  return context.initialization_error.empty() ? &kPlugin : nullptr;
}

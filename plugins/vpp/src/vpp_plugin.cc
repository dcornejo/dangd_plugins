// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/** @file Loadable ABI-v7 provider for VPP software loopbacks. */

#include "dangd/plugin_api.h"
#include "plugins/vpp/src/loopback_plan.h"
#include "plugins/vpp/src/ownership_inventory.h"
#include "plugins/vpp/src/ownership_policy.h"
#include "plugins/vpp/src/vapi_vpp_client.h"
#include "vpp_model_sources.h"

#include <cstdlib>
#include <charconv>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <libxml/parser.h>
#include <libxml/tree.h>

namespace {
using dang::vpp::LoopbackOperation;
using dang::vpp::LoopbackOperationKind;

struct Prepared {
  std::vector<LoopbackOperation> operations;
  std::vector<dang::vpp::DevicePolicy> ownership;
  std::size_t applied = 0;
};

thread_local std::string callback_error;
thread_local std::string callback_path;
thread_local std::string operational_xml;
thread_local std::string reconciled_xml;

int Fail(DangPluginErrorV1* error, std::string message,
         std::string path = "/") {
  callback_error = std::move(message);
  callback_path = std::move(path);
  if (error) {
    error->message = callback_error.c_str();
    error->instance_path = callback_path.c_str();
  }
  return 0;
}

std::string SocketPath() {
  const char* configured = std::getenv("DANG_VPP_API_SOCKET");
  return configured && *configured ? configured : "/run/vpp/api.sock";
}

bool Observe(std::vector<dang::vpp::LoopbackConfiguration>* observed,
             std::string* error) {
  auto client = dang::vpp::VapiVppClient::Connect(SocketPath(), error);
  if (!client) return false;
  std::vector<dang::vpp::CreatedInterface> live;
  if (!client->ListLoopbacks(&live, error)) return false;
  observed->clear();
  for (const auto& item : live) {
    if (!item.name.starts_with("loop")) continue;
    uint32_t instance = 0;
    const std::string_view suffix(item.name.data() + 4, item.name.size() - 4);
    const auto parsed = std::from_chars(suffix.data(),
                                        suffix.data() + suffix.size(), instance);
    if (suffix.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != suffix.data() + suffix.size()) {
      *error = "VPP returned a malformed loopback name: " + item.name;
      return false;
    }
    observed->push_back({.instance = instance, .enabled = item.admin_up});
  }
  return true;
}

std::string Render(const std::vector<dang::vpp::LoopbackConfiguration>& values) {
  std::string xml = "<vpp-interfaces xmlns=\"urn:dang:vpp:interfaces\">";
  for (const auto& value : values)
    xml += "<loopback><instance>" + std::to_string(value.instance) +
           "</instance><enabled>" + (value.enabled ? "true" : "false") +
           "</enabled></loopback>";
  return xml + "</vpp-interfaces>";
}

int Operational(void*, DangOperationalDataV1* result,
                DangPluginErrorV1* error) {
  if (!result) return Fail(error, "VPP operational result is missing");
  std::vector<dang::vpp::LoopbackConfiguration> observed;
  if (!Observe(&observed, &callback_error)) return Fail(error, callback_error);
  operational_xml = Render(observed);
  result->data_xml = operational_xml.c_str();
  return 1;
}

int OperationalV2(void*, DangOperationalDataV2* result,
                  DangPluginErrorV1* error) {
  if (!result) return Fail(error, "VPP operational result is missing");
  DangOperationalDataV1 legacy{};
  if (!Operational(nullptr, &legacy, error)) return 0;
  *result = {.data_xml = legacy.data_xml, .complete = 1};
  return 1;
}

int Reconcile(void*, void*, const char* current_xml,
              DangAppliedConfigurationV1* result, DangPluginErrorV1* error) {
  if (!current_xml || !result)
    return Fail(error, "VPP reconciliation input is incomplete");
  std::vector<dang::vpp::LoopbackConfiguration> observed;
  if (!Observe(&observed, &callback_error)) return Fail(error, callback_error);
  xmlDocPtr document = xmlReadMemory(current_xml,
      static_cast<int>(std::strlen(current_xml)), "vpp-applied.xml", nullptr,
      XML_PARSE_NONET | XML_PARSE_NOBLANKS | XML_PARSE_NOERROR |
      XML_PARSE_NOWARNING);
  if (!document) return Fail(error, "cannot parse applied configuration");
  xmlNodePtr root = xmlDocGetRootElement(document);
  if (!root) {
    xmlFreeDoc(document);
    return Fail(error, "applied configuration has no document element");
  }
  for (xmlNodePtr node = root ? root->children : nullptr; node;) {
    xmlNodePtr next = node->next;
    if (node->type == XML_ELEMENT_NODE && node->ns && node->ns->href &&
        std::string_view(reinterpret_cast<const char*>(node->ns->href)) ==
            "urn:dang:vpp:interfaces") {
      xmlUnlinkNode(node);
      xmlFreeNode(node);
    }
    node = next;
  }
  xmlNodePtr subtree = xmlNewChild(root, nullptr,
      reinterpret_cast<const xmlChar*>("vpp-interfaces"), nullptr);
  xmlNsPtr ns = xmlNewNs(subtree,
      reinterpret_cast<const xmlChar*>("urn:dang:vpp:interfaces"), nullptr);
  xmlSetNs(subtree, ns);
  for (const auto& value : observed) {
    xmlNodePtr loopback = xmlNewChild(subtree, ns,
        reinterpret_cast<const xmlChar*>("loopback"), nullptr);
    xmlNewChild(loopback, ns, reinterpret_cast<const xmlChar*>("instance"),
        reinterpret_cast<const xmlChar*>(std::to_string(value.instance).c_str()));
    xmlNewChild(loopback, ns, reinterpret_cast<const xmlChar*>("enabled"),
        reinterpret_cast<const xmlChar*>(value.enabled ? "true" : "false"));
  }
  xmlChar* buffer = nullptr;
  int size = 0;
  xmlDocDumpMemoryEnc(document, &buffer, &size, "UTF-8");
  if (!buffer) { xmlFreeDoc(document); return Fail(error, "cannot serialize applied configuration"); }
  reconciled_xml.assign(reinterpret_cast<const char*>(buffer),
                        static_cast<std::size_t>(size));
  xmlFree(buffer);
  xmlFreeDoc(document);
  *result = {.applied_xml = reconciled_xml.c_str(), .outcomes = nullptr,
             .outcome_count = 0};
  return 1;
}

std::size_t SourceCount(void*) { return 2; }
int SourceAt(void*, std::size_t index, DangYangSourceV1* source,
             DangPluginErrorV1*) {
  if (!source || index > 1) return 0;
  if (index == 0)
    *source = {"dang-vpp-interfaces", "2026-09-07", kVppInterfacesYang,
               std::strlen(kVppInterfacesYang), "plugin:dang-vpp-interfaces",
               DANG_YANG_IMPLEMENTED_V1, nullptr, 0};
  else
    *source = {"dang-vpp-interface-ownership", "2026-09-06",
               kVppOwnershipYang, std::strlen(kVppOwnershipYang),
               "plugin:dang-vpp-interface-ownership",
               DANG_YANG_IMPLEMENTED_V1, nullptr, 0};
  return 1;
}
std::size_t DependencyCount(void*) { return 0; }
const char* DependencyAt(void*, std::size_t) { return nullptr; }

int Prepare(void*, const DangTransactionV1* transaction, void** result,
            DangPluginErrorV1* error) {
  if (!transaction || !transaction->before_xml || !transaction->proposed_xml ||
      !result)
    return Fail(error, "VPP transaction snapshots are incomplete");
  auto prepared = std::make_unique<Prepared>();
  dang::vpp::LoopbackConfigurationMap before, proposed;
  if (!dang::vpp::ParseLoopbackConfiguration(transaction->before_xml, &before,
                                               &callback_error, &callback_path) ||
      !dang::vpp::ParseLoopbackConfiguration(transaction->proposed_xml,
                                               &proposed, &callback_error,
                                               &callback_path))
    return Fail(error, callback_error, callback_path);
  prepared->operations = dang::vpp::PlanLoopbackChanges(before, proposed);
  if (!dang::vpp::ParseOwnershipPolicy(transaction->proposed_xml,
                                        &prepared->ownership, &callback_error,
                                        &callback_path))
    return Fail(error, callback_error, callback_path);
  *result = prepared.release();
  return 1;
}

int Validate(void*, void* raw, DangPluginErrorV1* error) {
  auto* prepared = static_cast<Prepared*>(raw);
  if (!prepared) return Fail(error, "VPP transaction plan is missing");
  if (prepared->applied != 0)
    return Fail(error, "VPP transaction is already partially or fully applied");
  std::vector<dang::vpp::InterfaceEvidence> inventory;
  if (!dang::vpp::DiscoverInterfaces(&inventory, &callback_error))
    return Fail(error, callback_error,
                "/dang-vpp-interface-ownership:vpp-interface-ownership");
  if (!dang::vpp::ValidateOwnershipPolicy(prepared->ownership, inventory,
                                           &callback_error, &callback_path))
    return Fail(error, callback_error, callback_path);
  for (const auto& device : prepared->ownership) {
    if (!device.vpp_owner) continue;
    return Fail(error,
        "physical transfer to VPP is not implemented safely yet",
        "/dang-vpp-interface-ownership:vpp-interface-ownership/device"
        "[pci-address='" + device.pci_address + "']");
  }
  return 1;
}

bool Run(dang::vpp::VppClient* client, const LoopbackOperation& operation,
         bool reverse, std::string* error) {
  const auto kind = operation.kind;
  if ((!reverse && kind == LoopbackOperationKind::kCreate) ||
      (reverse && kind == LoopbackOperationKind::kDelete)) {
    dang::vpp::CreatedInterface created;
    return client->CreateLoopback(operation.instance, &created, error);
  }
  dang::vpp::CreatedInterface found;
  if (!client->FindLoopback(operation.instance, &found, error)) return false;
  if ((!reverse && kind == LoopbackOperationKind::kDelete) ||
      (reverse && kind == LoopbackOperationKind::kCreate))
    return client->DeleteLoopback(found.software_index, error);
  const bool enabled = reverse ? !operation.enabled : operation.enabled;
  return client->SetAdminState(found.software_index, enabled, error);
}

int Apply(void*, void* raw, DangPluginErrorV1* error) {
  auto* prepared = static_cast<Prepared*>(raw);
  if (!prepared) return Fail(error, "VPP transaction plan is missing");
  if (prepared->applied != 0)
    return Fail(error, "VPP transaction is already partially or fully applied");
  auto client = dang::vpp::VapiVppClient::Connect(SocketPath(), &callback_error);
  if (!client) return Fail(error, callback_error);
  for (; prepared->applied < prepared->operations.size(); ++prepared->applied) {
    const auto& operation = prepared->operations[prepared->applied];
    if (Run(client.get(), operation, false, &callback_error)) continue;
    const std::string primary_error = callback_error;
    while (prepared->applied > 0) {
      const auto& completed = prepared->operations[prepared->applied - 1];
      if (!Run(client.get(), completed, true, &callback_error))
        return Fail(error, primary_error + "; compensation failed: " +
                    callback_error, completed.instance_path);
      --prepared->applied;
    }
    return Fail(error, primary_error, operation.instance_path);
  }
  return 1;
}

int Rollback(void*, void* raw, DangPluginErrorV1* error) {
  auto* prepared = static_cast<Prepared*>(raw);
  if (!prepared) return Fail(error, "VPP transaction plan is missing");
  auto client = dang::vpp::VapiVppClient::Connect(SocketPath(), &callback_error);
  if (!client) return Fail(error, callback_error);
  while (prepared->applied > 0) {
    const auto& operation = prepared->operations[prepared->applied - 1];
    if (!Run(client.get(), operation, true, &callback_error))
      return Fail(error, callback_error, operation.instance_path);
    --prepared->applied;
  }
  return 1;
}

void Release(void*, void* raw) { delete static_cast<Prepared*>(raw); }
std::size_t ActionCount(void*, void* raw) {
  return raw && !static_cast<Prepared*>(raw)->operations.empty() ? 1 : 0;
}
int ActionAt(void*, void* raw, std::size_t index, DangHardwareActionV1* action,
             DangPluginErrorV1* error) {
  if (!raw || !action || index != 0)
    return Fail(error, "VPP configuration action is unavailable");
  *action = {"software-interfaces", "/dang-vpp-interfaces:vpp-interfaces",
             DANG_HARDWARE_NORMAL_V1, nullptr, 0};
  return 1;
}
int ApplyAction(void* context, void* raw, const char* id,
                DangPluginErrorV1* error) {
  return id && std::string_view(id) == "software-interfaces"
      ? Apply(context, raw, error) : Fail(error, "unknown VPP action");
}
int RollbackAction(void* context, void* raw, const char* id,
                   DangPluginErrorV1* error) {
  return id && std::string_view(id) == "software-interfaces"
      ? Rollback(context, raw, error) : Fail(error, "unknown VPP action");
}
std::size_t ResourceCount(void*) { return 2; }
const char* ResourceAt(void*, std::size_t index) {
  return index == 0 ? "vpp-software-interfaces" :
         index == 1 ? "hardware-interface-ownership" : nullptr;
}

const DangPluginV7 kPlugin{
    .v6 = {.v5 = {.v4 = {.v3 = {.v2 = {.v1 = {
        .abi_version = DANG_PLUGIN_ABI_V7, .plugin_name = "dang-vpp",
        .context = nullptr, .yang_source_count = SourceCount,
        .yang_source_at = SourceAt, .dependency_count = DependencyCount,
        .dependency_at = DependencyAt, .prepare = Prepare, .validate = Validate,
        .apply = Apply, .rollback = Rollback, .release = Release,
        .destroy = nullptr}, .invoke = nullptr}, .get_operational_data = Operational},
        .hardware_action_count = ActionCount, .hardware_action_at = ActionAt,
        .apply_hardware_action = ApplyAction,
        .rollback_hardware_action = RollbackAction},
        .get_operational_data_v2 = OperationalV2},
        .reconcile_applied_configuration = Reconcile},
    .resource_domain_count = ResourceCount, .resource_domain_at = ResourceAt};
}  // namespace

extern "C" const DangPluginV7* dang_plugin_init_v7() { return &kPlugin; }

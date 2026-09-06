// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/plugin_api.h"
#include "ip_management_models.h"
#include "plugins/ip_management/src/platform_backend.h"

#include <algorithm>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <pugixml.hpp>

namespace {

using nlohmann::json;

struct Action {
  // These strings deliberately describe intent instead of changing the host.
  // A real backend should replace them with typed operations (for example,
  // CreateLink, SetLinkFlags, AddAddress, and DeleteNeighbor) so ordering,
  // idempotence, precise errors, and rollback do not depend on parsing text.
  std::string forward;
  std::string reverse;
  std::string id;
  std::string instance_path;
  uint32_t action_class = DANG_HARDWARE_NORMAL_V1;
  bool applied = false;
};

struct Prepared {
  // Prepared owns everything needed after the validate phase. In production it
  // should also retain resolved interface indexes, capability/resource checks,
  // a dependency graph, and exact kernel snapshots needed for compensation.
  // Do not retain pointers into DangTransactionV1: all ABI input is borrowed.
  std::vector<Action> actions;
  std::string before;
  std::string proposed;
  bool platform_applied = false;
};

constexpr std::string_view kInterfacesModule = "ietf-interfaces";
constexpr std::string_view kIpModule = "ietf-ip";
std::string active_configuration;
// ABI v3 returns borrowed bytes. Keeping the serialized document here makes
// the pointer valid until the next callback. A production plugin should put
// both configuration and operational snapshots in its context object and
// protect them with a reader/writer lock; these globals are acceptable only
// for this single-instance teaching plugin.
std::string operational_xml;
std::string backend_error;
auto platform_backend = dangd::ip_management::MakePlatformBackend();

bool Reconcile(std::string_view before, std::string_view desired,
               DangPluginErrorV1* error) {
  backend_error.clear();
  if (platform_backend->Reconcile(before, desired, &backend_error)) return true;
  if (error) {
    error->message = backend_error.c_str();
    error->instance_path = nullptr;
  }
  return false;
}

size_t SourceCount(void*) { return 2; }

int SourceAt(void*, size_t index, DangYangSourceV1* source,
             DangPluginErrorV1*) {
  if (!source) return 0;
  if (index == 0) {
    *source = {"ietf-interfaces", "2018-02-20",
               dangd::ip_management::kIetfInterfacesYang.data(),
               dangd::ip_management::kIetfInterfacesYang.size(),
               "urn:ietf:rfc:8343", DANG_YANG_IMPLEMENTED_V1, nullptr, 0};
    return 1;
  }
  if (index == 1) {
    *source = {"ietf-ip", "2018-02-22",
               dangd::ip_management::kIetfIpYang.data(),
               dangd::ip_management::kIetfIpYang.size(),
               "urn:ietf:rfc:8344", DANG_YANG_IMPLEMENTED_V1, nullptr, 0};
    return 1;
  }
  return 0;
}

size_t DependencyCount(void*) { return 0; }
const char* DependencyAt(void*, size_t) { return nullptr; }

std::optional<std::string> JsonValue(const json& value) {
  if (value.is_null()) return std::nullopt;
  if (value.is_string()) return value.get<std::string>();
  return value.dump();
}

std::string Quoted(const std::optional<std::string>& value) {
  return value ? json(*value).dump() : "<absent>";
}

Action MakeAction(const json& change, std::size_t index) {
  // dangd supplies a schema-aware delta, including stable instance paths and
  // before/after values. A Linux implementation could translate these paths
  // to rtnetlink messages through libmnl/libnl or direct NETLINK_ROUTE calls.
  // Resolve interface names to ifindex during preparation, batch independent
  // operations where useful, and request acknowledgements with extended ACKs
  // so error.message and error.instance_path can identify the failing object.
  //
  // Never assume delta order is safe for hardware. Construct dependencies so
  // addresses, routes, ACL references, and other prerequisites are installed
  // before an interface is enabled; reverse that order during removal. ABI v4
  // publishes the classification below to dangd's common transaction planner.
  const int kind = change.at("kind").get<int>();
  const std::string path = change.at("path").get<std::string>();
  const auto before = JsonValue(change.at("before"));
  const auto after = JsonValue(change.at("after"));
  const std::string id = "change-" + std::to_string(index);
  uint32_t action_class = DANG_HARDWARE_NORMAL_V1;
  if (kind == 1 ||
      (path.ends_with("}enabled") && after && *after == "false"))
    action_class = DANG_HARDWARE_DEACTIVATE_V1;
  else if (path.ends_with("}enabled") && after && *after == "true")
    action_class = DANG_HARDWARE_ACTIVATE_V1;
  if (kind == 0) {
    return {"create " + path + (after ? " with value " + Quoted(after) : ""),
            "delete " + path, id, path, action_class, false};
  }
  if (kind == 1) {
    return {"delete " + path,
            "create " + path + (before ? " with value " + Quoted(before) : ""),
            id, path, action_class, false};
  }
  if (kind == 2) {
    return {"set " + path + " from " + Quoted(before) + " to " + Quoted(after),
            "set " + path + " from " + Quoted(after) + " to " + Quoted(before),
            id, path, action_class, false};
  }
  return {"replace subtree " + path, "restore subtree " + path, id, path,
          action_class, false};
}

int Prepare(void*, const DangTransactionV1* transaction, void** result,
            DangPluginErrorV1* error) {
  // Preparation must be side-effect free. It is the right phase to parse all
  // owned module changes, query immutable platform facts, reserve in-memory
  // planning resources, check device limits, and build forward and compensating
  // operations. Other plugins are prepared before any plugin is applied, so a
  // failure here safely rejects the complete transaction.
  if (!transaction || !transaction->changes_json || !result) return 0;
  try {
    auto prepared = std::make_unique<Prepared>();
    std::size_t action_index = 0;
    for (const json& change : json::parse(transaction->changes_json)) {
      const std::string module = change.at("module").get<std::string>();
      if (module == kInterfacesModule || module == kIpModule)
        prepared->actions.push_back(MakeAction(change, action_index++));
    }
    prepared->before = transaction->before_xml ? transaction->before_xml : "";
    prepared->proposed = transaction->proposed_xml
        ? transaction->proposed_xml : "";
    *result = prepared.release();
    return 1;
  } catch (const json::exception&) {
    if (error) {
      error->message = "dangd supplied malformed configuration changes";
      error->instance_path = nullptr;
    }
    return 0;
  }
}

int Validate(void*, void* opaque, DangPluginErrorV1*) {
  // The example accepts every syntactically valid plan because common YANG
  // validation has already run. Real validation should check constraints that
  // YANG cannot express: supported address families, MTU ranges reported by
  // the driver, address/neighbor capacity, interface existence, privileges,
  // cross-plugin dependencies, and whether rollback is actually possible.
  // It must remain side-effect free and should populate DangPluginErrorV1 with
  // a stable message and the schema instance path that caused rejection.
  return opaque != nullptr;
}

int Apply(void*, void* opaque, DangPluginErrorV1* error) {
  // The selected platform backend performs the complete snapshot transition.
  // Linux and FreeBSD wait for each administration command; other build hosts
  // deliberately retain the logging-only teaching behavior. A direct kernel
  // implementation should retain per-operation completion and acknowledgements
  // here so compensation can restore an exact observed snapshot.
  //
  // active_configuration is updated only after every backend action succeeds.
  // That ordering is essential: operational publication must describe applied
  // device state, never merely the configuration that dangd proposed.
  if (!opaque) return 0;
  auto* prepared = static_cast<Prepared*>(opaque);
  for (const Action& action : prepared->actions)
    std::clog << "ip-management: " << action.forward << '\n';
  if (!Reconcile(prepared->before, prepared->proposed, error)) return 0;
  prepared->platform_applied = true;
  active_configuration = prepared->proposed;
  return 1;
}

int Rollback(void*, void* opaque, DangPluginErrorV1* error) {
  // Compensation runs in reverse dependency order. Production code should use
  // captured pre-change kernel values rather than assuming the inverse of an
  // operation restores reality; asynchronous kernel changes or another agent
  // may have altered the object. Report rollback failures explicitly because
  // they mean datastore and device state may have diverged and need repair.
  if (!opaque) return 0;
  const auto& actions = static_cast<Prepared*>(opaque)->actions;
  for (auto action = actions.rbegin(); action != actions.rend(); ++action)
    std::clog << "ip-management rollback: " << action->reverse << '\n';
  auto* prepared = static_cast<Prepared*>(opaque);
  if (!Reconcile(prepared->proposed, prepared->before, error)) return 0;
  prepared->platform_applied = false;
  active_configuration = prepared->before;
  return 1;
}

size_t HardwareActionCount(void*, void* opaque) {
  return opaque ? static_cast<Prepared*>(opaque)->actions.size() : 0;
}

int HardwareActionAt(void*, void* opaque, size_t index,
                     DangHardwareActionV1* result, DangPluginErrorV1*) {
  if (!opaque || !result) return 0;
  auto& actions = static_cast<Prepared*>(opaque)->actions;
  if (index >= actions.size()) return 0;
  const Action& action = actions[index];
  *result = {action.id.c_str(), action.instance_path.c_str(),
             action.action_class, nullptr, 0};
  return 1;
}

Action* FindAction(void* opaque, const char* id) {
  if (!opaque || !id) return nullptr;
  auto& actions = static_cast<Prepared*>(opaque)->actions;
  const auto found = std::ranges::find(actions, id, &Action::id);
  return found == actions.end() ? nullptr : &*found;
}

int ApplyHardwareAction(void*, void* opaque, const char* id,
                        DangPluginErrorV1* error) {
  Action* action = FindAction(opaque, id);
  if (!action || action->applied) {
    if (error) error->message = "unknown or duplicate hardware action";
    return 0;
  }
  std::clog << "ip-management: " << action->forward << '\n';
  action->applied = true;
  auto* prepared = static_cast<Prepared*>(opaque);
  if (std::ranges::all_of(prepared->actions, &Action::applied)) {
    if (!Reconcile(prepared->before, prepared->proposed, error)) {
      action->applied = false;
      return 0;
    }
    prepared->platform_applied = true;
    active_configuration = prepared->proposed;
  }
  return 1;
}

int RollbackHardwareAction(void*, void* opaque, const char* id,
                           DangPluginErrorV1* error) {
  Action* action = FindAction(opaque, id);
  if (!action || !action->applied) {
    if (error) error->message = "unknown or unapplied hardware action";
    return 0;
  }
  std::clog << "ip-management rollback: " << action->reverse << '\n';
  auto* prepared = static_cast<Prepared*>(opaque);
  // The native transition is performed atomically at the final planned action,
  // so the first compensation restores the complete pre-transaction snapshot.
  if (std::ranges::all_of(prepared->actions, [](const Action& candidate) {
        return candidate.applied;
      }) && !Reconcile(prepared->proposed, prepared->before, error)) return 0;
  prepared->platform_applied = false;
  action->applied = false;
  active_configuration = prepared->before;
  return 1;
}

int OperationalData(void*, DangOperationalDataV1* result,
                    DangPluginErrorV1* error) {
  // Operational data should normally be observed from the platform at request
  // time (or from a coherently refreshed cache), for example with RTM_GETLINK,
  // RTM_GETADDR, RTM_GETNEIGH, and their IPv6 equivalents. Populate the NMDA
  // config-true interface/IP nodes with values actually in use and config-false
  // status, counters, address origins, DAD state, and neighbor state from the
  // kernel. Assign accurate RFC 8342 origin metadata once the plugin API carries
  // per-node origins. Do not echo intended configuration as if it were observed.
  //
  // This demonstration instead derives the deprecated RFC 8343
  // /interfaces-state compatibility tree from the last successfully applied
  // configuration. It intentionally omits volatile counters and IP/neighbor
  // state. Each returned element must use the model's exact XML namespace;
  // dangd copies the borrowed string immediately and validates it as typed
  // partial instance data. A real provider should still build one coherent
  // snapshot because constraints spanning providers cannot yet be checked.
  if (!result) return 0;
  backend_error.clear();
  if (!platform_backend->OperationalXml(active_configuration, &operational_xml,
                                        &backend_error)) {
    if (error) {
      error->message = backend_error.c_str();
      error->instance_path = "/ietf-interfaces:interfaces-state";
    }
    return 0;
  }
  if (!operational_xml.empty()) {
    result->data_xml = operational_xml.c_str();
    return 1;
  }
  pugi::xml_document configuration;
  configuration.load_buffer(active_configuration.data(),
                            active_configuration.size());
  pugi::xml_document state;
  pugi::xml_node interfaces_state = state.append_child("interfaces-state");
  interfaces_state.append_attribute("xmlns") =
      "urn:ietf:params:xml:ns:yang:ietf-interfaces";
  interfaces_state.append_attribute("xmlns:if") =
      "urn:ietf:params:xml:ns:yang:ietf-interfaces";
  for (const pugi::xml_node root : configuration.document_element().children()) {
    const std::string_view root_name = root.name();
    const std::size_t root_colon = root_name.find(':');
    const std::string_view root_local = root_colon == std::string_view::npos
        ? root_name : root_name.substr(root_colon + 1);
    if (root_local != "interfaces") continue;
    for (const pugi::xml_node interface : root.children()) {
      const std::string_view interface_name = interface.name();
      const std::size_t interface_colon = interface_name.find(':');
      if ((interface_colon == std::string_view::npos
               ? interface_name : interface_name.substr(interface_colon + 1)) !=
          "interface") continue;
      const pugi::xml_node name = interface.child("name");
      const pugi::xml_node type = interface.child("type");
      if (!name || !type) continue;
      pugi::xml_node entry = interfaces_state.append_child("interface");
      entry.append_child("name").text() = name.text().as_string();
      entry.append_child("type").text() = type.text().as_string();
      const pugi::xml_node enabled = interface.child("enabled");
      entry.append_child("oper-status").text() =
          !enabled || std::string_view(enabled.text().as_string()) == "true"
              ? "up" : "down";
    }
  }
  std::ostringstream output;
  state.print(output, "", pugi::format_raw);
  operational_xml = output.str();
  result->data_xml = operational_xml.c_str();
  return 1;
}

void Release(void*, void* opaque) {
  auto* prepared = static_cast<Prepared*>(opaque);
  if (prepared && prepared->platform_applied) platform_backend->Commit();
  delete prepared;
}

// ABI v4 retains the v1-v3 prefix and adds fine-grained hardware operations.
const DangPluginV4 kPlugin{{{{
    DANG_PLUGIN_ABI_V4, "dangd-ip-management", nullptr, SourceCount, SourceAt,
    DependencyCount, DependencyAt, Prepare, Validate, Apply, Rollback, Release,
    nullptr}, nullptr}, OperationalData}, HardwareActionCount, HardwareActionAt,
    ApplyHardwareAction, RollbackHardwareAction};

}  // namespace

extern "C" const DangPluginV4* dang_plugin_init_v4() { return &kPlugin; }

// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/**
 * @file
 * Development-stage dangd ABI adapter for RFC 9249 transaction planning.
 *
 * The adapter exposes model discovery, preparation, validation, and generic
 * hardware-action ordering. Apply and rollback deliberately fail closed until
 * a native Linux or FreeBSD implementation is linked into this target.
 */

#include <algorithm>
#include <memory>
#include <new>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "dangd/plugin_api.h"
#include "ntp_model_sources.h"
#include "plugins/ntp/src/ntp_config.h"

namespace {

using dang::ntp::Config;
using dang::ntp::PlannedAction;

struct Prepared {
  Config before;
  Config proposed;
  std::vector<PlannedAction> actions;
  std::vector<std::vector<const char*>> dependency_views;
};

thread_local std::string message;
thread_local std::string path;

int Fail(DangPluginErrorV1* error, std::string text,
         std::string where = "/ietf-ntp:ntp") {
  message = std::move(text);
  path = std::move(where);
  if (error) {
    error->message = message.c_str();
    error->instance_path = path.c_str();
  }
  return 0;
}

std::size_t SourceCount(void*) { return 7; }

int SourceAt(void*, std::size_t index, DangYangSourceV1* source,
             DangPluginErrorV1* error) {
  static constexpr const char* ntp_features[] = {
      "ntp-port", "authentication", "hex-key-string", "unicast-configuration"};
  if (!source || index >= SourceCount(nullptr))
    return Fail(error, "invalid RFC 9249 YANG source index");
  static constexpr struct Source {
    const char* module;
    const char* revision;
    std::string_view text;
    const char* uri;
  } sources[] = {
      {"ietf-ntp", "2022-07-05", kIetfNtpYang, "urn:ietf:rfc:9249"},
      {"ietf-access-control-list", "2019-03-04", kIetfAccessControlListYang,
       "urn:ietf:rfc:8519"},
      {"ietf-packet-fields", "2019-03-04", kIetfPacketFieldsYang,
       "urn:ietf:rfc:8519"},
      {"ietf-ethertypes", "2019-03-04", kIetfEthertypesYang,
       "urn:ietf:rfc:8519"},
      {"ietf-routing-types", "2017-12-04", kIetfRoutingTypesYang,
       "urn:ietf:rfc:8294"},
      {"ietf-interfaces", "2018-02-20", kNtpIetfInterfacesYang,
       "urn:ietf:rfc:8343"},
      {"ietf-system", "2014-08-06", kNtpIetfSystemYang, "urn:ietf:rfc:7317"},
  };
  const Source& selected = sources[index];
  *source = {
      .module_name = selected.module,
      .revision = selected.revision,
      .source = selected.text.data(),
      .source_size = selected.text.size(),
      .source_uri = selected.uri,
      .role = index == 0 ? DANG_YANG_IMPLEMENTED_V1 : DANG_YANG_IMPORT_ONLY_V1,
      .enabled_features = index == 0 ? ntp_features : nullptr,
      .enabled_feature_count = index == 0 ? std::size(ntp_features) : 0};
  return 1;
}

int Prepare(void*, const DangTransactionV1* transaction, void** output,
            DangPluginErrorV1* error) {
  if (!transaction || !transaction->before_xml || !transaction->proposed_xml ||
      !output)
    return Fail(error, "the RFC 9249 transaction snapshot is incomplete");
  auto prepared = std::make_unique<Prepared>();
  if (!dang::ntp::ParseConfig(transaction->before_xml, &prepared->before,
                              &message, &path) ||
      !dang::ntp::ParseConfig(transaction->proposed_xml, &prepared->proposed,
                              &message, &path))
    return Fail(error, message, path);
  prepared->actions =
      dang::ntp::PlanChanges(prepared->before, prepared->proposed);
  prepared->dependency_views.resize(prepared->actions.size());
  for (std::size_t index = 0; index < prepared->actions.size(); ++index) {
    auto& views = prepared->dependency_views[index];
    for (const std::string& dependency : prepared->actions[index].dependencies)
      views.push_back(dependency.c_str());
  }
  *output = prepared.release();
  return 1;
}

int Validate(void*, void* opaque, DangPluginErrorV1* error) {
  const auto* prepared = static_cast<const Prepared*>(opaque);
  if (!prepared)
    return Fail(error, "the prepared RFC 9249 transaction is missing");
  std::set<std::string> known;
  for (const PlannedAction& action : prepared->actions) {
    if (action.id.empty() || !known.insert(action.id).second)
      return Fail(error, "the RFC 9249 action identifiers are invalid");
    for (const std::string& dependency : action.dependencies)
      if (!known.contains(dependency))
        return Fail(error,
                    "the RFC 9249 action graph has a forward or unknown "
                    "dependency");
  }
  return 1;
}

int Apply(void*, void* opaque, DangPluginErrorV1* error) {
  if (!opaque)
    return Fail(error, "the prepared RFC 9249 transaction is missing");
  return Fail(error,
              "RFC 9249 native apply is unavailable in the development "
              "plugin");
}

int Rollback(void*, void* opaque, DangPluginErrorV1* error) {
  if (!opaque)
    return Fail(error, "the prepared RFC 9249 transaction is missing");
  return Fail(error,
              "RFC 9249 native rollback is unavailable in the development "
              "plugin");
}

void Release(void*, void* opaque) { delete static_cast<Prepared*>(opaque); }

std::size_t HardwareActionCount(void*, void* opaque) {
  const auto* prepared = static_cast<const Prepared*>(opaque);
  return prepared ? prepared->actions.size() : 0;
}

const char* ActionPath(std::string_view id) {
  if (id == "configure-authentication") return "/ietf-ntp:ntp/authentication";
  if (id == "configure-access-rules") return "/ietf-ntp:ntp/access-rules";
  if (id == "configure-listener-port") return "/ietf-ntp:ntp/port";
  if (id == "configure-reference-clock") return "/ietf-ntp:ntp/refclock-master";
  if (id.find("unicast") != std::string_view::npos)
    return "/ietf-ntp:ntp/unicast-configuration";
  return "/ietf-ntp:ntp";
}

int HardwareActionAt(void*, void* opaque, std::size_t index,
                     DangHardwareActionV1* output, DangPluginErrorV1* error) {
  const auto* prepared = static_cast<const Prepared*>(opaque);
  if (!prepared || !output || index >= prepared->actions.size())
    return Fail(error, "invalid RFC 9249 hardware action index");
  const PlannedAction& action = prepared->actions[index];
  const auto& dependencies = prepared->dependency_views[index];
  std::uint32_t action_class = DANG_HARDWARE_NORMAL_V1;
  if (action.id == "disable-ntp" || action.id == "stop-ntp-for-reconfiguration")
    action_class = DANG_HARDWARE_DEACTIVATE_V1;
  else if (action.id == "enable-ntp" ||
           action.id == "restart-ntp-after-reconfiguration")
    action_class = DANG_HARDWARE_ACTIVATE_V1;
  *output = {
      .action_id = action.id.c_str(),
      .instance_path = ActionPath(action.id),
      .action_class = action_class,
      .dependencies = dependencies.empty() ? nullptr : dependencies.data(),
      .dependency_count = dependencies.size()};
  return 1;
}

int ApplyHardwareAction(void*, void* opaque, const char* action_id,
                        DangPluginErrorV1* error) {
  if (!opaque || !action_id)
    return Fail(error, "invalid RFC 9249 hardware action request");
  return Fail(error,
              "RFC 9249 native action '" + std::string(action_id) +
                  "' is unavailable in the development plugin",
              ActionPath(action_id));
}

int RollbackHardwareAction(void*, void* opaque, const char* action_id,
                           DangPluginErrorV1* error) {
  if (!opaque || !action_id)
    return Fail(error, "invalid RFC 9249 hardware rollback request");
  return Fail(error,
              "RFC 9249 native rollback for action '" + std::string(action_id) +
                  "' is unavailable in the development plugin",
              ActionPath(action_id));
}

int Operational(void*, DangOperationalDataV2*, DangPluginErrorV1* error) {
  return Fail(error,
              "RFC 9249 operational state is unavailable in the development "
              "plugin",
              "/ietf-ntp:ntp/clock-state");
}

std::size_t ResourceDomainCount(void*) { return 1; }

const char* ResourceDomainAt(void*, std::size_t index) {
  return index == 0 ? "ntp" : nullptr;
}

const DangPluginV7 plugin = {
    .v6 = {.v5 = {.v4 = {.v3 = {.v2 = {.v1 = {.abi_version = DANG_PLUGIN_ABI_V7,
                                              .plugin_name = "ietf-ntp",
                                              .context = nullptr,
                                              .yang_source_count = SourceCount,
                                              .yang_source_at = SourceAt,
                                              .dependency_count = nullptr,
                                              .dependency_at = nullptr,
                                              .prepare = Prepare,
                                              .validate = Validate,
                                              .apply = Apply,
                                              .rollback = Rollback,
                                              .release = Release,
                                              .destroy = nullptr},
                                       .invoke = nullptr},
                                .get_operational_data = nullptr},
                         .hardware_action_count = HardwareActionCount,
                         .hardware_action_at = HardwareActionAt,
                         .apply_hardware_action = ApplyHardwareAction,
                         .rollback_hardware_action = RollbackHardwareAction},
                  .get_operational_data_v2 = Operational},
           .reconcile_applied_configuration = nullptr},
    .resource_domain_count = ResourceDomainCount,
    .resource_domain_at = ResourceDomainAt};

}  // namespace

extern "C" const DangPluginV7* dang_plugin_init_v7() { return &plugin; }

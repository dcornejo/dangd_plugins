// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/** @file Verifies the fail-closed RFC 9249 development plugin contract. */

#include <dlfcn.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "dangd/plugin_api.h"

namespace {

constexpr std::string_view kBefore = R"xml(<config>
  <ntp xmlns="urn:ietf:params:xml:ns:yang:ietf-ntp">
    <unicast-configuration><address>192.0.2.1</address>
      <type>uc-server</type></unicast-configuration>
  </ntp></config>)xml";

constexpr std::string_view kProposed = R"xml(<config>
  <ntp xmlns="urn:ietf:params:xml:ns:yang:ietf-ntp">
    <port>1123</port>
    <unicast-configuration><address>2001:db8::1</address>
      <type>uc-peer</type></unicast-configuration>
  </ntp></config>)xml";

bool HasDependency(const DangHardwareActionV1& action,
                   std::string_view expected) {
  for (std::size_t index = 0; index < action.dependency_count; ++index)
    if (std::string_view(action.dependencies[index]) == expected) return true;
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: ntp_plugin_contract_test PLUGIN\n";
    return 2;
  }
  void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  auto initialize = library ? reinterpret_cast<DangPluginInitV7>(
                                  dlsym(library, "dang_plugin_init_v7"))
                            : nullptr;
  const DangPluginV7* plugin = initialize ? initialize() : nullptr;
  if (!plugin) {
    std::cerr << (library ? "missing RFC 9249 v7 initializer" : dlerror())
              << '\n';
    return 1;
  }
  const DangPluginV1& base = plugin->v6.v5.v4.v3.v2.v1;
  bool valid =
      base.abi_version == DANG_PLUGIN_ABI_V7 && base.plugin_name &&
      std::string_view(base.plugin_name) == "ietf-ntp" &&
      base.yang_source_count(base.context) == 7 &&
      plugin->resource_domain_count(base.context) == 1 &&
      std::string_view(plugin->resource_domain_at(base.context, 0)) == "ntp";

  DangPluginErrorV1 error{};
  DangYangSourceV1 ntp_source{};
  valid = valid && base.yang_source_at(base.context, 0, &ntp_source, &error) &&
          ntp_source.role == DANG_YANG_IMPLEMENTED_V1;
  std::set<std::string> features;
  for (std::size_t index = 0; valid && index < ntp_source.enabled_feature_count;
       ++index)
    features.emplace(ntp_source.enabled_features[index]);
  valid = valid &&
          features ==
              std::set<std::string>({"authentication", "hex-key-string",
                                     "ntp-port", "unicast-configuration"}) &&
          !features.contains("access-rules");
  for (std::size_t index = 1; valid && index < 7; ++index) {
    DangYangSourceV1 imported{};
    valid = base.yang_source_at(base.context, index, &imported, &error) &&
            imported.role == DANG_YANG_IMPORT_ONLY_V1;
  }
  DangOperationalDataV2 operational{};
  error = {};
  valid = valid && plugin->v6.v5.get_operational_data_v2 &&
          !plugin->v6.v5.get_operational_data_v2(base.context, &operational,
                                                 &error) &&
          error.instance_path &&
          std::string_view(error.instance_path) == "/ietf-ntp:ntp/clock-state";

  const std::string before(kBefore);
  const std::string proposed(kProposed);
  DangTransactionV1 transaction{before.c_str(), proposed.c_str(), "[]"};
  void* prepared = nullptr;
  valid = valid &&
          base.prepare(base.context, &transaction, &prepared, &error) &&
          prepared && base.validate(base.context, prepared, &error) &&
          plugin->v6.v5.v4.hardware_action_count(base.context, prepared) == 5;

  std::vector<std::string> ids;
  DangHardwareActionV1 action{};
  for (std::size_t index = 0; valid && index < 5; ++index) {
    action = {};
    valid = plugin->v6.v5.v4.hardware_action_at(base.context, prepared, index,
                                                &action, &error) &&
            action.action_id && action.instance_path;
    if (valid) ids.emplace_back(action.action_id);
    if (valid && index == 0)
      valid = action.action_class == DANG_HARDWARE_DEACTIVATE_V1;
    if (valid && index == 4)
      valid = action.action_class == DANG_HARDWARE_ACTIVATE_V1 &&
              HasDependency(action, "configure-unicast-0");
  }
  valid =
      valid && ids == std::vector<std::string>(
                          {"stop-ntp-for-reconfiguration", "remove-unicast-0",
                           "configure-listener-port", "configure-unicast-0",
                           "restart-ntp-after-reconfiguration"});

  action = {};
  valid = valid &&
          plugin->v6.v5.v4.hardware_action_at(base.context, prepared, 0,
                                              &action, &error) &&
          !plugin->v6.v5.v4.apply_hardware_action(base.context, prepared,
                                                  action.action_id, &error) &&
          error.message && error.instance_path &&
          std::string_view(error.message).find("not implemented") !=
              std::string_view::npos &&
          std::string_view(error.instance_path) == "/ietf-ntp:ntp";
  if (prepared) base.release(base.context, prepared);

#if defined(__linux__)
  // The generic plugin delegates daemon semantics to the selected adapter.
  // chronyd does not implement burst startup on a symmetric peer.
  constexpr char chrony_semantic_error[] = R"xml(
    <ntp xmlns="urn:ietf:params:xml:ns:yang:ietf-ntp">
      <unicast-configuration><address>192.0.2.8</address>
        <type>uc-peer</type><iburst>true</iburst>
      </unicast-configuration>
    </ntp>)xml";
  transaction = {"<config/>", chrony_semantic_error, "[]"};
  prepared = nullptr;
  error = {};
  valid = valid &&
          base.prepare(base.context, &transaction, &prepared, &error) &&
          prepared && !base.validate(base.context, prepared, &error) &&
          error.instance_path &&
          std::string_view(error.instance_path) ==
              "/ietf-ntp:ntp/unicast-configuration/iburst";
  if (prepared) base.release(base.context, prepared);
#endif

#if defined(__linux__)
  constexpr const char* unsupported_daemon = "ntpd";
#elif defined(__FreeBSD__)
  constexpr const char* unsupported_daemon = "chrony";
#else
  constexpr const char* unsupported_daemon = "chrony";
#endif
  setenv("DANG_NTP_DAEMON", unsupported_daemon, 1);
  prepared = nullptr;
  error = {};
  valid = valid &&
          !base.prepare(base.context, &transaction, &prepared, &error) &&
          !prepared && error.message && error.instance_path &&
          std::string_view(error.message).find("not supported") !=
              std::string_view::npos &&
          std::string_view(error.instance_path) == "/ietf-ntp:ntp";
  unsetenv("DANG_NTP_DAEMON");

  constexpr char invalid[] =
      "<ntp xmlns=\"urn:ietf:params:xml:ns:yang:ietf-ntp\" "
      "xmlns:v=\"urn:vendor:ntp\"><v:clock/></ntp>";
  transaction = {"<config/>", invalid, "[]"};
  prepared = nullptr;
  error = {};
  valid = valid &&
          !base.prepare(base.context, &transaction, &prepared, &error) &&
          !prepared && error.instance_path &&
          std::string_view(error.instance_path) == "/ietf-ntp:ntp/clock";

  dlclose(library);
  if (!valid)
    std::cerr << "RFC 9249 plugin contract failed"
              << (error.message ? std::string(": ") + error.message : "")
              << '\n';
  return valid ? 0 : 1;
}

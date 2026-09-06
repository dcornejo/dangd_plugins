// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/plugin_api.h"

#include <dlfcn.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>

#include <unistd.h>

namespace {

void Write(const std::filesystem::path& path, std::string_view source) {
  std::ofstream output(path, std::ios::binary);
  output << source;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: frr_plugin_discovery_test PLUGIN\n";
    return 2;
  }
  const auto directory = std::filesystem::path("/tmp") /
      ("dang-frr-plugin-models-" + std::to_string(getpid()));
  std::filesystem::create_directory(directory);
  Write(directory / "frr-routing.yang",
        "module frr-routing { namespace \"http://frrouting.org/yang/routing\";"
        " prefix frr-rt; revision 2019-08-15; container routing {} }");
  Write(directory / "frr-staticd.yang",
        "module frr-staticd { namespace \"http://frrouting.org/yang/staticd\";"
        " prefix staticd; revision 2019-12-03; }");
  Write(directory / "frr-zebra.yang",
        "module frr-zebra { namespace \"http://frrouting.org/yang/zebra\";"
        " prefix zebra; revision 2019-06-01; container zebra {} }");
  Write(directory / "frr-ripd.yang",
        "module frr-ripd { namespace \"http://frrouting.org/yang/ripd\";"
        " prefix ripd; revision 2020-02-14;"
        " notification authentication-failure { leaf reason { type string; } } }");
  const auto library_path = directory / "yang-library.xml";
  Write(library_path, R"(<yang-library xmlns="urn:ietf:params:xml:ns:yang:ietf-yang-library"><module-set>
    <module><name>frr-routing</name><revision>2019-08-15</revision>
      <namespace>http://frrouting.org/yang/routing</namespace></module>
    <module><name>frr-staticd</name><revision>2019-12-03</revision>
      <namespace>http://frrouting.org/yang/staticd</namespace></module>
    <module><name>frr-zebra</name><revision>2019-06-01</revision>
      <namespace>http://frrouting.org/yang/zebra</namespace>
      <feature>ipv6-router-advertisements</feature></module>
    <module><name>frr-ripd</name><revision>2020-02-14</revision>
      <namespace>http://frrouting.org/yang/ripd</namespace></module>
  </module-set></yang-library>)");
  setenv("DANG_FRR_YANG_DIR", directory.c_str(), 1);
  setenv("DANG_FRR_YANG_LIBRARY_FILE", library_path.c_str(), 1);
  void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  auto initialize = library ? reinterpret_cast<DangPluginInitV8>(
      dlsym(library, "dang_plugin_init_v8")) : nullptr;
  const DangPluginV8* plugin = initialize ? initialize() : nullptr;
  bool valid = plugin &&
      plugin->v7.v6.v5.v4.v3.v2.v1.abi_version == DANG_PLUGIN_ABI_V8 &&
      std::string_view(plugin->v7.v6.v5.v4.v3.v2.v1.plugin_name) == "dang-frr" &&
      plugin->v7.resource_domain_count(
          plugin->v7.v6.v5.v4.v3.v2.v1.context) == 1 &&
      plugin->v7.v6.v5.v4.v3.get_operational_data != nullptr &&
      plugin->v7.v6.reconcile_applied_configuration != nullptr &&
      plugin->next_notification != nullptr &&
      std::string_view(plugin->v7.resource_domain_at(
          plugin->v7.v6.v5.v4.v3.v2.v1.context, 0)) == "routing";
  std::set<std::string> implemented;
  bool found_runtime_feature = false;
  if (valid) {
    const DangPluginV1& base = plugin->v7.v6.v5.v4.v3.v2.v1;
    for (std::size_t index = 0; index < base.yang_source_count(base.context);
         ++index) {
      DangYangSourceV1 source{};
      DangPluginErrorV1 error{};
      if (!base.yang_source_at(base.context, index, &source, &error)) {
        valid = false;
        break;
      }
      if (source.role == DANG_YANG_IMPLEMENTED_V1)
        implemented.emplace(source.module_name);
      if (std::string_view(source.module_name) == "frr-zebra" &&
          source.enabled_feature_count == 1 && source.enabled_features &&
          std::string_view(source.enabled_features[0]) ==
              "ipv6-router-advertisements")
        found_runtime_feature = true;
    }
    valid = valid && found_runtime_feature && implemented ==
        std::set<std::string>({"dang-frr-monitoring", "frr-ripd",
                               "frr-routing", "frr-staticd", "frr-zebra"});
    DangNotificationV1 event{};
    DangPluginErrorV1 notification_error{};
    valid = valid && plugin->next_notification(base.context, &event,
                                                 &notification_error) == 0;
    const std::string before = "<config/>";
    const std::string proposed =
        "<config><routing xmlns=\"http://frrouting.org/yang/routing\"/>"
        "</config>";
    DangTransactionV1 transaction{before.c_str(), proposed.c_str(), "[]"};
    DangPluginErrorV1 error{};
    void* prepared = nullptr;
    valid = valid && base.prepare(base.context, &transaction, &prepared, &error);
    DangHardwareActionV1 action{};
    valid = valid && prepared &&
        plugin->v7.v6.v5.v4.hardware_action_count(base.context, prepared) == 1 &&
        plugin->v7.v6.v5.v4.hardware_action_at(base.context, prepared, 0,
                                               &action, &error) &&
        action.action_id && std::string_view(action.action_id) == "configuration" &&
        action.action_class == DANG_HARDWARE_NORMAL_V1 &&
        action.dependency_count == 0;
    if (prepared) base.release(base.context, prepared);
  }
  if (library) dlclose(library);
  std::filesystem::remove_all(directory);
  if (!valid)
    std::cerr << (library ? "FRR ABI-v8 discovery contract failed" : dlerror())
              << '\n';
  return valid ? 0 : 1;
}

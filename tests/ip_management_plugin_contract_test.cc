// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/** @file Verifies operational publication before any owned configuration. */

#include "dangd/plugin_api.h"

#include <dlfcn.h>

#include <iostream>
#include <string>
#include <string_view>

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: ip_management_plugin_contract_test PLUGIN\n";
    return 2;
  }
  void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  auto initialize = library ? reinterpret_cast<DangPluginInitV4>(
                                  dlsym(library, "dang_plugin_init_v4"))
                            : nullptr;
  const DangPluginV4 *plugin = initialize ? initialize() : nullptr;
  if (!plugin || !plugin->v3.get_operational_data) {
    std::cerr << (library ? "missing IP plugin v4 operational callback"
                          : dlerror())
              << '\n';
    return 1;
  }

  const DangPluginV1 &base = plugin->v3.v2.v1;
  DangYangSourceV1 interfaces_source{};
  DangYangSourceV1 if_type_source{};
  DangOperationalDataV1 state{};
  DangPluginErrorV1 error{};
  const bool valid =
      base.yang_source_at &&
      base.yang_source_at(base.context, 0, &interfaces_source, &error) &&
      interfaces_source.enabled_feature_count == 1 &&
      interfaces_source.enabled_features &&
      interfaces_source.enabled_features[0] &&
      std::string_view(interfaces_source.enabled_features[0]) == "if-mib" &&
      base.yang_source_count(base.context) == 3 &&
      base.yang_source_at(base.context, 2, &if_type_source, &error) &&
      if_type_source.role == DANG_YANG_IMPORT_ONLY_V1 &&
      if_type_source.module_name &&
      std::string_view(if_type_source.module_name) == "iana-if-type" &&
      plugin->v3.get_operational_data(base.context, &state, &error) &&
      state.data_xml &&
      std::string_view(state.data_xml).find("<interfaces-state") !=
          std::string_view::npos;
  if (!valid) {
    std::cerr << "empty-startup operational state failed"
              << (error.message ? std::string(": ") + error.message : "")
              << '\n';
    if (base.destroy)
      base.destroy(base.context);
    dlclose(library);
    return 1;
  }
  if (base.destroy)
    base.destroy(base.context);
  dlclose(library);
  return 0;
}

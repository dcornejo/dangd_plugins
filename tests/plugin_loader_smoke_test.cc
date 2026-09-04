// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/plugin_api.h"

#include <dlfcn.h>

#include <cstring>
#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "usage: plugin_loader_smoke_test PLUGIN NAME SOURCE_MODULE\n";
    return 2;
  }
  void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!library) {
    std::cerr << dlerror() << '\n';
    return 1;
  }
  const DangPluginV1* plugin = nullptr;
  if (auto initialize_v7 = reinterpret_cast<DangPluginInitV7>(
          dlsym(library, "dang_plugin_init_v7")))
    plugin = &initialize_v7()->v6.v5.v4.v3.v2.v1;
  else if (auto initialize_v6 = reinterpret_cast<DangPluginInitV6>(
          dlsym(library, "dang_plugin_init_v6")))
    plugin = &initialize_v6()->v5.v4.v3.v2.v1;
  else if (auto initialize_v5 = reinterpret_cast<DangPluginInitV5>(
               dlsym(library, "dang_plugin_init_v5")))
    plugin = &initialize_v5()->v4.v3.v2.v1;
  else if (auto initialize_v4 = reinterpret_cast<DangPluginInitV4>(
               dlsym(library, "dang_plugin_init_v4")))
    plugin = &initialize_v4()->v3.v2.v1;
  else if (auto initialize_v3 = reinterpret_cast<DangPluginInitV3>(
               dlsym(library, "dang_plugin_init_v3")))
    plugin = &initialize_v3()->v2.v1;
  else if (auto initialize_v2 = reinterpret_cast<DangPluginInitV2>(
               dlsym(library, "dang_plugin_init_v2")))
    plugin = &initialize_v2()->v1;
  else if (auto initialize_v1 = reinterpret_cast<DangPluginInitV1>(
               dlsym(library, "dang_plugin_init_v1")))
    plugin = initialize_v1();
  DangYangSourceV1 source{};
  DangPluginErrorV1 error{};
  const bool valid = plugin && plugin->abi_version >= DANG_PLUGIN_ABI_V1 &&
      plugin->abi_version <= DANG_PLUGIN_ABI_V7 &&
      plugin->plugin_name &&
      std::string_view(plugin->plugin_name) == argv[2] &&
      plugin->yang_source_count && plugin->yang_source_count(plugin->context) >= 1 &&
      plugin->yang_source_at &&
      plugin->yang_source_at(plugin->context, 0, &source, &error) &&
      source.module_name &&
      std::string_view(source.module_name) == argv[3] &&
      source.source && source.source_size == std::strlen(source.source) &&
      std::string_view(source.source, source.source_size).find(
          std::string("module ") + argv[3]) != std::string_view::npos;
  if (plugin && plugin->destroy) plugin->destroy(plugin->context);
  dlclose(library);
  if (!valid) {
    std::cerr << "plugin discovery contract failed"
              << (error.message ? std::string(": ") + error.message : "")
              << '\n';
    return 1;
  }
  return 0;
}

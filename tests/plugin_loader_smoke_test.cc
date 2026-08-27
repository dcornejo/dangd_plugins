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
  auto initialize = reinterpret_cast<DangPluginInitV1>(
      dlsym(library, "dang_plugin_init_v1"));
  const DangPluginV1* plugin = initialize ? initialize() : nullptr;
  DangYangSourceV1 source{};
  DangPluginErrorV1 error{};
  const bool valid = plugin && plugin->abi_version == DANG_PLUGIN_ABI_V1 &&
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

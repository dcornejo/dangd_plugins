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
  setenv("DANG_FRR_YANG_DIR", directory.c_str(), 1);
  void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  auto initialize = library ? reinterpret_cast<DangPluginInitV7>(
      dlsym(library, "dang_plugin_init_v7")) : nullptr;
  const DangPluginV7* plugin = initialize ? initialize() : nullptr;
  bool valid = plugin &&
      plugin->v6.v5.v4.v3.v2.v1.abi_version == DANG_PLUGIN_ABI_V7 &&
      std::string_view(plugin->v6.v5.v4.v3.v2.v1.plugin_name) == "dang-frr" &&
      plugin->resource_domain_count(plugin->v6.v5.v4.v3.v2.v1.context) == 1 &&
      plugin->v6.v5.v4.v3.get_operational_data != nullptr &&
      plugin->v6.reconcile_applied_configuration != nullptr &&
      std::string_view(plugin->resource_domain_at(
          plugin->v6.v5.v4.v3.v2.v1.context, 0)) == "routing";
  std::set<std::string> implemented;
  if (valid) {
    const DangPluginV1& base = plugin->v6.v5.v4.v3.v2.v1;
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
    }
    valid = valid && implemented ==
        std::set<std::string>({"frr-routing", "frr-staticd", "frr-zebra"});
    const std::string before = "<config/>";
    const std::string proposed =
        "<config><routing xmlns=\"http://frrouting.org/yang/routing\"/>"
        "</config>";
    DangTransactionV1 transaction{before.c_str(), proposed.c_str(), "[]"};
    DangPluginErrorV1 error{};
    void* prepared = nullptr;
    valid = valid && base.prepare(base.context, &transaction, &prepared, &error);
    if (prepared) base.release(base.context, prepared);
  }
  if (library) dlclose(library);
  std::filesystem::remove_all(directory);
  if (!valid)
    std::cerr << (library ? "FRR ABI-v7 discovery contract failed" : dlerror())
              << '\n';
  return valid ? 0 : 1;
}

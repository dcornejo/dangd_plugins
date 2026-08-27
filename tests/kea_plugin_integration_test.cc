// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/plugin_api.h"

#include <dlfcn.h>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

std::string Read(const char* path) {
  std::ifstream input(path);
  std::ostringstream output;
  output << input.rdbuf();
  return output.str();
}

bool Report(const char* phase, const DangPluginErrorV1& error) {
  std::cerr << phase << " failed: "
            << (error.message ? error.message : "no plugin message");
  if (error.instance_path) std::cerr << " at " << error.instance_path;
  std::cerr << '\n';
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "usage: kea_plugin_integration_test PLUGIN BEFORE PROPOSED\n";
    return 2;
  }
  const std::string before = Read(argv[2]);
  const std::string proposed = Read(argv[3]);
  void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  auto initialize = library ? reinterpret_cast<DangPluginInitV1>(
      dlsym(library, "dang_plugin_init_v1")) : nullptr;
  const DangPluginV1* plugin = initialize ? initialize() : nullptr;
  if (!plugin) {
    std::cerr << (library ? "missing plugin initializer" : dlerror()) << '\n';
    return 1;
  }
  DangTransactionV1 transaction{before.c_str(), proposed.c_str(), "[]"};
  DangPluginErrorV1 error{};
  void* prepared = nullptr;
  bool valid = plugin->prepare(plugin->context, &transaction, &prepared, &error)
      || Report("prepare", error);
  if (valid)
    valid = plugin->validate(plugin->context, prepared, &error)
        || Report("validate", error);
  if (valid)
    valid = plugin->apply(plugin->context, prepared, &error)
        || Report("apply", error);
  if (valid)
    valid = plugin->rollback(plugin->context, prepared, &error)
        || Report("rollback", error);
  if (prepared) plugin->release(plugin->context, prepared);
  if (plugin->destroy) plugin->destroy(plugin->context);
  dlclose(library);
  if (valid)
    std::cout << "Kea DHCPv4 and DHCPv6 validate, apply, and rollback passed\n";
  return valid ? 0 : 1;
}

// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/** @file Live ABI-v7 VPP provider apply and rollback contract check. */

#include "dangd/plugin_api.h"

#include <dlfcn.h>

#include <iostream>
#include <string>

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!library) { std::cerr << dlerror() << '\n'; return 1; }
  auto init = reinterpret_cast<DangPluginInitV7>(
      dlsym(library, "dang_plugin_init_v7"));
  const DangPluginV7* plugin = init ? init() : nullptr;
  if (!plugin) { std::cerr << "VPP ABI-v7 entry point is missing\n"; return 1; }
  constexpr char kBefore[] = "<config/>";
  constexpr char kProposed[] =
      "<config><vpp-interfaces xmlns=\"urn:dang:vpp:interfaces\">"
      "<loopback><instance>73</instance><enabled>true</enabled></loopback>"
      "</vpp-interfaces></config>";
  const DangTransactionV1 transaction{kBefore, kProposed, "[]"};
  DangPluginErrorV1 error{};
  void* prepared = nullptr;
  const auto& api = plugin->v6.v5.v4.v3.v2.v1;
  if (!api.prepare(api.context, &transaction, &prepared, &error) ||
      !api.validate(api.context, prepared, &error)) {
    std::cerr << (error.message ? error.message : "prepare failed") << '\n';
    return 1;
  }
  if (plugin->v6.v5.v4.hardware_action_count(api.context, prepared) != 1) {
    std::cerr << "VPP provider did not publish its transaction action\n";
    return 1;
  }
  if (!plugin->v6.v5.v4.apply_hardware_action(
          api.context, prepared, "software-interfaces", &error)) {
    std::cerr << (error.message ? error.message : "apply failed") << '\n';
    return 1;
  }
  std::cout << "provider created and enabled loop73\n";
  DangOperationalDataV2 operational{};
  if (!plugin->v6.v5.get_operational_data_v2(
          api.context, &operational, &error) || !operational.complete ||
      !operational.data_xml ||
      std::string(operational.data_xml).find("<instance>73</instance>") ==
          std::string::npos) {
    std::cerr << "live loop73 was not published as complete state\n";
    return 1;
  }
  DangAppliedConfigurationV1 reconciled{};
  constexpr char kCurrent[] =
      "<config><unrelated xmlns=\"urn:test\">keep</unrelated></config>";
  if (!plugin->v6.reconcile_applied_configuration(
          api.context, prepared, kCurrent, &reconciled, &error) ||
      !reconciled.applied_xml ||
      std::string(reconciled.applied_xml).find("keep") == std::string::npos ||
      std::string(reconciled.applied_xml).find("<instance>73</instance>") ==
          std::string::npos) {
    std::cerr << "VPP applied-state reconciliation failed\n";
    return 1;
  }
  if (!plugin->v6.v5.v4.rollback_hardware_action(
          api.context, prepared, "software-interfaces", &error)) {
    std::cerr << (error.message ? error.message : "rollback failed") << '\n';
    return 1;
  }
  std::cout << "provider restored loop73 absence\n";
  if (!plugin->v6.v5.get_operational_data_v2(
          api.context, &operational, &error) ||
      std::string(operational.data_xml).find("<instance>73</instance>") !=
          std::string::npos) {
    std::cerr << "rolled-back loop73 remains in operational state\n";
    return 1;
  }
  api.release(api.context, prepared);
  dlclose(library);
}

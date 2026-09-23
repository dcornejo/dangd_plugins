// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/plugin_api.h"

#include "kea_adapter.h"

#include <dlfcn.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

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

bool NativeCommand(const char* socket, std::string_view command,
                   const nlohmann::json& arguments) {
  std::string reason;
  auto response = dang::plugins::kea::SendControlQuery(
      socket ? socket : "", command, arguments, &reason);
  if (response && dang::plugins::kea::CommandSucceeded(*response, &reason))
    return true;
  std::cerr << command << " failed: " << reason << '\n';
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
  const bool no_op = before == proposed;
  const bool skip_operational =
      std::getenv("DANG_KEA_SKIP_OPERATIONAL") != nullptr;
  void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  auto initialize = library ? reinterpret_cast<DangPluginInitV6>(
      dlsym(library, "dang_plugin_init_v6")) : nullptr;
  const DangPluginV6* plugin6 = initialize ? initialize() : nullptr;
  const DangPluginV5* plugin5 = plugin6 ? &plugin6->v5 : nullptr;
  const DangPluginV1* plugin = plugin5 ? &plugin5->v4.v3.v2.v1 : nullptr;
  if (!plugin6) {
    std::cerr << (library ? "missing plugin initializer" : dlerror()) << '\n';
    return 1;
  }
  DangTransactionV1 transaction{before.c_str(), proposed.c_str(), "[]"};
  DangPluginErrorV1 error{};
  DangAppliedConfigurationV1 reconciled{};
  bool valid = plugin6->reconcile_applied_configuration(
                   plugin->context, nullptr, before.c_str(), &reconciled,
                   &error) ||
               Report("startup reconciliation", error);
  valid = valid && reconciled.applied_xml == before.c_str() &&
          reconciled.outcomes == nullptr && reconciled.outcome_count == 0;
  void* prepared = nullptr;
  if (valid)
    valid = plugin->prepare(plugin->context, &transaction, &prepared, &error)
        || Report("prepare", error);
  if (valid)
    valid = plugin->validate(plugin->context, prepared, &error)
        || Report("validate", error);
  if (valid)
    valid = plugin5->v4.hardware_action_count(plugin->context, prepared) == 1;
  DangHardwareActionV1 action{};
  if (valid)
    valid = plugin5->v4.hardware_action_at(plugin->context, prepared, 0,
                                           &action, &error)
        || Report("hardware action", error);
  if (valid)
    valid = (action.action_id &&
        plugin5->v4.apply_hardware_action(plugin->context, prepared,
                                          action.action_id, &error))
        || Report("apply", error);
  if (valid && !no_op && !skip_operational)
    valid = NativeCommand(std::getenv("DANG_KEA_DHCP4_SOCKET"), "lease4-add",
                          {{"subnet-id", 401},
                           {"ip-address", "192.0.2.80"},
                           {"hw-address", "02:00:00:00:04:01"}});
  if (valid && !no_op && !skip_operational)
    valid = NativeCommand(std::getenv("DANG_KEA_DHCP6_SOCKET"), "lease6-add",
                          {{"subnet-id", 601},
                           {"ip-address", "2001:db8:6::180"},
                           {"duid", "00:01:00:01:02:03:04:05:06:07:08:09"},
                           {"iaid", 1234}});
  if (valid && !no_op && !skip_operational) {
    DangOperationalDataV2 state{};
    valid = plugin5->get_operational_data_v2(plugin->context, &state, &error)
        || Report("operational", error);
    const std::string xml = valid && state.data_xml ? state.data_xml : "";
    valid = valid && state.complete == 1 &&
        xml.find("urn:ietf:params:xml:ns:yang:kea-dhcp4-server") !=
            std::string::npos &&
        xml.find("urn:ietf:params:xml:ns:yang:kea-dhcp6-server") !=
            std::string::npos &&
        xml.find("<leases") != std::string::npos &&
        xml.find("<ip-address>192.0.2.80</ip-address>") !=
            std::string::npos &&
        xml.find("<hw-address>AgAAAAQB</hw-address>") !=
            std::string::npos &&
        xml.find("<ip-address>2001:db8:6::180</ip-address>") !=
            std::string::npos &&
        xml.find("<duid>AAEAAQIDBAUGBwgJ</duid>") !=
            std::string::npos &&
        xml.find("<iaid>1234</iaid>") != std::string::npos &&
        xml.find("<lease-stats") != std::string::npos &&
        xml.find("<assigned-addresses>1</assigned-addresses>") !=
            std::string::npos &&
        xml.find("<assigned-nas>1</assigned-nas>") != std::string::npos &&
        xml.find("<hosts") != std::string::npos &&
        xml.find("<subnet-id>401</subnet-id>") != std::string::npos &&
        xml.find("<subnet-id>601</subnet-id>") != std::string::npos &&
        xml.find("<identifier>00:01:02:03:04:05</identifier>") !=
            std::string::npos &&
        xml.find("<identifier>00:01:02:03</identifier>") != std::string::npos &&
        xml.find("<space>dhcp4</space>") != std::string::npos &&
        xml.find("<data>printer.example</data>") !=
            std::string::npos &&
        xml.find("<space>dhcp6</space>") != std::string::npos &&
        xml.find("<data>2001:db8:6::53</data>") !=
            std::string::npos;
    if (!valid) std::cerr << "operational XML is incomplete: " << xml << '\n';
  }
  if (valid)
    valid = plugin5->v4.rollback_hardware_action(plugin->context, prepared,
                                                 action.action_id, &error)
        || Report("rollback", error);
  if (prepared) plugin->release(plugin->context, prepared);
  if (plugin->destroy) plugin->destroy(plugin->context);
  dlclose(library);
  if (valid)
    std::cout << (no_op
        ? "Kea no-op validate, apply, and rollback passed\n"
        : skip_operational
            ? "Kea selective validate, apply, and rollback passed\n"
            : "Kea DHCPv4 and DHCPv6 validate, apply, and rollback passed\n");
  return valid ? 0 : 1;
}

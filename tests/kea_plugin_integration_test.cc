// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/plugin_api.h"

#include "kea_adapter.h"

#include <dlfcn.h>
#include <unistd.h>

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

bool AddDhcp6PagingLeases() {
  const char* socket = std::getenv("DANG_KEA_DHCP6_SOCKET");
  // CollectLeasePages requests 256 rows. Add exactly 256 leases beyond the
  // ordinary fixture lease so a native daemon must return a second page.
  // These addresses remain inside the documentation-only /64 used by the
  // isolated platform tests and never reach a host LAN interface.
  for (unsigned int index = 0; index < 256; ++index) {
    std::ostringstream address;
    address << "2001:db8:6::" << std::hex << 0x1000 + index;
    if (!NativeCommand(socket, "lease6-add",
                       {{"subnet-id", 601},
                        {"ip-address", address.str()},
                        {"duid", "00:01:00:01:02:03:04:05:06:07:08:0a"},
                        {"iaid", 2000 + index}})) {
      std::cerr << "cannot create DHCPv6 paging lease " << index << '\n';
      return false;
    }
  }
  return true;
}

bool AddDhcp6PagingReservations() {
  const char* socket = std::getenv("DANG_KEA_DHCP6_SOCKET");
  std::string reason;
  auto response = dang::plugins::kea::SendControlQuery(
      socket ? socket : "", "config-get", nlohmann::json::object(), &reason);
  if (!response ||
      !dang::plugins::kea::CommandSucceeded(*response, &reason)) {
    std::cerr << "config-get failed before host paging: " << reason << '\n';
    return false;
  }
  try {
    const nlohmann::json& answer = response->is_array()
        ? response->at(0) : *response;
    nlohmann::json arguments = answer.at("arguments");
    // The content hash describes config-get output and is not accepted by
    // config-set. Keep every other native setting exactly as Kea returned it.
    arguments.erase("hash");
    auto& reservations =
        arguments.at("Dhcp6").at("subnet6").at(0).at("reservations");
    static constexpr char kHex[] = "0123456789abcdef";
    // CollectHostPages requests 256 rows. The modeled transaction already
    // installs one reservation, so these additions force a second native page.
    for (unsigned int index = 0; index < 256; ++index) {
      std::string suffix;
      suffix.push_back(kHex[(index >> 4) & 0xf]);
      suffix.push_back(kHex[index & 0xf]);
      std::ostringstream address;
      address << "2001:db8:6::" << std::hex << 0x2000 + index;
      reservations.push_back(
          {{"duid", "00:01:00:01:02:03:04:05:06:07:08:" + suffix},
           {"hostname", "paging-host-" + std::to_string(index)},
           {"ip-addresses", {address.str()}}});
    }
    return NativeCommand(socket, "config-set", arguments);
  } catch (const std::exception& exception) {
    std::cerr << "cannot prepare host paging: " << exception.what() << '\n';
    return false;
  }
}

const char* SocketForModule(std::string_view module) {
  if (module == "kea-dhcp4-server")
    return std::getenv("DANG_KEA_DHCP4_SOCKET");
  if (module == "kea-dhcp6-server")
    return std::getenv("DANG_KEA_DHCP6_SOCKET");
  return nullptr;
}

bool RemoveHostHook(std::string_view module) {
  const char* socket = SocketForModule(module);
  const char* service = module == "kea-dhcp4-server" ? "Dhcp4"
      : module == "kea-dhcp6-server" ? "Dhcp6" : nullptr;
  if (!socket || !*socket || !service) return false;
  std::string reason;
  auto response = dang::plugins::kea::SendControlQuery(
      socket, "config-get", nlohmann::json::object(), &reason);
  if (!response ||
      !dang::plugins::kea::CommandSucceeded(*response, &reason)) {
    std::cerr << "config-get failed before host-hook drift: " << reason << '\n';
    return false;
  }
  try {
    const nlohmann::json& answer = response->is_array()
        ? response->at(0) : *response;
    nlohmann::json arguments = answer.at("arguments");
    // config-get adds a read-only content hash that config-set rejects.
    arguments.erase("hash");
    auto& hooks = arguments.at(service).at("hooks-libraries");
    std::size_t removed = 0;
    for (auto hook = hooks.begin(); hook != hooks.end();) {
      const auto library = hook->find("library");
      if (library != hook->end() && library->is_string() &&
          library->get_ref<const std::string&>().ends_with(
              "/libdhcp_host_cmds.so")) {
        hook = hooks.erase(hook);
        ++removed;
      } else {
        ++hook;
      }
    }
    if (removed != 1) {
      std::cerr << "config-get did not contain exactly one host hook\n";
      return false;
    }
    return NativeCommand(socket, "config-set", arguments);
  } catch (const std::exception& exception) {
    std::cerr << "cannot prepare host-hook drift: " << exception.what() << '\n';
    return false;
  }
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
  const bool force_lease_paging =
      std::getenv("DANG_KEA_FORCE_LEASE_PAGING") != nullptr;
  const bool force_host_paging =
      std::getenv("DANG_KEA_FORCE_HOST_PAGING") != nullptr;
  const char* expected_rollback_failure =
      std::getenv("DANG_KEA_EXPECT_ROLLBACK_FAILURE");
  const char* expected_apply_failure =
      std::getenv("DANG_KEA_EXPECT_APPLY_FAILURE");
  const char* expected_reconcile_failure =
      std::getenv("DANG_KEA_EXPECT_RECONCILE_FAILURE");
  const char* expected_validate_failure =
      std::getenv("DANG_KEA_EXPECT_VALIDATE_FAILURE");
  const char* expected_operational_failure =
      std::getenv("DANG_KEA_EXPECT_OPERATIONAL_FAILURE");
  const char* expected_operational_subtree =
      std::getenv("DANG_KEA_EXPECT_OPERATIONAL_SUBTREE");
  const char* remove_host_hook = std::getenv("DANG_KEA_REMOVE_HOST_HOOK");
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
  if (valid) {
    const bool validated =
        plugin->validate(plugin->context, prepared, &error);
    if (expected_validate_failure) {
      const std::string expected_path =
          "/{urn:ietf:params:xml:ns:yang:" +
          std::string(expected_validate_failure) + "}config";
      valid = !validated && error.message && error.instance_path &&
          std::string_view(error.message).find(expected_validate_failure) !=
              std::string_view::npos &&
          std::string_view(error.instance_path) == expected_path;
      if (!valid) Report("expected validation rejection", error);
    } else {
      valid = validated || Report("validate", error);
    }
  }
  if (valid && !expected_validate_failure)
    valid = plugin5->v4.hardware_action_count(plugin->context, prepared) == 1;
  DangHardwareActionV1 action{};
  if (valid && !expected_validate_failure)
    valid = plugin5->v4.hardware_action_at(plugin->context, prepared, 0,
                                           &action, &error)
        || Report("hardware action", error);
  if (valid && !expected_validate_failure && expected_apply_failure) {
    const char* socket = SocketForModule(expected_apply_failure);
    valid = socket && *socket && ::unlink(socket) == 0;
    if (!valid) std::cerr << "cannot remove expected apply socket\n";
  }
  if (valid && !expected_validate_failure) {
    const bool applied = action.action_id &&
        plugin5->v4.apply_hardware_action(plugin->context, prepared,
                                          action.action_id, &error);
    if (expected_apply_failure) {
      const std::string expected_path =
          "/{urn:ietf:params:xml:ns:yang:" +
          std::string(expected_apply_failure) + "}config";
      valid = !applied && error.message && error.instance_path &&
          std::string_view(error.message).find(expected_apply_failure) !=
              std::string_view::npos &&
          std::string_view(error.message).find("rollback") !=
              std::string_view::npos &&
          std::string_view(error.instance_path) == expected_path;
      if (!valid) Report("expected apply rejection", error);
    } else {
      valid = applied || Report("apply", error);
    }
  }
  if (valid && expected_reconcile_failure)
    valid = RemoveHostHook(expected_reconcile_failure);
  if (valid && !expected_validate_failure && !expected_apply_failure) {
    DangAppliedConfigurationV1 applied{};
    const bool reconciliation_accepted =
        plugin6->reconcile_applied_configuration(
        plugin->context, prepared, proposed.c_str(), &applied, &error);
    if (expected_reconcile_failure) {
      const std::string expected_path =
          "/{urn:ietf:params:xml:ns:yang:" +
          std::string(expected_reconcile_failure) + "}config";
      valid = !reconciliation_accepted && error.message && error.instance_path &&
          std::string_view(error.message).find(expected_reconcile_failure) !=
              std::string_view::npos &&
          std::string_view(error.instance_path) == expected_path;
      if (!valid) Report("expected reconciliation rejection", error);
    } else {
      valid = reconciliation_accepted ||
          Report("post-apply reconciliation", error);
      valid = valid && applied.applied_xml == proposed.c_str() &&
          applied.outcomes == nullptr && applied.outcome_count == 0;
    }
  }
  if (valid && !expected_validate_failure && expected_rollback_failure) {
    const char* socket = SocketForModule(expected_rollback_failure);
    valid = socket && *socket && ::unlink(socket) == 0;
    if (!valid) std::cerr << "cannot remove expected rollback socket\n";
  }
  if (valid && force_host_paging && !expected_validate_failure &&
      !expected_apply_failure && !no_op && !skip_operational)
    valid = AddDhcp6PagingReservations();
  if (valid && !expected_validate_failure && !expected_apply_failure &&
      !no_op && !skip_operational)
    valid = NativeCommand(std::getenv("DANG_KEA_DHCP4_SOCKET"), "lease4-add",
                          {{"subnet-id", 401},
                           {"ip-address", "192.0.2.80"},
                           {"hw-address", "02:00:00:00:04:01"}});
  if (valid && !expected_validate_failure && !expected_apply_failure &&
      !no_op && !skip_operational)
    valid = NativeCommand(std::getenv("DANG_KEA_DHCP6_SOCKET"), "lease6-add",
                          {{"subnet-id", 601},
                           {"ip-address", "2001:db8:6::180"},
                           {"duid", "00:01:00:01:02:03:04:05:06:07:08:09"},
                           {"iaid", 1234}});
  if (valid && force_lease_paging && !expected_validate_failure &&
      !expected_apply_failure && !no_op && !skip_operational)
    valid = AddDhcp6PagingLeases();
  if (valid && !expected_validate_failure && !expected_apply_failure &&
      !no_op && !skip_operational) {
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
        (!force_lease_paging ||
         (xml.find("<ip-address>2001:db8:6::10ff</ip-address>") !=
              std::string::npos &&
          xml.find("<iaid>2255</iaid>") != std::string::npos)) &&
        xml.find("<lease-stats") != std::string::npos &&
        xml.find("<assigned-addresses>1</assigned-addresses>") !=
            std::string::npos &&
        xml.find(force_lease_paging ? "<assigned-nas>257</assigned-nas>"
                                    : "<assigned-nas>1</assigned-nas>") !=
            std::string::npos &&
        xml.find("<hosts") != std::string::npos &&
        xml.find("<subnet-id>401</subnet-id>") != std::string::npos &&
        xml.find("<subnet-id>601</subnet-id>") != std::string::npos &&
        xml.find("<identifier>00:01:02:03:04:05</identifier>") !=
            std::string::npos &&
        xml.find("<identifier>00:01:02:03</identifier>") != std::string::npos &&
        (!force_host_paging ||
         (xml.find("<identifier>00:01:00:01:02:03:04:05:06:07:08:ff"
                   "</identifier>") != std::string::npos &&
          xml.find("<hostname>paging-host-255</hostname>") !=
              std::string::npos)) &&
        xml.find("<space>dhcp4</space>") != std::string::npos &&
        xml.find("<data>printer.example</data>") !=
            std::string::npos &&
        xml.find("<space>dhcp6</space>") != std::string::npos &&
        xml.find("<data>2001:db8:6::53</data>") !=
            std::string::npos;
    if (!valid) std::cerr << "operational XML is incomplete: " << xml << '\n';
  }
  if (valid && remove_host_hook)
    valid = RemoveHostHook(remove_host_hook);
  if (valid && expected_operational_failure) {
    DangOperationalDataV2 state{};
    const bool retrieved =
        plugin5->get_operational_data_v2(plugin->context, &state, &error);
    const std::string expected_path =
        "/{urn:ietf:params:xml:ns:yang:" +
        std::string(expected_operational_failure) + "}state/" +
        (expected_operational_subtree ? expected_operational_subtree
                                      : "leases");
    valid = !retrieved && error.message && error.instance_path &&
        std::string_view(error.message).find(expected_operational_failure) !=
            std::string_view::npos &&
        std::string_view(error.instance_path) == expected_path;
    if (!valid) Report("expected operational rejection", error);
  }
  if (valid && !expected_validate_failure && !expected_apply_failure) {
    const bool rolled_back = plugin5->v4.rollback_hardware_action(
        plugin->context, prepared, action.action_id, &error);
    if (expected_rollback_failure) {
      const std::string expected_path =
          "/{urn:ietf:params:xml:ns:yang:" +
          std::string(expected_rollback_failure) + "}config";
      valid = !rolled_back && error.message && error.instance_path &&
          std::string_view(error.message).find(expected_rollback_failure) !=
              std::string_view::npos &&
          std::string_view(error.instance_path) == expected_path;
      if (!valid) Report("expected rollback rejection", error);
    } else {
      valid = rolled_back || Report("rollback", error);
    }
  }
  if (prepared) plugin->release(plugin->context, prepared);
  if (plugin->destroy) plugin->destroy(plugin->context);
  dlclose(library);
  if (valid)
    std::cout << (expected_operational_failure
        ? "Kea operational failure attribution passed\n"
        : expected_validate_failure
        ? "Kea validation failure attribution passed\n"
        : expected_apply_failure
        ? "Kea apply failure attribution and compensation passed\n"
        : expected_reconcile_failure
        ? "Kea post-apply reconciliation rejection passed\n"
        : expected_rollback_failure
        ? "Kea rollback failure attribution passed\n"
        : no_op
        ? "Kea no-op validate, apply, and rollback passed\n"
        : skip_operational
            ? "Kea selective validate, apply, and rollback passed\n"
            : "Kea DHCPv4 and DHCPv6 validate, apply, and rollback passed\n");
  return valid ? 0 : 1;
}

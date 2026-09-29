// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/plugin_api.h"

#include "kea_adapter.h"

#include <dlfcn.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
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

bool AddDhcp6PagingReservations(std::string* xml) {
  if (!xml) return false;
  const std::size_t insertion = xml->rfind("</subnet6>");
  if (insertion == std::string::npos) return false;
  static constexpr char kHex[] = "0123456789abcdef";
  std::ostringstream reservations;
  // The proposed datastore already has one DHCPv6 reservation. Add 256 more
  // to authoritative dangd intent so the native operational read crosses the
  // 256-row host page without manufacturing out-of-band configuration drift.
  for (unsigned int index = 0; index < 256; ++index) {
    const char high = kHex[(index >> 4) & 0xf];
    const char low = kHex[index & 0xf];
    reservations << "<host><identifier-type>duid</identifier-type>"
                 << "<identifier>00:01:00:01:02:03:04:05:06:07:08:"
                 << high << low << "</identifier><ip-addresses>"
                 << "2001:db8:6::" << std::hex << 0x2000 + index << std::dec
                 << "</ip-addresses><hostname>paging-host-" << index
                 << "</hostname></host>";
  }
  xml->insert(insertion, reservations.str());
  return true;
}

const char* SocketForModule(std::string_view module) {
  if (module == "kea-dhcp4-server")
    return std::getenv("DANG_KEA_DHCP4_SOCKET");
  if (module == "kea-dhcp6-server")
    return std::getenv("DANG_KEA_DHCP6_SOCKET");
  return nullptr;
}

struct SavedConfiguration {
  std::string socket;
  nlohmann::json arguments;
};

std::optional<SavedConfiguration> saved_configuration;

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
    if (!NativeCommand(socket, "config-set", arguments)) return false;
    saved_configuration = SavedConfiguration{socket, answer.at("arguments")};
    saved_configuration->arguments.erase("hash");
    return true;
  } catch (const std::exception& exception) {
    std::cerr << "cannot prepare host-hook drift: " << exception.what() << '\n';
    return false;
  }
}

bool RestoreRemovedHostHook() {
  if (!saved_configuration) return true;
  const bool restored = NativeCommand(saved_configuration->socket.c_str(),
                                      "config-set",
                                      saved_configuration->arguments);
  if (restored) saved_configuration.reset();
  return restored;
}

/** Verifies that failed ABI calls never leave caller-owned stale outputs. */
bool CheckCallbackOutputContracts(const DangPluginV6& plugin6) {
  const DangPluginV5& plugin5 = plugin6.v5;
  const DangPluginV1& plugin = plugin5.v4.v3.v2.v1;
  DangPluginErrorV1 error{"stale error", "stale path"};

  DangYangSourceV1 source{"stale", "stale", "stale", 5, "stale",
                          DANG_YANG_IMPLEMENTED_V1, nullptr, 1};
  bool valid = !plugin.yang_source_at(plugin.context,
                                      plugin.yang_source_count(plugin.context),
                                      &source, &error) &&
      source.module_name == nullptr && source.revision == nullptr &&
      source.source == nullptr && source.source_size == 0 &&
      source.source_uri == nullptr && source.role == 0 &&
      source.enabled_features == nullptr &&
      source.enabled_feature_count == 0;

  int marker = 0;
  void* prepared = &marker;
  valid = valid && !plugin.prepare(plugin.context, nullptr, &prepared, &error) &&
      prepared == nullptr;

  DangHardwareActionV1 action{"stale", "stale", DANG_HARDWARE_ACTIVATE_V1,
                              nullptr, 1};
  valid = valid && !plugin5.v4.hardware_action_at(
      plugin.context, nullptr, 1, &action, &error) &&
      action.action_id == nullptr && action.instance_path == nullptr &&
      action.action_class == 0 && action.dependencies == nullptr &&
      action.dependency_count == 0;

  DangOperationalDataV1 legacy_state{"stale"};
  valid = valid && !plugin5.v4.v3.get_operational_data(
      plugin.context, &legacy_state, &error) &&
      legacy_state.data_xml == nullptr;

  DangOperationalDataV2 state{"stale", 1};
  valid = valid && !plugin5.get_operational_data_v2(
      plugin.context, &state, &error) && state.data_xml == nullptr &&
      state.complete == 0;

  DangAppliedConfigurationV1 applied{"stale", nullptr, 1};
  valid = valid && !plugin6.reconcile_applied_configuration(
      plugin.context, nullptr, nullptr, &applied, &error) &&
      applied.applied_xml == nullptr && applied.outcomes == nullptr &&
      applied.outcome_count == 0;

  source = {};
  error = {"stale error", "stale path"};
  valid = valid && plugin.yang_source_at(plugin.context, 0, &source, &error) &&
      source.module_name != nullptr && error.message == nullptr &&
      error.instance_path == nullptr;
  if (!valid) std::cerr << "plugin callback output contract failed\n";
  return valid;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "usage: kea_plugin_integration_test PLUGIN BEFORE PROPOSED\n";
    return 2;
  }
  const std::string before = Read(argv[2]);
  std::string proposed = Read(argv[3]);
  const bool skip_operational =
      std::getenv("DANG_KEA_SKIP_OPERATIONAL") != nullptr;
  const bool force_lease_paging =
      std::getenv("DANG_KEA_FORCE_LEASE_PAGING") != nullptr;
  const bool force_host_paging =
      std::getenv("DANG_KEA_FORCE_HOST_PAGING") != nullptr;
  const bool test_noop_seed =
      std::getenv("DANG_KEA_TEST_NOOP_SEED") != nullptr;
  // Portable contract cases have no daemon transport. They may bypass only
  // successful reconciliation; native workflows retain TEST_NOOP_SEED and
  // exercise the real control sockets, command inventory, and version read.
  const bool transport_free_seed =
      std::getenv("DANG_KEA_TEST_TRANSPORT_FREE_SEED") != nullptr;
  const bool empty_startup =
      std::getenv("DANG_KEA_EMPTY_STARTUP") != nullptr;
  const bool ha_member = std::getenv("DANG_KEA_HA_MEMBER") != nullptr;
  const char* instance_environment = std::getenv("DANG_KEA_INSTANCE_ID");
  const std::string instance_id =
      instance_environment ? instance_environment : "default";
  const char* ha_mode_environment = std::getenv("DANG_KEA_HA_MODE");
  const std::string_view ha_mode =
      ha_mode_environment ? ha_mode_environment : "hot-standby";
  const bool expect_prepared_mismatch =
      std::getenv("DANG_KEA_EXPECT_PREPARED_MISMATCH") != nullptr;
  const bool expect_unapplied_reconcile =
      std::getenv("DANG_KEA_EXPECT_UNAPPLIED_RECONCILE") != nullptr;
  const bool dhcp4_enabled = std::getenv("DANG_KEA_DHCP4_SOCKET") &&
      *std::getenv("DANG_KEA_DHCP4_SOCKET");
  const bool dhcp6_enabled = std::getenv("DANG_KEA_DHCP6_SOCKET") &&
      *std::getenv("DANG_KEA_DHCP6_SOCKET");
  bool valid = !force_host_paging || AddDhcp6PagingReservations(&proposed);
  const bool no_op = before == proposed;
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
  const char* expected_startup_failure =
      std::getenv("DANG_KEA_EXPECT_STARTUP_RECONCILE_FAILURE");
  const char* expected_startup_socket_failure =
      std::getenv("DANG_KEA_EXPECT_STARTUP_SOCKET_FAILURE");
  const char* expected_disabled_module =
      std::getenv("DANG_KEA_EXPECT_DISABLED_MODULE");
  const char* expected_inventory_error =
      std::getenv("DANG_KEA_EXPECT_INVENTORY_ERROR");
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
  valid = valid && CheckCallbackOutputContracts(*plugin6);
  if (expected_inventory_error) {
    DangTransactionV1 invalid_inventory_transaction{
        before.c_str(), proposed.c_str(), "[]"};
    DangPluginErrorV1 inventory_error{};
    void* invalid_prepared = nullptr;
    const bool prepared_invalid = valid && plugin->prepare(
        plugin->context, &invalid_inventory_transaction, &invalid_prepared,
        &inventory_error);
    valid = valid && !prepared_invalid && invalid_prepared == nullptr &&
        inventory_error.message && inventory_error.instance_path &&
        std::string_view(inventory_error.message).find(
            expected_inventory_error) != std::string_view::npos &&
        std::string_view(inventory_error.instance_path) == "/";
    if (!valid)
      Report("expected target-inventory rejection", inventory_error);
    if (plugin->destroy) plugin->destroy(plugin->context);
    dlclose(library);
    if (valid) std::cout << "Kea invalid target inventory rejection passed\n";
    return valid ? 0 : 1;
  }
  if (expected_disabled_module) {
    DangTransactionV1 disabled_transaction{
        before.c_str(), proposed.c_str(), "[]"};
    DangPluginErrorV1 disabled_error{};
    void* disabled_prepared = nullptr;
    const bool prepared_disabled = valid && plugin->prepare(
        plugin->context, &disabled_transaction, &disabled_prepared,
        &disabled_error);
    const std::string expected_path =
        "/{urn:ietf:params:xml:ns:yang:" +
        std::string(expected_disabled_module) + "}config";
    valid = valid && !prepared_disabled && disabled_prepared == nullptr &&
        disabled_error.message && disabled_error.instance_path &&
        std::string_view(disabled_error.message).find(
            "configuration is present") != std::string_view::npos &&
        std::string_view(disabled_error.message).find(
            expected_disabled_module) != std::string_view::npos &&
        std::string_view(disabled_error.instance_path) == expected_path;
    if (!valid)
      Report("expected disabled-family configuration rejection",
             disabled_error);
    if (plugin->destroy) plugin->destroy(plugin->context);
    dlclose(library);
    if (valid)
      std::cout << "Kea disabled-family configuration rejection passed\n";
    return valid ? 0 : 1;
  }
  if (valid && expected_startup_failure)
    valid = RemoveHostHook(expected_startup_failure);
  DangTransactionV1 transaction{before.c_str(), proposed.c_str(), "[]"};
  DangPluginErrorV1 error{};
  DangAppliedConfigurationV1 reconciled{};
  void* startup_prepared = nullptr;
  const bool seed_with_noop = expected_validate_failure ||
      expected_operational_failure || skip_operational || test_noop_seed ||
      transport_free_seed || expect_prepared_mismatch ||
      expect_unapplied_reconcile;
  if (valid && seed_with_noop) {
    DangTransactionV1 startup{before.c_str(), before.c_str(), "[]"};
    valid = plugin->prepare(plugin->context, &startup, &startup_prepared,
                            &error) || Report("startup preparation", error);
  }
  const bool startup_accepted = empty_startup || transport_free_seed ? valid :
      valid && plugin6->reconcile_applied_configuration(
          plugin->context, startup_prepared, before.c_str(), &reconciled,
          &error);
  if (startup_prepared)
    plugin->release(plugin->context, startup_prepared);
  if (expected_startup_failure) {
    const std::string expected_path =
        "/{urn:ietf:params:xml:ns:yang:" +
        std::string(expected_startup_failure) + "}config";
    const bool rejected = !startup_accepted && error.message &&
        error.instance_path &&
        std::string_view(error.message).find(expected_startup_failure) !=
            std::string_view::npos &&
        std::string_view(error.instance_path) == expected_path;
    const bool restored = RestoreRemovedHostHook();
    valid = valid && rejected && restored;
    if (!valid) Report("expected startup reconciliation rejection", error);
    if (plugin->destroy) plugin->destroy(plugin->context);
    dlclose(library);
    if (valid) std::cout << "Kea startup reconciliation rejection passed\n";
    return valid ? 0 : 1;
  }
  if (expected_startup_socket_failure) {
    const std::string expected_path =
        "/{urn:ietf:params:xml:ns:yang:" +
        std::string(expected_startup_socket_failure) + "}config";
    valid = valid && !startup_accepted && error.message &&
        error.instance_path &&
        std::string_view(error.message).find(
            expected_startup_socket_failure) != std::string_view::npos &&
        std::string_view(error.instance_path) == expected_path;
    if (!valid)
      Report("expected startup socket rejection", error);
    if (plugin->destroy) plugin->destroy(plugin->context);
    dlclose(library);
    if (valid) std::cout << "Kea startup socket rejection passed\n";
    return valid ? 0 : 1;
  }
  valid = valid &&
      (startup_accepted || Report("startup reconciliation", error));
  if (!empty_startup && !transport_free_seed)
    valid = valid && reconciled.applied_xml == before.c_str() &&
        reconciled.outcomes == nullptr && reconciled.outcome_count == 0;
  void* prepared = nullptr;
  if (valid)
    valid = plugin->prepare(plugin->context, &transaction, &prepared, &error)
        || Report("prepare", error);
  if (expect_prepared_mismatch) {
    DangAppliedConfigurationV1 mismatched{"stale", nullptr, 1};
    const bool accepted = valid && plugin6->reconcile_applied_configuration(
        plugin->context, prepared, before.c_str(), &mismatched, &error);
    const bool rejected = !accepted && error.message && error.instance_path &&
        std::string_view(error.message).find(
            "does not match the prepared proposal") != std::string_view::npos &&
        std::string_view(error.instance_path) ==
            "/{urn:ietf:params:xml:ns:yang:kea-dhcp4-server}config" &&
        mismatched.applied_xml == nullptr && mismatched.outcomes == nullptr &&
        mismatched.outcome_count == 0;
    valid = valid && rejected;
    if (!valid) Report("expected prepared reconciliation mismatch", error);
    if (prepared) plugin->release(plugin->context, prepared);
    if (plugin->destroy) plugin->destroy(plugin->context);
    dlclose(library);
    if (valid)
      std::cout << "Kea prepared reconciliation mismatch rejection passed\n";
    return valid ? 0 : 1;
  }
  if (expect_unapplied_reconcile) {
    DangAppliedConfigurationV1 unapplied{"stale", nullptr, 1};
    const bool accepted = valid && plugin6->reconcile_applied_configuration(
        plugin->context, prepared, proposed.c_str(), &unapplied, &error);
    const bool rejected = !accepted && error.message && error.instance_path &&
        std::string_view(error.message).find("has not been applied") !=
            std::string_view::npos &&
        std::string_view(error.instance_path) ==
            "/{urn:ietf:params:xml:ns:yang:kea-dhcp4-server}config" &&
        unapplied.applied_xml == nullptr && unapplied.outcomes == nullptr &&
        unapplied.outcome_count == 0;
    valid = valid && rejected;
    if (!valid) Report("expected unapplied reconciliation rejection", error);
    if (prepared) plugin->release(plugin->context, prepared);
    if (plugin->destroy) plugin->destroy(plugin->context);
    dlclose(library);
    if (valid)
      std::cout << "Kea unapplied reconciliation rejection passed\n";
    return valid ? 0 : 1;
  }
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
  if (valid && expected_apply_failure) {
    DangOperationalDataV2 state{"stale", 1};
    const bool retrieved =
        plugin5->get_operational_data_v2(plugin->context, &state, &error);
    const std::string expected_path =
        "/{urn:ietf:params:xml:ns:yang:" +
        std::string(expected_apply_failure) + "}config";
    valid = !retrieved && state.data_xml == nullptr && state.complete == 0 &&
        error.message && error.instance_path &&
        std::string_view(error.message).find("awaiting dangd reconciliation") !=
            std::string_view::npos &&
        std::string_view(error.instance_path) == expected_path;
    if (!valid) Report("incompletely compensated operational suppression",
                       error);
    DangPluginErrorV1 retry_error{};
    const bool retried = valid && plugin5->v4.apply_hardware_action(
        plugin->context, prepared, action.action_id, &retry_error);
    valid = valid && !retried && retry_error.message &&
        retry_error.instance_path &&
        std::string_view(retry_error.message).find(
            "earlier Kea mutation is still unresolved") !=
            std::string_view::npos &&
        std::string_view(retry_error.instance_path) == expected_path;
    if (!valid) Report("unresolved mutation retry rejection", retry_error);
    DangTransactionV1 no_op_transaction{before.c_str(), before.c_str(), "[]"};
    void* no_op_prepared = nullptr;
    DangPluginErrorV1 no_op_error{};
    if (valid)
      valid = plugin->prepare(plugin->context, &no_op_transaction,
                              &no_op_prepared, &no_op_error) ||
          Report("pending no-op preparation", no_op_error);
    DangAppliedConfigurationV1 no_op_applied{"stale", nullptr, 1};
    const bool no_op_reconciled = valid &&
        plugin6->reconcile_applied_configuration(
            plugin->context, no_op_prepared, before.c_str(), &no_op_applied,
            &no_op_error);
    valid = valid && !no_op_reconciled && no_op_error.message &&
        no_op_error.instance_path &&
        std::string_view(no_op_error.message).find(
            "does not own the pending mutation") != std::string_view::npos &&
        std::string_view(no_op_error.instance_path) == expected_path &&
        no_op_applied.applied_xml == nullptr &&
        no_op_applied.outcomes == nullptr && no_op_applied.outcome_count == 0;
    if (!valid) Report("pending no-op reconciliation rejection", no_op_error);
    no_op_error = {};
    const bool no_op_rolled_back = valid &&
        plugin5->v4.rollback_hardware_action(
            plugin->context, no_op_prepared, "configuration", &no_op_error);
    valid = valid && !no_op_rolled_back && no_op_error.message &&
        no_op_error.instance_path &&
        std::string_view(no_op_error.message).find(
            "does not own the unresolved Kea mutation") !=
            std::string_view::npos &&
        std::string_view(no_op_error.instance_path) == expected_path;
    if (!valid) Report("pending no-op rollback rejection", no_op_error);
    if (no_op_prepared)
      plugin->release(plugin->context, no_op_prepared);
  }
  if (valid && !expected_validate_failure && !expected_apply_failure &&
      !no_op && !skip_operational) {
    // Hardware has the proposed image, but dangd has not accepted readback.
    // Operational publication must continue to use the prior accepted image
    // and therefore fail closed on the temporary configuration mismatch.
    DangOperationalDataV2 state{"stale", 1};
    const bool retrieved =
        plugin5->get_operational_data_v2(plugin->context, &state, &error);
    valid = !retrieved && state.data_xml == nullptr && state.complete == 0 &&
        error.message && error.instance_path &&
        std::string_view(error.message).find("awaiting dangd reconciliation") !=
            std::string_view::npos &&
        std::string_view(error.instance_path).ends_with("}config");
    if (!valid) Report("uncommitted operational suppression", error);
  }
  if (valid && expected_reconcile_failure)
    valid = RemoveHostHook(expected_reconcile_failure);
  if (valid && !expected_validate_failure && !expected_apply_failure) {
    DangAppliedConfigurationV1 applied{};
    const bool reconciliation_accepted = transport_free_seed
        ? true
        : plugin6->reconcile_applied_configuration(
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
      valid = valid &&
          (transport_free_seed ||
           (applied.applied_xml == proposed.c_str() &&
            applied.outcomes == nullptr && applied.outcome_count == 0));
    }
  }
  if (valid && ha_member && dhcp4_enabled)
    valid = NativeCommand(std::getenv("DANG_KEA_DHCP4_SOCKET"),
                          "ha-heartbeat",
                          {{"server-name", "local-primary"}});
  if (valid && ha_member && dhcp6_enabled)
    valid = NativeCommand(std::getenv("DANG_KEA_DHCP6_SOCKET"),
                          "ha-heartbeat",
                          {{"server-name", "local-primary"}});
  if (valid && ha_member) {
    DangOperationalDataV2 state{};
    valid = plugin5->get_operational_data_v2(plugin->context, &state, &error) ||
            Report("HA operational", error);
    const std::string xml = valid && state.data_xml ? state.data_xml : "";
    valid = valid && state.complete == 1 &&
            xml.find("<kea-instance xmlns=\"urn:dang:kea:instance\">"
                     "<instance-id>" + instance_id + "</instance-id>") !=
                std::string::npos &&
            xml.find("<high-availability xmlns=\"urn:dang:kea:ha\">") !=
                std::string::npos &&
            (!dhcp4_enabled ||
             xml.find("<address-family>dhcpv4</address-family>") !=
                 std::string::npos) &&
            (!dhcp6_enabled ||
             xml.find("<address-family>dhcpv6</address-family>") !=
                 std::string::npos) &&
            (!dhcp4_enabled ||
             xml.find("<daemon><address-family>dhcpv4</address-family>"
                      "<version>") != std::string::npos) &&
            (!dhcp6_enabled ||
             xml.find("<daemon><address-family>dhcpv6</address-family>"
                      "<version>") != std::string::npos) &&
            xml.find("<mode>" + std::string(ha_mode) + "</mode>") !=
                std::string::npos &&
            xml.find("<server-name>local-primary</server-name>") !=
                std::string::npos &&
            xml.find(ha_mode == "passive-backup"
                         ? "<state>passive-backup</state>"
                         : "<state>waiting</state>") != std::string::npos &&
            (ha_mode == "passive-backup"
                 ? xml.find("<remote>") == std::string::npos
                 : xml.find("<server-name>remote-standby</server-name>") !=
                           std::string::npos &&
                       xml.find("<in-touch>false</in-touch>") !=
                           std::string::npos &&
                       xml.find("<age>") != std::string::npos &&
                       xml.find("<analyzed-packets>") != std::string::npos &&
                       xml.find("<connecting-clients>") != std::string::npos &&
                       xml.find("<unacked-clients>") != std::string::npos &&
                       xml.find("<unacked-clients-left>") !=
                           std::string::npos);
    if (!valid)
      std::cerr << "HA operational XML is incomplete: " << xml << '\n';
  }
  if (valid && !expected_validate_failure && expected_rollback_failure) {
    const char* socket = SocketForModule(expected_rollback_failure);
    valid = socket && *socket && ::unlink(socket) == 0;
    if (!valid) std::cerr << "cannot remove expected rollback socket\n";
  }
  if (valid && !expected_validate_failure && !expected_apply_failure &&
      !no_op && !skip_operational && dhcp4_enabled)
    valid = NativeCommand(std::getenv("DANG_KEA_DHCP4_SOCKET"), "lease4-add",
                          {{"subnet-id", 401},
                           {"ip-address", "192.0.2.80"},
                           {"hw-address", "02:00:00:00:04:01"}});
  if (valid && !expected_validate_failure && !expected_apply_failure &&
      !no_op && !skip_operational && dhcp6_enabled)
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
        xml.find("<kea-instance xmlns=\"urn:dang:kea:instance\">"
                 "<instance-id>" + instance_id + "</instance-id>") !=
            std::string::npos &&
        (dhcp4_enabled ==
         (xml.find("<address-family>dhcpv4</address-family>") !=
          std::string::npos)) &&
        (dhcp6_enabled ==
         (xml.find("<address-family>dhcpv6</address-family>") !=
          std::string::npos)) &&
        (dhcp4_enabled ==
         (xml.find("<daemon><address-family>dhcpv4</address-family>"
                   "<version>") != std::string::npos)) &&
        (dhcp6_enabled ==
         (xml.find("<daemon><address-family>dhcpv6</address-family>"
                   "<version>") != std::string::npos)) &&
        (dhcp4_enabled ==
         (xml.find("urn:ietf:params:xml:ns:yang:kea-dhcp4-server") !=
          std::string::npos)) &&
        (dhcp6_enabled ==
         (xml.find("urn:ietf:params:xml:ns:yang:kea-dhcp6-server") !=
          std::string::npos)) &&
        xml.find("<leases") != std::string::npos &&
        (!dhcp4_enabled ||
         (xml.find("<ip-address>192.0.2.80</ip-address>") !=
              std::string::npos &&
          xml.find("<hw-address>AgAAAAQB</hw-address>") !=
              std::string::npos)) &&
        (!dhcp6_enabled ||
         (xml.find("<ip-address>2001:db8:6::180</ip-address>") !=
              std::string::npos &&
          xml.find("<duid>AAEAAQIDBAUGBwgJ</duid>") !=
              std::string::npos &&
          xml.find("<iaid>1234</iaid>") != std::string::npos)) &&
        (!force_lease_paging ||
         (xml.find("<ip-address>2001:db8:6::10ff</ip-address>") !=
              std::string::npos &&
          xml.find("<iaid>2255</iaid>") != std::string::npos)) &&
        xml.find("<lease-stats") != std::string::npos &&
        (!dhcp4_enabled ||
         xml.find("<assigned-addresses>1</assigned-addresses>") !=
             std::string::npos) &&
        (!dhcp6_enabled ||
         xml.find(force_lease_paging ? "<assigned-nas>257</assigned-nas>"
                                     : "<assigned-nas>1</assigned-nas>") !=
             std::string::npos) &&
        xml.find("<hosts") != std::string::npos &&
        (!dhcp4_enabled ||
         (xml.find("<subnet-id>401</subnet-id>") != std::string::npos &&
          xml.find("<identifier>00:01:02:03:04:05</identifier>") !=
              std::string::npos)) &&
        (!dhcp6_enabled ||
         (xml.find("<subnet-id>601</subnet-id>") != std::string::npos &&
          xml.find("<identifier>00:01:02:03</identifier>") !=
              std::string::npos)) &&
        (!force_host_paging ||
         (xml.find("<identifier>00:01:00:01:02:03:04:05:06:07:08:ff"
                   "</identifier>") != std::string::npos &&
          xml.find("<hostname>paging-host-255</hostname>") !=
              std::string::npos)) &&
        (!dhcp4_enabled ||
         (xml.find("<space>dhcp4</space>") != std::string::npos &&
          xml.find("<data>printer.example</data>") !=
              std::string::npos)) &&
        (!dhcp6_enabled ||
         (xml.find("<space>dhcp6</space>") != std::string::npos &&
          xml.find("<data>2001:db8:6::53</data>") !=
              std::string::npos));
    if (!valid) std::cerr << "operational XML is incomplete: " << xml << '\n';
  }
  if (valid && remove_host_hook)
    valid = RemoveHostHook(remove_host_hook);
  if (valid && expected_operational_failure) {
    DangOperationalDataV2 state{};
    const bool retrieved =
        plugin5->get_operational_data_v2(plugin->context, &state, &error);
    const std::string subtree = expected_operational_subtree
        ? expected_operational_subtree : "leases";
    const std::string expected_path =
        "/{urn:ietf:params:xml:ns:yang:" +
        std::string(expected_operational_failure) + "}" +
        (subtree == "config" ? "config" : "state/" + subtree);
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
  if (valid && expected_rollback_failure) {
    DangOperationalDataV2 state{"stale", 1};
    const bool retrieved =
        plugin5->get_operational_data_v2(plugin->context, &state, &error);
    valid = !retrieved && state.data_xml == nullptr && state.complete == 0 &&
        error.message && error.instance_path &&
        std::string_view(error.message).find("awaiting dangd reconciliation") !=
            std::string_view::npos;
    if (!valid) Report("incomplete rollback operational suppression", error);
  }
  if (valid && expected_reconcile_failure) {
    DangOperationalDataV2 state{};
    valid = plugin5->get_operational_data_v2(plugin->context, &state, &error) &&
        state.data_xml != nullptr && state.complete == 1;
    if (!valid) Report("post-rollback operational restoration", error);
  }
  if (remove_host_hook) {
    const bool restored = RestoreRemovedHostHook();
    valid = valid && restored;
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
        : empty_startup
        ? "Kea empty-datastore startup apply and rollback passed\n"
        : no_op
        ? "Kea no-op validate, apply, and rollback passed\n"
        : ha_member
        ? "Kea local HA member validate, apply, reconcile, and rollback passed\n"
        : skip_operational
            ? "Kea selective validate, apply, and rollback passed\n"
        : dhcp4_enabled && dhcp6_enabled
            ? "Kea DHCPv4 and DHCPv6 validate, apply, and rollback passed\n"
        : dhcp4_enabled
            ? "Kea DHCPv4-only validate, apply, and rollback passed\n"
            : "Kea DHCPv6-only validate, apply, and rollback passed\n");
  return valid ? 0 : 1;
}

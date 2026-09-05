// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/**
 * @file
 * dangd transaction adapter for the Kea DHCPv4 and DHCPv6 services.  Validate
 * uses Kea's config-test command; apply uses config-set; rollback reapplies the
 * retained before-image to every service that was changed successfully.
 */

#include "dangd/plugin_api.h"

#include "kea_adapter.h"
#include "kea_model_sources.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using dang::plugins::kea::CommandSucceeded;
using dang::plugins::kea::CollectHostPages;
using dang::plugins::kea::CollectLeasePages;
using dang::plugins::kea::CollectStatistics;
using dang::plugins::kea::ExtractSubnetIds;
using dang::plugins::kea::SendControlCommand;
using dang::plugins::kea::SendControlQuery;
using dang::plugins::kea::ServerConfiguration;
using dang::plugins::kea::TranslateConfiguration;
using dang::plugins::kea::TranslateOperationalState;

struct Prepared {
  // Vector order is fixed as DHCPv4 then DHCPv6 and is shared by both images;
  // this makes index-based compensation unambiguous after a partial apply.
  std::vector<ServerConfiguration> before;
  std::vector<ServerConfiguration> proposed;
};

thread_local std::string callback_error;
thread_local std::string callback_path;
thread_local std::string operational_xml;

// Statistics must describe only configuration that actually reached Kea.
// Preparing or validating a candidate therefore cannot alter this inventory;
// successful apply and rollback callbacks are its only writers.
std::mutex subnet_inventory_mutex;
std::array<std::vector<std::uint32_t>, 2> accepted_subnet_ids;

void RememberAcceptedSubnets(
    const std::vector<ServerConfiguration>& configurations) {
  std::array<std::vector<std::uint32_t>, 2> next;
  for (std::size_t index = 0; index < configurations.size() && index < next.size();
       ++index)
    next[index] = ExtractSubnetIds(configurations[index]);
  std::lock_guard lock(subnet_inventory_mutex);
  accepted_subnet_ids = std::move(next);
}

std::array<std::vector<std::uint32_t>, 2> AcceptedSubnetSnapshot() {
  std::lock_guard lock(subnet_inventory_mutex);
  return accepted_subnet_ids;
}

void SetError(DangPluginErrorV1* error, std::string message,
              std::string path = {}) {
  if (!error) return;
  callback_error = std::move(message);
  callback_path = std::move(path);
  error->message = callback_error.c_str();
  error->instance_path = callback_path.empty() ? nullptr : callback_path.c_str();
}

size_t SourceCount(void*) { return 4; }

int SourceAt(void*, size_t index, DangYangSourceV1* source,
             DangPluginErrorV1* error) {
  if (!source) {
    SetError(error, "the YANG source output is missing");
    return 0;
  }
  static const DangYangSourceV1 sources[]{
      {"kea-types", "2025-06-25", kKeaTypesYang,
       std::strlen(kKeaTypesYang),
       "https://gitlab.isc.org/isc-projects/kea/-/blob/Kea-3.2.0/"
       "src/share/yang/modules/kea-types%402025-06-25.yang",
       DANG_YANG_IMPORT_ONLY_V1, nullptr, 0},
      {"kea-dhcp-types", "2026-06-24", kKeaDhcpTypesYang,
       std::strlen(kKeaDhcpTypesYang),
       "https://gitlab.isc.org/isc-projects/kea/-/blob/Kea-3.2.0/"
       "src/share/yang/modules/kea-dhcp-types%402026-06-24.yang",
       DANG_YANG_IMPORT_ONLY_V1, nullptr, 0},
      {"kea-dhcp4-server", "2026-06-24", kKeaDhcp4Yang,
       std::strlen(kKeaDhcp4Yang),
       "https://gitlab.isc.org/isc-projects/kea/-/blob/Kea-3.2.0/"
       "src/share/yang/modules/kea-dhcp4-server%402026-06-24.yang",
       DANG_YANG_IMPLEMENTED_V1, nullptr, 0},
      {"kea-dhcp6-server", "2026-06-24", kKeaDhcp6Yang,
       std::strlen(kKeaDhcp6Yang),
       "https://gitlab.isc.org/isc-projects/kea/-/blob/Kea-3.2.0/"
       "src/share/yang/modules/kea-dhcp6-server%402026-06-24.yang",
       DANG_YANG_IMPLEMENTED_V1, nullptr, 0}};
  if (index >= std::size(sources)) {
    SetError(error, "the YANG source index is out of range");
    return 0;
  }
  *source = sources[index];
  return 1;
}

size_t DependencyCount(void*) { return 0; }

const char* DependencyAt(void*, size_t) { return nullptr; }

std::optional<std::vector<ServerConfiguration>> TranslateBoth(
    const char* xml, DangPluginErrorV1* error) {
  if (!xml) {
    SetError(error, "the configuration snapshot is missing");
    return std::nullopt;
  }
  const char* socket4 = std::getenv("DANG_KEA_DHCP4_SOCKET");
  const char* socket6 = std::getenv("DANG_KEA_DHCP6_SOCKET");
  if (!socket4 || !*socket4 || !socket6 || !*socket6) {
    SetError(error,
             "DANG_KEA_DHCP4_SOCKET and DANG_KEA_DHCP6_SOCKET must name "
             "local Kea UNIX control sockets");
    return std::nullopt;
  }
  std::vector<ServerConfiguration> configurations;
  for (const auto& [module, socket] :
       {std::pair{"kea-dhcp4-server", socket4},
        std::pair{"kea-dhcp6-server", socket6}}) {
    std::string reason;
    auto translated = TranslateConfiguration(xml, module, socket, &reason);
    if (!translated) {
      SetError(error, module + std::string(": ") + reason,
               "/{" + std::string("urn:ietf:params:xml:ns:yang:") +
                   module + "}config");
      return std::nullopt;
    }
    configurations.push_back(std::move(*translated));
  }
  return configurations;
}

int PrepareConfiguration(void*, const DangTransactionV1* transaction,
                         void** result, DangPluginErrorV1* error) {
  if (!transaction || !result) {
    SetError(error, "the transaction input is incomplete");
    return 0;
  }
  auto before = TranslateBoth(transaction->before_xml, error);
  if (!before) return 0;
  auto proposed = TranslateBoth(transaction->proposed_xml, error);
  if (!proposed) return 0;
  auto* prepared = new (std::nothrow)
      Prepared{std::move(*before), std::move(*proposed)};
  if (!prepared) {
    SetError(error, "cannot retain the Kea transaction plan");
    return 0;
  }
  *result = prepared;
  return 1;
}

bool Execute(const ServerConfiguration& server, std::string_view command,
             std::string* reason) {
  auto response = SendControlCommand(server, command, reason);
  return response && CommandSucceeded(*response, reason);
}

int ValidateConfiguration(void*, void* opaque, DangPluginErrorV1* error) {
  const auto* prepared = static_cast<Prepared*>(opaque);
  if (!prepared) {
    SetError(error, "the prepared Kea transaction is missing");
    return 0;
  }
  for (const ServerConfiguration& server : prepared->proposed) {
    std::string reason;
    if (Execute(server, "config-test", &reason)) continue;
    SetError(error, server.module_name + ": " + reason,
             "/{urn:ietf:params:xml:ns:yang:" + server.module_name +
                 "}config");
    return 0;
  }
  return 1;
}

int ApplyConfiguration(void*, void* opaque, DangPluginErrorV1* error) {
  const auto* prepared = static_cast<Prepared*>(opaque);
  if (!prepared) {
    SetError(error, "the prepared Kea transaction is missing");
    return 0;
  }
  std::size_t completed = 0;
  for (; completed < prepared->proposed.size(); ++completed) {
    const ServerConfiguration& server = prepared->proposed[completed];
    std::string reason;
    if (Execute(server, "config-set", &reason)) continue;
    // Restore only services already changed in this callback.  The service
    // whose config-set failed is assumed not to have accepted the new image.
    std::string compensation;
    while (completed > 0) {
      --completed;
      std::string rollback_reason;
      if (!Execute(prepared->before[completed], "config-set", &rollback_reason))
        compensation += "; rollback of " +
            prepared->before[completed].module_name + " failed: " +
            rollback_reason;
    }
    SetError(error, server.module_name + ": " + reason + compensation,
             "/{urn:ietf:params:xml:ns:yang:" + server.module_name +
                 "}config");
    return 0;
  }
  RememberAcceptedSubnets(prepared->proposed);
  return 1;
}

int RollbackConfiguration(void*, void* opaque, DangPluginErrorV1* error) {
  const auto* prepared = static_cast<Prepared*>(opaque);
  if (!prepared) {
    SetError(error, "the prepared Kea transaction is missing");
    return 0;
  }
  std::string failures;
  for (auto server = prepared->before.rbegin(); server != prepared->before.rend();
       ++server) {
    std::string reason;
    if (!Execute(*server, "config-set", &reason))
      failures += (failures.empty() ? "" : "; ") + server->module_name +
          ": " + reason;
  }
  if (failures.empty()) {
    RememberAcceptedSubnets(prepared->before);
    return 1;
  }
  SetError(error, failures);
  return 0;
}

void Release(void*, void* opaque) { delete static_cast<Prepared*>(opaque); }

size_t HardwareActionCount(void*, void* opaque) {
  return opaque ? 1 : 0;
}

int HardwareActionAt(void*, void* opaque, size_t index,
                     DangHardwareActionV1* action,
                     DangPluginErrorV1* error) {
  if (!opaque || !action || index != 0) {
    SetError(error, "the Kea transaction action is unavailable");
    return 0;
  }
  // Kea accepts each daemon's complete configuration as one config-set.
  // Advertising one normal action preserves that indivisible unit instead of
  // pretending individual YANG leaves can be safely reordered by dangd.
  *action = {"configuration", "/", DANG_HARDWARE_NORMAL_V1, nullptr, 0};
  return 1;
}

int ApplyHardwareAction(void* context, void* opaque, const char* action_id,
                        DangPluginErrorV1* error) {
  if (!action_id || std::string_view(action_id) != "configuration") {
    SetError(error, "the Kea hardware action ID is unknown");
    return 0;
  }
  return ApplyConfiguration(context, opaque, error);
}

int RollbackHardwareAction(void* context, void* opaque, const char* action_id,
                           DangPluginErrorV1* error) {
  if (!action_id || std::string_view(action_id) != "configuration") {
    SetError(error, "the Kea hardware action ID is unknown");
    return 0;
  }
  return RollbackConfiguration(context, opaque, error);
}

int Operational(void*, DangOperationalDataV1* result,
                DangPluginErrorV1* error) {
  if (!result) {
    SetError(error, "the operational data output is missing");
    return 0;
  }
  const char* socket4 = std::getenv("DANG_KEA_DHCP4_SOCKET");
  const char* socket6 = std::getenv("DANG_KEA_DHCP6_SOCKET");
  if (!socket4 || !*socket4 || !socket6 || !*socket6) {
    SetError(error, "Kea operational sockets are not configured", "/");
    return 0;
  }
  operational_xml =
      "<data xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">";
  const auto subnet_ids = AcceptedSubnetSnapshot();
  std::size_t server_index = 0;
  for (const auto& [module, socket, dhcp6] : {
           std::tuple{"kea-dhcp4-server", socket4, false},
           std::tuple{"kea-dhcp6-server", socket6, true}}) {
    std::string reason;
    auto leases = CollectLeasePages(socket, dhcp6, SendControlQuery, &reason);
    if (!leases) {
      SetError(error, module + std::string(": ") + reason,
               "/{" + std::string("urn:ietf:params:xml:ns:yang:") + module +
                   "}state/leases");
      return 0;
    }
    auto statistics = CollectStatistics(socket, dhcp6,
                                        subnet_ids[server_index],
                                        SendControlQuery, &reason);
    if (!statistics) {
      SetError(error, module + std::string(": ") + reason,
               "/{" + std::string("urn:ietf:params:xml:ns:yang:") + module +
                   "}state/lease-stats");
      return 0;
    }
    auto hosts = CollectHostPages(socket, SendControlQuery, &reason);
    if (!hosts) {
      SetError(error, module + std::string(": ") + reason,
               "/{" + std::string("urn:ietf:params:xml:ns:yang:") + module +
                   "}state/hosts");
      return 0;
    }
    auto state = TranslateOperationalState(module, *leases, *statistics,
                                           *hosts, &reason);
    if (!state) {
      SetError(error, module + std::string(": ") + reason,
               "/{" + std::string("urn:ietf:params:xml:ns:yang:") + module +
                   "}state");
      return 0;
    }
    operational_xml += *state;
    ++server_index;
  }
  operational_xml += "</data>";
  result->data_xml = operational_xml.c_str();
  return 1;
}

int OperationalV2(void* context, DangOperationalDataV2* result,
                  DangPluginErrorV1* error) {
  if (!result) {
    SetError(error, "the operational data output is missing");
    return 0;
  }
  DangOperationalDataV1 legacy{};
  if (!Operational(context, &legacy, error)) return 0;
  *result = {legacy.data_xml, 1};
  return 1;
}

const DangPluginV5 kPlugin{
    .v4 = {.v3 = {.v2 = {.v1 = {.abi_version = DANG_PLUGIN_ABI_V5,
                  .plugin_name = "dang-kea",
                  .context = nullptr,
                  .yang_source_count = SourceCount,
                  .yang_source_at = SourceAt,
                  .dependency_count = DependencyCount,
                  .dependency_at = DependencyAt,
                  .prepare = PrepareConfiguration,
                  .validate = ValidateConfiguration,
                  .apply = ApplyConfiguration,
                  .rollback = RollbackConfiguration,
                  .release = Release,
                  .destroy = nullptr},
           .invoke = nullptr},
           .get_operational_data = Operational},
           .hardware_action_count = HardwareActionCount,
           .hardware_action_at = HardwareActionAt,
           .apply_hardware_action = ApplyHardwareAction,
           .rollback_hardware_action = RollbackHardwareAction},
    .get_operational_data_v2 = OperationalV2};

}  // namespace

extern "C" const DangPluginV5* dang_plugin_init_v5() { return &kPlugin; }
extern "C" const DangPluginV3* dang_plugin_init_v3() {
  return &kPlugin.v4.v3;
}
extern "C" const DangPluginV1* dang_plugin_init_v1() {
  return &kPlugin.v4.v3.v2.v1;
}

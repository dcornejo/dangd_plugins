// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/plugin_api.h"

#include "kea_adapter.h"
#include "kea_model_sources.h"

#include <cstdlib>
#include <cstring>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using dang::plugins::kea::CommandSucceeded;
using dang::plugins::kea::SendControlCommand;
using dang::plugins::kea::ServerConfiguration;
using dang::plugins::kea::TranslateConfiguration;

struct Prepared {
  std::vector<ServerConfiguration> before;
  std::vector<ServerConfiguration> proposed;
};

thread_local std::string callback_error;
thread_local std::string callback_path;

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
  if (failures.empty()) return 1;
  SetError(error, failures);
  return 0;
}

void Release(void*, void* opaque) { delete static_cast<Prepared*>(opaque); }

const DangPluginV1 kPlugin{
    DANG_PLUGIN_ABI_V1,
    "dang-kea",
    nullptr,
    SourceCount,
    SourceAt,
    DependencyCount,
    DependencyAt,
    PrepareConfiguration,
    ValidateConfiguration,
    ApplyConfiguration,
    RollbackConfiguration,
    Release,
    nullptr};

}  // namespace

extern "C" const DangPluginV1* dang_plugin_init_v1() { return &kPlugin; }

// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/**
 * @file
 * dangd transaction adapter for the Kea DHCPv4 and DHCPv6 services.  Validate
 * uses Kea's config-test command; apply uses config-set; rollback reapplies and
 * reads back the retained before-image for every changed service.
 */

#include "dangd/plugin_api.h"

#include "kea_adapter.h"
#include "kea_callback_guard.h"
#include "kea_model_sources.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <optional>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

using dang::plugins::kea::ApplyWithCompensation;
using dang::plugins::kea::CollectAuthoritativeOperationalState;
using dang::plugins::kea::CommandSucceeded;
using dang::plugins::kea::ExtractSubnetIds;
using dang::plugins::kea::GuardPluginCallback;
using dang::plugins::kea::PageLimits;
using dang::plugins::kea::ReadLiveConfiguration;
using dang::plugins::kea::RollbackChanged;
using dang::plugins::kea::SendControlCommand;
using dang::plugins::kea::SendControlQuery;
using dang::plugins::kea::ServerConfiguration;
using dang::plugins::kea::TranslateConfiguration;
using dang::plugins::kea::VerifyLiveConfiguration;
using dang::plugins::kea::VerifyRestoredConfigurations;

struct Prepared {
  // Vector order is fixed as DHCPv4 then DHCPv6 and is shared by both images;
  // this makes index-based compensation unambiguous after a partial apply.
  std::vector<ServerConfiguration> before;
  std::vector<ServerConfiguration> proposed;
};

thread_local std::string callback_error;
thread_local std::string callback_path;
thread_local std::string operational_xml;
thread_local std::array<char, 1024> unexpected_callback_error;

// Match dangd's default XML document ceiling before returning a provider
// buffer. The remaining allowance is shared by the DHCPv4 and DHCPv6 trees.
constexpr std::size_t kMaximumOperationalXmlBytes = 16U * 1024U * 1024U;
constexpr std::string_view kOperationalClose = "</data>";

// Operational state must describe only configuration accepted by dangd and
// known to have reached Kea. Preparing or validating a candidate therefore
// cannot alter this state. Apply also leaves it unchanged until post-apply
// reconciliation accepts readback; reconciliation and rollback are its only
// writers.
struct AcceptedConfigurationSnapshot {
  std::array<std::vector<std::uint32_t>, 2> subnet_ids;
  std::vector<ServerConfiguration> configurations;
  // Nonempty from the start of a changing hardware apply until verified
  // reconciliation or complete compensation. Operational reads fail closed
  // while the backend is not yet known to match accepted dangd state.
  std::string pending_module;
  // Exact proposal that owns pending_module. Reconciliation cannot use a
  // different or no-op prepared transaction to clear unresolved hardware.
  std::vector<ServerConfiguration> pending_proposal;
};

std::shared_mutex accepted_state_mutex;
AcceptedConfigurationSnapshot accepted_state;

void RememberAcceptedState(
    const std::vector<ServerConfiguration>& configurations) {
  // Build the complete replacement before taking the lock. If allocation
  // fails, readers retain the previous internally consistent snapshot.
  AcceptedConfigurationSnapshot next;
  next.configurations = configurations;
  for (std::size_t index = 0;
       index < configurations.size() && index < next.subnet_ids.size(); ++index)
    next.subnet_ids[index] = ExtractSubnetIds(configurations[index]);
  std::lock_guard lock(accepted_state_mutex);
  accepted_state = std::move(next);
}

bool SameConfigurationSet(const std::vector<ServerConfiguration>& left,
                          const std::vector<ServerConfiguration>& right) {
  if (left.size() != right.size()) return false;
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (left[index].module_name != right[index].module_name ||
        left[index].service_name != right[index].service_name ||
        left[index].socket_path != right[index].socket_path ||
        left[index].arguments != right[index].arguments)
      return false;
  }
  return true;
}

std::string ConfigurationMismatchModule(
    const std::vector<ServerConfiguration>& left,
    const std::vector<ServerConfiguration>& right) {
  const std::size_t shared = std::min(left.size(), right.size());
  for (std::size_t index = 0; index < shared; ++index) {
    if (left[index].module_name != right[index].module_name ||
        left[index].service_name != right[index].service_name ||
        left[index].socket_path != right[index].socket_path ||
        left[index].arguments != right[index].arguments)
      return right[index].module_name.empty() ? left[index].module_name
                                              : right[index].module_name;
  }
  if (left.size() > shared) return left[shared].module_name;
  if (right.size() > shared) return right[shared].module_name;
  return {};
}

bool BeginPendingApply(const Prepared& prepared, bool* pending,
                       std::string* failed_module, std::string* reason) {
  if (pending) *pending = false;
  std::string module;
  for (std::size_t index = 0;
       index < prepared.before.size() && index < prepared.proposed.size();
       ++index) {
    if (prepared.before[index].arguments != prepared.proposed[index].arguments) {
      module = prepared.proposed[index].module_name;
      break;
    }
  }
  std::lock_guard lock(accepted_state_mutex);
  if (!accepted_state.pending_module.empty()) {
    if (failed_module) *failed_module = accepted_state.pending_module;
    if (reason)
      *reason = accepted_state.pending_module +
          ": an earlier Kea mutation is still unresolved";
    return false;
  }
  if (!accepted_state.configurations.empty() &&
      !SameConfigurationSet(accepted_state.configurations, prepared.before)) {
    const std::string mismatch = ConfigurationMismatchModule(
        accepted_state.configurations, prepared.before);
    if (failed_module) *failed_module = mismatch;
    if (reason)
      *reason = (mismatch.empty() ? std::string("Kea") : mismatch) +
          ": prepared before-image is not the accepted configuration";
    return false;
  }
  if (module.empty()) return true;
  accepted_state.pending_module = std::move(module);
  accepted_state.pending_proposal = prepared.proposed;
  if (pending) *pending = true;
  return true;
}

void ClearPendingApply() {
  std::lock_guard lock(accepted_state_mutex);
  accepted_state.pending_module.clear();
  accepted_state.pending_proposal.clear();
}

bool BeginPendingRollback(const Prepared& prepared, std::string* failed_module,
                          std::string* reason) {
  std::string module;
  for (std::size_t index = 0;
       index < prepared.before.size() && index < prepared.proposed.size();
       ++index) {
    if (prepared.before[index].arguments != prepared.proposed[index].arguments) {
      module = prepared.before[index].module_name;
      break;
    }
  }
  std::lock_guard lock(accepted_state_mutex);
  if (!accepted_state.pending_module.empty() &&
      !SameConfigurationSet(accepted_state.pending_proposal,
                            prepared.proposed) &&
      !SameConfigurationSet(accepted_state.pending_proposal,
                            prepared.before)) {
    if (failed_module) *failed_module = accepted_state.pending_module;
    if (reason)
      *reason = accepted_state.pending_module +
          ": rollback does not own the unresolved Kea mutation";
    return false;
  }
  if (accepted_state.pending_module.empty() &&
      !accepted_state.configurations.empty() &&
      !SameConfigurationSet(accepted_state.configurations, prepared.before) &&
      !SameConfigurationSet(accepted_state.configurations,
                            prepared.proposed)) {
    const std::string mismatch = ConfigurationMismatchModule(
        accepted_state.configurations, prepared.before);
    if (failed_module) *failed_module = mismatch;
    if (reason)
      *reason = (mismatch.empty() ? std::string("Kea") : mismatch) +
          ": rollback transaction is stale";
    return false;
  }
  if (module.empty()) {
    if (!accepted_state.pending_module.empty()) {
      if (failed_module) *failed_module = accepted_state.pending_module;
      if (reason)
        *reason = accepted_state.pending_module +
            ": no-op rollback cannot clear an unresolved Kea mutation";
      return false;
    }
    return true;
  }
  accepted_state.pending_module = std::move(module);
  accepted_state.pending_proposal = prepared.before;
  return true;
}

void SetError(DangPluginErrorV1* error, std::string message,
              std::string path = {}) {
  if (!error) return;
  callback_error = std::move(message);
  callback_path = std::move(path);
  error->message = callback_error.c_str();
  error->instance_path = callback_path.empty() ? nullptr : callback_path.c_str();
}

void SetUnexpectedError(DangPluginErrorV1* error, std::string_view callback,
                        std::string_view detail) noexcept {
  if (!error) return;
  std::snprintf(unexpected_callback_error.data(),
                unexpected_callback_error.size(),
                "%.*s callback threw: %.*s",
                static_cast<int>(callback.size()), callback.data(),
                static_cast<int>(detail.size()), detail.data());
  error->message = unexpected_callback_error.data();
  error->instance_path = "/";
}

template <typename Callback>
int Guard(std::string_view name, DangPluginErrorV1* error,
          Callback&& callback) noexcept {
  // A host may reuse one error descriptor across several callbacks. Clear it
  // before dispatch so success cannot appear to carry an older failure and a
  // callback that fails before SetError cannot expose stale borrowed strings.
  if (error) *error = {};
  return GuardPluginCallback(
      std::forward<Callback>(callback),
      [&](std::string_view detail) noexcept {
        SetUnexpectedError(error, name, detail);
      });
}

size_t SourceCount(void*) { return 4; }

int SourceAtImpl(void*, size_t index, DangYangSourceV1* source,
                 DangPluginErrorV1* error) {
  if (!source) {
    SetError(error, "the YANG source output is missing");
    return 0;
  }
  *source = {};
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
    const char* xml, DangPluginErrorV1* error,
    bool capture_missing_from_live = false) {
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
    bool missing = false;
    auto translated =
        TranslateConfiguration(xml, module, socket, &reason, &missing);
    if (!translated && missing && capture_missing_from_live)
      translated =
          ReadLiveConfiguration(module, socket, SendControlQuery, &reason);
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

int PrepareConfigurationImpl(void*, const DangTransactionV1* transaction,
                             void** result, DangPluginErrorV1* error) {
  if (!result) {
    SetError(error, "the transaction input is incomplete");
    return 0;
  }
  *result = nullptr;
  if (!transaction) {
    SetError(error, "the transaction input is incomplete");
    return 0;
  }
  // Dangd activates startup configuration as an empty-to-running transaction.
  // Capture any absent module's live image so failed startup can restore the
  // daemon state that existed before dangd asserted its persisted authority.
  auto before = TranslateBoth(transaction->before_xml, error, true);
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

int ValidateConfigurationImpl(void*, void* opaque, DangPluginErrorV1* error) {
  const auto* prepared = static_cast<Prepared*>(opaque);
  if (!prepared) {
    SetError(error, "the prepared Kea transaction is missing");
    return 0;
  }
  for (std::size_t index = 0; index < prepared->proposed.size(); ++index) {
    const ServerConfiguration& server = prepared->proposed[index];
    if (index < prepared->before.size() &&
        prepared->before[index].arguments == server.arguments)
      continue;
    std::string reason;
    if (Execute(server, "config-test", &reason)) continue;
    SetError(error, server.module_name + ": " + reason,
             "/{urn:ietf:params:xml:ns:yang:" + server.module_name +
                 "}config");
    return 0;
  }
  return 1;
}

int ApplyConfigurationImpl(void*, void* opaque, DangPluginErrorV1* error) {
  const auto* prepared = static_cast<Prepared*>(opaque);
  if (!prepared) {
    SetError(error, "the prepared Kea transaction is missing");
    return 0;
  }
  std::string failed_module;
  std::string reason;
  bool pending = false;
  if (!BeginPendingApply(*prepared, &pending, &failed_module, &reason)) {
    const std::string path = failed_module.empty()
        ? "/"
        : "/{urn:ietf:params:xml:ns:yang:" + failed_module + "}config";
    SetError(error, std::move(reason), path);
    return 0;
  }
  bool compensation_complete = false;
  if (!ApplyWithCompensation(prepared->before, prepared->proposed, Execute,
                             &failed_module, &reason,
                             &compensation_complete)) {
    if (pending && compensation_complete) {
      std::string readback_module;
      std::string readback_reason;
      if (VerifyRestoredConfigurations(
              prepared->before, prepared->proposed, SendControlQuery,
              &readback_module, &readback_reason)) {
        ClearPendingApply();
      } else {
        reason += "; " + readback_reason;
        if (!readback_module.empty()) failed_module = readback_module;
      }
    }
    const std::string path = failed_module.empty()
        ? "/"
        : "/{urn:ietf:params:xml:ns:yang:" + failed_module + "}config";
    SetError(error, std::move(reason), path);
    return 0;
  }
  return 1;
}

int RollbackConfigurationImpl(void*, void* opaque, DangPluginErrorV1* error) {
  const auto* prepared = static_cast<Prepared*>(opaque);
  if (!prepared) {
    SetError(error, "the prepared Kea transaction is missing");
    return 0;
  }
  std::string failed_module;
  std::string reason;
  // Rollback is another multi-daemon hardware mutation. Suppress operational
  // publication from its first changing config-set until every restoration
  // succeeds and the accepted before-image is installed atomically.
  if (!BeginPendingRollback(*prepared, &failed_module, &reason)) {
    const std::string path = failed_module.empty()
        ? "/"
        : "/{urn:ietf:params:xml:ns:yang:" + failed_module + "}config";
    SetError(error, std::move(reason), path);
    return 0;
  }
  if (RollbackChanged(prepared->before, prepared->proposed, Execute,
                      &failed_module, &reason)) {
    if (VerifyRestoredConfigurations(
            prepared->before, prepared->proposed, SendControlQuery,
            &failed_module, &reason)) {
      RememberAcceptedState(prepared->before);
      return 1;
    }
  }
  const std::string path = failed_module.empty()
      ? "/"
      : "/{urn:ietf:params:xml:ns:yang:" + failed_module + "}config";
  SetError(error, std::move(reason), path);
  return 0;
}

void Release(void*, void* opaque) { delete static_cast<Prepared*>(opaque); }

size_t HardwareActionCount(void*, void* opaque) {
  return opaque ? 1 : 0;
}

int HardwareActionAtImpl(void*, void* opaque, size_t index,
                         DangHardwareActionV1* action,
                         DangPluginErrorV1* error) {
  if (!action) {
    SetError(error, "the Kea transaction action is unavailable");
    return 0;
  }
  *action = {};
  if (!opaque || index != 0) {
    SetError(error, "the Kea transaction action is unavailable");
    return 0;
  }
  // Kea accepts each daemon's complete configuration as one config-set.
  // Advertising one normal action preserves that indivisible unit instead of
  // pretending individual YANG leaves can be safely reordered by dangd.
  *action = {"configuration", "/", DANG_HARDWARE_NORMAL_V1, nullptr, 0};
  return 1;
}

int ApplyHardwareActionImpl(void* context, void* opaque, const char* action_id,
                            DangPluginErrorV1* error) {
  if (!action_id || std::string_view(action_id) != "configuration") {
    SetError(error, "the Kea hardware action ID is unknown");
    return 0;
  }
  return ApplyConfigurationImpl(context, opaque, error);
}

int RollbackHardwareActionImpl(void* context, void* opaque,
                               const char* action_id,
                               DangPluginErrorV1* error) {
  if (!action_id || std::string_view(action_id) != "configuration") {
    SetError(error, "the Kea hardware action ID is unknown");
    return 0;
  }
  return RollbackConfigurationImpl(context, opaque, error);
}

int OperationalImpl(void*, DangOperationalDataV1* result,
                    DangPluginErrorV1* error) {
  if (!result) {
    SetError(error, "the operational data output is missing");
    return 0;
  }
  *result = {};
  // Retain a shared authority lock through the complete multi-command read.
  // Apply must acquire the exclusive side before its first config-set, so a
  // state collection can never overlap a hardware configuration transition.
  std::shared_lock accepted_lock(accepted_state_mutex);
  const AcceptedConfigurationSnapshot accepted = accepted_state;
  if (!accepted.pending_module.empty()) {
    SetError(error,
             accepted.pending_module +
                 ": applied configuration is awaiting dangd reconciliation",
             "/{urn:ietf:params:xml:ns:yang:" + accepted.pending_module +
                 "}config");
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
  std::size_t server_index = 0;
  for (const auto& [module, dhcp6] : {
           std::pair{"kea-dhcp4-server", false},
           std::pair{"kea-dhcp6-server", true}}) {
    std::string reason;
    if (server_index >= accepted.configurations.size()) {
      SetError(error,
               module +
                   std::string(": accepted configuration is unavailable"),
               "/{" + std::string("urn:ietf:params:xml:ns:yang:") + module +
                   "}config");
      return 0;
    }
    std::string failure_path;
    if (operational_xml.size() + kOperationalClose.size() >=
        kMaximumOperationalXmlBytes) {
      SetError(error,
               module +
                   std::string(": operational XML exceeds the byte limit"),
               "/{" + std::string("urn:ietf:params:xml:ns:yang:") + module +
                   "}state");
      return 0;
    }
    PageLimits limits;
    limits.maximum_xml_bytes = kMaximumOperationalXmlBytes -
        operational_xml.size() - kOperationalClose.size();
    auto state = CollectAuthoritativeOperationalState(
        accepted.configurations[server_index], dhcp6,
        accepted.subnet_ids[server_index], SendControlQuery, &failure_path,
        &reason, limits);
    if (!state) {
      SetError(error, module + std::string(": ") + reason,
               "/{" + std::string("urn:ietf:params:xml:ns:yang:") + module +
                   "}" + failure_path);
      return 0;
    }
    operational_xml += *state;
    ++server_index;
  }
  operational_xml += kOperationalClose;
  result->data_xml = operational_xml.c_str();
  return 1;
}

int OperationalV2Impl(void* context, DangOperationalDataV2* result,
                      DangPluginErrorV1* error) {
  if (!result) {
    SetError(error, "the operational data output is missing");
    return 0;
  }
  *result = {};
  DangOperationalDataV1 legacy{};
  if (!OperationalImpl(context, &legacy, error)) return 0;
  *result = {legacy.data_xml, 1};
  return 1;
}

int ReconcileAppliedConfigurationImpl(
    void*, void* opaque, const char* current_xml,
    DangAppliedConfigurationV1* result, DangPluginErrorV1* error) {
  if (!result) {
    SetError(error, "the applied Kea configuration snapshot is missing", "/");
    return 0;
  }
  *result = {};
  if (!current_xml) {
    SetError(error, "the applied Kea configuration snapshot is missing", "/");
    return 0;
  }
  auto accepted = TranslateBoth(current_xml, error);
  if (!accepted) return 0;
  // Bind current_xml to the prepared proposal before using that proposal to
  // select changed daemons. Then read those daemons back before dangd makes the
  // snapshot authoritative. A null preparation is startup recovery, where
  // every daemon must match persisted intent before publication can resume.
  const auto* prepared = static_cast<const Prepared*>(opaque);
  if (prepared && (prepared->before.size() != accepted->size() ||
                   prepared->proposed.size() != accepted->size())) {
    SetError(error, "the prepared Kea transaction is inconsistent", "/");
    return 0;
  }
  if (prepared) {
    for (std::size_t index = 0; index < accepted->size(); ++index) {
      const auto& proposed = prepared->proposed[index];
      const auto& current = (*accepted)[index];
      if (current.module_name == proposed.module_name &&
          current.service_name == proposed.service_name &&
          current.socket_path == proposed.socket_path &&
          current.arguments == proposed.arguments)
        continue;
      const std::string& module = current.module_name.empty()
          ? proposed.module_name : current.module_name;
      SetError(error,
               module +
                   ": applied snapshot does not match the prepared proposal",
               "/{urn:ietf:params:xml:ns:yang:" + module + "}config");
      return 0;
    }
  }
  bool prepared_changes_configuration = false;
  std::string prepared_changed_module;
  if (prepared) {
    for (std::size_t index = 0; index < prepared->before.size(); ++index) {
      if (prepared->before[index].arguments !=
          prepared->proposed[index].arguments) {
        prepared_changes_configuration = true;
        prepared_changed_module = prepared->proposed[index].module_name;
        break;
      }
    }
  }
  bool verify_all_pending = false;
  {
    std::shared_lock lock(accepted_state_mutex);
    if (!accepted_state.pending_module.empty()) {
      if (!SameConfigurationSet(accepted_state.pending_proposal, *accepted)) {
        const std::string& module = accepted_state.pending_module;
        SetError(error,
                 module +
                     ": applied snapshot does not own the pending mutation",
                 "/{urn:ietf:params:xml:ns:yang:" + module + "}config");
        return 0;
      }
      // An ordinary changing transaction verifies its changed subset below.
      // Null/no-op recovery has no such delta, so it must verify every daemon
      // before it may resolve the exact pending proposal.
      verify_all_pending = !prepared_changes_configuration;
    } else if (prepared_changes_configuration) {
      const std::string& module = prepared_changed_module;
      SetError(error,
               module + ": prepared proposal has not been applied",
               "/{urn:ietf:params:xml:ns:yang:" + module + "}config");
      return 0;
    }
  }
  for (std::size_t index = 0; index < accepted->size(); ++index) {
    if (prepared && !verify_all_pending &&
        prepared->before[index].arguments ==
                        prepared->proposed[index].arguments)
      continue;
    std::string reason;
    if (VerifyLiveConfiguration((*accepted)[index], SendControlQuery, &reason))
      continue;
    const std::string& module = (*accepted)[index].module_name;
    SetError(error, module + ": " + reason,
             "/{urn:ietf:params:xml:ns:yang:" + module + "}config");
    return 0;
  }
  RememberAcceptedState(*accepted);
  *result = {.applied_xml = current_xml,
             .outcomes = nullptr,
             .outcome_count = 0};
  return 1;
}

int SourceAt(void* context, size_t index, DangYangSourceV1* source,
             DangPluginErrorV1* error) noexcept {
  return Guard("YANG source", error, [&]() {
    return SourceAtImpl(context, index, source, error);
  });
}

int PrepareConfiguration(void* context, const DangTransactionV1* transaction,
                         void** result, DangPluginErrorV1* error) noexcept {
  return Guard("prepare", error, [&]() {
    return PrepareConfigurationImpl(context, transaction, result, error);
  });
}

int ValidateConfiguration(void* context, void* opaque,
                          DangPluginErrorV1* error) noexcept {
  return Guard("validate", error, [&]() {
    return ValidateConfigurationImpl(context, opaque, error);
  });
}

int ApplyConfiguration(void* context, void* opaque,
                       DangPluginErrorV1* error) noexcept {
  return Guard("apply", error, [&]() {
    return ApplyConfigurationImpl(context, opaque, error);
  });
}

int RollbackConfiguration(void* context, void* opaque,
                          DangPluginErrorV1* error) noexcept {
  return Guard("rollback", error, [&]() {
    return RollbackConfigurationImpl(context, opaque, error);
  });
}

int HardwareActionAt(void* context, void* opaque, size_t index,
                     DangHardwareActionV1* action,
                     DangPluginErrorV1* error) noexcept {
  return Guard("hardware-action", error, [&]() {
    return HardwareActionAtImpl(context, opaque, index, action, error);
  });
}

int ApplyHardwareAction(void* context, void* opaque, const char* action_id,
                        DangPluginErrorV1* error) noexcept {
  return Guard("hardware apply", error, [&]() {
    return ApplyHardwareActionImpl(context, opaque, action_id, error);
  });
}

int RollbackHardwareAction(void* context, void* opaque, const char* action_id,
                           DangPluginErrorV1* error) noexcept {
  return Guard("hardware rollback", error, [&]() {
    return RollbackHardwareActionImpl(context, opaque, action_id, error);
  });
}

int Operational(void* context, DangOperationalDataV1* result,
                DangPluginErrorV1* error) noexcept {
  return Guard("operational", error, [&]() {
    return OperationalImpl(context, result, error);
  });
}

int OperationalV2(void* context, DangOperationalDataV2* result,
                  DangPluginErrorV1* error) noexcept {
  return Guard("complete operational", error, [&]() {
    return OperationalV2Impl(context, result, error);
  });
}

int ReconcileAppliedConfiguration(
    void* context, void* opaque, const char* current_xml,
    DangAppliedConfigurationV1* result, DangPluginErrorV1* error) noexcept {
  return Guard("applied-state reconciliation", error, [&]() {
    return ReconcileAppliedConfigurationImpl(context, opaque, current_xml,
                                              result, error);
  });
}

const DangPluginV6 kPlugin{
    .v5 = {.v4 = {.v3 = {.v2 = {.v1 = {.abi_version = DANG_PLUGIN_ABI_V6,
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
    .get_operational_data_v2 = OperationalV2},
    .reconcile_applied_configuration = ReconcileAppliedConfiguration};

}  // namespace

extern "C" const DangPluginV6* dang_plugin_init_v6() { return &kPlugin; }
extern "C" const DangPluginV5* dang_plugin_init_v5() { return &kPlugin.v5; }
extern "C" const DangPluginV3* dang_plugin_init_v3() {
  return &kPlugin.v5.v4.v3;
}
extern "C" const DangPluginV1* dang_plugin_init_v1() {
  return &kPlugin.v5.v4.v3.v2.v1;
}

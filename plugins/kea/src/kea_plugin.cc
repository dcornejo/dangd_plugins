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
#include "kea_instance.h"
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
using dang::plugins::kea::BuildInstanceOperationalXml;
using dang::plugins::kea::BuildPeerTransactionCandidates;
using dang::plugins::kea::CollectAuthoritativeOperationalState;
using dang::plugins::kea::CommandSucceeded;
using dang::plugins::kea::ExtractSubnetIds;
using dang::plugins::kea::GuardPluginCallback;
using dang::plugins::kea::PageLimits;
using dang::plugins::kea::PeerTransactionCandidate;
using dang::plugins::kea::ReadDaemonVersion;
using dang::plugins::kea::ReadLiveConfiguration;
using dang::plugins::kea::RollbackChanged;
using dang::plugins::kea::SendControlCommand;
using dang::plugins::kea::SendControlQuery;
using dang::plugins::kea::ServerConfiguration;
using dang::plugins::kea::TranslateConfiguration;
using dang::plugins::kea::VerifyLiveConfiguration;
using dang::plugins::kea::VerifyPeerTransactionReplies;
using dang::plugins::kea::VerifyRequiredControlCommands;
using dang::plugins::kea::VerifyRestoredConfigurations;
using dang::plugins::kea::ValidInstanceId;

struct Prepared {
  // Target order comes from the process-stable inventory and is shared by both
  // images. This makes index-based compensation unambiguous after a partial
  // apply, including single-stack deployments.
  std::vector<ServerConfiguration> before;
  std::vector<ServerConfiguration> proposed;
  struct PeerPlan {
    PeerTransactionCandidate candidate;
    // Opaque identity is echoed by dangd only to this plugin. Keeping the
    // authoritative expected images in Prepared avoids trusting serialized
    // JSON to reconstruct a safety decision.
    std::string verification_context;
  };
  std::vector<PeerPlan> peers;
};

/** One enabled local Kea daemon captured for this plugin process. */
struct TargetDefinition {
  /** YANG module that owns the daemon's configuration and state. */
  std::string module_name;
  /** Local UNIX control endpoint used for every native operation. */
  std::string socket_path;
  /** Selects DHCPv6 rather than DHCPv4 native state commands. */
  bool dhcp6;
};

/** Immutable bootstrap result shared by every callback. */
struct PluginContext {
  /** Stable operator identity for this independently persisted dangd process. */
  std::string instance_id = "default";
  std::vector<TargetDefinition> targets;
  std::string configuration_error;
};

/** Mapping from one supported family to its bootstrap variable. */
struct TargetEnvironment {
  const char* module_name;
  const char* variable_name;
  bool dhcp6;
};

constexpr std::array<TargetEnvironment, 2> kTargetEnvironments{{
    {"kea-dhcp4-server", "DANG_KEA_DHCP4_SOCKET", false},
    {"kea-dhcp6-server", "DANG_KEA_DHCP6_SOCKET", true},
}};

/** Captures enabled families once, distinguishing unset from empty variables. */
PluginContext BuildPluginContext() {
  PluginContext context;
  if (const char* instance = std::getenv("DANG_KEA_INSTANCE_ID")) {
    if (!ValidInstanceId(instance)) {
      context.configuration_error =
          "DANG_KEA_INSTANCE_ID must contain 1 to 64 portable identifier "
          "characters and start with an ASCII letter or digit";
      return context;
    }
    context.instance_id = instance;
  }
  for (const auto& target : kTargetEnvironments) {
    const char* socket = std::getenv(target.variable_name);
    if (!socket) continue;
    if (!*socket) {
      context.configuration_error = std::string(target.variable_name) +
          " is present but does not name a local Kea UNIX control socket";
      return context;
    }
    context.targets.push_back(
        {target.module_name, socket, target.dhcp6});
  }
  if (context.targets.empty())
    context.configuration_error =
        "at least one of DANG_KEA_DHCP4_SOCKET or DANG_KEA_DHCP6_SOCKET "
        "must name a local Kea UNIX control socket";
  return context;
}

PluginContext plugin_context = BuildPluginContext();

thread_local std::string callback_error;
thread_local std::string callback_path;
thread_local std::string operational_xml;
thread_local std::array<char, 1024> unexpected_callback_error;

// Match dangd's default XML document ceiling before returning a provider
// buffer. The remaining allowance is shared by the DHCPv4 and DHCPv6 trees.
constexpr std::size_t kMaximumOperationalXmlBytes = 16U * 1024U * 1024U;
constexpr std::string_view kOperationalClose = "</data>";
constexpr std::string_view kHaOperationalOpen =
    "<high-availability xmlns=\"urn:dang:kea:ha\">";
constexpr std::string_view kHaOperationalClose = "</high-availability>";

// Operational state must describe only configuration accepted by dangd and
// known to have reached Kea. Preparing or validating a candidate therefore
// cannot alter this state. Apply also leaves it unchanged until post-apply
// reconciliation accepts readback; reconciliation and rollback are its only
// writers.
struct AcceptedConfigurationSnapshot {
  std::vector<std::vector<std::uint32_t>> subnet_ids;
  std::vector<ServerConfiguration> configurations;
  // Native identities sampled at the same reconciliation boundary as the
  // accepted configuration. Empty entries occur only in transport-free test
  // seeding and are not published as daemon identity.
  std::vector<std::string> daemon_versions;
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
    const std::vector<ServerConfiguration>& configurations,
    std::vector<std::string> daemon_versions = {}) {
  // Build the complete replacement before taking the lock. If allocation
  // fails, readers retain the previous internally consistent snapshot.
  AcceptedConfigurationSnapshot next;
  next.configurations = configurations;
  next.subnet_ids.reserve(configurations.size());
  for (const auto& configuration : configurations)
    next.subnet_ids.push_back(ExtractSubnetIds(configuration));
  std::lock_guard lock(accepted_state_mutex);
  if (daemon_versions.empty() &&
      accepted_state.daemon_versions.size() == configurations.size()) {
    next.daemon_versions = accepted_state.daemon_versions;
  } else {
    next.daemon_versions = std::move(daemon_versions);
  }
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

size_t SourceCount(void*) { return 6; }

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
       DANG_YANG_IMPLEMENTED_V1, nullptr, 0},
      {"dang-kea-ha", "2026-09-28", kDangKeaHaYang,
       std::strlen(kDangKeaHaYang),
       "https://github.com/dcornejo/dang_plugins/blob/main/plugins/kea/models/"
       "dang-kea-ha%402026-09-28.yang",
       DANG_YANG_IMPLEMENTED_V1, nullptr, 0},
      {"dang-kea-instance", "2026-09-28", kDangKeaInstanceYang,
       std::strlen(kDangKeaInstanceYang),
       "https://github.com/dcornejo/dang_plugins/blob/main/plugins/kea/models/"
       "dang-kea-instance%402026-09-28.yang",
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

std::optional<std::vector<ServerConfiguration>> TranslateTargets(
    const PluginContext& context, const char* xml, DangPluginErrorV1* error,
    bool capture_missing_from_live = false) {
  if (!xml) {
    SetError(error, "the configuration snapshot is missing");
    return std::nullopt;
  }
  if (!context.configuration_error.empty()) {
    SetError(error, context.configuration_error, "/");
    return std::nullopt;
  }
  // A disabled family must also be absent from authoritative configuration.
  // This keeps deployment intent explicit: omitting a socket disables a
  // family, while retaining its YANG tree cannot silently discard it.
  for (const auto& environment : kTargetEnvironments) {
    const bool enabled = std::any_of(
        context.targets.begin(), context.targets.end(),
        [&](const TargetDefinition& target) {
          return target.module_name == environment.module_name;
        });
    if (enabled) continue;
    std::string ignored_reason;
    bool missing = false;
    (void)TranslateConfiguration(xml, environment.module_name,
                                 "/disabled-kea-target", &ignored_reason,
                                 &missing);
    if (missing) continue;
    const std::string module = environment.module_name;
    SetError(error,
             module + ": configuration is present while " +
                 environment.variable_name + " is disabled",
             "/{urn:ietf:params:xml:ns:yang:" + module + "}config");
    return std::nullopt;
  }
  std::vector<ServerConfiguration> configurations;
  configurations.reserve(context.targets.size());
  for (const auto& target : context.targets) {
    std::string reason;
    bool missing = false;
    auto translated = TranslateConfiguration(xml, target.module_name,
                                              target.socket_path, &reason,
                                              &missing);
    if (!translated && missing && capture_missing_from_live)
      translated = ReadLiveConfiguration(target.module_name,
                                          target.socket_path,
                                          SendControlQuery, &reason);
    if (!translated) {
      SetError(error, target.module_name + std::string(": ") + reason,
               "/{" + std::string("urn:ietf:params:xml:ns:yang:") +
                   target.module_name + "}config");
      return std::nullopt;
    }
    configurations.push_back(std::move(*translated));
  }
  return configurations;
}

int PrepareConfigurationImpl(void* opaque_context,
                             const DangTransactionV1* transaction,
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
  const auto& context = *static_cast<const PluginContext*>(opaque_context);
  auto before =
      TranslateTargets(context, transaction->before_xml, error, true);
  if (!before) return 0;
  auto proposed = TranslateTargets(context, transaction->proposed_xml, error);
  if (!proposed) return 0;
  std::string peer_error;
  auto peer_candidates = BuildPeerTransactionCandidates(
      transaction->proposed_xml, *proposed, &peer_error);
  if (!peer_candidates) {
    SetError(error, std::move(peer_error), "/");
    return 0;
  }
  std::vector<Prepared::PeerPlan> peers;
  peers.reserve(peer_candidates->size());
  for (PeerTransactionCandidate& candidate : *peer_candidates) {
    const std::string context_json =
        nlohmann::json{{"version", 1},
                       {"group_id", candidate.group_id},
                       {"participant_id", candidate.participant_id}}
            .dump();
    peers.push_back({std::move(candidate), context_json});
  }
  auto* prepared = new (std::nothrow)
      Prepared{std::move(*before), std::move(*proposed), std::move(peers)};
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

int OperationalImpl(void* opaque_context, DangOperationalDataV1* result,
                    DangPluginErrorV1* error) {
  if (!result) {
    SetError(error, "the operational data output is missing");
    return 0;
  }
  *result = {};
  const auto& context = *static_cast<const PluginContext*>(opaque_context);
  if (!context.configuration_error.empty()) {
    SetError(error, context.configuration_error, "/");
    return 0;
  }
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
  if (accepted.configurations.empty() ||
      accepted.configurations.size() != accepted.subnet_ids.size() ||
      accepted.configurations.size() != context.targets.size() ||
      (!accepted.daemon_versions.empty() &&
       accepted.configurations.size() != accepted.daemon_versions.size())) {
    SetError(error, "Kea accepted configuration is unavailable", "/");
    return 0;
  }
  operational_xml =
      "<data xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">";
  std::vector<bool> dhcp6_families;
  dhcp6_families.reserve(context.targets.size());
  for (const auto& target : context.targets) {
    dhcp6_families.push_back(target.dhcp6);
  }
  operational_xml +=
      BuildInstanceOperationalXml(context.instance_id, dhcp6_families,
                                  accepted.daemon_versions);
  // The ISC state containers and dang-owned HA companion tree have different
  // namespaces and must be sibling top-level data nodes. Accumulate only the
  // list entries here, then emit one companion container after all targets.
  std::string ha_entries;
  for (std::size_t index = 0; index < accepted.configurations.size(); ++index) {
    const auto& configuration = accepted.configurations[index];
    const auto& target = context.targets[index];
    const std::string& module = target.module_name;
    if (configuration.module_name != target.module_name ||
        configuration.socket_path != target.socket_path) {
      SetError(error, module + ": accepted target identity is inconsistent",
               "/{urn:ietf:params:xml:ns:yang:" + module + "}config");
      return 0;
    }
    std::string reason;
    std::string failure_path;
    const std::size_t fixed_tail = kOperationalClose.size() +
                                   kHaOperationalOpen.size() +
                                   kHaOperationalClose.size();
    if (operational_xml.size() + ha_entries.size() + fixed_tail >=
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
        operational_xml.size() - ha_entries.size() - fixed_tail;
    std::string ha_state;
    auto state = CollectAuthoritativeOperationalState(
        configuration, target.dhcp6, accepted.subnet_ids[index],
        SendControlQuery, &failure_path, &reason, limits, &ha_state);
    if (!state) {
      const std::string path =
          failure_path == "ha-state"
              ? "/{urn:dang:kea:ha}high-availability"
              : "/{" + std::string("urn:ietf:params:xml:ns:yang:") + module +
                    "}" + failure_path;
      SetError(error, module + std::string(": ") + reason, path);
      return 0;
    }
    operational_xml += *state;
    ha_entries += ha_state;
  }
  if (!ha_entries.empty()) {
    operational_xml += kHaOperationalOpen;
    operational_xml += ha_entries;
    operational_xml += kHaOperationalClose;
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
    void* opaque_context, void* opaque, const char* current_xml,
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
  const auto& context = *static_cast<const PluginContext*>(opaque_context);
  auto accepted = TranslateTargets(context, current_xml, error);
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
  std::vector<std::string> daemon_versions;
  {
    std::shared_lock lock(accepted_state_mutex);
    if (accepted_state.daemon_versions.size() == accepted->size())
      daemon_versions = accepted_state.daemon_versions;
  }
  if (daemon_versions.empty()) daemon_versions.resize(accepted->size());
  for (std::size_t index = 0; index < accepted->size(); ++index) {
    if (prepared && !verify_all_pending &&
        prepared->before[index].arguments ==
                        prepared->proposed[index].arguments)
      continue;
    std::string reason;
    if (VerifyLiveConfiguration((*accepted)[index], SendControlQuery, &reason) &&
        VerifyRequiredControlCommands(
            (*accepted)[index], SendControlQuery, &reason)) {
      auto version = ReadDaemonVersion(
          (*accepted)[index].socket_path, SendControlQuery, &reason);
      if (version) {
        daemon_versions[index] = std::move(*version);
        continue;
      }
    }
    const std::string& module = (*accepted)[index].module_name;
    SetError(error, module + ": " + reason,
             "/{urn:ietf:params:xml:ns:yang:" + module + "}config");
    return 0;
  }
  RememberAcceptedState(*accepted, std::move(daemon_versions));
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

int NextNotification(void*, DangNotificationV1* notification,
                     DangPluginErrorV1* error) noexcept {
  return Guard("notification", error, [&]() {
    if (!notification) {
      SetError(error, "the notification output is missing");
      return -1;
    }
    *notification = {};
    return 0;
  });
}

/** Returns the flattened module contribution count for all planned members. */
size_t PeerCandidateCount(void*, void* opaque) {
  const auto* prepared = static_cast<const Prepared*>(opaque);
  if (!prepared) return 0;
  std::size_t count = 0;
  for (const auto& peer : prepared->peers)
    count += peer.candidate.modules.size();
  return count;
}

/** Borrows one retained module image through the public ABI-v9 descriptor. */
int PeerCandidateAtImpl(void* opaque_context, void* opaque, size_t index,
                        DangPeerCandidateV1* result,
                        DangPluginErrorV1* error) {
  if (!result) {
    SetError(error, "the peer candidate output is missing");
    return 0;
  }
  *result = {};
  const auto* prepared = static_cast<const Prepared*>(opaque);
  if (!prepared) {
    SetError(error, "the prepared Kea transaction is missing");
    return 0;
  }
  for (const auto& peer : prepared->peers) {
    if (index >= peer.candidate.modules.size()) {
      index -= peer.candidate.modules.size();
      continue;
    }
    const auto& module = peer.candidate.modules[index];
    result->group_id = peer.candidate.group_id.c_str();
    result->participant_id = peer.candidate.participant_id.c_str();
    const auto* context = static_cast<const PluginContext*>(opaque_context);
    result->local = context &&
        context->instance_id == peer.candidate.participant_id;
    result->role = peer.candidate.primary ? DANG_PEER_PRIMARY_V1
                                          : DANG_PEER_STANDBY_V1;
    result->confirmed_timeout_seconds = 60;
    result->module_name = module.module_name.c_str();
    result->configuration_xml = module.configuration_xml.c_str();
    result->verification_context_json = peer.verification_context.c_str();
    return 1;
  }
  SetError(error, "the peer candidate index is out of range");
  return 0;
}

int PeerCandidateAt(void* context, void* opaque, size_t index,
                    DangPeerCandidateV1* result,
                    DangPluginErrorV1* error) noexcept {
  return Guard("peer candidate", error, [&]() {
    return PeerCandidateAtImpl(context, opaque, index, result, error);
  });
}

/**
 * Verifies authenticated replies against the retained member-specific plan.
 *
 * Returns one for acceptance, zero for permanent rejection, and minus one for
 * a recognized bounded Kea convergence state under the generic ABI contract.
 */
int VerifyPeerImpl(void*, void* opaque,
                   const DangPeerVerificationV1* verification,
                   DangPluginErrorV1* error) {
  const auto* prepared = static_cast<const Prepared*>(opaque);
  if (!prepared || !verification || !verification->group_id ||
      !verification->participant_id ||
      !verification->verification_context_json ||
      !verification->running_reply_xml || !verification->operational_reply_xml) {
    SetError(error, "the Kea peer verification input is incomplete", "/");
    return 0;
  }
  const Prepared::PeerPlan* selected = nullptr;
  for (const auto& peer : prepared->peers) {
    if (peer.candidate.group_id != verification->group_id ||
        peer.candidate.participant_id != verification->participant_id)
      continue;
    if (selected) {
      SetError(error, "the Kea peer verification identity is ambiguous", "/");
      return 0;
    }
    selected = &peer;
  }
  if (!selected ||
      selected->verification_context != verification->verification_context_json) {
    SetError(error, "the Kea peer verification identity is unknown", "/");
    return 0;
  }
  std::vector<ServerConfiguration> expected;
  expected.reserve(selected->candidate.modules.size());
  for (const auto& module : selected->candidate.modules)
    expected.push_back(module.expected_configuration);
  std::string reason;
  bool pending = false;
  if (!VerifyPeerTransactionReplies(
          expected, selected->candidate.health, verification->running_reply_xml,
          verification->operational_reply_xml, &reason, &pending)) {
    SetError(error, "Kea peer " + selected->candidate.participant_id +
                        " verification failed: " + reason,
             "/");
    return pending ? -1 : 0;
  }
  return 1;
}

int VerifyPeer(void* context, void* opaque,
               const DangPeerVerificationV1* verification,
               DangPluginErrorV1* error) noexcept {
  return Guard("peer verification", error, [&]() {
    return VerifyPeerImpl(context, opaque, verification, error);
  });
}

const DangPluginV9 kPlugin{
    .v8 = {.v7 = {.v6 = {.v5 = {.v4 = {.v3 = {.v2 = {.v1 = {
                  .abi_version = DANG_PLUGIN_ABI_V9,
                  .plugin_name = "dang-kea",
                  .context = &plugin_context,
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
    .reconcile_applied_configuration = ReconcileAppliedConfiguration},
    .resource_domain_count = nullptr,
    .resource_domain_at = nullptr},
    .next_notification = NextNotification},
    .peer_candidate_count = PeerCandidateCount,
    .peer_candidate_at = PeerCandidateAt,
    .verify_peer = VerifyPeer};

}  // namespace

extern "C" const DangPluginV9* dang_plugin_init_v9() { return &kPlugin; }

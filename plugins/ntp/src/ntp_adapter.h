// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANG_PLUGINS_NTP_ADAPTER_H_
#define DANG_PLUGINS_NTP_ADAPTER_H_

#include <memory>
#include <string>
#include <string_view>

#include "plugins/ntp/src/ntp_config.h"

namespace dang::ntp {

/** Operating-system family whose native NTP integration is being selected. */
enum class Platform { kUnsupported, kLinux, kFreeBsd };

/** Native daemon family selected for one plugin transaction. */
enum class Daemon { kUnavailable, kChrony, kNtpd };

/** Immutable identity and readiness claims for one native adapter. */
struct AdapterDescriptor {
  Platform platform = Platform::kUnsupported;
  Daemon daemon = Daemon::kUnavailable;
  std::string platform_name;
  std::string daemon_name;
  bool configuration_ready = false;
  bool operational_ready = false;
};

/**
 * Per-action state retained until commit succeeds or compensation finishes.
 *
 * A real adapter must capture every native before-image needed to reverse its
 * action before setting applied. Secrets stored here must be zeroized by the
 * adapter when the transaction is released.
 */
struct AdapterCheckpoint {
  std::string action_id;
  std::string before_image;
  bool applied = false;
};

/**
 * Daemon boundary consumed by the generic RFC 9249 plugin lifecycle.
 *
 * Implementations must acknowledge completion, update the checkpoint only
 * after acknowledgement, and make rollback safe after partial failure. They
 * may use programmatic daemon/service APIs but must never invoke a shell.
 */
class NativeAdapter {
 public:
  virtual ~NativeAdapter() = default;

  [[nodiscard]] virtual const AdapterDescriptor& descriptor() const = 0;
  [[nodiscard]] virtual bool ApplyAction(const Config& before,
                                         const Config& proposed,
                                         const PlannedAction& action,
                                         AdapterCheckpoint* checkpoint,
                                         std::string* error,
                                         std::string* error_path) = 0;
  [[nodiscard]] virtual bool RollbackAction(const Config& before,
                                            const Config& proposed,
                                            const PlannedAction& action,
                                            AdapterCheckpoint* checkpoint,
                                            std::string* error,
                                            std::string* error_path) = 0;
  [[nodiscard]] virtual bool OperationalXml(std::string* xml,
                                            std::string* error,
                                            std::string* error_path) = 0;
};

/** Native platform reported by the selected OS-specific translation unit. */
[[nodiscard]] Platform NativePlatform();

/** Pure selection policy used directly by cross-platform contract tests. */
[[nodiscard]] bool SelectAdapter(Platform platform, std::string_view requested,
                                 AdapterDescriptor* descriptor,
                                 std::string* error, std::string* error_path);

/**
 * Creates the current platform adapter for `auto`, `chrony`, or `ntpd`.
 *
 * Selection succeeds for the supported platform/daemon pair even while its
 * mutating methods remain fail-closed development stubs. This lets selection,
 * transaction retention, and error attribution mature independently of native
 * mutations.
 */
[[nodiscard]] std::unique_ptr<NativeAdapter> CreateNativeAdapter(
    std::string_view requested, std::string* error, std::string* error_path);

/** Stable model path associated with a planned native action. */
[[nodiscard]] const char* ActionPath(std::string_view action_id);

}  // namespace dang::ntp

#endif  // DANG_PLUGINS_NTP_ADAPTER_H_

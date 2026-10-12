// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0
#ifndef DANG_PLUGINS_NTP_CHRONY_FILES_H_
#define DANG_PLUGINS_NTP_CHRONY_FILES_H_

#include <string>

#include "plugins/ntp/src/chrony_config.h"
#include "plugins/ntp/src/managed_file.h"

namespace dang::ntp {
/**
 * Compensating transaction for a chrony fragment and its private key file.
 *
 * Both before-images are captured before either file changes. Installation
 * writes keys before the referencing fragment; removal deletes the fragment
 * before keys. Compensation reverses that order. The service adapter must
 * acknowledge a stopped daemon before Apply, and must leave it stopped while
 * recovery_pending() is true. The class itself never controls a service.
 *
 * Each file replacement is atomic, but the pair is not. Before-images exist
 * only in memory, so process/host crash recovery remains separate work.
 * Destroying this object erases captured buffers without performing I/O.
 */
class ChronyFiles {
 public:
  /** Optional sync injection provides deterministic durability-failure tests.
   */
  explicit ChronyFiles(int (*sync)(int) = nullptr)
      : keys_(sync), fragment_(sync) {}
  /** Capture both trusted, distinct absolute file paths. Call once. */
  bool Capture(const std::string& fragment_path, const std::string& key_path,
               std::string* error);
  /** Install one image, compensating both files immediately on any failure. */
  bool Apply(const ChronyConfigImage& image, std::string* error);
  /** Restore the pair, allowing retries if a previous compensation failed. */
  bool Rollback(std::string* error);
  bool recovery_pending() const { return state_ == State::kRecoveryPending; }
  bool applied() const { return state_ == State::kApplied; }

 private:
  enum class State {
    kNew,
    kCaptured,
    kApplied,
    kRecoveryPending,
    kRestored,
    kInvalid
  };
  bool Restore(std::string* error);
  ManagedFile keys_;
  ManagedFile fragment_;
  State state_ = State::kNew;
  bool removal_ = false;
};
}  // namespace dang::ntp
#endif  // DANG_PLUGINS_NTP_CHRONY_FILES_H_

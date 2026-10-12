// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0
#ifndef DANG_PLUGINS_NTP_CHRONY_TRANSACTION_H_
#define DANG_PLUGINS_NTP_CHRONY_TRANSACTION_H_
#include <string>

#include "plugins/ntp/src/chrony_files.h"

namespace dang::ntp {
/**
 * Synchronous native service boundary. Success means the requested state was
 * observed after job completion, not merely that a request was enqueued.
 * Failure may have changed state; the transaction must compensate explicitly.
 * Implementations must use bounded waits and supply a diagnostic on failure.
 */
class ChronyService {
 public:
  virtual ~ChronyService() = default;
  virtual bool Active(bool* active, std::string* error) = 0;
  virtual bool SetActive(bool active, std::string* error) = 0;
};

/**
 * Service-bracketed chrony configuration transaction, independent of systemd.
 *
 * Capture reads service state and both files without mutation. Apply stops the
 * service, installs the pair, then activates only a present configuration.
 * Failure compensates in the same bounded calls used for explicit rollback:
 * acknowledge stopped state, restore files, restore initial service state.
 * If any compensation fails, retain recovery state and allow Rollback retries.
 * The referenced service must outlive the transaction. Destruction performs
 * no service/file changes; durable worker/host crash recovery is not provided.
 */
class ChronyTransaction {
 public:
  explicit ChronyTransaction(ChronyService& service, int (*sync)(int) = nullptr)
      : service_(service), files_(sync) {}
  bool Capture(const std::string& fragment_path, const std::string& key_path,
               std::string* error);
  bool Apply(const ChronyConfigImage& image, std::string* error);
  bool Rollback(std::string* error);
  bool applied() const { return state_ == State::kApplied; }
  bool recovery_pending() const { return state_ == State::kRecoveryPending; }

 private:
  enum class State {
    kNew,
    kCaptured,
    kApplied,
    kRecoveryPending,
    kRestored,
    kInvalid
  };
  bool Compensate(std::string* error);
  ChronyService& service_;
  ChronyFiles files_;
  State state_ = State::kNew;
  bool initially_active_ = false;
  bool files_attempted_ = false;
};
}  // namespace dang::ntp
#endif  // DANG_PLUGINS_NTP_CHRONY_TRANSACTION_H_

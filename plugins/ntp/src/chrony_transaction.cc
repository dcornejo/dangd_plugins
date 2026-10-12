// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0
/** @file Service acknowledgement and file compensation for chrony updates. */
#include "plugins/ntp/src/chrony_transaction.h"

namespace dang::ntp {
namespace {
bool Fail(const char* message, std::string* error) {
  if (error) *error = message;
  return false;
}
}  // namespace

bool ChronyTransaction::Capture(const std::string& fragment_path,
                                const std::string& key_path,
                                std::string* error) {
  if (state_ != State::kNew)
    return Fail("chrony transaction may only be captured once", error);
  state_ = State::kInvalid;
  if (!service_.Active(&initially_active_, error) ||
      !files_.Capture(fragment_path, key_path, error))
    return false;
  state_ = State::kCaptured;
  return true;
}

bool ChronyTransaction::Apply(const ChronyConfigImage& image,
                              std::string* error) {
  if (state_ != State::kCaptured)
    return Fail("chrony transaction is not ready to apply", error);
  if (image.fragment.empty() && !image.key_file.empty())
    return Fail("chrony keys cannot be installed without a fragment", error);
  if (!Stop(error) || !Install(image, error)) return false;
  return image.fragment.empty() || Activate(error);
}

bool ChronyTransaction::Stop(std::string* error) {
  if (state_ != State::kCaptured)
    return Fail("chrony transaction is not ready to stop", error);
  std::string failure;
  if (!service_.SetActive(false, &failure)) return Abort(failure, error);
  state_ = State::kStopped;
  return true;
}

bool ChronyTransaction::Install(const ChronyConfigImage& image,
                                std::string* error) {
  if (state_ != State::kStopped)
    return Fail("chrony transaction requires an acknowledged stop", error);
  if (image.fragment.empty() && !image.key_file.empty())
    return Fail("chrony keys cannot be installed without a fragment", error);
  std::string failure;
  files_attempted_ = true;
  if (!files_.Apply(image, &failure)) return Abort(failure, error);
  state_ = image.fragment.empty() ? State::kApplied : State::kInstalled;
  return true;
}

bool ChronyTransaction::Activate(std::string* error) {
  if (state_ != State::kInstalled)
    return Fail("chrony transaction requires installed files before activation",
                error);
  std::string failure;
  if (!service_.SetActive(true, &failure)) return Abort(failure, error);
  state_ = State::kApplied;
  return true;
}

bool ChronyTransaction::Abort(const std::string& failure, std::string* error) {
  state_ = State::kRecoveryPending;
  std::string compensation;
  const bool restored = Compensate(&compensation);
  if (error) {
    *error = failure;
    if (!restored) *error += "; chrony compensation failed: " + compensation;
  }
  return false;
}

bool ChronyTransaction::Compensate(std::string* error) {
  state_ = State::kRecoveryPending;
  // A failed start/stop may have changed the daemon. Confirm stopped state
  // again before touching any files; a failed stop blocks file restoration.
  if (!service_.SetActive(false, error)) return false;
  if (files_attempted_ && !files_.Rollback(error)) return false;
  if (initially_active_ && !service_.SetActive(true, error)) return false;
  state_ = State::kRestored;
  return true;
}

bool ChronyTransaction::Rollback(std::string* error) {
  if (state_ == State::kRestored) return true;
  if (state_ != State::kApplied && state_ != State::kRecoveryPending &&
      state_ != State::kStopped && state_ != State::kInstalled)
    return Fail("chrony transaction has no applied checkpoint", error);
  return Compensate(error);
}
}  // namespace dang::ntp

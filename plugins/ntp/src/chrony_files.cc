// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0
/** @file Ordered installation and compensation of chrony's managed file pair.
 */
#include "plugins/ntp/src/chrony_files.h"

#include <filesystem>
#include <optional>
#include <string_view>

namespace dang::ntp {
namespace {
bool Fail(const char* message, std::string* error) {
  if (error) *error = message;
  return false;
}
}  // namespace

bool ChronyFiles::Capture(const std::string& fragment_path,
                          const std::string& key_path, std::string* error) {
  if (state_ != State::kNew)
    return Fail("chrony files may only be captured once", error);
  state_ = State::kInvalid;
  if (std::filesystem::path(fragment_path).lexically_normal() ==
      std::filesystem::path(key_path).lexically_normal())
    return Fail("chrony fragment and key paths must differ", error);
  if (!keys_.Capture(key_path, error) ||
      !fragment_.Capture(fragment_path, error))
    return false;
  state_ = State::kCaptured;
  return true;
}

bool ChronyFiles::Apply(const ChronyConfigImage& image, std::string* error) {
  if (state_ != State::kCaptured)
    return Fail("chrony file transaction is not ready to apply", error);
  // An empty fragment denotes an absent /ntp presence container. Never leave
  // a private key file behind when removing the configuration owner.
  if (image.fragment.empty() && !image.key_file.empty())
    return Fail("chrony keys cannot be installed without a fragment", error);
  removal_ = image.fragment.empty();
  std::string failure;
  bool success;
  if (removal_) {
    success = fragment_.Install(std::nullopt, 0644, &failure) &&
              keys_.Install(std::nullopt, 0600, &failure);
  } else {
    // Even an empty key image replaces the previous key file: stale keys must
    // not survive a successful configuration that removes authentication.
    success = keys_.Install(image.key_file, 0600, &failure) &&
              fragment_.Install(image.fragment, 0644, &failure);
  }
  if (success) {
    state_ = State::kApplied;
    return true;
  }
  state_ = State::kRecoveryPending;
  std::string compensation;
  const bool restored = Restore(&compensation);
  if (error) {
    *error = failure;
    if (!restored) *error += "; chrony file rollback failed: " + compensation;
  }
  return false;
}

bool ChronyFiles::Restore(std::string* error) {
  // Stop at the first failed restoration. In particular, never restore old
  // keys underneath a new fragment whose compensation was not acknowledged.
  const bool restored =
      removal_ ? (keys_.Rollback(error) && fragment_.Rollback(error))
               : (fragment_.Rollback(error) && keys_.Rollback(error));
  state_ = restored ? State::kRestored : State::kRecoveryPending;
  return restored;
}

bool ChronyFiles::Rollback(std::string* error) {
  if (state_ == State::kRestored) return true;
  if (state_ != State::kApplied && state_ != State::kRecoveryPending)
    return Fail("chrony file transaction has no applied checkpoint", error);
  return Restore(error);
}
}  // namespace dang::ntp

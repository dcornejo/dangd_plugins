// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/** @file Reversible, API-only VPP loopback transaction. */

#include "plugins/vpp/src/loopback_transaction.h"

namespace dang::vpp {

bool LoopbackTransaction::Apply(std::string* error) {
  if (!client_ || !error) return false;
  error->clear();
  if (created_) {
    *error = "the loopback transaction has already been applied";
    return false;
  }
  CreatedInterface candidate;
  if (!client_->CreateLoopback(instance_, &candidate, error)) return false;
  if (candidate.name.empty()) {
    std::string compensation_error;
    if (!client_->DeleteLoopback(candidate.software_index,
                                 &compensation_error)) {
      created_ = candidate;
      *error = "VPP created a loopback without returning its interface name; "
               "compensation failed: " + compensation_error;
      return false;
    }
    *error = "VPP created a loopback without returning its interface name";
    return false;
  }
  if (!client_->SetAdminState(candidate.software_index, true, error)) {
    // Creation has already changed VPP. Always attempt immediate compensation;
    // retain no applied state because a successful delete restores absence.
    std::string compensation_error;
    if (!client_->DeleteLoopback(candidate.software_index,
                                 &compensation_error)) {
      *error += "; compensation failed: " + compensation_error;
      created_ = std::move(candidate);
    }
    return false;
  }
  created_ = std::move(candidate);
  return true;
}

bool LoopbackTransaction::Rollback(std::string* error) {
  if (!client_ || !error) return false;
  error->clear();
  if (!created_) return true;
  std::string down_error;
  if (!client_->SetAdminState(created_->software_index, false, &down_error)) {
    // Delete is still attempted: VPP accepts deletion of an administratively
    // up loopback, and removing it is the exact pre-transaction state.
    *error = "cannot bring the loopback down: " + down_error;
  }
  std::string delete_error;
  if (!client_->DeleteLoopback(created_->software_index, &delete_error)) {
    if (!error->empty()) *error += "; ";
    *error += "cannot delete the loopback: " + delete_error;
    return false;
  }
  created_.reset();
  error->clear();
  return true;
}

}  // namespace dang::vpp

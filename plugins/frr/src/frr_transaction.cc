// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "frr_transaction.h"

#include <string_view>

namespace dang::plugins::frr {
namespace {

void AppendFailure(std::string* destination, std::string_view phase,
                   const std::string& failure) {
  if (!destination || failure.empty()) return;
  if (!destination->empty()) destination->append("; ");
  destination->append(phase).append(": ").append(failure);
}

}  // namespace

bool FrrTransaction::Execute(std::string_view xml, bool apply,
                             std::string* error) {
  if (error) error->clear();
  if (!sessions_) {
    if (error) *error = "FRR mgmtd session factory is missing";
    return false;
  }
  std::string failure;
  auto session = sessions_(&failure);
  if (!session) {
    if (error) *error = "open: " + failure;
    return false;
  }
  bool locked = false;
  bool candidate_changed = false;
  bool committed = false;
  bool operation_ok = false;
  const auto record_failure = [&](std::string_view phase) {
    operation_ok = false;
    AppendFailure(error, phase, failure);
  };
  if (!session->LockCandidate(&failure)) {
    record_failure("lock");
  } else {
    locked = true;
    if (!session->ReplaceCandidate(xpath_, xml, &failure)) {
      record_failure("replace");
    } else {
      candidate_changed = true;
      if (!session->ValidateCandidate(&failure)) {
        record_failure("validate");
      } else if (apply) {
        if (!session->ApplyCandidate(&failure))
          record_failure("apply");
        else {
          committed = true;
          operation_ok = true;
        }
      } else {
        operation_ok = true;
      }
    }
  }

  // Abort means discard candidate edits by restoring candidate from running.
  // It is required after validation-only and every pre-commit failure. Once an
  // apply succeeds, rollback must instead commit the retained before-image.
  if (locked && candidate_changed && !committed) {
    failure.clear();
    if (!session->AbortCandidate(&failure)) record_failure("abort");
  }
  if (locked) {
    failure.clear();
    if (!session->UnlockCandidate(&failure))
      record_failure("unlock");
  }
  failure.clear();
  if (!session->Close(&failure)) record_failure("close");
  if (committed) applied_ = true;
  return operation_ok;
}

bool FrrTransaction::Validate(std::string* error) {
  if (applied_) {
    if (error) *error = "FRR transaction is already applied";
    return false;
  }
  if (!Execute(proposed_xml_, false, error)) return false;
  validated_ = true;
  return true;
}

bool FrrTransaction::Apply(std::string* error) {
  if (!validated_) {
    if (error) *error = "FRR transaction must pass validation before apply";
    return false;
  }
  if (applied_) {
    if (error) *error = "FRR transaction is already applied";
    return false;
  }
  return Execute(proposed_xml_, true, error);
}

bool FrrTransaction::Rollback(std::string* error) {
  if (!applied_) return true;
  const bool restored = Execute(before_xml_, true, error);
  if (restored) applied_ = false;
  return restored;
}

}  // namespace dang::plugins::frr

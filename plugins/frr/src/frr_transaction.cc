// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/**
 * @file
 * Maps dangd's prepare/validate/apply/rollback lifecycle onto isolated FRR
 * mgmtd candidate sessions.  Each phase owns a fresh session so no candidate
 * lock or uncommitted edit can leak across callback boundaries.
 */

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

bool FrrTransaction::Execute(bool before_image, bool apply,
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
    bool edits_ok = true;
    for (const ConfigurationRoot& root : roots_) {
      const auto& xml = before_image ? root.before_xml : root.proposed_xml;
      const bool changed = xml
          ? session->ReplaceCandidate(root.xpath, *xml, &failure)
          : session->DeleteCandidate(root.xpath, &failure);
      if (!changed) {
        record_failure(xml ? "replace" : "delete");
        edits_ok = false;
        break;
      }
      candidate_changed = true;
    }
    if (edits_ok) {
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

  // FRR prohibits running-datastore reads while candidate is locked. Verify a
  // successful commit in a fresh session after the mutation session is fully
  // unlocked and closed. Keep applied_ true on mismatch so the coordinator can
  // still restore the retained before-image.
  if (committed && operation_ok) {
    failure.clear();
    auto verifier = sessions_(&failure);
    if (!verifier) {
      record_failure("verify open");
    } else {
      for (const ConfigurationRoot& root : roots_) {
        auto observed = verifier->GetRunningConfiguration(root.xpath, &failure);
        if (!observed) {
          record_failure("verify " + root.xpath);
          break;
        }
        const std::optional<std::string> observed_root = observed->empty()
            ? std::nullopt
            : std::optional<std::string>(std::move(*observed));
        const auto& expected = before_image ? root.before_xml
                                            : root.proposed_xml;
        if (expected.has_value() != observed_root.has_value()) {
          failure = "FRR running datastore did not retain the committed root";
          record_failure("verify " + root.xpath);
          break;
        }
      }
      failure.clear();
      if (!verifier->Close(&failure)) record_failure("verify close");
    }
  }
  return operation_ok;
}

bool FrrTransaction::Validate(std::string* error) {
  if (applied_) {
    if (error) *error = "FRR transaction is already applied";
    return false;
  }
  if (!Execute(false, false, error)) return false;
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
  return Execute(false, true, error);
}

bool FrrTransaction::Rollback(std::string* error) {
  if (!applied_) return true;
  const bool restored = Execute(true, true, error);
  if (restored) applied_ = false;
  return restored;
}

}  // namespace dang::plugins::frr

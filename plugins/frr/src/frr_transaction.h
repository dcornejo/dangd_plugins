// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANG_PLUGINS_FRR_FRR_TRANSACTION_H_
#define DANG_PLUGINS_FRR_FRR_TRANSACTION_H_

#include "mgmtd_session.h"

#include <functional>
#include <memory>
#include <string>

namespace dang::plugins::frr {

using SessionFactory =
    std::function<std::unique_ptr<mgmtd::SessionOperations>(std::string*)>;

// Retains both snapshots for the complete dangd transaction lifetime. Validate
// uses a disposable candidate session; Apply repeats the checked replacement;
// Rollback commits the retained before-image as a new real transaction.
class FrrTransaction {
 public:
  FrrTransaction(SessionFactory sessions, std::string xpath,
                 std::string before_xml, std::string proposed_xml)
      : sessions_(std::move(sessions)),
        xpath_(std::move(xpath)),
        before_xml_(std::move(before_xml)),
        proposed_xml_(std::move(proposed_xml)) {}

  bool Validate(std::string* error);
  bool Apply(std::string* error);
  bool Rollback(std::string* error);
  bool applied() const { return applied_; }

 private:
  bool Execute(std::string_view xml, bool apply, std::string* error);

  SessionFactory sessions_;
  std::string xpath_;
  std::string before_xml_;
  std::string proposed_xml_;
  bool validated_ = false;
  bool applied_ = false;
};

}  // namespace dang::plugins::frr

#endif  // DANG_PLUGINS_FRR_FRR_TRANSACTION_H_

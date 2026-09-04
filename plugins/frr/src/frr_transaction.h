// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANG_PLUGINS_FRR_FRR_TRANSACTION_H_
#define DANG_PLUGINS_FRR_FRR_TRANSACTION_H_

#include "mgmtd_session.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace dang::plugins::frr {

using SessionFactory =
    std::function<std::unique_ptr<mgmtd::SessionOperations>(std::string*)>;

// Retains both snapshots for the complete dangd transaction lifetime. Validate
// uses a disposable candidate session; Apply repeats the checked replacement;
// Rollback commits the retained before-image as a new real transaction.
struct ConfigurationRoot {
  std::string xpath;
  std::optional<std::string> before_xml;
  std::optional<std::string> proposed_xml;
};

class FrrTransaction {
 public:
  FrrTransaction(SessionFactory sessions, std::vector<ConfigurationRoot> roots)
      : sessions_(std::move(sessions)),
        roots_(std::move(roots)) {}

  bool Validate(std::string* error);
  bool Apply(std::string* error);
  bool Rollback(std::string* error);
  bool applied() const { return applied_; }

 private:
  bool Execute(bool before_image, bool apply, std::string* error);

  SessionFactory sessions_;
  std::vector<ConfigurationRoot> roots_;
  bool validated_ = false;
  bool applied_ = false;
};

}  // namespace dang::plugins::frr

#endif  // DANG_PLUGINS_FRR_FRR_TRANSACTION_H_

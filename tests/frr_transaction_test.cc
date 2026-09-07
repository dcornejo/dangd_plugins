// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "plugins/frr/src/frr_transaction.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {
using dang::plugins::frr::FrrTransaction;
using dang::plugins::frr::ConfigurationRoot;
using dang::plugins::frr::mgmtd::SessionOperations;

class FakeSession final : public SessionOperations {
 public:
  FakeSession(std::vector<std::string>* calls, std::string failure,
              bool drop_commit, std::string* running_xml)
      : calls_(calls), failure_(std::move(failure)),
        drop_commit_(drop_commit), running_xml_(running_xml) {}

  bool LockCandidate(std::string* error) override {
    return Call("lock", error);
  }
  bool ReplaceCandidate(std::string_view xpath, std::string_view xml,
                        std::string* error) override {
    calls_->push_back("replace " + std::string(xpath) + " " + std::string(xml));
    pending_xml_ = std::string(xml);
    return Result("replace", error);
  }
  bool DeleteCandidate(std::string_view xpath, std::string* error) override {
    calls_->push_back("delete " + std::string(xpath));
    pending_xml_.clear();
    return Result("delete", error);
  }
  bool ValidateCandidate(std::string* error) override {
    return Call("validate", error);
  }
  bool ApplyCandidate(std::string* error) override {
    if (!Call("apply", error)) return false;
    if (!drop_commit_) *running_xml_ = pending_xml_;
    return true;
  }
  bool AbortCandidate(std::string* error) override {
    return Call("abort", error);
  }
  std::optional<std::string> GetRunningConfiguration(
      std::string_view xpath, std::string* error) override {
    calls_->push_back("read " + std::string(xpath));
    if (!Result("read", error)) return std::nullopt;
    return *running_xml_;
  }
  bool UnlockCandidate(std::string* error) override {
    return Call("unlock", error);
  }
  bool Close(std::string* error) override { return Call("close", error); }

 private:
  bool Call(std::string name, std::string* error) {
    calls_->push_back(name);
    return Result(name, error);
  }
  bool Result(std::string_view name, std::string* error) const {
    if (failure_ != name) return true;
    if (error) *error = "injected " + std::string(name) + " failure";
    return false;
  }

  std::vector<std::string>* calls_;
  std::string failure_;
  bool drop_commit_ = false;
  std::string pending_xml_;
  std::string* running_xml_;
};

struct FakeSessions {
  std::vector<std::string> calls;
  std::vector<std::string> failures;
  std::vector<bool> drop_commits;
  std::string running_xml;
  std::size_t created = 0;

  std::unique_ptr<SessionOperations> Create(std::string*) {
    calls.push_back("open " + std::to_string(created));
    const std::string failure =
        created < failures.size() ? failures[created] : std::string{};
    const bool drop_commit =
        created < drop_commits.size() ? drop_commits[created] : false;
    ++created;
    return std::make_unique<FakeSession>(&calls, failure, drop_commit,
                                         &running_xml);
  }
};

TEST(FrrTransactionTest, ValidatesDisposablyThenAppliesAndRestoresBeforeImage) {
  FakeSessions sessions;
  FrrTransaction transaction(
      [&](std::string* error) { return sessions.Create(error); },
      {ConfigurationRoot{"/frr-routing:routing",
                         "<routing><before/></routing>",
                         "<routing><after/></routing>"}});
  std::string error;
  EXPECT_TRUE(transaction.Validate(&error)) << error;
  EXPECT_FALSE(transaction.applied());
  EXPECT_TRUE(transaction.Apply(&error)) << error;
  EXPECT_TRUE(transaction.applied());
  EXPECT_TRUE(transaction.Rollback(&error)) << error;
  EXPECT_FALSE(transaction.applied());

  EXPECT_EQ(sessions.calls,
            std::vector<std::string>({
                "open 0", "lock",
                "replace /frr-routing:routing <routing><after/></routing>",
                "validate", "abort", "unlock", "close", "open 1", "lock",
                "replace /frr-routing:routing <routing><after/></routing>",
                "validate", "apply", "unlock", "close", "open 2",
                "read /frr-routing:routing", "close", "open 3", "lock",
                "replace /frr-routing:routing <routing><before/></routing>",
                "validate", "apply", "unlock", "close", "open 4",
                "read /frr-routing:routing", "close"}));
}

TEST(FrrTransactionTest, FailedValidationAbortsAndPreventsApply) {
  FakeSessions sessions;
  sessions.failures = {"validate"};
  FrrTransaction transaction(
      [&](std::string* error) { return sessions.Create(error); },
      {ConfigurationRoot{"/root", "<before/>", "<after/>"}});
  std::string error;
  EXPECT_FALSE(transaction.Validate(&error));
  EXPECT_NE(error.find("validate"), std::string::npos);
  EXPECT_FALSE(transaction.Apply(&error));
  EXPECT_NE(error.find("must pass validation"), std::string::npos);
  EXPECT_EQ(sessions.calls,
            std::vector<std::string>({"open 0", "lock",
                                      "replace /root <after/>", "validate",
                                      "abort", "unlock", "close"}));
}

TEST(FrrTransactionTest, MissingProposedRootProducesCandidateDelete) {
  FakeSessions sessions;
  FrrTransaction transaction(
      [&](std::string* error) { return sessions.Create(error); },
      {ConfigurationRoot{"/frr-zebra:zebra", "<zebra/>", std::nullopt}});
  std::string error;
  EXPECT_TRUE(transaction.Validate(&error)) << error;
  EXPECT_NE(std::find(sessions.calls.begin(), sessions.calls.end(),
                      "delete /frr-zebra:zebra"),
            sessions.calls.end());
}

TEST(FrrTransactionTest, CommitCleanupFailureKeepsRollbackAvailable) {
  FakeSessions sessions;
  sessions.failures = {"", "unlock", ""};
  FrrTransaction transaction(
      [&](std::string* error) { return sessions.Create(error); },
      {ConfigurationRoot{"/root", "<before/>", "<after/>"}});
  std::string error;
  ASSERT_TRUE(transaction.Validate(&error)) << error;
  EXPECT_FALSE(transaction.Apply(&error));
  EXPECT_TRUE(transaction.applied());
  EXPECT_NE(error.find("unlock"), std::string::npos);
  EXPECT_TRUE(transaction.Rollback(&error)) << error;
  EXPECT_FALSE(transaction.applied());
  EXPECT_NE(std::find(sessions.calls.begin(), sessions.calls.end(),
                      "replace /root <before/>"),
            sessions.calls.end());
}

TEST(FrrTransactionTest, SilentCommitDropFailsWithRollbackStillAvailable) {
  FakeSessions sessions;
  sessions.drop_commits = {false, true, false};
  FrrTransaction transaction(
      [&](std::string* error) { return sessions.Create(error); },
      {ConfigurationRoot{"/frr-bfdd:bfdd", std::nullopt,
                         "<bfdd xmlns=\"http://frrouting.org/yang/bfdd\"/>"}});
  std::string error;
  ASSERT_TRUE(transaction.Validate(&error)) << error;
  EXPECT_FALSE(transaction.Apply(&error));
  EXPECT_TRUE(transaction.applied());
  EXPECT_NE(error.find("verify /frr-bfdd:bfdd"), std::string::npos);
  EXPECT_TRUE(transaction.Rollback(&error)) << error;
  EXPECT_FALSE(transaction.applied());
}

}  // namespace

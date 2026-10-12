// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0
/** @file Tests service ordering and partial native outcomes with real files. */
#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <set>
#include <vector>

#include "plugins/ntp/src/chrony_transaction.h"

namespace dang::ntp {
namespace {
class FakeService final : public ChronyService {
 public:
  bool active = true;
  unsigned calls = 0;
  std::set<unsigned> failures;
  std::vector<bool> requests;
  std::function<void(bool)> inspect;
  bool Active(bool* output, std::string*) override {
    *output = active;
    return true;
  }
  bool SetActive(bool requested, std::string* error) override {
    requests.push_back(requested);
    if (inspect) inspect(requested);
    active =
        requested;  // Errors deliberately include a changed native outcome.
    if (failures.contains(++calls)) {
      *error = "injected service acknowledgement failure";
      return false;
    }
    return true;
  }
};
class NtpChronyTransactionTest : public testing::Test {
 protected:
  void SetUp() override {
    char pattern[] = "/tmp/dang-ntp-service-XXXXXX";
    const char* created = mkdtemp(pattern);
    ASSERT_NE(created, nullptr);
    directory = created;
    fragment = directory + "/managed.conf";
    keys = directory + "/managed.keys";
    {
      std::ofstream out(fragment);
      out << "old fragment";
    }
    {
      std::ofstream out(keys);
      out << "old keys";
    }
  }
  void TearDown() override { std::filesystem::remove_all(directory); }
  std::string Read(const std::string& file) {
    std::ifstream in(file);
    return {std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
  }
  std::string directory, fragment, keys, error;
  FakeService service;
  ChronyConfigImage image{.fragment = "new fragment", .key_file = "new keys"};
};

TEST_F(NtpChronyTransactionTest, StopsBeforeFilesAndStartsAfterBothImages) {
  ChronyTransaction transaction(service);
  ASSERT_TRUE(transaction.Capture(fragment, keys, &error));
  service.inspect = [&](bool requested) {
    EXPECT_EQ(Read(fragment), requested ? "new fragment" : "old fragment");
    EXPECT_EQ(Read(keys), requested ? "new keys" : "old keys");
  };
  ASSERT_TRUE(transaction.Apply(image, &error)) << error;
  EXPECT_TRUE(transaction.applied());
  EXPECT_EQ(service.requests, std::vector<bool>({false, true}));
  service.inspect = {};
  ASSERT_TRUE(transaction.Rollback(&error)) << error;
  EXPECT_EQ(Read(fragment), "old fragment");
  EXPECT_EQ(Read(keys), "old keys");
  EXPECT_TRUE(service.active);
}

TEST_F(NtpChronyTransactionTest,
       ActivationFailureRestoresFilesAndInitialState) {
  ChronyTransaction transaction(service);
  ASSERT_TRUE(transaction.Capture(fragment, keys, &error));
  service.failures = {2};
  EXPECT_FALSE(transaction.Apply(image, &error));
  EXPECT_FALSE(transaction.recovery_pending());
  EXPECT_EQ(service.requests, std::vector<bool>({false, true, false, true}));
  EXPECT_EQ(Read(fragment), "old fragment");
  EXPECT_EQ(Read(keys), "old keys");
  EXPECT_TRUE(service.active);
}

TEST_F(NtpChronyTransactionTest,
       FailedCompensationStopDoesNotRestoreLiveFiles) {
  ChronyTransaction transaction(service);
  ASSERT_TRUE(transaction.Capture(fragment, keys, &error));
  service.failures = {2, 3};
  EXPECT_FALSE(transaction.Apply(image, &error));
  EXPECT_TRUE(transaction.recovery_pending());
  EXPECT_EQ(Read(fragment), "new fragment");
  EXPECT_EQ(Read(keys), "new keys");
  ASSERT_TRUE(transaction.Rollback(&error)) << error;
  EXPECT_FALSE(transaction.recovery_pending());
  EXPECT_TRUE(service.active);
  EXPECT_EQ(Read(fragment), "old fragment");
}

TEST_F(NtpChronyTransactionTest, StopFailureDoesNotInstallAnyFiles) {
  ChronyTransaction transaction(service);
  ASSERT_TRUE(transaction.Capture(fragment, keys, &error));
  service.failures = {1};
  EXPECT_FALSE(transaction.Apply(image, &error));
  EXPECT_EQ(Read(fragment), "old fragment");
  EXPECT_EQ(Read(keys), "old keys");
  EXPECT_TRUE(service.active);
}

TEST_F(NtpChronyTransactionTest, RollbackRestoresInitiallyInactiveService) {
  service.active = false;
  ChronyTransaction transaction(service);
  ASSERT_TRUE(transaction.Capture(fragment, keys, &error));
  ASSERT_TRUE(transaction.Apply(image, &error));
  EXPECT_TRUE(service.active);
  ASSERT_TRUE(transaction.Rollback(&error));
  EXPECT_FALSE(service.active);
  EXPECT_EQ(service.requests, std::vector<bool>({false, true, false}));
}

TEST_F(NtpChronyTransactionTest,
       RemovalKeepsServiceStoppedAndRollbackRestoresIt) {
  ChronyTransaction transaction(service);
  ASSERT_TRUE(transaction.Capture(fragment, keys, &error));
  ASSERT_TRUE(transaction.Apply({}, &error));
  EXPECT_FALSE(service.active);
  EXPECT_FALSE(std::filesystem::exists(fragment));
  ASSERT_TRUE(transaction.Rollback(&error));
  EXPECT_TRUE(service.active);
  EXPECT_EQ(Read(fragment), "old fragment");
}

TEST_F(NtpChronyTransactionTest, FailedRestoreStartAllowsLaterRecovery) {
  ChronyTransaction transaction(service);
  ASSERT_TRUE(transaction.Capture(fragment, keys, &error));
  ASSERT_TRUE(transaction.Apply(image, &error));
  service.failures = {4};
  EXPECT_FALSE(transaction.Rollback(&error));
  EXPECT_TRUE(transaction.recovery_pending());
  EXPECT_EQ(Read(fragment), "old fragment");
  ASSERT_TRUE(transaction.Rollback(&error));
  EXPECT_FALSE(transaction.recovery_pending());
  EXPECT_TRUE(service.active);
}

TEST_F(NtpChronyTransactionTest, FileCompensationFailureKeepsServiceStopped) {
  ChronyTransaction transaction(service);
  ASSERT_TRUE(transaction.Capture(fragment, keys, &error));
  service.failures = {2};
  service.inspect = [&](bool requested) {
    if (requested) {
      std::ofstream out(fragment);
      out << "outside edit";
    }
  };
  EXPECT_FALSE(transaction.Apply(image, &error));
  EXPECT_TRUE(transaction.recovery_pending());
  EXPECT_FALSE(service.active);
  EXPECT_EQ(Read(fragment), "outside edit");
  EXPECT_EQ(Read(keys), "new keys");
  EXPECT_EQ(service.requests, std::vector<bool>({false, true, false}));
}
}  // namespace
}  // namespace dang::ntp

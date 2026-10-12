// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0
/** @file Real filesystem tests for ordered chrony file-pair compensation. */
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>

#include "plugins/ntp/src/chrony_files.h"

namespace dang::ntp {
namespace {
int sync_count;
std::set<int> failing_syncs;
int InjectSync(int fd) {
  if (failing_syncs.contains(++sync_count)) {
    errno = EIO;
    return -1;
  }
  return fsync(fd);
}
class NtpChronyFilesTest : public testing::Test {
 protected:
  void SetUp() override {
    char pattern[] = "/tmp/dang-ntp-pair-XXXXXX";
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
    ASSERT_EQ(chmod(keys.c_str(), 0640), 0);
    sync_count = 0;
    failing_syncs.clear();
  }
  void TearDown() override { std::filesystem::remove_all(directory); }
  std::string Read(const std::string& file) {
    std::ifstream input(file);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
  }
  std::string directory, fragment, keys;
  ChronyConfigImage image{.fragment = "new fragment", .key_file = "new keys"};
};

TEST_F(NtpChronyFilesTest, InstallsAndRestoresBothImages) {
  ChronyFiles transaction;
  std::string error;
  ASSERT_TRUE(transaction.Capture(fragment, keys, &error)) << error;
  ASSERT_TRUE(transaction.Apply(image, &error)) << error;
  EXPECT_TRUE(transaction.applied());
  EXPECT_EQ(Read(fragment), "new fragment");
  EXPECT_EQ(Read(keys), "new keys");
  struct stat state{};
  ASSERT_EQ(stat(keys.c_str(), &state), 0);
  EXPECT_EQ(state.st_mode & 0777, 0600);
  ASSERT_TRUE(transaction.Rollback(&error)) << error;
  EXPECT_EQ(Read(fragment), "old fragment");
  EXPECT_EQ(Read(keys), "old keys");
  ASSERT_EQ(stat(keys.c_str(), &state), 0);
  EXPECT_EQ(state.st_mode & 0777, 0640);
  EXPECT_TRUE(transaction.Rollback(&error));
}

TEST_F(NtpChronyFilesTest, CompensatesFailureAfterBothFilesBecameVisible) {
  ChronyFiles transaction(InjectSync);
  std::string error;
  ASSERT_TRUE(transaction.Capture(fragment, keys, &error));
  failing_syncs = {
      4};  // Fragment directory sync, after key and fragment rename.
  EXPECT_FALSE(transaction.Apply(image, &error));
  EXPECT_FALSE(transaction.recovery_pending());
  EXPECT_FALSE(transaction.applied());
  EXPECT_EQ(Read(fragment), "old fragment");
  EXPECT_EQ(Read(keys), "old keys");
}

TEST_F(NtpChronyFilesTest,
       RetainsCheckpointWhenCompensationFailsAndAllowsRetry) {
  ChronyFiles transaction(InjectSync);
  std::string error;
  ASSERT_TRUE(transaction.Capture(fragment, keys, &error));
  failing_syncs = {
      4, 5};  // Apply directory sync and rollback temporary-file sync.
  EXPECT_FALSE(transaction.Apply(image, &error));
  EXPECT_TRUE(transaction.recovery_pending());
  EXPECT_EQ(Read(fragment), "new fragment");
  EXPECT_EQ(Read(keys), "new keys");
  EXPECT_NE(error.find("rollback failed"), std::string::npos);
  ASSERT_TRUE(transaction.Rollback(&error)) << error;
  EXPECT_FALSE(transaction.recovery_pending());
  EXPECT_EQ(Read(fragment), "old fragment");
  EXPECT_EQ(Read(keys), "old keys");
}

TEST_F(NtpChronyFilesTest, RemovesAndRestoresWithCorrectCompensationOrder) {
  ChronyFiles transaction(InjectSync);
  std::string error;
  ASSERT_TRUE(transaction.Capture(fragment, keys, &error));
  failing_syncs = {2};  // Key removal directory sync, after fragment removal.
  EXPECT_FALSE(transaction.Apply({}, &error));
  EXPECT_FALSE(transaction.recovery_pending());
  EXPECT_EQ(Read(fragment), "old fragment");
  EXPECT_EQ(Read(keys), "old keys");
}

TEST_F(NtpChronyFilesTest, FailedSecondCaptureCannotMutateFirstFile) {
  ASSERT_EQ(unlink(fragment.c_str()), 0);
  ASSERT_EQ(symlink(keys.c_str(), fragment.c_str()), 0);
  ChronyFiles transaction;
  std::string error;
  EXPECT_FALSE(transaction.Capture(fragment, keys, &error));
  EXPECT_FALSE(transaction.Apply(image, &error));
  EXPECT_EQ(Read(keys), "old keys");
}

TEST_F(NtpChronyFilesTest,
       RefusesExternalEditWithoutRestoringKeysUnderNewFragment) {
  ChronyFiles transaction;
  std::string error;
  ASSERT_TRUE(transaction.Capture(fragment, keys, &error));
  ASSERT_TRUE(transaction.Apply(image, &error));
  {
    std::ofstream out(fragment);
    out << "outside edit";
  }
  EXPECT_FALSE(transaction.Rollback(&error));
  EXPECT_TRUE(transaction.recovery_pending());
  EXPECT_EQ(Read(fragment), "outside edit");
  EXPECT_EQ(Read(keys), "new keys");
}
}  // namespace
}  // namespace dang::ntp

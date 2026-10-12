// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0
/** @file Exercises real file replacement and compensation in temporary
 * directories. */
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "plugins/ntp/src/managed_file.h"

namespace dang::ntp {
namespace {
int sync_calls = 0;
int FailFirstDirectorySync(int fd) {
  if (++sync_calls == 2) {
    errno = EIO;
    return -1;
  }
  return fsync(fd);
}
class NtpManagedFileTest : public testing::Test {
 protected:
  void SetUp() override {
    char pattern[] = "/tmp/dang-ntp-file-XXXXXX";
    const char* path = mkdtemp(pattern);
    ASSERT_NE(path, nullptr);
    directory = path;
    file = directory + "/keys";
  }
  void TearDown() override { std::filesystem::remove_all(directory); }
  std::string Contents() {
    std::ifstream input(file);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
  }
  std::string directory;
  std::string file;
};

TEST_F(NtpManagedFileTest, RestoresBytesPermissionsAndOwnership) {
  {
    std::ofstream output(file);
    output << "previous keys";
  }
  ASSERT_EQ(chmod(file.c_str(), 0640), 0);
  struct stat before{};
  ASSERT_EQ(stat(file.c_str(), &before), 0);
  ManagedFile transaction;
  std::string error;
  ASSERT_TRUE(transaction.Capture(file, &error)) << error;
  ASSERT_TRUE(transaction.Install("new keys", 0600, &error)) << error;
  EXPECT_TRUE(transaction.changed());
  EXPECT_EQ(Contents(), "new keys");
  struct stat current{};
  ASSERT_EQ(stat(file.c_str(), &current), 0);
  EXPECT_EQ(current.st_mode & 0777, 0600);
  ASSERT_TRUE(transaction.Rollback(&error)) << error;
  EXPECT_FALSE(transaction.changed());
  EXPECT_EQ(Contents(), "previous keys");
  ASSERT_EQ(stat(file.c_str(), &current), 0);
  EXPECT_EQ(current.st_mode & 0777, 0640);
  EXPECT_EQ(current.st_uid, before.st_uid);
  EXPECT_EQ(current.st_gid, before.st_gid);
  EXPECT_TRUE(transaction.Rollback(&error));
}

TEST_F(NtpManagedFileTest, RestoresAbsenceAndCompensatesRemoval) {
  ManagedFile creation;
  std::string error;
  ASSERT_TRUE(creation.Capture(file, &error)) << error;
  ASSERT_TRUE(creation.Install("keys", 0600, &error)) << error;
  ASSERT_TRUE(creation.Rollback(&error)) << error;
  EXPECT_FALSE(std::filesystem::exists(file));
  {
    std::ofstream output(file);
    output << "old keys";
  }
  ManagedFile removal;
  ASSERT_TRUE(removal.Capture(file, &error));
  ASSERT_TRUE(removal.Install(std::nullopt, 0600, &error));
  EXPECT_FALSE(std::filesystem::exists(file));
  ASSERT_TRUE(removal.Rollback(&error)) << error;
  EXPECT_EQ(Contents(), "old keys");
}

TEST_F(NtpManagedFileTest, RefusesSymlinksHardLinksAndFifos) {
  std::string error;
  const std::string target = directory + "/other";
  {
    std::ofstream output(target);
    output << "unrelated";
  }
  ASSERT_EQ(symlink(target.c_str(), file.c_str()), 0);
  {
    ManagedFile transaction;
    EXPECT_FALSE(transaction.Capture(file, &error));
  }
  ASSERT_EQ(unlink(file.c_str()), 0);
  ASSERT_EQ(link(target.c_str(), file.c_str()), 0);
  {
    ManagedFile transaction;
    EXPECT_FALSE(transaction.Capture(file, &error));
  }
  ASSERT_EQ(unlink(file.c_str()), 0);
  ASSERT_EQ(mkfifo(file.c_str(), 0600), 0);
  {
    ManagedFile transaction;
    EXPECT_FALSE(transaction.Capture(file, &error));
  }
}

TEST_F(NtpManagedFileTest, RefusesToOverwriteAnExternalEdit) {
  ManagedFile transaction;
  std::string error;
  ASSERT_TRUE(transaction.Capture(file, &error));
  ASSERT_TRUE(transaction.Install("managed", 0600, &error));
  {
    std::ofstream output(file);
    output << "external edit";
  }
  EXPECT_FALSE(transaction.Rollback(&error));
  EXPECT_TRUE(transaction.changed());
  EXPECT_EQ(Contents(), "external edit");
  EXPECT_EQ(error, "managed file changed outside this transaction");
}

TEST_F(NtpManagedFileTest, RejectsMutationWithoutCapture) {
  ManagedFile transaction;
  std::string error;
  EXPECT_FALSE(transaction.Install("keys", 0600, &error));
  EXPECT_FALSE(transaction.Rollback(&error));
  EXPECT_FALSE(std::filesystem::exists(file));
}

TEST_F(NtpManagedFileTest, RetainsRollbackAfterPostRenameSyncFailure) {
  {
    std::ofstream output(file);
    output << "original";
  }
  sync_calls = 0;
  ManagedFile transaction(FailFirstDirectorySync);
  std::string error;
  ASSERT_TRUE(transaction.Capture(file, &error));
  EXPECT_FALSE(transaction.Install("replacement", 0600, &error));
  EXPECT_TRUE(transaction.changed());
  EXPECT_EQ(Contents(), "replacement");
  ASSERT_TRUE(transaction.Rollback(&error)) << error;
  EXPECT_EQ(Contents(), "original");
  EXPECT_FALSE(transaction.changed());
}
}  // namespace
}  // namespace dang::ntp

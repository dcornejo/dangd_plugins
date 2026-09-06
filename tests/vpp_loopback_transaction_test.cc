// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "plugins/vpp/src/loopback_transaction.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace dang::vpp {
namespace {

class FakeVppClient final : public VppClient {
 public:
  bool create_ok = true;
  bool admin_up_ok = true;
  bool admin_down_ok = true;
  bool delete_ok = true;
  std::string created_name = "loop0";
  std::vector<std::string> calls;

  bool CreateLoopback(CreatedInterface* created, std::string* error) override {
    calls.push_back("create");
    if (!create_ok) { *error = "injected create failure"; return false; }
    *created = {.software_index = 17, .name = created_name};
    return true;
  }
  bool SetAdminState(uint32_t index, bool up, std::string* error) override {
    calls.push_back(std::string(up ? "up:" : "down:") + std::to_string(index));
    if ((up && !admin_up_ok) || (!up && !admin_down_ok)) {
      *error = "injected admin failure";
      return false;
    }
    return true;
  }
  bool DeleteLoopback(uint32_t index, std::string* error) override {
    calls.push_back("delete:" + std::to_string(index));
    if (!delete_ok) { *error = "injected delete failure"; return false; }
    return true;
  }
};

TEST(VppLoopbackTransaction, AppliesAndRestoresAbsence) {
  FakeVppClient client;
  LoopbackTransaction transaction(&client);
  std::string error;
  ASSERT_TRUE(transaction.Apply(&error)) << error;
  ASSERT_TRUE(transaction.created());
  EXPECT_EQ(transaction.created()->name, "loop0");
  ASSERT_TRUE(transaction.Rollback(&error)) << error;
  EXPECT_FALSE(transaction.created());
  EXPECT_EQ(client.calls, (std::vector<std::string>{
      "create", "up:17", "down:17", "delete:17"}));
}

TEST(VppLoopbackTransaction, CompensatesAdminUpFailure) {
  FakeVppClient client;
  client.admin_up_ok = false;
  LoopbackTransaction transaction(&client);
  std::string error;
  EXPECT_FALSE(transaction.Apply(&error));
  EXPECT_FALSE(transaction.created());
  EXPECT_EQ(client.calls, (std::vector<std::string>{
      "create", "up:17", "delete:17"}));
}

TEST(VppLoopbackTransaction, RetainsHandleWhenCompensationFails) {
  FakeVppClient client;
  client.admin_up_ok = false;
  client.delete_ok = false;
  LoopbackTransaction transaction(&client);
  std::string error;
  EXPECT_FALSE(transaction.Apply(&error));
  EXPECT_TRUE(transaction.created());
  EXPECT_NE(error.find("compensation failed"), std::string::npos);
}

TEST(VppLoopbackTransaction, RetainsUnnamedInterfaceWhenDeletionFails) {
  FakeVppClient client;
  client.created_name.clear();
  client.delete_ok = false;
  LoopbackTransaction transaction(&client);
  std::string error;
  EXPECT_FALSE(transaction.Apply(&error));
  ASSERT_TRUE(transaction.created());
  EXPECT_EQ(transaction.created()->software_index, 17U);
  EXPECT_NE(error.find("compensation failed"), std::string::npos);
}

TEST(VppLoopbackTransaction, RollbackDeletesAfterAdminDownFailure) {
  FakeVppClient client;
  LoopbackTransaction transaction(&client);
  std::string error;
  ASSERT_TRUE(transaction.Apply(&error));
  client.admin_down_ok = false;
  EXPECT_TRUE(transaction.Rollback(&error)) << error;
  EXPECT_FALSE(transaction.created());
  EXPECT_EQ(client.calls.back(), "delete:17");
}

}  // namespace
}  // namespace dang::vpp

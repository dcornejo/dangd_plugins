// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "plugins/vpp/src/loopback_plan.h"

#include <gtest/gtest.h>

namespace dang::vpp {
namespace {

TEST(VppLoopbackPlan, ParsesDefaultsAndExplicitState) {
  constexpr char kXml[] = R"(<config><vpp-interfaces xmlns="urn:dang:vpp:interfaces"><loopback><instance>7</instance></loopback><loopback><instance>9</instance><enabled>true</enabled></loopback></vpp-interfaces></config>)";
  LoopbackConfigurationMap parsed;
  std::string error, path;
  ASSERT_TRUE(ParseLoopbackConfiguration(kXml, &parsed, &error, &path)) << error;
  ASSERT_EQ(parsed.size(), 2U);
  EXPECT_FALSE(parsed.at(7).enabled);
  EXPECT_TRUE(parsed.at(9).enabled);
}

TEST(VppLoopbackPlan, CreatesEverythingBeforeActivation) {
  const LoopbackConfigurationMap proposed{
      {2, {.instance = 2, .enabled = true}},
      {1, {.instance = 1, .enabled = true}}};
  const auto plan = PlanLoopbackChanges({}, proposed);
  ASSERT_EQ(plan.size(), 4U);
  EXPECT_EQ(plan[0].kind, LoopbackOperationKind::kCreate);
  EXPECT_EQ(plan[0].instance, 1U);
  EXPECT_EQ(plan[1].kind, LoopbackOperationKind::kCreate);
  EXPECT_EQ(plan[1].instance, 2U);
  EXPECT_EQ(plan[2].kind, LoopbackOperationKind::kSetAdminState);
  EXPECT_EQ(plan[3].kind, LoopbackOperationKind::kSetAdminState);
}

TEST(VppLoopbackPlan, DeactivatesBeforeDeletion) {
  const LoopbackConfigurationMap before{
      {4, {.instance = 4, .enabled = true}}};
  const auto plan = PlanLoopbackChanges(before, {});
  ASSERT_EQ(plan.size(), 2U);
  EXPECT_EQ(plan[0].kind, LoopbackOperationKind::kSetAdminState);
  EXPECT_FALSE(plan[0].enabled);
  EXPECT_EQ(plan[1].kind, LoopbackOperationKind::kDelete);
}

TEST(VppLoopbackPlan, RejectsDuplicateAndMalformedInstances) {
  constexpr char kDuplicate[] = R"(<vpp-interfaces xmlns="urn:dang:vpp:interfaces"><loopback><instance>3</instance></loopback><loopback><instance>3</instance></loopback></vpp-interfaces>)";
  LoopbackConfigurationMap parsed;
  std::string error, path;
  EXPECT_FALSE(ParseLoopbackConfiguration(kDuplicate, &parsed, &error, &path));
  EXPECT_NE(error.find("duplicated"), std::string::npos);
  constexpr char kMalformed[] = R"(<vpp-interfaces xmlns="urn:dang:vpp:interfaces"><loopback><instance>-1</instance></loopback></vpp-interfaces>)";
  EXPECT_FALSE(ParseLoopbackConfiguration(kMalformed, &parsed, &error, &path));
  EXPECT_NE(path.find("instance"), std::string::npos);
}

}  // namespace
}  // namespace dang::vpp

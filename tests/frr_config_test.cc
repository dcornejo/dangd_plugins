// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "plugins/frr/src/frr_config.h"

#include <gtest/gtest.h>

namespace {

const std::vector<dang::plugins::frr::RootDescriptor> kRoots{
    {"frr-routing", "http://frrouting.org/yang/routing", "routing",
     "/frr-routing:routing"},
    {"frr-zebra", "http://frrouting.org/yang/zebra", "zebra",
     "/frr-zebra:zebra"},
    {"frr-interface", "http://frrouting.org/yang/interface", "lib",
     "/frr-interface:lib"},
    {"frr-ripngd", "http://frrouting.org/yang/ripngd", "ripngd",
     "/frr-ripngd:ripngd"}};

TEST(FrrConfigTest, ExtractsChangedRootsAndPreservesAugmentedNamespaces) {
  const std::string before = R"(<config>
    <routing xmlns="http://frrouting.org/yang/routing">
      <control-plane-protocols/>
    </routing>
  </config>)";
  const std::string proposed = R"(<config>
    <routing xmlns="http://frrouting.org/yang/routing"
             xmlns:s="http://frrouting.org/yang/staticd">
      <control-plane-protocols><s:staticd/></control-plane-protocols>
    </routing>
    <zebra xmlns="http://frrouting.org/yang/zebra"><ip-forwarding>true</ip-forwarding></zebra>
  </config>)";
  std::string error;
  std::string path;
  auto roots = dang::plugins::frr::ExtractConfigurationRoots(
      before, proposed, kRoots, &error, &path);
  ASSERT_TRUE(roots) << error;
  ASSERT_EQ(roots->size(), 2);
  ASSERT_TRUE((*roots)[0].before_xml);
  ASSERT_TRUE((*roots)[0].proposed_xml);
  EXPECT_NE((*roots)[0].proposed_xml->find("staticd"), std::string::npos);
  EXPECT_FALSE((*roots)[1].before_xml);
  ASSERT_TRUE((*roots)[1].proposed_xml);
}

TEST(FrrConfigTest, RepresentsRemovedRootAsDelete) {
  std::string error;
  std::string path;
  auto roots = dang::plugins::frr::ExtractConfigurationRoots(
      R"(<config><zebra xmlns="http://frrouting.org/yang/zebra"/></config>)",
      "<config/>", kRoots, &error, &path);
  ASSERT_TRUE(roots) << error;
  ASSERT_EQ(roots->size(), 1);
  EXPECT_EQ(roots->front().xpath, "/frr-zebra:zebra");
  EXPECT_TRUE(roots->front().before_xml);
  EXPECT_FALSE(roots->front().proposed_xml);
}

TEST(FrrConfigTest, IncludesProtocolAugmentsInsideTheirParentRoot) {
  const std::string proposed = R"(<config>
    <lib xmlns="http://frrouting.org/yang/interface"
         xmlns:rip="http://frrouting.org/yang/ripd">
      <interface><name>eth0</name><rip:rip><rip:v2-broadcast>true</rip:v2-broadcast></rip:rip></interface>
    </lib>
  </config>)";
  std::string error;
  std::string path;
  auto roots = dang::plugins::frr::ExtractConfigurationRoots(
      "<config/>", proposed, kRoots, &error, &path);
  ASSERT_TRUE(roots) << error;
  ASSERT_EQ(roots->size(), 1);
  EXPECT_EQ(roots->front().xpath, "/frr-interface:lib");
  ASSERT_TRUE(roots->front().proposed_xml);
  EXPECT_NE(roots->front().proposed_xml->find("v2-broadcast"),
            std::string::npos);
}

TEST(FrrConfigTest, RejectsMalformedAndDuplicateRootsWithPath) {
  std::string error;
  std::string path;
  EXPECT_FALSE(dang::plugins::frr::ExtractConfigurationRoots(
      "<config>", "<config/>", kRoots, &error, &path));
  EXPECT_NE(error.find("well-formed"), std::string::npos);

  const std::string duplicate = R"(<config>
    <routing xmlns="http://frrouting.org/yang/routing"/>
    <routing xmlns="http://frrouting.org/yang/routing"/>
  </config>)";
  EXPECT_FALSE(dang::plugins::frr::ExtractConfigurationRoots(
      duplicate, "<config/>", kRoots, &error, &path));
  EXPECT_EQ(path, "/frr-routing:routing");
}

TEST(FrrConfigTest, ReplacesOnlyOwnedRootsWithObservedRunningState) {
  const std::string current = R"(<config>
    <unrelated xmlns="urn:example">keep</unrelated>
    <routing xmlns="http://frrouting.org/yang/routing"><old/></routing>
    <zebra xmlns="http://frrouting.org/yang/zebra"><old/></zebra>
  </config>)";
  const std::vector<std::optional<std::string>> observed{
      R"(<routing xmlns="http://frrouting.org/yang/routing"><accepted/></routing>)",
      std::nullopt, std::nullopt, std::nullopt};
  std::string error;
  std::string path;
  auto reconciled = dang::plugins::frr::ReconcileConfigurationRoots(
      current, kRoots, observed, &error, &path);
  ASSERT_TRUE(reconciled) << error;
  EXPECT_NE(reconciled->find("unrelated"), std::string::npos);
  EXPECT_NE(reconciled->find("accepted"), std::string::npos);
  EXPECT_EQ(reconciled->find("<old"), std::string::npos);
  EXPECT_EQ(reconciled->find("yang/zebra"), std::string::npos);
}

TEST(FrrConfigTest, RejectsObservedRootFromWrongModule) {
  std::string error;
  std::string path;
  const std::vector<std::optional<std::string>> observed{
      R"(<zebra xmlns="http://frrouting.org/yang/zebra"/>)", std::nullopt,
      std::nullopt, std::nullopt};
  EXPECT_FALSE(dang::plugins::frr::ReconcileConfigurationRoots(
      "<config/>", kRoots, observed, &error, &path));
  EXPECT_EQ(path, "/frr-routing:routing");
}

TEST(FrrConfigTest, ComparesCanonicalRootsForDrift) {
  std::string error;
  const std::optional<std::string> expected =
      R"(<r:routing xmlns:r="http://frrouting.org/yang/routing"><r:value>1</r:value></r:routing>)";
  const std::optional<std::string> equivalent =
      R"(<routing xmlns="http://frrouting.org/yang/routing">
            <value>1</value>
          </routing>)";
  auto same = dang::plugins::frr::EquivalentConfigurationRoot(
      expected, equivalent, &error);
  ASSERT_TRUE(same) << error;
  EXPECT_TRUE(*same);
  auto changed = dang::plugins::frr::EquivalentConfigurationRoot(
      expected,
      R"(<routing xmlns="http://frrouting.org/yang/routing"><value>2</value></routing>)",
      &error);
  ASSERT_TRUE(changed) << error;
  EXPECT_FALSE(*changed);
  EXPECT_TRUE(*dang::plugins::frr::EquivalentConfigurationRoot(
      std::nullopt, std::nullopt, &error));
}

}  // namespace

// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "plugins/frr/src/frr_config.h"

#include <gtest/gtest.h>

namespace {

const std::vector<dang::plugins::frr::RootDescriptor> kRoots{
    {"frr-routing", "http://frrouting.org/yang/routing", "routing",
     "/frr-routing:routing"},
    {"frr-zebra", "http://frrouting.org/yang/zebra", "zebra",
     "/frr-zebra:zebra"}};

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

}  // namespace

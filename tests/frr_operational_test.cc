// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "plugins/frr/src/frr_operational.h"

#include <gtest/gtest.h>

namespace {
using dang::plugins::frr::AugmentedList;
using dang::plugins::frr::ExtractZebraAugments;
using dang::plugins::frr::OperationalDocument;

TEST(FrrOperationalTest, RetainsKeysAndZebraOwnedAugmentsOnly) {
  const std::string xml = R"xml(
    <lib xmlns="http://frrouting.org/yang/interface">
      <interface><name>eth0</name><vrf>default</vrf>
        <state><mtu>1500</mtu></state>
        <zebra xmlns="http://frrouting.org/yang/zebra">
          <state><up-count>4</up-count></state>
        </zebra>
      </interface>
      <interface><name>eth1</name><vrf>default</vrf>
        <state><mtu>9000</mtu></state>
      </interface>
    </lib>)xml";
  std::string error;
  auto filtered = ExtractZebraAugments(
      xml,
      {"http://frrouting.org/yang/interface", "lib", "interface",
       {"name", "vrf"}},
      &error);
  ASSERT_TRUE(filtered) << error;
  EXPECT_NE(filtered->find("eth0"), std::string::npos);
  EXPECT_NE(filtered->find("up-count"), std::string::npos);
  EXPECT_EQ(filtered->find("eth1"), std::string::npos);
  EXPECT_EQ(filtered->find("mtu"), std::string::npos);
}

TEST(FrrOperationalTest, RejectsWrongRootAndBuildsOneDataEnvelope) {
  std::string error;
  EXPECT_FALSE(ExtractZebraAugments(
      "<wrong xmlns=\"http://frrouting.org/yang/vrf\"/>",
      {"http://frrouting.org/yang/vrf", "lib", "vrf", {"name"}}, &error));
  const std::string document = OperationalDocument({"<zebra/>", "<lib/>"});
  EXPECT_EQ(document,
            "<data xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">"
            "<zebra/><lib/></data>");
}

}  // namespace

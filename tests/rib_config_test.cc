// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "plugins/rib/src/rib_config.h"
#include "plugins/rib/src/platform_command.h"
#include "plugins/rib/src/platform_executor.h"
#include "plugins/rib/src/route_observer.h"
#include "plugins/rib/src/rib_rpc.h"

#include <gtest/gtest.h>

#include <algorithm>

namespace dang::rib {
namespace {

constexpr char kBefore[] = R"xml(<config>
  <routing-instance xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib">
    <name>default</name><rib-list><name>100</name>
    <address-family>ipv4</address-family><route-list>
      <route-index>7</route-index><match><ipv4>
        <dest-ipv4-prefix>192.0.2.0/24</dest-ipv4-prefix>
      </ipv4></match><nexthop><nexthop-base>
        <egress-interface-ipv4-address><outgoing-interface>dummy0</outgoing-interface>
        <ipv4-address>198.51.100.1</ipv4-address></egress-interface-ipv4-address>
      </nexthop-base></nexthop><route-attributes>
        <route-preference>10</route-preference><local-only>false</local-only>
      </route-attributes>
    </route-list></rib-list>
  </routing-instance></config>)xml";

TEST(RibConfigTest, ParsesPortableDestinationRoute) {
  Config config;
  std::string error;
  std::string path;
  ASSERT_TRUE(ParseConfig(kBefore, &config, &error, &path)) << error;
  ASSERT_EQ(config.routes.size(), 1U);
  EXPECT_EQ(config.routes[0].rib, "100");
  EXPECT_EQ(config.routes[0].destination, "192.0.2.0/24");
  EXPECT_EQ(config.routes[0].gateway, "198.51.100.1");
  EXPECT_EQ(config.routes[0].interface, "dummy0");
  EXPECT_EQ(config.routes[0].preference, 10U);
}

TEST(RibConfigTest, RejectsUnsupportedSourceRouteWithAttributedPath) {
  std::string xml(kBefore);
  const auto prefix = xml.find("dest-ipv4-prefix");
  ASSERT_NE(prefix, std::string::npos);
  xml.replace(prefix, std::string("dest-ipv4-prefix").size(),
              "src-ipv4-prefix");
  const auto closing = xml.find("dest-ipv4-prefix");
  ASSERT_NE(closing, std::string::npos);
  xml.replace(closing, std::string("dest-ipv4-prefix").size(),
              "src-ipv4-prefix");
  Config config;
  std::string error;
  std::string path;
  EXPECT_FALSE(ParseConfig(xml.c_str(), &config, &error, &path));
  EXPECT_NE(error.find("destination-prefix"), std::string::npos);
  EXPECT_NE(path.find("/match"), std::string::npos);
}

TEST(RibConfigTest, PlansReplacementAsDeleteThenInstall) {
  Config before;
  std::string error;
  std::string path;
  ASSERT_TRUE(ParseConfig(kBefore, &before, &error, &path));
  Config proposed = before;
  proposed.routes[0].gateway = "198.51.100.2";
  const auto changes = PlanChanges(before, proposed);
  ASSERT_EQ(changes.size(), 2U);
  EXPECT_EQ(changes[0].kind, ChangeKind::kDelete);
  EXPECT_EQ(changes[1].kind, ChangeKind::kInstall);
  EXPECT_EQ(Describe(changes[1]),
            "install ipv4 route 192.0.2.0/24 in RIB 100 via 198.51.100.2 "
            "dev dummy0 preference 10");
}

TEST(RibConfigTest, ProducesShellFreeLinuxAndFreeBsdCommands) {
  Config config;
  std::string error;
  std::string path;
  ASSERT_TRUE(ParseConfig(kBefore, &config, &error, &path));
  const std::vector<Change> changes{{ChangeKind::kInstall, config.routes[0]}};
  std::vector<NativeCommand> commands;
  ASSERT_TRUE(BuildLinuxCommands(changes, &commands, &error, &path)) << error;
  EXPECT_EQ(commands[0].arguments,
            (std::vector<std::string>{"ip", "-4", "route", "replace",
                                      "192.0.2.0/24", "table", "100", "via",
                                      "198.51.100.1", "dev", "dummy0", "metric",
                                      "10", "proto", "static"}));
  ASSERT_TRUE(BuildFreeBsdCommands(changes, &commands, &error, &path)) << error;
  EXPECT_EQ(commands[0].arguments,
            (std::vector<std::string>{"route", "-n", "add", "-inet", "-fib",
                                      "100", "192.0.2.0/24", "198.51.100.1",
                                      "-ifp", "dummy0"}));

  Route directly_connected = config.routes[0];
  directly_connected.gateway.reset();
  EXPECT_FALSE(BuildFreeBsdCommands(
      {{ChangeKind::kInstall, directly_connected}}, &commands, &error, &path));
  EXPECT_NE(error.find("address resolution"), std::string::npos);
}

TEST(RibConfigTest, CompensatesCompletedCommandsInReverseAfterFailure) {
  Config before;
  std::string error;
  std::string path;
  ASSERT_TRUE(ParseConfig(kBefore, &before, &error, &path));
  Config proposed = before;
  proposed.routes[0].gateway = "198.51.100.2";
  const auto changes = PlanChanges(before, proposed);
  std::vector<std::vector<std::string>> observed;
  unsigned invocation = 0;
  const auto result = ExecuteChanges(
      NativePlatform::kLinux, changes,
      [&](const NativeCommand& command, std::string* command_error) {
        observed.push_back(command.arguments);
        ++invocation;
        if (invocation != 2) return true;
        *command_error = "injected installation failure";
        return false;
      });
  EXPECT_FALSE(result.ok);
  EXPECT_TRUE(result.rollback_failures.empty());
  ASSERT_EQ(observed.size(), 3U);
  EXPECT_EQ(observed[0][3], "delete");
  EXPECT_EQ(observed[1][3], "replace");
  EXPECT_EQ(observed[2][3], "replace");
  EXPECT_NE(std::ranges::find(observed[2], "198.51.100.1"), observed[2].end());
}

TEST(RibConfigTest, ReportsIncompleteCompensation) {
  Config before;
  std::string error;
  std::string path;
  ASSERT_TRUE(ParseConfig(kBefore, &before, &error, &path));
  Config proposed = before;
  proposed.routes[0].gateway = "198.51.100.2";
  unsigned invocation = 0;
  const auto result = ExecuteChanges(
      NativePlatform::kFreeBsd, PlanChanges(before, proposed),
      [&](const NativeCommand&, std::string* command_error) {
        ++invocation;
        if (invocation == 1) return true;
        *command_error = invocation == 2 ? "apply failed" : "rollback failed";
        return false;
      });
  EXPECT_FALSE(result.ok);
  ASSERT_EQ(result.rollback_failures.size(), 1U);
  EXPECT_NE(result.rollback_failures[0].find("rollback failed"),
            std::string::npos);
}

TEST(RibConfigTest, SerializesObservedRoutesAsRfc8431State) {
  ObservedRoute observed;
  observed.route = {.routing_instance = "default",
                    .rib = "100",
                    .address_family = "ipv4",
                    .index = 42,
                    .destination = "192.0.2.0/24",
                    .gateway = "198.51.100.1",
                    .interface = "dummy&0",
                    .preference = 10,
                    .local_only = false};
  const std::string xml = SerializeOperationalRoutes({observed});
  EXPECT_NE(xml.find("<route-index>42</route-index>"), std::string::npos);
  EXPECT_NE(xml.find("<route-state>active</route-state>"), std::string::npos);
  EXPECT_NE(xml.find("<route-installed-state>installed</route-installed-state>"),
            std::string::npos);
  EXPECT_NE(xml.find("dummy&amp;0"), std::string::npos);
}

TEST(RibConfigTest, RouteAddReportsMixedBatchResults) {
  constexpr char input[] = R"xml(<route-add xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib">
    <return-failure-detail>true</return-failure-detail><rib-name>100</rib-name>
    <routes>
      <route-list><route-index>7</route-index><match><ipv4><dest-ipv4-prefix>192.0.2.0/24</dest-ipv4-prefix></ipv4></match><route-attributes><route-preference>10</route-preference><local-only>false</local-only></route-attributes><nexthop><nexthop-base><ipv4-address>198.51.100.1</ipv4-address></nexthop-base></nexthop></route-list>
      <route-list><route-index>8</route-index><match><ipv4><dest-ipv4-prefix>198.51.100.0/24</dest-ipv4-prefix></ipv4></match><route-attributes><route-preference>20</route-preference><local-only>false</local-only></route-attributes><nexthop><nexthop-base><ipv4-address>192.0.2.1</ipv4-address></nexthop-base></nexthop></route-list>
    </routes></route-add>)xml";
  unsigned calls = 0;
  std::string output;
  std::string error;
  std::string path;
  ASSERT_TRUE(InvokeRouteAdd(
      NativePlatform::kLinux, input, &output, &error, &path,
      [&](const NativeCommand&, std::string* command_error) {
        ++calls;
        if (calls == 1) return true;
        *command_error = "injected failure";
        return false;
      })) << error;
  EXPECT_NE(output.find("<success-count xmlns="), std::string::npos);
  EXPECT_NE(output.find(">1</success-count>"), std::string::npos);
  EXPECT_NE(output.find(">1</failed-count>"), std::string::npos);
  EXPECT_NE(output.find("<route-index>8</route-index>"), std::string::npos);
  EXPECT_NE(output.find("<error-code>0</error-code>"), std::string::npos);
}

TEST(RibConfigTest, RouteAddRejectsMalformedEnvelope) {
  std::string output;
  std::string error;
  std::string path;
  EXPECT_FALSE(InvokeRouteAdd(NativePlatform::kLinux, "<route-add/>", &output,
                              &error, &path));
  EXPECT_NE(error.find("RFC 8431"), std::string::npos);
  EXPECT_EQ(path, "/ietf-i2rs-rib:route-add");
}

TEST(RibConfigTest, RouteDeleteResolvesObservedRouteAndReportsMissingRoute) {
  constexpr char input[] = R"xml(<route-delete xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib">
    <return-failure-detail>true</return-failure-detail><rib-name>100</rib-name><routes>
      <route-list><route-index>7</route-index><match><ipv4><dest-ipv4-prefix>192.0.2.0/24</dest-ipv4-prefix></ipv4></match></route-list>
      <route-list><route-index>8</route-index><match><ipv4><dest-ipv4-prefix>198.51.100.0/24</dest-ipv4-prefix></ipv4></match></route-list>
    </routes></route-delete>)xml";
  ObservedRoute route;
  route.route = {.routing_instance = "default", .rib = "100",
                 .address_family = "ipv4", .index = 99,
                 .destination = "192.0.2.0/24", .gateway = "192.0.2.1",
                 .interface = "dummy0", .preference = 10};
  std::vector<NativeCommand> commands;
  std::string output;
  std::string error;
  std::string path;
  ASSERT_TRUE(InvokeRouteDelete(
      NativePlatform::kLinux, input, &output, &error, &path,
      [&](const NativeCommand& command, std::string*) {
        commands.push_back(command); return true;
      },
      [&](std::vector<ObservedRoute>* routes, std::string*) {
        *routes = {route}; return true;
      })) << error;
  ASSERT_EQ(commands.size(), 1U);
  EXPECT_EQ(commands.front().arguments[3], "delete");
  EXPECT_NE(output.find(">1</success-count>"), std::string::npos);
  EXPECT_NE(output.find(">1</failed-count>"), std::string::npos);
  EXPECT_NE(output.find("<route-index>8</route-index><error-code>2</error-code>"),
            std::string::npos);
}

TEST(RibConfigTest, RouteDeleteRejectsAmbiguousObservedRoute) {
  constexpr char input[] = R"xml(<route-delete xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib"><return-failure-detail>true</return-failure-detail><rib-name>100</rib-name><routes><route-list><route-index>7</route-index><match><ipv4><dest-ipv4-prefix>192.0.2.0/24</dest-ipv4-prefix></ipv4></match></route-list></routes></route-delete>)xml";
  ObservedRoute route;
  route.route = {.routing_instance = "default", .rib = "100",
                 .address_family = "ipv4", .destination = "192.0.2.0/24",
                 .gateway = std::nullopt, .interface = std::nullopt};
  std::string output;
  std::string error;
  std::string path;
  ASSERT_TRUE(InvokeRouteDelete(
      NativePlatform::kLinux, input, &output, &error, &path,
      [](const NativeCommand&, std::string*) { return true; },
      [&](std::vector<ObservedRoute>* routes, std::string*) {
        *routes = {route, route}; return true;
      }));
  EXPECT_NE(output.find("<error-code>0</error-code>"), std::string::npos);
}

TEST(RibConfigTest, RouteUpdateReplacesAttributesTransactionally) {
  constexpr char input[] = R"xml(<route-update xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib"><return-failure-detail>true</return-failure-detail><rib-name>100</rib-name><input-routes><route-list><route-index>7</route-index><match><ipv4><dest-ipv4-prefix>192.0.2.0/24</dest-ipv4-prefix></ipv4></match><updated-route-attr><route-preference>20</route-preference><local-only>true</local-only></updated-route-attr></route-list></input-routes></route-update>)xml";
  ObservedRoute route;
  route.route = {.routing_instance = "default", .rib = "100",
                 .address_family = "ipv4", .index = 99,
                 .destination = "192.0.2.0/24", .gateway = "192.0.2.1",
                 .interface = "dummy0", .preference = 10};
  std::vector<NativeCommand> commands;
  std::string output;
  std::string error;
  std::string path;
  ASSERT_TRUE(InvokeRouteUpdate(
      NativePlatform::kLinux, input, &output, &error, &path,
      [&](const NativeCommand& command, std::string*) {
        commands.push_back(command); return true;
      },
      [&](std::vector<ObservedRoute>* routes, std::string*) {
        *routes = {route}; return true;
      })) << error;
  ASSERT_EQ(commands.size(), 2U);
  EXPECT_EQ(commands[0].arguments[3], "delete");
  EXPECT_EQ(commands[1].arguments[3], "replace");
  EXPECT_NE(std::ranges::find(commands[1].arguments, "20"),
            commands[1].arguments.end());
  EXPECT_NE(output.find(">1</success-count>"), std::string::npos);
}

TEST(RibConfigTest, RouteUpdateRestoresOriginalAfterInstallFailure) {
  constexpr char input[] = R"xml(<route-update xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib"><rib-name>100</rib-name><input-routes><route-list><route-index>7</route-index><match><ipv4><dest-ipv4-prefix>192.0.2.0/24</dest-ipv4-prefix></ipv4></match><updated-route-attr><route-preference>20</route-preference><local-only>false</local-only></updated-route-attr></route-list></input-routes></route-update>)xml";
  ObservedRoute route;
  route.route = {.routing_instance = "default", .rib = "100",
                 .address_family = "ipv4", .destination = "192.0.2.0/24",
                 .gateway = "192.0.2.1", .interface = "dummy0",
                 .preference = 10};
  unsigned calls = 0;
  std::string output;
  std::string error;
  std::string path;
  ASSERT_TRUE(InvokeRouteUpdate(
      NativePlatform::kLinux, input, &output, &error, &path,
      [&](const NativeCommand&, std::string* command_error) {
        ++calls;
        if (calls != 2U) return true;
        *command_error = "injected update failure"; return false;
      },
      [&](std::vector<ObservedRoute>* routes, std::string*) {
        *routes = {route}; return true;
      }));
  EXPECT_EQ(calls, 3U);
  EXPECT_NE(output.find(">1</failed-count>"), std::string::npos);
}

TEST(RibConfigTest, RibAddValidatesLogicalNamespaceAndRejectsRpf) {
  std::string output;
  std::string error;
  std::string path;
  ASSERT_TRUE(InvokeRibAdd(NativePlatform::kLinux,
      R"(<rib-add xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib"><name>100</name><address-family>ipv4</address-family></rib-add>)",
      &output, &error, &path));
  EXPECT_NE(output.find(">true</result>"), std::string::npos);
  ASSERT_TRUE(InvokeRibAdd(NativePlatform::kLinux,
      R"(<rib-add xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib"><name>100</name><address-family>ipv4</address-family><ip-rpf-check>true</ip-rpf-check></rib-add>)",
      &output, &error, &path));
  EXPECT_NE(output.find(">false</result>"), std::string::npos);
  EXPECT_NE(output.find("RPF"), std::string::npos);
}

TEST(RibConfigTest, RibDeleteRestoresEarlierRoutesAfterFailure) {
  ObservedRoute first;
  first.route = {.routing_instance = "default", .rib = "100",
                 .address_family = "ipv4", .destination = "192.0.2.0/24",
                 .gateway = "192.0.2.1", .interface = "dummy0"};
  ObservedRoute second = first;
  second.route.destination = "198.51.100.0/24";
  unsigned calls = 0;
  std::string output;
  std::string error;
  std::string path;
  ASSERT_TRUE(InvokeRibDelete(
      NativePlatform::kLinux,
      R"(<rib-delete xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib"><name>100</name></rib-delete>)",
      &output, &error, &path,
      [&](const NativeCommand&, std::string* command_error) {
        ++calls;
        if (calls != 2U) return true;
        *command_error = "injected delete failure"; return false;
      },
      [&](std::vector<ObservedRoute>* routes, std::string*) {
        *routes = {first, second}; return true;
      }));
  EXPECT_EQ(calls, 3U);
  EXPECT_NE(output.find(">false</result>"), std::string::npos);
}

}  // namespace
}  // namespace dang::rib

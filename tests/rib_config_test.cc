// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "plugins/rib/src/rib_config.h"
#include "plugins/rib/src/platform_command.h"
#include "plugins/rib/src/platform_executor.h"
#include "plugins/rib/src/route_observer.h"
#include "plugins/rib/src/rib_rpc.h"
#include "plugins/rib/src/rib_persistence.h"
#include "plugins/rib/src/rib_mapping.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace dang::rib {
namespace {

TEST(RibConfigTest, MapsArbitraryRibNamesBidirectionallyPerPlatform) {
  RibMapping mapping;
  std::string error;
  ASSERT_TRUE(mapping.Add("blue", NativePlatform::kLinux, 100, &error));
  ASSERT_TRUE(mapping.Add("blue", NativePlatform::kFreeBsd, 2, &error));
  EXPECT_EQ(mapping.ToNative("blue", NativePlatform::kLinux), "100");
  EXPECT_EQ(mapping.ToNative("blue", NativePlatform::kFreeBsd), "2");
  EXPECT_EQ(mapping.ToModeled("100", NativePlatform::kLinux), "blue");
  EXPECT_EQ(mapping.ToModeled("2", NativePlatform::kFreeBsd), "blue");
  EXPECT_EQ(mapping.NativeNumbers(NativePlatform::kLinux),
            std::vector<std::uint32_t>({100U}));
  EXPECT_EQ(mapping.NativeNumbers(NativePlatform::kFreeBsd),
            std::vector<std::uint32_t>({2U}));
  EXPECT_EQ(mapping.ToNative("77", NativePlatform::kLinux), "77");
  EXPECT_FALSE(mapping.ToNative("missing", NativePlatform::kLinux));
  EXPECT_FALSE(mapping.Add("red", NativePlatform::kLinux, 100, &error));
  EXPECT_NE(error.find("multiple modeled names"), std::string::npos);
}

TEST(RibConfigTest, LoadsVersionedRibMappingAndRejectsAmbiguousReverseNames) {
  const auto path = std::filesystem::temp_directory_path() /
                    ("dang-rib-map-" + std::to_string(getpid()) + ".json");
  {
    std::ofstream output(path);
    output << R"({"version":1,"ribs":[{"name":"blue","linux-table":100,"freebsd-fib":2}]})";
  }
  RibMapping mapping;
  std::string error;
  ASSERT_TRUE(LoadRibMapping(path, &mapping, &error)) << error;
  EXPECT_EQ(mapping.ToNative("blue", NativePlatform::kLinux), "100");
  {
    std::ofstream output(path);
    output << R"({"version":1,"ribs":[{"name":"blue","linux-table":100},{"name":"red","linux-table":100}]})";
  }
  EXPECT_FALSE(LoadRibMapping(path, &mapping, &error));
  EXPECT_NE(error.find("multiple modeled names"), std::string::npos);
  std::filesystem::remove(path);
}

constexpr char kBefore[] = R"xml(<config>
  <routing-instance xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib">
    <name>default</name><rib-list><name>100</name>
    <address-family>ipv4-address-family</address-family><route-list>
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

TEST(RibConfigTest, RejectsUnmappedRoutingInstanceInsteadOfUsingDefault) {
  std::string xml(kBefore);
  const std::string modeled_name = "<name>default</name>";
  const auto position = xml.find(modeled_name);
  ASSERT_NE(position, std::string::npos);
  xml.replace(position, modeled_name.size(), "<name>tenant-blue</name>");
  Config config;
  std::string error;
  std::string path;
  EXPECT_FALSE(ParseConfig(xml.c_str(), &config, &error, &path));
  EXPECT_NE(error.find("VRF or VNET"), std::string::npos);
  EXPECT_EQ(path, "/ietf-i2rs-rib:routing-instance/name");
  EXPECT_TRUE(config.routes.empty());
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
  ASSERT_TRUE(BuildFreeBsdCommands(
      {{ChangeKind::kInstall, directly_connected}}, &commands, &error, &path,
      [](const std::string& interface, const std::string& family,
         std::string* address, std::string*) {
        if (interface != "dummy0" || family != "ipv4") return false;
        *address = "198.51.100.254";
        return true;
      })) << error;
  EXPECT_EQ(commands[0].arguments,
            (std::vector<std::string>{"route", "-n", "add", "-inet", "-fib",
                                      "100", "192.0.2.0/24", "198.51.100.254",
                                      "-ifp", "dummy0"}));

  EXPECT_FALSE(BuildFreeBsdCommands(
      {{ChangeKind::kInstall, directly_connected}}, &commands, &error, &path,
      [](const std::string&, const std::string&, std::string*,
         std::string* why) {
        *why = "outgoing interface has multiple usable local ipv4 addresses";
        return false;
      }));
  EXPECT_NE(error.find("multiple usable"), std::string::npos);
  EXPECT_NE(path.find("/nexthop"), std::string::npos);
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
                    .nexthop_ref = std::nullopt,
                    .preference = 10,
                    .local_only = false};
  const std::string xml = SerializeOperationalRoutes({observed});
  EXPECT_NE(xml.find("<route-index>42</route-index>"), std::string::npos);
  EXPECT_NE(xml.find("<route-state>active</route-state>"), std::string::npos);
  EXPECT_NE(xml.find("<route-installed-state>installed</route-installed-state>"),
            std::string::npos);
  EXPECT_NE(xml.find("dummy&amp;0"), std::string::npos);
  EXPECT_NE(xml.find("<egress-interface-ipv4-address>"), std::string::npos);
  EXPECT_NE(xml.find("</egress-interface-ipv4-address>"), std::string::npos);
  EXPECT_EQ(xml.find("</ipv4-address><outgoing-interface>"),
            std::string::npos);
}

TEST(RibConfigTest, SerializesCombinedIpv6NexthopAsOneChoiceCase) {
  ObservedRoute observed;
  observed.route = {.routing_instance = "default",
                    .rib = "200",
                    .address_family = "ipv6",
                    .index = 43,
                    .destination = "2001:db8:1::/64",
                    .gateway = "2001:db8::1",
                    .interface = "dummy0",
                    .nexthop_ref = std::nullopt,
                    .preference = 20,
                    .local_only = true};
  const std::string xml = SerializeOperationalRoutes({observed});
  EXPECT_NE(xml.find("<egress-interface-ipv6-address>"), std::string::npos);
  EXPECT_NE(xml.find("<ipv6-address>2001:db8::1</ipv6-address>"),
            std::string::npos);
  EXPECT_NE(xml.find("</egress-interface-ipv6-address>"), std::string::npos);
  EXPECT_NE(xml.find("<local-only>true</local-only>"), std::string::npos);
}

TEST(RibConfigTest, SerializesNativeSpecialNexthopIdentity) {
  ObservedRoute observed;
  observed.route = {.routing_instance = "default",
                    .rib = "255",
                    .address_family = "ipv4",
                    .index = 44,
                    .destination = "192.0.2.7/32",
                    .preference = 0,
                    .local_only = true,
                    .special = "receive"};
  observed.mutable_route = false;
  const std::string xml = SerializeOperationalRoutes({observed});
  EXPECT_NE(xml.find("<special>receive</special>"), std::string::npos);
  EXPECT_EQ(xml.find("<outgoing-interface>"), std::string::npos);
}

TEST(RibConfigTest, SerializesInstalledAndRemovedRouteNotifications) {
  Route route{.routing_instance = "default",
              .rib = "100&blue",
              .address_family = "ipv6",
              .index = 42,
              .destination = "2001:db8::/64"};
  const std::string installed = SerializeRouteChange(route, true);
  EXPECT_NE(installed.find("<route-change xmlns="), std::string::npos);
  EXPECT_NE(installed.find("<rib-name>100&amp;blue</rib-name>"),
            std::string::npos);
  EXPECT_NE(installed.find("<dest-ipv6-prefix>2001:db8::/64"),
            std::string::npos);
  EXPECT_NE(installed.find("<route-installed-state>installed"),
            std::string::npos);
  EXPECT_NE(installed.find("<route-state>active"), std::string::npos);
  const std::string removed = SerializeRouteChange(route, false);
  EXPECT_NE(removed.find("<route-installed-state>uninstalled"),
            std::string::npos);
  EXPECT_NE(removed.find("<route-state>inactive"), std::string::npos);
}

TEST(RibConfigTest, TracksExternalRouteChangesWithoutInitialFlood) {
  Route first{.routing_instance = "default",
              .rib = "100",
              .address_family = "ipv4",
              .index = 1,
              .destination = "192.0.2.0/24",
              .gateway = "192.0.2.1"};
  Route second = first;
  second.index = 2;
  second.destination = "198.51.100.0/24";
  RouteChangeTracker tracker;
  EXPECT_TRUE(tracker.Observe({{first, true}}).empty());
  const auto added = tracker.Observe({{first, true}, {second, true}});
  ASSERT_EQ(added.size(), 1U);
  EXPECT_EQ(added[0].route.destination, second.destination);
  EXPECT_TRUE(added[0].installed);

  second.gateway = "198.51.100.1";
  const auto changed = tracker.Observe({{first, true}, {second, true}});
  ASSERT_EQ(changed.size(), 2U);
  EXPECT_FALSE(changed[0].installed);
  EXPECT_TRUE(changed[1].installed);
  EXPECT_EQ(changed[1].route.gateway, second.gateway);

  const auto removed = tracker.Observe({{second, true}});
  ASSERT_EQ(removed.size(), 1U);
  EXPECT_EQ(removed[0].route.destination, first.destination);
  EXPECT_FALSE(removed[0].installed);
}

TEST(RibConfigTest, ManagedChangesAdvanceExternalNotificationBaseline) {
  Route route{.routing_instance = "default",
              .rib = "100",
              .address_family = "ipv4",
              .index = 1,
              .destination = "192.0.2.0/24",
              .gateway = "192.0.2.1"};
  RouteChangeTracker tracker;
  EXPECT_TRUE(tracker.Observe({}).empty());
  tracker.ApplyManaged(route, true);
  Route observed = route;
  observed.index = 0x123456789abcdef0ULL;
  EXPECT_TRUE(tracker.Observe({{observed, true}}).empty());
  tracker.ApplyManaged(route, false);
  EXPECT_TRUE(tracker.Observe({}).empty());
}

TEST(RibConfigTest, TracksSamePrefixMultipathRoutesIndependently) {
  Route first{.routing_instance = "default",
              .rib = "100",
              .address_family = "ipv4",
              .index = 1,
              .destination = "192.0.2.0/24",
              .gateway = "198.51.100.1",
              .interface = "dummy0"};
  Route second = first;
  second.index = 2;
  second.gateway = "198.51.100.2";
  RouteChangeTracker tracker;
  EXPECT_TRUE(tracker.Observe({{first, true}, {second, true}}).empty());
  const auto removed = tracker.Observe({{second, true}});
  ASSERT_EQ(removed.size(), 1U);
  EXPECT_EQ(removed[0].route.gateway, first.gateway);
  EXPECT_FALSE(removed[0].installed);
  EXPECT_TRUE(tracker.Observe({{second, true}}).empty());
}

TEST(RibConfigTest, TracksObservedReusableNexthopResolutionTransitions) {
  PersistentRegistry registry;
  registry.nexthops.push_back(
      {"100", 7, "192.0.2.1", "dummy&0", "ipv4", true});
  registry.bindings.push_back({"100", "ipv4", "198.51.100.0/24", 1, 7});
  Route route{.routing_instance = "default",
              .rib = "100",
              .address_family = "ipv4",
              .index = 1,
              .destination = "198.51.100.0/24",
              .gateway = "192.0.2.1",
              .interface = "dummy&0"};
  NexthopResolutionTracker tracker;
  EXPECT_TRUE(tracker.Observe(registry, {}).empty());
  const auto resolved = tracker.Observe(registry, {{route, true}});
  ASSERT_EQ(resolved.size(), 1U);
  EXPECT_TRUE(resolved[0].resolved);
  const std::string xml = SerializeNexthopResolutionChange(
      resolved[0].nexthop, resolved[0].resolved);
  EXPECT_NE(xml.find("<nexthop-id>7</nexthop-id>"), std::string::npos);
  EXPECT_NE(xml.find("dummy&amp;0"), std::string::npos);
  EXPECT_NE(xml.find("<nexthop-state>resolved</nexthop-state>"),
            std::string::npos);
  EXPECT_TRUE(tracker.Observe(registry, {{route, true}}).empty());
  const auto unresolved = tracker.Observe(registry, {});
  ASSERT_EQ(unresolved.size(), 1U);
  EXPECT_FALSE(unresolved[0].resolved);
}

TEST(RibConfigTest, DoesNotResolveNexthopFromDifferentInstalledPath) {
  PersistentRegistry registry;
  registry.nexthops.push_back(
      {"100", 7, "192.0.2.1", "dummy0", "ipv4", true});
  registry.bindings.push_back({"100", "ipv4", "198.51.100.0/24", 2, 7});
  Route other_path{.routing_instance = "default",
                   .rib = "100",
                   .address_family = "ipv4",
                   .index = 2,
                   .destination = "198.51.100.0/24",
                   .gateway = "192.0.2.2",
                   .interface = "dummy1"};
  NexthopResolutionTracker tracker;
  EXPECT_TRUE(tracker.Observe(registry, {{other_path, true}}).empty());

  Route matching_path = other_path;
  matching_path.index = 3;
  matching_path.gateway = "192.0.2.1";
  matching_path.interface = "dummy0";
  const auto resolved = tracker.Observe(registry, {{other_path, true},
                                                   {matching_path, true}});
  ASSERT_EQ(resolved.size(), 1U);
  EXPECT_TRUE(resolved[0].resolved);
}

TEST(RibConfigTest, IncludesDatastoreRoutesInResolutionState) {
  NexthopRegistry registry;
  ASSERT_EQ(registry.Add({.rib = "100",
                          .gateway = "192.0.2.1",
                          .address_family = "ipv4"}), 1U);
  Route route{.routing_instance = "default",
              .rib = "100",
              .address_family = "ipv4",
              .index = 1,
              .destination = "198.51.100.0/24",
              .nexthop_ref = 1};
  ASSERT_TRUE(registry.ReplaceConfigurationRouteBindings({route}));
  const PersistentRegistry state = registry.ResolutionState();
  ASSERT_EQ(state.bindings.size(), 1U);
  EXPECT_EQ(state.bindings[0].nexthop_id, 1U);
  const PersistentRegistry checkpoint = registry.PersistentState();
  std::string restore_error;
  ASSERT_TRUE(registry.ReplacePersistentState(checkpoint, &restore_error))
      << restore_error;
  ASSERT_EQ(registry.ResolutionState().bindings.size(), 1U);
  ASSERT_TRUE(registry.ReplaceConfigurationRouteBindings({}));
  EXPECT_TRUE(registry.ResolutionState().bindings.empty());
}

TEST(RibConfigTest, SerializesRegisteredNexthopsWithAndWithoutRoutes) {
  const std::vector<std::tuple<std::string, std::string, std::uint32_t>> refs{
      {"100", "ipv4", 7}, {"200", "ipv6", 9}};
  const std::string xml = SerializeOperationalRoutes({}, refs);
  EXPECT_NE(xml.find("<name>100</name><address-family>ipv4-address-family</address-family>"),
            std::string::npos);
  EXPECT_NE(xml.find("<nexthop-member-id>7</nexthop-member-id>"),
            std::string::npos);
  EXPECT_NE(xml.find("<name>200</name><address-family>ipv6-address-family</address-family>"),
            std::string::npos);
  EXPECT_NE(xml.find("<nexthop-member-id>9</nexthop-member-id>"),
            std::string::npos);
}

TEST(RibConfigTest, OmitsFamilyUnknownNexthopWithoutContainingRib) {
  const std::string xml = SerializeOperationalRoutes({}, {{"100", "", 7}});
  EXPECT_EQ(xml.find("<nexthop-member-id>7</nexthop-member-id>"),
            std::string::npos);
}

TEST(RibConfigTest, RouteAddReportsMixedBatchResults) {
  constexpr char input[] = R"xml(<route-add xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib">
    <return-failure-detail>true</return-failure-detail><rib-name>100</rib-name>
    <routes>
      <route-list><route-index>7</route-index><match><ipv4><dest-ipv4-prefix>192.0.2.0/24</dest-ipv4-prefix></ipv4></match><route-attributes><route-preference>10</route-preference><local-only>false</local-only></route-attributes><nexthop><nexthop-base><ipv4-address>198.51.100.1</ipv4-address></nexthop-base></nexthop></route-list>
      <route-list><route-index>8</route-index><match><ipv4><dest-ipv4-prefix>198.51.100.0/24</dest-ipv4-prefix></ipv4></match><route-attributes><route-preference>20</route-preference><local-only>false</local-only></route-attributes><nexthop><nexthop-base><ipv4-address>192.0.2.1</ipv4-address></nexthop-base></nexthop></route-list>
    </routes></route-add>)xml";
  unsigned calls = 0;
  std::vector<Route> events;
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
      }, {}, nullptr, {},
      [&](const Route& route, bool installed) {
        if (installed) events.push_back(route);
      })) << error;
  EXPECT_NE(output.find("<success-count xmlns="), std::string::npos);
  EXPECT_NE(output.find(">1</success-count>"), std::string::npos);
  EXPECT_NE(output.find(">1</failed-count>"), std::string::npos);
  EXPECT_NE(output.find("<route-index>8</route-index>"), std::string::npos);
  EXPECT_NE(output.find("<error-code>0</error-code>"), std::string::npos);
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].index, 7U);
}

TEST(RibConfigTest, RouteRpcUsesNativeMappingAndPreservesModeledEventName) {
  std::vector<NativeCommand> commands;
  std::vector<Route> events;
  std::string output;
  std::string error;
  std::string path;
  ASSERT_TRUE(InvokeRouteAdd(
      NativePlatform::kLinux,
      R"(<route-add xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib"><rib-name>blue</rib-name><routes><route-list><route-index>7</route-index><match><ipv4><dest-ipv4-prefix>192.0.2.0/24</dest-ipv4-prefix></ipv4></match><route-attributes><route-preference>10</route-preference><local-only>false</local-only></route-attributes><nexthop><nexthop-base><ipv4-address>198.51.100.1</ipv4-address></nexthop-base></nexthop></route-list></routes></route-add>)",
      &output, &error, &path,
      [&](const NativeCommand& command, std::string*) {
        commands.push_back(command);
        return true;
      },
      {}, nullptr, {},
      [&](const Route& route, bool installed) {
        if (installed) events.push_back(route);
      },
      [](const std::string& name) -> std::optional<std::string> {
        return name == "blue" ? std::optional<std::string>("100")
                              : std::nullopt;
      })) << error;
  ASSERT_EQ(commands.size(), 1U);
  EXPECT_NE(std::ranges::find(commands[0].arguments, "100"),
            commands[0].arguments.end());
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].rib, "blue");
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
                 .interface = "dummy0", .nexthop_ref = std::nullopt,
                 .preference = 10};
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
                 .gateway = std::nullopt, .interface = std::nullopt,
                 .nexthop_ref = std::nullopt};
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

TEST(RibConfigTest, RouteDeleteRefusesObservedKernelOwnedSpecialRoute) {
  ObservedRoute route;
  route.route = {.routing_instance = "default", .rib = "255",
                 .address_family = "ipv4", .index = 9,
                 .destination = "192.0.2.7/32", .preference = 0,
                 .local_only = true, .special = "receive"};
  route.mutable_route = false;
  unsigned commands = 0;
  std::string output;
  std::string error;
  std::string path;
  ASSERT_TRUE(InvokeRouteDelete(
      NativePlatform::kLinux,
      R"(<route-delete xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib"><return-failure-detail>true</return-failure-detail><rib-name>255</rib-name><routes><route-list><route-index>9</route-index><match><ipv4><dest-ipv4-prefix>192.0.2.7/32</dest-ipv4-prefix></ipv4></match></route-list></routes></route-delete>)",
      &output, &error, &path,
      [&](const NativeCommand&, std::string*) {
        ++commands;
        return true;
      },
      [&](std::vector<ObservedRoute>* routes, std::string*) {
        *routes = {route};
        return true;
      })) << error;
  EXPECT_EQ(commands, 0U);
  EXPECT_NE(output.find("<error-code>0</error-code>"), std::string::npos);
}

TEST(RibConfigTest, RouteUpdateReplacesAttributesTransactionally) {
  constexpr char input[] = R"xml(<route-update xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib"><return-failure-detail>true</return-failure-detail><rib-name>100</rib-name><input-routes><route-list><route-index>7</route-index><match><ipv4><dest-ipv4-prefix>192.0.2.0/24</dest-ipv4-prefix></ipv4></match><updated-route-attr><route-preference>20</route-preference><local-only>true</local-only></updated-route-attr></route-list></input-routes></route-update>)xml";
  ObservedRoute route;
  route.route = {.routing_instance = "default", .rib = "100",
                 .address_family = "ipv4", .index = 99,
                 .destination = "192.0.2.0/24", .gateway = "192.0.2.1",
                 .interface = "dummy0", .nexthop_ref = std::nullopt,
                 .preference = 10};
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
                 .nexthop_ref = std::nullopt,
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

TEST(RibConfigTest, RouteUpdateResolvesRegisteredNexthop) {
  NexthopRegistry registry;
  ASSERT_EQ(registry.Add({.rib = "100", .gateway = "192.0.2.44",
                          .interface = "dummy44", .address_family = "ipv4",
                          .sharable = false}), 1U);
  ObservedRoute observed;
  observed.route = {.routing_instance = "default", .rib = "100",
                    .address_family = "ipv4", .destination = "192.0.2.0/24",
                    .gateway = "192.0.2.1", .interface = "dummy0",
                    .nexthop_ref = std::nullopt,
                    .preference = 10};
  std::vector<NativeCommand> commands;
  std::string output;
  std::string error;
  std::string path;
  ASSERT_TRUE(InvokeRouteUpdate(
      NativePlatform::kLinux,
      R"(<route-update xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib"><rib-name>100</rib-name><input-routes><route-list><route-index>7</route-index><match><ipv4><dest-ipv4-prefix>192.0.2.0/24</dest-ipv4-prefix></ipv4></match><updated-nexthop><nexthop-base><nexthop-ref>1</nexthop-ref></nexthop-base></updated-nexthop></route-list></input-routes></route-update>)",
      &output, &error, &path,
      [&](const NativeCommand& command, std::string*) {
        commands.push_back(command); return true;
      },
      [&](std::vector<ObservedRoute>* routes, std::string*) {
        *routes = {observed}; return true;
      },
      [&](const std::string& rib, std::uint32_t id,
          std::optional<std::string>* gateway,
          std::optional<std::string>* interface) {
        return registry.Resolve(rib, id, gateway, interface);
      }, &registry)) << error;
  ASSERT_EQ(commands.size(), 2U);
  EXPECT_NE(std::ranges::find(commands[1].arguments, "192.0.2.44"),
            commands[1].arguments.end());
  EXPECT_NE(std::ranges::find(commands[1].arguments, "dummy44"),
            commands[1].arguments.end());
  EXPECT_NE(output.find(">1</success-count>"), std::string::npos);
}

TEST(RibConfigTest, RouteUpdateReportsMissingNexthopReference) {
  ObservedRoute observed;
  observed.route = {.routing_instance = "default", .rib = "100",
                    .address_family = "ipv4", .destination = "192.0.2.0/24",
                    .gateway = "192.0.2.1", .interface = std::nullopt,
                    .nexthop_ref = std::nullopt,
                    .preference = 10};
  std::string output;
  std::string error;
  std::string path;
  ASSERT_TRUE(InvokeRouteUpdate(
      NativePlatform::kLinux,
      R"(<route-update xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib"><return-failure-detail>true</return-failure-detail><rib-name>100</rib-name><input-routes><route-list><route-index>7</route-index><match><ipv4><dest-ipv4-prefix>192.0.2.0/24</dest-ipv4-prefix></ipv4></match><updated-nexthop><nexthop-base><nexthop-ref>99</nexthop-ref></nexthop-base></updated-nexthop></route-list></input-routes></route-update>)",
      &output, &error, &path,
      [](const NativeCommand&, std::string*) { return true; },
      [&](std::vector<ObservedRoute>* routes, std::string*) {
        *routes = {observed}; return true;
      }));
  EXPECT_NE(output.find("<error-code>2</error-code>"), std::string::npos);
}

TEST(RibConfigTest, RouteUpdateCompensatesWhenBindingSaveFails) {
  NexthopRegistry registry;
  ASSERT_EQ(registry.Add({.rib = "100", .gateway = "192.0.2.44",
                          .interface = "dummy44", .address_family = "ipv4",
                          .sharable = false}), 1U);
  ObservedRoute observed;
  observed.route = {.routing_instance = "default", .rib = "100",
                    .address_family = "ipv4", .destination = "192.0.2.0/24",
                    .gateway = "192.0.2.1", .interface = "dummy0",
                    .nexthop_ref = std::nullopt, .preference = 10};
  std::vector<NativeCommand> commands;
  std::string output;
  std::string error;
  std::string path;
  EXPECT_FALSE(InvokeRouteUpdate(
      NativePlatform::kLinux,
      R"(<route-update xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib"><rib-name>100</rib-name><input-routes><route-list><route-index>7</route-index><match><ipv4><dest-ipv4-prefix>192.0.2.0/24</dest-ipv4-prefix></ipv4></match><updated-nexthop><nexthop-base><nexthop-ref>1</nexthop-ref></nexthop-base></updated-nexthop></route-list></input-routes></route-update>)",
      &output, &error, &path,
      [&](const NativeCommand& command, std::string*) {
        commands.push_back(command);
        return true;
      },
      [&](std::vector<ObservedRoute>* routes, std::string*) {
        *routes = {observed};
        return true;
      },
      [&](const std::string& rib, std::uint32_t id,
          std::optional<std::string>* gateway,
          std::optional<std::string>* interface) {
        return registry.Resolve(rib, id, gateway, interface);
      },
      &registry,
      [](const PersistentRegistry&, std::string* why) {
        *why = "injected update-binding failure";
        return false;
      }));
  ASSERT_EQ(commands.size(), 4U);
  EXPECT_EQ(commands[0].arguments[3], "delete");
  EXPECT_EQ(commands[1].arguments[3], "replace");
  EXPECT_EQ(commands[2].arguments[3], "delete");
  EXPECT_EQ(commands[3].arguments[3], "replace");
  EXPECT_FALSE(registry.RouteReference(observed.route).has_value());
}

TEST(RibConfigTest, RibAddValidatesLogicalNamespaceAndRejectsRpf) {
  std::string output;
  std::string error;
  std::string path;
  ASSERT_TRUE(InvokeRibAdd(NativePlatform::kLinux,
      R"(<rib-add xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib"><name>100</name><address-family>ipv4-address-family</address-family></rib-add>)",
      &output, &error, &path));
  EXPECT_NE(output.find(">true</result>"), std::string::npos) << output;
  ASSERT_TRUE(InvokeRibAdd(NativePlatform::kLinux,
      R"(<rib-add xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib"><name>100</name><address-family>ipv4-address-family</address-family><ip-rpf-check>true</ip-rpf-check></rib-add>)",
      &output, &error, &path));
  EXPECT_NE(output.find(">false</result>"), std::string::npos);
  EXPECT_NE(output.find("RPF"), std::string::npos);
}

TEST(RibConfigTest, RibAddAcceptsMappedArbitraryNameAndRejectsUnknownName) {
  std::string output;
  std::string error;
  std::string path;
  const auto mapping = [](const std::string& name)
      -> std::optional<std::string> {
    return name == "blue" ? std::optional<std::string>("100") : std::nullopt;
  };
  ASSERT_TRUE(InvokeRibAdd(
      NativePlatform::kLinux,
      R"(<rib-add xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib"><name>blue</name><address-family>ipv4-address-family</address-family></rib-add>)",
      &output, &error, &path, nullptr, {}, mapping));
  EXPECT_NE(output.find(">true</result>"), std::string::npos);
  ASSERT_TRUE(InvokeRibAdd(
      NativePlatform::kLinux,
      R"(<rib-add xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib"><name>red</name><address-family>ipv4-address-family</address-family></rib-add>)",
      &output, &error, &path, nullptr, {}, mapping));
  EXPECT_NE(output.find(">false</result>"), std::string::npos);
}

TEST(RibConfigTest, RouteUpdateMappingFailureDoesNotReleaseNexthop) {
  NexthopRegistry registry;
  const auto id = registry.Add({.rib = "blue", .gateway = "198.51.100.1"});
  ASSERT_TRUE(id);
  ASSERT_TRUE(registry.Retain("blue", *id));
  std::string output;
  std::string error;
  std::string path;
  const RouteObserver observer = [](std::vector<ObservedRoute>* routes,
                                    std::string*) {
    Route route;
    route.rib = "blue";
    route.index = 7;
    route.destination = "192.0.2.0/24";
    route.gateway = "198.51.100.1";
    routes->push_back({route, true});
    return true;
  };
  ASSERT_TRUE(InvokeRouteUpdate(
      NativePlatform::kLinux,
      R"(<route-update xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib"><rib-name>blue</rib-name><input-routes><route-list><route-index>7</route-index><match><ipv4><dest-ipv4-prefix>192.0.2.0/24</dest-ipv4-prefix></ipv4></match><updated-nexthop><nexthop-base><nexthop-ref>1</nexthop-ref></nexthop-base></updated-nexthop></route-list></input-routes></route-update>)",
      &output, &error, &path, {}, observer, {}, &registry, {}, {},
      [](const std::string&) -> std::optional<std::string> {
        return std::nullopt;
      })) << error;
  EXPECT_NE(output.find(">1</failed-count>"), std::string::npos);
  EXPECT_EQ(registry.Remove("blue", *id),
            NexthopRegistry::RemoveResult::kInUse);
}

TEST(RibConfigTest, RibAddDurablySuppliesInterfaceOnlyNexthopFamily) {
  NexthopRegistry registry;
  PersistentRegistry written;
  std::string output;
  std::string error;
  std::string path;
  ASSERT_TRUE(InvokeRibAdd(
      NativePlatform::kLinux,
      R"(<rib-add xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib"><name>100</name><address-family>ipv6-address-family</address-family></rib-add>)",
      &output, &error, &path, &registry,
      [&](const PersistentRegistry& state, std::string*) {
        written = state;
        return true;
      })) << error;
  ASSERT_EQ(written.ribs.size(), 1U);
  EXPECT_EQ(written.ribs[0], (PersistentRib{"100", "ipv6"}));

  ASSERT_TRUE(InvokeNexthopAdd(
      &registry,
      R"(<nh-add xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib"><rib-name>100</rib-name><nexthop-base><outgoing-interface>dummy0</outgoing-interface></nexthop-base></nh-add>)",
      &output, &error, &path)) << error;
  const auto snapshot = registry.Snapshot();
  ASSERT_EQ(snapshot.size(), 1U);
  EXPECT_EQ(std::get<1>(snapshot[0]), "ipv6");
  EXPECT_NE(SerializeOperationalRoutes({}, snapshot).find(
                "<address-family>ipv6-address-family</address-family>"),
            std::string::npos);
}

TEST(RibConfigTest, InterfaceOnlyNexthopRejectsMissingOrConflictingRibFamily) {
  NexthopRegistry registry;
  std::string output;
  std::string error;
  std::string path;
  constexpr char nexthop[] =
      R"(<nh-add xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib"><rib-name>100</rib-name><nexthop-base><outgoing-interface>dummy0</outgoing-interface></nexthop-base></nh-add>)";
  ASSERT_TRUE(InvokeNexthopAdd(&registry, nexthop, &output, &error, &path));
  EXPECT_NE(output.find("prior rib-add"), std::string::npos);
  EXPECT_TRUE(registry.PersistentState().nexthops.empty());

  ASSERT_TRUE(InvokeRibAdd(
      NativePlatform::kLinux,
      R"(<rib-add xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib"><name>100</name><address-family>ipv4-address-family</address-family></rib-add>)",
      &output, &error, &path, &registry));
  ASSERT_TRUE(InvokeRibAdd(
      NativePlatform::kLinux,
      R"(<rib-add xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib"><name>100</name><address-family>ipv6-address-family</address-family></rib-add>)",
      &output, &error, &path, &registry));
  EXPECT_NE(output.find("another address family"), std::string::npos);
  EXPECT_EQ(registry.RibFamily("100"), "ipv4");
}

TEST(RibConfigTest, RibAddRollsBackFamilyWhenPersistenceFails) {
  NexthopRegistry registry;
  std::string output;
  std::string error;
  std::string path;
  EXPECT_FALSE(InvokeRibAdd(
      NativePlatform::kLinux,
      R"(<rib-add xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib"><name>100</name><address-family>ipv4-address-family</address-family></rib-add>)",
      &output, &error, &path, &registry,
      [](const PersistentRegistry&, std::string* why) {
        *why = "injected RIB-family write failure";
        return false;
      }));
  EXPECT_NE(error.find("injected RIB-family write failure"), std::string::npos);
  EXPECT_FALSE(registry.RibFamily("100").has_value());
}

TEST(RibConfigTest, RibDeleteRestoresEarlierRoutesAfterFailure) {
  ObservedRoute first;
  first.route = {.routing_instance = "default", .rib = "100",
                 .address_family = "ipv4", .destination = "192.0.2.0/24",
                 .gateway = "192.0.2.1", .interface = "dummy0",
                 .nexthop_ref = std::nullopt};
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

TEST(RibConfigTest, RibDeleteCompensatesWhenBindingSaveFails) {
  NexthopRegistry registry;
  ASSERT_EQ(registry.Add({.rib = "100", .gateway = "192.0.2.1",
                          .interface = "dummy0", .address_family = "ipv4",
                          .sharable = false}), 1U);
  ObservedRoute observed;
  observed.route = {.routing_instance = "default", .rib = "100",
                    .address_family = "ipv4", .destination = "192.0.2.0/24",
                    .gateway = "192.0.2.1", .interface = "dummy0",
                    .nexthop_ref = std::nullopt};
  ASSERT_TRUE(registry.Retain("100", 1));
  registry.BindRoute(observed.route, 1);
  std::vector<NativeCommand> commands;
  std::string output;
  std::string error;
  std::string path;
  EXPECT_FALSE(InvokeRibDelete(
      NativePlatform::kLinux,
      R"(<rib-delete xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib"><name>100</name></rib-delete>)",
      &output, &error, &path,
      [&](const NativeCommand& command, std::string*) {
        commands.push_back(command);
        return true;
      },
      [&](std::vector<ObservedRoute>* routes, std::string*) {
        *routes = {observed};
        return true;
      },
      &registry,
      [](const PersistentRegistry&, std::string* why) {
        *why = "injected rib-binding failure";
        return false;
      }));
  ASSERT_EQ(commands.size(), 2U);
  EXPECT_EQ(commands[0].arguments[3], "delete");
  EXPECT_EQ(commands[1].arguments[3], "replace");
  EXPECT_EQ(registry.RouteReference(observed.route), 1U);
}

TEST(RibConfigTest, NexthopLifecycleAllocatesAndScopesIdentifiersByRib) {
  NexthopRegistry registry;
  std::string output;
  std::string error;
  std::string path;
  ASSERT_TRUE(InvokeNexthopAdd(
      &registry,
      R"(<nh-add xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib"><rib-name>100</rib-name><sharing-flag>true</sharing-flag><nexthop-base><egress-interface-ipv4-address><outgoing-interface>dummy0</outgoing-interface><ipv4-address>192.0.2.1</ipv4-address></egress-interface-ipv4-address></nexthop-base></nh-add>)",
      &output, &error, &path)) << error;
  EXPECT_NE(output.find(">true</result>"), std::string::npos);
  EXPECT_NE(output.find(">1</nexthop-id>"), std::string::npos);

  ASSERT_TRUE(InvokeNexthopDelete(
      &registry,
      R"(<nh-delete xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib"><rib-name>200</rib-name><nexthop-id>1</nexthop-id></nh-delete>)",
      &output, &error, &path));
  EXPECT_NE(output.find(">false</result>"), std::string::npos);
  EXPECT_NE(output.find("does not exist"), std::string::npos);

  ASSERT_TRUE(InvokeNexthopDelete(
      &registry,
      R"(<nh-delete xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib"><rib-name>100</rib-name><nexthop-id>1</nexthop-id></nh-delete>)",
      &output, &error, &path));
  EXPECT_NE(output.find(">true</result>"), std::string::npos);
  ASSERT_TRUE(InvokeNexthopDelete(
      &registry,
      R"(<nh-delete xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib"><rib-name>100</rib-name><nexthop-id>1</nexthop-id></nh-delete>)",
      &output, &error, &path));
  EXPECT_NE(output.find(">false</result>"), std::string::npos);
}

TEST(RibConfigTest, NexthopDeleteRejectsRetainedReferenceUntilRelease) {
  NexthopRegistry registry;
  ASSERT_EQ(registry.Add({.rib = "100", .gateway = "192.0.2.1",
                          .interface = std::nullopt, .address_family = "ipv4",
                          .sharable = false}), 1U);
  ASSERT_TRUE(registry.Retain("100", 1));
  std::string output;
  std::string error;
  std::string path;
  constexpr char deletion[] =
      R"(<nh-delete xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib"><rib-name>100</rib-name><nexthop-id>1</nexthop-id></nh-delete>)";
  ASSERT_TRUE(InvokeNexthopDelete(&registry, deletion, &output, &error, &path));
  EXPECT_NE(output.find(">false</result>"), std::string::npos);
  EXPECT_NE(output.find("referenced by a route"), std::string::npos);
  registry.Release("100", 1);
  ASSERT_TRUE(InvokeNexthopDelete(&registry, deletion, &output, &error, &path));
  EXPECT_NE(output.find(">true</result>"), std::string::npos);
}

TEST(RibConfigTest, ReconcilesExactDatastoreReferenceSet) {
  NexthopRegistry registry;
  ASSERT_EQ(registry.Add({.rib = "100", .gateway = "192.0.2.1",
                          .interface = std::nullopt,
                          .address_family = "ipv4", .sharable = false}), 1U);
  ASSERT_TRUE(registry.ReplaceConfigurationReferences({{"100", 1}}));
  EXPECT_EQ(registry.Remove("100", 1),
            NexthopRegistry::RemoveResult::kInUse);
  ASSERT_TRUE(registry.ReplaceConfigurationReferences({}));
  EXPECT_EQ(registry.Remove("100", 1),
            NexthopRegistry::RemoveResult::kRemoved);
}

TEST(RibConfigTest, PersistsAndRestoresPrivateRegistryAtomically) {
  const auto directory = std::filesystem::temp_directory_path() /
      ("dang-rib-state-" + std::to_string(getpid()));
  std::filesystem::create_directories(directory);
  const auto state = directory / "registry.json";
  PersistentRegistry expected;
  expected.next_id = 9;
  expected.ribs.push_back({"100", "ipv4"});
  expected.nexthops.push_back({"100", 7, "192.0.2.1", "dummy0", "ipv4", true});
  expected.nexthops.push_back(
      {"200", 8, std::nullopt, std::nullopt, std::nullopt, false});
  expected.bindings.push_back({"100", "ipv4", "198.51.100.0/24", 42, 7});
  std::string error;
  ASSERT_TRUE(SaveRegistry(state, expected, &error)) << error;
  std::ifstream encoded(state);
  const std::string encoded_text((std::istreambuf_iterator<char>(encoded)),
                                 std::istreambuf_iterator<char>());
  EXPECT_NE(encoded_text.find("\"version\": 2"), std::string::npos);
  EXPECT_NE(encoded_text.find("\"route-index\": 42"), std::string::npos);
  EXPECT_EQ(std::filesystem::status(state).permissions() &
                (std::filesystem::perms::group_all |
                 std::filesystem::perms::others_all),
            std::filesystem::perms::none);
  PersistentRegistry restored;
  ASSERT_TRUE(LoadRegistry(state, &restored, &error)) << error;
  EXPECT_EQ(restored, expected);
  std::filesystem::remove_all(directory);
}

TEST(RibConfigTest, RetainsParallelRouteBindingsByModeledIndex) {
  NexthopRegistry registry;
  ASSERT_EQ(registry.Add({.rib = "100", .gateway = "192.0.2.1",
                          .address_family = "ipv4"}), 1U);
  ASSERT_EQ(registry.Add({.rib = "100", .gateway = "192.0.2.2",
                          .address_family = "ipv4"}), 2U);
  ASSERT_TRUE(registry.Retain("100", 1));
  ASSERT_TRUE(registry.Retain("100", 2));
  Route first{.routing_instance = "default", .rib = "100",
              .address_family = "ipv4", .index = 11,
              .destination = "198.51.100.0/24", .gateway = "192.0.2.1",
              .nexthop_ref = 1};
  Route second = first;
  second.index = 12;
  second.gateway = "192.0.2.2";
  second.nexthop_ref = 2;
  registry.BindRoute(first, 1);
  registry.BindRoute(second, 2);
  const PersistentRegistry state = registry.PersistentState();
  ASSERT_EQ(state.bindings.size(), 2U);
  EXPECT_EQ(state.bindings[0].route_index, 11U);
  EXPECT_EQ(state.bindings[1].route_index, 12U);
  EXPECT_EQ(registry.RouteReference(first), 1U);
  EXPECT_EQ(registry.RouteReference(second), 2U);
  registry.ForgetRoute(first);
  EXPECT_FALSE(registry.RouteReference(first));
  EXPECT_EQ(registry.RouteReference(second), 2U);
}

TEST(RibConfigTest, LoadsVersionOneBindingWithHistoricalIndexZero) {
  const auto path = std::filesystem::temp_directory_path() /
                    ("dang-rib-v1-" + std::to_string(getpid()) + ".json");
  {
    std::ofstream output(path);
    output << R"({"version":1,"next-id":2,"nexthops":[{"rib":"100","id":1,"gateway":"192.0.2.1","interface":null,"address-family":"ipv4","sharable":false}],"bindings":[{"rib":"100","address-family":"ipv4","destination":"198.51.100.0/24","nexthop-id":1}]})";
  }
  std::filesystem::permissions(path, std::filesystem::perms::owner_read |
                                         std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::replace);
  PersistentRegistry restored;
  std::string error;
  ASSERT_TRUE(LoadRegistry(path, &restored, &error)) << error;
  ASSERT_EQ(restored.bindings.size(), 1U);
  EXPECT_EQ(restored.bindings[0].route_index, 0U);
  EXPECT_EQ(restored.bindings[0].nexthop_id, 1U);
  std::filesystem::remove(path);
}

TEST(RibConfigTest, RebuildsRegistryObjectsBindingsAndReferences) {
  NexthopRegistry original;
  ASSERT_EQ(original.RegisterRib("100", "ipv4"),
            NexthopRegistry::RegisterRibResult::kRegistered);
  ASSERT_EQ(original.Add({.rib = "100", .gateway = "192.0.2.1",
                          .interface = "dummy0",
                          .address_family = "ipv4", .sharable = true}), 1U);
  Route route;
  route.rib = "100";
  route.address_family = "ipv4";
  route.destination = "198.51.100.0/24";
  ASSERT_TRUE(original.Retain("100", 1));
  original.BindRoute(route, 1);

  const PersistentRegistry state = original.PersistentState();
  ASSERT_EQ(state.nexthops.size(), 1U);
  ASSERT_EQ(state.bindings.size(), 1U);
  EXPECT_EQ(state.bindings[0].destination, route.destination);

  NexthopRegistry restored;
  std::string error;
  ASSERT_TRUE(restored.RestorePersistentState(state, &error)) << error;
  std::optional<std::string> gateway;
  std::optional<std::string> interface;
  ASSERT_TRUE(restored.Resolve("100", 1, &gateway, &interface));
  EXPECT_EQ(gateway, "192.0.2.1");
  EXPECT_EQ(interface, "dummy0");
  EXPECT_EQ(restored.RouteReference(route), 1U);
  EXPECT_EQ(restored.RibFamily("100"), "ipv4");
  EXPECT_EQ(restored.Remove("100", 1),
            NexthopRegistry::RemoveResult::kInUse);
  EXPECT_EQ(restored.Add({.rib = "100", .gateway = "192.0.2.2",
                          .interface = std::nullopt,
                          .address_family = "ipv4", .sharable = false}), 2U);
}

TEST(RibConfigTest, RejectsInconsistentPersistentRegistryRecovery) {
  PersistentRegistry state;
  state.nexthops.push_back(
      {"100", 1, "192.0.2.1", std::nullopt, "ipv4", false});
  state.bindings.push_back({"100", "ipv4", "198.51.100.0/24", 1, 2});
  NexthopRegistry registry;
  std::string error;
  EXPECT_FALSE(registry.RestorePersistentState(state, &error));
  EXPECT_NE(error.find("invalid route binding"), std::string::npos);
  EXPECT_TRUE(registry.PersistentState().nexthops.empty());
}

TEST(RibConfigTest, RejectsNexthopThatConflictsWithPersistentRibFamily) {
  PersistentRegistry state;
  state.ribs.push_back({"100", "ipv6"});
  state.nexthops.push_back(
      {"100", 1, "192.0.2.1", std::nullopt, "ipv4", false});
  NexthopRegistry registry;
  std::string error;
  EXPECT_FALSE(registry.RestorePersistentState(state, &error));
  EXPECT_NE(error.find("conflicting"), std::string::npos);
  EXPECT_TRUE(registry.PersistentState().ribs.empty());
}

TEST(RibConfigTest, MakesNexthopMutationDurableBeforeAcknowledgement) {
  constexpr char addition[] =
      R"(<nh-add xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib"><rib-name>100</rib-name><nexthop-base><ipv4-address>192.0.2.1</ipv4-address></nexthop-base></nh-add>)";
  constexpr char deletion[] =
      R"(<nh-delete xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib"><rib-name>100</rib-name><nexthop-id>1</nexthop-id></nh-delete>)";
  NexthopRegistry registry;
  PersistentRegistry written;
  std::string output;
  std::string error;
  std::string path;
  ASSERT_TRUE(InvokeNexthopAdd(
      &registry, addition, &output, &error, &path,
      [&](const PersistentRegistry& state, std::string*) {
        written = state;
        return true;
      })) << error;
  ASSERT_EQ(written.nexthops.size(), 1U);
  EXPECT_EQ(written.nexthops[0].id, 1U);
  ASSERT_TRUE(InvokeNexthopDelete(
      &registry, deletion, &output, &error, &path,
      [&](const PersistentRegistry& state, std::string*) {
        written = state;
        return true;
      })) << error;
  EXPECT_TRUE(written.nexthops.empty());
}

TEST(RibConfigTest, RollsNexthopMutationBackWhenPersistenceFails) {
  constexpr char addition[] =
      R"(<nh-add xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib"><rib-name>100</rib-name><nexthop-base><ipv4-address>192.0.2.1</ipv4-address></nexthop-base></nh-add>)";
  constexpr char deletion[] =
      R"(<nh-delete xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib"><rib-name>100</rib-name><nexthop-id>1</nexthop-id></nh-delete>)";
  const RegistryWriter failure = [](const PersistentRegistry&, std::string* why) {
    *why = "injected durable-write failure";
    return false;
  };
  NexthopRegistry registry;
  std::string output;
  std::string error;
  std::string path;
  EXPECT_FALSE(InvokeNexthopAdd(&registry, addition, &output, &error, &path,
                                failure));
  EXPECT_NE(error.find("injected durable-write failure"), std::string::npos);
  EXPECT_TRUE(registry.PersistentState().nexthops.empty());

  ASSERT_EQ(registry.Add({.rib = "100", .gateway = "192.0.2.1",
                          .interface = std::nullopt,
                          .address_family = "ipv4", .sharable = false}), 1U);
  EXPECT_FALSE(InvokeNexthopDelete(&registry, deletion, &output, &error, &path,
                                   failure));
  EXPECT_EQ(registry.PersistentState().nexthops.size(), 1U);
}

TEST(RibConfigTest, NexthopAddRejectsUnsupportedCompositeForm) {
  NexthopRegistry registry;
  std::string output;
  std::string error;
  std::string path;
  ASSERT_TRUE(InvokeNexthopAdd(
      &registry,
      R"(<nh-add xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib"><rib-name>100</rib-name><nexthop-chain/></nh-add>)",
      &output, &error, &path));
  EXPECT_NE(output.find(">false</result>"), std::string::npos);
  EXPECT_NE(output.find("only a base nexthop"), std::string::npos);
}

TEST(RibConfigTest, ResolvesRegisteredNexthopReferenceInConfiguration) {
  std::string xml(kBefore);
  const auto begin = xml.find("<egress-interface-ipv4-address>");
  const auto end = xml.find("</egress-interface-ipv4-address>");
  ASSERT_NE(begin, std::string::npos);
  ASSERT_NE(end, std::string::npos);
  xml.replace(begin, end + std::string("</egress-interface-ipv4-address>").size() - begin,
              "<nexthop-ref>23</nexthop-ref>");
  Config config;
  std::string error;
  std::string path;
  ASSERT_TRUE(ParseConfig(
      xml.c_str(), &config, &error, &path,
      [](const std::string& rib, std::uint32_t id,
         std::optional<std::string>* gateway,
         std::optional<std::string>* interface) {
        if (rib != "100" || id != 23) return false;
        *gateway = "198.51.100.9";
        *interface = "dummy23";
        return true;
      })) << error;
  ASSERT_EQ(config.routes.size(), 1U);
  EXPECT_EQ(config.routes.front().gateway, "198.51.100.9");
  EXPECT_EQ(config.routes.front().interface, "dummy23");
}

TEST(RibConfigTest, RejectsMissingOrCrossRibNexthopReference) {
  std::string xml(kBefore);
  const auto begin = xml.find("<egress-interface-ipv4-address>");
  const auto end = xml.find("</egress-interface-ipv4-address>");
  ASSERT_NE(begin, std::string::npos);
  ASSERT_NE(end, std::string::npos);
  xml.replace(begin, end + std::string("</egress-interface-ipv4-address>").size() - begin,
              "<nexthop-ref>23</nexthop-ref>");
  Config config;
  std::string error;
  std::string path;
  EXPECT_FALSE(ParseConfig(xml.c_str(), &config, &error, &path));
  EXPECT_NE(error.find("registered nexthop"), std::string::npos);
  EXPECT_NE(path.find("nexthop-ref"), std::string::npos);
}

TEST(RibConfigTest, RouteAddExecutesResolvedRegisteredNexthop) {
  NexthopRegistry registry;
  const auto id = registry.Add({.rib = "100", .gateway = "192.0.2.9",
                                .interface = "dummy9",
                                .address_family = "ipv4",
                                .sharable = false});
  ASSERT_EQ(id, 1U);
  std::vector<NativeCommand> commands;
  std::string output;
  std::string error;
  std::string path;
  ASSERT_TRUE(InvokeRouteAdd(
      NativePlatform::kLinux,
      R"(<route-add xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib"><rib-name>100</rib-name><routes><route-list><route-index>9</route-index><match><ipv4><dest-ipv4-prefix>198.51.100.0/24</dest-ipv4-prefix></ipv4></match><route-attributes><route-preference>10</route-preference><local-only>false</local-only></route-attributes><nexthop><nexthop-base><nexthop-ref>1</nexthop-ref></nexthop-base></nexthop></route-list></routes></route-add>)",
      &output, &error, &path,
      [&](const NativeCommand& command, std::string*) {
        commands.push_back(command);
        return true;
      },
      [&](const std::string& rib, std::uint32_t reference,
          std::optional<std::string>* gateway,
          std::optional<std::string>* interface) {
        return registry.Resolve(rib, reference, gateway, interface);
      }, &registry)) << error;
  ASSERT_EQ(commands.size(), 1U);
  EXPECT_NE(std::ranges::find(commands.front().arguments, "192.0.2.9"),
            commands.front().arguments.end());
  EXPECT_NE(std::ranges::find(commands.front().arguments, "dummy9"),
            commands.front().arguments.end());
  EXPECT_NE(output.find(">1</success-count>"), std::string::npos);

  constexpr char nh_delete[] =
      R"(<nh-delete xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib"><rib-name>100</rib-name><nexthop-id>1</nexthop-id></nh-delete>)";
  ASSERT_TRUE(InvokeNexthopDelete(&registry, nh_delete, &output, &error, &path));
  EXPECT_NE(output.find("referenced by a route"), std::string::npos);

  ObservedRoute installed;
  installed.route = {.routing_instance = "default", .rib = "100",
                     .address_family = "ipv4", .index = 9,
                     .destination = "198.51.100.0/24",
                     .gateway = "192.0.2.9", .interface = "dummy9",
                     .nexthop_ref = std::nullopt, .preference = 10,
                     .local_only = false};
  ASSERT_TRUE(InvokeRouteDelete(
      NativePlatform::kLinux,
      R"(<route-delete xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib"><rib-name>100</rib-name><routes><route-list><route-index>9</route-index><match><ipv4><dest-ipv4-prefix>198.51.100.0/24</dest-ipv4-prefix></ipv4></match></route-list></routes></route-delete>)",
      &output, &error, &path,
      [](const NativeCommand&, std::string*) { return true; },
      [&](std::vector<ObservedRoute>* routes, std::string*) {
        *routes = {installed}; return true;
      }, &registry));
  ASSERT_TRUE(InvokeNexthopDelete(&registry, nh_delete, &output, &error, &path));
  EXPECT_NE(output.find(">true</result>"), std::string::npos) << output;
}

TEST(RibConfigTest, RouteAddCompensatesNativeStateWhenBindingSaveFails) {
  NexthopRegistry registry;
  ASSERT_EQ(registry.Add({.rib = "100", .gateway = "192.0.2.9",
                          .interface = "dummy9", .address_family = "ipv4",
                          .sharable = false}), 1U);
  std::vector<NativeCommand> commands;
  std::string output;
  std::string error;
  std::string path;
  unsigned events = 0;
  EXPECT_FALSE(InvokeRouteAdd(
      NativePlatform::kLinux,
      R"(<route-add xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib"><rib-name>100</rib-name><routes><route-list><route-index>9</route-index><match><ipv4><dest-ipv4-prefix>198.51.100.0/24</dest-ipv4-prefix></ipv4></match><route-attributes><route-preference>10</route-preference><local-only>false</local-only></route-attributes><nexthop><nexthop-base><nexthop-ref>1</nexthop-ref></nexthop-base></nexthop></route-list></routes></route-add>)",
      &output, &error, &path,
      [&](const NativeCommand& command, std::string*) {
        commands.push_back(command);
        return true;
      },
      [&](const std::string& rib, std::uint32_t id,
          std::optional<std::string>* gateway,
          std::optional<std::string>* interface) {
        return registry.Resolve(rib, id, gateway, interface);
      },
      &registry,
      [](const PersistentRegistry&, std::string* why) {
        *why = "injected binding-write failure";
        return false;
      },
      [&](const Route&, bool) { ++events; }));
  ASSERT_EQ(commands.size(), 2U);
  EXPECT_EQ(commands[0].arguments[3], "replace");
  EXPECT_EQ(commands[1].arguments[3], "delete");
  EXPECT_NE(error.find("injected binding-write failure"), std::string::npos);
  Route route;
  route.rib = "100";
  route.address_family = "ipv4";
  route.destination = "198.51.100.0/24";
  EXPECT_FALSE(registry.RouteReference(route).has_value());
  EXPECT_EQ(events, 0U);
}

TEST(RibConfigTest, RouteDeleteRestoresNativeStateWhenBindingSaveFails) {
  NexthopRegistry registry;
  ASSERT_EQ(registry.Add({.rib = "100", .gateway = "192.0.2.9",
                          .interface = "dummy9", .address_family = "ipv4",
                          .sharable = false}), 1U);
  ObservedRoute observed;
  observed.route = {.routing_instance = "default", .rib = "100",
                    .address_family = "ipv4", .index = 9,
                    .destination = "198.51.100.0/24",
                    .gateway = "192.0.2.9", .interface = "dummy9",
                    .nexthop_ref = std::nullopt, .preference = 10};
  ASSERT_TRUE(registry.Retain("100", 1));
  registry.BindRoute(observed.route, 1);
  std::vector<NativeCommand> commands;
  std::string output;
  std::string error;
  std::string path;
  EXPECT_FALSE(InvokeRouteDelete(
      NativePlatform::kLinux,
      R"(<route-delete xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib"><rib-name>100</rib-name><routes><route-list><route-index>9</route-index><match><ipv4><dest-ipv4-prefix>198.51.100.0/24</dest-ipv4-prefix></ipv4></match></route-list></routes></route-delete>)",
      &output, &error, &path,
      [&](const NativeCommand& command, std::string*) {
        commands.push_back(command);
        return true;
      },
      [&](std::vector<ObservedRoute>* routes, std::string*) {
        *routes = {observed};
        return true;
      },
      &registry,
      [](const PersistentRegistry&, std::string* why) {
        *why = "injected binding-write failure";
        return false;
      }));
  ASSERT_EQ(commands.size(), 2U);
  EXPECT_EQ(commands[0].arguments[3], "delete");
  EXPECT_EQ(commands[1].arguments[3], "replace");
  EXPECT_EQ(registry.RouteReference(observed.route), 1U);
}

}  // namespace
}  // namespace dang::rib

// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "kea_adapter.h"

#include <chrono>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool Check(bool condition, const char* message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}

}  // namespace

int main() {
  constexpr char xml[] = R"xml(<config xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
  <config xmlns="urn:ietf:params:xml:ns:yang:kea-dhcp4-server">
    <subnet4><id>4</id><pool><start-address>192.0.2.10</start-address>
      <end-address>192.0.2.20</end-address></pool><subnet>192.0.2.0/24</subnet></subnet4>
    <interfaces-config><interfaces>dangtest0</interfaces></interfaces-config>
    <hosts-database><database-type>memfile</database-type></hosts-database>
    <config-control><config-database><database-type>mysql</database-type>
      <host>db.example</host></config-database></config-control>
    <hook-library><library>/usr/lib/kea/hooks/libdhcp_test.so</library>
      <parameters>{"mode":"strict"}</parameters></hook-library>
    <host><identifier-type>hw-address</identifier-type>
      <identifier>00:01:02:03:04:05</identifier></host>
    <t1-percent>0.5</t1-percent>
    <dhcp-queue-control>{"enable-queue":true,"queue-type":"kea-ring4"}</dhcp-queue-control>
    <control-sockets><socket-type>unix</socket-type><socket-name>/tmp/kea4.sock</socket-name>
      <http-headers><name>X-Kea-Test</name><value>true</value></http-headers>
    </control-sockets>
  </config>
  <config xmlns="urn:ietf:params:xml:ns:yang:kea-dhcp6-server">
    <subnet6><id>6</id><pool><prefix>2001:db8:1::100/120</prefix></pool>
      <subnet>2001:db8:1::/64</subnet></subnet6>
    <interfaces-config><interfaces>dangtest0</interfaces></interfaces-config>
    <host><identifier-type>duid</identifier-type><identifier>00:01</identifier>
      <ip-addresses>2001:db8:1::10</ip-addresses>
      <prefixes>2001:db8:10::/56</prefixes>
      <excluded-prefixes>2001:db8:10:ff00::/64</excluded-prefixes></host>
    <relay-supplied-options>65</relay-supplied-options>
  </config>
</config>)xml";
  std::string error;
  auto dhcp4 = dang::plugins::kea::TranslateConfiguration(
      xml, "kea-dhcp4-server", "/tmp/kea4.sock", &error);
  auto dhcp6 = dang::plugins::kea::TranslateConfiguration(
      xml, "kea-dhcp6-server", "/tmp/kea6.sock", &error);
  if (!Check(dhcp4.has_value() && dhcp6.has_value(), error.c_str())) return 1;
  const auto& four = dhcp4->arguments.at("Dhcp4");
  const auto& six = dhcp6->arguments.at("Dhcp6");
  bool valid = true;
  valid &= Check(four.at("subnet4").is_array(), "subnet4 is not an array");
  valid &= Check(four.at("subnet4").at(0).at("id") == 4,
                 "subnet4 ID is not numeric");
  valid &= Check(four.at("subnet4").at(0).at("pools").at(0).at("pool") ==
                     "192.0.2.10 - 192.0.2.20",
                 "IPv4 pool range was not translated");
  valid &= Check(four.at("subnet4").at(0).at("subnet") == "192.0.2.0/24",
                 "subnet prefix leaf was confused with a state list");
  valid &= Check(four.at("interfaces-config").at("interfaces").is_array(),
                 "single interface leaf-list is not an array");
  valid &= Check(four.at("hosts-databases").is_array(),
                 "singleton hosts-database is not an array");
  valid &= Check(four.at("config-control").at("config-databases").is_array(),
                 "singleton config-database is not an array");
  valid &= Check(four.at("config-control").at("config-databases").at(0)
                         .at("host") == "db.example",
                 "database host leaf was confused with reservation list");
  valid &= Check(four.at("hooks-libraries").at(0).at("parameters").at("mode") ==
                     "strict",
                 "hook library JSON parameters were not translated");
  valid &= Check(four.at("reservations").is_array(),
                 "host reservations were not renamed");
  valid &= Check(four.at("reservations").at(0).at("hw-address") ==
                     "00:01:02:03:04:05" &&
                     !four.at("reservations").at(0).contains("identifier"),
                 "host reservation identifier was not converted to Kea form");
  valid &= Check(four.at("t1-percent").is_number_float(),
                 "decimal64 value is not numeric");
  valid &= Check(four.at("dhcp-queue-control").at("enable-queue") == true,
                 "DHCP queue control JSON was not translated");
  valid &= Check(four.at("control-sockets").at(0).at("http-headers").at(0)
                         .at("value") == true,
                 "HTTP header JSON value was not translated");
  valid &= Check(six.at("subnet6").at(0).at("pools").at(0).at("pool") ==
                     "2001:db8:1::100/120",
                 "IPv6 pool prefix was not translated");
  valid &= Check(six.at("reservations").at(0).at("ip-addresses").is_array(),
                 "singleton reservation IP leaf-list is not an array");
  valid &= Check(six.at("reservations").at(0).at("prefixes").is_array(),
                 "singleton reservation prefix leaf-list is not an array");
  valid &= Check(six.at("reservations").at(0).at("excluded-prefixes").is_array(),
                 "singleton excluded-prefix leaf-list is not an array");
  valid &= Check(six.at("relay-supplied-options").is_array(),
                 "singleton relay option leaf-list is not an array");
  valid &= Check(dang::plugins::kea::ExtractSubnetIds(*dhcp4) ==
                     std::vector<std::uint32_t>{4},
                 "DHCPv4 subnet IDs were not extracted from translated JSON");
  valid &= Check(dang::plugins::kea::ExtractSubnetIds(*dhcp6) ==
                     std::vector<std::uint32_t>{6},
                 "DHCPv6 subnet IDs were not extracted from translated JSON");
  const nlohmann::json leases4 = nlohmann::json::parse(R"json({
    "result": 0, "arguments": {"leases": [{
      "ip-address": "192.0.2.44", "hw-address": "00:01:02:03:04:05",
      "client-id": "01:ff", "valid-lft": 600, "cltt": 1234,
      "subnet-id": 4, "fqdn-fwd": true, "state": 1
    }]}
  })json");
  const nlohmann::json stats4 = nlohmann::json::parse(R"json({
    "result": 0, "arguments": {"result-set": {
      "columns": ["subnet-id", "total-addresses",
        "cumulative-assigned-addresses", "assigned-addresses",
        "declined-addresses"],
      "rows": [[4, 32, 10, 1, 1]]
    }}
  })json");
  const nlohmann::json hosts4 = nlohmann::json::parse(R"json({
    "result": 0, "arguments": {"hosts": [{
      "subnet-id": 4, "hw-address": "00:01:02:03:04:05",
      "ip-address": "192.0.2.50", "hostname": "printer.example",
      "client-classes": ["office"],
      "option-data": [{"code": 6, "space": "dhcp4", "data": "192.0.2.53",
        "csv-format": true, "client-classes": ["office"],
        "user-context": {"owner": "test"}}]
    }]}
  })json");
  auto state4 = dang::plugins::kea::TranslateOperationalState(
      "kea-dhcp4-server", leases4, stats4, hosts4, &error);
  valid &= Check(state4.has_value(), error.c_str());
  valid &= Check(state4 && state4->find("<hw-address>AAECAwQF</hw-address>") !=
                     std::string::npos,
                 "DHCPv4 hardware address was not encoded as YANG binary");
  valid &= Check(state4 && state4->find("<state>declined</state>") !=
                     std::string::npos,
                 "DHCPv4 lease state was not translated");
  valid &= Check(state4 && state4->find("<total-addresses>32</total-addresses>") !=
                     std::string::npos,
                 "DHCPv4 lease statistics were not translated");
  valid &= Check(state4 &&
                     state4->find("<identifier-type>hw-address</identifier-type>") !=
                         std::string::npos &&
                     state4->find("<identifier>00:01:02:03:04:05</identifier>") !=
                         std::string::npos,
                 "DHCPv4 host identifier was not translated");
  valid &= Check(state4 &&
                     state4->find("<option-data><code>6</code><space>dhcp4</space>") !=
                         std::string::npos &&
                     state4->find("<client-classes>office</client-classes>") !=
                         std::string::npos &&
                     state4->find("&quot;owner&quot;:&quot;test&quot;") !=
                         std::string::npos,
                 "DHCPv4 host option data was not translated");
  const nlohmann::json leases6 = nlohmann::json::parse(R"json({
    "result": 0, "arguments": {"leases": [{
      "ip-address": "2001:db8::44", "duid": "00:01:02:03",
      "valid-lft": 600, "cltt": 1234, "subnet-id": 6,
      "preferred-lft": 300, "type": 0, "iaid": 7, "prefix-len": 128
    }]}
  })json");
  const nlohmann::json stats6 = nlohmann::json::parse(R"json({
    "result": 0, "arguments": {"result-set": {
      "columns": ["subnet-id", "total-nas", "cumulative-assigned-nas",
        "assigned-nas", "declined-addresses", "total-pds",
        "cumulative-assigned-pds", "assigned-pds"],
      "rows": [[6, 256, 3, 1, 0, 16, 2, 1]]
    }}
  })json");
  const nlohmann::json hosts6 = nlohmann::json::parse(R"json({
    "result": 0, "arguments": {"hosts": [{
      "subnet-id": 6, "duid": "00:01:02:03",
      "ip-addresses": ["2001:db8::50"],
      "prefixes": ["2001:db8:50::/56"]
    }]}
  })json");
  auto state6 = dang::plugins::kea::TranslateOperationalState(
      "kea-dhcp6-server", leases6, stats6, hosts6, &error);
  valid &= Check(state6.has_value(), error.c_str());
  valid &= Check(state6 && state6->find("<duid>AAECAw==</duid>") !=
                     std::string::npos,
                 "DHCPv6 DUID was not encoded as YANG binary");
  valid &= Check(state6 && state6->find("<lease-type>IA_NA</lease-type>") !=
                     std::string::npos,
                 "DHCPv6 lease type was not translated");
  valid &= Check(state6 && state6->find("<assigned-pds>1</assigned-pds>") !=
                     std::string::npos,
                 "DHCPv6 lease statistics were not translated");
  valid &= Check(state6 &&
                     state6->find("<ip-addresses>2001:db8::50</ip-addresses>") !=
                         std::string::npos,
                 "DHCPv6 host addresses were not translated");

  std::vector<nlohmann::json> requests;
  std::size_t invocation = 0;
  const dang::plugins::kea::ControlQuery pages = [&](std::string_view,
      std::string_view command, const nlohmann::json& arguments,
      std::string*) -> std::optional<nlohmann::json> {
    requests.push_back({{"command", command}, {"arguments", arguments}});
    ++invocation;
    if (invocation == 1)
      return std::optional<nlohmann::json>(nlohmann::json{
          {"result", 0},
          {"arguments", {{"count", 2},
                         {"leases", {{{"ip-address", "192.0.2.1"}},
                                      {{"ip-address", "192.0.2.2"}}}}}}});
    return std::optional<nlohmann::json>(nlohmann::json{
        {"result", 0},
        {"arguments", {{"count", 1},
                       {"leases", {{{"ip-address", "192.0.2.3"}}}}}}});
  };
  auto paged = dang::plugins::kea::CollectLeasePages(
      "/tmp/kea4.sock", false, pages, &error,
      {.page_size = 2, .maximum_pages = 3, .maximum_items = 4,
       .maximum_bytes = 1024,
       .maximum_duration = std::chrono::milliseconds(100)});
  valid &= Check(paged && paged->at("arguments").at("leases").size() == 3,
                 "lease pages were not combined");
  valid &= Check(requests.size() == 2 &&
                     requests[0].at("arguments").at("from") == "start" &&
                     requests[1].at("arguments").at("from") == "192.0.2.2" &&
                     requests[1].at("command") == "lease4-get-page",
                 "lease paging did not carry the last address forward");

  const dang::plugins::kea::ControlQuery repeated_cursor =
      [](std::string_view, std::string_view, const nlohmann::json&,
         std::string*) -> std::optional<nlohmann::json> {
    return std::optional<nlohmann::json>(nlohmann::json{
        {"result", 0},
        {"arguments", {{"count", 1},
                       {"leases", {{{"ip-address", "start"}}}}}}});
  };
  error.clear();
  auto stalled = dang::plugins::kea::CollectLeasePages(
      "/tmp/kea4.sock", false, repeated_cursor, &error,
      {.page_size = 1, .maximum_pages = 2, .maximum_items = 2,
       .maximum_bytes = 1024,
       .maximum_duration = std::chrono::milliseconds(100)});
  valid &= Check(!stalled && error.find("did not advance") != std::string::npos,
                 "a repeated lease paging cursor was accepted");

  const dang::plugins::kea::ControlQuery oversized_page =
      [](std::string_view, std::string_view, const nlohmann::json&,
         std::string*) -> std::optional<nlohmann::json> {
    return std::optional<nlohmann::json>(nlohmann::json{
        {"result", 0},
        {"arguments", {{"count", 2},
                       {"leases", {{{"ip-address", "192.0.2.1"}},
                                    {{"ip-address", "192.0.2.2"}}}}}}});
  };
  error.clear();
  auto oversized = dang::plugins::kea::CollectLeasePages(
      "/tmp/kea4.sock", false, oversized_page, &error,
      {.page_size = 1, .maximum_pages = 2, .maximum_items = 2,
       .maximum_bytes = 1024,
       .maximum_duration = std::chrono::milliseconds(100)});
  valid &= Check(!oversized && error.find("leases/count") != std::string::npos,
                 "a page larger than the requested limit was accepted");

  error.clear();
  auto too_many = dang::plugins::kea::CollectLeasePages(
      "/tmp/kea4.sock", false, oversized_page, &error,
      {.page_size = 2,
       .maximum_pages = 2,
       .maximum_items = 1,
       .maximum_bytes = 1024,
       .maximum_duration = std::chrono::milliseconds(100)});
  valid &= Check(!too_many && error.find("item limit") != std::string::npos,
                 "the aggregate lease item limit was not enforced");

  error.clear();
  auto too_large = dang::plugins::kea::CollectLeasePages(
      "/tmp/kea4.sock", false, oversized_page, &error,
      {.page_size = 2,
       .maximum_pages = 2,
       .maximum_items = 2,
       .maximum_bytes = 1,
       .maximum_duration = std::chrono::milliseconds(100)});
  valid &= Check(!too_large && error.find("byte limit") != std::string::npos,
                 "the aggregate lease byte limit was not enforced");

  std::size_t host_invocation = 0;
  std::vector<nlohmann::json> host_requests;
  const dang::plugins::kea::ControlQuery host_pages =
      [&](std::string_view, std::string_view command,
          const nlohmann::json& arguments,
          std::string*) -> std::optional<nlohmann::json> {
    host_requests.push_back({{"command", command}, {"arguments", arguments}});
    if (host_invocation++ == 0)
      return std::optional<nlohmann::json>(nlohmann::json{
          {"result", 0},
          {"arguments",
           {{"count", 1},
            {"hosts", {{{"subnet-id", 4}, {"hw-address", "00:01"}}}},
            {"next", {{"from", 42}, {"source-index", 1}}}}}});
    return std::optional<nlohmann::json>(nlohmann::json{
        {"result", 3}, {"arguments", {{"count", 0}, {"hosts", {}}}}});
  };
  error.clear();
  auto paged_hosts = dang::plugins::kea::CollectHostPages(
      "/tmp/kea4.sock", host_pages, &error,
      {.page_size = 1,
       .maximum_pages = 3,
       .maximum_items = 2,
       .maximum_bytes = 1024,
       .maximum_duration = std::chrono::milliseconds(100)});
  valid &= Check(paged_hosts &&
                     paged_hosts->at("arguments").at("hosts").size() == 1,
                 "host reservation pages were not combined");
  valid &= Check(host_requests.size() == 2 &&
                     host_requests[1].at("arguments").at("from") == 42 &&
                     host_requests[1].at("arguments").at("source-index") == 1,
                 "host paging did not carry Kea's continuation map forward");

  std::vector<nlohmann::json> statistic_requests;
  const dang::plugins::kea::ControlQuery statistics =
      [&](std::string_view, std::string_view command,
          const nlohmann::json& arguments,
          std::string*) -> std::optional<nlohmann::json> {
    statistic_requests.push_back(
        {{"command", command}, {"arguments", arguments}});
    const auto id = arguments.at("subnet-id");
    nlohmann::json response{
        {"result", 0},
        {"arguments", {{"result-set",
                         {{"columns", {"subnet-id", "total-addresses"}},
                          {"rows", nlohmann::json::array({{id, 32}})}}}}}};
    return std::optional<nlohmann::json>(std::move(response));
  };
  error.clear();
  auto collected_statistics = dang::plugins::kea::CollectStatistics(
      "/tmp/kea4.sock", false, {4, 9}, statistics, &error,
      {.page_size = 1,
       .maximum_pages = 2,
       .maximum_items = 2,
       .maximum_bytes = 1024,
       .maximum_duration = std::chrono::milliseconds(100)});
  valid &= Check(collected_statistics &&
                     collected_statistics->at("arguments")
                             .at("result-set").at("rows").size() == 2,
                 "per-subnet statistics were not combined");
  valid &= Check(statistic_requests.size() == 2 &&
                     statistic_requests[0].at("command") ==
                         "stat-lease4-get" &&
                     statistic_requests[0].at("arguments").at("subnet-id") == 4 &&
                     statistic_requests[1].at("arguments").at("subnet-id") == 9,
                 "statistics queries did not use exact configured subnet IDs");

  error.clear();
  auto excessive_statistics = dang::plugins::kea::CollectStatistics(
      "/tmp/kea4.sock", false, {4, 9}, statistics, &error,
      {.page_size = 1,
       .maximum_pages = 1,
       .maximum_items = 2,
       .maximum_bytes = 1024,
       .maximum_duration = std::chrono::milliseconds(100)});
  valid &= Check(!excessive_statistics &&
                     error.find("query limit") != std::string::npos,
                 "the aggregate statistics query limit was not enforced");

  std::string command_reason;
  valid &= Check(dang::plugins::kea::CommandSucceeded(
                     nlohmann::json::array({{{"result", 0}}}),
                     &command_reason),
                 "a singleton successful Kea command response was rejected");
  valid &= Check(!dang::plugins::kea::CommandSucceeded(
                     nlohmann::json::array(), &command_reason) &&
                     command_reason.find("ambiguous") != std::string::npos,
                 "an empty Kea command response was accepted");
  valid &= Check(!dang::plugins::kea::CommandSucceeded(
                     nlohmann::json::array(
                         {{{"result", 0}}, {{"result", 1}}}),
                     &command_reason) &&
                     command_reason.find("ambiguous") != std::string::npos,
                 "a multi-answer Kea command response was accepted");
  return valid ? 0 : 1;
}

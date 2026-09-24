// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "kea_adapter.h"

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

bool Check(bool condition, const char* message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}

std::optional<nlohmann::json> OptionalJson(nlohmann::json value) {
  return std::optional<nlohmann::json>(value);
}

}  // namespace

int main() {
  constexpr char xml[] = R"xml(<config xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
  <config xmlns="urn:ietf:params:xml:ns:yang:kea-dhcp4-server">
    <server-tag>123</server-tag>
    <subnet4><id>4</id><pool><start-address>192.0.2.10</start-address>
      <end-address>192.0.2.20</end-address></pool><subnet>192.0.2.0/24</subnet></subnet4>
    <interfaces-config><interfaces>dangtest0</interfaces></interfaces-config>
    <lease-database><database-type>memfile</database-type>
      <persist>false</persist></lease-database>
    <hosts-database><database-type>memfile</database-type></hosts-database>
    <config-control><config-database><database-type>mysql</database-type>
      <host>db.example</host></config-database></config-control>
    <hook-library><library>/usr/lib/kea/hooks/libdhcp_test.so</library>
      <parameters>{"mode":"strict"}</parameters></hook-library>
    <hook-library><library>/usr/lib/kea/hooks/libdhcp_lease_cmds.so</library></hook-library>
    <hook-library><library>/usr/lib/kea/hooks/libdhcp_stat_cmds.so</library></hook-library>
    <hook-library><library>/usr/lib/kea/hooks/libdhcp_host_cmds.so</library></hook-library>
    <host><identifier-type>hw-address</identifier-type>
      <identifier>00:01:02:03:04:05</identifier>
      <hostname>true</hostname></host>
    <t1-percent>0.5</t1-percent>
    <dhcp-queue-control>{"enable-queue":true,"queue-type":"kea-ring4"}</dhcp-queue-control>
    <control-sockets><socket-type>unix</socket-type><socket-name>/tmp/kea4.sock</socket-name>
      <http-headers><name>X-Kea-Test</name><value>true</value></http-headers>
    </control-sockets>
  </config>
  <config xmlns="urn:ietf:params:xml:ns:yang:kea-dhcp6-server">
    <server-id/>
    <subnet6><id>6</id><pool><prefix>2001:db8:1::100/120</prefix></pool>
      <subnet>2001:db8:1::/64</subnet></subnet6>
    <interfaces-config><interfaces>dangtest0</interfaces></interfaces-config>
    <lease-database><database-type>memfile</database-type>
      <persist>false</persist></lease-database>
    <control-sockets><socket-type>unix</socket-type>
      <socket-name>/tmp/kea6.sock</socket-name></control-sockets>
    <hook-library><library>/usr/local/lib/kea/hooks/libdhcp_lease_cmds.so</library></hook-library>
    <hook-library><library>/usr/local/lib/kea/hooks/libdhcp_stat_cmds.so</library></hook-library>
    <hook-library><library>/usr/local/lib/kea/hooks/libdhcp_host_cmds.so</library></hook-library>
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
  constexpr char missing_socket_xml[] = R"xml(
    <config xmlns="urn:ietf:params:xml:ns:yang:kea-dhcp4-server">
      <valid-lifetime>600</valid-lifetime>
    </config>)xml";
  error.clear();
  auto missing_socket = dang::plugins::kea::TranslateConfiguration(
      missing_socket_xml, "kea-dhcp4-server", "/tmp/kea4.sock", &error);
  bool valid = Check(!missing_socket &&
                         error.find("preserve the managed UNIX control socket") !=
                             std::string::npos,
                     "configuration without the managed socket was accepted");
  constexpr char foreign_namespace_xml[] = R"xml(
    <config xmlns="urn:ietf:params:xml:ns:yang:kea-dhcp4-server"
            xmlns:other="urn:example:other">
      <other:control-sockets><other:socket-type>unix</other:socket-type>
        <other:socket-name>/tmp/kea4.sock</other:socket-name>
      </other:control-sockets>
      <other:hook-library><other:library>/opt/kea/libdhcp_lease_cmds.so</other:library>
      </other:hook-library>
      <other:hook-library><other:library>/opt/kea/libdhcp_stat_cmds.so</other:library>
      </other:hook-library>
      <other:hook-library><other:library>/opt/kea/libdhcp_host_cmds.so</other:library>
      </other:hook-library>
    </config>)xml";
  error.clear();
  auto foreign_namespace = dang::plugins::kea::TranslateConfiguration(
      foreign_namespace_xml, "kea-dhcp4-server", "/tmp/kea4.sock", &error);
  valid &= Check(!foreign_namespace &&
                     error.find("foreign-namespace element control-sockets") !=
                         std::string::npos,
                 "foreign elements acquired Kea configuration semantics");
  constexpr char duplicate_configuration_xml[] = R"xml(
    <data xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
      <config xmlns="urn:ietf:params:xml:ns:yang:kea-dhcp4-server"/>
      <config xmlns="urn:ietf:params:xml:ns:yang:kea-dhcp4-server"/>
    </data>)xml";
  error.clear();
  auto duplicate_configuration = dang::plugins::kea::TranslateConfiguration(
      duplicate_configuration_xml, "kea-dhcp4-server", "/tmp/kea4.sock",
      &error);
  valid &= Check(!duplicate_configuration &&
                     error.find("multiple configuration containers") !=
                         std::string::npos,
                 "duplicate Kea configuration containers were accepted");
  constexpr char absent_configuration_xml[] = R"xml(
    <data xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"/>)xml";
  error.clear();
  auto absent_configuration = dang::plugins::kea::TranslateConfiguration(
      absent_configuration_xml, "kea-dhcp4-server", "/tmp/kea4.sock", &error);
  valid &= Check(!absent_configuration &&
                     error.find("omits the module configuration container") !=
                         std::string::npos,
                 "absent Kea configuration produced an indirect error");
  const std::string oversized_datastore(16U * 1024U * 1024U + 1U, 'x');
  error.clear();
  auto oversized_configuration = dang::plugins::kea::TranslateConfiguration(
      oversized_datastore, "kea-dhcp4-server", "/tmp/kea4.sock", &error);
  valid &= Check(!oversized_configuration &&
                     error.find("exceeds the plugin limit") != std::string::npos,
                 "oversized Kea datastore reached the XML parser");
  constexpr char nested_configuration_xml[] = R"xml(
    <data xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
      <wrapper xmlns="urn:example:wrapper">
        <config xmlns="urn:ietf:params:xml:ns:yang:kea-dhcp4-server"/>
      </wrapper>
    </data>)xml";
  error.clear();
  auto nested_configuration = dang::plugins::kea::TranslateConfiguration(
      nested_configuration_xml, "kea-dhcp4-server", "/tmp/kea4.sock",
      &error);
  valid &= Check(!nested_configuration &&
                     error.find("not a top-level datastore node") !=
                         std::string::npos,
                 "nested Kea configuration container was accepted as a root");
  constexpr char entity_configuration_xml[] = R"xml(
    <!DOCTYPE config [<!ENTITY socket "/tmp/kea4.sock">]>
    <config xmlns="urn:ietf:params:xml:ns:yang:kea-dhcp4-server">
      <control-sockets><socket-type>unix</socket-type>
        <socket-name>&socket;</socket-name></control-sockets>
    </config>)xml";
  error.clear();
  auto entity_configuration = dang::plugins::kea::TranslateConfiguration(
      entity_configuration_xml, "kea-dhcp4-server", "/tmp/kea4.sock",
      &error);
  valid &= Check(!entity_configuration &&
                     error.find("must not contain a DTD") != std::string::npos,
                 "DTD-backed entity expansion was accepted");
  constexpr char mixed_content_xml[] = R"xml(
    <config xmlns="urn:ietf:params:xml:ns:yang:kea-dhcp4-server">
      unexpected text
      <control-sockets><socket-type>unix</socket-type>
        <socket-name>/tmp/kea4.sock</socket-name></control-sockets>
    </config>)xml";
  error.clear();
  auto mixed_content = dang::plugins::kea::TranslateConfiguration(
      mixed_content_xml, "kea-dhcp4-server", "/tmp/kea4.sock", &error);
  valid &= Check(!mixed_content &&
                     error.find("mixed character data in config") !=
                         std::string::npos,
                 "mixed configuration character data was silently ignored");
  constexpr char attributed_configuration_xml[] = R"xml(
    <config xmlns="urn:ietf:params:xml:ns:yang:kea-dhcp4-server">
      <valid-lifetime operation="delete">600</valid-lifetime>
    </config>)xml";
  error.clear();
  auto attributed_configuration =
      dang::plugins::kea::TranslateConfiguration(
          attributed_configuration_xml, "kea-dhcp4-server",
          "/tmp/kea4.sock", &error);
  valid &= Check(!attributed_configuration &&
                     error.find("unsupported attribute operation on "
                                "valid-lifetime") != std::string::npos,
                 "configuration attribute was silently ignored");
  constexpr char duplicate_singleton_xml[] = R"xml(
    <config xmlns="urn:ietf:params:xml:ns:yang:kea-dhcp4-server">
      <valid-lifetime>600</valid-lifetime>
      <valid-lifetime>700</valid-lifetime>
    </config>)xml";
  error.clear();
  auto duplicate_singleton = dang::plugins::kea::TranslateConfiguration(
      duplicate_singleton_xml, "kea-dhcp4-server", "/tmp/kea4.sock", &error);
  valid &= Check(!duplicate_singleton &&
                     error.find("repeats singleton node valid-lifetime under "
                                "config") != std::string::npos,
                 "duplicate singleton leaf acquired list semantics");
  constexpr char scalar_list_xml[] = R"xml(
    <config xmlns="urn:ietf:params:xml:ns:yang:kea-dhcp4-server">
      <hook-library>/opt/kea/libdhcp_lease_cmds.so</hook-library>
    </config>)xml";
  error.clear();
  auto scalar_list = dang::plugins::kea::TranslateConfiguration(
      scalar_list_xml, "kea-dhcp4-server", "/tmp/kea4.sock", &error);
  valid &= Check(!scalar_list &&
                     error.find("invalid collection shape for hook-library") !=
                         std::string::npos,
                 "scalar list entry was accepted");
  constexpr char object_leaf_list_xml[] = R"xml(
    <config xmlns="urn:ietf:params:xml:ns:yang:kea-dhcp4-server">
      <interfaces-config><interfaces><name>dangtest0</name></interfaces>
      </interfaces-config>
    </config>)xml";
  error.clear();
  auto object_leaf_list = dang::plugins::kea::TranslateConfiguration(
      object_leaf_list_xml, "kea-dhcp4-server", "/tmp/kea4.sock", &error);
  valid &= Check(!object_leaf_list &&
                     error.find("invalid collection shape for interfaces") !=
                         std::string::npos,
                 "structured leaf-list entry was accepted");
  constexpr char malformed_json_xml[] = R"xml(
    <config xmlns="urn:ietf:params:xml:ns:yang:kea-dhcp4-server">
      <hook-library><library>/opt/kea/libdhcp_test.so</library>
        <parameters>not-json</parameters></hook-library>
    </config>)xml";
  error.clear();
  auto malformed_json = dang::plugins::kea::TranslateConfiguration(
      malformed_json_xml, "kea-dhcp4-server", "/tmp/kea4.sock", &error);
  valid &= Check(!malformed_json &&
                     error.find("invalid JSON in parameters") !=
                         std::string::npos,
                 "malformed JSON-valued leaf fell back to a string");
  constexpr char scalar_context_xml[] = R"xml(
    <config xmlns="urn:ietf:params:xml:ns:yang:kea-dhcp4-server">
      <user-context>"not-a-map"</user-context>
    </config>)xml";
  error.clear();
  auto scalar_context = dang::plugins::kea::TranslateConfiguration(
      scalar_context_xml, "kea-dhcp4-server", "/tmp/kea4.sock", &error);
  valid &= Check(!scalar_context &&
                     error.find("user-context must contain a JSON object") !=
                         std::string::npos,
                 "scalar user-context was accepted as a JSON map");
  constexpr char malformed_decimal_xml[] = R"xml(
    <config xmlns="urn:ietf:params:xml:ns:yang:kea-dhcp4-server">
      <t1-percent>not-a-decimal</t1-percent>
    </config>)xml";
  error.clear();
  auto malformed_decimal = dang::plugins::kea::TranslateConfiguration(
      malformed_decimal_xml, "kea-dhcp4-server", "/tmp/kea4.sock", &error);
  valid &= Check(!malformed_decimal &&
                     error.find("invalid decimal value for t1-percent") !=
                         std::string::npos,
                 "malformed decimal leaf fell back to a string");
  constexpr char malformed_boolean_xml[] = R"xml(
    <config xmlns="urn:ietf:params:xml:ns:yang:kea-dhcp4-server">
      <authoritative>yes</authoritative>
    </config>)xml";
  error.clear();
  auto malformed_boolean = dang::plugins::kea::TranslateConfiguration(
      malformed_boolean_xml, "kea-dhcp4-server", "/tmp/kea4.sock", &error);
  valid &= Check(!malformed_boolean &&
                     error.find("invalid boolean value for authoritative") !=
                         std::string::npos,
                 "malformed boolean leaf fell back to a string");
  constexpr char negative_unsigned_xml[] = R"xml(
    <config xmlns="urn:ietf:params:xml:ns:yang:kea-dhcp4-server">
      <valid-lifetime>-1</valid-lifetime>
    </config>)xml";
  error.clear();
  auto negative_unsigned = dang::plugins::kea::TranslateConfiguration(
      negative_unsigned_xml, "kea-dhcp4-server", "/tmp/kea4.sock", &error);
  valid &= Check(!negative_unsigned &&
                     error.find("invalid unsigned integer value for "
                                "valid-lifetime") != std::string::npos,
                 "negative unsigned leaf was accepted");
  constexpr char overflowing_unsigned_xml[] = R"xml(
    <config xmlns="urn:ietf:params:xml:ns:yang:kea-dhcp4-server">
      <debuglevel>256</debuglevel>
    </config>)xml";
  error.clear();
  auto overflowing_unsigned = dang::plugins::kea::TranslateConfiguration(
      overflowing_unsigned_xml, "kea-dhcp4-server", "/tmp/kea4.sock", &error);
  valid &= Check(!overflowing_unsigned &&
                     error.find("invalid unsigned integer value for "
                                "debuglevel") != std::string::npos,
                 "out-of-range uint8 leaf was accepted");
  constexpr char deprecated_socket_xml[] = R"xml(
    <config xmlns="urn:ietf:params:xml:ns:yang:kea-dhcp4-server">
      <control-socket><socket-type>unix</socket-type>
        <socket-name>/tmp/kea4.sock</socket-name></control-socket>
      <hook-library><library>/opt/kea/libdhcp_lease_cmds.so</library></hook-library>
      <hook-library><library>/opt/kea/libdhcp_stat_cmds.so</library></hook-library>
      <hook-library><library>/opt/kea/libdhcp_host_cmds.so</library></hook-library>
    </config>)xml";
  error.clear();
  auto deprecated_socket = dang::plugins::kea::TranslateConfiguration(
      deprecated_socket_xml, "kea-dhcp4-server", "/tmp/kea4.sock", &error);
  valid &= Check(deprecated_socket.has_value(),
                 "deprecated managed control-socket was rejected");
  constexpr char missing_hook_xml[] = R"xml(
    <config xmlns="urn:ietf:params:xml:ns:yang:kea-dhcp4-server">
      <control-sockets><socket-type>unix</socket-type>
        <socket-name>/tmp/kea4.sock</socket-name></control-sockets>
      <hook-library><library>/opt/kea/libdhcp_lease_cmds.so</library></hook-library>
      <hook-library><library>/opt/kea/libdhcp_host_cmds.so</library></hook-library>
    </config>)xml";
  error.clear();
  auto missing_hook = dang::plugins::kea::TranslateConfiguration(
      missing_hook_xml, "kea-dhcp4-server", "/tmp/kea4.sock", &error);
  valid &= Check(!missing_hook &&
                     error.find("libdhcp_stat_cmds.so") != std::string::npos,
                 "configuration removing a required hook was accepted");
  std::string wrong_socket_xml(xml);
  const auto socket_name = wrong_socket_xml.find("/tmp/kea4.sock");
  wrong_socket_xml.replace(socket_name, std::string("/tmp/kea4.sock").size(),
                           "/tmp/other.sock");
  error.clear();
  auto wrong_socket = dang::plugins::kea::TranslateConfiguration(
      wrong_socket_xml, "kea-dhcp4-server", "/tmp/kea4.sock", &error);
  valid &= Check(!wrong_socket && error.find("/tmp/kea4.sock") !=
                                      std::string::npos,
                 "configuration replacing the managed socket was accepted");
  const std::string nul_socket_path("/tmp/kea4.sock\0suffix", 21);
  error.clear();
  auto nul_socket_configuration = dang::plugins::kea::TranslateConfiguration(
      xml, "kea-dhcp4-server", nul_socket_path, &error);
  valid &= Check(!nul_socket_configuration &&
                     error.find("contains NUL") != std::string::npos,
                 "embedded-NUL configuration socket path was accepted");
  std::string http_socket_xml(xml);
  const auto socket_type = http_socket_xml.find("<socket-type>unix</socket-type>");
  http_socket_xml.replace(socket_type,
                          std::string("<socket-type>unix</socket-type>").size(),
                          "<socket-type>http</socket-type>");
  error.clear();
  auto http_socket = dang::plugins::kea::TranslateConfiguration(
      http_socket_xml, "kea-dhcp4-server", "/tmp/kea4.sock", &error);
  valid &= Check(!http_socket &&
                     error.find("preserve the managed UNIX control socket") !=
                         std::string::npos,
                 "non-UNIX socket was accepted as the managed socket");
  constexpr char malformed_pool_xml[] = R"xml(
    <config xmlns="urn:ietf:params:xml:ns:yang:kea-dhcp4-server">
      <control-sockets><socket-type>unix</socket-type>
        <socket-name>/tmp/kea4.sock</socket-name></control-sockets>
      <hook-library><library>/opt/kea/libdhcp_lease_cmds.so</library></hook-library>
      <hook-library><library>/opt/kea/libdhcp_stat_cmds.so</library></hook-library>
      <hook-library><library>/opt/kea/libdhcp_host_cmds.so</library></hook-library>
      <subnet4><id>4</id><subnet>192.0.2.0/24</subnet><pool>
        <start-address><unexpected>192.0.2.10</unexpected></start-address>
        <end-address>192.0.2.20</end-address>
      </pool></subnet4>
    </config>)xml";
  error.clear();
  auto malformed_pool = dang::plugins::kea::TranslateConfiguration(
      malformed_pool_xml, "kea-dhcp4-server", "/tmp/kea4.sock", &error);
  valid &= Check(!malformed_pool &&
                     error.find("invalid Kea configuration structure") !=
                         std::string::npos,
                 "malformed pool structure escaped controlled failure");
  const auto& four = dhcp4->arguments.at("Dhcp4");
  const auto& six = dhcp6->arguments.at("Dhcp6");
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
  valid &= Check(four.at("lease-database").at("type") == "memfile" &&
                     four.at("lease-database").at("persist") == false &&
                     !four.at("lease-database").contains("database-type"),
                 "lease database fields were not converted to Kea form");
  valid &= Check(four.at("hosts-databases").is_array(),
                 "singleton hosts-database is not an array");
  valid &= Check(four.at("hosts-databases").at(0).at("type") == "memfile",
                 "host database type was not converted to Kea form");
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
  valid &= Check(four.at("server-tag").is_string() &&
                     four.at("server-tag") == "123" &&
                     four.at("reservations").at(0).at("hostname").is_string() &&
                     four.at("reservations").at(0).at("hostname") == "true",
                 "YANG strings resembling JSON scalars changed type");
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
  valid &= Check(six.at("server-id").is_object() &&
                     six.at("server-id").empty(),
                 "empty DHCPv6 server-id presence became a scalar");
  valid &= Check(six.at("lease-database").at("type") == "memfile" &&
                     six.at("lease-database").at("persist") == false,
                 "DHCPv6 lease database was not converted to Kea form");
  valid &= Check(dang::plugins::kea::ExtractSubnetIds(*dhcp4) ==
                     std::vector<std::uint32_t>{4},
                 "DHCPv4 subnet IDs were not extracted from translated JSON");
  valid &= Check(dang::plugins::kea::ExtractSubnetIds(*dhcp6) ==
                     std::vector<std::uint32_t>{6},
                 "DHCPv6 subnet IDs were not extracted from translated JSON");
  nlohmann::json live_configuration = dhcp4->arguments;
  live_configuration["hash"] = "read-only";
  live_configuration["Dhcp4"]["authoritative"] = false;
  const dang::plugins::kea::ControlQuery matching_configuration =
      [&](std::string_view, std::string_view command,
          const nlohmann::json& arguments,
          std::string*) -> std::optional<nlohmann::json> {
    if (command != "config-get" || !arguments.is_object() ||
        !arguments.empty())
      return std::nullopt;
    return OptionalJson(
        nlohmann::json{{"result", 0}, {"arguments", live_configuration}});
  };
  error.clear();
  valid &= Check(dang::plugins::kea::VerifyLiveConfiguration(
                     *dhcp4, matching_configuration, &error),
                 "Kea-added defaults were mistaken for configuration drift");
  dang::plugins::kea::ServerConfiguration reordered_expected = *dhcp4;
  auto& expected_reservations =
      reordered_expected.arguments["Dhcp4"]["reservations"];
  nlohmann::json second_reservation = expected_reservations.at(0);
  second_reservation["hw-address"] = "00:01:02:03:04:06";
  second_reservation["hostname"] = "second";
  expected_reservations.push_back(second_reservation);
  live_configuration = reordered_expected.arguments;
  auto& live_reservations = live_configuration["Dhcp4"]["reservations"];
  std::reverse(live_reservations.begin(), live_reservations.end());
  error.clear();
  valid &= Check(dang::plugins::kea::VerifyLiveConfiguration(
                     reordered_expected, matching_configuration, &error),
                 "reordered Kea object list was mistaken for drift");
  live_reservations.at(0)["hostname"] = "changed";
  error.clear();
  valid &= Check(!dang::plugins::kea::VerifyLiveConfiguration(
                      reordered_expected, matching_configuration, &error) &&
                     error.find("hostname") != std::string::npos,
                 "changed member of reordered Kea list was accepted");
  live_configuration = reordered_expected.arguments;
  auto& duplicate_reservations = live_configuration["Dhcp4"]["reservations"];
  duplicate_reservations.at(1)["hw-address"] =
      duplicate_reservations.at(0)["hw-address"];
  error.clear();
  valid &= Check(!dang::plugins::kea::VerifyLiveConfiguration(
                      reordered_expected, matching_configuration, &error) &&
                     error.find("duplicate list identity") != std::string::npos,
                 "duplicate live Kea list identity was accepted");
  dang::plugins::kea::ServerConfiguration ordered_expected = *dhcp4;
  auto& expected_subnets = ordered_expected.arguments["Dhcp4"]["subnet4"];
  nlohmann::json second_subnet = expected_subnets.at(0);
  second_subnet["id"] = 5;
  second_subnet["subnet"] = "198.51.100.0/24";
  expected_subnets.push_back(second_subnet);
  live_configuration = ordered_expected.arguments;
  auto& live_subnets = live_configuration["Dhcp4"]["subnet4"];
  std::reverse(live_subnets.begin(), live_subnets.end());
  error.clear();
  valid &= Check(!dang::plugins::kea::VerifyLiveConfiguration(
                      ordered_expected, matching_configuration, &error) &&
                     error.find("subnet4/0/id") != std::string::npos,
                 "reordered user-ordered Kea list was accepted");
  dang::plugins::kea::ServerConfiguration embedded_json_expected = *dhcp4;
  auto& embedded_reservations =
      embedded_json_expected.arguments["Dhcp4"]["hooks-libraries"]
                                      .at(0)["parameters"]["reservations"];
  embedded_reservations = nlohmann::json::array();
  embedded_reservations.push_back({{"id", 1}});
  embedded_reservations.push_back({{"id", 2}});
  live_configuration = embedded_json_expected.arguments;
  auto& live_embedded_reservations =
      live_configuration["Dhcp4"]["hooks-libraries"]
                        .at(0)["parameters"]["reservations"];
  std::reverse(live_embedded_reservations.begin(),
               live_embedded_reservations.end());
  error.clear();
  valid &= Check(!dang::plugins::kea::VerifyLiveConfiguration(
                      embedded_json_expected, matching_configuration,
                      &error) &&
                     error.find("parameters/reservations/0/id") !=
                         std::string::npos,
                 "reordered arbitrary JSON array was treated as a YANG list");
  dang::plugins::kea::ServerConfiguration leaf_list_expected = *dhcp6;
  auto& expected_relay_options =
      leaf_list_expected.arguments["Dhcp6"]["relay-supplied-options"];
  expected_relay_options.push_back("66");
  live_configuration = leaf_list_expected.arguments;
  auto& live_relay_options =
      live_configuration["Dhcp6"]["relay-supplied-options"];
  std::reverse(live_relay_options.begin(), live_relay_options.end());
  error.clear();
  valid &= Check(dang::plugins::kea::VerifyLiveConfiguration(
                     leaf_list_expected, matching_configuration, &error),
                 "reordered system-ordered Kea leaf-list was rejected");
  live_relay_options.at(1) = live_relay_options.at(0);
  error.clear();
  valid &= Check(!dang::plugins::kea::VerifyLiveConfiguration(
                      leaf_list_expected, matching_configuration, &error) &&
                     error.find("duplicate leaf-list value") !=
                         std::string::npos,
                 "duplicate live Kea leaf-list value was accepted");
  live_configuration = dhcp4->arguments;
  live_configuration["hash"] = "read-only";
  live_configuration["Dhcp4"]["authoritative"] = false;
  live_configuration["Dhcp4"]["server-tag"] = "changed";
  error.clear();
  valid &= Check(!dang::plugins::kea::VerifyLiveConfiguration(
                      *dhcp4, matching_configuration, &error) &&
                     error.find("Dhcp4/server-tag") != std::string::npos,
                 "changed live Kea configuration was accepted");
  live_configuration["Dhcp4"].erase("server-tag");
  error.clear();
  valid &= Check(!dang::plugins::kea::VerifyLiveConfiguration(
                      *dhcp4, matching_configuration, &error) &&
                     error.find("omits Dhcp4/server-tag") !=
                         std::string::npos,
                 "missing live Kea configuration was accepted");

  std::size_t authority_checks = 0;
  const dang::plugins::kea::ControlQuery stable_operational_read =
      [&](std::string_view, std::string_view command,
          const nlohmann::json& arguments,
          std::string*) -> std::optional<nlohmann::json> {
    if (command == "config-get") {
      ++authority_checks;
      if (!arguments.is_object() || !arguments.empty()) return std::nullopt;
      return OptionalJson(
          nlohmann::json{{"result", 0}, {"arguments", dhcp4->arguments}});
    }
    if (command == "lease4-get-page" || command == "reservation-get-page")
      return OptionalJson(nlohmann::json{{"result", 3}});
    return std::nullopt;
  };
  std::string failure_path;
  error.clear();
  auto stable_state =
      dang::plugins::kea::CollectAuthoritativeOperationalState(
          *dhcp4, false, {}, stable_operational_read, &failure_path, &error);
  valid &= Check(stable_state && authority_checks == 2 &&
                     failure_path.empty(),
                 "operational read was not bounded by two authority checks");

  authority_checks = 0;
  failure_path.clear();
  error.clear();
  auto query_limited_state =
      dang::plugins::kea::CollectAuthoritativeOperationalState(
          *dhcp4, false, {}, stable_operational_read, &failure_path, &error,
          {.page_size = 256,
           .maximum_pages = 1,
           .maximum_items = 65536,
           .maximum_bytes = 8U * 1024U * 1024U,
           .maximum_duration = std::chrono::milliseconds(100)});
  valid &= Check(!query_limited_state && failure_path == "state/hosts" &&
                     error.find(
                         "operational collection exceeds the query limit") !=
                         std::string::npos,
                 "operational collectors did not share one query limit");

  authority_checks = 0;
  const dang::plugins::kea::ControlQuery drifting_operational_read =
      [&](std::string_view, std::string_view command,
          const nlohmann::json&,
          std::string*) -> std::optional<nlohmann::json> {
    if (command == "config-get") {
      nlohmann::json live = dhcp4->arguments;
      if (++authority_checks == 2) live["Dhcp4"]["server-tag"] = "changed";
      return OptionalJson(
          nlohmann::json{{"result", 0}, {"arguments", std::move(live)}});
    }
    if (command == "lease4-get-page" || command == "reservation-get-page")
      return OptionalJson(nlohmann::json{{"result", 3}});
    return std::nullopt;
  };
  failure_path.clear();
  error.clear();
  auto drifted_state =
      dang::plugins::kea::CollectAuthoritativeOperationalState(
          *dhcp4, false, {}, drifting_operational_read, &failure_path, &error);
  valid &= Check(!drifted_state && authority_checks == 2 &&
                     failure_path == "config" &&
                     error.find("Dhcp4/server-tag") != std::string::npos,
                 "configuration drift during operational collection was published");

  const dang::plugins::kea::ControlQuery failed_operational_read =
      [&](std::string_view, std::string_view command,
          const nlohmann::json&,
          std::string* query_error) -> std::optional<nlohmann::json> {
    if (command == "config-get")
      return OptionalJson(
          nlohmann::json{{"result", 0}, {"arguments", dhcp4->arguments}});
    if (query_error) *query_error = "injected lease failure";
    return std::nullopt;
  };
  failure_path.clear();
  error.clear();
  valid &= Check(
      !dang::plugins::kea::CollectAuthoritativeOperationalState(
          *dhcp4, false, {}, failed_operational_read, &failure_path, &error) &&
          failure_path == "state/leases" &&
          error == "injected lease failure",
      "authoritative operational failure lost its model path");

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

  auto contiguous_identity = leases4;
  contiguous_identity["arguments"]["leases"][0]["hw-address"] =
      "000102030405";
  auto contiguous_state = dang::plugins::kea::TranslateOperationalState(
      "kea-dhcp4-server", contiguous_identity, stats4, hosts4, &error);
  valid &= Check(contiguous_state &&
                     contiguous_state->find(
                         "<hw-address>AAECAwQF</hw-address>") !=
                         std::string::npos,
                 "contiguous DHCPv4 hardware address was not decoded");
  auto malformed_hardware = leases4;
  malformed_hardware["arguments"]["leases"][0]["hw-address"] = "00::01";
  valid &= Check(!dang::plugins::kea::TranslateOperationalState(
                     "kea-dhcp4-server", malformed_hardware, stats4, hosts4,
                     &error) &&
                     error.find("malformed hw-address") != std::string::npos,
                 "doubled hardware-address separator was accepted");
  auto malformed_client = leases4;
  malformed_client["arguments"]["leases"][0]["client-id"] = ":01";
  valid &= Check(!dang::plugins::kea::TranslateOperationalState(
                     "kea-dhcp4-server", malformed_client, stats4, hosts4,
                     &error) &&
                     error.find("malformed client-id") != std::string::npos,
                 "leading client-id separator was accepted");
  auto non_string_client = leases4;
  non_string_client["arguments"]["leases"][0]["client-id"] = 7;
  valid &= Check(!dang::plugins::kea::TranslateOperationalState(
                     "kea-dhcp4-server", non_string_client, stats4, hosts4,
                     &error) &&
                     error.find("malformed client-id") != std::string::npos,
                 "non-string client-id was silently omitted");
  auto non_boolean_fqdn = leases4;
  non_boolean_fqdn["arguments"]["leases"][0]["fqdn-fwd"] = "true";
  valid &= Check(!dang::plugins::kea::TranslateOperationalState(
                     "kea-dhcp4-server", non_boolean_fqdn, stats4, hosts4,
                     &error) &&
                     error.find("fqdn-fwd") != std::string::npos,
                 "non-boolean lease flag was stringified");
  auto oversized_prefix = leases6;
  oversized_prefix["arguments"]["leases"][0]["prefix-len"] = 129;
  valid &= Check(!dang::plugins::kea::TranslateOperationalState(
                     "kea-dhcp6-server", oversized_prefix, stats6, hosts6,
                     &error) &&
                     error.find("prefix-len") != std::string::npos,
                 "out-of-range DHCPv6 prefix length was published");
  auto object_hostname = leases4;
  object_hostname["arguments"]["leases"][0]["hostname"] =
      {{"unexpected", true}};
  valid &= Check(!dang::plugins::kea::TranslateOperationalState(
                     "kea-dhcp4-server", object_hostname, stats4, hosts4,
                     &error) &&
                     error.find("hostname") != std::string::npos,
                 "object-valued lease hostname was stringified");
  auto control_hostname = leases4;
  control_hostname["arguments"]["leases"][0]["hostname"] =
      std::string("bad\x01name", 8);
  valid &= Check(!dang::plugins::kea::TranslateOperationalState(
                     "kea-dhcp4-server", control_hostname, stats4, hosts4,
                     &error) &&
                     error.find("hostname") != std::string::npos &&
                     error.find("valid XML text") != std::string::npos,
                 "XML-forbidden lease hostname was published");
  auto malformed_lease_context = leases4;
  malformed_lease_context["arguments"]["leases"][0]["user-context"] =
      std::string(1, static_cast<char>(0xff));
  valid &= Check(!dang::plugins::kea::TranslateOperationalState(
                     "kea-dhcp4-server", malformed_lease_context, stats4,
                     hosts4, &error) &&
                     error.find("lease user-context") != std::string::npos,
                 "malformed lease user-context escaped controlled failure");
  auto malformed_duid = leases6;
  malformed_duid["arguments"]["leases"][0]["duid"] = "00:01:";
  valid &= Check(!dang::plugins::kea::TranslateOperationalState(
                     "kea-dhcp6-server", malformed_duid, stats6, hosts6,
                     &error) &&
                     error.find("malformed duid") != std::string::npos,
                 "trailing DUID separator was accepted");
  auto empty_identity = leases4;
  empty_identity["arguments"]["leases"][0]["hw-address"] = "";
  valid &= Check(!dang::plugins::kea::TranslateOperationalState(
                     "kea-dhcp4-server", empty_identity, stats4, hosts4,
                     &error) &&
                     error.find("malformed hw-address") != std::string::npos,
                 "empty hardware address was accepted");
  auto malformed_host_identifier = hosts4;
  malformed_host_identifier["arguments"]["hosts"][0]["duid"] = 7;
  valid &= Check(!dang::plugins::kea::TranslateOperationalState(
                     "kea-dhcp4-server", leases4, stats4,
                     malformed_host_identifier, &error) &&
                     error.find("malformed identifier duid") !=
                         std::string::npos,
                 "malformed secondary host identifier was ignored");
  auto empty_host_identifier = hosts4;
  empty_host_identifier["arguments"]["hosts"][0].erase("hw-address");
  empty_host_identifier["arguments"]["hosts"][0]["flex-id"] = "";
  valid &= Check(!dang::plugins::kea::TranslateOperationalState(
                     "kea-dhcp4-server", leases4, stats4,
                     empty_host_identifier, &error) &&
                     error.find("malformed identifier flex-id") !=
                         std::string::npos,
                 "empty host identifier was accepted");
  auto unsupported_dhcp6_identifier = hosts6;
  unsupported_dhcp6_identifier["arguments"]["hosts"][0]["client-id"] =
      "01:02";
  valid &= Check(!dang::plugins::kea::TranslateOperationalState(
                     "kea-dhcp6-server", leases6, stats6,
                     unsupported_dhcp6_identifier, &error) &&
                     error.find("unsupported identifier client-id") !=
                         std::string::npos,
                 "DHCPv4-only identifier was published in DHCPv6 state");
  auto negative_host_subnet = hosts4;
  negative_host_subnet["arguments"]["hosts"][0]["subnet-id"] = -1;
  valid &= Check(!dang::plugins::kea::TranslateOperationalState(
                     "kea-dhcp4-server", leases4, stats4,
                     negative_host_subnet, &error) &&
                     error.find("invalid") != std::string::npos,
                 "negative host subnet ID was published");
  auto object_host_name = hosts4;
  object_host_name["arguments"]["hosts"][0]["hostname"] =
      {{"unexpected", true}};
  valid &= Check(!dang::plugins::kea::TranslateOperationalState(
                     "kea-dhcp4-server", leases4, stats4, object_host_name,
                     &error) &&
                     error.find("hostname") != std::string::npos,
                 "object-valued reservation hostname was stringified");
  auto malformed_host_name = hosts4;
  malformed_host_name["arguments"]["hosts"][0]["hostname"] =
      std::string(1, static_cast<char>(0xff));
  valid &= Check(!dang::plugins::kea::TranslateOperationalState(
                     "kea-dhcp4-server", leases4, stats4,
                     malformed_host_name, &error) &&
                     error.find("hostname") != std::string::npos &&
                     error.find("valid XML text") != std::string::npos,
                 "malformed UTF-8 reservation hostname was published");
  auto oversized_option_code = hosts4;
  oversized_option_code["arguments"]["hosts"][0]["option-data"][0]["code"] =
      256;
  valid &= Check(!dang::plugins::kea::TranslateOperationalState(
                     "kea-dhcp4-server", leases4, stats4,
                     oversized_option_code, &error) &&
                     error.find("option-data") != std::string::npos,
                 "oversized DHCPv4 option code was published");
  auto non_boolean_option_flag = hosts4;
  non_boolean_option_flag["arguments"]["hosts"][0]["option-data"][0]
                         ["csv-format"] = "true";
  valid &= Check(!dang::plugins::kea::TranslateOperationalState(
                     "kea-dhcp4-server", leases4, stats4,
                     non_boolean_option_flag, &error) &&
                     error.find("csv-format") != std::string::npos,
                 "non-boolean option flag was stringified");
  auto malformed_option_context = hosts4;
  malformed_option_context["arguments"]["hosts"][0]["option-data"][0]
                          ["user-context"] =
      std::string(1, static_cast<char>(0xff));
  valid &= Check(!dang::plugins::kea::TranslateOperationalState(
                     "kea-dhcp4-server", leases4, stats4,
                     malformed_option_context, &error) &&
                     error.find("option user-context") != std::string::npos,
                 "malformed option user-context escaped controlled failure");
  auto duplicate_option = hosts4;
  duplicate_option["arguments"]["hosts"][0]["option-data"].push_back(
      duplicate_option["arguments"]["hosts"][0]["option-data"][0]);
  valid &= Check(!dang::plugins::kea::TranslateOperationalState(
                     "kea-dhcp4-server", leases4, stats4, duplicate_option,
                     &error) &&
                     error.find("duplicate key") != std::string::npos,
                 "duplicate reservation option key was published");
  auto duplicate_host_class = hosts4;
  duplicate_host_class["arguments"]["hosts"][0]["client-classes"].push_back(
      "office");
  valid &= Check(!dang::plugins::kea::TranslateOperationalState(
                     "kea-dhcp4-server", leases4, stats4,
                     duplicate_host_class, &error) &&
                     error.find("duplicate client-classes") !=
                         std::string::npos,
                 "duplicate reservation client class was published");
  auto duplicate_option_class = hosts4;
  duplicate_option_class["arguments"]["hosts"][0]["option-data"][0]
                        ["client-classes"].push_back("office");
  valid &= Check(!dang::plugins::kea::TranslateOperationalState(
                     "kea-dhcp4-server", leases4, stats4,
                     duplicate_option_class, &error) &&
                     error.find("duplicate client-classes") !=
                         std::string::npos,
                 "duplicate option client class was published");
  auto duplicate_host_address = hosts6;
  duplicate_host_address["arguments"]["hosts"][0]["ip-addresses"].push_back(
      "2001:db8::50");
  valid &= Check(!dang::plugins::kea::TranslateOperationalState(
                     "kea-dhcp6-server", leases6, stats6,
                     duplicate_host_address, &error) &&
                     error.find("duplicate ip-addresses") != std::string::npos,
                 "duplicate DHCPv6 reservation address was published");

  auto duplicate_leases = leases4;
  duplicate_leases["arguments"]["leases"].push_back(
      duplicate_leases["arguments"]["leases"].front());
  valid &= Check(!dang::plugins::kea::TranslateOperationalState(
                     "kea-dhcp4-server", duplicate_leases, stats4, hosts4,
                     &error) &&
                     error.find("duplicate IP address") != std::string::npos,
                 "duplicate lease keys were accepted as complete state");
  auto duplicate_statistics = stats4;
  duplicate_statistics["arguments"]["result-set"]["rows"].push_back(
      duplicate_statistics["arguments"]["result-set"]["rows"].front());
  valid &= Check(!dang::plugins::kea::TranslateOperationalState(
                     "kea-dhcp4-server", leases4, duplicate_statistics, hosts4,
                     &error) &&
                     error.find("duplicate subnet-id") != std::string::npos,
                 "duplicate statistic keys were accepted as complete state");
  auto duplicate_statistic_columns = stats4;
  duplicate_statistic_columns["arguments"]["result-set"]["columns"][1] =
      "subnet-id";
  valid &= Check(!dang::plugins::kea::TranslateOperationalState(
                     "kea-dhcp4-server", leases4,
                     duplicate_statistic_columns, hosts4, &error) &&
                     error.find("duplicate column subnet-id") !=
                         std::string::npos,
                 "duplicate statistic columns were accepted in final state");
  auto non_string_statistic_column = stats4;
  non_string_statistic_column["arguments"]["result-set"]["columns"][0] = 7;
  valid &= Check(!dang::plugins::kea::TranslateOperationalState(
                     "kea-dhcp4-server", leases4,
                     non_string_statistic_column, hosts4, &error) &&
                     error.find("column name is not a string") !=
                         std::string::npos,
                 "non-string statistic column was ignored");
  auto oversized_statistic = stats4;
  oversized_statistic["arguments"]["result-set"]["rows"][0][1] =
      static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1;
  valid &= Check(!dang::plugins::kea::TranslateOperationalState(
                     "kea-dhcp4-server", leases4, oversized_statistic, hosts4,
                     &error) &&
                     error.find("total-addresses") != std::string::npos,
                 "oversized statistic counter was published");
  auto negative_statistic = stats6;
  negative_statistic["arguments"]["result-set"]["rows"][0][3] = -1;
  valid &= Check(!dang::plugins::kea::TranslateOperationalState(
                     "kea-dhcp6-server", leases6, negative_statistic, hosts6,
                     &error) &&
                     error.find("assigned-nas") != std::string::npos,
                 "negative statistic counter was published");
  auto duplicate_hosts = hosts4;
  duplicate_hosts["arguments"]["hosts"].push_back(
      duplicate_hosts["arguments"]["hosts"].front());
  valid &= Check(!dang::plugins::kea::TranslateOperationalState(
                     "kea-dhcp4-server", leases4, stats4, duplicate_hosts,
                     &error) &&
                     error.find("duplicate key") != std::string::npos,
                 "duplicate host keys were accepted as complete state");

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
  const dang::plugins::kea::ControlQuery slow_empty_lease_page =
      [](std::string_view, std::string_view, const nlohmann::json&,
         std::string*) -> std::optional<nlohmann::json> {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return OptionalJson(nlohmann::json{{"result", 3}});
  };
  error.clear();
  auto late_lease_page = dang::plugins::kea::CollectLeasePages(
      "/tmp/kea4.sock", false, slow_empty_lease_page, &error,
      {.page_size = 1,
       .maximum_pages = 1,
       .maximum_items = 1,
       .maximum_bytes = 1024,
       .maximum_duration = std::chrono::milliseconds(1)});
  valid &= Check(!late_lease_page && error.find("deadline") != std::string::npos,
                 "a lease reply arriving after the deadline was accepted");

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
  valid &= Check(!stalled && error.find("repeated") != std::string::npos,
                 "a repeated lease paging cursor was accepted");

  std::size_t lease_cycle_invocation = 0;
  const dang::plugins::kea::ControlQuery lease_cursor_cycle =
      [&](std::string_view, std::string_view, const nlohmann::json&,
          std::string*) -> std::optional<nlohmann::json> {
    static constexpr const char* addresses[]{"192.0.2.1", "192.0.2.2",
                                              "192.0.2.1"};
    const std::string address = addresses[lease_cycle_invocation++];
    return OptionalJson(nlohmann::json{
        {"result", 0},
        {"arguments",
         {{"count", 1}, {"leases", {{{"ip-address", address}}}}}}});
  };
  error.clear();
  auto lease_cycle = dang::plugins::kea::CollectLeasePages(
      "/tmp/kea4.sock", false, lease_cursor_cycle, &error,
      {.page_size = 1,
       .maximum_pages = 4,
       .maximum_items = 4,
       .maximum_bytes = 1024,
       .maximum_duration = std::chrono::milliseconds(100)});
  valid &= Check(!lease_cycle && error.find("repeated") != std::string::npos &&
                     lease_cycle_invocation == 3,
                 "a non-adjacent lease cursor cycle was not rejected early");

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
  const dang::plugins::kea::ControlQuery slow_empty_host_page =
      [](std::string_view, std::string_view, const nlohmann::json&,
         std::string*) -> std::optional<nlohmann::json> {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return OptionalJson(nlohmann::json{{"result", 3}});
  };
  error.clear();
  auto late_host_page = dang::plugins::kea::CollectHostPages(
      "/tmp/kea4.sock", slow_empty_host_page, &error,
      {.page_size = 1,
       .maximum_pages = 1,
       .maximum_items = 1,
       .maximum_bytes = 1024,
       .maximum_duration = std::chrono::milliseconds(1)});
  valid &= Check(!late_host_page && error.find("deadline") != std::string::npos,
                 "a host reply arriving after the deadline was accepted");

  std::size_t host_cycle_invocation = 0;
  const dang::plugins::kea::ControlQuery host_cursor_cycle =
      [&](std::string_view, std::string_view, const nlohmann::json&,
          std::string*) -> std::optional<nlohmann::json> {
    static constexpr std::uint64_t positions[]{1, 2, 1};
    const auto position = positions[host_cycle_invocation++];
    return OptionalJson(nlohmann::json{
        {"result", 0},
        {"arguments",
         {{"count", 1},
          {"hosts", {{{"subnet-id", 4}, {"hw-address", "00:01"}}}},
          {"next", {{"from", position}, {"source-index", 0}}}}}});
  };
  error.clear();
  auto host_cycle = dang::plugins::kea::CollectHostPages(
      "/tmp/kea4.sock", host_cursor_cycle, &error,
      {.page_size = 1,
       .maximum_pages = 4,
       .maximum_items = 4,
       .maximum_bytes = 1024,
       .maximum_duration = std::chrono::milliseconds(100)});
  valid &= Check(!host_cycle && error.find("repeated") != std::string::npos &&
                     host_cycle_invocation == 3,
                 "a non-adjacent host cursor cycle was not rejected early");

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
  const dang::plugins::kea::ControlQuery slow_empty_statistics =
      [](std::string_view, std::string_view, const nlohmann::json&,
         std::string*) -> std::optional<nlohmann::json> {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return OptionalJson(nlohmann::json{{"result", 3}});
  };
  error.clear();
  auto late_statistics = dang::plugins::kea::CollectStatistics(
      "/tmp/kea4.sock", false, {4}, slow_empty_statistics, &error,
      {.page_size = 1,
       .maximum_pages = 1,
       .maximum_items = 1,
       .maximum_bytes = 1024,
       .maximum_duration = std::chrono::milliseconds(1)});
  valid &= Check(!late_statistics && error.find("deadline") != std::string::npos,
                 "a statistics reply arriving after the deadline was accepted");
  const dang::plugins::kea::ControlQuery missing_statistics =
      [](std::string_view, std::string_view, const nlohmann::json&,
         std::string*) -> std::optional<nlohmann::json> {
    return OptionalJson(nlohmann::json{{"result", 3}});
  };
  error.clear();
  auto absent_configured_statistics =
      dang::plugins::kea::CollectStatistics(
          "/tmp/kea4.sock", false, {4}, missing_statistics, &error);
  valid &= Check(!absent_configured_statistics &&
                     error.find("configured subnet 4") != std::string::npos,
                 "missing configured-subnet statistics were treated as empty");
  const dang::plugins::kea::ControlQuery throwing_query =
      [](std::string_view, std::string_view, const nlohmann::json&,
         std::string*) -> std::optional<nlohmann::json> {
    throw std::runtime_error("query implementation failed");
  };
  error.clear();
  auto throwing_lease_page = dang::plugins::kea::CollectLeasePages(
      "/tmp/kea4.sock", false, throwing_query, &error);
  valid &= Check(!throwing_lease_page &&
                     error.find("query implementation failed") !=
                         std::string::npos,
                 "a throwing lease query escaped the collector");
  error.clear();
  auto throwing_host_page = dang::plugins::kea::CollectHostPages(
      "/tmp/kea4.sock", throwing_query, &error);
  valid &= Check(!throwing_host_page &&
                     error.find("query implementation failed") !=
                         std::string::npos,
                 "a throwing host query escaped the collector");
  error.clear();
  auto throwing_statistics = dang::plugins::kea::CollectStatistics(
      "/tmp/kea4.sock", false, {4}, throwing_query, &error);
  valid &= Check(!throwing_statistics &&
                     error.find("query implementation failed") !=
                         std::string::npos,
                 "a throwing statistics query escaped the collector");

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

  const dang::plugins::kea::ControlQuery wrong_statistics =
      [](std::string_view, std::string_view, const nlohmann::json&,
         std::string*) -> std::optional<nlohmann::json> {
    return OptionalJson(nlohmann::json{
        {"result", 0},
        {"arguments", {{"result-set",
                         {{"columns", {"subnet-id", "total-addresses"}},
                          {"rows", {{99, 32}}}}}}}});
  };
  error.clear();
  auto mismatched_statistics = dang::plugins::kea::CollectStatistics(
      "/tmp/kea4.sock", false, {4}, wrong_statistics, &error,
      {.page_size = 1,
       .maximum_pages = 1,
       .maximum_items = 1,
       .maximum_bytes = 1024,
       .maximum_duration = std::chrono::milliseconds(100)});
  valid &= Check(!mismatched_statistics &&
                     error.find("does not match the query") != std::string::npos,
                 "statistics from the wrong subnet were accepted");

  const dang::plugins::kea::ControlQuery ambiguous_statistics =
      [](std::string_view, std::string_view, const nlohmann::json&,
         std::string*) -> std::optional<nlohmann::json> {
    return OptionalJson(nlohmann::json{
        {"result", 0},
        {"arguments", {{"result-set",
                         {{"columns", {"subnet-id", "subnet-id"}},
                          {"rows", {{4, 4}}}}}}}});
  };
  error.clear();
  auto duplicate_collected_statistic_columns =
      dang::plugins::kea::CollectStatistics(
      "/tmp/kea4.sock", false, {4}, ambiguous_statistics, &error,
      {.page_size = 1,
       .maximum_pages = 1,
       .maximum_items = 1,
       .maximum_bytes = 1024,
       .maximum_duration = std::chrono::milliseconds(100)});
  valid &= Check(!duplicate_collected_statistic_columns &&
                     error.find("duplicate column") != std::string::npos,
                 "ambiguous statistics columns were accepted");

  const dang::plugins::kea::ControlQuery multiple_statistics =
      [](std::string_view, std::string_view, const nlohmann::json&,
         std::string*) -> std::optional<nlohmann::json> {
    return OptionalJson(nlohmann::json{
        {"result", 0},
        {"arguments", {{"result-set",
                         {{"columns", {"subnet-id", "total-addresses"}},
                          {"rows", {{4, 32}, {4, 33}}}}}}}});
  };
  error.clear();
  auto multiple_statistic_rows = dang::plugins::kea::CollectStatistics(
      "/tmp/kea4.sock", false, {4}, multiple_statistics, &error,
      {.page_size = 1,
       .maximum_pages = 1,
       .maximum_items = 2,
       .maximum_bytes = 1024,
       .maximum_duration = std::chrono::milliseconds(100)});
  valid &= Check(!multiple_statistic_rows &&
                     error.find("one complete subnet row") != std::string::npos,
                 "multiple rows for an exact statistics query were accepted");

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
  const nlohmann::json oversized_result{
      {"result", std::numeric_limits<std::uint64_t>::max()}};
  valid &= Check(!dang::plugins::kea::CommandSucceeded(oversized_result,
                                                        &command_reason),
                 "an oversized Kea transaction result was accepted");
  error.clear();
  valid &= Check(!dang::plugins::kea::TranslateOperationalState(
                     "kea-dhcp4-server", oversized_result, stats4, hosts4,
                     &error),
                 "an oversized Kea lease result escaped controlled failure");
  error.clear();
  valid &= Check(!dang::plugins::kea::TranslateOperationalState(
                     "kea-dhcp4-server", leases4, oversized_result, hosts4,
                     &error),
                 "an oversized Kea statistics result escaped controlled failure");
  error.clear();
  valid &= Check(!dang::plugins::kea::TranslateOperationalState(
                     "kea-dhcp4-server", leases4, stats4, oversized_result,
                     &error),
                 "an oversized Kea host result escaped controlled failure");

  const dang::plugins::kea::ControlQuery oversized_page_result =
      [&](std::string_view, std::string_view, const nlohmann::json&,
          std::string*) -> std::optional<nlohmann::json> {
    return std::optional<nlohmann::json>(oversized_result);
  };
  error.clear();
  auto rejected_oversized_page = dang::plugins::kea::CollectLeasePages(
      "/tmp/kea4.sock", false, oversized_page_result, &error,
      {.page_size = 1,
       .maximum_pages = 1,
       .maximum_items = 1,
       .maximum_bytes = 1024,
       .maximum_duration = std::chrono::milliseconds(100)});
  valid &= Check(!rejected_oversized_page,
                 "an oversized Kea page result escaped controlled failure");
  const dang::plugins::kea::ControlQuery oversized_count_page =
      [](std::string_view, std::string_view, const nlohmann::json&,
         std::string*) -> std::optional<nlohmann::json> {
    return OptionalJson(nlohmann::json{
        {"result", 0},
        {"arguments",
         {{"count", std::numeric_limits<std::uint64_t>::max()},
          {"leases", nlohmann::json::array()}}}});
  };
  error.clear();
  auto rejected_oversized_count = dang::plugins::kea::CollectLeasePages(
      "/tmp/kea4.sock", false, oversized_count_page, &error,
      {.page_size = 1,
       .maximum_pages = 1,
       .maximum_items = 1,
       .maximum_bytes = 1024,
       .maximum_duration = std::chrono::milliseconds(100)});
  valid &= Check(!rejected_oversized_count &&
                     error.find("leases/count") != std::string::npos,
                 "an oversized lease count escaped controlled failure");
  const dang::plugins::kea::ControlQuery oversized_host_cursor =
      [](std::string_view, std::string_view, const nlohmann::json&,
         std::string*) -> std::optional<nlohmann::json> {
    return OptionalJson(nlohmann::json{
        {"result", 0},
        {"arguments",
         {{"count", 1},
          {"hosts", {{{"subnet-id", 4}, {"hw-address", "00:01"}}}},
          {"next",
           {{"from", std::numeric_limits<std::uint64_t>::max()},
            {"source-index", 0}}}}}});
  };
  error.clear();
  auto rejected_host_cursor = dang::plugins::kea::CollectHostPages(
      "/tmp/kea4.sock", oversized_host_cursor, &error,
      {.page_size = 1,
       .maximum_pages = 2,
       .maximum_items = 2,
       .maximum_bytes = 1024,
       .maximum_duration = std::chrono::milliseconds(100)});
  valid &= Check(!rejected_host_cursor &&
                     error.find("cursor") != std::string::npos,
                 "an oversized host cursor escaped controlled failure");
  auto oversized_state = leases4;
  oversized_state["arguments"]["leases"][0]["state"] =
      std::numeric_limits<std::uint64_t>::max();
  error.clear();
  valid &= Check(!dang::plugins::kea::TranslateOperationalState(
                     "kea-dhcp4-server", oversized_state, stats4, hosts4,
                     &error) &&
                     error.find("unknown state") != std::string::npos,
                 "an oversized lease state escaped controlled failure");
  auto oversized_type = leases6;
  oversized_type["arguments"]["leases"][0]["type"] =
      std::numeric_limits<std::uint64_t>::max();
  error.clear();
  valid &= Check(!dang::plugins::kea::TranslateOperationalState(
                     "kea-dhcp6-server", oversized_type, stats6, hosts6,
                     &error) &&
                     error.find("unknown lease type") != std::string::npos,
                 "an oversized lease type escaped controlled failure");
  const nlohmann::json malformed_error_text{
      {"result", 1}, {"text", {{"unexpected", true}}}};
  command_reason.clear();
  valid &= Check(!dang::plugins::kea::CommandSucceeded(malformed_error_text,
                                                        &command_reason) &&
                     command_reason.find("non-string text") != std::string::npos,
                 "non-string transaction error text escaped controlled failure");
  error.clear();
  valid &= Check(!dang::plugins::kea::TranslateOperationalState(
                     "kea-dhcp4-server", malformed_error_text, stats4, hosts4,
                     &error) &&
                     error.find("non-string text") != std::string::npos,
                 "non-string operational error text escaped controlled failure");
  const dang::plugins::kea::ControlQuery malformed_error_page =
      [&](std::string_view, std::string_view, const nlohmann::json&,
          std::string*) -> std::optional<nlohmann::json> {
    return std::optional<nlohmann::json>(malformed_error_text);
  };
  error.clear();
  auto rejected_error_page = dang::plugins::kea::CollectLeasePages(
      "/tmp/kea4.sock", false, malformed_error_page, &error,
      {.page_size = 1,
       .maximum_pages = 1,
       .maximum_items = 1,
       .maximum_bytes = 1024,
       .maximum_duration = std::chrono::milliseconds(100)});
  valid &= Check(!rejected_error_page &&
                     error.find("non-string text") != std::string::npos,
                 "non-string page error text escaped controlled failure");

  const std::vector<dang::plugins::kea::ServerConfiguration> transaction_before{
      {"kea-dhcp4-server", "Dhcp4", "/tmp/kea4.sock", {{"image", "before4"}}},
      {"kea-dhcp6-server", "Dhcp6", "/tmp/kea6.sock", {{"image", "before6"}}}};
  const std::vector<dang::plugins::kea::ServerConfiguration> transaction_after{
      {"kea-dhcp4-server", "Dhcp4", "/tmp/kea4.sock", {{"image", "after4"}}},
      {"kea-dhcp6-server", "Dhcp6", "/tmp/kea6.sock", {{"image", "after6"}}}};
  std::vector<std::string> transaction_calls;
  const dang::plugins::kea::ConfigurationCommand ambiguous_apply =
      [&](const dang::plugins::kea::ServerConfiguration& server,
          std::string_view command, std::string* reason) {
        const std::string image = server.arguments.at("image");
        transaction_calls.push_back(image + ":" + std::string(command));
        if (image == "after6") {
          if (reason) *reason = "reply lost after send";
          return false;
        }
        return true;
      };
  std::string failed_module;
  command_reason.clear();
  valid &= Check(!dang::plugins::kea::ApplyWithCompensation(
                     transaction_before, transaction_after, ambiguous_apply,
                     &failed_module, &command_reason) &&
                     failed_module == "kea-dhcp6-server" &&
                     transaction_calls ==
                         std::vector<std::string>{"after4:config-set",
                                                  "after6:config-set",
                                                  "before6:config-set",
                                                  "before4:config-set"},
                 "ambiguous apply did not restore every possible target");
  valid &= Check(command_reason.find("reply lost after send") !=
                     std::string::npos,
                 "ambiguous apply failure reason was not preserved");
  auto only_six_changed = transaction_before;
  only_six_changed[1] = transaction_after[1];
  transaction_calls.clear();
  const dang::plugins::kea::ConfigurationCommand successful_apply =
      [&](const dang::plugins::kea::ServerConfiguration& server,
          std::string_view command, std::string*) {
        transaction_calls.push_back(
            server.arguments.at("image").get<std::string>() + ":" +
            std::string(command));
        return true;
      };
  command_reason.clear();
  valid &= Check(dang::plugins::kea::ApplyWithCompensation(
                     transaction_before, only_six_changed, successful_apply,
                     &failed_module, &command_reason) &&
                     transaction_calls ==
                         std::vector<std::string>{"after6:config-set"},
                 "unchanged DHCPv4 configuration reached the daemon");
  transaction_calls.clear();
  command_reason.clear();
  valid &= Check(dang::plugins::kea::ApplyWithCompensation(
                     transaction_before, transaction_before, successful_apply,
                     &failed_module, &command_reason) &&
                     transaction_calls.empty(),
                 "no-op Kea transaction reached a daemon");
  transaction_calls.clear();
  command_reason.clear();
  valid &= Check(!dang::plugins::kea::ApplyWithCompensation(
                     transaction_before, only_six_changed, ambiguous_apply,
                     &failed_module, &command_reason) &&
                     failed_module == "kea-dhcp6-server" &&
                     transaction_calls ==
                         std::vector<std::string>{"after6:config-set",
                                                  "before6:config-set"},
                 "single-module failure restored an unchanged daemon");
  transaction_calls.clear();
  command_reason.clear();
  valid &= Check(dang::plugins::kea::RollbackChanged(
                     transaction_before, only_six_changed, successful_apply,
                     &failed_module, &command_reason) &&
                     failed_module.empty() && command_reason.empty() &&
                     transaction_calls ==
                         std::vector<std::string>{"before6:config-set"},
                 "explicit rollback reached an unchanged daemon");
  transaction_calls.clear();
  const dang::plugins::kea::ConfigurationCommand failing_rollback =
      [&](const dang::plugins::kea::ServerConfiguration& server,
          std::string_view command, std::string* reason) {
        const std::string image = server.arguments.at("image");
        transaction_calls.push_back(image + ":" + std::string(command));
        if (reason) *reason = "restore rejected for " + image;
        return false;
      };
  command_reason.clear();
  valid &= Check(!dang::plugins::kea::RollbackChanged(
                     transaction_before, transaction_after, failing_rollback,
                     &failed_module, &command_reason) &&
                     failed_module == "kea-dhcp6-server" &&
                     transaction_calls ==
                         std::vector<std::string>{"before6:config-set",
                                                  "before4:config-set"} &&
                     command_reason.find("kea-dhcp6-server: restore rejected") !=
                         std::string::npos &&
                     command_reason.find("kea-dhcp4-server: restore rejected") !=
                         std::string::npos,
                 "explicit rollback did not report and continue after failures");
  transaction_calls.clear();
  command_reason.clear();
  auto rollback_reordered = transaction_after;
  std::swap(rollback_reordered[0], rollback_reordered[1]);
  valid &= Check(!dang::plugins::kea::RollbackChanged(
                     transaction_before, rollback_reordered, successful_apply,
                     &failed_module, &command_reason) &&
                     transaction_calls.empty() && failed_module.empty() &&
                     command_reason.find("pairing") != std::string::npos,
                 "explicit rollback accepted a reordered transaction");
  transaction_calls.clear();
  const dang::plugins::kea::ConfigurationCommand throwing_apply =
      [&](const dang::plugins::kea::ServerConfiguration& server,
          std::string_view command, std::string*) {
        const std::string image = server.arguments.at("image");
        transaction_calls.push_back(image + ":" + std::string(command));
        if (image == "after6")
          throw std::runtime_error("transport implementation failed");
        return true;
      };
  command_reason.clear();
  valid &= Check(!dang::plugins::kea::ApplyWithCompensation(
                     transaction_before, transaction_after, throwing_apply,
                     &failed_module, &command_reason) &&
                     transaction_calls ==
                         std::vector<std::string>{"after4:config-set",
                                                  "after6:config-set",
                                                  "before6:config-set",
                                                  "before4:config-set"} &&
                     command_reason.find("transport implementation failed") !=
                         std::string::npos,
                 "throwing apply escaped reverse compensation");
  auto reordered_after = transaction_after;
  std::swap(reordered_after[0], reordered_after[1]);
  transaction_calls.clear();
  command_reason.clear();
  valid &= Check(!dang::plugins::kea::ApplyWithCompensation(
                     transaction_before, reordered_after, ambiguous_apply,
                     &failed_module, &command_reason) &&
                     transaction_calls.empty() && failed_module.empty() &&
                     command_reason.find("pairing") != std::string::npos,
                 "reordered Kea transaction reached a daemon");
  auto duplicate_before = transaction_before;
  auto duplicate_after = transaction_after;
  duplicate_before[1] = duplicate_before[0];
  duplicate_after[1] = duplicate_after[0];
  transaction_calls.clear();
  command_reason.clear();
  valid &= Check(!dang::plugins::kea::ApplyWithCompensation(
                     duplicate_before, duplicate_after, ambiguous_apply,
                     &failed_module, &command_reason) &&
                     transaction_calls.empty() &&
                     command_reason.find("pairing") != std::string::npos,
                 "duplicate Kea transaction target reached a daemon");

  error.clear();
  auto oversized_request = dang::plugins::kea::SendControlQuery(
      "/tmp/dang-kea-unused.sock", "oversized-request-test",
      {{"payload", std::string(16 * 1024 * 1024, 'x')}}, &error);
  valid &= Check(!oversized_request &&
                     error.find("request exceeds") != std::string::npos,
                 "oversized Kea request reached the transport");
  error.clear();
  auto malformed_request = dang::plugins::kea::SendControlQuery(
      "/tmp/dang-kea-unused.sock", "malformed-request-test",
      {{"payload", std::string(1, static_cast<char>(0xff))}}, &error);
  valid &= Check(!malformed_request &&
                     error.find("invalid Kea request") != std::string::npos,
                 "malformed Kea request escaped controlled failure");
  error.clear();
  auto nul_socket_response = dang::plugins::kea::SendControlQuery(
      nul_socket_path, "nul-socket-test", nlohmann::json::object(), &error);
  valid &= Check(!nul_socket_response &&
                     error.find("contains NUL") != std::string::npos,
                 "embedded-NUL query socket path reached the transport");
  const std::string missing_peer_path =
      "/tmp/dang-kea-missing-" + std::to_string(getpid()) + ".sock";
  unlink(missing_peer_path.c_str());
  error.clear();
  auto missing_response = dang::plugins::kea::SendControlQuery(
      missing_peer_path, "missing-peer-test", nlohmann::json::object(), &error);
  valid &= Check(!missing_response &&
                     error.find("cannot connect") != std::string::npos,
                 "missing Kea peer did not return a connection error");

  const std::string closed_socket =
      "/tmp/dang-kea-closed-" + std::to_string(getpid()) + ".sock";
  const int listener = socket(AF_UNIX, SOCK_STREAM, 0);
  sockaddr_un closed_address{};
  closed_address.sun_family = AF_UNIX;
  std::memcpy(closed_address.sun_path, closed_socket.data(),
              closed_socket.size());
  closed_address.sun_path[closed_socket.size()] = '\0';
  const auto closed_address_size = static_cast<socklen_t>(
      offsetof(sockaddr_un, sun_path) + closed_socket.size() + 1);
  unlink(closed_socket.c_str());
  const bool listening = listener >= 0 &&
      bind(listener, reinterpret_cast<const sockaddr*>(&closed_address),
           closed_address_size) == 0 && listen(listener, 1) == 0;
  const int listen_error = listening ? 0 : errno;
  if (!listening && listen_error != EPERM && listen_error != EACCES)
    valid &= Check(false, "could not create the closed-peer socket fixture");
  if (!listening && (listen_error == EPERM || listen_error == EACCES))
    std::cerr << "closed-peer fixture skipped: UNIX socket bind is denied\n";
  if (listening) {
    std::thread closed_peer([listener]() {
      const int connection = accept(listener, nullptr, nullptr);
      if (connection >= 0) {
        (void)shutdown(connection, SHUT_RDWR);
        close(connection);
      }
    });
    error.clear();
    auto closed_response = dang::plugins::kea::SendControlQuery(
        closed_socket, "closed-peer-test",
        {{"payload", std::string(8 * 1024 * 1024, 'x')}}, &error);
    closed_peer.join();
    valid &= Check(!closed_response && !error.empty(),
                   "a closed Kea peer did not return a controlled error");

    std::thread stalled_peer([listener]() {
      const int connection = accept(listener, nullptr, nullptr);
      if (connection >= 0) {
        std::this_thread::sleep_for(std::chrono::seconds(6));
        close(connection);
      }
    });
    error.clear();
    const auto stalled_start = std::chrono::steady_clock::now();
    auto stalled_response = dang::plugins::kea::SendControlQuery(
        closed_socket, "stalled-peer-test",
        {{"payload", std::string(15 * 1024 * 1024, 'x')}}, &error);
    const auto stalled_elapsed = std::chrono::steady_clock::now() - stalled_start;
    stalled_peer.join();
    valid &= Check(!stalled_response &&
                       error.find("timed out") != std::string::npos &&
                       stalled_elapsed < std::chrono::seconds(6),
                   "a stalled Kea peer escaped the exchange deadline");
  }
  if (listener >= 0) close(listener);
  unlink(closed_socket.c_str());

  transaction_calls.clear();
  const dang::plugins::kea::ConfigurationCommand failed_compensation =
      [&](const dang::plugins::kea::ServerConfiguration& server,
          std::string_view command, std::string* reason) {
        const std::string image = server.arguments.at("image");
        transaction_calls.push_back(image + ":" + std::string(command));
        if (image == "after4" || image == "before4") {
          if (reason)
            *reason = image == "after4" ? "outcome unknown" : "restore failed";
          return false;
        }
        return true;
      };
  command_reason.clear();
  valid &= Check(!dang::plugins::kea::ApplyWithCompensation(
                     transaction_before, transaction_after,
                     failed_compensation, &failed_module, &command_reason) &&
                     transaction_calls ==
                         std::vector<std::string>{"after4:config-set",
                                                  "before4:config-set"} &&
                     command_reason.find("rollback of kea-dhcp4-server failed: "
                                         "restore failed") != std::string::npos,
                 "failed compensation was not attempted and reported");
  transaction_calls.clear();
  const dang::plugins::kea::ConfigurationCommand throwing_compensation =
      [&](const dang::plugins::kea::ServerConfiguration& server,
          std::string_view command, std::string* reason) {
        const std::string image = server.arguments.at("image");
        transaction_calls.push_back(image + ":" + std::string(command));
        if (image == "after6") {
          if (reason) *reason = "apply outcome unknown";
          return false;
        }
        if (image == "before6")
          throw std::runtime_error("rollback implementation failed");
        return true;
      };
  command_reason.clear();
  valid &= Check(!dang::plugins::kea::ApplyWithCompensation(
                     transaction_before, transaction_after,
                     throwing_compensation, &failed_module, &command_reason) &&
                     transaction_calls ==
                         std::vector<std::string>{"after4:config-set",
                                                  "after6:config-set",
                                                  "before6:config-set",
                                                  "before4:config-set"} &&
                     command_reason.find("rollback implementation failed") !=
                         std::string::npos,
                 "throwing rollback stopped remaining compensation");
  return valid ? 0 : 1;
}

// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "kea_adapter.h"

#include <iostream>
#include <string>

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
  return valid ? 0 : 1;
}

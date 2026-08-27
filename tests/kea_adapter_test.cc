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
    <control-sockets><socket-type>unix</socket-type><socket-name>/tmp/kea4.sock</socket-name></control-sockets>
  </config>
  <config xmlns="urn:ietf:params:xml:ns:yang:kea-dhcp6-server">
    <subnet6><id>6</id><pool><prefix>2001:db8:1::100/120</prefix></pool>
      <subnet>2001:db8:1::/64</subnet></subnet6>
    <interfaces-config><interfaces>dangtest0</interfaces></interfaces-config>
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
  valid &= Check(four.at("interfaces-config").at("interfaces").is_array(),
                 "single interface leaf-list is not an array");
  valid &= Check(six.at("subnet6").at(0).at("pools").at(0).at("pool") ==
                     "2001:db8:1::100/120",
                 "IPv6 pool prefix was not translated");
  return valid ? 0 : 1;
}

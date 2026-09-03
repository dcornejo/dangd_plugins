// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "plugins/system/src/system_config.h"

#include <iostream>

int main() {
  constexpr const char* xml = R"xml(<config>
    <system xmlns="urn:ietf:params:xml:ns:yang:ietf-system">
      <hostname>router.example</hostname>
      <ntp><enabled>true</enabled><server><name>primary</name><udp>
        <address>192.0.2.123</address><port>123</port>
      </udp><iburst>true</iburst></server></ntp>
      <dns-resolver><search>example</search><server><name>resolver</name>
        <udp-and-tcp><address>192.0.2.53</address></udp-and-tcp>
      </server><options><timeout>3</timeout><attempts>2</attempts></options>
      </dns-resolver>
      <authentication>
        <user-authentication-order>local-users</user-authentication-order>
        <user><name>alice</name>
          <password>$6$salt$..............................................................
.......................</password>
        </user>
      </authentication>
    </system></config>)xml";
  dang::system::Config config;
  std::string error;
  std::string path;
  if (!dang::system::ParseConfig(xml, &config, &error, &path)) {
    std::cerr << path << ": " << error << '\n';
    return 1;
  }
  if (config.hostname != "router.example" || config.ntp_servers.size() != 1 ||
      !config.ntp_servers[0].iburst || config.dns_servers.size() != 1 ||
      config.dns_timeout != 3 || !config.local_password_authentication ||
      config.users.size() != 1)
    return 2;
  constexpr const char* cleartext = R"xml(<system
    xmlns="urn:ietf:params:xml:ns:yang:ietf-system"><authentication><user>
    <name>bad</name><password>$0$secret</password></user></authentication>
    </system>)xml";
  if (dang::system::ParseConfig(cleartext, &config, &error, &path) ||
      path.find("password") == std::string::npos)
    return 3;
  return 0;
}

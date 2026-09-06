// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/** @file Exercises the loadable RFC 8431 provider inside OS test isolation. */

#include "dangd/plugin_api.h"

#include <dlfcn.h>

#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

int main(int argc, char** argv) {
  if (argc != 5 && argc != 6) {
    std::cerr << "usage: rib_plugin_native_test PLUGIN RIB PREFIX INTERFACE "
                 "[GATEWAY]\n";
    return 2;
  }
  const std::string prefix = argv[3];
  const bool ipv6 = prefix.find(':') != std::string::npos;
  std::ostringstream proposed;
  proposed << "<config><routing-instance xmlns=\"urn:ietf:params:xml:ns:yang:ietf-i2rs-rib\">"
              "<name>native-test</name><rib-list><name>" << argv[2]
           << "</name><address-family>" << (ipv6 ? "ipv6" : "ipv4")
           << "</address-family><route-list><route-index>1</route-index><match><"
           << (ipv6 ? "ipv6><dest-ipv6-prefix>" : "ipv4><dest-ipv4-prefix>")
           << prefix << (ipv6 ? "</dest-ipv6-prefix></ipv6>" : "</dest-ipv4-prefix></ipv4>")
           << "</match><nexthop><nexthop-base>";
  if (argc == 6)
    proposed << '<' << (ipv6 ? "egress-interface-ipv6-address" : "egress-interface-ipv4-address")
             << "><outgoing-interface>" << argv[4] << "</outgoing-interface><"
             << (ipv6 ? "ipv6-address>" : "ipv4-address>") << argv[5] << "</"
             << (ipv6 ? "ipv6-address>" : "ipv4-address>") << "</"
             << (ipv6 ? "egress-interface-ipv6-address>" : "egress-interface-ipv4-address>");
  else
    proposed << "<outgoing-interface>" << argv[4] << "</outgoing-interface>";
  proposed << "</nexthop-base></nexthop><route-attributes><route-preference>10"
              "</route-preference><local-only>false</local-only></route-attributes>"
              "</route-list></rib-list></routing-instance></config>";

  void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  auto init = library ? reinterpret_cast<DangPluginInitV7>(
      dlsym(library, "dang_plugin_init_v7")) : nullptr;
  const DangPluginV7* api = init ? init() : nullptr;
  if (!api) {
    std::cerr << (library ? "missing v7 initializer" : dlerror()) << '\n';
    return 1;
  }
  const DangPluginV1& base = api->v6.v5.v4.v3.v2.v1;
  const std::string candidate = proposed.str();
  constexpr char before[] = "<config/>";
  DangTransactionV1 transaction{before, candidate.c_str(), "[]"};
  DangPluginErrorV1 error{};
  void* prepared = nullptr;
  bool ok = base.prepare(base.context, &transaction, &prepared, &error) &&
      base.validate(base.context, prepared, &error);
  DangHardwareActionV1 action{};
  ok = ok && api->v6.v5.v4.hardware_action_at(base.context, prepared, 0,
                                               &action, &error) &&
      api->v6.v5.v4.apply_hardware_action(base.context, prepared,
                                          action.action_id, &error);
  DangOperationalDataV2 state{};
  ok = ok && api->v6.v5.get_operational_data_v2(base.context, &state, &error) &&
      state.data_xml && std::string_view(state.data_xml).find(prefix) != std::string_view::npos &&
      api->v6.v5.v4.rollback_hardware_action(base.context, prepared,
                                             action.action_id, &error);
  if (!ok)
    std::cerr << (error.message ? error.message : "RIB plugin lifecycle failed")
              << (error.instance_path ? std::string(" at ") + error.instance_path : "")
              << '\n';
  if (prepared) base.release(base.context, prepared);
  dlclose(library);
  return ok ? 0 : 1;
}

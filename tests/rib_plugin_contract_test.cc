// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/plugin_api.h"
#include <dlfcn.h>
#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  auto init = library ? reinterpret_cast<DangPluginInitV7>(
      dlsym(library, "dang_plugin_init_v7")) : nullptr;
  const DangPluginV7* api = init ? init() : nullptr;
  if (!api) return 1;
  const DangPluginV1& base = api->v6.v5.v4.v3.v2.v1;
  constexpr char before[] = "<config/>";
  constexpr char proposed[] = R"(<config><routing-instance xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib"><name>default</name><rib-list><name>100</name><address-family>ipv4</address-family><route-list><route-index>7</route-index><match><ipv4><dest-ipv4-prefix>198.18.0.0/24</dest-ipv4-prefix></ipv4></match><nexthop><nexthop-base><egress-interface-ipv4-address><outgoing-interface>dummy0</outgoing-interface><ipv4-address>192.0.2.1</ipv4-address></egress-interface-ipv4-address></nexthop-base></nexthop><route-attributes><route-preference>10</route-preference><local-only>false</local-only></route-attributes></route-list></rib-list></routing-instance></config>)";
  DangTransactionV1 transaction{before, proposed, "[]"};
  DangPluginErrorV1 error{};
  void* prepared = nullptr;
  bool valid = base.abi_version == DANG_PLUGIN_ABI_V7 &&
      std::string_view(base.plugin_name) == "dang-rib" &&
      api->resource_domain_count(base.context) == 1 &&
      std::string_view(api->resource_domain_at(base.context, 0)) == "routing" &&
      base.prepare(base.context, &transaction, &prepared, &error) &&
      base.validate(base.context, prepared, &error);
  DangHardwareActionV1 action{};
  valid = valid && api->v6.v5.v4.hardware_action_count(base.context, prepared) == 1 &&
      api->v6.v5.v4.hardware_action_at(base.context, prepared, 0, &action, &error) &&
      action.action_id && std::string_view(action.action_id) == "routes";
  DangOperationalDataV2 state{};
  valid = valid && api->v6.v5.get_operational_data_v2(base.context, &state, &error) &&
      state.complete == 0;
  if (prepared) base.release(base.context, prepared);
  dlclose(library);
  if (!valid) std::cerr << (error.message ? error.message : "RIB plugin contract failed") << '\n';
  return valid ? 0 : 1;
}

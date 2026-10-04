// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/plugin_api.h"
#include <dlfcn.h>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  const char* registry_path = std::getenv("DANG_RIB_REGISTRY_FILE");
  std::error_code ignored;
  if (registry_path) std::filesystem::remove(registry_path, ignored);
  void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  auto init = library ? reinterpret_cast<DangPluginInitV8>(
      dlsym(library, "dang_plugin_init_v8")) : nullptr;
  const DangPluginV8* api = init ? init() : nullptr;
  if (!api) return 1;
  const DangPluginV7& v7 = api->v7;
  const DangPluginV1& base = v7.v6.v5.v4.v3.v2.v1;
  constexpr char before[] = "<config/>";
  constexpr char proposed[] = R"(<config><routing-instance xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib"><name>default</name><rib-list><name>ipv4-100</name><address-family>ipv4-address-family</address-family><route-list><route-index>7</route-index><match><ipv4><dest-ipv4-prefix>198.18.0.0/24</dest-ipv4-prefix></ipv4></match><nexthop><nexthop-base><egress-interface-ipv4-address><outgoing-interface>dummy0</outgoing-interface><ipv4-address>192.0.2.1</ipv4-address></egress-interface-ipv4-address></nexthop-base></nexthop><route-attributes><route-preference>10</route-preference><local-only>false</local-only></route-attributes></route-list></rib-list></routing-instance></config>)";
  DangTransactionV1 transaction{before, proposed, "[]"};
  DangPluginErrorV1 error{};
  void* prepared = nullptr;
  bool valid = base.abi_version == DANG_PLUGIN_ABI_V8 &&
      std::string_view(base.plugin_name) == "dang-rib" &&
      v7.resource_domain_count(base.context) == 1 &&
      std::string_view(v7.resource_domain_at(base.context, 0)) == "routing" &&
      base.prepare(base.context, &transaction, &prepared, &error) &&
      base.validate(base.context, prepared, &error);
  DangHardwareActionV1 action{};
  valid = valid && v7.v6.v5.v4.hardware_action_count(base.context, prepared) == 1 &&
      v7.v6.v5.v4.hardware_action_at(base.context, prepared, 0, &action, &error) &&
      action.action_id && std::string_view(action.action_id) == "routes";
  DangOperationalDataV2 state{};
  valid = valid && v7.v6.v5.get_operational_data_v2(base.context, &state, &error) &&
      state.complete == 0 && state.data_xml &&
      std::string_view(state.data_xml).find("routing-instance") != std::string_view::npos;
  if (prepared) base.release(base.context, prepared);

  std::string unsupported(proposed);
  const std::string local_only = "<local-only>false</local-only>";
  const std::size_t local_only_position = unsupported.find(local_only);
  if (local_only_position == std::string::npos) {
    valid = false;
  } else {
    unsupported.replace(local_only_position, local_only.size(),
                        "<local-only>true</local-only>");
    DangTransactionV1 unsupported_transaction{before, unsupported.c_str(),
                                               "[]"};
    void* unsupported_prepared = nullptr;
    error = {};
    valid = valid &&
        !base.prepare(base.context, &unsupported_transaction,
                      &unsupported_prepared, &error) &&
        unsupported_prepared == nullptr && error.instance_path &&
        std::string_view(error.instance_path).ends_with(
            "/route-attributes/local-only");
  }

  constexpr char nh_add_xml[] = R"(<nh-add xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib"><rib-name>ipv4-100</rib-name><nexthop-base><ipv4-address>192.0.2.1</ipv4-address></nexthop-base></nh-add>)";
  DangOperationV1 operation{"ietf-i2rs-rib", "nh-add",
                            "/ietf-i2rs-rib:nh-add", nh_add_xml};
  DangOperationResultV1 operation_result{};
  valid = valid && v7.v6.v5.v4.v3.v2.invoke(
      base.context, &operation, &operation_result, &error) &&
      operation_result.output_xml &&
      std::string_view(operation_result.output_xml).find(">1</nexthop-id>") !=
          std::string_view::npos;
  state = {};
  valid = valid && v7.v6.v5.get_operational_data_v2(
      base.context, &state, &error) && state.data_xml &&
      std::string_view(state.data_xml).find(
          "<nexthop-member-id>1</nexthop-member-id>") !=
          std::string_view::npos;
  constexpr char referenced[] = R"(<config><routing-instance xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib"><name>default</name><rib-list><name>ipv4-100</name><address-family>ipv4-address-family</address-family><route-list><route-index>8</route-index><match><ipv4><dest-ipv4-prefix>198.18.1.0/24</dest-ipv4-prefix></ipv4></match><nexthop><nexthop-base><nexthop-ref>1</nexthop-ref></nexthop-base></nexthop><route-attributes><route-preference>10</route-preference><local-only>false</local-only></route-attributes></route-list></rib-list></routing-instance></config>)";
  DangTransactionV1 referenced_transaction{before, referenced, "[]"};
  void* referenced_prepared = nullptr;
  valid = valid && base.prepare(base.context, &referenced_transaction,
                                 &referenced_prepared, &error);
  constexpr char nh_delete_xml[] = R"(<nh-delete xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib"><rib-name>ipv4-100</rib-name><nexthop-id>1</nexthop-id></nh-delete>)";
  operation = {"ietf-i2rs-rib", "nh-delete", "/ietf-i2rs-rib:nh-delete",
               nh_delete_xml};
  operation_result = {};
  valid = valid && v7.v6.v5.v4.v3.v2.invoke(
      base.context, &operation, &operation_result, &error) &&
      operation_result.output_xml &&
      std::string_view(operation_result.output_xml).find("referenced by a route") !=
          std::string_view::npos;
  DangAppliedConfigurationV1 applied{};
  valid = valid && v7.v6.reconcile_applied_configuration(
      base.context, referenced_prepared, referenced, &applied, &error) &&
      applied.applied_xml == referenced;
  if (referenced_prepared)
    base.release(base.context, referenced_prepared);
  operation_result = {};
  valid = valid && v7.v6.v5.v4.v3.v2.invoke(
      base.context, &operation, &operation_result, &error) &&
      operation_result.output_xml &&
      std::string_view(operation_result.output_xml).find("referenced by a route") !=
          std::string_view::npos;
  applied = {};
  valid = valid && v7.v6.reconcile_applied_configuration(
      base.context, nullptr, before, &applied, &error);
  operation_result = {};
  valid = valid && v7.v6.v5.v4.v3.v2.invoke(
      base.context, &operation, &operation_result, &error) &&
      operation_result.output_xml &&
      std::string_view(operation_result.output_xml).find(">true</result>") !=
          std::string_view::npos;
  dlclose(library);
  if (registry_path) std::filesystem::remove(registry_path, ignored);
  if (!valid) std::cerr << (error.message ? error.message : "RIB plugin contract failed") << '\n';
  return valid ? 0 : 1;
}

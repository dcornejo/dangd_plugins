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
  auto init = library ? reinterpret_cast<DangPluginInitV8>(
      dlsym(library, "dang_plugin_init_v8")) : nullptr;
  const DangPluginV8* api = init ? init() : nullptr;
  if (!api) {
    std::cerr << (library ? "missing v8 initializer" : dlerror()) << '\n';
    return 1;
  }
  const DangPluginV7& v7 = api->v7;
  const DangPluginV1& base = v7.v6.v5.v4.v3.v2.v1;
  const std::string rib_add_input =
      "<rib-add xmlns=\"urn:ietf:params:xml:ns:yang:ietf-i2rs-rib\"><name>" +
      std::string(argv[2]) + "</name><address-family>" +
      (ipv6 ? "ipv6" : "ipv4") + "</address-family></rib-add>";
  DangOperationV1 rib_add{"ietf-i2rs-rib", "rib-add",
                          "/ietf-i2rs-rib:rib-add", rib_add_input.c_str()};
  DangOperationResultV1 rib_add_result{};
  const std::string candidate = proposed.str();
  constexpr char before[] = "<config/>";
  DangTransactionV1 transaction{before, candidate.c_str(), "[]"};
  DangPluginErrorV1 error{};
  void* prepared = nullptr;
  bool ok = v7.v6.v5.v4.v3.v2.invoke(base.context, &rib_add,
                                        &rib_add_result, &error) &&
      rib_add_result.output_xml &&
      std::string_view(rib_add_result.output_xml).find(">true</result>") !=
          std::string_view::npos &&
      base.prepare(base.context, &transaction, &prepared, &error) &&
      base.validate(base.context, prepared, &error);
  const std::string nh_add_input =
      "<nh-add xmlns=\"urn:ietf:params:xml:ns:yang:ietf-i2rs-rib\"><rib-name>" +
      std::string(argv[2]) + "</rib-name><nexthop-base><outgoing-interface>" +
      argv[4] + "</outgoing-interface></nexthop-base></nh-add>";
  DangOperationV1 nh_operation{"ietf-i2rs-rib", "nh-add",
                               "/ietf-i2rs-rib:nh-add", nh_add_input.c_str()};
  DangOperationResultV1 nh_result{};
  ok = ok && v7.v6.v5.v4.v3.v2.invoke(base.context, &nh_operation,
                                        &nh_result, &error) &&
      nh_result.output_xml &&
      std::string_view(nh_result.output_xml).find(">1</nexthop-id>") !=
          std::string_view::npos;
  const std::string nh_delete_input =
      "<nh-delete xmlns=\"urn:ietf:params:xml:ns:yang:ietf-i2rs-rib\"><rib-name>" +
      std::string(argv[2]) +
      "</rib-name><nexthop-id>1</nexthop-id></nh-delete>";
  nh_operation = {"ietf-i2rs-rib", "nh-delete", "/ietf-i2rs-rib:nh-delete",
                  nh_delete_input.c_str()};
  nh_result = {};
  ok = ok && v7.v6.v5.v4.v3.v2.invoke(base.context, &nh_operation,
                                        &nh_result, &error) &&
      nh_result.output_xml &&
      std::string_view(nh_result.output_xml).find(">true</result>") !=
          std::string_view::npos;
  DangHardwareActionV1 action{};
  ok = ok && v7.v6.v5.v4.hardware_action_at(base.context, prepared, 0,
                                               &action, &error) &&
      v7.v6.v5.v4.apply_hardware_action(base.context, prepared,
                                          action.action_id, &error);
  DangOperationalDataV2 state{};
  ok = ok && v7.v6.v5.get_operational_data_v2(base.context, &state, &error) &&
      state.data_xml && std::string_view(state.data_xml).find(prefix) != std::string_view::npos &&
      v7.v6.v5.v4.rollback_hardware_action(base.context, prepared,
                                             action.action_id, &error);
  std::ostringstream rpc;
  rpc << "<route-add xmlns=\"urn:ietf:params:xml:ns:yang:ietf-i2rs-rib\">"
         "<return-failure-detail>true</return-failure-detail><rib-name>"
      << argv[2] << "</rib-name><routes><route-list><route-index>1</route-index>"
      << "<match><" << (ipv6 ? "ipv6><dest-ipv6-prefix>" : "ipv4><dest-ipv4-prefix>")
      << prefix << (ipv6 ? "</dest-ipv6-prefix></ipv6>" : "</dest-ipv4-prefix></ipv4>")
      << "</match><route-attributes><route-preference>10</route-preference>"
         "<local-only>false</local-only></route-attributes><nexthop><nexthop-base>";
  if (argc == 6)
    rpc << '<' << (ipv6 ? "egress-interface-ipv6-address" : "egress-interface-ipv4-address")
        << "><outgoing-interface>" << argv[4] << "</outgoing-interface><"
        << (ipv6 ? "ipv6-address>" : "ipv4-address>") << argv[5] << "</"
        << (ipv6 ? "ipv6-address>" : "ipv4-address>") << "</"
        << (ipv6 ? "egress-interface-ipv6-address>" : "egress-interface-ipv4-address>");
  else
    rpc << "<outgoing-interface>" << argv[4] << "</outgoing-interface>";
  rpc << "</nexthop-base></nexthop></route-list></routes></route-add>";
  const std::string rpc_input = rpc.str();
  DangOperationV1 operation{"ietf-i2rs-rib", "route-add", "/ietf-i2rs-rib:route-add",
                            rpc_input.c_str()};
  DangOperationResultV1 operation_result{};
  ok = ok && v7.v6.v5.v4.v3.v2.invoke(base.context, &operation,
                                        &operation_result, &error) &&
      operation_result.output_xml &&
      std::string_view(operation_result.output_xml).find(">1</success-count>") !=
          std::string_view::npos;
  DangNotificationV1 route_event{};
  ok = ok && api->next_notification(base.context, &route_event, &error) == 1 &&
      route_event.module_name &&
      std::string_view(route_event.module_name) == "ietf-i2rs-rib" &&
      route_event.notification_name &&
      std::string_view(route_event.notification_name) == "route-change" &&
      route_event.content_xml &&
      std::string_view(route_event.content_xml).find(prefix) !=
          std::string_view::npos &&
      std::string_view(route_event.content_xml).find(
          "<route-installed-state>installed</route-installed-state>") !=
          std::string_view::npos;
  std::ostringstream update;
  update << "<route-update xmlns=\"urn:ietf:params:xml:ns:yang:ietf-i2rs-rib\">"
            "<rib-name>" << argv[2]
         << "</rib-name><input-routes><route-list><route-index>1</route-index><match><"
         << (ipv6 ? "ipv6><dest-ipv6-prefix>" : "ipv4><dest-ipv4-prefix>")
         << prefix << (ipv6 ? "</dest-ipv6-prefix></ipv6>" : "</dest-ipv4-prefix></ipv4>")
         << "</match><updated-route-attr><route-preference>20</route-preference>"
            "<local-only>false</local-only></updated-route-attr></route-list>"
            "</input-routes></route-update>";
  const std::string update_input = update.str();
  operation = {"ietf-i2rs-rib", "route-update",
               "/ietf-i2rs-rib:route-update", update_input.c_str()};
  operation_result = {};
  ok = ok && v7.v6.v5.v4.v3.v2.invoke(base.context, &operation,
                                        &operation_result, &error) &&
      operation_result.output_xml &&
      std::string_view(operation_result.output_xml).find(">1</success-count>") !=
          std::string_view::npos;
  std::ostringstream deletion;
  deletion << "<route-delete xmlns=\"urn:ietf:params:xml:ns:yang:ietf-i2rs-rib\">"
              "<return-failure-detail>true</return-failure-detail><rib-name>"
           << argv[2] << "</rib-name><routes><route-list><route-index>1</route-index><match><"
           << (ipv6 ? "ipv6><dest-ipv6-prefix>" : "ipv4><dest-ipv4-prefix>")
           << prefix << (ipv6 ? "</dest-ipv6-prefix></ipv6>" : "</dest-ipv4-prefix></ipv4>")
           << "</match></route-list></routes></route-delete>";
  const std::string deletion_input = deletion.str();
  const bool may_empty_rib = std::string_view(argv[2]) != "0";
  const std::string rib_delete_input =
      "<rib-delete xmlns=\"urn:ietf:params:xml:ns:yang:ietf-i2rs-rib\"><name>" +
      std::string(argv[2]) + "</name></rib-delete>";
  operation = {"ietf-i2rs-rib", may_empty_rib ? "rib-delete" : "route-delete",
               may_empty_rib ? "/ietf-i2rs-rib:rib-delete"
                             : "/ietf-i2rs-rib:route-delete",
               may_empty_rib ? rib_delete_input.c_str() : deletion_input.c_str()};
  operation_result = {};
  ok = ok && v7.v6.v5.v4.v3.v2.invoke(base.context, &operation,
                                        &operation_result, &error) &&
      operation_result.output_xml &&
      std::string_view(operation_result.output_xml).find(
          may_empty_rib ? ">true</result>" : ">1</success-count>") !=
          std::string_view::npos;
  if (!ok)
    std::cerr << (error.message ? error.message : "RIB plugin lifecycle failed")
              << (error.instance_path ? std::string(" at ") + error.instance_path : "")
              << '\n';
  if (prepared) base.release(base.context, prepared);
  dlclose(library);
  return ok ? 0 : 1;
}

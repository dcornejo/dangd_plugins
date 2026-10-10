// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/** @file Exercises a weighted RFC 8431 transaction through the loadable plugin. */

#include "dangd/plugin_api.h"

#include <dlfcn.h>

#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

bool Invoke(const DangPluginV7& api, const DangPluginV1& base,
            const std::string& name, const std::string& input,
            std::string_view expected, DangPluginErrorV1* error) {
  const std::string path = "/ietf-i2rs-rib:" + name;
  DangOperationV1 operation{"ietf-i2rs-rib", name.c_str(), path.c_str(),
                            input.c_str()};
  DangOperationResultV1 result{};
  return api.v6.v5.v4.v3.v2.invoke(base.context, &operation, &result, error) &&
         result.output_xml &&
         std::string_view(result.output_xml).find(expected) !=
             std::string_view::npos;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 8) {
    std::cerr << "usage: rib_plugin_load_balance_native_test PLUGIN RIB "
                 "PREFIX INTERFACE GATEWAY INTERFACE GATEWAY\n";
    return 2;
  }
  const std::string prefix = argv[3];
  const bool ipv6 = prefix.find(':') != std::string::npos;
  const std::string family =
      ipv6 ? "ipv6-address-family" : "ipv4-address-family";
  const std::string rib_name = (ipv6 ? "ipv6-" : "ipv4-") +
                               std::string(argv[2]);

  void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  auto init = library ? reinterpret_cast<DangPluginInitV8>(
                            dlsym(library, "dang_plugin_init_v8"))
                      : nullptr;
  const DangPluginV8* api = init ? init() : nullptr;
  if (!api) {
    std::cerr << (library ? "missing v8 initializer" : dlerror()) << '\n';
    return 1;
  }
  const DangPluginV7& v7 = api->v7;
  const DangPluginV1& base = v7.v6.v5.v4.v3.v2.v1;
  DangPluginErrorV1 error{};
  bool ok = true;
  std::string stage = "rib-add";

  const std::string rib_add =
      "<rib-add xmlns=\"urn:ietf:params:xml:ns:yang:ietf-i2rs-rib\">"
      "<name>" +
      rib_name + "</name><address-family>" + family +
      "</address-family></rib-add>";
  ok = Invoke(v7, base, "rib-add", rib_add, ">true</result>", &error);

  for (unsigned index = 0; ok && index < 2U; ++index) {
    stage = "nh-add " + std::to_string(index + 1U);
    const char* interface = argv[index == 0U ? 4 : 6];
    const char* gateway = argv[index == 0U ? 5 : 7];
    std::ostringstream input;
    input << "<nh-add xmlns=\"urn:ietf:params:xml:ns:yang:ietf-i2rs-rib\">"
             "<rib-name>"
          << rib_name << "</rib-name><nexthop-base><"
          << (ipv6 ? "egress-interface-ipv6-address"
                   : "egress-interface-ipv4-address")
          << "><outgoing-interface>" << interface << "</outgoing-interface><"
          << (ipv6 ? "ipv6-address" : "ipv4-address") << '>' << gateway
          << "</" << (ipv6 ? "ipv6-address" : "ipv4-address") << "></"
          << (ipv6 ? "egress-interface-ipv6-address"
                   : "egress-interface-ipv4-address")
          << "></nexthop-base></nh-add>";
    ok = Invoke(v7, base, "nh-add", input.str(),
                ">" + std::to_string(index + 1U) + "</nexthop-id>",
                &error);
  }

  // Establish a quiet native baseline before the managed transaction. This
  // makes the later drain prove that two kernel ECMP paths confirm one
  // modeled route instead of creating per-path synthetic notifications.
  DangNotificationV1 baseline_event{};
  if (ok) stage = "notification baseline";
  ok = ok && api->next_notification(base.context, &baseline_event, &error) == 0;

  std::ostringstream proposed;
  proposed
      << "<config><routing-instance "
         "xmlns=\"urn:ietf:params:xml:ns:yang:ietf-i2rs-rib\">"
         "<name>default</name><rib-list><name>"
      << rib_name << "</name><address-family>" << family
      << "</address-family><route-list><route-index>1</route-index><match><"
      << (ipv6 ? "ipv6><dest-ipv6-prefix>" : "ipv4><dest-ipv4-prefix>")
      << prefix
      << (ipv6 ? "</dest-ipv6-prefix></ipv6>"
               : "</dest-ipv4-prefix></ipv4>")
      << "</match><nexthop><nexthop-lb>"
         "<nexthop-list><nexthop-member-id>1</nexthop-member-id>"
         "<nexthop-lb-weight>2</nexthop-lb-weight></nexthop-list>"
         "<nexthop-list><nexthop-member-id>2</nexthop-member-id>"
         "<nexthop-lb-weight>3</nexthop-lb-weight></nexthop-list>"
         "</nexthop-lb></nexthop><route-attributes>"
         "<route-preference>10</route-preference><local-only>false</local-only>"
         "</route-attributes></route-list></rib-list></routing-instance>"
         "</config>";

  const std::string candidate = proposed.str();
  constexpr char before[] = "<config/>";
  DangTransactionV1 transaction{before, candidate.c_str(), "[]"};
  void* prepared = nullptr;
  if (ok) stage = "prepare/validate";
  ok = ok && base.prepare(base.context, &transaction, &prepared, &error) &&
       base.validate(base.context, prepared, &error);
  DangHardwareActionV1 action{};
  if (ok) stage = "hardware apply";
  ok = ok &&
       v7.v6.v5.v4.hardware_action_at(base.context, prepared, 0, &action,
                                      &error) &&
       v7.v6.v5.v4.apply_hardware_action(base.context, prepared,
                                         action.action_id, &error);

  DangAppliedConfigurationV1 applied{};
  if (ok) stage = "applied-state reconciliation";
  ok = ok && v7.v6.reconcile_applied_configuration(
                 base.context, prepared, candidate.c_str(), &applied, &error) &&
       applied.applied_xml &&
       std::string_view(applied.applied_xml) == candidate;

  DangOperationalDataV2 installed{};
  if (ok) stage = "installed operational observation";
  ok = ok && v7.v6.v5.get_operational_data_v2(base.context, &installed,
                                               &error) &&
       installed.data_xml &&
       std::string_view(installed.data_xml).find(prefix) !=
           std::string_view::npos &&
       std::string_view(installed.data_xml).find("<nexthop-lb>") !=
           std::string_view::npos &&
       std::string_view(installed.data_xml).find(
           "<nexthop-member-id>1</nexthop-member-id>"
           "<nexthop-lb-weight>2</nexthop-lb-weight>") !=
           std::string_view::npos &&
       std::string_view(installed.data_xml).find(
           "<nexthop-member-id>2</nexthop-member-id>"
           "<nexthop-lb-weight>3</nexthop-lb-weight>") !=
           std::string_view::npos;

  unsigned route_change_count = 0;
  unsigned resolution_change_count = 0;
  bool notifications_drained = false;
  if (ok) stage = "modeled notification projection";
  for (unsigned attempt = 0; ok && attempt < 8U; ++attempt) {
    DangNotificationV1 event{};
    const int available =
        api->next_notification(base.context, &event, &error);
    if (available < 0) {
      ok = false;
      break;
    }
    if (available == 0) {
      notifications_drained = true;
      break;
    }
    if (event.notification_name &&
        std::string_view(event.notification_name) == "route-change") {
      ++route_change_count;
      ok = event.content_xml &&
           std::string_view(event.content_xml).find(
               "<route-index>1</route-index>") != std::string_view::npos &&
           std::string_view(event.content_xml).find(prefix) !=
               std::string_view::npos;
    } else if (event.notification_name &&
               std::string_view(event.notification_name) ==
                   "nexthop-resolution-status-change") {
      ++resolution_change_count;
    } else {
      ok = false;
    }
  }
  ok = ok && notifications_drained && route_change_count == 1U &&
       resolution_change_count == 2U;
  if (ok) stage = "hardware rollback";
  ok = ok && v7.v6.v5.v4.rollback_hardware_action(
                 base.context, prepared, action.action_id, &error);
  DangOperationalDataV2 rolled_back{};
  if (ok) stage = "rolled-back operational observation";
  ok = ok && v7.v6.v5.get_operational_data_v2(base.context, &rolled_back,
                                               &error) &&
       rolled_back.data_xml &&
       std::string_view(rolled_back.data_xml).find(prefix) ==
           std::string_view::npos;

  if (!ok)
    std::cerr << stage << ": "
              << (error.message ? error.message
                                : "weighted RIB plugin transaction failed")
              << (error.instance_path
                      ? std::string(" at ") + error.instance_path
                      : "")
              << '\n';
  if (prepared) base.release(base.context, prepared);
  dlclose(library);
  return ok ? 0 : 1;
}

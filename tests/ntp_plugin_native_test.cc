// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0
/** @file Explicit loaded-plugin test with disposable files and systemd units.
 */
#include <dlfcn.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

#include "dangd/plugin_api.h"
#include "plugins/ntp/src/linux/chrony_service.h"

std::string Read(const std::string& path) {
  std::ifstream in(path);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
int main(int argc, char** argv) {
  if (argc != 3 ||
      (std::string_view(argv[2]) != "dangd-ntp-native-test.service" &&
       std::string_view(argv[2]) != "dangd-ntp-native-fail-test.service"))
    return 2;
  const bool expect_failure =
      std::string_view(argv[2]) == "dangd-ntp-native-fail-test.service";
  dang::ntp::SystemdChronyService service(argv[2]);
  std::string message;
  if (!service.SetActive(false, &message)) {
    std::cerr << message << '\n';
    return 1;
  }
  char pattern[] = "/tmp/dang-ntp-plugin-XXXXXX";
  const char* created = mkdtemp(pattern);
  if (!created) return 1;
  const std::string root(created);
  {
    std::ofstream out(root + "/managed.conf");
    out << "original fragment";
  }
  {
    std::ofstream out(root + "/managed.keys");
    out << "original keys";
  }
  setenv("DANG_NTP_NATIVE_TEST_ROOT", root.c_str(), 1);
  setenv("DANG_NTP_NATIVE_TEST_UNIT", argv[2], 1);
  void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  auto init = library ? reinterpret_cast<DangPluginInitV7>(
                            dlsym(library, "dang_plugin_init_v7"))
                      : nullptr;
  const auto* plugin = init ? init() : nullptr;
  if (!plugin) {
    std::cerr << "cannot load plugin\n";
    return 1;
  }
  const auto& base = plugin->v6.v5.v4.v3.v2.v1;
  const auto& actions = plugin->v6.v5.v4;
  DangTransactionV1 input{
      "<ntp "
      "xmlns='urn:ietf:params:xml:ns:yang:ietf-ntp'><port>123</port></ntp>",
      "<ntp "
      "xmlns='urn:ietf:params:xml:ns:yang:ietf-ntp'><port>1123</port></ntp>",
      "[]"};
  DangPluginErrorV1 error{};
  void* prepared = nullptr;
  bool ok = base.prepare(base.context, &input, &prepared, &error);
  ok = ok && base.validate(base.context, prepared, &error);
  std::vector<std::string> applied;
  bool failed = false;
  const auto count =
      ok ? actions.hardware_action_count(base.context, prepared) : 0;
  for (std::size_t i = 0; ok && i < count; ++i) {
    DangHardwareActionV1 action{};
    ok = actions.hardware_action_at(base.context, prepared, i, &action, &error);
    if (!ok) break;
    if (!actions.apply_hardware_action(base.context, prepared, action.action_id,
                                       &error)) {
      failed = true;
      ok = expect_failure && i + 1 == count && error.instance_path &&
           std::string_view(error.instance_path) == "/ietf-ntp:ntp";
      break;
    }
    applied.emplace_back(action.action_id);
  }
  ok = ok && failed == expect_failure && count > 0;
  bool active = false;
  ok = ok && service.Active(&active, &message) && active == !expect_failure;
  if (!expect_failure)
    ok = ok &&
         Read(root + "/managed.conf").find("port 1123") != std::string::npos;
  // Always attempt compensation of acknowledged actions, even if assertions
  // fail.
  for (auto i = applied.rbegin(); i != applied.rend(); ++i)
    if (!actions.rollback_hardware_action(base.context, prepared, i->c_str(),
                                          &error))
      ok = false;
  ok = ok && service.Active(&active, &message) && !active &&
       Read(root + "/managed.conf") == "original fragment" &&
       Read(root + "/managed.keys") == "original keys";
  if (prepared) base.release(base.context, prepared);
  dlclose(library);
  unsetenv("DANG_NTP_NATIVE_TEST_ROOT");
  unsetenv("DANG_NTP_NATIVE_TEST_UNIT");
  if (ok)
    std::filesystem::remove_all(root);
  else
    std::cerr << "native plugin test failed; recovery files retained at "
              << root << '\n';
  std::cout
      << (ok ? "PASS: loaded plugin files and initial service state restored\n"
             : "FAIL\n");
  return ok ? 0 : 1;
}

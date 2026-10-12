// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0
/** @file Explicit loaded-plugin test with disposable files and systemd units.
 */
#include <arpa/inet.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

#include "dangd/plugin_api.h"
#include "plugins/ntp/src/linux/chrony_process.h"
#include "plugins/ntp/src/linux/chrony_service.h"

std::string Read(const std::string& path) {
  std::ifstream in(path);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
/** Direct NTPv4 loopback exchange proves the daemon consumed the rendered
 * port/local clock. */
bool NtpReply() {
  const int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (socket_fd < 0) return false;
  const timeval timeout{.tv_sec = 2, .tv_usec = 0};
  setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(1123);
  inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
  if (connect(socket_fd, reinterpret_cast<sockaddr*>(&address),
              sizeof(address)) < 0) {
    close(socket_fd);
    return false;
  }
  std::array<unsigned char, 48> request{}, reply{};
  request[0] = 0x23;
  request[40] = 0xe9;
  request[47] = 1;
  bool valid = false;
  for (int attempt = 0; attempt < 4 && !valid; ++attempt) {
    if (send(socket_fd, request.data(), request.size(), 0) != 48) break;
    const auto received = recv(socket_fd, reply.data(), reply.size(), 0);
    valid = received == 48 && (reply[0] & 7U) == 4U && reply[1] == 8U &&
            std::equal(request.begin() + 40, request.end(), reply.begin() + 24);
  }
  close(socket_fd);
  return valid;
}
int main(int argc, char** argv) {
  if ((argc != 3 && argc != 4) ||
      (std::string_view(argv[2]) != "dangd-ntp-native-test.service" &&
       std::string_view(argv[2]) != "dangd-ntp-native-fail-test.service" &&
       std::string_view(argv[2]) != "dangd-ntp-live-test.service"))
    return 2;
  const bool expect_failure =
      std::string_view(argv[2]) == "dangd-ntp-native-fail-test.service";
  const bool live = std::string_view(argv[2]) == "dangd-ntp-live-test.service";
  const bool policy_refusal = argc == 4;
  if (policy_refusal &&
      (!live || std::string_view(argv[3]) != "expect-policy-refusal"))
    return 2;
  dang::ntp::SystemdChronyService service(argv[2]);
  std::string message;
  if (!service.SetActive(false, &message)) {
    std::cerr << message << '\n';
    return 1;
  }
  char pattern[] = "/tmp/dang-ntp-plugin-XXXXXX";
  const char* created =
      live ? (std::filesystem::create_directory("/tmp/dang-ntp-live-test")
                  ? "/tmp/dang-ntp-live-test"
                  : nullptr)
           : mkdtemp(pattern);
  if (!created) return 1;
  const std::string root(created);
  {
    std::ofstream out(root + "/managed.conf");
    out << (live ? "port 1124\nlocal stratum 8\n" : "original fragment");
  }
  if (live) {
    std::string identity_error;
    if (dang::ntp::ValidateChronyProcess(
            static_cast<std::uint32_t>(getpid()), "/run/dangd-ntp-live-chronyd",
            {"unrelated-process"}, geteuid(), &identity_error))
      return 1;
    std::cout << "PASS: unrelated process identity rejected\n";
    std::ofstream out(root + "/main.conf");
    out << "pidfile " << root << "/chronyd.pid\nbindcmdaddress " << root
        << "/chronyd.sock\ncmdport 0\nbindaddress 127.0.0.1\nallow "
           "127.0.0.1\ninclude "
        << root << "/managed.conf\n";
  }
  {
    std::ofstream out(root + "/managed.keys");
    out << "original keys";
  }
  if (live && chmod((root + "/managed.keys").c_str(), 0600) < 0) return 1;
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
  if (live)
    input = {
        "<ntp "
        "xmlns='urn:ietf:params:xml:ns:yang:ietf-ntp'><port>1124</"
        "port><refclock-master><master-stratum>8</master-stratum></"
        "refclock-master></ntp>",
        "<ntp "
        "xmlns='urn:ietf:params:xml:ns:yang:ietf-ntp'><port>1123</"
        "port><refclock-master><master-stratum>8</master-stratum></"
        "refclock-master></ntp>",
        "[]"};
  DangPluginErrorV1 error{};
  void* prepared = nullptr;
  if (policy_refusal) {
    bool rejected = base.prepare(base.context, &input, &prepared, &error) &&
                    base.validate(base.context, prepared, &error);
    DangHardwareActionV1 first{};
    rejected =
        rejected &&
        actions.hardware_action_at(base.context, prepared, 0, &first, &error) &&
        !actions.apply_hardware_action(base.context, prepared, first.action_id,
                                       &error) &&
        error.message &&
        std::string_view(error.message).find("service policy") !=
            std::string_view::npos;
    bool active = true;
    rejected = rejected && service.Active(&active, &message) && !active &&
               Read(root + "/managed.conf") == "port 1124\nlocal stratum 8\n" &&
               Read(root + "/managed.keys") == "original keys";
    if (prepared) base.release(base.context, prepared);
    dlclose(library);
    unsetenv("DANG_NTP_NATIVE_TEST_ROOT");
    unsetenv("DANG_NTP_NATIVE_TEST_UNIT");
    if (rejected) std::filesystem::remove_all(root);
    std::cout
        << (rejected
                ? "PASS: native service override rejected before mutation\n"
                : "FAIL: service override policy\n");
    return rejected ? 0 : 1;
  }
  bool preflight_ok = true;
  if (live) {
    const std::string wrapper = Read(root + "/main.conf");
    {
      std::ofstream out(root + "/main.conf", std::ios::app);
      out << "server 192.0.2.1\n";
    }
    preflight_ok = base.prepare(base.context, &input, &prepared, &error) &&
                   base.validate(base.context, prepared, &error);
    DangHardwareActionV1 first{};
    preflight_ok =
        preflight_ok &&
        actions.hardware_action_at(base.context, prepared, 0, &first, &error) &&
        !actions.apply_hardware_action(base.context, prepared, first.action_id,
                                       &error) &&
        error.message &&
        std::string_view(error.message).find("unmanaged") !=
            std::string_view::npos;
    bool active = true;
    preflight_ok =
        preflight_ok && service.Active(&active, &message) && !active &&
        Read(root + "/managed.conf") == "port 1124\nlocal stratum 8\n";
    if (prepared) base.release(base.context, prepared);
    prepared = nullptr;
    {
      std::ofstream out(root + "/main.conf");
      out << wrapper;
    }
    std::cout << (preflight_ok
                      ? "PASS: competing source rejected before mutation\n"
                      : "FAIL: wrapper preflight\n");
  }
  if (live) {
    const auto refused = [&]() {
      void* probe = nullptr;
      bool rejected = base.prepare(base.context, &input, &probe, &error) &&
                      base.validate(base.context, probe, &error);
      DangHardwareActionV1 first{};
      rejected =
          rejected &&
          actions.hardware_action_at(base.context, probe, 0, &first, &error) &&
          !actions.apply_hardware_action(base.context, probe, first.action_id,
                                         &error);
      bool active = true;
      rejected = rejected && service.Active(&active, &message) && !active &&
                 Read(root + "/managed.conf") == "port 1124\nlocal stratum 8\n";
      if (probe) base.release(base.context, probe);
      return rejected;
    };
    if (chmod(root.c_str(), 0777) < 0) return 1;
    const bool directory_refused = refused();
    if (chmod(root.c_str(), 0755) < 0 ||
        chmod((root + "/managed.keys").c_str(), 0644) < 0)
      return 1;
    const bool keys_refused = refused();
    if (chmod((root + "/managed.keys").c_str(), 0600) < 0) return 1;
    preflight_ok = preflight_ok && directory_refused && keys_refused;
    std::cout << (directory_refused && keys_refused
                      ? "PASS: unsafe directory and public keys rejected "
                        "before mutation\n"
                      : "FAIL: ownership preflight\n");
  }
  bool ok =
      preflight_ok && base.prepare(base.context, &input, &prepared, &error);
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
      std::cerr << "action failure at "
                << (error.instance_path ? error.instance_path : "unknown")
                << ": " << (error.message ? error.message : "no diagnostic")
                << '\n';
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
  if (live) {
    const bool reply = NtpReply();
    std::cout << (reply ? "PASS: live NTPv4 reply on managed port, stratum 8\n"
                        : "FAIL: live NTP reply\n");
    ok = ok && reply;
  }
  // Always attempt compensation of acknowledged actions, even if assertions
  // fail.
  for (auto i = applied.rbegin(); i != applied.rend(); ++i)
    if (!actions.rollback_hardware_action(base.context, prepared, i->c_str(),
                                          &error))
      ok = false;
  ok = ok && service.Active(&active, &message) && !active &&
       Read(root + "/managed.conf") ==
           (live ? "port 1124\nlocal stratum 8\n" : "original fragment") &&
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

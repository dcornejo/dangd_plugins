// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/plugin_api.h"
#include "plugins/system/src/auth_protocol.h"

#include <dlfcn.h>
#include <security/pam_appl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

std::string Read(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), {}};
}

bool Authenticate(const std::string& socket_path, std::string_view username,
                  std::string_view password) {
  const int socket = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (socket < 0) return false;
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
  bool accepted = false;
  if (connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) ==
      0) {
    const dang::system::AuthRequestHeader request{
        dang::system::kAuthMagic, dang::system::kAuthVersion, 0,
        static_cast<std::uint32_t>(username.size()),
        static_cast<std::uint32_t>(password.size())};
    dang::system::AuthResponse response{};
    accepted = write(socket, &request, sizeof(request)) == sizeof(request) &&
        write(socket, username.data(), username.size()) ==
            static_cast<ssize_t>(username.size()) &&
        write(socket, password.data(), password.size()) ==
            static_cast<ssize_t>(password.size()) &&
        read(socket, &response, sizeof(response)) == sizeof(response) &&
        response == dang::system::AuthResponse::kAccepted;
  }
  close(socket);
  return accepted;
}

int Converse(int count, const pam_message** messages, pam_response** output,
             void* opaque) {
  auto* password = static_cast<const char*>(opaque);
  auto* responses = static_cast<pam_response*>(
      calloc(static_cast<std::size_t>(count), sizeof(pam_response)));
  if (!responses) return PAM_BUF_ERR;
  for (int index = 0; index < count; ++index) {
    if (messages[index]->msg_style == PAM_PROMPT_ECHO_OFF ||
        messages[index]->msg_style == PAM_PROMPT_ECHO_ON) {
      responses[index].resp = strdup(password);
      if (!responses[index].resp) {
        free(responses);
        return PAM_BUF_ERR;
      }
    }
  }
  *output = responses;
  return PAM_SUCCESS;
}

bool PamAuthenticate(const char* service, const char* password) {
  pam_conv conversation{Converse, const_cast<char*>(password)};
  pam_handle_t* handle = nullptr;
  int status = pam_start(service, "alice", &conversation, &handle);
  if (status == PAM_SUCCESS) status = pam_authenticate(handle, 0);
  if (status == PAM_SUCCESS) status = pam_acct_mgmt(handle, 0);
  if (status != PAM_SUCCESS)
    std::cerr << "PAM status " << status << ": "
              << pam_strerror(handle, status) << '\n';
  if (handle) (void)pam_end(handle, status);
  return status == PAM_SUCCESS;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 6) {
    std::cerr << "usage: system_plugin_integration_test PLUGIN BEFORE PROPOSED ROOT PAM_SERVICE\n";
    return 2;
  }
  const std::filesystem::path root = argv[4];
  const std::filesystem::path ntp_path =
#if defined(__FreeBSD__)
      root / "etc/ntp.conf";
#else
      root / "etc/chrony/conf.d/dangd.conf";
#endif
  std::filesystem::create_directories(ntp_path.parent_path());
  std::ofstream(root / "etc/resolv.conf") << "original resolver\n";
  std::ofstream(ntp_path) << "original ntp\n";
  const std::string socket_path = (root / "auth.sock").string();
  setenv("DANG_SYSTEM_ROOT", root.c_str(), 1);
  setenv("DANG_SYSTEM_AUTH_SOCKET", socket_path.c_str(), 1);
  void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!library) {
    std::cerr << dlerror() << '\n';
    return 1;
  }
  auto initialize = reinterpret_cast<DangPluginInitV3>(
      dlsym(library, "dang_plugin_init_v3"));
  const DangPluginV3* v3 = initialize ? initialize() : nullptr;
  if (!v3) return 3;
  const DangPluginV1& plugin = v3->v2.v1;
  const std::string before = Read(argv[2]);
  const std::string proposed = Read(argv[3]);
  DangTransactionV1 transaction{before.c_str(), proposed.c_str(), "[]"};
  DangPluginErrorV1 error{};
  void* prepared = nullptr;
  if (!plugin.prepare(plugin.context, &transaction, &prepared, &error) ||
      !plugin.validate(plugin.context, prepared, &error) ||
      !plugin.apply(plugin.context, prepared, &error)) {
    std::cerr << (error.instance_path ? error.instance_path : "") << ": "
              << (error.message ? error.message : "plugin failure") << '\n';
    return 4;
  }
  if (Read(root / "etc/resolv.conf").find("192.0.2.53") == std::string::npos) {
    std::cerr << "generated resolver configuration is missing\n";
    return 5;
  }
  if (Read(ntp_path).find("192.0.2.123") == std::string::npos) {
    std::cerr << "generated NTP configuration is missing\n";
    return 5;
  }
  const bool persistent_hostname =
      !Read(root / "etc/hostname").empty() ||
      !Read(root / "etc/rc.conf.d/dangd-hostname").empty();
  if (std::filesystem::file_size(root / "etc/localtime") < 50 ||
      !persistent_hostname) {
    std::cerr << "timezone or persistent hostname was not generated\n";
    return 5;
  }
  if (!Authenticate(socket_path, "alice", "secret")) {
    std::cerr << "direct valid password verification failed\n";
    return 5;
  }
  if (Authenticate(socket_path, "alice", "wrong")) {
    std::cerr << "direct invalid password was accepted\n";
    return 5;
  }
  if (!PamAuthenticate(argv[5], "secret")) {
    std::cerr << "PAM valid password verification failed\n";
    return 5;
  }
  if (PamAuthenticate(argv[5], "wrong")) {
    std::cerr << "PAM invalid password was accepted\n";
    return 5;
  }
  DangOperationalDataV1 operational{};
  if (!v3->get_operational_data(plugin.context, &operational, &error) ||
      !operational.data_xml ||
      std::string_view(operational.data_xml).find("<os-name>") ==
          std::string_view::npos)
    return 6;
  if (!plugin.rollback(plugin.context, prepared, &error) ||
      Read(root / "etc/resolv.conf") != "original resolver\n" ||
      Read(ntp_path) != "original ntp\n")
    return 7;
  plugin.release(plugin.context, prepared);
  dlclose(library);
  std::cout << "RFC 7317 apply, PAM verification, state, and rollback passed\n";
  return 0;
}

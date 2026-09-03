// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/**
 * PAM authentication adapter for RFC 7317 local users managed by dangd.
 *
 * The module never reads datastore files or password hashes. It submits the
 * PAM username and authentication token to the root-only local verifier owned
 * by the supervised ietf-system plugin. A missing, malformed, or slow service
 * fails closed. Configure sshd with an `auth` entry for pam_dangd; public-key
 * authentication remains SSH's responsibility because PAM has no portable
 * public-key verification interface.
 */

#include "plugins/system/src/auth_protocol.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#include <security/pam_appl.h>
#include <security/pam_modules.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

std::string SocketPath(int argc, const char* argv[]) {
  constexpr std::string_view prefix = "socket=";
  for (int index = 0; index < argc; ++index) {
    const std::string_view option(argv[index]);
    if (option.starts_with(prefix)) return std::string(option.substr(prefix.size()));
  }
  return "/run/dangd/auth.sock";
}

bool WriteAll(int socket, const void* input, std::size_t size) {
  const auto* bytes = static_cast<const unsigned char*>(input);
  while (size > 0) {
#if defined(MSG_NOSIGNAL)
    const ssize_t count = send(socket, bytes, size, MSG_NOSIGNAL);
#else
    const ssize_t count = send(socket, bytes, size, 0);
#endif
    if (count <= 0) return false;
    bytes += count;
    size -= static_cast<std::size_t>(count);
  }
  return true;
}

bool Verify(std::string_view socket_path, std::string_view username,
            std::string_view password) {
  if (socket_path.empty() || socket_path.size() >= sizeof(sockaddr_un::sun_path) ||
      username.empty() || username.size() > dang::system::kMaximumUsernameBytes ||
      password.size() > dang::system::kMaximumPasswordBytes)
    return false;
  const int connection = socket(AF_UNIX, SOCK_STREAM, 0);
  if (connection < 0) return false;
#if defined(SO_NOSIGPIPE)
  int enabled = 1;
  (void)setsockopt(connection, SOL_SOCKET, SO_NOSIGPIPE, &enabled,
                   sizeof(enabled));
#endif
  timeval timeout{5, 0};
  (void)setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                   sizeof(timeout));
  (void)setsockopt(connection, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                   sizeof(timeout));
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, socket_path.data(), socket_path.size());
  bool accepted = false;
  if (connect(connection, reinterpret_cast<sockaddr*>(&address),
              sizeof(address)) == 0) {
    const dang::system::AuthRequestHeader request{
        dang::system::kAuthMagic, dang::system::kAuthVersion, 0,
        static_cast<std::uint32_t>(username.size()),
        static_cast<std::uint32_t>(password.size())};
    dang::system::AuthResponse response =
        dang::system::AuthResponse::kUnavailable;
    accepted = WriteAll(connection, &request, sizeof(request)) &&
               WriteAll(connection, username.data(), username.size()) &&
               WriteAll(connection, password.data(), password.size()) &&
               read(connection, &response, sizeof(response)) == sizeof(response) &&
               response == dang::system::AuthResponse::kAccepted;
  }
  close(connection);
  return accepted;
}

std::optional<std::string> Password(pam_handle_t* pamh) {
  const void* existing = nullptr;
  if (pam_get_item(pamh, PAM_AUTHTOK, &existing) == PAM_SUCCESS && existing)
    return std::string(static_cast<const char*>(existing));
  const void* item = nullptr;
  if (pam_get_item(pamh, PAM_CONV, &item) != PAM_SUCCESS || !item)
    return std::nullopt;
  const auto* conversation = static_cast<const pam_conv*>(item);
  char prompt[] = "Password: ";
  const pam_message message{PAM_PROMPT_ECHO_OFF, prompt};
  const pam_message* messages[] = {&message};
  pam_response* response = nullptr;
  if (!conversation->conv ||
      conversation->conv(1, messages, &response,
                         conversation->appdata_ptr) != PAM_SUCCESS ||
      !response || !response[0].resp) {
    std::free(response);
    return std::nullopt;
  }
  std::string password(response[0].resp);
  (void)pam_set_item(pamh, PAM_AUTHTOK, password.c_str());
  std::memset(response[0].resp, 0, std::strlen(response[0].resp));
  std::free(response[0].resp);
  std::free(response);
  return password;
}

}  // namespace

extern "C" {

PAM_EXTERN int pam_sm_authenticate(pam_handle_t* pamh, int, int argc,
                                   const char* argv[]) {
  const char* username = nullptr;
  if (pam_get_user(pamh, &username, nullptr) != PAM_SUCCESS || !username)
    return PAM_USER_UNKNOWN;
  auto password = Password(pamh);
  if (!password) return PAM_AUTH_ERR;
  const bool accepted = Verify(SocketPath(argc, argv), username, *password);
  std::fill(password->begin(), password->end(), '\0');
  return accepted ? PAM_SUCCESS : PAM_AUTH_ERR;
}

PAM_EXTERN int pam_sm_setcred(pam_handle_t*, int, int, const char**) {
  return PAM_SUCCESS;
}

PAM_EXTERN int pam_sm_acct_mgmt(pam_handle_t*, int, int, const char**) {
  // RFC 7317 has no account expiry, lockout, or login-class state. Successful
  // password verification is the complete portable account assertion.
  return PAM_SUCCESS;
}

PAM_EXTERN int pam_sm_open_session(pam_handle_t*, int, int, const char**) {
  return PAM_SUCCESS;
}

PAM_EXTERN int pam_sm_close_session(pam_handle_t*, int, int, const char**) {
  return PAM_SUCCESS;
}

PAM_EXTERN int pam_sm_chauthtok(pam_handle_t*, int, int, const char**) {
  // Password changes must be committed through NETCONF so validation,
  // persistence, NACM, and audit semantics are preserved.
  return PAM_AUTHTOK_ERR;
}

}  // extern "C"

#ifdef PAM_MODULE_ENTRY
PAM_MODULE_ENTRY("pam_dangd");
#endif

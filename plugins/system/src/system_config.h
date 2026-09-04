// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANG_PLUGINS_SYSTEM_CONFIG_H_
#define DANG_PLUGINS_SYSTEM_CONFIG_H_

#include <optional>
#include <string>
#include <vector>

namespace dang::system {

/** RFC 7317 authorized-key entry retained without altering its key material. */
struct AuthorizedKey {
  std::string name;
  std::string algorithm;
  std::string key_data;
};

/** Local authentication data consumed only by the privileged PAM endpoint. */
struct User {
  std::string name;
  std::optional<std::string> password_hash;
  std::vector<AuthorizedKey> authorized_keys;
};

/** Platform-neutral NTP association after YANG defaults are applied. */
struct NtpServer {
  std::string name;
  std::string address;
  unsigned port = 123;
  std::string association = "server";
  bool iburst = false;
  bool prefer = false;
};

/** Resolver endpoint; non-default ports may be rejected by a host backend. */
struct DnsServer {
  std::string name;
  std::string address;
  unsigned port = 53;
};

/** Supported RFC 7317 configuration projection with explicit YANG defaults. */
struct Config {
  std::optional<std::string> contact;
  std::optional<std::string> hostname;
  std::optional<std::string> location;
  std::optional<std::string> timezone_name;
  std::optional<int> timezone_offset_minutes;
  bool ntp_present = false;
  bool ntp_enabled = true;
  std::vector<NtpServer> ntp_servers;
  bool dns_present = false;
  std::vector<std::string> dns_search;
  std::vector<DnsServer> dns_servers;
  unsigned dns_timeout = 5;
  unsigned dns_attempts = 2;
  bool local_password_authentication = false;
  std::vector<User> users;
};

/** Extracts the RFC 7317 subtree from a complete datastore XML snapshot. */
[[nodiscard]] bool ParseConfig(const char* xml, Config* config,
                               std::string* error, std::string* error_path);

/** Constant-time verification through the host crypt(3) implementation. */
[[nodiscard]] bool VerifyPassword(std::string_view password,
                                  std::string_view stored_hash);

}  // namespace dang::system

#endif  // DANG_PLUGINS_SYSTEM_CONFIG_H_

// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANG_PLUGINS_NTP_CONFIG_H_
#define DANG_PLUGINS_NTP_CONFIG_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dang::ntp {

/** Polling and transport settings shared by RFC 9249 associations. */
struct CommonAttributes {
  int minpoll = 6;
  int maxpoll = 10;
  std::uint16_t port = 123;
  unsigned version = 4;
  std::optional<std::uint32_t> key_id;

  bool operator==(const CommonAttributes&) const = default;
};

/** Symmetric authentication material retained only for backend preparation. */
struct AuthenticationKey {
  std::uint32_t id = 0;
  std::string algorithm;
  std::optional<std::string> hexadecimal;
  bool trusted = false;

  bool operator==(const AuthenticationKey&) const = default;
};

/** Reference from one RFC 9249 access mode to an RFC 8519 ACL. */
struct AccessRule {
  std::string mode;
  std::string acl;

  bool operator==(const AccessRule&) const = default;
};

/** Platform-neutral unicast server or peer association. */
struct UnicastAssociation {
  std::string address;
  std::string type;
  bool prefer = false;
  bool burst = false;
  bool iburst = false;
  std::optional<std::string> source_interface;
  CommonAttributes common;

  bool operator==(const UnicastAssociation&) const = default;
};

/** RFC 9249 configuration with all YANG defaults made explicit. */
struct Config {
  bool present = false;
  std::uint16_t port = 123;
  std::optional<unsigned> refclock_master_stratum;
  bool authentication_present = false;
  bool authentication_enabled = false;
  std::vector<AuthenticationKey> authentication_keys;
  std::vector<AccessRule> access_rules;
  std::vector<UnicastAssociation> unicast;

  bool operator==(const Config&) const = default;
};

/** One deterministic, side-effect-free step for a future native adapter. */
struct PlannedAction {
  std::string id;
  std::string description;
  std::vector<std::string> dependencies;

  bool operator==(const PlannedAction&) const = default;
};

/**
 * Extract the RFC 9249 subtree from a complete datastore XML snapshot.
 *
 * No host state is read or changed. Unsupported configured model branches and
 * malformed direct-plugin input fail at an attributed RFC 9249 instance path.
 */
[[nodiscard]] bool ParseConfig(const char* xml, Config* config,
                               std::string* error, std::string* error_path);

/**
 * Produce ordered English deltas without exposing authentication key bytes.
 *
 * Dependencies encode the safety rule that key/access prerequisites precede
 * new associations, while removed associations precede obsolete key removal.
 */
[[nodiscard]] std::vector<PlannedAction> PlanChanges(const Config& before,
                                                     const Config& proposed);

}  // namespace dang::ntp

#endif  // DANG_PLUGINS_NTP_CONFIG_H_

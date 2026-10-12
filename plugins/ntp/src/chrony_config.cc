// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/** @file Side-effect-free RFC 9249 to chrony configuration translation. */

#include "plugins/ntp/src/chrony_config.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

namespace dang::ntp {
namespace {

constexpr std::string_view kAuthenticationPath =
    "/ietf-ntp:ntp/authentication/authentication-keys";
constexpr std::string_view kUnicastPath = "/ietf-ntp:ntp/unicast-configuration";

bool Fail(std::string text, std::string path, std::string* error,
          std::string* error_path) {
  if (error) *error = std::move(text);
  if (error_path) *error_path = std::move(path);
  return false;
}

bool NormalizeHex(const AuthenticationKey& key, std::string* normalized,
                  std::string* error, std::string* error_path) {
  normalized->clear();
  if (!key.hexadecimal)
    return Fail("hexadecimal authentication key material is required",
                std::string(kAuthenticationPath) + "/key/hexadecimal-string",
                error, error_path);
  for (const char raw_character : *key.hexadecimal) {
    const auto character = static_cast<unsigned char>(raw_character);
    if (character == ':') continue;
    if (!std::isxdigit(character))
      return Fail("authentication key is not a hexadecimal octet string",
                  std::string(kAuthenticationPath) + "/key/hexadecimal-string",
                  error, error_path);
    normalized->push_back(
        static_cast<char>(std::toupper(static_cast<unsigned char>(character))));
  }
  if (normalized->empty() || normalized->size() % 2U != 0U)
    return Fail("authentication key is not a hexadecimal octet string",
                std::string(kAuthenticationPath) + "/key/hexadecimal-string",
                error, error_path);
  return true;
}

bool ChronyAlgorithm(const AuthenticationKey& key, std::size_t hex_size,
                     std::string* algorithm, std::string* error,
                     std::string* error_path) {
  // chrony's SHA names denote the legacy NTP hash(key || packet) MAC,
  // not HMAC. Substituting one for an RFC 9249 HMAC identity would change
  // the on-wire algorithm despite producing syntactically valid files.
  if (key.algorithm.starts_with("hmac-"))
    return Fail(
        "chrony legacy SHA MACs cannot implement the modeled HMAC algorithm",
        std::string(kAuthenticationPath) + "/algorithm", error, error_path);
  if (key.algorithm == "aes-cmac" && hex_size == 32U)
    *algorithm = "AES128";
  else if (key.algorithm == "aes-cmac" && hex_size == 64U)
    *algorithm = "AES256";
  else if (key.algorithm == "aes-cmac")
    return Fail("chrony AES-CMAC keys must contain exactly 128 or 256 bits",
                std::string(kAuthenticationPath) + "/key/hexadecimal-string",
                error, error_path);
  else
    return Fail("authentication algorithm has no chrony mapping",
                std::string(kAuthenticationPath) + "/algorithm", error,
                error_path);
  return true;
}

bool RenderKeys(const Config& config, std::ostringstream* keys,
                std::string* error, std::string* error_path) {
  for (const AuthenticationKey& key : config.authentication_keys) {
    std::string hexadecimal;
    if (!NormalizeHex(key, &hexadecimal, error, error_path)) return false;
    std::string algorithm;
    if (!ChronyAlgorithm(key, hexadecimal.size(), &algorithm, error,
                         error_path))
      return false;
    *keys << key.id << ' ' << algorithm << " HEX:" << hexadecimal << '\n';
  }
  return true;
}

bool RenderAssociation(const Config& config,
                       const UnicastAssociation& association,
                       std::ostringstream* fragment, std::string* error,
                       std::string* error_path) {
  if (association.source_interface)
    return Fail(
        "chrony has only a daemon-wide acquisition interface and cannot "
        "preserve an association-specific source interface",
        std::string(kUnicastPath) + "/source", error, error_path);
  if (association.type == "uc-peer" &&
      (association.burst || association.iburst))
    return Fail(
        "chrony does not support burst or iburst on a symmetric peer",
        std::string(kUnicastPath) + (association.burst ? "/burst" : "/iburst"),
        error, error_path);
  if (association.common.key_id && !config.authentication_enabled)
    return Fail("an authenticated association requires auth-enabled=true",
                std::string(kUnicastPath) + "/authentication/keyid", error,
                error_path);
  if (association.common.key_id) {
    const auto found = std::ranges::find_if(
        config.authentication_keys, [&](const AuthenticationKey& key) {
          return key.id == *association.common.key_id;
        });
    if (found == config.authentication_keys.end() || !found->trusted)
      return Fail("an authenticated chrony association requires a trusted key",
                  std::string(kUnicastPath) + "/authentication/keyid", error,
                  error_path);
  }

  *fragment << (association.type == "uc-peer" ? "peer " : "server ")
            << association.address << " minpoll " << association.common.minpoll
            << " maxpoll " << association.common.maxpoll << " version "
            << association.common.version;
  if (association.common.port != 123U)
    *fragment << " port " << association.common.port;
  if (association.common.key_id)
    *fragment << " key " << *association.common.key_id;
  if (association.prefer) *fragment << " prefer";
  if (association.burst) *fragment << " burst";
  if (association.iburst) *fragment << " iburst";
  *fragment << '\n';
  return true;
}

}  // namespace

bool RenderChronyConfig(const Config& config, std::string_view key_file_path,
                        ChronyConfigImage* image, std::string* error,
                        std::string* error_path) {
  if (!image)
    return Fail("missing chrony configuration output", "/ietf-ntp:ntp", error,
                error_path);
  image->fragment.clear();
  image->key_file.clear();
  if (!config.present) return true;
  if (key_file_path.empty() || key_file_path.front() != '/' ||
      key_file_path.find_first_of(" \t\r\n\"\\") != std::string_view::npos)
    return Fail("invalid managed chrony key-file path", "/ietf-ntp:ntp", error,
                error_path);
  if (!config.access_rules.empty())
    return Fail(
        "RFC 8519 access-rule translation is not implemented for chrony",
        "/ietf-ntp:ntp/access-rules", error, error_path);
  if (config.refclock_master_stratum && *config.refclock_master_stratum > 15U)
    return Fail("chrony local-reference stratum must be in the range 1..15",
                "/ietf-ntp:ntp/refclock-master/master-stratum", error,
                error_path);

  std::ostringstream fragment;
  std::ostringstream keys;
  fragment << "# Managed by the dangd RFC 9249 plugin. Do not edit.\n"
           << "port " << config.port << '\n';
  if (config.refclock_master_stratum)
    fragment << "local stratum " << *config.refclock_master_stratum << '\n';
  if (!config.authentication_keys.empty()) {
    if (!RenderKeys(config, &keys, error, error_path)) return false;
    fragment << "keyfile " << key_file_path << '\n';
  }
  for (const UnicastAssociation& association : config.unicast)
    if (!RenderAssociation(config, association, &fragment, error, error_path))
      return false;

  image->fragment = std::move(fragment).str();
  image->key_file = std::move(keys).str();
  return true;
}

}  // namespace dang::ntp

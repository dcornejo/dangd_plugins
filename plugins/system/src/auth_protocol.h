// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANG_PLUGINS_SYSTEM_AUTH_PROTOCOL_H_
#define DANG_PLUGINS_SYSTEM_AUTH_PROTOCOL_H_

#include <cstddef>
#include <cstdint>

namespace dang::system {

/** Fixed, local-only request framing shared with pam_dangd. */
constexpr std::uint32_t kAuthMagic = 0x44414e47U;  // "DANG"
constexpr std::uint16_t kAuthVersion = 1;
constexpr std::size_t kMaximumUsernameBytes = 256;
constexpr std::size_t kMaximumPasswordBytes = 4096;

/** Network-byte-order header followed by username and password bytes. */
struct AuthRequestHeader {
  std::uint32_t magic;
  std::uint16_t version;
  std::uint16_t reserved;
  std::uint32_t username_size;
  std::uint32_t password_size;
};

/** Single-byte result; unavailable lets PAM distinguish service failure. */
enum class AuthResponse : std::uint8_t {
  kDenied = 0,
  kAccepted = 1,
  kUnavailable = 2,
};

}  // namespace dang::system

#endif  // DANG_PLUGINS_SYSTEM_AUTH_PROTOCOL_H_

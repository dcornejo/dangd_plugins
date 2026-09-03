// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANG_PLUGINS_SYSTEM_AUTH_PROTOCOL_H_
#define DANG_PLUGINS_SYSTEM_AUTH_PROTOCOL_H_

#include <cstddef>
#include <cstdint>

namespace dang::system {

constexpr std::uint32_t kAuthMagic = 0x44414e47U;  // "DANG"
constexpr std::uint16_t kAuthVersion = 1;
constexpr std::size_t kMaximumUsernameBytes = 256;
constexpr std::size_t kMaximumPasswordBytes = 4096;

struct AuthRequestHeader {
  std::uint32_t magic;
  std::uint16_t version;
  std::uint16_t reserved;
  std::uint32_t username_size;
  std::uint32_t password_size;
};

enum class AuthResponse : std::uint8_t {
  kDenied = 0,
  kAccepted = 1,
  kUnavailable = 2,
};

}  // namespace dang::system

#endif  // DANG_PLUGINS_SYSTEM_AUTH_PROTOCOL_H_

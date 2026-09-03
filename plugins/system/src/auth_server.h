// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANG_PLUGINS_SYSTEM_AUTH_SERVER_H_
#define DANG_PLUGINS_SYSTEM_AUTH_SERVER_H_

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace dang::system {

/** Root-only local password-verification service consumed by pam_dangd. */
class AuthServer {
 public:
  using Verifier = std::function<bool(std::string_view, std::string_view)>;

  AuthServer();
  ~AuthServer();
  AuthServer(const AuthServer&) = delete;
  AuthServer& operator=(const AuthServer&) = delete;

  [[nodiscard]] bool Start(std::string path, Verifier verifier,
                           std::string* error);
  void Stop();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace dang::system

#endif  // DANG_PLUGINS_SYSTEM_AUTH_SERVER_H_

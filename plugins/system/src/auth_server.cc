// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/**
 * @file
 * Privilege-separated local authentication service used by pam_dangd.  It
 * accepts only suitably privileged UNIX-socket peers, bounds credential sizes,
 * and erases the in-memory password copy after each verification attempt.
 */

#include "plugins/system/src/auth_server.h"

#include "plugins/system/src/auth_protocol.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#if defined(__FreeBSD__)
#include <sys/ucred.h>
#endif

namespace dang::system {
namespace {

bool ReadAll(int socket, void* output, std::size_t size) {
  auto* bytes = static_cast<unsigned char*>(output);
  while (size > 0) {
    const ssize_t count = read(socket, bytes, size);
    if (count <= 0) return false;
    bytes += count;
    size -= static_cast<std::size_t>(count);
  }
  return true;
}

bool PeerMayAuthenticate(int socket) {
#if defined(__linux__)
  ucred credentials{};
  socklen_t size = sizeof(credentials);
  return getsockopt(socket, SOL_SOCKET, SO_PEERCRED, &credentials, &size) == 0 &&
         size == sizeof(credentials) && credentials.uid == 0;
#elif defined(__FreeBSD__)
  xucred credentials{};
  socklen_t size = sizeof(credentials);
  return getsockopt(socket, 0, LOCAL_PEERCRED, &credentials, &size) == 0 &&
         credentials.cr_version == XUCRED_VERSION &&
         credentials.cr_uid == 0;
#else
  (void)socket;
  return false;
#endif
}

void Erase(std::string* value) {
  std::fill(value->begin(), value->end(), '\0');
  value->clear();
}

}  // namespace

struct AuthServer::Impl {
  int listener = -1;
  std::string path;
  Verifier verifier;
  std::atomic<bool> stopping{false};
  std::thread worker;

  void ServeClient(int client) {
    timeval timeout{5, 0};
    (void)setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                     sizeof(timeout));
    (void)setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                     sizeof(timeout));
    AuthResponse response = AuthResponse::kDenied;
    AuthRequestHeader header{};
    if (PeerMayAuthenticate(client) && ReadAll(client, &header, sizeof(header)) &&
        header.magic == kAuthMagic && header.version == kAuthVersion &&
        header.reserved == 0 && header.username_size > 0 &&
        header.username_size <= kMaximumUsernameBytes &&
        header.password_size <= kMaximumPasswordBytes) {
      std::string username(header.username_size, '\0');
      std::string password(header.password_size, '\0');
      if (ReadAll(client, username.data(), username.size()) &&
          ReadAll(client, password.data(), password.size()) &&
          verifier(username, password))
        response = AuthResponse::kAccepted;
      Erase(&password);
    }
    (void)write(client, &response, sizeof(response));
  }

  void Run() {
    while (!stopping.load()) {
      const int client = accept(listener, nullptr, nullptr);
      if (client < 0) {
        if (errno == EINTR) continue;
        if (stopping.load()) break;
        continue;
      }
      ServeClient(client);
      close(client);
    }
  }
};

AuthServer::AuthServer() : impl_(std::make_unique<Impl>()) {}
AuthServer::~AuthServer() { Stop(); }

bool AuthServer::Start(std::string path, Verifier verifier,
                       std::string* error) {
  if (path.empty()) return true;
  if (path.size() >= sizeof(sockaddr_un::sun_path)) {
    *error = "PAM authentication socket path is too long";
    return false;
  }
  const std::filesystem::path parent = std::filesystem::path(path).parent_path();
  std::error_code filesystem_error;
  if (!parent.empty())
    std::filesystem::create_directories(parent, filesystem_error);
  if (filesystem_error) {
    *error = "cannot create PAM authentication socket directory: " +
             filesystem_error.message();
    return false;
  }
  const int listener = socket(AF_UNIX, SOCK_STREAM, 0);
  if (listener < 0) {
    *error = "cannot create PAM authentication socket: " +
             std::string(std::strerror(errno));
    return false;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  (void)unlink(path.c_str());
  if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) !=
          0 ||
      chmod(path.c_str(), 0600) != 0 || listen(listener, 16) != 0) {
    *error = "cannot bind PAM authentication socket: " +
             std::string(std::strerror(errno));
    close(listener);
    (void)unlink(path.c_str());
    return false;
  }
  impl_->listener = listener;
  impl_->path = std::move(path);
  impl_->verifier = std::move(verifier);
  impl_->worker = std::thread([this] { impl_->Run(); });
  return true;
}

void AuthServer::Stop() {
  if (!impl_ || impl_->listener < 0) return;
  impl_->stopping.store(true);
  shutdown(impl_->listener, SHUT_RDWR);
  close(impl_->listener);
  impl_->listener = -1;
  if (impl_->worker.joinable()) impl_->worker.join();
  if (!impl_->path.empty()) (void)unlink(impl_->path.c_str());
}

}  // namespace dang::system

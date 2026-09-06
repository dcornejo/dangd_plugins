// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/**
 * @file
 * Bounded UNIX-domain transport for FRR's native mgmtd protocol.  Framing,
 * timeouts, peer locality, message-size limits, and reply correlation are
 * enforced here so the session layer never handles an untrusted partial frame.
 */

#include "mgmtd_transport.h"

#include <cerrno>
#include <cstring>
#include <limits>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace dang::plugins::frr::mgmtd {
namespace {

template <typename Value>
Value Load(std::span<const std::byte> input, std::size_t offset) {
  Value value{};
  std::memcpy(&value, input.data() + offset, sizeof(value));
  return value;
}

std::string SystemError(std::string_view action) {
  return std::string(action) + ": " + std::strerror(errno);
}

int RemainingMilliseconds(std::chrono::steady_clock::time_point deadline) {
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - std::chrono::steady_clock::now());
  if (remaining.count() <= 0) return 0;
  return static_cast<int>(std::min<std::int64_t>(
      remaining.count(), std::numeric_limits<int>::max()));
}

}  // namespace

Transport::~Transport() {
  Disconnect();
}

void Transport::Disconnect() {
  if (socket_ >= 0) close(socket_);
  socket_ = -1;
}

std::unique_ptr<Transport> Transport::Connect(
    const std::filesystem::path& socket_path,
    std::chrono::milliseconds timeout, std::string* error) {
  const std::string path = socket_path.string();
  if (timeout.count() <= 0) {
    if (error) *error = "mgmtd timeout must be positive";
    return nullptr;
  }
  sockaddr_un address{};
  if (path.empty() || path.size() >= sizeof(address.sun_path)) {
    if (error) *error = "mgmtd UNIX socket path is empty or too long";
    return nullptr;
  }
  const int socket = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (socket < 0) {
    if (error) *error = SystemError("cannot create mgmtd UNIX socket");
    return nullptr;
  }
  const int flags = fcntl(socket, F_GETFL, 0);
  const int descriptor_flags = fcntl(socket, F_GETFD, 0);
  if (flags < 0 || fcntl(socket, F_SETFL, flags | O_NONBLOCK) < 0 ||
      descriptor_flags < 0 ||
      fcntl(socket, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0) {
    if (error) *error = SystemError("cannot secure mgmtd UNIX socket");
    close(socket);
    return nullptr;
  }
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
#if defined(__FreeBSD__) || defined(__APPLE__)
  address.sun_len = static_cast<unsigned char>(SUN_LEN(&address));
  const socklen_t address_length = address.sun_len;
#else
  const socklen_t address_length = static_cast<socklen_t>(
      offsetof(sockaddr_un, sun_path) + path.size() + 1);
#endif
  if (connect(socket, reinterpret_cast<sockaddr*>(&address), address_length) <
          0 &&
      errno != EINPROGRESS) {
    if (error) *error = SystemError("cannot connect to mgmtd UNIX socket");
    close(socket);
    return nullptr;
  }
  pollfd descriptor{socket, POLLOUT, 0};
  const int connect_timeout = static_cast<int>(std::min<std::int64_t>(
      timeout.count(), std::numeric_limits<int>::max()));
  const int polled = poll(&descriptor, 1, connect_timeout);
  if (polled <= 0) {
    if (error)
      *error = polled == 0 ? "mgmtd UNIX socket connection timed out"
                           : SystemError("cannot poll mgmtd UNIX socket");
    close(socket);
    return nullptr;
  }
  int connect_error = 0;
  socklen_t connect_error_size = sizeof(connect_error);
  if (getsockopt(socket, SOL_SOCKET, SO_ERROR, &connect_error,
                 &connect_error_size) < 0 ||
      connect_error != 0) {
    if (connect_error != 0) errno = connect_error;
    if (error) *error = SystemError("cannot connect to mgmtd UNIX socket");
    close(socket);
    return nullptr;
  }
  return std::unique_ptr<Transport>(new Transport(socket, timeout));
}

std::unique_ptr<Transport> Transport::AdoptConnectedSocket(
    int socket, std::chrono::milliseconds timeout, std::string* error) {
  if (socket < 0 || timeout.count() <= 0) {
    if (error) *error = "connected socket and positive timeout are required";
    if (socket >= 0) close(socket);
    return nullptr;
  }
  const int flags = fcntl(socket, F_GETFL, 0);
  const int descriptor_flags = fcntl(socket, F_GETFD, 0);
  if (flags < 0 || fcntl(socket, F_SETFL, flags | O_NONBLOCK) < 0 ||
      descriptor_flags < 0 ||
      fcntl(socket, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0) {
    if (error) *error = SystemError("cannot secure connected mgmtd socket");
    close(socket);
    return nullptr;
  }
  return std::unique_ptr<Transport>(new Transport(socket, timeout));
}

bool Transport::Transfer(bool write, std::span<std::byte> bytes,
                         std::chrono::steady_clock::time_point deadline,
                         std::string* error) {
  std::size_t completed = 0;
  while (completed < bytes.size()) {
    pollfd descriptor{socket_, static_cast<short>(write ? POLLOUT : POLLIN), 0};
    const int polled = poll(&descriptor, 1, RemainingMilliseconds(deadline));
    if (polled == 0) {
      if (error) *error = write ? "mgmtd write timed out" : "mgmtd read timed out";
      return false;
    }
    if (polled < 0) {
      if (errno == EINTR) continue;
      if (error) *error = SystemError("cannot poll mgmtd UNIX socket");
      return false;
    }
    if (descriptor.revents & (POLLERR | POLLNVAL)) {
      if (error) *error = "mgmtd UNIX socket reported an error";
      return false;
    }
    ssize_t transferred = 0;
    if (write)
      transferred = send(socket_, bytes.data() + completed,
                         bytes.size() - completed, MSG_NOSIGNAL);
    else
      transferred = recv(socket_, bytes.data() + completed,
                         bytes.size() - completed, 0);
    if (transferred > 0) {
      completed += static_cast<std::size_t>(transferred);
      continue;
    }
    if (transferred < 0 && (errno == EINTR || errno == EAGAIN ||
                            errno == EWOULDBLOCK))
      continue;
    if (error)
      *error = transferred == 0 ? "mgmtd UNIX socket closed unexpectedly"
                                : SystemError("mgmtd UNIX socket transfer failed");
    return false;
  }
  return true;
}

std::optional<Reply> Transport::Exchange(std::span<const std::byte> request,
                                         Code expected_reply,
                                         bool allow_new_session_reference,
                                         std::string* error) {
  std::string decode_error;
  auto decoded_request = Decode(request, &decode_error);
  if (!decoded_request) {
    if (error) *error = "invalid outgoing request: " + decode_error;
    return std::nullopt;
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout_;
  std::vector<std::byte> outbound(request.begin(), request.end());
  if (!Transfer(true, outbound, deadline, error)) {
    Disconnect();
    return std::nullopt;
  }
  auto reply = ReceiveUntil(deadline, false, nullptr, error);
  if (!reply) return std::nullopt;
  DecodedFrame decoded{reply->header, reply->body};
  if (decoded.header.request != decoded_request->header.request ||
      (!allow_new_session_reference &&
       decoded.header.reference != decoded_request->header.reference)) {
    if (error) *error = "mgmtd reply does not correlate to its request";
    Disconnect();
    return std::nullopt;
  }
  if (decoded.header.code != expected_reply &&
      decoded.header.code != Code::kError) {
    if (error) *error = "mgmtd returned an unexpected reply type";
    Disconnect();
    return std::nullopt;
  }
  if (decoded.header.code == Code::kError) {
    auto message = ErrorText(decoded, &decode_error);
    if (error)
      *error = message ? "mgmtd rejected request: " + *message
                       : "invalid mgmtd error reply: " + decode_error;
    Disconnect();
    return std::nullopt;
  }
  return reply;
}

bool Transport::Send(std::span<const std::byte> request, std::string* error) {
  std::string decode_error;
  if (!Decode(request, &decode_error)) {
    if (error) *error = "invalid outgoing request: " + decode_error;
    return false;
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout_;
  std::vector<std::byte> outbound(request.begin(), request.end());
  if (Transfer(true, outbound, deadline, error)) return true;
  Disconnect();
  return false;
}

std::optional<Reply> Transport::Receive(bool* timed_out, std::string* error) {
  return ReceiveUntil(std::chrono::steady_clock::now() + timeout_, true,
                      timed_out, error);
}

std::optional<Reply> Transport::ReceiveUntil(
    std::chrono::steady_clock::time_point deadline, bool idle_timeout_ok,
    bool* timed_out, std::string* error) {
  if (timed_out) *timed_out = false;
  if (socket_ < 0) {
    if (error) *error = "mgmtd session is disconnected";
    return std::nullopt;
  }
  // Poll before consuming the frame header. A timeout here leaves framing
  // untouched and is therefore safe for long-lived notification sessions.
  pollfd descriptor{socket_, POLLIN, 0};
  int polled = 0;
  do {
    polled = poll(&descriptor, 1, RemainingMilliseconds(deadline));
  } while (polled < 0 && errno == EINTR);
  if (polled == 0 && idle_timeout_ok) {
    if (timed_out) *timed_out = true;
    return std::nullopt;
  }
  if (polled <= 0) {
    if (error)
      *error = polled == 0 ? "mgmtd read timed out"
                           : SystemError("cannot poll mgmtd UNIX socket");
    Disconnect();
    return std::nullopt;
  }
  if (descriptor.revents & (POLLERR | POLLNVAL)) {
    if (error) *error = "mgmtd UNIX socket reported an error";
    Disconnect();
    return std::nullopt;
  }
  std::vector<std::byte> frame(kFrameHeaderBytes);
  if (!Transfer(false, frame, deadline, error)) {
    Disconnect();
    return std::nullopt;
  }
  if (Load<std::uint32_t>(frame, 0) != kNativeMarker) {
    if (error) *error = "mgmtd reply has an unsupported marker or version";
    Disconnect();
    return std::nullopt;
  }
  const std::uint32_t length = Load<std::uint32_t>(frame, 4);
  if (length < kFrameHeaderBytes + kFixedMessageBytes ||
      length > kMaximumFrameBytes) {
    if (error) *error = "mgmtd reply length is invalid";
    Disconnect();
    return std::nullopt;
  }
  frame.resize(length);
  if (!Transfer(false, std::span<std::byte>(frame).subspan(kFrameHeaderBytes),
                deadline, error)) {
    Disconnect();
    return std::nullopt;
  }
  std::string decode_error;
  auto decoded = Decode(frame, &decode_error);
  if (!decoded) {
    if (error) *error = "invalid mgmtd reply: " + decode_error;
    Disconnect();
    return std::nullopt;
  }
  return Reply{decoded->header,
               std::vector<std::byte>(decoded->body.begin(),
                                      decoded->body.end())};
}

}  // namespace dang::plugins::frr::mgmtd

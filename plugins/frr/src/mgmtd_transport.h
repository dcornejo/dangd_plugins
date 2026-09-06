// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANG_PLUGINS_FRR_MGMTD_TRANSPORT_H_
#define DANG_PLUGINS_FRR_MGMTD_TRANSPORT_H_

#include "mgmtd_wire.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace dang::plugins::frr::mgmtd {

/** Validated native header plus an owned copy of its variable payload. */
struct Reply {
  NativeHeader header;
  std::vector<std::byte> body;
};

/** Deadline-bounded owner of one connected local mgmtd stream socket. */
class Transport {
 public:
  ~Transport();
  Transport(const Transport&) = delete;
  Transport& operator=(const Transport&) = delete;

  /** Connects only to a UNIX socket path and applies one timeout per exchange. */
  static std::unique_ptr<Transport> Connect(
      const std::filesystem::path& socket_path,
      std::chrono::milliseconds timeout, std::string* error);

  // Takes ownership of an already connected UNIX stream. This supports
  // socketpair-based protocol tests without weakening pathname validation.
  static std::unique_ptr<Transport> AdoptConnectedSocket(
      int socket, std::chrono::milliseconds timeout, std::string* error);

  // Sends one request and accepts only its expected correlated reply or an
  // ERROR carrying the same request and session identifiers.
  std::optional<Reply> Exchange(std::span<const std::byte> request,
                                Code expected_reply,
                                bool allow_new_session_reference,
                                std::string* error);

  /** Sends a one-way request such as NOTIFY_SELECT. */
  bool Send(std::span<const std::byte> request, std::string* error);

  /**
   * Receives one unsolicited frame. An idle timeout is not a transport error:
   * it returns no frame and sets @p timed_out, leaving the stream connected.
   */
  std::optional<Reply> Receive(bool* timed_out, std::string* error);

 private:
  Transport(int socket, std::chrono::milliseconds timeout)
      : socket_(socket), timeout_(timeout) {}
  void Disconnect();
  bool Transfer(bool write, std::span<std::byte> bytes,
                std::chrono::steady_clock::time_point deadline,
                std::string* error);
  std::optional<Reply> ReceiveUntil(
      std::chrono::steady_clock::time_point deadline, bool idle_timeout_ok,
      bool* timed_out, std::string* error);

  int socket_ = -1;
  std::chrono::milliseconds timeout_;
};

}  // namespace dang::plugins::frr::mgmtd

#endif  // DANG_PLUGINS_FRR_MGMTD_TRANSPORT_H_

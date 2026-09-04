// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANG_PLUGINS_FRR_MGMTD_WIRE_H_
#define DANG_PLUGINS_FRR_MGMTD_WIRE_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dang::plugins::frr::mgmtd {

constexpr std::uint32_t kNativeMarker = 0x23232301u;
constexpr std::size_t kFrameHeaderBytes = 8;
constexpr std::size_t kNativeHeaderBytes = 24;
constexpr std::size_t kFixedMessageBytes = 32;
constexpr std::size_t kMaximumFrameBytes = 16 * 1024 * 1024;

enum class Code : std::uint16_t {
  kError = 0,
  kEdit = 5,
  kEditReply = 6,
  kSessionRequest = 10,
  kSessionReply = 11,
  kLock = 19,
  kLockReply = 20,
  kCommit = 21,
  kCommitReply = 22,
};

enum class Datastore : std::uint8_t {
  kRunning = 1,
  kCandidate = 2,
  kOperational = 3,
};

enum class EditOperation : std::uint8_t {
  kCreate = 0,
  kMerge = 2,
  kRemove = 3,
  kDelete = 4,
  kReplace = 5,
};

enum class CommitAction : std::uint8_t {
  kApply = 0,
  kAbort = 1,
  kValidate = 2,
};

struct NativeHeader {
  Code code;
  std::uint32_t split;
  std::uint64_t reference;
  std::uint64_t request;
};

struct DecodedFrame {
  NativeHeader header;
  std::span<const std::byte> body;
};

std::vector<std::byte> SessionCreate(std::uint64_t client_id,
                                     std::string_view client_name);
std::vector<std::byte> Lock(std::uint64_t session_id,
                            std::uint64_t request_id, Datastore datastore,
                            bool acquire);
std::vector<std::byte> Edit(std::uint64_t session_id,
                            std::uint64_t request_id, Datastore datastore,
                            EditOperation operation, std::string_view xpath,
                            std::string_view xml);
std::vector<std::byte> Commit(std::uint64_t session_id,
                              std::uint64_t request_id, Datastore source,
                              Datastore target, CommitAction action,
                              bool unlock);

// Decodes exactly one complete frame. FRR's native local protocol uses host
// byte order and natural C layout; it is therefore intentionally limited to a
// local UNIX socket and must never be exposed as a network protocol.
std::optional<DecodedFrame> Decode(std::span<const std::byte> frame,
                                   std::string* error);
std::optional<std::string> ErrorText(const DecodedFrame& frame,
                                     std::string* error);

}  // namespace dang::plugins::frr::mgmtd

#endif  // DANG_PLUGINS_FRR_MGMTD_WIRE_H_

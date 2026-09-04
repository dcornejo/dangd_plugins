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

/** FRR native message marker and framing limits mirrored from mgmtd. */
constexpr std::uint32_t kNativeMarker = 0x23232301u;
constexpr std::size_t kFrameHeaderBytes = 8;
constexpr std::size_t kNativeHeaderBytes = 24;
constexpr std::size_t kFixedMessageBytes = 32;
constexpr std::size_t kMaximumFrameBytes = 16 * 1024 * 1024;

/** Native message codes used by the supported session/edit transaction slice. */
enum class Code : std::uint16_t {
  kError = 0,
  kTreeData = 2,
  kGetData = 3,
  kEdit = 5,
  kEditReply = 6,
  kSessionRequest = 10,
  kSessionReply = 11,
  kLock = 19,
  kLockReply = 20,
  kCommit = 21,
  kCommitReply = 22,
};

/** RFC 6243 default-reporting modes accepted by FRR GET_DATA. */
enum class DefaultsMode : std::uint8_t {
  kExplicit = 0,
  kTrim = 1,
  kAll = 2,
  kAllTagged = 3,
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

/** Decoded fields shared by every native mgmtd message. */
struct NativeHeader {
  Code code;
  std::uint32_t split;
  std::uint64_t reference;
  std::uint64_t request;
};

/** Non-owning view into a caller-owned complete frame. */
struct DecodedFrame {
  NativeHeader header;
  std::span<const std::byte> body;
};

/** Validated view of one XML TREE_DATA reply body. */
struct TreeDataResult {
  std::string xml;
  bool more = false;
};

/** Message builders below return one complete length-prefixed native frame. */
std::vector<std::byte> SessionCreate(std::uint64_t client_id,
                                     std::string_view client_name);
std::vector<std::byte> SessionDestroy(std::uint64_t session_id,
                                      std::uint64_t client_id);
std::vector<std::byte> Lock(std::uint64_t session_id,
                            std::uint64_t request_id, Datastore datastore,
                            bool acquire);
std::vector<std::byte> Edit(std::uint64_t session_id,
                            std::uint64_t request_id, Datastore datastore,
                            EditOperation operation, std::string_view xpath,
                            std::string_view xml);
std::vector<std::byte> Delete(std::uint64_t session_id,
                              std::uint64_t request_id, Datastore datastore,
                              std::string_view xpath);
std::vector<std::byte> Commit(std::uint64_t session_id,
                              std::uint64_t request_id, Datastore source,
                              Datastore target, CommitAction action,
                              bool unlock);
/** Requests config and/or state data rooted at an FRR schema XPath. */
std::vector<std::byte> GetData(std::uint64_t session_id,
                               std::uint64_t request_id,
                               Datastore datastore, bool include_state,
                               bool include_config, std::string_view xpath,
                               DefaultsMode defaults = DefaultsMode::kExplicit);

// Decodes exactly one complete frame. FRR's native local protocol uses host
// byte order and natural C layout; it is therefore intentionally limited to a
// local UNIX socket and must never be exposed as a network protocol.
std::optional<DecodedFrame> Decode(std::span<const std::byte> frame,
                                   std::string* error);
std::optional<std::string> ErrorText(const DecodedFrame& frame,
                                     std::string* error);
/** Decodes a successful XML TREE_DATA body and rejects partial results. */
std::optional<TreeDataResult> TreeData(const DecodedFrame& frame,
                                       std::string* error);

}  // namespace dang::plugins::frr::mgmtd

#endif  // DANG_PLUGINS_FRR_MGMTD_WIRE_H_

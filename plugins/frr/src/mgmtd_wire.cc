// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "mgmtd_wire.h"

#include <cstring>
#include <limits>

namespace dang::plugins::frr::mgmtd {
namespace {

template <typename Value>
void Store(std::vector<std::byte>* output, std::size_t offset, Value value) {
  std::memcpy(output->data() + offset, &value, sizeof(value));
}

template <typename Value>
Value Load(std::span<const std::byte> input, std::size_t offset) {
  Value value{};
  std::memcpy(&value, input.data() + offset, sizeof(value));
  return value;
}

std::vector<std::byte> Message(Code code, std::uint64_t reference,
                               std::uint64_t request, std::uint32_t split,
                               std::size_t body_bytes) {
  const std::size_t total = kFrameHeaderBytes + kNativeHeaderBytes + body_bytes;
  if (total > kMaximumFrameBytes) return {};
  std::vector<std::byte> output(total);
  Store(&output, 0, kNativeMarker);
  Store(&output, 4, static_cast<std::uint32_t>(total));
  Store(&output, 8, static_cast<std::uint16_t>(code));
  Store(&output, 12, split);
  Store(&output, 16, reference);
  Store(&output, 24, request);
  return output;
}

void AppendString(std::vector<std::byte>* output, std::size_t offset,
                  std::string_view value) {
  std::memcpy(output->data() + offset, value.data(), value.size());
  (*output)[offset + value.size()] = std::byte{0};
}

bool ValidString(std::string_view value) {
  return value.find('\0') == std::string_view::npos;
}

bool KnownCode(Code code) {
  switch (code) {
    case Code::kError:
    case Code::kEdit:
    case Code::kEditReply:
    case Code::kSessionRequest:
    case Code::kSessionReply:
    case Code::kLock:
    case Code::kLockReply:
    case Code::kCommit:
    case Code::kCommitReply:
      return true;
  }
  return false;
}

}  // namespace

std::vector<std::byte> SessionCreate(std::uint64_t client_id,
                                     std::string_view client_name) {
  if (!ValidString(client_name)) return {};
  auto output = Message(Code::kSessionRequest, 0, client_id, 0,
                        8 + client_name.size() + 1);
  if (output.empty()) return {};
  output[32] = std::byte{2};  // JSON notifications.
  AppendString(&output, 40, client_name);
  return output;
}

std::vector<std::byte> SessionDestroy(std::uint64_t session_id,
                                      std::uint64_t client_id) {
  return Message(Code::kSessionRequest, session_id, client_id, 0, 8);
}

std::vector<std::byte> Lock(std::uint64_t session_id,
                            std::uint64_t request_id, Datastore datastore,
                            bool acquire) {
  auto output = Message(Code::kLock, session_id, request_id, 0, 8);
  output[32] = static_cast<std::byte>(datastore);
  output[33] = static_cast<std::byte>(acquire ? 1 : 0);
  return output;
}

std::vector<std::byte> Edit(std::uint64_t session_id,
                            std::uint64_t request_id, Datastore datastore,
                            EditOperation operation, std::string_view xpath,
                            std::string_view xml) {
  const std::size_t xpath_bytes = xpath.size() + 1;
  if (!ValidString(xpath) || !ValidString(xml) ||
      xpath_bytes > std::numeric_limits<std::uint32_t>::max())
    return {};
  auto output = Message(Code::kEdit, session_id, request_id,
                        static_cast<std::uint32_t>(xpath_bytes),
                        8 + xpath_bytes + xml.size() + 1);
  if (output.empty()) return {};
  output[32] = std::byte{1};  // LYD_XML / MGMT_MSG_FORMAT_XML.
  output[34] = static_cast<std::byte>(datastore);
  output[35] = static_cast<std::byte>(operation);
  AppendString(&output, 40, xpath);
  AppendString(&output, 40 + xpath_bytes, xml);
  return output;
}

std::vector<std::byte> Delete(std::uint64_t session_id,
                              std::uint64_t request_id, Datastore datastore,
                              std::string_view xpath) {
  const std::size_t xpath_bytes = xpath.size() + 1;
  if (!ValidString(xpath) ||
      xpath_bytes > std::numeric_limits<std::uint32_t>::max())
    return {};
  auto output = Message(Code::kEdit, session_id, request_id,
                        static_cast<std::uint32_t>(xpath_bytes),
                        8 + xpath_bytes);
  if (output.empty()) return {};
  output[32] = std::byte{1};
  output[34] = static_cast<std::byte>(datastore);
  output[35] = static_cast<std::byte>(EditOperation::kDelete);
  AppendString(&output, 40, xpath);
  return output;
}

std::vector<std::byte> Commit(std::uint64_t session_id,
                              std::uint64_t request_id, Datastore source,
                              Datastore target, CommitAction action,
                              bool unlock) {
  auto output = Message(Code::kCommit, session_id, request_id, 0, 8);
  output[32] = static_cast<std::byte>(source);
  output[33] = static_cast<std::byte>(target);
  output[34] = static_cast<std::byte>(action);
  output[35] = static_cast<std::byte>(unlock ? 1 : 0);
  return output;
}

std::optional<DecodedFrame> Decode(std::span<const std::byte> frame,
                                   std::string* error) {
  if (frame.size() < kFrameHeaderBytes + kFixedMessageBytes) {
    if (error) *error = "mgmtd frame is shorter than its fixed message";
    return std::nullopt;
  }
  if (Load<std::uint32_t>(frame, 0) != kNativeMarker) {
    if (error) *error = "mgmtd frame has an unsupported marker or version";
    return std::nullopt;
  }
  const std::uint32_t length = Load<std::uint32_t>(frame, 4);
  if (length != frame.size() || length > kMaximumFrameBytes) {
    if (error) *error = "mgmtd frame length is invalid";
    return std::nullopt;
  }
  const Code code = static_cast<Code>(Load<std::uint16_t>(frame, 8));
  if (!KnownCode(code)) {
    if (error) *error = "mgmtd frame has an unknown native message code";
    return std::nullopt;
  }
  DecodedFrame decoded{{code,
                        Load<std::uint32_t>(frame, 12),
                        Load<std::uint64_t>(frame, 16),
                        Load<std::uint64_t>(frame, 24)},
                       frame.subspan(kFrameHeaderBytes + kNativeHeaderBytes)};
  return decoded;
}

std::optional<std::string> ErrorText(const DecodedFrame& frame,
                                     std::string* error) {
  if (frame.header.code != Code::kError || frame.body.size() < 9) {
    if (error) *error = "mgmtd error reply has an invalid fixed body";
    return std::nullopt;
  }
  const auto* bytes = reinterpret_cast<const char*>(frame.body.data());
  const std::size_t text_bytes = frame.body.size() - 8;
  if (bytes[frame.body.size() - 1] != '\0') {
    if (error) *error = "mgmtd error reply is not NUL terminated";
    return std::nullopt;
  }
  if (std::memchr(bytes + 8, '\0', text_bytes - 1) != nullptr) {
    if (error) *error = "mgmtd error reply contains an embedded NUL";
    return std::nullopt;
  }
  return std::string(bytes + 8, text_bytes - 1);
}

}  // namespace dang::plugins::frr::mgmtd

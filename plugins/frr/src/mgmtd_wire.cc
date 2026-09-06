// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/**
 * @file
 * Encoder and decoder for the subset of FRR's native mgmtd wire ABI used by
 * this plugin.  The format mirrors FRR C structure layout and host byte order;
 * consequently these frames are valid only between local compatible builds.
 */

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
    case Code::kTreeData:
    case Code::kGetData:
    case Code::kNotify:
    case Code::kEdit:
    case Code::kEditReply:
    case Code::kRpc:
    case Code::kRpcReply:
    case Code::kNotifySelect:
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

std::vector<std::byte> GetData(std::uint64_t session_id,
                               std::uint64_t request_id,
                               Datastore datastore, bool include_state,
                               bool include_config, std::string_view xpath,
                               DefaultsMode defaults) {
  if (!ValidString(xpath)) return {};
  auto output = Message(Code::kGetData, session_id, request_id, 0,
                        8 + xpath.size() + 1);
  if (output.empty()) return {};
  output[32] = std::byte{1};  // LYD_XML / MGMT_MSG_FORMAT_XML.
  output[33] = static_cast<std::byte>((include_state ? 0x01 : 0) |
                                     (include_config ? 0x02 : 0));
  output[34] = static_cast<std::byte>(defaults);
  output[35] = static_cast<std::byte>(datastore);
  AppendString(&output, 40, xpath);
  return output;
}

std::vector<std::byte> Rpc(std::uint64_t session_id, std::uint64_t request_id,
                           std::string_view xpath, std::string_view xml) {
  const std::size_t xpath_bytes = xpath.size() + 1;
  if (!ValidString(xpath) || !ValidString(xml) ||
      xpath_bytes > std::numeric_limits<std::uint32_t>::max())
    return {};
  auto output = Message(Code::kRpc, session_id, request_id,
                        static_cast<std::uint32_t>(xpath_bytes),
                        8 + xpath_bytes + (xml.empty() ? 0 : xml.size() + 1));
  if (output.empty()) return {};
  output[32] = std::byte{1};  // LYD_XML / MGMT_MSG_FORMAT_XML.
  output[33] = std::byte{0};  // Native YANG rather than RESTCONF wrapping.
  AppendString(&output, 40, xpath);
  if (!xml.empty()) AppendString(&output, 40 + xpath_bytes, xml);
  return output;
}

std::vector<std::byte> NotifySelect(
    std::uint64_t session_id, std::uint64_t request_id, bool replace,
    NotifyMode mode, std::uint32_t interval_milliseconds,
    std::span<const std::string_view> selectors) {
  if (session_id == 0 || request_id == 0 || selectors.empty() ||
      (mode == NotifyMode::kOnChange && interval_milliseconds != 0) ||
      (mode == NotifyMode::kPeriodic && interval_milliseconds == 0))
    return {};
  std::size_t selector_bytes = 0;
  for (const std::string_view selector : selectors) {
    if (selector.empty() || !ValidString(selector) ||
        selector.size() >= kMaximumFrameBytes - selector_bytes)
      return {};
    selector_bytes += selector.size() + 1;
  }
  auto output = Message(Code::kNotifySelect, session_id, request_id, 0,
                        8 + selector_bytes);
  if (output.empty()) return {};
  output[32] = static_cast<std::byte>(replace ? 1 : 0);
  output[33] = std::byte{0};  // get_only is reserved for backend clients.
  output[34] = std::byte{0};  // subscribing is assigned internally by mgmtd.
  output[35] = static_cast<std::byte>(mode);
  Store(&output, 36, interval_milliseconds);
  std::size_t offset = 40;
  for (const std::string_view selector : selectors) {
    AppendString(&output, offset, selector);
    offset += selector.size() + 1;
  }
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

std::optional<TreeDataResult> TreeData(const DecodedFrame& frame,
                                       std::string* error) {
  if (frame.header.code != Code::kTreeData || frame.body.size() < 8) {
    if (error) *error = "mgmtd tree-data reply has an invalid fixed body";
    return std::nullopt;
  }
  const auto partial_error = std::to_integer<std::int8_t>(frame.body[0]);
  if (partial_error != 0) {
    if (error)
      *error = "mgmtd returned partial operational data (error " +
               std::to_string(partial_error) + ")";
    return std::nullopt;
  }
  if (frame.body[1] != std::byte{1}) {
    if (error) *error = "mgmtd tree-data reply is not XML";
    return std::nullopt;
  }
  if (frame.body[2] != std::byte{0} && frame.body[2] != std::byte{1}) {
    if (error) *error = "mgmtd tree-data continuation flag is invalid";
    return std::nullopt;
  }
  const auto* data = reinterpret_cast<const char*>(frame.body.data() + 8);
  std::size_t size = frame.body.size() - 8;
  // FRR's native serializer includes a conventional terminal NUL in the
  // result length. Treat it as framing, while rejecting any earlier NUL that
  // could truncate the XML seen by dangd.
  if (size > 0 && data[size - 1] == '\0') --size;
  if (std::memchr(data, '\0', size) != nullptr) {
    if (error) *error = "mgmtd XML tree-data contains an embedded NUL";
    return std::nullopt;
  }
  return TreeDataResult{std::string(data, size), frame.body[2] == std::byte{1}};
}

std::optional<std::string> RpcReply(const DecodedFrame& frame,
                                    std::string* error) {
  if (frame.header.code != Code::kRpcReply || frame.body.size() < 8) {
    if (error) *error = "mgmtd RPC reply has an invalid fixed body";
    return std::nullopt;
  }
  if (frame.body[0] != std::byte{1} || frame.body[1] != std::byte{0}) {
    if (error) *error = "mgmtd RPC reply is not native YANG XML";
    return std::nullopt;
  }
  const auto* data = reinterpret_cast<const char*>(frame.body.data() + 8);
  std::size_t size = frame.body.size() - 8;
  if (size > 0 && data[size - 1] == '\0') --size;
  if (std::memchr(data, '\0', size) != nullptr) {
    if (error) *error = "mgmtd RPC reply contains an embedded NUL";
    return std::nullopt;
  }
  return std::string(data, size);
}

std::optional<NotifyResult> Notify(const DecodedFrame& frame,
                                   std::string* error) {
  if (frame.header.code != Code::kNotify || frame.body.size() < 10) {
    if (error) *error = "mgmtd notification has an invalid fixed body";
    return std::nullopt;
  }
  if (frame.body[0] != std::byte{1}) {
    if (error) *error = "mgmtd notification is not XML";
    return std::nullopt;
  }
  // Operation zero is a modeled YANG notification. The remaining operation
  // values describe datastore synchronization and must not be mislabeled as
  // RFC 5277 events by the dangd provider.
  if (frame.body[1] != std::byte{0}) {
    if (error) *error = "mgmtd frame is not a modeled notification";
    return std::nullopt;
  }
  const std::size_t split = frame.header.split;
  const std::size_t variable_bytes = frame.body.size() - 8;
  if (split == 0 || split >= variable_bytes ||
      frame.body[8 + split - 1] != std::byte{0}) {
    if (error) *error = "mgmtd notification XPath framing is invalid";
    return std::nullopt;
  }
  const auto* variable = reinterpret_cast<const char*>(frame.body.data() + 8);
  if (std::memchr(variable, '\0', split - 1) != nullptr) {
    if (error) *error = "mgmtd notification XPath contains an embedded NUL";
    return std::nullopt;
  }
  const char* data = variable + split;
  std::size_t size = variable_bytes - split;
  if (size > 0 && data[size - 1] == '\0') --size;
  if (size == 0 || std::memchr(data, '\0', size) != nullptr) {
    if (error) *error = "mgmtd notification XML is empty or contains an embedded NUL";
    return std::nullopt;
  }
  return NotifyResult{std::string(variable, split - 1),
                      std::string(data, size)};
}

}  // namespace dang::plugins::frr::mgmtd

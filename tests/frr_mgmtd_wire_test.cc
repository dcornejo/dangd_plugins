// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "plugins/frr/src/mgmtd_wire.h"

#include <gtest/gtest.h>

#include <array>
#include <cstring>

namespace {
using namespace dang::plugins::frr::mgmtd;

template <typename Value>
Value Load(const std::vector<std::byte>& bytes, std::size_t offset) {
  Value value{};
  std::memcpy(&value, bytes.data() + offset, sizeof(value));
  return value;
}

template <typename Value>
void Store(std::vector<std::byte>* bytes, std::size_t offset, Value value) {
  std::memcpy(bytes->data() + offset, &value, sizeof(value));
}

TEST(FrrMgmtdWireTest, EncodesSessionLockEditAndCommitLayouts) {
  const auto session = SessionCreate(41, "dangd-frr");
  EXPECT_EQ(Load<std::uint32_t>(session, 0), kNativeMarker);
  EXPECT_EQ(Load<std::uint32_t>(session, 4), session.size());
  EXPECT_EQ(Load<std::uint16_t>(session, 8),
            static_cast<std::uint16_t>(Code::kSessionRequest));
  EXPECT_EQ(Load<std::uint64_t>(session, 24), 41);
  EXPECT_STREQ(reinterpret_cast<const char*>(session.data() + 40), "dangd-frr");

  const auto lock = Lock(82, 3, Datastore::kCandidate, true);
  EXPECT_EQ(lock.size(), 40);
  EXPECT_EQ(Load<std::uint64_t>(lock, 16), 82);
  EXPECT_EQ(lock[32], std::byte{2});
  EXPECT_EQ(lock[33], std::byte{1});

  const auto edit = Edit(82, 4, Datastore::kCandidate,
                         EditOperation::kReplace, "/frr-routing:routing",
                         "<routing/>");
  EXPECT_EQ(Load<std::uint32_t>(edit, 12),
            std::string_view("/frr-routing:routing").size() + 1);
  EXPECT_EQ(edit[32], std::byte{1});
  EXPECT_EQ(edit[34], std::byte{2});
  EXPECT_EQ(edit[35], std::byte{5});
  EXPECT_STREQ(reinterpret_cast<const char*>(edit.data() + 40),
               "/frr-routing:routing");

  const auto deletion =
      Delete(82, 6, Datastore::kCandidate, "/frr-zebra:zebra");
  EXPECT_EQ(deletion[35], std::byte{4});
  EXPECT_EQ(deletion.size(),
            40 + std::string_view("/frr-zebra:zebra").size() + 1);
  EXPECT_STREQ(reinterpret_cast<const char*>(deletion.data() + 40),
               "/frr-zebra:zebra");

  const auto commit = Commit(82, 5, Datastore::kCandidate,
                             Datastore::kRunning, CommitAction::kValidate,
                             false);
  EXPECT_EQ(commit.size(), 40);
  EXPECT_EQ(commit[32], std::byte{2});
  EXPECT_EQ(commit[33], std::byte{1});
  EXPECT_EQ(commit[34], std::byte{2});

  const auto get = GetData(82, 9, Datastore::kOperational, true, false,
                           "/*", DefaultsMode::kExplicit);
  EXPECT_EQ(Load<std::uint16_t>(get, 8),
            static_cast<std::uint16_t>(Code::kGetData));
  EXPECT_EQ(get[32], std::byte{1});
  EXPECT_EQ(get[33], std::byte{1});
  EXPECT_EQ(get[34], std::byte{0});
  EXPECT_EQ(get[35], std::byte{3});
  EXPECT_STREQ(reinterpret_cast<const char*>(get.data() + 40), "/*");

  const auto rpc = Rpc(82, 10, "/frr-zebra:get-vrf-info", "<input/>");
  EXPECT_EQ(Load<std::uint16_t>(rpc, 8),
            static_cast<std::uint16_t>(Code::kRpc));
  EXPECT_EQ(Load<std::uint32_t>(rpc, 12),
            std::string_view("/frr-zebra:get-vrf-info").size() + 1);
  EXPECT_EQ(rpc[32], std::byte{1});
  EXPECT_EQ(rpc[33], std::byte{0});
  EXPECT_STREQ(reinterpret_cast<const char*>(rpc.data() + 40),
               "/frr-zebra:get-vrf-info");
}

TEST(FrrMgmtdWireTest, DecodesCompleteXmlTreeData) {
  auto frame = GetData(82, 9, Datastore::kOperational, true, false, "/*");
  Store(&frame, 8, static_cast<std::uint16_t>(Code::kTreeData));
  const std::string xml = "<routing xmlns=\"http://frrouting.org/yang/routing\"/>";
  frame.resize(40 + xml.size() + 1);
  Store(&frame, 4, static_cast<std::uint32_t>(frame.size()));
  frame[32] = std::byte{0};
  frame[33] = std::byte{1};
  frame[34] = std::byte{0};
  std::memcpy(frame.data() + 40, xml.data(), xml.size());
  frame.back() = std::byte{0};
  std::string error;
  auto decoded = Decode(frame, &error);
  ASSERT_TRUE(decoded) << error;
  auto result = TreeData(*decoded, &error);
  ASSERT_TRUE(result) << error;
  EXPECT_EQ(result->xml, xml);
  EXPECT_FALSE(result->more);

  frame[41] = std::byte{0};
  decoded = Decode(frame, &error);
  ASSERT_TRUE(decoded);
  EXPECT_FALSE(TreeData(*decoded, &error));
  EXPECT_NE(error.find("embedded NUL"), std::string::npos);
  frame[41] = static_cast<std::byte>(xml[1]);

  frame[32] = std::byte{5};
  decoded = Decode(frame, &error);
  ASSERT_TRUE(decoded);
  EXPECT_FALSE(TreeData(*decoded, &error));
  EXPECT_NE(error.find("partial"), std::string::npos);
}

TEST(FrrMgmtdWireTest, DecodesAndValidatesCompleteFrames) {
  auto frame = Commit(19, 7, Datastore::kCandidate, Datastore::kRunning,
                      CommitAction::kApply, true);
  // Turn the request into a reply while retaining the identical fixed layout.
  const auto reply = static_cast<std::uint16_t>(Code::kCommitReply);
  std::memcpy(frame.data() + 8, &reply, sizeof(reply));
  std::string error;
  auto decoded = Decode(frame, &error);
  ASSERT_TRUE(decoded) << error;
  EXPECT_EQ(decoded->header.code, Code::kCommitReply);
  EXPECT_EQ(decoded->header.reference, 19);
  EXPECT_EQ(decoded->header.request, 7);
  EXPECT_EQ(decoded->body.size(), 8);

  frame[0] = std::byte{0};
  EXPECT_FALSE(Decode(frame, &error));
  EXPECT_NE(error.find("marker"), std::string::npos);
}

TEST(FrrMgmtdWireTest, DecodesNativeXmlRpcReply) {
  auto frame = Rpc(82, 10, "/frr-zebra:get-vrf-info", "");
  Store(&frame, 8, static_cast<std::uint16_t>(Code::kRpcReply));
  const std::string xml = "<vrf-list><name>blue</name></vrf-list>";
  frame.resize(40 + xml.size() + 1);
  Store(&frame, 4, static_cast<std::uint32_t>(frame.size()));
  frame[32] = std::byte{1};
  frame[33] = std::byte{0};
  std::memcpy(frame.data() + 40, xml.data(), xml.size());
  frame.back() = std::byte{0};
  std::string error;
  auto decoded = Decode(frame, &error);
  ASSERT_TRUE(decoded) << error;
  EXPECT_EQ(RpcReply(*decoded, &error), xml) << error;

  frame[33] = std::byte{1};
  decoded = Decode(frame, &error);
  ASSERT_TRUE(decoded);
  EXPECT_FALSE(RpcReply(*decoded, &error));
}

TEST(FrrMgmtdWireTest, EncodesNotificationSelection) {
  const std::string_view selectors[]{"/frr-isisd:*", "/frr-ripd:*"};
  const auto request = NotifySelect(82, 11, true, NotifyMode::kOnChange, 0,
                                    selectors);
  ASSERT_FALSE(request.empty());
  EXPECT_EQ(Load<std::uint16_t>(request, 8),
            static_cast<std::uint16_t>(Code::kNotifySelect));
  EXPECT_EQ(Load<std::uint64_t>(request, 16), 82);
  EXPECT_EQ(Load<std::uint64_t>(request, 24), 11);
  EXPECT_EQ(request[32], std::byte{1});
  EXPECT_EQ(request[35], std::byte{0});
  EXPECT_EQ(Load<std::uint32_t>(request, 36), 0);
  EXPECT_STREQ(reinterpret_cast<const char*>(request.data() + 40),
               "/frr-isisd:*");
  EXPECT_STREQ(reinterpret_cast<const char*>(request.data() + 40 +
                                                selectors[0].size() + 1),
               "/frr-ripd:*");

  EXPECT_TRUE(NotifySelect(82, 11, true, NotifyMode::kOnChange, 1, selectors)
                  .empty());
  EXPECT_TRUE(NotifySelect(82, 11, true, NotifyMode::kPeriodic, 0, selectors)
                  .empty());
  EXPECT_TRUE(NotifySelect(82, 11, true, NotifyMode::kOnChange, 0, {}).empty());
}

TEST(FrrMgmtdWireTest, DecodesModeledXmlNotification) {
  const std::string xpath = "/frr-ripd:authentication-failure";
  const std::string xml =
      "<authentication-failure xmlns=\"http://frrouting.org/yang/ripd\"/>";
  auto frame = NotifySelect(82, 11, true, NotifyMode::kOnChange, 0,
                            std::array<std::string_view, 1>{"/frr-ripd:*"});
  Store(&frame, 8, static_cast<std::uint16_t>(Code::kNotify));
  Store(&frame, 12, static_cast<std::uint32_t>(xpath.size() + 1));
  frame.resize(40 + xpath.size() + 1 + xml.size() + 1);
  Store(&frame, 4, static_cast<std::uint32_t>(frame.size()));
  frame[32] = std::byte{1};
  frame[33] = std::byte{0};
  std::memcpy(frame.data() + 40, xpath.c_str(), xpath.size() + 1);
  std::memcpy(frame.data() + 40 + xpath.size() + 1, xml.c_str(), xml.size() + 1);
  std::string error;
  auto decoded = Decode(frame, &error);
  ASSERT_TRUE(decoded) << error;
  const auto notification = Notify(*decoded, &error);
  ASSERT_TRUE(notification) << error;
  EXPECT_EQ(notification->xpath, xpath);
  EXPECT_EQ(notification->xml, xml);

  frame[33] = std::byte{1};
  decoded = Decode(frame, &error);
  ASSERT_TRUE(decoded);
  EXPECT_FALSE(Notify(*decoded, &error));
  EXPECT_NE(error.find("not a modeled"), std::string::npos);
  frame[33] = std::byte{0};

  Store(&frame, 12, static_cast<std::uint32_t>(xpath.size()));
  decoded = Decode(frame, &error);
  ASSERT_TRUE(decoded);
  EXPECT_FALSE(Notify(*decoded, &error));
  EXPECT_NE(error.find("XPath framing"), std::string::npos);
}

TEST(FrrMgmtdWireTest, RejectsUnterminatedErrorText) {
  auto frame = Lock(19, 8, Datastore::kCandidate, false);
  const auto error_code = static_cast<std::uint16_t>(Code::kError);
  std::memcpy(frame.data() + 8, &error_code, sizeof(error_code));
  frame.push_back(std::byte{'x'});
  const auto length = static_cast<std::uint32_t>(frame.size());
  std::memcpy(frame.data() + 4, &length, sizeof(length));
  std::string error;
  auto decoded = Decode(frame, &error);
  ASSERT_TRUE(decoded) << error;
  EXPECT_FALSE(ErrorText(*decoded, &error));
  EXPECT_NE(error.find("NUL"), std::string::npos);
}

TEST(FrrMgmtdWireTest, RejectsEmbeddedNulAndUnknownMessageCode) {
  EXPECT_TRUE(SessionCreate(1, std::string_view("bad\0name", 8)).empty());
  EXPECT_TRUE(Edit(1, 2, Datastore::kCandidate, EditOperation::kReplace,
                   std::string_view("/bad\0path", 9), "<x/>")
                  .empty());
  auto frame = Lock(1, 3, Datastore::kCandidate, true);
  const std::uint16_t unknown = 65535;
  std::memcpy(frame.data() + 8, &unknown, sizeof(unknown));
  std::string error;
  EXPECT_FALSE(Decode(frame, &error));
  EXPECT_NE(error.find("unknown"), std::string::npos);

  frame.resize(kFrameHeaderBytes + kNativeHeaderBytes);
  const auto short_length = static_cast<std::uint32_t>(frame.size());
  std::memcpy(frame.data() + 4, &short_length, sizeof(short_length));
  EXPECT_FALSE(Decode(frame, &error));
  EXPECT_NE(error.find("fixed"), std::string::npos);
}

}  // namespace

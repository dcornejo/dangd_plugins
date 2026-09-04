// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "plugins/frr/src/mgmtd_wire.h"

#include <gtest/gtest.h>

#include <cstring>

namespace {
using namespace dang::plugins::frr::mgmtd;

template <typename Value>
Value Load(const std::vector<std::byte>& bytes, std::size_t offset) {
  Value value{};
  std::memcpy(&value, bytes.data() + offset, sizeof(value));
  return value;
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

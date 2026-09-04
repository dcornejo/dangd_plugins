// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "plugins/frr/src/mgmtd_session.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

namespace {
using namespace dang::plugins::frr::mgmtd;

bool TransferAll(int socket, void* data, std::size_t size, bool write) {
  auto* bytes = static_cast<std::byte*>(data);
  std::size_t completed = 0;
  while (completed < size) {
    const ssize_t result = write
        ? send(socket, bytes + completed, size - completed, 0)
        : recv(socket, bytes + completed, size - completed, 0);
    if (result <= 0) return false;
    completed += static_cast<std::size_t>(result);
  }
  return true;
}

std::vector<std::byte> ReceiveFrame(int socket) {
  std::vector<std::byte> frame(kFrameHeaderBytes);
  if (!TransferAll(socket, frame.data(), frame.size(), false)) return {};
  std::uint32_t length = 0;
  std::memcpy(&length, frame.data() + 4, sizeof(length));
  if (length < frame.size() || length > kMaximumFrameBytes) return {};
  frame.resize(length);
  if (!TransferAll(socket, frame.data() + kFrameHeaderBytes,
                   frame.size() - kFrameHeaderBytes, false))
    return {};
  return frame;
}

template <typename Value>
void Store(std::vector<std::byte>* bytes, std::size_t offset, Value value) {
  std::memcpy(bytes->data() + offset, &value, sizeof(value));
}

void SuccessfulServer(int socket, std::vector<Code>* requests) {
  while (true) {
    auto frame = ReceiveFrame(socket);
    if (frame.empty()) break;
    std::string error;
    auto decoded = Decode(frame, &error);
    if (!decoded) break;
    requests->push_back(decoded->header.code);
    Code reply_code{};
    switch (decoded->header.code) {
      case Code::kSessionRequest:
        reply_code = Code::kSessionReply;
        if (decoded->header.reference == 0) {
          frame.resize(kFrameHeaderBytes + kFixedMessageBytes);
          Store(&frame, 4, static_cast<std::uint32_t>(frame.size()));
          Store(&frame, 16, std::uint64_t{501});
          frame[32] = std::byte{1};
        } else {
          frame[32] = std::byte{0};
        }
        break;
      case Code::kLock:
        reply_code = Code::kLockReply;
        break;
      case Code::kEdit:
        reply_code = Code::kEditReply;
        frame[32] = std::byte{1};
        frame[33] = std::byte{0};
        break;
      case Code::kCommit:
        reply_code = Code::kCommitReply;
        break;
      case Code::kGetData: {
        reply_code = Code::kTreeData;
        const std::string xml =
            "<routing xmlns=\"http://frrouting.org/yang/routing\"/>";
        frame.resize(40 + xml.size() + 1);
        Store(&frame, 4, static_cast<std::uint32_t>(frame.size()));
        frame[32] = std::byte{0};
        frame[33] = std::byte{1};
        frame[34] = std::byte{0};
        std::memcpy(frame.data() + 40, xml.data(), xml.size());
        frame.back() = std::byte{0};
        break;
      }
      default:
        close(socket);
        return;
    }
    Store(&frame, 8, static_cast<std::uint16_t>(reply_code));
    if (!TransferAll(socket, frame.data(), frame.size(), true)) break;
  }
  close(socket);
}

TEST(FrrMgmtdSessionTest, EnforcesCompleteValidationLifecycle) {
  int sockets[2]{-1, -1};
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
  std::vector<Code> requests;
  std::thread server(SuccessfulServer, sockets[1], &requests);
  std::string error;
  auto transport = Transport::AdoptConnectedSocket(
      sockets[0], std::chrono::seconds(1), &error);
  ASSERT_TRUE(transport) << error;
  auto session = Session::Open(std::move(transport), 77, "dangd-frr-test", &error);
  ASSERT_TRUE(session) << error;
  EXPECT_EQ(session->id(), 501);
  EXPECT_TRUE(session->LockCandidate(&error)) << error;
  EXPECT_TRUE(session->ReplaceCandidate("/frr-routing:routing", "<routing/>",
                                        &error))
      << error;
  EXPECT_TRUE(session->ValidateCandidate(&error)) << error;
  EXPECT_TRUE(session->AbortCandidate(&error)) << error;
  EXPECT_TRUE(session->UnlockCandidate(&error)) << error;
  EXPECT_TRUE(session->Close(&error)) << error;
  server.join();
  EXPECT_EQ(requests,
            std::vector<Code>({Code::kSessionRequest, Code::kLock, Code::kEdit,
                               Code::kCommit, Code::kCommit, Code::kLock,
                               Code::kSessionRequest}));
}

TEST(FrrMgmtdSessionTest, RejectsOutOfOrderOperationsLocally) {
  int sockets[2]{-1, -1};
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
  std::vector<Code> requests;
  std::thread server(SuccessfulServer, sockets[1], &requests);
  std::string error;
  auto transport = Transport::AdoptConnectedSocket(
      sockets[0], std::chrono::seconds(1), &error);
  auto session = Session::Open(std::move(transport), 78, "dangd-frr-test", &error);
  ASSERT_TRUE(session) << error;
  EXPECT_FALSE(session->ReplaceCandidate("/x", "<x/>", &error));
  EXPECT_NE(error.find("locked"), std::string::npos);
  EXPECT_FALSE(session->UnlockCandidate(&error));
  EXPECT_TRUE(session->Close(&error)) << error;
  server.join();
  EXPECT_EQ(requests,
            std::vector<Code>({Code::kSessionRequest, Code::kSessionRequest}));
}

TEST(FrrMgmtdSessionTest, RetrievesLiveOperationalXml) {
  int sockets[2]{-1, -1};
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
  std::vector<Code> requests;
  std::thread server(SuccessfulServer, sockets[1], &requests);
  std::string error;
  auto transport = Transport::AdoptConnectedSocket(
      sockets[0], std::chrono::seconds(1), &error);
  auto session = Session::Open(std::move(transport), 79, "dangd-frr-test", &error);
  ASSERT_TRUE(session) << error;
  auto xml = session->GetOperationalData("/*", &error);
  ASSERT_TRUE(xml) << error;
  EXPECT_NE(xml->find("<routing"), std::string::npos);
  EXPECT_TRUE(session->Close(&error)) << error;
  server.join();
  EXPECT_EQ(requests,
            std::vector<Code>({Code::kSessionRequest, Code::kGetData,
                               Code::kSessionRequest}));
}

}  // namespace

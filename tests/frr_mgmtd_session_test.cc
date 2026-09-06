// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "plugins/frr/src/mgmtd_session.h"

#include <gtest/gtest.h>

#include <array>
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
        // FRR currently maps abort completion to the non-validation action.
        if (frame[34] == std::byte{1}) frame[34] = std::byte{0};
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
      case Code::kRpc: {
        reply_code = Code::kRpcReply;
        const std::string xml = "<vrf-list><name>blue</name></vrf-list>";
        frame.resize(40 + xml.size() + 1);
        Store(&frame, 4, static_cast<std::uint32_t>(frame.size()));
        frame[32] = std::byte{1};
        frame[33] = std::byte{0};
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
            std::vector<Code>({Code::kSessionRequest, Code::kLock, Code::kLock,
                               Code::kEdit,
                               Code::kCommit, Code::kCommit, Code::kLock,
                               Code::kLock,
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
  auto running = session->GetRunningConfiguration("/frr-routing:routing",
                                                  &error);
  ASSERT_TRUE(running) << error;
  EXPECT_NE(running->find("<routing"), std::string::npos);
  EXPECT_TRUE(session->Close(&error)) << error;
  server.join();
  EXPECT_EQ(requests,
            std::vector<Code>({Code::kSessionRequest, Code::kGetData,
                               Code::kGetData,
                               Code::kSessionRequest}));
}

TEST(FrrMgmtdSessionTest, InvokesModeledRpcAndReturnsXml) {
  int sockets[2]{-1, -1};
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
  std::vector<Code> requests;
  std::thread server(SuccessfulServer, sockets[1], &requests);
  std::string error;
  auto transport = Transport::AdoptConnectedSocket(
      sockets[0], std::chrono::seconds(1), &error);
  auto session = Session::Open(std::move(transport), 80, "dangd-frr-test", &error);
  ASSERT_TRUE(session) << error;
  auto output = session->InvokeRpc("/frr-zebra:get-vrf-info", "", &error);
  ASSERT_TRUE(output) << error;
  EXPECT_EQ(*output, "<vrf-list><name>blue</name></vrf-list>");
  EXPECT_TRUE(session->Close(&error)) << error;
  server.join();
  EXPECT_EQ(requests,
            std::vector<Code>({Code::kSessionRequest, Code::kRpc,
                               Code::kSessionRequest}));
}

TEST(FrrMgmtdSessionTest, SelectsAndReceivesModeledNotifications) {
  int sockets[2]{-1, -1};
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
  std::vector<Code> requests;
  std::thread server([&] {
    auto create = ReceiveFrame(sockets[1]);
    ASSERT_FALSE(create.empty());
    requests.push_back(Code::kSessionRequest);
    create.resize(kFrameHeaderBytes + kFixedMessageBytes);
    Store(&create, 4, static_cast<std::uint32_t>(create.size()));
    Store(&create, 8, static_cast<std::uint16_t>(Code::kSessionReply));
    Store(&create, 16, std::uint64_t{601});
    create[32] = std::byte{1};
    ASSERT_TRUE(TransferAll(sockets[1], create.data(), create.size(), true));

    auto selection = ReceiveFrame(sockets[1]);
    ASSERT_FALSE(selection.empty());
    std::string decode_error;
    auto selected = Decode(selection, &decode_error);
    ASSERT_TRUE(selected) << decode_error;
    requests.push_back(selected->header.code);
    EXPECT_EQ(selected->header.code, Code::kNotifySelect);
    EXPECT_STREQ(reinterpret_cast<const char*>(selection.data() + 40),
                 "/frr-ripd:*");

    const std::string xpath = "/frr-ripd:authentication-failure";
    const std::string xml =
        "<authentication-failure xmlns=\"http://frrouting.org/yang/ripd\">"
        "<interface-name>test0</interface-name></authentication-failure>";
    auto event = NotifySelect(601, 1, true, NotifyMode::kOnChange, 0,
                              std::array<std::string_view, 1>{"/frr-ripd:*"});
    // Reuse the public fixed layout, then model mgmtd's unsolicited header.
    Store(&event, 8, static_cast<std::uint16_t>(Code::kNotify));
    Store(&event, 24, std::uint64_t{0});
    Store(&event, 12, static_cast<std::uint32_t>(xpath.size() + 1));
    event.resize(40 + xpath.size() + 1 + xml.size() + 1);
    Store(&event, 4, static_cast<std::uint32_t>(event.size()));
    event[32] = std::byte{1};
    event[33] = std::byte{0};
    std::memcpy(event.data() + 40, xpath.c_str(), xpath.size() + 1);
    std::memcpy(event.data() + 40 + xpath.size() + 1, xml.c_str(),
                xml.size() + 1);
    ASSERT_TRUE(TransferAll(sockets[1], event.data(), event.size(), true));

    auto destroy = ReceiveFrame(sockets[1]);
    ASSERT_FALSE(destroy.empty());
    requests.push_back(Code::kSessionRequest);
    Store(&destroy, 8, static_cast<std::uint16_t>(Code::kSessionReply));
    destroy[32] = std::byte{0};
    ASSERT_TRUE(TransferAll(sockets[1], destroy.data(), destroy.size(), true));
    close(sockets[1]);
  });

  std::string error;
  auto transport = Transport::AdoptConnectedSocket(
      sockets[0], std::chrono::seconds(1), &error);
  auto session = Session::Open(std::move(transport), 81, "dangd-frr-events",
                               &error);
  ASSERT_TRUE(session) << error;
  const std::string_view selectors[]{"/frr-ripd:*"};
  ASSERT_TRUE(session->SelectNotifications(selectors, &error)) << error;
  bool timed_out = false;
  auto event = session->NextNotification(&timed_out, &error);
  ASSERT_TRUE(event) << error;
  EXPECT_FALSE(timed_out);
  EXPECT_EQ(event->xpath, "/frr-ripd:authentication-failure");
  EXPECT_NE(event->xml.find("<interface-name>test0</interface-name>"),
            std::string::npos);
  EXPECT_TRUE(session->Close(&error)) << error;
  server.join();
  EXPECT_EQ(requests,
            std::vector<Code>({Code::kSessionRequest, Code::kNotifySelect,
                               Code::kSessionRequest}));
}

}  // namespace

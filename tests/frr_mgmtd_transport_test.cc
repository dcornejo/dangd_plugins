// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "plugins/frr/src/mgmtd_transport.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <functional>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

namespace {
using namespace dang::plugins::frr::mgmtd;

template <typename Value>
void Store(std::vector<std::byte>* bytes, std::size_t offset, Value value) {
  std::memcpy(bytes->data() + offset, &value, sizeof(value));
}

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

class FakeMgmtd {
 public:
  using Handler = std::function<std::vector<std::byte>(std::vector<std::byte>)>;

  explicit FakeMgmtd(Handler handler) : handler_(std::move(handler)) {
    int sockets[2]{-1, -1};
    EXPECT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
    client_ = sockets[0];
    server_ = sockets[1];
    worker_ = std::thread([this] { Run(); });
  }

  ~FakeMgmtd() {
    if (worker_.joinable()) worker_.join();
    if (client_ >= 0) close(client_);
    if (server_ >= 0) close(server_);
  }

  int TakeClient() {
    const int result = client_;
    client_ = -1;
    return result;
  }

 private:
  void Run() {
    std::vector<std::byte> request(kFrameHeaderBytes);
    if (!TransferAll(server_, request.data(), request.size(), false)) {
      return;
    }
    std::uint32_t length = 0;
    std::memcpy(&length, request.data() + 4, sizeof(length));
    if (length < request.size() || length > kMaximumFrameBytes) {
      return;
    }
    request.resize(length);
    if (!TransferAll(server_, request.data() + kFrameHeaderBytes,
                     request.size() - kFrameHeaderBytes, false)) {
      return;
    }
    std::vector<std::byte> reply = handler_(std::move(request));
    if (!reply.empty())
      TransferAll(server_, reply.data(), reply.size(), true);
    close(server_);
    server_ = -1;
  }

  Handler handler_;
  int client_ = -1;
  int server_ = -1;
  std::thread worker_;
};

TEST(FrrMgmtdTransportTest, ExchangesAndCorrelatesSessionReply) {
  FakeMgmtd server([](std::vector<std::byte> request) {
    Store(&request, 8, static_cast<std::uint16_t>(Code::kSessionReply));
    Store(&request, 16, std::uint64_t{991});
    request[32] = std::byte{1};
    return request;
  });
  std::string error;
  auto transport = Transport::AdoptConnectedSocket(
      server.TakeClient(), std::chrono::seconds(1), &error);
  ASSERT_TRUE(transport) << error;
  const auto request = SessionCreate(73, "dangd-frr-test");
  auto reply = transport->Exchange(request, Code::kSessionReply, true, &error);
  ASSERT_TRUE(reply) << error;
  EXPECT_EQ(reply->header.reference, 991);
  EXPECT_EQ(reply->header.request, 73);
}

TEST(FrrMgmtdTransportTest, RejectsMismatchedRequestIdentifier) {
  FakeMgmtd server([](std::vector<std::byte> request) {
    Store(&request, 8, static_cast<std::uint16_t>(Code::kLockReply));
    Store(&request, 24, std::uint64_t{999});
    return request;
  });
  std::string error;
  auto transport = Transport::AdoptConnectedSocket(
      server.TakeClient(), std::chrono::seconds(1), &error);
  ASSERT_TRUE(transport) << error;
  const auto request = Lock(12, 4, Datastore::kCandidate, true);
  EXPECT_FALSE(transport->Exchange(request, Code::kLockReply, false, &error));
  EXPECT_NE(error.find("correlate"), std::string::npos);
}

TEST(FrrMgmtdTransportTest, TimesOutWhenPeerDoesNotReply) {
  FakeMgmtd server([](std::vector<std::byte>) {
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    return std::vector<std::byte>{};
  });
  std::string error;
  auto transport = Transport::AdoptConnectedSocket(
      server.TakeClient(), std::chrono::milliseconds(20), &error);
  ASSERT_TRUE(transport) << error;
  const auto request = Lock(12, 4, Datastore::kCandidate, true);
  EXPECT_FALSE(transport->Exchange(request, Code::kLockReply, false, &error));
  EXPECT_NE(error.find("timed out"), std::string::npos);
}

}  // namespace

// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANG_PLUGINS_FRR_MGMTD_SESSION_H_
#define DANG_PLUGINS_FRR_MGMTD_SESSION_H_

#include "mgmtd_transport.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace dang::plugins::frr::mgmtd {

class Session {
 public:
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  static std::unique_ptr<Session> Open(std::unique_ptr<Transport> transport,
                                       std::uint64_t client_id,
                                       std::string_view client_name,
                                       std::string* error);

  bool LockCandidate(std::string* error);
  bool ReplaceCandidate(std::string_view xpath, std::string_view xml,
                        std::string* error);
  bool ValidateCandidate(std::string* error);
  bool ApplyCandidate(std::string* error);
  bool AbortCandidate(std::string* error);
  bool UnlockCandidate(std::string* error);
  bool Close(std::string* error);

  std::uint64_t id() const { return session_id_; }
  bool candidate_locked() const { return candidate_locked_; }

 private:
  Session(std::unique_ptr<Transport> transport, std::uint64_t client_id,
          std::uint64_t session_id)
      : transport_(std::move(transport)),
        client_id_(client_id),
        session_id_(session_id) {}

  std::optional<std::uint64_t> NextRequest(std::string* error);
  bool CommitCandidate(CommitAction action, std::string* error);

  std::unique_ptr<Transport> transport_;
  std::uint64_t client_id_ = 0;
  std::uint64_t session_id_ = 0;
  std::uint64_t next_request_ = 1;
  bool candidate_locked_ = false;
};

}  // namespace dang::plugins::frr::mgmtd

#endif  // DANG_PLUGINS_FRR_MGMTD_SESSION_H_

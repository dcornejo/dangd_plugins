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

/** Mockable transaction surface used by FrrTransaction and its unit tests. */
class SessionOperations {
 public:
  virtual ~SessionOperations() = default;
  virtual bool LockCandidate(std::string* error) = 0;
  virtual bool ReplaceCandidate(std::string_view xpath, std::string_view xml,
                                std::string* error) = 0;
  virtual bool DeleteCandidate(std::string_view xpath, std::string* error) = 0;
  virtual bool ValidateCandidate(std::string* error) = 0;
  virtual bool ApplyCandidate(std::string* error) = 0;
  virtual bool AbortCandidate(std::string* error) = 0;
  virtual bool UnlockCandidate(std::string* error) = 0;
  virtual bool Close(std::string* error) = 0;
};

/** One correlated client session on an already connected mgmtd transport. */
class Session : public SessionOperations {
 public:
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  /** Creates the remote FRR session and consumes the returned session id. */
  static std::unique_ptr<Session> Open(std::unique_ptr<Transport> transport,
                                       std::uint64_t client_id,
                                       std::string_view client_name,
                                       std::string* error);

  bool LockCandidate(std::string* error) override;
  bool ReplaceCandidate(std::string_view xpath, std::string_view xml,
                        std::string* error) override;
  bool DeleteCandidate(std::string_view xpath, std::string* error) override;
  bool ValidateCandidate(std::string* error) override;
  bool ApplyCandidate(std::string* error) override;
  bool AbortCandidate(std::string* error) override;
  bool UnlockCandidate(std::string* error) override;
  /** Retrieves a complete live operational XML tree for xpath. */
  std::optional<std::string> GetOperationalData(std::string_view xpath,
                                                std::string* error);
  /** Retrieves config-true XML exactly as accepted in FRR's running store. */
  std::optional<std::string> GetRunningConfiguration(std::string_view xpath,
                                                     std::string* error);
  /** Invokes one modeled FRR RPC and returns its native XML output. */
  std::optional<std::string> InvokeRpc(std::string_view xpath,
                                      std::string_view input_xml,
                                      std::string* error);
  bool Close(std::string* error) override;

  std::uint64_t id() const { return session_id_; }
  bool candidate_locked() const { return candidate_locked_; }

 private:
  Session(std::unique_ptr<Transport> transport, std::uint64_t client_id,
          std::uint64_t session_id)
      : transport_(std::move(transport)),
        client_id_(client_id),
        session_id_(session_id) {}

  std::optional<std::uint64_t> NextRequest(std::string* error);
  bool SetDatastoreLock(Datastore datastore, bool lock,
                        std::string_view operation, std::string* error);
  std::optional<std::string> GetData(Datastore datastore, bool include_state,
                                     bool include_config,
                                     std::string_view xpath,
                                     std::string* error);
  bool CommitCandidate(CommitAction action, std::string* error);

  std::unique_ptr<Transport> transport_;
  std::uint64_t client_id_ = 0;
  std::uint64_t session_id_ = 0;
  std::uint64_t next_request_ = 1;
  bool candidate_locked_ = false;
  bool running_locked_ = false;
};

}  // namespace dang::plugins::frr::mgmtd

#endif  // DANG_PLUGINS_FRR_MGMTD_SESSION_H_

// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/**
 * @file
 * Stateful, correlation-checked wrapper around FRR's local mgmtd messages.
 * The wrapper records candidate-lock ownership and rejects operations in an
 * invalid order before bytes are placed on the transport.
 */

#include "mgmtd_session.h"

#include <limits>
#include <utility>

namespace dang::plugins::frr::mgmtd {
namespace {

bool FixedReply(const Reply& reply, std::string_view operation,
                std::string* error) {
  if (reply.body.size() >= 8) return true;
  if (error) *error = "mgmtd " + std::string(operation) + " reply is truncated";
  return false;
}

bool ExactFixedReply(const Reply& reply, std::string_view operation,
                     std::string* error) {
  if (reply.body.size() == 8) return true;
  if (error)
    *error = "mgmtd " + std::string(operation) +
        " reply has an invalid fixed size";
  return false;
}

bool OptionalTextReply(const Reply& reply, std::string_view operation,
                       std::string* error) {
  if (!FixedReply(reply, operation, error)) return false;
  if (reply.body.size() == 8 || reply.body.back() == std::byte{0}) return true;
  if (error)
    *error = "mgmtd " + std::string(operation) +
        " reply text is not NUL terminated";
  return false;
}

}  // namespace

std::unique_ptr<Session> Session::Open(std::unique_ptr<Transport> transport,
                                       std::uint64_t client_id,
                                       std::string_view client_name,
                                       std::string* error) {
  if (!transport || client_id == 0 || client_name.empty()) {
    if (error) *error = "mgmtd transport, client ID, and client name are required";
    return nullptr;
  }
  const auto request = SessionCreate(client_id, client_name);
  if (request.empty()) {
    if (error) *error = "mgmtd client name is invalid";
    return nullptr;
  }
  auto reply = transport->Exchange(request, Code::kSessionReply, true, error);
  if (!reply || !ExactFixedReply(*reply, "session-create", error)) return nullptr;
  if (reply->header.reference == 0 || reply->body[0] != std::byte{1}) {
    if (error) *error = "mgmtd did not create the requested session";
    return nullptr;
  }
  return std::unique_ptr<Session>(
      new Session(std::move(transport), client_id, reply->header.reference));
}

std::optional<std::uint64_t> Session::NextRequest(std::string* error) {
  if (!transport_ || session_id_ == 0) {
    if (error) *error = "mgmtd session is closed";
    return std::nullopt;
  }
  if (next_request_ == std::numeric_limits<std::uint64_t>::max()) {
    if (error) *error = "mgmtd request identifier space is exhausted";
    return std::nullopt;
  }
  return next_request_++;
}

bool Session::LockCandidate(std::string* error) {
  if (candidate_locked_) {
    if (error) *error = "mgmtd candidate datastore is already locked";
    return false;
  }
  if (!SetDatastoreLock(Datastore::kCandidate, true, "candidate-lock", error))
    return false;
  candidate_locked_ = true;
  if (!SetDatastoreLock(Datastore::kRunning, true, "running-lock", error)) {
    std::string ignored;
    if (SetDatastoreLock(Datastore::kCandidate, false, "candidate-unlock",
                         &ignored))
      candidate_locked_ = false;
    return false;
  }
  running_locked_ = true;
  return true;
}

bool Session::SetDatastoreLock(Datastore datastore, bool lock,
                               std::string_view operation,
                               std::string* error) {
  auto request_id = NextRequest(error);
  if (!request_id) return false;
  auto reply = transport_->Exchange(
      Lock(session_id_, *request_id, datastore, lock), Code::kLockReply, false,
      error);
  if (!reply || !ExactFixedReply(*reply, operation, error)) return false;
  if (reply->body[0] != std::byte{static_cast<std::uint8_t>(datastore)} ||
      reply->body[1] != std::byte{static_cast<std::uint8_t>(lock ? 1 : 0)}) {
    if (error)
      *error = "mgmtd " + std::string(operation) + " reply has invalid state";
    return false;
  }
  return true;
}

bool Session::ReplaceCandidate(std::string_view xpath, std::string_view xml,
                               std::string* error) {
  if (!candidate_locked_) {
    if (error) *error = "mgmtd candidate datastore must be locked before edit";
    return false;
  }
  auto request_id = NextRequest(error);
  if (!request_id) return false;
  auto request = Edit(session_id_, *request_id, Datastore::kCandidate,
                      EditOperation::kReplace, xpath, xml);
  if (request.empty()) {
    if (error) *error = "mgmtd replacement XPath or XML is invalid";
    return false;
  }
  auto reply = transport_->Exchange(request, Code::kEditReply, false, error);
  if (!reply || !FixedReply(*reply, "candidate-edit", error)) return false;
  const std::size_t split = reply->header.split;
  if (split == 0 || split > reply->body.size() - 8 ||
      reply->body[8 + split - 1] != std::byte{0} ||
      (reply->body.size() > 8 + split &&
       reply->body.back() != std::byte{0})) {
    if (error) *error = "mgmtd candidate-edit reply has invalid string data";
    return false;
  }
  return true;
}

bool Session::DeleteCandidate(std::string_view xpath, std::string* error) {
  if (!candidate_locked_) {
    if (error) *error = "mgmtd candidate datastore must be locked before edit";
    return false;
  }
  auto request_id = NextRequest(error);
  if (!request_id) return false;
  auto request = Delete(session_id_, *request_id, Datastore::kCandidate, xpath);
  if (request.empty()) {
    if (error) *error = "mgmtd deletion XPath is invalid";
    return false;
  }
  auto reply = transport_->Exchange(request, Code::kEditReply, false, error);
  if (!reply || !FixedReply(*reply, "candidate-delete", error)) return false;
  const std::size_t split = reply->header.split;
  if (split == 0 || split > reply->body.size() - 8 ||
      reply->body[8 + split - 1] != std::byte{0}) {
    if (error) *error = "mgmtd candidate-delete reply has invalid XPath";
    return false;
  }
  return true;
}

bool Session::CommitCandidate(CommitAction action, std::string* error) {
  if (!candidate_locked_) {
    if (error) *error = "mgmtd candidate datastore must be locked before commit";
    return false;
  }
  auto request_id = NextRequest(error);
  if (!request_id) return false;
  auto reply = transport_->Exchange(
      Commit(session_id_, *request_id, Datastore::kCandidate,
             Datastore::kRunning, action, false),
      Code::kCommitReply, false, error);
  if (!reply || !OptionalTextReply(*reply, "commit", error)) return false;
  // FRR executes abort distinctly but its frontend reply maps every
  // non-validation completion to APPLY. Accept that observed wire behavior
  // while still requiring the exact action for validate and apply.
  const CommitAction reply_action = action == CommitAction::kAbort
      ? CommitAction::kApply
      : action;
  if (reply->body[0] != std::byte{2} || reply->body[1] != std::byte{1} ||
      reply->body[2] != static_cast<std::byte>(reply_action)) {
    if (error)
      *error = "mgmtd commit reply does not match the requested action: " +
          std::to_string(std::to_integer<unsigned>(reply->body[0])) + "/" +
          std::to_string(std::to_integer<unsigned>(reply->body[1])) + "/" +
          std::to_string(std::to_integer<unsigned>(reply->body[2]));
    return false;
  }
  if (reply->body[3] != std::byte{0}) {
    if (error) *error = "mgmtd commit reply unexpectedly requested unlock";
    return false;
  }
  return true;
}

bool Session::ValidateCandidate(std::string* error) {
  return CommitCandidate(CommitAction::kValidate, error);
}

bool Session::ApplyCandidate(std::string* error) {
  return CommitCandidate(CommitAction::kApply, error);
}

bool Session::AbortCandidate(std::string* error) {
  return CommitCandidate(CommitAction::kAbort, error);
}

bool Session::UnlockCandidate(std::string* error) {
  if (!candidate_locked_ || !running_locked_) {
    if (error) *error = "mgmtd transaction datastores are not locked";
    return false;
  }
  if (!SetDatastoreLock(Datastore::kRunning, false, "running-unlock",
                        error))
    return false;
  running_locked_ = false;
  if (!SetDatastoreLock(Datastore::kCandidate, false, "candidate-unlock", error))
    return false;
  candidate_locked_ = false;
  return true;
}

std::optional<std::string> Session::GetOperationalData(std::string_view xpath,
                                                       std::string* error) {
  return GetData(Datastore::kOperational, true, false, xpath, error);
}

std::optional<std::string> Session::GetRunningConfiguration(
    std::string_view xpath, std::string* error) {
  return GetData(Datastore::kRunning, false, true, xpath, error);
}

std::optional<std::string> Session::InvokeRpc(std::string_view xpath,
                                              std::string_view input_xml,
                                              std::string* error) {
  if (candidate_locked_) {
    if (error) *error = "cannot invoke an mgmtd RPC while candidate is locked";
    return std::nullopt;
  }
  auto request = NextRequest(error);
  if (!request) return std::nullopt;
  auto reply = transport_->Exchange(mgmtd::Rpc(session_id_, *request, xpath,
                                                input_xml),
                                    Code::kRpcReply, false, error);
  if (!reply) return std::nullopt;
  return RpcReply({reply->header, reply->body}, error);
}

std::optional<std::string> Session::GetData(Datastore datastore,
                                            bool include_state,
                                            bool include_config,
                                            std::string_view xpath,
                                            std::string* error) {
  if (candidate_locked_) {
    if (error) *error = "cannot retrieve mgmtd data while candidate is locked";
    return std::nullopt;
  }
  auto request = NextRequest(error);
  if (!request) return std::nullopt;
  auto reply = transport_->Exchange(
      mgmtd::GetData(session_id_, *request, datastore, include_state,
                     include_config, xpath),
      Code::kTreeData, false, error);
  if (!reply) return std::nullopt;
  DecodedFrame frame{reply->header, reply->body};
  auto result = TreeData(frame, error);
  if (!result) return std::nullopt;
  // The frontend currently aggregates backend chunks into one final reply. A
  // continuation here cannot be consumed safely by Exchange's request model.
  if (result->more) {
    if (error) *error = "mgmtd returned an unsupported continued tree-data reply";
    return std::nullopt;
  }
  return std::move(result->xml);
}

bool Session::Close(std::string* error) {
  if (!transport_ || session_id_ == 0) return true;
  if (candidate_locked_ || running_locked_) {
    if (error) *error = "mgmtd session cannot close while a datastore is locked";
    return false;
  }
  auto reply = transport_->Exchange(SessionDestroy(session_id_, client_id_),
                                    Code::kSessionReply, false, error);
  if (!reply || !ExactFixedReply(*reply, "session-destroy", error)) return false;
  if (reply->body[0] != std::byte{0}) {
    if (error) *error = "mgmtd did not destroy the requested session";
    return false;
  }
  transport_.reset();
  session_id_ = 0;
  return true;
}

}  // namespace dang::plugins::frr::mgmtd

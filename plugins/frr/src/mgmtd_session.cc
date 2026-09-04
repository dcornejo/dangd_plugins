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
  auto request_id = NextRequest(error);
  if (!request_id) return false;
  auto reply = transport_->Exchange(
      Lock(session_id_, *request_id, Datastore::kCandidate, true),
      Code::kLockReply, false, error);
  if (!reply || !ExactFixedReply(*reply, "candidate-lock", error)) return false;
  if (reply->body[0] != std::byte{2} || reply->body[1] != std::byte{1}) {
    if (error) *error = "mgmtd candidate-lock reply has invalid state";
    return false;
  }
  candidate_locked_ = true;
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
  if (reply->body[0] != std::byte{2} || reply->body[1] != std::byte{1} ||
      reply->body[2] != static_cast<std::byte>(action)) {
    if (error) *error = "mgmtd commit reply does not match the requested action";
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
  if (!candidate_locked_) {
    if (error) *error = "mgmtd candidate datastore is not locked";
    return false;
  }
  auto request_id = NextRequest(error);
  if (!request_id) return false;
  auto reply = transport_->Exchange(
      Lock(session_id_, *request_id, Datastore::kCandidate, false),
      Code::kLockReply, false, error);
  if (!reply || !ExactFixedReply(*reply, "candidate-unlock", error)) return false;
  if (reply->body[0] != std::byte{2} || reply->body[1] != std::byte{0}) {
    if (error) *error = "mgmtd candidate-unlock reply has invalid state";
    return false;
  }
  candidate_locked_ = false;
  return true;
}

bool Session::Close(std::string* error) {
  if (!transport_ || session_id_ == 0) return true;
  if (candidate_locked_) {
    if (error) *error = "mgmtd session cannot close while candidate is locked";
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

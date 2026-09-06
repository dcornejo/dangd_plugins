// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/** @file FD.io generated C++ VAPI implementation of the narrow VPP client. */

#include "plugins/vpp/src/vapi_vpp_client.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>

#include <vapi/interface.api.vapi.hpp>
DEFINE_VAPI_MSG_IDS_VPE_API_JSON
DEFINE_VAPI_MSG_IDS_INTERFACE_API_JSON

namespace dang::vpp {
namespace {

std::string ApiError(const char* operation, vapi_error_e result) {
  return std::string(operation) + " failed with VAPI status " +
         std::to_string(static_cast<int>(result));
}

bool RequireMessage(vapi::Connection* connection, vapi_msg_id_t message,
                    const char* operation, std::string* error) {
  if (connection->is_msg_available(message)) return true;
  *error = std::string("the connected VPP does not advertise ") + operation;
  return false;
}

template <typename Request>
bool Execute(vapi::Connection* connection, Request* request,
             const char* operation, std::string* error) {
  vapi_error_e result = request->execute();
  if (result != VAPI_OK) {
    *error = ApiError(operation, result);
    return false;
  }
  // execute() confirms that the request was queued. Even in VAPI's blocking
  // mode the response must then be dispatched before get_response() is valid.
  // Use the explicit one-second VAPI receive bound rather than its convenience
  // wrapper so an unresponsive daemon cannot hold a plugin worker indefinitely.
  result = connection->dispatch(request, 1);
  if (result == VAPI_OK) return true;
  *error = ApiError(operation, result);
  return false;
}

}  // namespace

class VapiVppClient::Impl {
 public:
  vapi::Connection connection;
};

VapiVppClient::VapiVppClient(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

VapiVppClient::~VapiVppClient() {
  if (impl_) (void)impl_->connection.disconnect();
}

std::unique_ptr<VapiVppClient> VapiVppClient::Connect(
    const std::string& socket_path, std::string* error) {
  if (!error) return nullptr;
  error->clear();
  if (socket_path.empty()) {
    *error = "the VPP binary API socket path is empty";
    return nullptr;
  }
  auto impl = std::make_unique<Impl>();
  const vapi_error_e result =
      impl->connection.connect("dangd-vpp", socket_path.c_str(), 16, 32,
                               true, true);
  if (result != VAPI_OK) {
    *error = ApiError("VPP connection", result);
    return nullptr;
  }
  return std::unique_ptr<VapiVppClient>(
      new VapiVppClient(std::move(impl)));
}

bool VapiVppClient::CreateLoopback(uint32_t instance,
                                    CreatedInterface* created,
                                    std::string* error) {
  if (!created || !error) return false;
  error->clear();
  if (!RequireMessage(&impl_->connection, vapi_msg_id_create_loopback_instance,
                      "create_loopback_instance", error))
    return false;
  vapi::Create_loopback_instance request(impl_->connection);
  auto& payload = request.get_request().get_payload();
  std::ranges::fill(payload.mac_address, 0);
  payload.is_specified = true;
  payload.user_instance = instance;
  if (!Execute(&impl_->connection, &request, "create_loopback_instance", error))
    return false;
  const auto& reply = request.get_response().get_payload();
  if (reply.retval != 0) {
    *error = "create_loopback_instance was rejected by VPP with retval " +
             std::to_string(reply.retval);
    return false;
  }
  created->software_index = reply.sw_if_index;
  // The software index is the authoritative handle. Resolving VPP's cosmetic
  // interface name requires a separate dump and is intentionally not part of
  // the mutation's success boundary.
  created->name = "loop" + std::to_string(instance);
  return true;
}

bool VapiVppClient::SetAdminState(uint32_t software_index, bool up,
                                   std::string* error) {
  if (!error) return false;
  error->clear();
  if (!RequireMessage(&impl_->connection, vapi_msg_id_sw_interface_set_flags,
                      "sw_interface_set_flags", error))
    return false;
  vapi::Sw_interface_set_flags request(impl_->connection);
  auto& payload = request.get_request().get_payload();
  payload.sw_if_index = software_index;
  payload.flags = up ? IF_STATUS_API_FLAG_ADMIN_UP
                     : static_cast<vapi_enum_if_status_flags>(0);
  if (!Execute(&impl_->connection, &request, "sw_interface_set_flags", error))
    return false;
  const auto& reply = request.get_response().get_payload();
  if (reply.retval == 0) return true;
  *error = "sw_interface_set_flags was rejected by VPP with retval " +
           std::to_string(reply.retval);
  return false;
}

bool VapiVppClient::DeleteLoopback(uint32_t software_index,
                                    std::string* error) {
  if (!error) return false;
  error->clear();
  if (!RequireMessage(&impl_->connection, vapi_msg_id_delete_loopback,
                      "delete_loopback", error))
    return false;
  vapi::Delete_loopback request(impl_->connection);
  request.get_request().get_payload().sw_if_index = software_index;
  if (!Execute(&impl_->connection, &request, "delete_loopback", error))
    return false;
  const auto& reply = request.get_response().get_payload();
  if (reply.retval == 0) return true;
  *error = "delete_loopback was rejected by VPP with retval " +
           std::to_string(reply.retval);
  return false;
}

}  // namespace dang::vpp

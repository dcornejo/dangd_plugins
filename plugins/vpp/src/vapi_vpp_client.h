// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANG_PLUGINS_VPP_VAPI_VPP_CLIENT_H_
#define DANG_PLUGINS_VPP_VAPI_VPP_CLIENT_H_

#include <memory>
#include <string>

#include "plugins/vpp/src/vpp_client.h"

namespace dang::vpp {

/** Production VPP client backed by FD.io's generated C++ VAPI bindings. */
class VapiVppClient final : public VppClient {
 public:
  /** Connects to VPP's binary API Unix socket. */
  [[nodiscard]] static std::unique_ptr<VapiVppClient> Connect(
      const std::string& socket_path, std::string* error);
  ~VapiVppClient() override;

  VapiVppClient(const VapiVppClient&) = delete;
  VapiVppClient& operator=(const VapiVppClient&) = delete;

  [[nodiscard]] bool CreateLoopback(uint32_t instance,
                                     CreatedInterface* created,
                                     std::string* error) override;
  [[nodiscard]] bool SetAdminState(uint32_t software_index, bool up,
                                    std::string* error) override;
  [[nodiscard]] bool DeleteLoopback(uint32_t software_index,
                                     std::string* error) override;

 private:
  class Impl;
  explicit VapiVppClient(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace dang::vpp

#endif

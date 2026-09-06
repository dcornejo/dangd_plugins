// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANG_PLUGINS_VPP_VPP_CLIENT_H_
#define DANG_PLUGINS_VPP_VPP_CLIENT_H_

#include <cstdint>
#include <string>
#include <vector>

namespace dang::vpp {

/** Stable result returned by VPP after creating a software loopback. */
struct CreatedInterface {
  uint32_t software_index = 0;
  std::string name;
  bool admin_up = false;
};

/**
 * Narrow programmatic seam around the VPP binary API.
 *
 * The production implementation will use VAPI generated headers. Keeping the
 * transaction logic behind this interface lets its compensation rules be
 * tested without a daemon and prevents command-line fallback from becoming an
 * accidental production dependency.
 */
class VppClient {
 public:
  virtual ~VppClient() = default;
  [[nodiscard]] virtual bool CreateLoopback(uint32_t instance,
                                             CreatedInterface* created,
                                             std::string* error) = 0;
  /** Resolves stable loopN identity to its current VPP software index. */
  [[nodiscard]] virtual bool FindLoopback(uint32_t instance,
                                           CreatedInterface* found,
                                           std::string* error) = 0;
  /** Returns every VPP loopback with live index and administrative state. */
  [[nodiscard]] virtual bool ListLoopbacks(
      std::vector<CreatedInterface>* loopbacks, std::string* error) = 0;
  [[nodiscard]] virtual bool SetAdminState(uint32_t software_index, bool up,
                                            std::string* error) = 0;
  [[nodiscard]] virtual bool DeleteLoopback(uint32_t software_index,
                                             std::string* error) = 0;
};

}  // namespace dang::vpp

#endif

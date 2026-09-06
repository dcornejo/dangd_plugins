// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANG_PLUGINS_VPP_OWNERSHIP_POLICY_H_
#define DANG_PLUGINS_VPP_OWNERSHIP_POLICY_H_

#include <string>
#include <vector>

#include "plugins/vpp/src/ownership_inventory.h"

namespace dang::vpp {

struct DevicePolicy {
  std::string pci_address;
  std::string expected_mac_address;
  std::string expected_vendor_device;
  bool vpp_owner = false;
};

/** Parses the ownership subtree from a complete datastore snapshot. */
[[nodiscard]] bool ParseOwnershipPolicy(const char* xml,
                                        std::vector<DevicePolicy>* policy,
                                        std::string* error,
                                        std::string* error_path);

/** Validates every VPP claim against fresh, read-only host evidence. */
[[nodiscard]] bool ValidateOwnershipPolicy(
    const std::vector<DevicePolicy>& policy,
    const std::vector<InterfaceEvidence>& inventory, std::string* error,
    std::string* error_path);

}  // namespace dang::vpp

#endif

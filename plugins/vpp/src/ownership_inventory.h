// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANG_PLUGINS_VPP_OWNERSHIP_INVENTORY_H_
#define DANG_PLUGINS_VPP_OWNERSHIP_INVENTORY_H_

#include <string>
#include <vector>

namespace dang::vpp {

/** Read-only evidence used before a physical interface may be allowlisted. */
struct InterfaceEvidence {
  std::string name;
  std::string pci_address;
  std::string mac_address;
  std::string driver;
  bool carries_default_route = false;
  bool carries_management_session = false;
  [[nodiscard]] bool eligible() const {
    return !pci_address.empty() && !carries_default_route &&
           !carries_management_session;
  }
};

/** Inventories Linux interfaces without changing link, driver, or route state. */
[[nodiscard]] bool DiscoverInterfaces(std::vector<InterfaceEvidence>* interfaces,
                                      std::string* error);

}  // namespace dang::vpp

#endif

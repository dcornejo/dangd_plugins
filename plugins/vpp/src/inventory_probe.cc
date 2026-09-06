// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/** @file Human-readable read-only VPP ownership eligibility probe. */

#include "plugins/vpp/src/ownership_inventory.h"

#include <iostream>

int main() {
  std::vector<dang::vpp::InterfaceEvidence> interfaces;
  std::string error;
  if (!dang::vpp::DiscoverInterfaces(&interfaces, &error)) {
    std::cerr << error << '\n';
    return 1;
  }
  for (const auto& item : interfaces)
    std::cout << item.name << " pci="
              << (item.pci_address.empty() ? "none" : item.pci_address)
              << " driver=" << (item.driver.empty() ? "none" : item.driver)
              << " mac=" << item.mac_address
              << " default-route=" << (item.carries_default_route ? "yes" : "no")
              << " management-session="
              << (item.carries_management_session ? "yes" : "no")
              << " eligible=" << (item.eligible() ? "yes" : "no") << '\n';
}

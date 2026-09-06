// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "plugins/vpp/src/ownership_policy.h"

#include <gtest/gtest.h>

namespace dang::vpp {
namespace {
constexpr char kPolicy[] = R"(<config><vpp-interface-ownership xmlns="urn:dang:vpp:interface-ownership"><device><pci-address>0000:06:13.0</pci-address><expected-mac-address>bc:24:11:bb:cf:95</expected-mac-address><expected-vendor-device>1af4:1000</expected-vendor-device><owner>vpp</owner></device></vpp-interface-ownership></config>)";

TEST(VppOwnershipPolicy, AllowsExactNonManagementIdentity) {
  std::vector<DevicePolicy> policy;
  std::string error, path;
  ASSERT_TRUE(ParseOwnershipPolicy(kPolicy, &policy, &error, &path)) << error;
  InterfaceEvidence evidence{.name = "ens19", .pci_address = "0000:06:13.0",
      .mac_address = "bc:24:11:bb:cf:95", .vendor_device = "1af4:1000",
      .driver = "virtio_net", .carries_default_route = false,
      .carries_management_session = false};
  EXPECT_TRUE(ValidateOwnershipPolicy(policy, {evidence}, &error, &path)) << error;
}

TEST(VppOwnershipPolicy, ManagementEvidenceOverridesExactAllowlist) {
  std::vector<DevicePolicy> policy;
  std::string error, path;
  ASSERT_TRUE(ParseOwnershipPolicy(kPolicy, &policy, &error, &path));
  InterfaceEvidence evidence{.name = "ens19", .pci_address = "0000:06:13.0",
      .mac_address = "bc:24:11:bb:cf:95", .vendor_device = "1af4:1000",
      .driver = "virtio_net", .carries_default_route = true,
      .carries_management_session = false};
  EXPECT_FALSE(ValidateOwnershipPolicy(policy, {evidence}, &error, &path));
  EXPECT_NE(error.find("management-path"), std::string::npos);
  EXPECT_NE(path.find("0000:06:13.0"), std::string::npos);
}

TEST(VppOwnershipPolicy, RejectsIdentityDrift) {
  std::vector<DevicePolicy> policy;
  std::string error, path;
  ASSERT_TRUE(ParseOwnershipPolicy(kPolicy, &policy, &error, &path));
  InterfaceEvidence evidence{.name = "ens19", .pci_address = "0000:06:13.0",
      .mac_address = "00:00:00:00:00:01", .vendor_device = "1af4:1000",
      .driver = "virtio_net", .carries_default_route = false,
      .carries_management_session = false};
  EXPECT_FALSE(ValidateOwnershipPolicy(policy, {evidence}, &error, &path));
  EXPECT_NE(error.find("identity"), std::string::npos);
}

TEST(VppOwnershipPolicy, RejectsUnknownPciFunction) {
  std::vector<DevicePolicy> policy;
  std::string error, path;
  ASSERT_TRUE(ParseOwnershipPolicy(kPolicy, &policy, &error, &path));
  EXPECT_FALSE(ValidateOwnershipPolicy(policy, {}, &error, &path));
  EXPECT_NE(error.find("not a Linux network interface"), std::string::npos);
}

TEST(VppOwnershipPolicy, HostOwnershipDoesNotRequestTransfer) {
  constexpr char kHostPolicy[] = R"(<vpp-interface-ownership xmlns="urn:dang:vpp:interface-ownership"><device><pci-address>0000:06:13.0</pci-address><expected-mac-address>bc:24:11:bb:cf:95</expected-mac-address><expected-vendor-device>1af4:1000</expected-vendor-device><owner>host</owner></device></vpp-interface-ownership>)";
  std::vector<DevicePolicy> policy;
  std::string error, path;
  ASSERT_TRUE(ParseOwnershipPolicy(kHostPolicy, &policy, &error, &path));
  EXPECT_TRUE(ValidateOwnershipPolicy(policy, {}, &error, &path));
}
}  // namespace
}  // namespace dang::vpp

// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANGD_PLUGINS_IP_MANAGEMENT_PLATFORM_BACKEND_H_
#define DANGD_PLUGINS_IP_MANAGEMENT_PLATFORM_BACKEND_H_

#include <memory>
#include <string>
#include <string_view>

namespace dangd::ip_management {

// PlatformBackend is the narrow boundary between the RFC 8343/8344 plugin and
// host networking. Implementations must either apply the complete transition
// or return false after making a best effort to restore `before_xml`.
class PlatformBackend {
 public:
  virtual ~PlatformBackend() = default;
  virtual bool Reconcile(std::string_view before_xml,
                         std::string_view desired_xml,
                         std::string* error) = 0;

  // Forget any live platform snapshot retained for transaction rollback. The
  // plugin calls this only after dangd has accepted the complete transaction;
  // until then Reconcile(desired, before) must restore observed pre-apply
  // values that cannot be reconstructed from configuration alone.
  virtual void Commit() {}

  // Return a coherent RFC 8343/8344 legacy operational-state fragment for
  // interfaces owned by configuration_xml. An empty successful result asks
  // the teaching plugin to use its portable simulated fallback.
  virtual bool OperationalXml(std::string_view,
                              std::string* output, std::string*) {
    output->clear();
    return true;
  }
};

// The build selects exactly one implementation. Linux and FreeBSD production
// builds return a native backend; other hosts return a logging-only backend.
std::unique_ptr<PlatformBackend> MakePlatformBackend();

}  // namespace dangd::ip_management

#endif  // DANGD_PLUGINS_IP_MANAGEMENT_PLATFORM_BACKEND_H_

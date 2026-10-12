// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/** @file Linux chrony selection and daemon-specific validation boundary. */

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <utility>

#include "plugins/ntp/src/chrony_action_adapter.h"
#include "plugins/ntp/src/chrony_config.h"
#include "plugins/ntp/src/chrony_preflight.h"
#include "plugins/ntp/src/linux/chrony_service.h"
#include "plugins/ntp/src/ntp_adapter.h"

namespace dang::ntp {
namespace {
/** Dedicated live-test wrapper preflight runs before service capture/stop. */
class WrapperService final : public ChronyService {
 public:
  explicit WrapperService(std::string root)
      : root_(std::move(root)), service_("dangd-ntp-live-test.service") {}
  bool Active(bool* active, std::string* error) override {
    std::ifstream file(root_ + "/main.conf");
    if (!file) {
      if (error) *error = "cannot read dedicated chrony wrapper";
      return false;
    }
    const std::string text{std::istreambuf_iterator<char>(file),
                           std::istreambuf_iterator<char>()};
    if (!ValidateChronyWrapper(text, root_ + "/managed.conf", true, error))
      return false;
    return service_.Active(active, error);
  }
  bool SetActive(bool active, std::string* error) override {
    return service_.SetActive(active, error);
  }

 private:
  std::string root_;
  SystemdChronyService service_;
};

/**
 * Adds exact chrony semantic validation while native mutation remains closed.
 *
 * Composition keeps checkpoint and failure behavior in the common adapter;
 * this platform layer owns only knowledge that is specific to chronyd.
 */
class ChronyAdapter final : public NativeAdapter {
 public:
  explicit ChronyAdapter(AdapterDescriptor descriptor)
      : fallback_(CreateFailClosedAdapter(std::move(descriptor))) {}

  const AdapterDescriptor& descriptor() const override {
    return fallback_->descriptor();
  }

  bool ValidateConfig(const Config& proposed, std::string* error,
                      std::string* error_path) override {
    ChronyConfigImage image;
    return RenderChronyConfig(proposed, "/etc/chrony/dangd-rfc9249.keys",
                              &image, error, error_path);
  }

  bool ApplyAction(const Config& before, const Config& proposed,
                   const PlannedAction& action, AdapterCheckpoint* checkpoint,
                   std::string* error, std::string* error_path) override {
    return fallback_->ApplyAction(before, proposed, action, checkpoint, error,
                                  error_path);
  }

  bool RollbackAction(const Config& before, const Config& proposed,
                      const PlannedAction& action,
                      AdapterCheckpoint* checkpoint, std::string* error,
                      std::string* error_path) override {
    return fallback_->RollbackAction(before, proposed, action, checkpoint,
                                     error, error_path);
  }

  bool OperationalXml(std::string* xml, std::string* error,
                      std::string* error_path) override {
    return fallback_->OperationalXml(xml, error, error_path);
  }

 private:
  std::unique_ptr<NativeAdapter> fallback_;
};

}  // namespace

Platform NativePlatform() { return Platform::kLinux; }

std::unique_ptr<NativeAdapter> CreatePlatformAdapter(
    AdapterDescriptor descriptor) {
  // Development ABI integration is restricted to explicit disposable files
  // and test units. Production ownership/include preflight is still pending.
  const char* root = std::getenv("DANG_NTP_NATIVE_TEST_ROOT");
  const char* unit = std::getenv("DANG_NTP_NATIVE_TEST_UNIT");
  if (root && unit && std::string_view(root) == "/tmp/dang-ntp-live-test" &&
      std::string_view(unit) == "dangd-ntp-live-test.service")
    return CreateChronyActionAdapter(std::move(descriptor),
                                     std::make_unique<WrapperService>(root),
                                     std::string(root) + "/managed.conf",
                                     std::string(root) + "/managed.keys");
  if (root && unit && std::string_view(root).starts_with("/tmp/dang-ntp-") &&
      std::string_view(root).find("..") == std::string_view::npos &&
      (std::string_view(unit) == "dangd-ntp-native-test.service" ||
       std::string_view(unit) == "dangd-ntp-native-fail-test.service"))
    return CreateChronyActionAdapter(
        std::move(descriptor), std::make_unique<SystemdChronyService>(unit),
        std::string(root) + "/managed.conf",
        std::string(root) + "/managed.keys");
  return std::make_unique<ChronyAdapter>(std::move(descriptor));
}

}  // namespace dang::ntp

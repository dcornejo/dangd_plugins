// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/** @file Linux chrony selection and daemon-specific validation boundary. */

#include <memory>
#include <utility>

#include "plugins/ntp/src/chrony_config.h"
#include "plugins/ntp/src/ntp_adapter.h"

namespace dang::ntp {
namespace {

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
  return std::make_unique<ChronyAdapter>(std::move(descriptor));
}

}  // namespace dang::ntp

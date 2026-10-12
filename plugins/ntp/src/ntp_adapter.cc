// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/** @file Fail-closed RFC 9249 daemon selection and native adapter boundary. */

#include "plugins/ntp/src/ntp_adapter.h"

#include <utility>

namespace dang::ntp {
namespace {

bool Fail(std::string text, std::string path, std::string* error,
          std::string* error_path) {
  if (error) *error = std::move(text);
  if (error_path) *error_path = std::move(path);
  return false;
}

AdapterDescriptor Descriptor(Platform platform, Daemon daemon) {
  if (platform == Platform::kLinux && daemon == Daemon::kChrony)
    return {.platform = platform,
            .daemon = daemon,
            .platform_name = "Linux",
            .daemon_name = "chrony"};
  if (platform == Platform::kFreeBsd && daemon == Daemon::kNtpd)
    return {.platform = platform,
            .daemon = daemon,
            .platform_name = "FreeBSD",
            .daemon_name = "ntpd"};
  return {.platform = platform,
          .daemon = Daemon::kUnavailable,
          .platform_name = "unsupported",
          .daemon_name = "unavailable"};
}

class FailClosedAdapter final : public NativeAdapter {
 public:
  explicit FailClosedAdapter(AdapterDescriptor descriptor)
      : descriptor_(std::move(descriptor)) {}

  const AdapterDescriptor& descriptor() const override { return descriptor_; }

  bool ApplyAction(const Config&, const Config&, const PlannedAction& action,
                   AdapterCheckpoint* checkpoint, std::string* error,
                   std::string* error_path) override {
    if (!checkpoint)
      return Fail("missing RFC 9249 rollback checkpoint", ActionPath(action.id),
                  error, error_path);
    checkpoint->action_id = action.id;
    checkpoint->before_image.clear();
    checkpoint->applied = false;
    return Fail("RFC 9249 " + descriptor_.daemon_name +
                    " native apply is not implemented",
                ActionPath(action.id), error, error_path);
  }

  bool RollbackAction(const Config&, const Config&, const PlannedAction& action,
                      AdapterCheckpoint* checkpoint, std::string* error,
                      std::string* error_path) override {
    if (!checkpoint || !checkpoint->applied ||
        checkpoint->action_id != action.id)
      return Fail("RFC 9249 action has no acknowledged native before-image",
                  ActionPath(action.id), error, error_path);
    return Fail("RFC 9249 " + descriptor_.daemon_name +
                    " native rollback is not implemented",
                ActionPath(action.id), error, error_path);
  }

  bool OperationalXml(std::string* xml, std::string* error,
                      std::string* error_path) override {
    if (xml) xml->clear();
    return Fail("RFC 9249 " + descriptor_.daemon_name +
                    " operational readback is not implemented",
                "/ietf-ntp:ntp/clock-state", error, error_path);
  }

 private:
  AdapterDescriptor descriptor_;
};

}  // namespace

bool SelectAdapter(Platform platform, std::string_view requested,
                   AdapterDescriptor* descriptor, std::string* error,
                   std::string* error_path) {
  if (!descriptor)
    return Fail("missing RFC 9249 adapter selection output", "/ietf-ntp:ntp",
                error, error_path);
  if (requested.empty()) requested = "auto";

  if (platform == Platform::kLinux) {
    if (requested == "auto" || requested == "chrony") {
      *descriptor = Descriptor(platform, Daemon::kChrony);
      return true;
    }
    return Fail("RFC 9249 daemon '" + std::string(requested) +
                    "' is not supported on Linux; choose auto or chrony",
                "/ietf-ntp:ntp", error, error_path);
  }
  if (platform == Platform::kFreeBsd) {
    if (requested == "auto" || requested == "ntpd") {
      *descriptor = Descriptor(platform, Daemon::kNtpd);
      return true;
    }
    return Fail("RFC 9249 daemon '" + std::string(requested) +
                    "' is not supported on FreeBSD; choose auto or ntpd",
                "/ietf-ntp:ntp", error, error_path);
  }
  if (requested != "auto" && requested != "none")
    return Fail("RFC 9249 daemon '" + std::string(requested) +
                    "' is not supported on this platform",
                "/ietf-ntp:ntp", error, error_path);
  *descriptor = Descriptor(platform, Daemon::kUnavailable);
  return true;
}

std::unique_ptr<NativeAdapter> CreateNativeAdapter(std::string_view requested,
                                                   std::string* error,
                                                   std::string* error_path) {
  AdapterDescriptor descriptor;
  if (!SelectAdapter(NativePlatform(), requested, &descriptor, error,
                     error_path))
    return nullptr;
  return std::make_unique<FailClosedAdapter>(std::move(descriptor));
}

const char* ActionPath(std::string_view id) {
  if (id == "configure-authentication") return "/ietf-ntp:ntp/authentication";
  if (id == "configure-access-rules") return "/ietf-ntp:ntp/access-rules";
  if (id == "configure-listener-port") return "/ietf-ntp:ntp/port";
  if (id == "configure-reference-clock") return "/ietf-ntp:ntp/refclock-master";
  if (id.find("unicast") != std::string_view::npos)
    return "/ietf-ntp:ntp/unicast-configuration";
  return "/ietf-ntp:ntp";
}

}  // namespace dang::ntp

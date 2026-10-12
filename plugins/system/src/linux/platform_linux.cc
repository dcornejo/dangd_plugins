// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0
/** @file Linux file layout and shared native systemd integration. */
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "plugins/common/systemd_unit.h"
#include "plugins/system/src/platform.h"
namespace dang::system {
PlatformLayout NativePlatformLayout() {
  return {.ntp_configuration = "/etc/chrony/conf.d/dangd.conf",
          .hostname_configuration = "/etc/hostname"};
}
bool NativeNtpServiceOperation(bool enabled, std::string* error) {
  if (!error) return false;
  return native::ChangeSystemdUnit("chrony.service",
                                   enabled
                                       ? native::UnitOperation::kReloadOrRestart
                                       : native::UnitOperation::kStop,
                                   error);
}
bool NativePowerOperation(bool restart, std::string* error) {
  // systemd SIGRTMIN+4/+5 requests orderly power-off/reboot through PID 1,
  // allowing it to stop services and synchronize/unmount filesystems first.
  const int signal = SIGRTMIN + (restart ? 5 : 4);
  if (::kill(1, signal) == 0) return true;
  if (error)
    *error = "Linux systemd power request failed: " +
             std::string(std::strerror(errno));
  return false;
}
}  // namespace dang::system

// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/** @file FreeBSD file layout and native service-manager integration. */

#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include "plugins/system/src/platform.h"

namespace dang::system {

PlatformLayout NativePlatformLayout() {
  return {.ntp_configuration = "/etc/ntp.conf",
          .hostname_configuration = "/etc/rc.conf.d/dangd-hostname"};
}

bool NativeNtpServiceOperation(bool enabled, std::string* error) {
  // FreeBSD's supported ntpd lifecycle remains the audited rc.d boundary.
  // The command is fixed host policy and contains no modeled input. A later
  // audit increment will replace the shell or document the narrow argv-only
  // exception after checking the service-management interfaces available in
  // supported FreeBSD releases.
  const char* command = enabled ? "service ntpd onerestart"
                                : "service ntpd onestop";
  if (std::system(command) == 0) return true;
  if (error) *error = "FreeBSD ntpd service command failed";
  return false;
}

bool NativePowerOperation(bool restart, std::string* error) {
  // FreeBSD init documents SIGINT as an orderly reboot request and SIGUSR2 as
  // an orderly power-off request. PID 1 runs rc.shutdown before the final
  // kernel operation, unlike a direct reboot(2) call from the plugin.
  const int signal = restart ? SIGINT : SIGUSR2;
  if (::kill(1, signal) == 0) return true;
  if (error)
    *error = "FreeBSD init power request failed: " +
             std::string(std::strerror(errno));
  return false;
}

}  // namespace dang::system

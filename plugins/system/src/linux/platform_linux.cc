// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/** @file Linux file layout and native service-manager integration. */

#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "plugins/system/src/platform.h"

namespace dang::system {

PlatformLayout NativePlatformLayout() {
  return {
      .ntp_configuration = "/etc/chrony/conf.d/dangd.conf",
      .hostname_configuration = "/etc/hostname",
      .ntp_reload_command = "systemctl try-reload-or-restart chrony.service",
      .ntp_stop_command = "systemctl stop chrony.service"};
}

bool NativePowerOperation(bool restart, std::string* error) {
  // systemd documents SIGRTMIN+4 as an orderly power-off request and
  // SIGRTMIN+5 as an orderly reboot request. Unlike reboot(2), these requests
  // let PID 1 stop services and unmount or synchronize filesystems first.
  const int signal = SIGRTMIN + (restart ? 5 : 4);
  if (::kill(1, signal) == 0) return true;
  if (error)
    *error = "Linux systemd power request failed: " +
             std::string(std::strerror(errno));
  return false;
}

}  // namespace dang::system

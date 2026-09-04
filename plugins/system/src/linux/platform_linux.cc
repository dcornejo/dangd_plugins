// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/** @file Linux file layout and service commands for RFC 7317 integration. */

#include "plugins/system/src/platform.h"

namespace dang::system {

PlatformLayout NativePlatformLayout() {
  return {.ntp_configuration = "/etc/chrony/conf.d/dangd.conf",
          .hostname_configuration = "/etc/hostname",
          .ntp_reload_command = "systemctl try-reload-or-restart chrony.service",
          .ntp_stop_command = "systemctl stop chrony.service",
          .restart_command = "systemctl reboot",
          .shutdown_command = "systemctl poweroff"};
}

}  // namespace dang::system

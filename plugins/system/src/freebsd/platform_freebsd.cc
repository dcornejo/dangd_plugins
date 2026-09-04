// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/** @file FreeBSD file layout and service commands for RFC 7317 integration. */

#include "plugins/system/src/platform.h"

namespace dang::system {

PlatformLayout NativePlatformLayout() {
  return {.ntp_configuration = "/etc/ntp.conf",
          .hostname_configuration = "/etc/rc.conf.d/dangd-hostname",
          .ntp_reload_command = "service ntpd onerestart",
          .ntp_stop_command = "service ntpd onestop",
          .restart_command = "/sbin/shutdown -r now",
          .shutdown_command = "/sbin/shutdown -p now"};
}

}  // namespace dang::system

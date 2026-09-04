#!/bin/sh
# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

set -eu
binary=$1
jail_name="dang_rib_$$"
epair=$(sudo ifconfig epair create)
peer="${epair%a}b"
cleanup() {
  sudo jail -r "$jail_name" >/dev/null 2>&1 || true
  sudo ifconfig "$epair" destroy >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

sudo ifconfig "$epair" inet 192.0.2.1/24 up
sudo jail -c name="$jail_name" path=/ host.hostname="$jail_name" \
  persist vnet vnet.interface="$peer"
sudo jexec "$jail_name" ifconfig lo0 up
sudo jexec "$jail_name" ifconfig "$peer" inet 192.0.2.2/24 up
sudo jexec "$jail_name" "$binary" \
  freebsd install 0 198.18.0.0/24 "$peer" 192.0.2.1
sudo jexec "$jail_name" netstat -rn -f inet |
  grep -F "198.18.0.0/24"
sudo jexec "$jail_name" "$binary" \
  freebsd delete 0 198.18.0.0/24 "$peer" 192.0.2.1
if sudo jexec "$jail_name" netstat -rn -f inet |
    grep -F "198.18.0.0/24"; then
  exit 1
fi

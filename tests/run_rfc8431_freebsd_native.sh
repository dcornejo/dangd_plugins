#!/bin/sh
# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

set -eu
binary=$(realpath "$1")
plugin_test=${2:+$(realpath "$2")}
plugin=${3:+$(realpath "$3")}
jail_name="dang_rib_$$"
registry="/tmp/dang-rib-registry-$$.json"
error_output="/tmp/dang-rib-error-$$.txt"
epair=$(sudo ifconfig epair create)
peer="${epair%a}b"
cleanup() {
  sudo jail -r "$jail_name" >/dev/null 2>&1 || true
  sudo ifconfig "$epair" destroy >/dev/null 2>&1 || true
  sudo rm -f "$registry" "$error_output"
}
trap cleanup EXIT INT TERM

sudo ifconfig "$epair" inet 192.0.2.1/24 up
sudo ifconfig "$epair" inet6 2001:db8:8431::1/64
sudo jail -c name="$jail_name" path=/ host.hostname="$jail_name" \
  persist vnet vnet.interface="$peer"
sudo jexec "$jail_name" ifconfig lo0 up
sudo jexec "$jail_name" ifconfig "$peer" inet 192.0.2.2/24 up
sudo jexec "$jail_name" ifconfig "$peer" inet6 2001:db8:8431::2/64
if [ -n "$plugin_test" ] && [ -n "$plugin" ]; then
  sudo jexec "$jail_name" env DANG_RIB_REGISTRY_FILE="$registry" \
    "$plugin_test" "$plugin" \
    0 198.18.1.0/24 "$peer" 192.0.2.1
  if sudo jexec "$jail_name" netstat -rn -f inet | grep -F "198.18.1.0/24"; then
    exit 1
  fi
fi
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

# Exercise both address families through route netlink rather than relying on
# the IPv4-only transaction above as evidence for the shared encoder.
sudo jexec "$jail_name" "$binary" freebsd install 0 \
  2001:db8:8432::/64 "$peer" 2001:db8:8431::1
sudo jexec "$jail_name" netstat -rn -f inet6 | grep -F "2001:db8:8432::/64"
sudo jexec "$jail_name" "$binary" freebsd delete 0 \
  2001:db8:8432::/64 "$peer" 2001:db8:8431::1
if sudo jexec "$jail_name" netstat -rn -f inet6 |
    grep -F "2001:db8:8432::/64"; then
  exit 1
fi

# A direct RTA_OIF route must work on an unnumbered interface. The retired
# route(8) adapter could express this only by guessing a local gateway address.
sudo jexec "$jail_name" ifconfig "$peer" inet 192.0.2.2 delete
sudo jexec "$jail_name" ifconfig "$peer" inet6 2001:db8:8431::2 delete
sudo jexec "$jail_name" "$binary" \
  freebsd install 0 198.18.2.0/24 "$peer"
sudo jexec "$jail_name" netstat -rn -f inet | grep -F "198.18.2.0/24"
sudo jexec "$jail_name" "$binary" \
  freebsd delete 0 198.18.2.0/24 "$peer"

# Preflight failures must remain path-bearing and must not reach the kernel.
if sudo jexec "$jail_name" "$binary" freebsd install 0 \
    198.18.3.0/24 dang-no-such-interface >"$error_output" 2>&1; then
  echo "missing FreeBSD interface unexpectedly accepted" >&2
  exit 1
fi
grep -F "FreeBSD route outgoing interface does not exist" "$error_output"

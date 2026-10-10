#!/bin/sh
# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

set -eu
binary=$(realpath "$1")
plugin_test=${2:+$(realpath "$2")}
plugin=${3:+$(realpath "$3")}
load_balance_plugin_test=${4:+$(realpath "$4")}
netconf_test=${5:+$(realpath "$5")}
dangd=${6:+$(realpath "$6")}
plugin_worker=${7:+$(realpath "$7")}
interface_plugin=${8:+$(realpath "$8")}
jail_name="dang_rib_$$"
registry="/tmp/dang-rib-registry-$$.json"
load_balance_registry="/tmp/dang-rib-lb-registry-$$.json"
netconf_registry="/tmp/dang-rib-netconf-registry-$$.json"
error_output="/tmp/dang-rib-error-$$.txt"
epair=$(sudo ifconfig epair create)
peer="${epair%a}b"
epair2=$(sudo ifconfig epair create)
peer2="${epair2%a}b"
cleanup() {
  sudo jail -r "$jail_name" >/dev/null 2>&1 || true
  sudo ifconfig "$epair" destroy >/dev/null 2>&1 || true
  sudo ifconfig "$epair2" destroy >/dev/null 2>&1 || true
  sudo rm -f "$registry" "$load_balance_registry" "$netconf_registry" \
    "$error_output"
}
trap cleanup EXIT INT TERM

sudo ifconfig "$epair" inet 192.0.2.1/24 up
sudo ifconfig "$epair" inet6 2001:db8:8431::1/64
sudo ifconfig "$epair2" inet 192.0.3.1/24 up
sudo jail -c name="$jail_name" path=/ host.hostname="$jail_name" \
  persist vnet vnet.interface="$peer"
sudo ifconfig "$peer2" vnet "$jail_name"
# A newly created VNET jail has an unconfigured lo0.  FreeBSD resolves
# blackhole and reject nexthops through the matching loopback address, just as
# a normal boot configures them on the host.
sudo jexec "$jail_name" ifconfig lo0 inet 127.0.0.1/8 up
sudo jexec "$jail_name" ifconfig lo0 inet6 ::1/128
sudo jexec "$jail_name" ifconfig "$peer" inet 192.0.2.2/24 up
sudo jexec "$jail_name" ifconfig "$peer" inet6 2001:db8:8431::2/64
sudo jexec "$jail_name" ifconfig "$peer2" inet 192.0.3.2/24 up

# Exercise the distinct FreeBSD metric and ECMP weight fields. Both paths are
# installed only inside the disposable VNET, and the observer must return the
# exact native weights rather than confusing either one with route preference.
sudo jexec "$jail_name" route add -net 198.18.6.0/24 192.0.2.1 -weight 2
sudo jexec "$jail_name" route add -net 198.18.6.0/24 192.0.3.1 -weight 3
sudo jexec "$jail_name" "$binary" freebsd observe-weighted-multipath 0 \
  198.18.6.0/24 "$peer" "$peer2"
sudo jexec "$jail_name" "$binary" freebsd install-load-balance 0 \
  198.18.7.0/24 "$peer" 192.0.2.1 2 "$peer2" 192.0.3.1 3
sudo jexec "$jail_name" "$binary" freebsd observe-weighted-multipath 0 \
  198.18.7.0/24 "$peer" "$peer2"
sudo jexec "$jail_name" "$binary" freebsd delete-load-balance 0 \
  198.18.7.0/24 "$peer" 192.0.2.1 2 "$peer2" 192.0.3.1 3
if sudo jexec "$jail_name" netstat -rn -f inet |
    grep -F "198.18.7.0/24"; then
  exit 1
fi
if [ -n "$load_balance_plugin_test" ] && [ -n "$plugin" ]; then
  sudo jexec "$jail_name" env \
    DANG_RIB_REGISTRY_FILE="$load_balance_registry" \
    "$load_balance_plugin_test" "$plugin" \
    0 198.18.8.0/24 "$peer" 192.0.2.1 "$peer2" 192.0.3.1
  if sudo jexec "$jail_name" netstat -rn -f inet |
      grep -F "198.18.8.0/24"; then
    exit 1
  fi
fi
if [ -n "$netconf_test" ] && [ -n "$dangd" ] &&
   [ -n "$plugin_worker" ] && [ -n "$interface_plugin" ] &&
   [ -n "$plugin" ]; then
  sudo jexec "$jail_name" env DANG_RIB_REGISTRY_FILE="$netconf_registry" \
    "$netconf_test" "$dangd" "$plugin_worker" "$plugin" \
    "$interface_plugin" \
    ipv4-0 198.18.9.0/24 "$peer" 192.0.2.2 192.0.2.1
  if sudo jexec "$jail_name" netstat -rn -f inet |
      grep -F "198.18.9.0/24"; then
    exit 1
  fi
fi
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

sudo jexec "$jail_name" "$binary" freebsd install-special 0 \
  198.18.4.0/24 discard
sudo jexec "$jail_name" "$binary" freebsd observe-special 0 \
  198.18.4.0/24 discard
sudo jexec "$jail_name" "$binary" freebsd delete-special 0 \
  198.18.4.0/24 discard
sudo jexec "$jail_name" "$binary" freebsd install-special 0 \
  2001:db8:8434::/64 discard-with-error
sudo jexec "$jail_name" "$binary" freebsd observe-special 0 \
  2001:db8:8434::/64 discard-with-error
sudo jexec "$jail_name" "$binary" freebsd delete-special 0 \
  2001:db8:8434::/64 discard-with-error

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

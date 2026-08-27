#!/bin/sh
# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

set -eu

root=${1:-/tmp/dang_plugins_validation}
jail_name=dang_kea_test
host_interface=
pid4=
pid6=

cleanup() {
  [ -z "$pid4" ] || kill "$pid4" 2>/dev/null || true
  [ -z "$pid6" ] || kill "$pid6" 2>/dev/null || true
  jail -r "$jail_name" 2>/dev/null || true
  if [ -n "$host_interface" ]; then
    ifconfig "$host_interface" destroy 2>/dev/null || true
  fi
  rm -f /var/run/kea/kea4-ctrl-socket /var/run/kea/kea6-ctrl-socket
}
trap cleanup EXIT INT TERM
cleanup

host_interface=$(ifconfig epair create)
peer_interface=${host_interface%a}b
jail -c name="$jail_name" persist vnet path=/ host.hostname=dang-kea-test \
  allow.raw_sockets=1
ifconfig "$peer_interface" vnet "$jail_name"
jexec "$jail_name" ifconfig "$peer_interface" name dangkea0
jexec "$jail_name" ifconfig lo0 up
jexec "$jail_name" ifconfig dangkea0 inet 192.0.2.1/24 up
jexec "$jail_name" ifconfig dangkea0 inet6 2001:db8:6::1/64 up
ifconfig "$host_interface" up

jexec "$jail_name" /usr/local/sbin/kea-dhcp4 -d \
  -c "$root/tests/kea4-boot.json" > /tmp/dang-kea4.log 2>&1 &
pid4=$!
jexec "$jail_name" /usr/local/sbin/kea-dhcp6 -d \
  -c "$root/tests/kea6-boot.json" > /tmp/dang-kea6.log 2>&1 &
pid6=$!

attempt=0
while [ ! -S /var/run/kea/kea4-ctrl-socket ] || \
      [ ! -S /var/run/kea/kea6-ctrl-socket ]; do
  attempt=$((attempt + 1))
  if [ "$attempt" -ge 100 ]; then
    cat /tmp/dang-kea4.log /tmp/dang-kea6.log
    exit 1
  fi
  sleep 0.1
done

jexec -l -U root "$jail_name" env \
  DANG_KEA_DHCP4_SOCKET=/var/run/kea/kea4-ctrl-socket \
  DANG_KEA_DHCP6_SOCKET=/var/run/kea/kea6-ctrl-socket \
  "$root/build/kea_plugin_integration_test" \
  "$root/build/dangd_kea_plugin.so" "$root/tests/kea-before.xml" \
  "$root/tests/kea-proposed.xml"

if jexec "$jail_name" ifconfig -l | tr ' ' '\n' | grep -Ev '^(lo0|dangkea0)$' \
  | grep -q .; then
  echo "unexpected host interface entered the VNET jail" >&2
  exit 1
fi

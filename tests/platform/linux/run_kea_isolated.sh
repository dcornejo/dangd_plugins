#!/bin/sh
# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

set -eu

root=${1:-/tmp/dang_plugins_validation}
namespace=dang-kea-test
host_interface=dangkea-host
pid4=
pid6=

cleanup() {
  [ -z "$pid4" ] || kill "$pid4" 2>/dev/null || true
  [ -z "$pid6" ] || kill "$pid6" 2>/dev/null || true
  ip netns del "$namespace" 2>/dev/null || true
  rm -f /var/run/kea/kea4-ctrl-socket /var/run/kea/kea6-ctrl-socket \
    /tmp/kea-dhcp4.conf /tmp/kea-dhcp6.conf \
    /tmp/kea-before-linux.xml /tmp/kea-proposed-linux.xml \
    /tmp/dang-kea-dhcp4 /tmp/dang-kea-dhcp6
}
trap cleanup EXIT INT TERM
cleanup

ip netns add "$namespace"
ip link add "$host_interface" type veth peer name dangkea0
ip link set dangkea0 netns "$namespace"
ip link set "$host_interface" up
ip netns exec "$namespace" ip link set lo up
ip netns exec "$namespace" ip link set dangkea0 up
ip netns exec "$namespace" ip address add 192.0.2.1/24 dev dangkea0
ip netns exec "$namespace" ip -6 address add 2001:db8:6::1/64 dev dangkea0

dhcp4=$(command -v kea-dhcp4)
dhcp6=$(command -v kea-dhcp6)
cp "$dhcp4" /tmp/dang-kea-dhcp4
cp "$dhcp6" /tmp/dang-kea-dhcp6
dhcp4=/tmp/dang-kea-dhcp4
dhcp6=/tmp/dang-kea-dhcp6
sed 's#/var/run/kea#/run/kea#g' "$root/tests/kea4-boot.json" \
  > /tmp/kea-dhcp4.conf
sed 's#/var/run/kea#/run/kea#g' "$root/tests/kea6-boot.json" \
  > /tmp/kea-dhcp6.conf
sed 's#/var/run/kea#/run/kea#g' "$root/tests/kea-before.xml" \
  > /tmp/kea-before-linux.xml
sed 's#/var/run/kea#/run/kea#g' "$root/tests/kea-proposed.xml" \
  > /tmp/kea-proposed-linux.xml
ip netns exec "$namespace" "$dhcp4" -d -p 1067 \
  -c /tmp/kea-dhcp4.conf \
  > /tmp/dang-kea4.log 2>&1 &
pid4=$!
ip netns exec "$namespace" "$dhcp6" -d -p 1547 \
  -c /tmp/kea-dhcp6.conf \
  > /tmp/dang-kea6.log 2>&1 &
pid6=$!

attempt=0
while [ ! -S /run/kea/kea4-ctrl-socket ] || \
      [ ! -S /run/kea/kea6-ctrl-socket ]; do
  attempt=$((attempt + 1))
  if [ "$attempt" -ge 100 ]; then
    cat /tmp/dang-kea4.log /tmp/dang-kea6.log
    exit 1
  fi
  sleep 0.1
done

DANG_KEA_DHCP4_SOCKET=/run/kea/kea4-ctrl-socket \
DANG_KEA_DHCP6_SOCKET=/run/kea/kea6-ctrl-socket \
  ip netns exec "$namespace" "$root/build/kea_plugin_integration_test" \
  "$root/build/dangd_kea_plugin.so" /tmp/kea-before-linux.xml \
  /tmp/kea-proposed-linux.xml

if ip netns exec "$namespace" ip -o link show \
  | awk -F': ' '{print $2}' | sed 's/@.*//' \
  | grep -Ev '^(lo|dangkea0)$' | grep -q .; then
  echo "unexpected LAN-like interface entered the test namespace" >&2
  exit 1
fi

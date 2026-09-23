#!/bin/sh
# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

set -eu

root=${1:-/tmp/dang_plugins_validation}
namespace=dang-kea-test
host_interface=dangkea-host
runtime_dir=/tmp/dang-kea-runtime-$$
socket4=/run/kea/dang-kea4-$$.sock
socket6=/run/kea/dang-kea6-$$.sock
pid4=
pid6=

cleanup() {
  [ -z "$pid4" ] || kill "$pid4" 2>/dev/null || true
  [ -z "$pid6" ] || kill "$pid6" 2>/dev/null || true
  ip netns del "$namespace" 2>/dev/null || true
  rm -rf "$runtime_dir"
  rm -f "$socket4" "$socket6"
  rm -f /tmp/kea-dhcp4.conf /tmp/kea-dhcp6.conf \
    /tmp/kea-before-linux.xml /tmp/kea-proposed-linux.xml \
    /tmp/kea-before4-linux.xml /tmp/kea-proposed4-linux.xml \
    /tmp/kea-before6-linux.xml /tmp/kea-proposed6-linux.xml \
    /tmp/dang-kea-dhcp4 /tmp/dang-kea-dhcp6
}
trap cleanup EXIT INT TERM
cleanup
mkdir -p "$runtime_dir"

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
sed -e "s#/var/run/kea/kea4-ctrl-socket#$socket4#g" \
  -e 's#@KEA_HOOK_DIR@#/usr/lib/x86_64-linux-gnu/kea/hooks#g' \
  "$root/tests/kea4-boot.json" \
  > /tmp/kea-dhcp4.conf
sed -e "s#/var/run/kea/kea6-ctrl-socket#$socket6#g" \
  -e 's#@KEA_HOOK_DIR@#/usr/lib/x86_64-linux-gnu/kea/hooks#g' \
  "$root/tests/kea6-boot.json" \
  > /tmp/kea-dhcp6.conf
sed -e "s#/var/run/kea/kea4-ctrl-socket#$socket4#g" \
  -e "s#/var/run/kea/kea6-ctrl-socket#$socket6#g" \
  -e 's#@KEA_HOOK_DIR@#/usr/lib/x86_64-linux-gnu/kea/hooks#g' \
  "$root/tests/kea-before.xml" \
  > /tmp/kea-before-linux.xml
sed -e "s#/var/run/kea/kea4-ctrl-socket#$socket4#g" \
  -e "s#/var/run/kea/kea6-ctrl-socket#$socket6#g" \
  -e 's#@KEA_HOOK_DIR@#/usr/lib/x86_64-linux-gnu/kea/hooks#g' \
  "$root/tests/kea-proposed.xml" \
  > /tmp/kea-proposed-linux.xml
unavailable4="$runtime_dir/unavailable4.sock"
sed -e "s#/var/run/kea/kea4-ctrl-socket#$unavailable4#g" \
  -e "s#/var/run/kea/kea6-ctrl-socket#$socket6#g" \
  -e 's#@KEA_HOOK_DIR@#/usr/lib/x86_64-linux-gnu/kea/hooks#g' \
  "$root/tests/kea-before.xml" \
  > /tmp/kea-before6-linux.xml
sed -e "s#/var/run/kea/kea4-ctrl-socket#$unavailable4#g" \
  -e "s#/var/run/kea/kea6-ctrl-socket#$socket6#g" \
  -e 's#@KEA_HOOK_DIR@#/usr/lib/x86_64-linux-gnu/kea/hooks#g' \
  "$root/tests/kea-proposed6.xml" \
  > /tmp/kea-proposed6-linux.xml
unavailable6="$runtime_dir/unavailable6.sock"
sed -e "s#/var/run/kea/kea4-ctrl-socket#$socket4#g" \
  -e "s#/var/run/kea/kea6-ctrl-socket#$unavailable6#g" \
  -e 's#@KEA_HOOK_DIR@#/usr/lib/x86_64-linux-gnu/kea/hooks#g' \
  "$root/tests/kea-before.xml" \
  > /tmp/kea-before4-linux.xml
sed -e "s#/var/run/kea/kea4-ctrl-socket#$socket4#g" \
  -e "s#/var/run/kea/kea6-ctrl-socket#$unavailable6#g" \
  -e 's#@KEA_HOOK_DIR@#/usr/lib/x86_64-linux-gnu/kea/hooks#g' \
  "$root/tests/kea-proposed4.xml" \
  > /tmp/kea-proposed4-linux.xml
ip netns exec "$namespace" env KEA_PIDFILE_DIR="$runtime_dir" \
  "$dhcp4" -d -p 1067 \
  -c /tmp/kea-dhcp4.conf \
  > /tmp/dang-kea4.log 2>&1 &
pid4=$!
ip netns exec "$namespace" env KEA_PIDFILE_DIR="$runtime_dir" \
  "$dhcp6" -d -p 1547 \
  -c /tmp/kea-dhcp6.conf \
  > /tmp/dang-kea6.log 2>&1 &
pid6=$!

attempt=0
while [ ! -S "$socket4" ] || [ ! -S "$socket6" ]; do
  attempt=$((attempt + 1))
  if [ "$attempt" -ge 100 ]; then
    cat /tmp/dang-kea4.log /tmp/dang-kea6.log
    exit 1
  fi
  sleep 0.1
done

DANG_KEA_DHCP4_SOCKET="$socket4" \
DANG_KEA_DHCP6_SOCKET="$socket6" \
  ip netns exec "$namespace" "$root/build/kea_plugin_integration_test" \
  "$root/build/dangd_kea_plugin.so" /tmp/kea-before-linux.xml \
  /tmp/kea-proposed-linux.xml

# DHCPv4 deliberately has no listening socket in this second transaction. It
# can pass only when the plugin leaves that unchanged daemon untouched.
DANG_KEA_SKIP_OPERATIONAL=1 \
DANG_KEA_DHCP4_SOCKET="$unavailable4" \
DANG_KEA_DHCP6_SOCKET="$socket6" \
  ip netns exec "$namespace" "$root/build/kea_plugin_integration_test" \
  "$root/build/dangd_kea_plugin.so" /tmp/kea-before6-linux.xml \
  /tmp/kea-proposed6-linux.xml

# Mirror the proof with an unavailable unchanged DHCPv6 daemon.
DANG_KEA_SKIP_OPERATIONAL=1 \
DANG_KEA_DHCP4_SOCKET="$socket4" \
DANG_KEA_DHCP6_SOCKET="$unavailable6" \
  ip netns exec "$namespace" "$root/build/kea_plugin_integration_test" \
  "$root/build/dangd_kea_plugin.so" /tmp/kea-before4-linux.xml \
  /tmp/kea-proposed4-linux.xml

# Let validation succeed, remove DHCPv4 immediately before apply, and require
# both apply and its conservative compensation to be attributed structurally.
DANG_KEA_SKIP_OPERATIONAL=1 \
DANG_KEA_EXPECT_APPLY_FAILURE=kea-dhcp4-server \
DANG_KEA_DHCP4_SOCKET="$socket4" \
DANG_KEA_DHCP6_SOCKET="$unavailable6" \
  ip netns exec "$namespace" "$root/build/kea_plugin_integration_test" \
  "$root/build/dangd_kea_plugin.so" /tmp/kea-before4-linux.xml \
  /tmp/kea-proposed4-linux.xml

# Run this destructive socket-removal case last. It requires rollback failure
# to carry the changed DHCPv6 module and configuration path.
DANG_KEA_SKIP_OPERATIONAL=1 \
DANG_KEA_EXPECT_ROLLBACK_FAILURE=kea-dhcp6-server \
DANG_KEA_DHCP4_SOCKET="$unavailable4" \
DANG_KEA_DHCP6_SOCKET="$socket6" \
  ip netns exec "$namespace" "$root/build/kea_plugin_integration_test" \
  "$root/build/dangd_kea_plugin.so" /tmp/kea-before6-linux.xml \
  /tmp/kea-proposed6-linux.xml

if ip netns exec "$namespace" ip -o link show \
  | awk -F': ' '{print $2}' | sed 's/@.*//' \
  | grep -Ev '^(lo|dangkea0)$' | grep -q .; then
  echo "unexpected LAN-like interface entered the test namespace" >&2
  exit 1
fi

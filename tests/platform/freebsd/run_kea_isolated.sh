#!/bin/sh
# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

set -eu

root=${1:-/tmp/dang_plugins_validation}
jail_name=dang_kea_test
host_interface=
runtime_dir=/tmp/dang-kea-runtime-$$
socket4=/var/run/kea/dang-kea4-$$.sock
socket6=/var/run/kea/dang-kea6-$$.sock
pid4=
pid6=

cleanup() {
  [ -z "$pid4" ] || kill "$pid4" 2>/dev/null || true
  [ -z "$pid6" ] || kill "$pid6" 2>/dev/null || true
  jail -r "$jail_name" 2>/dev/null || true
  if [ -n "$host_interface" ]; then
    ifconfig "$host_interface" destroy 2>/dev/null || true
  fi
  rm -rf "$runtime_dir"
  rm -f "$socket4" "$socket6"
  rm -f /tmp/kea-dhcp4-freebsd.json /tmp/kea-dhcp6-freebsd.json \
    /tmp/kea-before-freebsd.xml /tmp/kea-proposed-freebsd.xml \
    /tmp/kea-before4-freebsd.xml /tmp/kea-proposed4-freebsd.xml \
    /tmp/kea-before6-freebsd.xml /tmp/kea-proposed6-freebsd.xml \
    /tmp/kea-before-validation-freebsd.xml \
    /tmp/kea-proposed-validation-freebsd.xml
}
trap cleanup EXIT INT TERM
cleanup
mkdir -p "$runtime_dir"

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

sed -e 's#@KEA_HOOK_DIR@#/usr/local/lib/kea/hooks#g' \
  -e "s#/var/run/kea/kea4-ctrl-socket#$socket4#g" \
  "$root/tests/kea4-boot.json" > /tmp/kea-dhcp4-freebsd.json
sed -e 's#@KEA_HOOK_DIR@#/usr/local/lib/kea/hooks#g' \
  -e "s#/var/run/kea/kea6-ctrl-socket#$socket6#g" \
  "$root/tests/kea6-boot.json" > /tmp/kea-dhcp6-freebsd.json
sed -e 's#@KEA_HOOK_DIR@#/usr/local/lib/kea/hooks#g' \
  -e "s#/var/run/kea/kea4-ctrl-socket#$socket4#g" \
  -e "s#/var/run/kea/kea6-ctrl-socket#$socket6#g" \
  "$root/tests/kea-before.xml" > /tmp/kea-before-freebsd.xml
sed -e 's#@KEA_HOOK_DIR@#/usr/local/lib/kea/hooks#g' \
  -e "s#/var/run/kea/kea4-ctrl-socket#$socket4#g" \
  -e "s#/var/run/kea/kea6-ctrl-socket#$socket6#g" \
  "$root/tests/kea-proposed.xml" > /tmp/kea-proposed-freebsd.xml
unavailable4="$runtime_dir/unavailable4.sock"
sed -e 's#@KEA_HOOK_DIR@#/usr/local/lib/kea/hooks#g' \
  -e "s#/var/run/kea/kea4-ctrl-socket#$unavailable4#g" \
  -e "s#/var/run/kea/kea6-ctrl-socket#$socket6#g" \
  "$root/tests/kea-before.xml" > /tmp/kea-before6-freebsd.xml
sed -e 's#@KEA_HOOK_DIR@#/usr/local/lib/kea/hooks#g' \
  -e "s#/var/run/kea/kea4-ctrl-socket#$unavailable4#g" \
  -e "s#/var/run/kea/kea6-ctrl-socket#$socket6#g" \
  "$root/tests/kea-proposed6.xml" > /tmp/kea-proposed6-freebsd.xml
unavailable6="$runtime_dir/unavailable6.sock"
sed -e 's#@KEA_HOOK_DIR@#/usr/local/lib/kea/hooks#g' \
  -e "s#/var/run/kea/kea4-ctrl-socket#$socket4#g" \
  -e "s#/var/run/kea/kea6-ctrl-socket#$unavailable6#g" \
  "$root/tests/kea-before.xml" > /tmp/kea-before4-freebsd.xml
sed -e 's#@KEA_HOOK_DIR@#/usr/local/lib/kea/hooks#g' \
  -e "s#/var/run/kea/kea4-ctrl-socket#$socket4#g" \
  -e "s#/var/run/kea/kea6-ctrl-socket#$unavailable6#g" \
  "$root/tests/kea-proposed4.xml" > /tmp/kea-proposed4-freebsd.xml
sed "s#$socket6#$unavailable6#g" /tmp/kea-before6-freebsd.xml \
  > /tmp/kea-before-validation-freebsd.xml
sed "s#$socket6#$unavailable6#g" /tmp/kea-proposed6-freebsd.xml \
  > /tmp/kea-proposed-validation-freebsd.xml

jexec "$jail_name" env KEA_PIDFILE_DIR="$runtime_dir" \
  /usr/local/sbin/kea-dhcp4 -d \
  -c /tmp/kea-dhcp4-freebsd.json > /tmp/dang-kea4.log 2>&1 &
pid4=$!
jexec "$jail_name" env KEA_PIDFILE_DIR="$runtime_dir" \
  /usr/local/sbin/kea-dhcp6 -d \
  -c /tmp/kea-dhcp6-freebsd.json > /tmp/dang-kea6.log 2>&1 &
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

# Both sockets are absent, but only DHCPv6 changed. Validation must skip
# unchanged DHCPv4 and attribute the expected rejection to DHCPv6.
jexec -l -U root "$jail_name" env \
  DANG_KEA_EXPECT_VALIDATE_FAILURE=kea-dhcp6-server \
  DANG_KEA_DHCP4_SOCKET="$unavailable4" \
  DANG_KEA_DHCP6_SOCKET="$unavailable6" \
  "$root/build/kea_plugin_integration_test" \
  "$root/build/dangd_kea_plugin.so" \
  /tmp/kea-before-validation-freebsd.xml \
  /tmp/kea-proposed-validation-freebsd.xml

# A no-op transaction reaches no daemon, then complete state retrieval must
# attribute the unavailable DHCPv4 lease source precisely.
jexec -l -U root "$jail_name" env \
  DANG_KEA_EXPECT_OPERATIONAL_FAILURE=kea-dhcp4-server \
  DANG_KEA_DHCP4_SOCKET="$unavailable4" \
  DANG_KEA_DHCP6_SOCKET="$socket6" \
  "$root/build/kea_plugin_integration_test" \
  "$root/build/dangd_kea_plugin.so" /tmp/kea-before6-freebsd.xml \
  /tmp/kea-before6-freebsd.xml

# Mirror the read-side proof after DHCPv4 retrieval succeeds completely.
jexec -l -U root "$jail_name" env \
  DANG_KEA_EXPECT_OPERATIONAL_FAILURE=kea-dhcp6-server \
  DANG_KEA_DHCP4_SOCKET="$socket4" \
  DANG_KEA_DHCP6_SOCKET="$unavailable6" \
  "$root/build/kea_plugin_integration_test" \
  "$root/build/dangd_kea_plugin.so" /tmp/kea-before4-freebsd.xml \
  /tmp/kea-before4-freebsd.xml

# Reconcile a configured DHCPv4 subnet that the untouched boot daemon lacks.
# Complete state must fail closed rather than publish empty lease statistics.
jexec -l -U root "$jail_name" env \
  DANG_KEA_EXPECT_OPERATIONAL_FAILURE=kea-dhcp4-server \
  DANG_KEA_EXPECT_OPERATIONAL_SUBTREE=lease-stats \
  DANG_KEA_DHCP4_SOCKET="$socket4" \
  DANG_KEA_DHCP6_SOCKET="$unavailable6" \
  "$root/build/kea_plugin_integration_test" \
  "$root/build/dangd_kea_plugin.so" /tmp/kea-proposed4-freebsd.xml \
  /tmp/kea-proposed4-freebsd.xml

jexec -l -U root "$jail_name" env \
  DANG_KEA_DHCP4_SOCKET="$socket4" \
  DANG_KEA_DHCP6_SOCKET="$socket6" \
  "$root/build/kea_plugin_integration_test" \
  "$root/build/dangd_kea_plugin.so" /tmp/kea-before-freebsd.xml \
  /tmp/kea-proposed-freebsd.xml

# An absent DHCPv4 socket proves the unchanged daemon is not contacted during
# validation, apply, or explicit rollback of this DHCPv6-only transaction.
jexec -l -U root "$jail_name" env \
  DANG_KEA_SKIP_OPERATIONAL=1 \
  DANG_KEA_DHCP4_SOCKET="$unavailable4" \
  DANG_KEA_DHCP6_SOCKET="$socket6" \
  "$root/build/kea_plugin_integration_test" \
  "$root/build/dangd_kea_plugin.so" /tmp/kea-before6-freebsd.xml \
  /tmp/kea-proposed6-freebsd.xml

# Mirror the proof with an unavailable unchanged DHCPv6 daemon.
jexec -l -U root "$jail_name" env \
  DANG_KEA_SKIP_OPERATIONAL=1 \
  DANG_KEA_DHCP4_SOCKET="$socket4" \
  DANG_KEA_DHCP6_SOCKET="$unavailable6" \
  "$root/build/kea_plugin_integration_test" \
  "$root/build/dangd_kea_plugin.so" /tmp/kea-before4-freebsd.xml \
  /tmp/kea-proposed4-freebsd.xml

# Let validation succeed, remove DHCPv4 immediately before apply, and require
# structural attribution of both apply and conservative compensation failure.
jexec -l -U root "$jail_name" env \
  DANG_KEA_SKIP_OPERATIONAL=1 \
  DANG_KEA_EXPECT_APPLY_FAILURE=kea-dhcp4-server \
  DANG_KEA_DHCP4_SOCKET="$socket4" \
  DANG_KEA_DHCP6_SOCKET="$unavailable6" \
  "$root/build/kea_plugin_integration_test" \
  "$root/build/dangd_kea_plugin.so" /tmp/kea-before4-freebsd.xml \
  /tmp/kea-proposed4-freebsd.xml

# Run this destructive socket-removal case last and require structural
# attribution of the expected DHCPv6 rollback failure.
jexec -l -U root "$jail_name" env \
  DANG_KEA_SKIP_OPERATIONAL=1 \
  DANG_KEA_EXPECT_ROLLBACK_FAILURE=kea-dhcp6-server \
  DANG_KEA_DHCP4_SOCKET="$unavailable4" \
  DANG_KEA_DHCP6_SOCKET="$socket6" \
  "$root/build/kea_plugin_integration_test" \
  "$root/build/dangd_kea_plugin.so" /tmp/kea-before6-freebsd.xml \
  /tmp/kea-proposed6-freebsd.xml

if jexec "$jail_name" ifconfig -l | tr ' ' '\n' | grep -Ev '^(lo0|dangkea0)$' \
  | grep -q .; then
  echo "unexpected host interface entered the VNET jail" >&2
  exit 1
fi

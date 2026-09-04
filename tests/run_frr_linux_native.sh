#!/bin/sh
# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

set -eu

if [ "$#" -ne 1 ] || [ "$(id -u)" -ne 0 ]; then
  echo "usage: sudo $0 FRR_MUTATION_CHECK" >&2
  exit 2
fi

probe=$1
name="dangd-test-$$"
run_dir="/var/run/frr/$name"
state_dir="/var/lib/frr/$name"
log="/tmp/$name.log"

cleanup() {
  if [ -f "$run_dir/mgmtd.pid" ]; then
    kill "$(cat "$run_dir/mgmtd.pid")" 2>/dev/null || true
  fi
  rm -rf -- "$run_dir" "$state_dir"
  rm -f -- "$log"
}
trap cleanup EXIT HUP INT TERM

install -d -o frr -g frr -m 0750 "$run_dir" "$state_dir"
/usr/lib/frr/mgmtd -N "$name" -d -u frr -g frr \
  -i "$run_dir/mgmtd.pid" --log "file:$log"
i=0
while [ ! -S "$run_dir/mgmtd_fe.sock" ]; do
  i=$((i + 1))
  [ "$i" -lt 50 ] || { echo "isolated mgmtd did not start" >&2; exit 1; }
  sleep 0.1
done

xml='<routing xmlns="http://frrouting.org/yang/routing" xmlns:s="http://frrouting.org/yang/staticd"><control-plane-protocols><control-plane-protocol><type>s:staticd</type><name>dangd-test</name><vrf>default</vrf></control-plane-protocol></control-plane-protocols></routing>'
"$probe" --allow-isolated-test "$run_dir/mgmtd_fe.sock" \
  /frr-routing:routing "$xml" dangd-test

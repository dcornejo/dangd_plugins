#!/bin/sh
# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

# Exercise a reversible BFD profile transaction in a disposable FRR pathspace.
# A profile has no peer or interface binding, so this interaction creates no
# packet, interface, address, route, or dependency on the host's LAN.
set -eu

if [ "$#" -ne 2 ] || [ "$(id -u)" -ne 0 ]; then
  echo "usage: sudo $0 FRR_MUTATION_CHECK FRR_SESSION_CHECK" >&2
  exit 2
fi

probe=$1
session_probe=$2
case "$(uname -s)" in
  Linux)
    frr_dir=/usr/lib/frr
    group=frrvty
    copy_bfdd=1
    ;;
  FreeBSD)
    frr_dir=/usr/local/sbin
    group=frr
    copy_bfdd=0
    ;;
  *)
    echo "SKIP: native FRR BFD test supports Linux and FreeBSD only"
    exit 77
    ;;
esac

for required in mgmtd zebra bfdd; do
  [ -x "$frr_dir/$required" ] || {
    echo "SKIP: $required is not installed"
    exit 77
  }
done

tag="dangd-test-bfd-$$"
run_dir="/var/run/frr/$tag"
state_dir="/var/lib/frr/$tag"
mgmtd_log="/tmp/$tag-mgmtd.log"
zebra_log="/tmp/$tag-zebra.log"
bfd_log="/tmp/$tag-bfdd.log"
bfd_copy="/tmp/$tag-bfdd"

cleanup() {
  status=$?
  for daemon in bfdd zebra mgmtd; do
    if [ -s "$run_dir/$daemon.pid" ]; then
      kill "$(cat "$run_dir/$daemon.pid")" 2>/dev/null || true
    fi
  done
  if [ "$status" -ne 0 ] && [ "$status" -ne 77 ]; then
    for daemon_log in "$mgmtd_log" "$zebra_log" "$bfd_log"; do
      [ ! -s "$daemon_log" ] || {
        echo "--- $daemon_log" >&2
        tail -100 "$daemon_log" >&2
      }
    done
  fi
  rm -rf -- "$run_dir" "$state_dir"
  rm -f -- "$mgmtd_log" "$zebra_log" "$bfd_log" "$bfd_copy"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

install -d -o frr -g "$group" -m 0770 "$run_dir" "$state_dir"
for log in "$mgmtd_log" "$zebra_log" "$bfd_log"; do
  install -o frr -g "$group" -m 0660 /dev/null "$log"
done
for daemon in mgmtd zebra bfdd; do
  install -o frr -g "$group" -m 0660 /dev/null "$run_dir/$daemon.pid"
done

bfd_command="$frr_dir/bfdd"
if [ "$copy_bfdd" -eq 1 ]; then
  # See run_frr_protocol_inventory.sh for the Ubuntu AppArmor rationale.
  cp "$frr_dir/bfdd" "$bfd_copy"
  chmod 0755 "$bfd_copy"
  bfd_command=$bfd_copy
fi

"$frr_dir/mgmtd" -N "$tag" -d -u frr -g "$group" \
  -i "$run_dir/mgmtd.pid" --log "file:$mgmtd_log" >/dev/null 2>&1
"$frr_dir/zebra" -N "$tag" -d -u frr -g "$group" \
  -i "$run_dir/zebra.pid" --log "file:$zebra_log" >/dev/null 2>&1
"$bfd_command" -N "$tag" -d -u frr -g "$group" \
  -i "$run_dir/bfdd.pid" --log "file:$bfd_log" >/dev/null 2>&1

i=0
while [ ! -S "$run_dir/mgmtd_fe.sock" ]; do
  i=$((i + 1))
  [ "$i" -lt 50 ] || {
    echo "isolated mgmtd did not start" >&2
    exit 1
  }
  sleep 0.1
done

# A frontend socket can accept requests before protocol backends finish their
# registration. Wait for the live capability instead of allowing mgmtd to
# accept a successful no-op replacement against an unowned path.
i=0
while :; do
  i=$((i + 1))
  if "$session_probe" "$run_dir/mgmtd_fe.sock" --operational \
      /ietf-yang-library:yang-library 2>/dev/null |
      grep -q '<name[^>]*>frr-bfdd</name>'; then
    break
  fi
  [ "$i" -lt 50 ] || {
    echo "bfdd did not register frr-bfdd with mgmtd" >&2
    exit 1
  }
  sleep 0.1
done

xml='<bfdd xmlns="http://frrouting.org/yang/bfdd"><bfd><profile><name>dangd-test</name></profile></bfd></bfdd>'
set +e
result=$("$probe" --allow-isolated-test "$run_dir/mgmtd_fe.sock" \
  /frr-bfdd:bfdd "$xml" dangd-test 2>&1)
status=$?
set -e
if [ "$status" -eq 3 ] &&
    printf '%s\n' "$result" |
      grep -q 'verify /frr-bfdd:bfdd: FRR running datastore did not retain'; then
  echo "SKIP: FRR advertises frr-bfdd but bfdd has no live mgmtd config backend"
  exit 77
fi
printf '%s\n' "$result"
exit "$status"

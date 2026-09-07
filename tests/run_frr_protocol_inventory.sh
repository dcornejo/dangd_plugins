#!/bin/sh
# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

# Inventory optional FRR mgmtd backends in disposable pathspaces. This test
# intentionally creates no interface, address, or route; it only asks each
# daemon to register its YANG modules and reads RFC 8525 YANG Library data.
set -eu

if [ "$#" -ne 1 ] || [ "$(id -u)" -ne 0 ]; then
  echo "usage: sudo $0 FRR_SESSION_CHECK" >&2
  exit 2
fi

probe=$1
case "$(uname -s)" in
  Linux)
    frr_dir=/usr/lib/frr
    group=frrvty
    copy_daemon=1
    ;;
  FreeBSD)
    frr_dir=/usr/local/sbin
    group=frr
    copy_daemon=0
    ;;
  *)
    echo "SKIP: native FRR inventory supports Linux and FreeBSD only"
    exit 77
    ;;
esac
tested=0
advertised=0
run_dir=
state_dir=
mgmtd_log=
zebra_log=
daemon_log=
library_xml=
daemon_copy=

cleanup() {
  if [ -n "$run_dir" ]; then
    for process in "${daemon:-}" zebra mgmtd; do
      if [ -n "$process" ] && [ -s "$run_dir/$process.pid" ]; then
        kill "$(cat "$run_dir/$process.pid")" 2>/dev/null || true
      fi
    done
    rm -rf -- "$run_dir" "$state_dir"
    rm -f -- "$mgmtd_log" "$zebra_log" "$daemon_log" "$library_xml" \
      "$daemon_copy"
  fi
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

inventory_daemon() {
  daemon=$1
  module=$2
  [ -x "$frr_dir/$daemon" ] || {
    echo "SKIP $daemon: binary is not installed"
    return
  }

  tested=$((tested + 1))
  tag="dang-inventory-${daemon}-$$"
  run_dir="/var/run/frr/$tag"
  state_dir="/var/lib/frr/$tag"
  mgmtd_log="/tmp/$tag-mgmtd.log"
  zebra_log="/tmp/$tag-zebra.log"
  daemon_log="/tmp/$tag-$daemon.log"
  library_xml="/tmp/$tag-library.xml"
  daemon_copy="/tmp/$tag-$daemon"

  install -d -o frr -g "$group" -m 0770 "$run_dir" "$state_dir"
  for log in "$mgmtd_log" "$zebra_log" "$daemon_log"; do
    install -o frr -g "$group" -m 0660 /dev/null "$log"
  done
  for process in mgmtd zebra "$daemon"; do
    install -o frr -g "$group" -m 0660 /dev/null "$run_dir/$process.pid"
  done

  daemon_command="$frr_dir/$daemon"
  if [ "$copy_daemon" -eq 1 ]; then
    # Ubuntu's packaged AppArmor policy denies nonstandard daemon pathspaces.
    # A disposable copy avoids changing that host policy and cannot collide
    # with the production daemon's executable profile or sockets. FreeBSD has
    # no corresponding executable-path restriction and runs the package binary.
    cp "$frr_dir/$daemon" "$daemon_copy"
    chmod 0755 "$daemon_copy"
    daemon_command=$daemon_copy
  fi

  "$frr_dir/mgmtd" -N "$tag" -d -u frr -g "$group" \
    -i "$run_dir/mgmtd.pid" --log "file:$mgmtd_log" \
    >/dev/null 2>&1
  "$frr_dir/zebra" -N "$tag" -d -u frr -g "$group" \
    -i "$run_dir/zebra.pid" --log "file:$zebra_log" \
    >/dev/null 2>&1
  "$daemon_command" -N "$tag" -d -u frr -g "$group" \
    -i "$run_dir/$daemon.pid" --log "file:$daemon_log" \
    >/dev/null 2>&1

  i=0
  while [ ! -S "$run_dir/mgmtd_fe.sock" ]; do
    i=$((i + 1))
    [ "$i" -lt 50 ] || {
      echo "FAIL $daemon: isolated mgmtd did not start" >&2
      return 1
    }
    sleep 0.1
  done
  if [ ! -s "$run_dir/$daemon.pid" ] ||
      ! kill -0 "$(cat "$run_dir/$daemon.pid")" 2>/dev/null; then
    echo "UNAVAILABLE $daemon: daemon stopped before YANG registration"
  else
    queried=0
    found=0
    i=0
    while [ "$i" -lt 50 ]; do
      i=$((i + 1))
      if "$probe" "$run_dir/mgmtd_fe.sock" --operational \
          /ietf-yang-library:yang-library >"$library_xml" 2>/dev/null; then
        queried=1
        if grep -q "<name[^>]*>$module</name>" "$library_xml"; then
          found=1
          break
        fi
      fi
      sleep 0.1
    done
    if [ "$queried" -eq 0 ]; then
      echo "FAIL $daemon: could not read the live YANG Library" >&2
      return 1
    fi
    if [ "$found" -eq 1 ]; then
      advertised=$((advertised + 1))
      echo "ADVERTISED $daemon: $module"
    else
      echo "UNSUPPORTED $daemon: $module is not in the live YANG Library"
    fi
  fi

  for process in "$daemon" zebra mgmtd; do
    if [ -s "$run_dir/$process.pid" ]; then
      kill "$(cat "$run_dir/$process.pid")" 2>/dev/null || true
    fi
  done
  rm -rf -- "$run_dir" "$state_dir"
  rm -f -- "$mgmtd_log" "$zebra_log" "$daemon_log" "$library_xml" \
    "$daemon_copy"
  run_dir=
  state_dir=
  mgmtd_log=
  zebra_log=
  daemon_log=
  library_xml=
  daemon_copy=
}

inventory_daemon bfdd frr-bfdd
inventory_daemon eigrpd frr-eigrpd
inventory_daemon isisd frr-isisd
inventory_daemon ospfd frr-ospfd
inventory_daemon pathd frr-pathd
inventory_daemon pimd frr-pim
inventory_daemon ripd frr-ripd
inventory_daemon ripngd frr-ripngd
inventory_daemon vrrpd frr-vrrpd

[ "$tested" -ne 0 ] || {
  echo "SKIP: no optional FRR protocol daemon is installed"
  exit 77
}
echo "FRR optional backend inventory: $advertised of $tested advertised"

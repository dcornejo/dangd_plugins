#!/bin/sh
# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

# Exercise reversible RIP and RIPng instance-only transactions. No interface,
# network, neighbor, address, packet, or route is created by this fixture.
set -eu

if [ "$#" -ne 2 ] || [ "$(id -u)" -ne 0 ]; then
  echo "usage: sudo $0 FRR_MUTATION_CHECK FRR_SESSION_CHECK" >&2
  exit 2
fi

mutation_probe=$1
session_probe=$2
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
    echo "SKIP: native FRR RIP test supports Linux and FreeBSD only"
    exit 77
    ;;
esac

run_dir=
state_dir=
mgmtd_log=
zebra_log=
protocol_log=
protocol_copy=

cleanup() {
  status=$?
  if [ -n "$run_dir" ]; then
    for process in "${daemon:-}" zebra mgmtd; do
      if [ -n "$process" ] && [ -s "$run_dir/$process.pid" ]; then
        kill "$(cat "$run_dir/$process.pid")" 2>/dev/null || true
      fi
    done
    if [ "$status" -ne 0 ] && [ "$status" -ne 77 ]; then
      for daemon_log in "$mgmtd_log" "$zebra_log" "$protocol_log"; do
        [ ! -s "$daemon_log" ] || {
          echo "--- $daemon_log" >&2
          tail -100 "$daemon_log" >&2
        }
      done
    fi
    rm -rf -- "$run_dir" "$state_dir"
    rm -f -- "$mgmtd_log" "$zebra_log" "$protocol_log" "$protocol_copy"
  fi
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

exercise_protocol() {
  daemon=$1
  module=$2
  root=$3
  xml=$4
  [ -x "$frr_dir/$daemon" ] || {
    echo "SKIP: $daemon is not installed"
    exit 77
  }

  tag="dangd-test-${daemon}-$$"
  run_dir="/var/run/frr/$tag"
  state_dir="/var/lib/frr/$tag"
  mgmtd_log="/tmp/$tag-mgmtd.log"
  zebra_log="/tmp/$tag-zebra.log"
  protocol_log="/tmp/$tag-$daemon.log"
  protocol_copy="/tmp/$tag-$daemon"

  install -d -o frr -g "$group" -m 0770 "$run_dir" "$state_dir"
  for log in "$mgmtd_log" "$zebra_log" "$protocol_log"; do
    install -o frr -g "$group" -m 0660 /dev/null "$log"
  done
  for process in mgmtd zebra "$daemon"; do
    install -o frr -g "$group" -m 0660 /dev/null "$run_dir/$process.pid"
  done

  protocol_command="$frr_dir/$daemon"
  if [ "$copy_daemon" -eq 1 ]; then
    # Avoid changing Ubuntu's packaged AppArmor policy for test pathspaces.
    cp "$frr_dir/$daemon" "$protocol_copy"
    chmod 0755 "$protocol_copy"
    protocol_command=$protocol_copy
  fi

  "$frr_dir/mgmtd" -N "$tag" -d -u frr -g "$group" \
    -i "$run_dir/mgmtd.pid" --log "file:$mgmtd_log" >/dev/null 2>&1
  "$frr_dir/zebra" -N "$tag" -d -u frr -g "$group" \
    -i "$run_dir/zebra.pid" --log "file:$zebra_log" >/dev/null 2>&1
  "$protocol_command" -N "$tag" -d -u frr -g "$group" \
    -i "$run_dir/$daemon.pid" --log "file:$protocol_log" >/dev/null 2>&1

  i=0
  while [ ! -S "$run_dir/mgmtd_fe.sock" ]; do
    i=$((i + 1))
    [ "$i" -lt 50 ] || {
      echo "isolated mgmtd did not start" >&2
      return 1
    }
    sleep 0.1
  done
  i=0
  while :; do
    i=$((i + 1))
    if "$session_probe" "$run_dir/mgmtd_fe.sock" --operational \
        /ietf-yang-library:yang-library 2>/dev/null |
        grep -q "<name[^>]*>$module</name>"; then
      break
    fi
    [ "$i" -lt 50 ] || {
      echo "$daemon did not expose $module through mgmtd" >&2
      return 1
    }
    sleep 0.1
  done

  "$mutation_probe" --allow-isolated-test "$run_dir/mgmtd_fe.sock" \
    "$root" "$xml" '<vrf>default</vrf>'

  for process in "$daemon" zebra mgmtd; do
    if [ -s "$run_dir/$process.pid" ]; then
      kill "$(cat "$run_dir/$process.pid")" 2>/dev/null || true
    fi
  done
  rm -rf -- "$run_dir" "$state_dir"
  rm -f -- "$mgmtd_log" "$zebra_log" "$protocol_log" "$protocol_copy"
  run_dir=
  state_dir=
  mgmtd_log=
  zebra_log=
  protocol_log=
  protocol_copy=
}

rip_xml='<ripd xmlns="http://frrouting.org/yang/ripd"><instance><vrf>default</vrf></instance></ripd>'
ripng_xml='<ripngd xmlns="http://frrouting.org/yang/ripngd"><instance><vrf>default</vrf></instance></ripngd>'
exercise_protocol ripd frr-ripd /frr-ripd:ripd "$rip_xml"
exercise_protocol ripngd frr-ripngd /frr-ripngd:ripngd "$ripng_xml"

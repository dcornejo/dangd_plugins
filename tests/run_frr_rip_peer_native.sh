#!/bin/sh
# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

# Run concurrently on two isolated Linux/FreeBSD peers. Each peer advertises a
# temporary loopback prefix and verifies FRR's native neighbor and learned-route
# state. FRR access uses the programmatic mgmtd client; platform commands only
# create and remove the disposable loopback address used by this test.
set -eu

usage() {
  echo "usage: sudo $0 --allow-private-lan-test SESSION_CHECK INTERFACE LAN_PREFIX LOCAL_LOOPBACK PEER_ADDRESS PEER_LOOPBACK" >&2
  exit 2
}

[ "$#" -eq 7 ] || usage
[ "$1" = "--allow-private-lan-test" ] || usage
[ "$(id -u)" -eq 0 ] || usage
session_probe=$2
interface=$3
lan_prefix=$4
local_loopback=$5
peer_address=$6
peer_loopback=$7
case "$lan_prefix:$local_loopback:$peer_address:$peer_loopback" in
  *[!0-9./:a-fA-F]*) usage ;;
esac
case "$local_loopback:$peer_loopback" in
  */32:*/32) ;;
  *) echo "RIP peer test loopbacks must be IPv4 /32 prefixes" >&2; exit 2 ;;
esac
[ -x "$session_probe" ] || {
  echo "FRR mgmtd session probe is not executable: $session_probe" >&2
  exit 2
}

tag="dang-notify-rip-peer-$$"
run_dir="/var/run/frr/$tag"
state_dir="/var/lib/frr/$tag"
mgmtd_log="/tmp/$tag-mgmtd.log"
zebra_log="/tmp/$tag-zebra.log"
ripd_log="/tmp/$tag-ripd.log"
ripd_copy=
loopback_added=0

case "$(uname -s)" in
  Linux) frr_dir=/usr/lib/frr; group=frrvty; loopback=lo ;;
  FreeBSD)
    frr_dir=/usr/local/sbin; group=frr; loopback=lo0
    export PATH="/usr/local/bin:/usr/local/sbin:$PATH"
    ;;
  *) echo "SKIP: native FRR RIP peer test supports Linux and FreeBSD only"; exit 77 ;;
esac

cleanup() {
  result=$?
  for process in ripd zebra mgmtd; do
    if [ -s "$run_dir/$process.pid" ]; then
      kill "$(cat "$run_dir/$process.pid")" 2>/dev/null || true
    fi
  done
  if [ "$loopback_added" -eq 1 ]; then
    case "$(uname -s)" in
      Linux) ip address delete "$local_loopback" dev "$loopback" 2>/dev/null || true ;;
      FreeBSD) ifconfig "$loopback" -alias "${local_loopback%/*}" 2>/dev/null || true ;;
    esac
  fi
  if [ "$result" -ne 0 ] && [ "$result" -ne 77 ]; then
    for log in "$mgmtd_log" "$zebra_log" "$ripd_log"; do
      [ ! -s "$log" ] || { echo "--- $log" >&2; tail -100 "$log" >&2; }
    done
  fi
  rm -rf -- "$run_dir" "$state_dir"
  rm -f -- "$mgmtd_log" "$zebra_log" "$ripd_log" "$ripd_copy"
  exit "$result"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

case "$(uname -s)" in
  Linux)
    ip link show dev "$interface" >/dev/null 2>&1 || {
      echo "test interface does not exist: $interface" >&2; exit 2;
    }
    ip address add "$local_loopback" dev "$loopback"
    ;;
  FreeBSD)
    ifconfig "$interface" >/dev/null 2>&1 || {
      echo "test interface does not exist: $interface" >&2; exit 2;
    }
    ifconfig "$loopback" alias "$local_loopback"
    ;;
esac
loopback_added=1

install -d -o frr -g "$group" -m 0770 "$run_dir" "$state_dir"
for log in "$mgmtd_log" "$zebra_log" "$ripd_log"; do
  install -o frr -g "$group" -m 0660 /dev/null "$log"
done
for process in mgmtd zebra ripd; do
  install -o frr -g "$group" -m 0660 /dev/null "$run_dir/$process.pid"
done

ripd_command="$frr_dir/ripd"
if [ "$(uname -s)" = Linux ]; then
  # Ubuntu's packaged AppArmor profile does not admit arbitrary pathspaces for
  # the installed pathname. A private binary copy keeps this test isolated.
  ripd_copy="/tmp/$tag-ripd"
  cp "$ripd_command" "$ripd_copy"
  chmod 0755 "$ripd_copy"
  ripd_command=$ripd_copy
fi

"$frr_dir/mgmtd" -N "$tag" -d -u frr -g "$group" \
  -i "$run_dir/mgmtd.pid" --log "file:$mgmtd_log" >/dev/null 2>&1
"$frr_dir/zebra" -N "$tag" -d -u frr -g "$group" \
  -i "$run_dir/zebra.pid" --log "file:$zebra_log" >/dev/null 2>&1
"$ripd_command" -N "$tag" -d -u frr -g "$group" \
  -i "$run_dir/ripd.pid" --log "file:$ripd_log" >/dev/null 2>&1

attempt=0
while [ ! -S "$run_dir/mgmtd_fe.sock" ]; do
  attempt=$((attempt + 1))
  [ "$attempt" -lt 50 ] || { echo "isolated mgmtd did not start" >&2; exit 1; }
  sleep 0.1
done
attempt=0
while :; do
  attempt=$((attempt + 1))
  if "$session_probe" "$run_dir/mgmtd_fe.sock" --operational \
      /ietf-yang-library:yang-library 2>/dev/null |
      grep -q '<name[^>]*>frr-ripd</name>'; then
    break
  fi
  [ "$attempt" -lt 50 ] || { echo "ripd did not register with mgmtd" >&2; exit 1; }
  sleep 0.1
done

configuration='<ripd xmlns="http://frrouting.org/yang/ripd"><instance><vrf>default</vrf><network>'"$lan_prefix"'</network><network>'"$local_loopback"'</network></instance></ripd>'
"$session_probe" "$run_dir/mgmtd_fe.sock" --replace \
  /frr-ripd:ripd "$configuration" >/dev/null

attempt=0
while :; do
  attempt=$((attempt + 1))
  state=$("$session_probe" "$run_dir/mgmtd_fe.sock" --operational /frr-ripd:ripd)
  learned_route='<route><prefix>'"$peer_loopback"'</prefix><nexthops><nexthop><nh-type>ip4</nh-type><protocol>rip</protocol>'
  if printf '%s' "$state" | grep -Fq "<address>$peer_address</address>" &&
     printf '%s' "$state" | grep -Fq "$learned_route"; then
    printf '%s\n' "$state"
    echo "PASS: learned $peer_loopback from RIP neighbor $peer_address on $interface"
    exit 0
  fi
  [ "$attempt" -lt 60 ] || {
    echo "RIP neighbor or learned route did not appear" >&2
    printf '%s\n' "$state" >&2
    exit 1
  }
  sleep 2
done

#!/bin/sh
# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

# Run concurrently on two isolated Linux/FreeBSD peers. Each peer receives a
# temporary IPv6 address on the explicitly named private interface, advertises
# a temporary loopback /128, and checks FRR's native RIPng operational data.
# FRR access uses the programmatic mgmtd client; platform commands are confined
# to creating and removing the disposable addresses required by the test.
set -eu

usage() {
  echo "usage: sudo $0 --allow-private-lan-test clear|hold SESSION_CHECK INTERFACE LOCAL_ADDRESS LAN_PREFIX LOCAL_LOOPBACK PEER_LOOPBACK" >&2
  exit 2
}

[ "$#" -eq 8 ] || usage
[ "$1" = "--allow-private-lan-test" ] || usage
[ "$(id -u)" -eq 0 ] || usage
role=$2
[ "$role" = clear ] || [ "$role" = hold ] || usage
session_probe=$3
interface=$4
local_address=$5
lan_prefix=$6
local_loopback=$7
peer_loopback=$8
case "$local_address:$lan_prefix:$local_loopback:$peer_loopback" in
  *[!0-9/:a-fA-F]*) usage ;;
esac
case "$local_address:$lan_prefix:$local_loopback:$peer_loopback" in
  *:*/64:*/128:*/128) ;;
  *) echo "RIPng test requires an interface address, a /64 LAN, and /128 loopbacks" >&2; exit 2 ;;
esac
[ -x "$session_probe" ] || {
  echo "FRR mgmtd session probe is not executable: $session_probe" >&2
  exit 2
}

tag="dang-notify-ripng-peer-$$"
run_dir="/var/run/frr/$tag"
state_dir="/var/lib/frr/$tag"
mgmtd_log="/tmp/$tag-mgmtd.log"
zebra_log="/tmp/$tag-zebra.log"
ripngd_log="/tmp/$tag-ripngd.log"
ripngd_copy=
interface_address_added=0
loopback_added=0

case "$(uname -s)" in
  Linux) frr_dir=/usr/lib/frr; group=frrvty; loopback=lo ;;
  FreeBSD)
    frr_dir=/usr/local/sbin; group=frr; loopback=lo0
    export PATH="/usr/local/bin:/usr/local/sbin:$PATH"
    ;;
  *) echo "SKIP: native FRR RIPng peer test supports Linux and FreeBSD only"; exit 77 ;;
esac

cleanup() {
  result=$?
  for process in ripngd zebra mgmtd; do
    if [ -s "$run_dir/$process.pid" ]; then
      kill "$(cat "$run_dir/$process.pid")" 2>/dev/null || true
    fi
  done
  if [ "$loopback_added" -eq 1 ]; then
    case "$(uname -s)" in
      Linux) ip -6 address delete "$local_loopback" dev "$loopback" 2>/dev/null || true ;;
      FreeBSD) ifconfig "$loopback" inet6 "${local_loopback%/*}" delete 2>/dev/null || true ;;
    esac
  fi
  if [ "$interface_address_added" -eq 1 ]; then
    case "$(uname -s)" in
      Linux) ip -6 address delete "$local_address" dev "$interface" 2>/dev/null || true ;;
      FreeBSD) ifconfig "$interface" inet6 "${local_address%/*}" delete 2>/dev/null || true ;;
    esac
  fi
  if [ "$result" -ne 0 ] && [ "$result" -ne 77 ]; then
    for log in "$mgmtd_log" "$zebra_log" "$ripngd_log"; do
      [ ! -s "$log" ] || { echo "--- $log" >&2; tail -100 "$log" >&2; }
    done
  fi
  rm -rf -- "$run_dir" "$state_dir"
  rm -f -- "$mgmtd_log" "$zebra_log" "$ripngd_log" "$ripngd_copy"
  exit "$result"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

case "$(uname -s)" in
  Linux)
    ip link show dev "$interface" >/dev/null 2>&1 || {
      echo "test interface does not exist: $interface" >&2; exit 2;
    }
    ip -6 address add "$local_address" dev "$interface"
    interface_address_added=1
    ip -6 address add "$local_loopback" dev "$loopback"
    ;;
  FreeBSD)
    ifconfig "$interface" >/dev/null 2>&1 || {
      echo "test interface does not exist: $interface" >&2; exit 2;
    }
    ifconfig "$interface" inet6 "$local_address" alias
    interface_address_added=1
    ifconfig "$loopback" inet6 "$local_loopback" alias
    ;;
esac
loopback_added=1

install -d -o frr -g "$group" -m 0770 "$run_dir" "$state_dir"
for log in "$mgmtd_log" "$zebra_log" "$ripngd_log"; do
  install -o frr -g "$group" -m 0660 /dev/null "$log"
done
for process in mgmtd zebra ripngd; do
  install -o frr -g "$group" -m 0660 /dev/null "$run_dir/$process.pid"
done

ripngd_command="$frr_dir/ripngd"
if [ "$(uname -s)" = Linux ]; then
  # Ubuntu's packaged AppArmor profile does not admit arbitrary pathspaces for
  # the installed pathname. A private binary copy keeps this test isolated.
  ripngd_copy="/tmp/$tag-ripngd"
  cp "$ripngd_command" "$ripngd_copy"
  chmod 0755 "$ripngd_copy"
  ripngd_command=$ripngd_copy
fi

"$frr_dir/mgmtd" -N "$tag" -d -u frr -g "$group" \
  -i "$run_dir/mgmtd.pid" --log "file:$mgmtd_log" >/dev/null 2>&1
"$frr_dir/zebra" -N "$tag" -d -u frr -g "$group" \
  -i "$run_dir/zebra.pid" --log "file:$zebra_log" >/dev/null 2>&1
"$ripngd_command" -N "$tag" -d -u frr -g "$group" \
  -i "$run_dir/ripngd.pid" --log "file:$ripngd_log" >/dev/null 2>&1

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
      grep -q '<name[^>]*>frr-ripngd</name>'; then
    break
  fi
  [ "$attempt" -lt 50 ] || { echo "ripngd did not register with mgmtd" >&2; exit 1; }
  sleep 0.1
done

configuration='<ripngd xmlns="http://frrouting.org/yang/ripngd"><instance><vrf>default</vrf><network>'"$lan_prefix"'</network><network>'"$local_loopback"'</network></instance></ripngd>'
"$session_probe" "$run_dir/mgmtd_fe.sock" --replace \
  /frr-ripngd:ripngd "$configuration" >/dev/null

has_learned_state() {
  printf '%s' "$1" | grep -Fq '<neighbor>' &&
    printf '%s' "$1" | grep -Fq "<prefix>$peer_loopback</prefix>"
}

attempt=0
while :; do
  attempt=$((attempt + 1))
  state=$("$session_probe" "$run_dir/mgmtd_fe.sock" --operational /frr-ripngd:ripngd)
  has_learned_state "$state" && break
  [ "$attempt" -lt 60 ] || {
    echo "RIPng neighbor or learned route did not appear" >&2
    printf '%s\n' "$state" >&2
    exit 1
  }
  sleep 2
done

if [ "$role" = hold ]; then
  # The clear peer may finish and remove its disposable addresses before this
  # delay expires. Withdrawal of that peer's route is then correct, so do not
  # require its learned state to remain after the hold interval. Reversing the
  # roles in a second run proves learning and RPC behavior on this endpoint.
  sleep 60
  echo "PASS: held RIPng advertisement for clear-ripng-route peer on $interface"
  exit 0
fi

# A successful RPC acknowledgement is insufficient: the learned route must
# disappear from native state and return after a normal update from the peer.
"$session_probe" "$run_dir/mgmtd_fe.sock" --rpc \
  /frr-ripngd:clear-ripng-route >/dev/null
cleared_state=$("$session_probe" "$run_dir/mgmtd_fe.sock" --operational /frr-ripngd:ripngd)
if printf '%s' "$cleared_state" | grep -Fq "<prefix>$peer_loopback</prefix>"; then
  echo "clear-ripng-route returned success but retained $peer_loopback" >&2
  printf '%s\n' "$cleared_state" >&2
  exit 1
fi

attempt=0
while :; do
  attempt=$((attempt + 1))
  state=$("$session_probe" "$run_dir/mgmtd_fe.sock" --operational /frr-ripngd:ripngd)
  has_learned_state "$state" && break
  [ "$attempt" -lt 60 ] || {
    echo "RIPng route did not return after clear-ripng-route" >&2
    printf '%s\n' "$state" >&2
    exit 1
  }
  sleep 2
done
printf '%s\n' "$state"
echo "PASS: learned, cleared, and relearned $peer_loopback from a RIPng neighbor on $interface"

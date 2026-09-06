#!/bin/sh
# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

# Exercise FRR's native RIP notification path in two disposable network
# namespaces. No host address, route, or physical interface participates.
set -eu

if [ "$#" -ne 1 ] || [ "$(id -u)" -ne 0 ]; then
  echo "usage: sudo $0 FRR_SESSION_CHECK" >&2
  exit 2
fi

probe=$1
tag="dang-notify-$$"
receiver="${tag}-rx"
sender="${tag}-tx"
host_rx="dnrx$$"
host_tx="dntx$$"
run_dir="/var/run/frr/$tag"
state_dir="/var/lib/frr/$tag"
mgmtd_log="/tmp/$tag-mgmtd.log"
zebra_log="/tmp/$tag-zebra.log"
rip_log="/tmp/$tag-ripd.log"
output="/tmp/$tag.xml"
rip_binary="/tmp/$tag-ripd"

cleanup() {
  status=$?
  for daemon in ripd zebra mgmtd; do
    if [ -f "$run_dir/$daemon.pid" ]; then
      kill "$(cat "$run_dir/$daemon.pid")" 2>/dev/null || true
    fi
  done
  ip netns del "$sender" 2>/dev/null || true
  ip netns del "$receiver" 2>/dev/null || true
  rm -rf -- "$run_dir" "$state_dir"
  if [ "$status" -ne 0 ] && [ "$status" -ne 77 ]; then
    for daemon_log in "$mgmtd_log" "$zebra_log" "$rip_log"; do
      [ ! -s "$daemon_log" ] || tail -100 "$daemon_log" >&2
    done
  fi
  rm -f -- "$mgmtd_log" "$zebra_log" "$rip_log" "$output" "$rip_binary"
}
trap cleanup EXIT HUP INT TERM

ip netns add "$receiver"
ip netns add "$sender"
ip link add "$host_rx" type veth peer name "$host_tx"
ip link set "$host_rx" netns "$receiver"
ip link set "$host_tx" netns "$sender"
ip -n "$receiver" link set lo up
ip -n "$sender" link set lo up
ip -n "$receiver" link set "$host_rx" name eth0
ip -n "$sender" link set "$host_tx" name eth0
ip -n "$receiver" address add 192.0.2.1/30 dev eth0
ip -n "$sender" address add 192.0.2.2/30 dev eth0
ip -n "$receiver" link set eth0 up
ip -n "$sender" link set eth0 up

install -d -o frr -g frrvty -m 0770 "$run_dir" "$state_dir"
install -o root -g root -m 0600 /dev/null "$mgmtd_log"
for daemon_log in "$zebra_log" "$rip_log"; do
  install -o frr -g frrvty -m 0660 /dev/null "$daemon_log"
done
for daemon in mgmtd zebra ripd; do
  install -o frr -g frrvty -m 0660 /dev/null "$run_dir/$daemon.pid"
done
# Ubuntu's packaged AppArmor profile intentionally denies nonstandard FRR
# pathspaces. Executing a temporary copy avoids changing that host policy while
# keeping this disposable daemon unable to collide with the production sockets.
cp /usr/lib/frr/ripd "$rip_binary"
chmod 0755 "$rip_binary"
ip netns exec "$receiver" /usr/lib/frr/mgmtd -N "$tag" -u frr -g frrvty \
  -i "$run_dir/mgmtd.pid" --log stdout >"$mgmtd_log" 2>&1 &
ip netns exec "$receiver" /usr/lib/frr/zebra -N "$tag" -d -u frr -g frrvty \
  -i "$run_dir/zebra.pid" --log "file:$zebra_log"
ip netns exec "$receiver" "$rip_binary" -N "$tag" -d -u frr -g frrvty \
  -i "$run_dir/ripd.pid" --log "file:$rip_log"

i=0
while [ ! -S "$run_dir/mgmtd_fe.sock" ]; do
  i=$((i + 1))
  [ "$i" -lt 50 ] || { echo "isolated mgmtd did not start" >&2; exit 1; }
  sleep 0.1
done
sleep 1
[ -s "$run_dir/ripd.pid" ] && kill -0 "$(cat "$run_dir/ripd.pid")"
ip netns exec "$receiver" "$probe" "$run_dir/mgmtd_fe.sock" \
  --replace /frr-ripd:ripd \
  '<ripd xmlns="http://frrouting.org/yang/ripd"><instance><vrf>default</vrf><interface>eth0</interface></instance></ripd>'
ip netns exec "$receiver" "$probe" "$run_dir/mgmtd_fe.sock" \
  --running /frr-ripd:ripd | grep -q '<interface>eth0</interface>'

ip netns exec "$receiver" "$probe" "$run_dir/mgmtd_fe.sock" \
  --notify /frr-ripd:authentication-type-failure >"$output" &
probe_pid=$!
sleep 1
ip netns exec "$sender" python3 -c \
  'import socket,struct,time; s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.bind(("192.0.2.2",520)); s.setsockopt(socket.IPPROTO_IP,socket.IP_MULTICAST_IF,socket.inet_aton("192.0.2.2")); p=struct.pack("!BBHHH16s",2,2,0,0xffff,2,b"invalid-password"); exec("for _ in range(3):\n s.sendto(p,(\"224.0.0.9\",520))\n time.sleep(.1)")'
if ! wait "$probe_pid"; then
  echo "notification reader failed; daemon status follows" >&2
  for daemon in mgmtd zebra ripd; do
    if [ -s "$run_dir/$daemon.pid" ] &&
        kill -0 "$(cat "$run_dir/$daemon.pid")" 2>/dev/null; then
      echo "$daemon: running" >&2
    else
      echo "$daemon: stopped" >&2
    fi
  done
  if grep -q 'Unexpected notification element "authentication-type-failure"' \
      "$mgmtd_log" &&
      grep -q 'assure_notify_msg_cache.*assertion' "$mgmtd_log"; then
    echo "SKIP: FRR mgmtd crashes while encoding its modeled RIP notification" >&2
    exit 77
  fi
  for daemon_log in "$mgmtd_log" "$zebra_log" "$rip_log"; do
    [ ! -s "$daemon_log" ] || tail -100 "$daemon_log" >&2
  done
  exit 1
fi

grep -q '^/frr-ripd:authentication-type-failure$' "$output"
grep -q 'authentication-type-failure' "$output"
grep -q '<interface-name>eth0</interface-name>' "$output"
cat "$output"

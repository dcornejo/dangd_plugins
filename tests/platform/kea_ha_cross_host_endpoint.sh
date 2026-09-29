#!/bin/sh
# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

# Runs one disposable member of a real two-host Kea hot-standby pair. The
# orchestrator supplies only documentation-prefix addresses on an explicitly
# selected non-management interface. No persistent Kea service or host route is
# modified.

set -eu

action=${1:-}
interface=${2:-}
runtime=/tmp/dang-kea-ha-cross-${interface}
port4=1067
port6=1547
control_port4=18000
control_port6=18001

fail() {
  echo "kea HA cross-host endpoint: $*" >&2
  exit 1
}

[ "$(id -u)" -eq 0 ] || fail "must run as root"
[ -n "$interface" ] || fail "an explicit interface is required"

case "$(uname -s)" in
  Linux)
    ip link show dev "$interface" >/dev/null 2>&1 || fail "unknown interface $interface"
    if ip route show default | grep -Eq "(^| )dev $interface( |$)"; then
      fail "$interface carries the IPv4 default route"
    fi
    if ip -6 route show default | grep -Eq "(^| )dev $interface( |$)"; then
      fail "$interface carries the IPv6 default route"
    fi
    hook_dir=/usr/lib/x86_64-linux-gnu/kea/hooks
    socket_dir=/run/kea
    dhcp4=$(command -v kea-dhcp4)
    dhcp6=$(command -v kea-dhcp6)
    python=$(command -v python3 || command -v python3.12 || command -v python3.11)
    client_mac=$(cat "/sys/class/net/$interface/address")
    add_addresses() {
      ip link set "$interface" up
      ip address add "$local4/24" dev "$interface"
      ip -6 address add "$local6/64" dev "$interface"
    }
    remove_addresses() {
      [ -n "${local4:-}" ] &&
        ip address del "$local4/24" dev "$interface" 2>/dev/null || true
      [ -n "${local6:-}" ] &&
        ip -6 address del "$local6/64" dev "$interface" 2>/dev/null || true
    }
    addresses_are_free() {
      ! ip -o address show dev "$interface" |
        awk '{print $4}' | cut -d/ -f1 |
        grep -Eq "^($local4|$local6)$"
    }
    ;;
  FreeBSD)
    ifconfig "$interface" >/dev/null 2>&1 || fail "unknown interface $interface"
    if netstat -rn -f inet | awk '$1 == "default" {print $NF}' |
       grep -Fxq "$interface"; then
      fail "$interface carries the IPv4 default route"
    fi
    if netstat -rn -f inet6 | awk '$1 == "default" {print $NF}' |
       grep -Fxq "$interface"; then
      fail "$interface carries the IPv6 default route"
    fi
    hook_dir=/usr/local/lib/kea/hooks
    socket_dir=/var/run/kea
    dhcp4=/usr/local/sbin/kea-dhcp4
    dhcp6=/usr/local/sbin/kea-dhcp6
    python=$(command -v python3 || command -v python3.12 || command -v python3.11)
    client_mac=$(ifconfig "$interface" | awk '$1 == "ether" {print $2; exit}')
    add_addresses() {
      ifconfig "$interface" up
      ifconfig "$interface" inet "$local4/24" alias
      ifconfig "$interface" inet6 "$local6/64" alias
    }
    remove_addresses() {
      [ -n "${local4:-}" ] &&
        ifconfig "$interface" inet "$local4" -alias 2>/dev/null || true
      [ -n "${local6:-}" ] &&
        ifconfig "$interface" inet6 "$local6" -alias 2>/dev/null || true
    }
    addresses_are_free() {
      ! ifconfig "$interface" |
        awk '$1 == "inet" || $1 == "inet6" {print $2}' |
        grep -Eq "^($local4|$local6)$"
    }
    ;;
  *) fail "unsupported operating system" ;;
esac

socket4=$socket_dir/dang-ha-cross-${interface}-4.sock
socket6=$socket_dir/dang-ha-cross-${interface}-6.sock

read_runtime_address() {
  file=$1
  [ -f "$runtime/$file" ] && cat "$runtime/$file" || true
}

stop_server() {
  local4=$(read_runtime_address local4)
  local6=$(read_runtime_address local6)
  if [ -f "$runtime/pid4" ]; then
    kill "$(cat "$runtime/pid4")" 2>/dev/null || true
  fi
  if [ -f "$runtime/pid6" ]; then
    kill "$(cat "$runtime/pid6")" 2>/dev/null || true
  fi
  remove_addresses
  rm -f "$socket4" "$socket6"
  rm -rf "$runtime"
}

halt_server() {
  # Keep the test aliases and runtime files so this host can act as the second
  # client after its primary daemons have been proven fully stopped.
  for pid_file in "$runtime/pid4" "$runtime/pid6"; do
    if [ -f "$pid_file" ]; then
      pid=$(cat "$pid_file")
      kill "$pid" 2>/dev/null || true
      attempt=0
      while kill -0 "$pid" 2>/dev/null; do
        attempt=$((attempt + 1))
        [ "$attempt" -lt 50 ] || fail "Kea process $pid did not stop"
        sleep 0.1
      done
      if kill -0 "$pid" 2>/dev/null; then
        fail "Kea process $pid did not stop"
      fi
      rm -f "$pid_file"
    fi
  done
  rm -f "$socket4" "$socket6"
}

start_daemons() {
  KEA_PIDFILE_DIR="$runtime" "$dhcp4" -d -p "$port4" -P 1068 \
    -c "$runtime/kea4.json" >>"$runtime/kea4.stdout" 2>&1 &
  echo $! >"$runtime/pid4"
  KEA_PIDFILE_DIR="$runtime" "$dhcp6" -d -p "$port6" -P 1546 \
    -c "$runtime/kea6.json" >>"$runtime/kea6.stdout" 2>&1 &
  echo $! >"$runtime/pid6"
  attempt=0
  while [ ! -S "$socket4" ] || [ ! -S "$socket6" ]; do
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 100 ]; then
      cat "$runtime/kea4.stdout" "$runtime/kea6.stdout" 2>/dev/null || true
      fail "Kea did not create both control sockets"
    fi
    sleep 0.1
  done
}

case "$action" in
  start)
    # start IFACE LOCAL4 LOCAL6 LOCAL_NAME LOCAL_ROLE REMOTE4 REMOTE6
    #       REMOTE_NAME REMOTE_ROLE
    [ "$#" -eq 10 ] || fail "invalid start arguments"
    local4=$3
    local6=$4
    local_name=$5
    local_role=$6
    remote4=$7
    remote6=$8
    remote_name=$9
    remote_role=${10}
    case "$local4:$remote4:$local6:$remote6" in
      192.0.2.1:192.0.2.2:2001:db8:6::1:2001:db8:6::2 | \
      192.0.2.2:192.0.2.1:2001:db8:6::2:2001:db8:6::1) ;;
      *) fail "addresses must be the two documented sterile-VLAN endpoints" ;;
    esac
    case "$local_name:$remote_name" in
      linux:freebsd | freebsd:linux) ;;
      *) fail "member names must identify the Linux and FreeBSD endpoints" ;;
    esac
    case "$local_role:$remote_role" in
      primary:standby)
        primary_name=$local_name
        primary4=$local4
        standby_name=$remote_name
        standby4=$remote4
        ;;
      standby:primary)
        primary_name=$remote_name
        primary4=$remote4
        standby_name=$local_name
        standby4=$local4
        ;;
      *) fail "roles must be one primary and one standby" ;;
    esac
    [ ! -e "$runtime" ] || fail "stale runtime exists at $runtime"
    [ ! -e "$socket4" ] && [ ! -e "$socket6" ] ||
      fail "a test control socket already exists"
    addresses_are_free || fail "a test address already exists on $interface"
    mkdir -p "$runtime"
    printf '%s\n' "$local4" >"$runtime/local4"
    printf '%s\n' "$local6" >"$runtime/local6"
    trap 'stop_server' EXIT INT TERM
    add_addresses
    if [ "$(uname -s)" = Linux ]; then
      # The packaged Ubuntu path has service-specific AppArmor policy. Private
      # executable paths isolate these test daemons without changing policy.
      cp "$dhcp4" "$runtime/kea-dhcp4"
      cp "$dhcp6" "$runtime/kea-dhcp6"
      dhcp4=$runtime/kea-dhcp4
      dhcp6=$runtime/kea-dhcp6
    fi
    cat >"$runtime/kea4.json" <<EOF
{"Dhcp4":{
  "interfaces-config":{"interfaces":["$interface"],"dhcp-socket-type":"udp"},
  "control-sockets":[{"socket-type":"unix","socket-name":"$socket4"}],
  "multi-threading":{"enable-multi-threading":true,"thread-pool-size":2,
                     "packet-queue-size":64},
  "hooks-libraries":[
    {"library":"$hook_dir/libdhcp_lease_cmds.so"},
    {"library":"$hook_dir/libdhcp_ha.so","parameters":{
      "high-availability":[{
        "this-server-name":"$local_name","mode":"hot-standby",
        "heartbeat-delay":1000,"max-response-delay":3000,
        "multi-threading":{"enable-multi-threading":true,
                           "http-dedicated-listener":true,
                           "http-listener-threads":1,
                           "http-client-threads":1},
        "peers":[
          {"name":"$primary_name","url":"http://$primary4:$control_port4/",
           "role":"primary","auto-failover":false},
          {"name":"$standby_name","url":"http://$standby4:$control_port4/",
           "role":"standby","auto-failover":false}
        ]
      }]
    }}
  ],
  "lease-database":{"type":"memfile","persist":false},
  "subnet4":[{"id":9401,"subnet":"192.0.2.0/24",
              "pools":[{"pool":"192.0.2.100 - 192.0.2.120"}]}],
  "valid-lifetime":600,
  "loggers":[{"name":"kea-dhcp4","severity":"INFO",
              "output-options":[{"output":"stderr"}]}]
}}
EOF
    cat >"$runtime/kea6.json" <<EOF
{"Dhcp6":{
  "interfaces-config":{"interfaces":["$interface"]},
  "control-sockets":[{"socket-type":"unix","socket-name":"$socket6"}],
  "multi-threading":{"enable-multi-threading":true,"thread-pool-size":2,
                     "packet-queue-size":64},
  "hooks-libraries":[
    {"library":"$hook_dir/libdhcp_lease_cmds.so"},
    {"library":"$hook_dir/libdhcp_ha.so","parameters":{
      "high-availability":[{
        "this-server-name":"$local_name","mode":"hot-standby",
        "heartbeat-delay":1000,"max-response-delay":3000,
        "multi-threading":{"enable-multi-threading":true,
                           "http-dedicated-listener":true,
                           "http-listener-threads":1,
                           "http-client-threads":1},
        "peers":[
          {"name":"$primary_name","url":"http://$primary4:$control_port6/",
           "role":"primary","auto-failover":false},
          {"name":"$standby_name","url":"http://$standby4:$control_port6/",
           "role":"standby","auto-failover":false}
        ]
      }]
    }}
  ],
  "lease-database":{"type":"memfile","persist":false},
  "subnet6":[{"id":9601,"rapid-commit":true,
              "subnet":"2001:db8:6::/64",
              "pools":[{"pool":"2001:db8:6::100 - 2001:db8:6::120"}]}],
  "preferred-lifetime":300,"valid-lifetime":600,
  "loggers":[{"name":"kea-dhcp6","severity":"INFO",
              "output-options":[{"output":"stderr"}]}]
}}
EOF
    : >"$runtime/kea4.stdout"
    : >"$runtime/kea6.stdout"
    start_daemons
    trap - EXIT INT TERM
    echo "$local_name Kea HA member is ready on $interface"
    ;;
  resume)
    [ "$#" -eq 2 ] || fail "invalid resume arguments"
    [ -d "$runtime" ] || fail "no halted Kea runtime exists"
    [ ! -e "$runtime/pid4" ] && [ ! -e "$runtime/pid6" ] ||
      fail "Kea runtime still has active process identifiers"
    [ ! -e "$socket4" ] && [ ! -e "$socket6" ] ||
      fail "a test control socket still exists"
    [ -f "$runtime/kea4.json" ] && [ -f "$runtime/kea6.json" ] ||
      fail "halted Kea configuration is incomplete"
    if [ "$(uname -s)" = Linux ]; then
      dhcp4=$runtime/kea-dhcp4
      dhcp6=$runtime/kea-dhcp6
      [ -x "$dhcp4" ] && [ -x "$dhcp6" ] ||
        fail "halted Linux Kea executables are incomplete"
    fi
    start_daemons
    echo "Kea primary daemons resumed on $interface"
    ;;
  ready)
    # ready IFACE LOCAL_NAME REMOTE_NAME LOCAL_ROLE PROBE
    [ "$#" -eq 6 ] || fail "invalid ready arguments"
    "$python" "$6" ready "$socket4" "$3" "$4" "$5"
    "$python" "$6" ready "$socket6" "$3" "$4" "$5"
    echo "$3 reaches $4 for DHCPv4 and DHCPv6"
    ;;
  client)
    # client IFACE LOCAL4 LOCAL6 SERVER4 SERVER6 CLIENT
    #        [REQUEST4 EXPECTED6 [CLIENT_MAC]]
    [ "$#" -eq 7 ] || [ "$#" -eq 9 ] || [ "$#" -eq 10 ] ||
      fail "invalid client arguments"
    if [ "$#" -ge 9 ]; then
      [ "$#" -eq 9 ] || client_mac=${10}
      "$python" "$7" "$5" "$6" "$interface" "$client_mac" "$3" "$4" \
        "$8" "$9"
    else
      "$python" "$7" "$5" "$6" "$interface" "$client_mac" "$3" "$4"
    fi
    echo "HA client exchanges were sent from $interface"
    ;;
  verify)
    # verify IFACE PROBE ADDRESS4 ADDRESS6
    [ "$#" -eq 5 ] || fail "invalid verify arguments"
    "$python" "$3" leases "$socket4" "$socket6" "$4" "$5"
    echo "local DHCPv4 and DHCPv6 lease databases contain the HA leases"
    ;;
  takeover)
    # takeover IFACE LOCAL_NAME PRIMARY_NAME PROBE
    [ "$#" -eq 5 ] || fail "invalid takeover arguments"
    "$python" "$5" takeover "$socket4" "$3" "$4"
    "$python" "$5" takeover "$socket6" "$3" "$4"
    echo "$3 now serves the stopped primary $4 scope"
    ;;
  relinquish)
    # relinquish IFACE LOCAL_NAME PROBE
    [ "$#" -eq 4 ] || fail "invalid relinquish arguments"
    "$python" "$4" relinquish "$socket4" "$3"
    "$python" "$4" relinquish "$socket6" "$3"
    echo "$3 relinquished the manually assigned primary scope"
    ;;
  halt)
    halt_server
    echo "Kea primary daemons stopped; test interface retained"
    ;;
  logs)
    grep -E 'HA_STATE_TRANSITION|HA_LOCAL_DHCP|HA_LEASE|LEASE_ALLOC|DROP|ERROR' \
      "$runtime/kea4.stdout" "$runtime/kea6.stdout" 2>/dev/null | tail -100 || true
    ;;
  stop)
    stop_server
    ;;
  *)
    fail "unknown action $action"
    ;;
esac

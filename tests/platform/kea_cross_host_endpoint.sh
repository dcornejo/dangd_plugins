#!/bin/sh
# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

set -eu

action=${1:-}
interface=${2:-}
client_program=${3:-/tmp/dang-kea-cross-client.py}
runtime=/tmp/dang-kea-cross-${interface}
address4=192.0.2.1
address6=2001:db8:6::1
client_address4=192.0.2.2
client_address6=2001:db8:6::2
port4=1067
port6=1547

fail() {
  echo "kea cross-host endpoint: $*" >&2
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
    dhcp4=$(command -v kea-dhcp4)
    dhcp6=$(command -v kea-dhcp6)
    python=$(command -v python3 || command -v python3.12 || command -v python3.11)
    client_mac=$(cat "/sys/class/net/$interface/address")
    add_addresses() {
      ip link set "$interface" up
      ip address add "$address4/24" dev "$interface"
      ip -6 address add "$address6/64" dev "$interface"
    }
    remove_addresses() {
      ip address del "$address4/24" dev "$interface" 2>/dev/null || true
      ip -6 address del "$address6/64" dev "$interface" 2>/dev/null || true
    }
    add_client_addresses() {
      ip link set "$interface" up
      ip address add "$client_address4/24" dev "$interface"
      ip -6 address add "$client_address6/64" dev "$interface"
    }
    remove_client_addresses() {
      ip address del "$client_address4/24" dev "$interface" 2>/dev/null || true
      ip -6 address del "$client_address6/64" dev "$interface" 2>/dev/null || true
    }
    ;;
  FreeBSD)
    ifconfig "$interface" >/dev/null 2>&1 || fail "unknown interface $interface"
    if netstat -rn -f inet | awk '$1 == "default" {print $NF}' | grep -Fxq "$interface"; then
      fail "$interface carries the IPv4 default route"
    fi
    if netstat -rn -f inet6 | awk '$1 == "default" {print $NF}' | grep -Fxq "$interface"; then
      fail "$interface carries the IPv6 default route"
    fi
    hook_dir=/usr/local/lib/kea/hooks
    dhcp4=/usr/local/sbin/kea-dhcp4
    dhcp6=/usr/local/sbin/kea-dhcp6
    python=$(command -v python3 || command -v python3.12 || command -v python3.11)
    client_mac=$(ifconfig "$interface" | awk '$1 == "ether" {print $2; exit}')
    add_addresses() {
      ifconfig "$interface" up
      ifconfig "$interface" inet "$address4/24" alias
      ifconfig "$interface" inet6 "$address6/64" alias
    }
    remove_addresses() {
      ifconfig "$interface" inet "$address4" -alias 2>/dev/null || true
      ifconfig "$interface" inet6 "$address6" -alias 2>/dev/null || true
    }
    add_client_addresses() {
      ifconfig "$interface" up
      ifconfig "$interface" inet "$client_address4/24" alias
      ifconfig "$interface" inet6 "$client_address6/64" alias
    }
    remove_client_addresses() {
      ifconfig "$interface" inet "$client_address4" -alias 2>/dev/null || true
      ifconfig "$interface" inet6 "$client_address6" -alias 2>/dev/null || true
    }
    ;;
  *) fail "unsupported operating system" ;;
esac

stop_server() {
  if [ -f "$runtime/pid4" ]; then kill "$(cat "$runtime/pid4")" 2>/dev/null || true; fi
  if [ -f "$runtime/pid6" ]; then kill "$(cat "$runtime/pid6")" 2>/dev/null || true; fi
  remove_addresses
  remove_client_addresses
  rm -rf "$runtime"
}

case "$action" in
  start)
    [ ! -e "$runtime" ] || fail "stale runtime exists at $runtime"
    mkdir -p "$runtime"
    trap 'stop_server' EXIT INT TERM
    add_addresses
    if [ "$(uname -s)" = Linux ]; then
      # Ubuntu attaches service-specific AppArmor policy to the packaged path.
      # A private executable pathname keeps this disposable instance separate
      # from the installed service without changing the host policy.
      cp "$dhcp4" "$runtime/kea-dhcp4"
      cp "$dhcp6" "$runtime/kea-dhcp6"
      dhcp4=$runtime/kea-dhcp4
      dhcp6=$runtime/kea-dhcp6
    fi
    cat >"$runtime/kea4.json" <<EOF
{"Dhcp4":{"interfaces-config":{"interfaces":["$interface"],"dhcp-socket-type":"udp"},"lease-database":{"type":"memfile","persist":false},"subnet4":[{"id":9401,"subnet":"192.0.2.0/24","pools":[{"pool":"192.0.2.100-192.0.2.120"}]}],"valid-lifetime":600,"loggers":[{"name":"kea-dhcp4","severity":"INFO","output-options":[{"output":"stderr"}]}]}}
EOF
    cat >"$runtime/kea6.json" <<EOF
{"Dhcp6":{"interfaces-config":{"interfaces":["$interface"]},"lease-database":{"type":"memfile","persist":false},"subnet6":[{"id":9601,"rapid-commit":true,"subnet":"2001:db8:6::/64","pools":[{"pool":"2001:db8:6::100-2001:db8:6::120"}]}],"preferred-lifetime":300,"valid-lifetime":600,"loggers":[{"name":"kea-dhcp6","severity":"INFO","output-options":[{"output":"stderr"}]}]}}
EOF
    KEA_PIDFILE_DIR="$runtime" "$dhcp4" -d -p "$port4" -P 1068 -c "$runtime/kea4.json" >"$runtime/kea4.stdout" 2>&1 &
    echo $! >"$runtime/pid4"
    KEA_PIDFILE_DIR="$runtime" "$dhcp6" -d -p "$port6" -P 1546 -c "$runtime/kea6.json" >"$runtime/kea6.stdout" 2>&1 &
    echo $! >"$runtime/pid6"
    sleep 1
    if ! kill -0 "$(cat "$runtime/pid4")" 2>/dev/null ||
       ! kill -0 "$(cat "$runtime/pid6")" 2>/dev/null; then
      cat "$runtime/kea4.stdout" "$runtime/kea6.stdout" 2>/dev/null || true
      fail "Kea did not become ready"
    fi
    trap - EXIT INT TERM
    echo "Kea is ready on $interface"
    ;;
  client)
    trap 'remove_client_addresses' EXIT INT TERM
    add_client_addresses
    # Allow IPv6 duplicate-address detection to finish before binding the
    # client socket. FreeBSD normally completes immediately; Linux does not.
    sleep 1
    ping -c 1 "$address4" >/dev/null
    "$python" "$client_program" "$address4" "$address6" "$interface" \
      "$client_mac"
    remove_client_addresses
    trap - EXIT INT TERM
    echo "DHCPv4 and DHCPv6 client exchanges passed on $interface"
    ;;
  verify)
    grep -q 'DHCP4_LEASE_ALLOC' "$runtime/kea4.stdout" ||
      fail "no completed DHCPv4 allocation was observed"
    grep -q 'DHCP6_LEASE_ALLOC' "$runtime/kea6.stdout" ||
      fail "no completed DHCPv6 allocation was observed"
    echo "Kea observed completed DHCPv4 and DHCPv6 allocations on $interface"
    ;;
  stop)
    stop_server
    ;;
  *) fail "usage: $0 {start|client|verify|stop} INTERFACE" ;;
esac

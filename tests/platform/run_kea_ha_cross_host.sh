#!/bin/sh
# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

# Proves real Kea hot-standby communication and lease replication between the
# disposable Linux and FreeBSD test endpoints, then reverses their roles.

set -eu

linux_host=${1:-dev-linux-1}
freebsd_host=${2:-dev-freebsd-1}
linux_interface=${3:-ens19}
freebsd_interface=${4:-vtnet1}
endpoint=/tmp/dang-kea-ha-cross-endpoint-$$.sh
client=/tmp/dang-kea-ha-cross-client-$$.py
probe=/tmp/dang-kea-ha-cross-probe-$$.py
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ssh_command=${DANG_TEST_SSH:-ssh}
scp_command=${DANG_TEST_SCP:-scp}
linux4=192.0.2.1
linux6=2001:db8:6::1
freebsd4=192.0.2.2
freebsd6=2001:db8:6::2

remote() {
  host=$1
  shift
  "$ssh_command" "$host" "$@"
}

cleanup() {
  remote "$linux_host" sudo "$endpoint" stop "$linux_interface" \
    >/dev/null 2>&1 || true
  remote "$freebsd_host" sudo "$endpoint" stop "$freebsd_interface" \
    >/dev/null 2>&1 || true
  remote "$linux_host" rm -f "$endpoint" "$client" "$probe" \
    >/dev/null 2>&1 || true
  remote "$freebsd_host" rm -f "$endpoint" "$client" "$probe" \
    >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

for host in "$linux_host" "$freebsd_host"; do
  "$scp_command" "$script_dir/kea_ha_cross_host_endpoint.sh" "$host:$endpoint"
  "$scp_command" "$script_dir/kea_cross_host_client.py" "$host:$client"
  "$scp_command" "$script_dir/kea_ha_cross_host_probe.py" "$host:$probe"
  remote "$host" chmod 755 "$endpoint" "$client" "$probe"
done

run_phase() {
  primary=$1
  if [ "$primary" = linux ]; then
    linux_role=primary
    freebsd_role=standby
    client_host=$freebsd_host
    client_interface=$freebsd_interface
    client4=$freebsd4
    client6=$freebsd6
    primary4=$linux4
    primary6=$linux6
  else
    linux_role=standby
    freebsd_role=primary
    client_host=$linux_host
    client_interface=$linux_interface
    client4=$linux4
    client6=$linux6
    primary4=$freebsd4
    primary6=$freebsd6
  fi

  # Bring up the standby first so the primary can immediately establish both
  # HA protocol sessions. Every command names both peers explicitly; socket or
  # array position never acts as durable identity.
  if [ "$linux_role" = standby ]; then
    first_host=$linux_host
    first_interface=$linux_interface
    first4=$linux4
    first6=$linux6
    first_name=linux
    first_role=$linux_role
    first_remote4=$freebsd4
    first_remote6=$freebsd6
    first_remote_name=freebsd
    first_remote_role=$freebsd_role
    second_host=$freebsd_host
    second_interface=$freebsd_interface
    second4=$freebsd4
    second6=$freebsd6
    second_name=freebsd
    second_role=$freebsd_role
    second_remote4=$linux4
    second_remote6=$linux6
    second_remote_name=linux
    second_remote_role=$linux_role
  else
    first_host=$freebsd_host
    first_interface=$freebsd_interface
    first4=$freebsd4
    first6=$freebsd6
    first_name=freebsd
    first_role=$freebsd_role
    first_remote4=$linux4
    first_remote6=$linux6
    first_remote_name=linux
    first_remote_role=$linux_role
    second_host=$linux_host
    second_interface=$linux_interface
    second4=$linux4
    second6=$linux6
    second_name=linux
    second_role=$linux_role
    second_remote4=$freebsd4
    second_remote6=$freebsd6
    second_remote_name=freebsd
    second_remote_role=$freebsd_role
  fi

  remote "$first_host" sudo "$endpoint" start "$first_interface" \
    "$first4" "$first6" "$first_name" "$first_role" \
    "$first_remote4" "$first_remote6" "$first_remote_name" \
    "$first_remote_role"
  remote "$second_host" sudo "$endpoint" start "$second_interface" \
    "$second4" "$second6" "$second_name" "$second_role" \
    "$second_remote4" "$second_remote6" "$second_remote_name" \
    "$second_remote_role"

  remote "$linux_host" sudo "$endpoint" ready "$linux_interface" linux \
    freebsd "$linux_role" "$probe"
  remote "$freebsd_host" sudo "$endpoint" ready "$freebsd_interface" \
    freebsd linux "$freebsd_role" "$probe"
  remote "$client_host" sudo "$endpoint" client "$client_interface" \
    "$client4" "$client6" "$primary4" "$primary6" "$client"
  if ! remote "$linux_host" sudo "$endpoint" verify "$linux_interface" \
       "$probe"; then
    remote "$linux_host" sudo "$endpoint" logs "$linux_interface" || true
    remote "$freebsd_host" sudo "$endpoint" logs "$freebsd_interface" || true
    return 1
  fi
  if ! remote "$freebsd_host" sudo "$endpoint" verify \
       "$freebsd_interface" "$probe"; then
    remote "$linux_host" sudo "$endpoint" logs "$linux_interface" || true
    remote "$freebsd_host" sudo "$endpoint" logs "$freebsd_interface" || true
    return 1
  fi

  remote "$linux_host" sudo "$endpoint" stop "$linux_interface"
  remote "$freebsd_host" sudo "$endpoint" stop "$freebsd_interface"
  echo "$primary primary replicated DHCPv4 and DHCPv6 leases to its standby"
}

run_phase linux
run_phase freebsd
echo "Bidirectional Linux/FreeBSD Kea HA replication passed"

#!/bin/sh
# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

set -eu

linux_host=${1:-dev-linux-1}
freebsd_host=${2:-dev-freebsd-1}
linux_interface=${3:-ens19}
freebsd_interface=${4:-vtnet1}
endpoint=/tmp/dang-kea-cross-endpoint-$$.sh
client=/tmp/dang-kea-cross-client-$$.py
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
source_endpoint=$script_dir/kea_cross_host_endpoint.sh
source_client=$script_dir/kea_cross_host_client.py
ssh_command=${DANG_TEST_SSH:-ssh}
scp_command=${DANG_TEST_SCP:-scp}

remote() {
  host=$1
  shift
  # DANG_TEST_SSH is deliberately a command name, not shell-expanded options.
  "$ssh_command" "$host" "$@"
}

cleanup() {
  remote "$linux_host" sudo "$endpoint" stop "$linux_interface" >/dev/null 2>&1 || true
  remote "$freebsd_host" sudo "$endpoint" stop "$freebsd_interface" >/dev/null 2>&1 || true
  remote "$linux_host" rm -f "$endpoint" >/dev/null 2>&1 || true
  remote "$freebsd_host" rm -f "$endpoint" >/dev/null 2>&1 || true
  remote "$linux_host" rm -f "$client" >/dev/null 2>&1 || true
  remote "$freebsd_host" rm -f "$client" >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

"$scp_command" "$source_endpoint" "$linux_host:$endpoint"
"$scp_command" "$source_endpoint" "$freebsd_host:$endpoint"
"$scp_command" "$source_client" "$linux_host:$client"
"$scp_command" "$source_client" "$freebsd_host:$client"
remote "$linux_host" chmod 755 "$endpoint"
remote "$freebsd_host" chmod 755 "$endpoint"

remote "$linux_host" sudo "$endpoint" start "$linux_interface"
remote "$freebsd_host" sudo "$endpoint" client "$freebsd_interface" "$client"
remote "$linux_host" sudo "$endpoint" verify "$linux_interface"
remote "$linux_host" sudo "$endpoint" stop "$linux_interface"

remote "$freebsd_host" sudo "$endpoint" start "$freebsd_interface"
remote "$linux_host" sudo "$endpoint" client "$linux_interface" "$client"
remote "$freebsd_host" sudo "$endpoint" verify "$freebsd_interface"
remote "$freebsd_host" sudo "$endpoint" stop "$freebsd_interface"

echo "Bidirectional Linux/FreeBSD Kea DHCPv4 and DHCPv6 exchanges passed"

#!/bin/sh
# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

# Management-plane hook for run_kea_ha_cross_host.sh. Keeping this separate
# leaves the native HA harness independent of dangd and plugin details.

set -eu

primary=$1
linux_host=$2
freebsd_host=$3
linux_interface=$4
freebsd_interface=$5
linux_role=$6
freebsd_role=$7
linux4=$8
freebsd4=${10}
endpoint=${DANG_KEA_PAIR_NETCONF_ENDPOINT:?missing endpoint path}
cert_dir=${DANG_KEA_PAIR_NETCONF_CERT_DIR:?missing certificate path}
ssh_command=${DANG_TEST_SSH:-ssh}

remote() {
  host=$1
  shift
  "$ssh_command" "$host" "$@"
}

fail() {
  echo "Kea pair NETCONF hook: $*" >&2
  exit 1
}

if [ "$primary" = linux ]; then
  group_id=kea-ha-a142e22fd2a2703b
  initiating_host=$linux_host
  initiating_interface=$linux_interface
  initiating4=$linux4
  unavailable_host=$freebsd_host
  unavailable_interface=$freebsd_interface
else
  group_id=kea-ha-8c1c77c9a196b18b
  initiating_host=$freebsd_host
  initiating_interface=$freebsd_interface
  initiating4=$freebsd4
  unavailable_host=$linux_host
  unavailable_interface=$linux_interface
fi

linux_core_source=$(remote "$linux_host" realpath \
  '$HOME/dang-validation/dang')
linux_core_build=$(remote "$linux_host" realpath \
  '$HOME/dang-validation/dang/build-package')
linux_plugin_build=$(remote "$linux_host" realpath \
  '$HOME/dang-validation/dang_plugins/build-package')
freebsd_core_source=$(remote "$freebsd_host" realpath \
  '$HOME/dang-validation/dang')
freebsd_core_build=$(remote "$freebsd_host" realpath \
  '$HOME/dang-validation/dang/build-package')
freebsd_plugin_build=$(remote "$freebsd_host" realpath \
  '$HOME/dang-validation/dang_plugins/build-package')

remote "$linux_host" sudo "$endpoint" start "$linux_interface" \
  "$linux4" linux "$linux_role" "$freebsd4" freebsd "$freebsd_role" \
  "$group_id" "$cert_dir" "$linux_core_source" "$linux_core_build" \
  "$linux_plugin_build"
remote "$freebsd_host" sudo "$endpoint" start "$freebsd_interface" \
  "$freebsd4" freebsd "$freebsd_role" "$linux4" linux "$linux_role" \
  "$group_id" "$cert_dir" "$freebsd_core_source" "$freebsd_core_build" \
  "$freebsd_plugin_build"

initiating_build=$(remote "$initiating_host" realpath \
  '$HOME/dang-validation/dang/build-package')
if ! remote "$initiating_host" sudo "$endpoint" commit \
    "$initiating_interface" "$initiating4" 601 "$cert_dir" \
    "$initiating_build"; then
  remote "$linux_host" sudo "$endpoint" logs "$linux_interface" || true
  remote "$freebsd_host" sudo "$endpoint" logs "$freebsd_interface" || true
  fail "healthy pair-wide commit was rejected"
fi
remote "$linux_host" sudo "$endpoint" assert-lifetime "$linux_interface" 601
remote "$freebsd_host" sudo "$endpoint" assert-lifetime "$freebsd_interface" 601

# Make one participant unreachable, then require the normal NETCONF commit to
# fail without changing either authoritative daemon image.
remote "$unavailable_host" sudo "$endpoint" stop "$unavailable_interface"
if remote "$initiating_host" sudo "$endpoint" commit \
    "$initiating_interface" "$initiating4" 602 "$cert_dir" \
    "$initiating_build"; then
  fail "pair-wide commit unexpectedly accepted an unreachable participant"
fi
remote "$linux_host" sudo "$endpoint" assert-lifetime "$linux_interface" 601
remote "$freebsd_host" sudo "$endpoint" assert-lifetime "$freebsd_interface" 601
remote "$initiating_host" sudo "$endpoint" stop "$initiating_interface"
echo "$primary-initiated pair-wide NETCONF success and fail-closed checks passed"

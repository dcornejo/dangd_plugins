#!/bin/sh
# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

# Validate RFC 9249 with every imported model implemented where doing so
# affects schema constraints.  The inputs are explicit so this test never
# succeeds accidentally by finding a different revision in a host-wide YANG
# directory.
set -eu

if [ "$#" -ne 5 ]; then
  echo "usage: $0 YANGLINT NTP_MODELS SYSTEM_MODELS INTERFACE_MODELS CORE_MODELS" >&2
  exit 2
fi

yanglint=$1
ntp_models=$2
system_models=$3
interface_models=$4
core_models=$5

exec "$yanglint" \
  -p "$ntp_models" \
  -p "$system_models" \
  -p "$interface_models" \
  -p "$core_models" \
  "$system_models/ietf-system@2014-08-06.yang" \
  "$interface_models/ietf-interfaces@2018-02-20.yang" \
  "$ntp_models/ietf-access-control-list@2019-03-04.yang" \
  "$ntp_models/ietf-ntp@2022-07-05.yang"

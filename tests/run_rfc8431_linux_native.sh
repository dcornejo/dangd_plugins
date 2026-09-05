#!/bin/sh
# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

set -eu
binary=$1
plugin_test=${2:-}
plugin=${3:-}
namespace="dang-rib-$$"
cleanup() { sudo ip netns del "$namespace" >/dev/null 2>&1 || true; }
trap cleanup EXIT INT TERM

sudo ip netns add "$namespace"
sudo ip -n "$namespace" link add dummy0 type dummy
sudo ip -n "$namespace" link set dummy0 up
if [ -n "$plugin_test" ] && [ -n "$plugin" ]; then
  sudo ip netns exec "$namespace" "$plugin_test" "$plugin" \
    100 198.18.1.0/24 dummy0
  test -z "$(sudo ip -n "$namespace" route show table 100)"
fi
sudo ip netns exec "$namespace" "$binary" \
  linux install 100 198.18.0.0/24 dummy0
sudo ip -n "$namespace" route show table 100 |
  grep -F "198.18.0.0/24 dev dummy0"
sudo ip netns exec "$namespace" "$binary" \
  linux delete 100 198.18.0.0/24 dummy0
test -z "$(sudo ip -n "$namespace" route show table 100)"

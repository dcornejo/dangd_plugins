#!/bin/sh
# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

set -eu
binary=$1
plugin_test=${2:-}
plugin=${3:-}
namespace="dang-rib-$$"
registry="/tmp/dang-rib-registry-$$.json"
cleanup() {
  sudo ip netns del "$namespace" >/dev/null 2>&1 || true
  sudo rm -f "$registry"
}
trap cleanup EXIT INT TERM

sudo ip netns add "$namespace"
sudo ip -n "$namespace" link add dummy0 type dummy
sudo ip -n "$namespace" link set dummy0 up
sudo ip -n "$namespace" address add 192.0.2.1/24 dev dummy0
sudo ip -n "$namespace" -6 address add 2001:db8:1::1/64 dev dummy0
sudo ip -n "$namespace" link add dummy1 type dummy
sudo ip -n "$namespace" link set dummy1 up
if [ -n "$plugin_test" ] && [ -n "$plugin" ]; then
  sudo ip netns exec "$namespace" env DANG_RIB_REGISTRY_FILE="$registry" \
    "$plugin_test" "$plugin" \
    100 198.18.1.0/24 dummy0 192.0.2.2
  test -z "$(sudo ip -n "$namespace" route show table 100)"
fi
sudo ip -n "$namespace" route replace 198.18.2.0/24 table 101 \
  proto static metric 20 \
  nexthop dev dummy0 weight 1 nexthop dev dummy1 weight 1
sudo ip netns exec "$namespace" "$binary" \
  linux observe-multipath 101 198.18.2.0/24 dummy0 dummy1
sudo ip -n "$namespace" address add 192.0.3.1/24 dev dummy1
sudo ip -n "$namespace" nexthop add id 10 via 192.0.2.2 dev dummy0
sudo ip -n "$namespace" nexthop add id 11 via 192.0.3.2 dev dummy1
# The observer must retain each exact group weight internally even though the
# optional RFC load-balance feature is not advertised or serialized yet.
sudo ip -n "$namespace" nexthop add id 20 group 10,2/11,3
sudo ip -n "$namespace" route replace 198.18.4.0/24 table 104 nhid 20
sudo ip netns exec "$namespace" "$binary" \
  linux observe-nexthop-group 104 198.18.4.0/24 dummy0 dummy1
sudo ip netns exec "$namespace" "$binary" \
  linux install 100 198.18.0.0/24 dummy0
sudo ip -n "$namespace" route show table 100 |
  grep -F "198.18.0.0/24 dev dummy0"
sudo ip netns exec "$namespace" "$binary" \
  linux delete 100 198.18.0.0/24 dummy0
test -z "$(sudo ip -n "$namespace" route show table 100)"
sudo ip netns exec "$namespace" "$binary" \
  linux install 103 2001:db8:103::/64 dummy0
sudo ip -6 -n "$namespace" route show table 103 |
  grep -F "2001:db8:103::/64 dev dummy0"
sudo ip netns exec "$namespace" "$binary" \
  linux delete 103 2001:db8:103::/64 dummy0
test -z "$(sudo ip -6 -n "$namespace" route show table 103)"
sudo ip netns exec "$namespace" "$binary" \
  linux install-special 105 198.18.5.0/24 discard
sudo ip netns exec "$namespace" "$binary" \
  linux observe-special 105 198.18.5.0/24 discard
sudo ip netns exec "$namespace" "$binary" \
  linux delete-special 105 198.18.5.0/24 discard
test -z "$(sudo ip -n "$namespace" route show table 105)"
sudo ip netns exec "$namespace" "$binary" \
  linux install-special 106 2001:db8:106::/64 discard-with-error
sudo ip netns exec "$namespace" "$binary" \
  linux observe-special 106 2001:db8:106::/64 discard-with-error
sudo ip netns exec "$namespace" "$binary" \
  linux delete-special 106 2001:db8:106::/64 discard-with-error
test -z "$(sudo ip -6 -n "$namespace" route show table 106)"
if sudo ip netns exec "$namespace" "$binary" \
  linux install 102 198.18.3.0/24 dummy0 203.0.113.1; then
  echo "unreachable gateway unexpectedly passed rtnetlink validation" >&2
  exit 1
fi
test -z "$(sudo ip -n "$namespace" route show table 102)"

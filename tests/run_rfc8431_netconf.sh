#!/bin/sh
# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

# Drives one complete RFC 8431 candidate transaction through dangd's actual
# NETCONF framing, schema, plugin-worker, commit, and operational-data path.
# The caller must already have placed this process in disposable network
# isolation and supplies only documentation addresses, an interface, and an
# isolated RIB. The candidate configures that interface through the independent
# RFC 8343/8344 provider and references it from the RFC 8431 route, proving the
# generic cross-module leafref and two-plugin transaction contract.

set -eu

dangd=$1
worker=$2
plugin=$3
interface_plugin=$4
rib=$5
prefix=$6
interface=$7
local_address=$8
gateway=$9
runtime=$(mktemp -d /tmp/dang-rib-netconf.XXXXXX)

fail() {
  echo "RFC 8431 NETCONF test: $*" >&2
  if [ -r "$runtime/replies.xml" ]; then
    tail -200 "$runtime/replies.xml" >&2 || true
  fi
  if [ -r "$runtime/dangd.log" ]; then
    tail -200 "$runtime/dangd.log" >&2 || true
  fi
  exit 1
}

cleanup() {
  rm -rf "$runtime"
}
trap cleanup EXIT INT TERM

[ -x "$dangd" ] || fail "dangd is not executable"
[ -x "$worker" ] || fail "plugin worker is not executable"
[ -r "$plugin" ] || fail "RIB plugin is not readable"
[ -r "$interface_plugin" ] || fail "interface plugin is not readable"

cat >"$runtime/test.yang" <<'EOF'
module dang-rib-netconf-test {
  yang-version 1.1;
  namespace "urn:example:dang:rib-netconf-test";
  prefix drnt;
  revision 2026-10-09 {
    description "Disposable RFC 8431 NETCONF integration test.";
  }
}
EOF

cat >"$runtime/config.xml" <<'EOF'
<config xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"/>
EOF

cat >"$runtime/session.xml" <<EOF
<hello xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"><capabilities><capability>urn:ietf:params:netconf:base:1.0</capability></capabilities></hello>]]>]]>
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="lock"><lock><target><candidate/></target></lock></rpc>]]>]]>
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="edit"><edit-config><target><candidate/></target><default-operation>replace</default-operation><config><interfaces xmlns="urn:ietf:params:xml:ns:yang:ietf-interfaces" xmlns:iana-if-type="urn:ietf:params:xml:ns:yang:iana-if-type"><interface><name>$interface</name><type>iana-if-type:ethernetCsmacd</type><enabled>true</enabled><ipv4 xmlns="urn:ietf:params:xml:ns:yang:ietf-ip"><address><ip>$local_address</ip><prefix-length>24</prefix-length></address></ipv4></interface></interfaces><routing-instance xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib"><name>default</name><rib-list><name>$rib</name><address-family>ipv4-address-family</address-family><route-list><route-index>8431</route-index><match><ipv4><dest-ipv4-prefix>$prefix</dest-ipv4-prefix></ipv4></match><nexthop><nexthop-base><egress-interface-ipv4-address><outgoing-interface>$interface</outgoing-interface><ipv4-address>$gateway</ipv4-address></egress-interface-ipv4-address></nexthop-base></nexthop><route-attributes><route-preference>10</route-preference><local-only>false</local-only></route-attributes></route-list></rib-list></routing-instance></config></edit-config></rpc>]]>]]>
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="validate"><validate><source><candidate/></source></validate></rpc>]]>]]>
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="commit-add"><commit/></rpc>]]>]]>
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="get"><get/></rpc>]]>]]>
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="delete"><edit-config><target><candidate/></target><config><routing-instance xmlns="urn:ietf:params:xml:ns:yang:ietf-i2rs-rib" xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0" nc:operation="delete"><name>default</name></routing-instance></config></edit-config></rpc>]]>]]>
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="commit-delete"><commit/></rpc>]]>]]>
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="unlock"><unlock><target><candidate/></target></unlock></rpc>]]>]]>
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="close"><close-session/></rpc>]]>]]>
EOF

if ! "$dangd" --model "$runtime/test.yang" --config "$runtime/config.xml" \
    --state "$runtime/state.json" --plugin "$interface_plugin" \
    --plugin "$plugin" \
    --plugin-worker "$worker" --no-default-superuser \
    --recovery-user rib-netconf-test --stdio --username rib-netconf-test \
    <"$runtime/session.xml" >"$runtime/replies.xml" \
    2>"$runtime/dangd.log"; then
  fail "dangd session failed"
fi

if grep -q 'rpc-error' "$runtime/replies.xml"; then
  fail "NETCONF returned rpc-error"
fi
[ "$(grep -o '<ok/>' "$runtime/replies.xml" | wc -l | tr -d ' ')" -eq 8 ] ||
  fail "expected eight successful state-changing RPC replies"
grep -F "<route-index>8431</route-index>" "$runtime/replies.xml" >/dev/null ||
  fail "operational reply lost the modeled route index"
grep -F "$prefix" "$runtime/replies.xml" >/dev/null ||
  fail "operational reply omitted the committed route"
grep -F "<outgoing-interface>$interface</outgoing-interface>" \
  "$runtime/replies.xml" >/dev/null ||
  fail "operational reply omitted the cross-module interface reference"
grep -F "<ipv4-address>$gateway</ipv4-address>" \
  "$runtime/replies.xml" >/dev/null ||
  fail "operational reply omitted the native gateway"
grep -F 'message-id="close"' "$runtime/replies.xml" >/dev/null ||
  fail "close-session reply is missing"

echo "RFC 8431 NETCONF interface/route commit, read, and delete passed for $rib"

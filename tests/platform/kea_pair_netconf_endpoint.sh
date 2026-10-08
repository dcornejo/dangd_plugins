#!/bin/sh
# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

# Runs one disposable dangd participant over an already-running Kea HA member.
# The caller supplies only sterile-VLAN addresses and explicit build paths.

set -eu

action=${1:-}
interface=${2:-}
runtime=/tmp/dang-kea-netconf-${interface}
netconf_port=16513

fail() {
  echo "Kea pair NETCONF endpoint: $*" >&2
  exit 1
}

[ "$(id -u)" -eq 0 ] || fail "must run as root"
[ -n "$interface" ] || fail "an explicit interface is required"

case "$(uname -s)" in
  Linux)
    hook_dir=/usr/lib/x86_64-linux-gnu/kea/hooks
    python=$(command -v python3)
    ;;
  FreeBSD)
    hook_dir=/usr/local/lib/kea/hooks
    python=$(command -v python3 || command -v python3.12)
    ;;
  *) fail "unsupported operating system" ;;
esac

socket4=/var/run/kea/dang-ha-cross-dang-netconf-4.sock
socket6=/var/run/kea/dang-ha-cross-dang-netconf-6.sock
portable_hook_dir=/platform/kea/hooks

stop_server() {
  if [ -f "$runtime/pid" ]; then
    pid=$(cat "$runtime/pid")
    kill "$pid" 2>/dev/null || true
    attempt=0
    while kill -0 "$pid" 2>/dev/null; do
      attempt=$((attempt + 1))
      [ "$attempt" -lt 100 ] || fail "dangd process $pid did not stop"
      sleep 0.1
    done
  fi
  rm -rf "$runtime"
}

write_configuration() {
  lifetime=$1
  destination=$2
  cat >"$destination" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<config xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
  <system xmlns="urn:example:appliance"><hostname>kea-$local_name</hostname></system>
  <config xmlns="urn:ietf:params:xml:ns:yang:kea-dhcp4-server">
    <interfaces-config><interfaces>*</interfaces><dhcp-socket-type>udp</dhcp-socket-type></interfaces-config>
    <control-sockets><socket-type>unix</socket-type><socket-name>$socket4</socket-name></control-sockets>
    <lease-database><database-type>memfile</database-type><persist>false</persist></lease-database>
    <valid-lifetime>$lifetime</valid-lifetime>
    <hook-library><library>$portable_hook_dir/libdhcp_lease_cmds.so</library></hook-library>
    <hook-library><library>$portable_hook_dir/libdhcp_stat_cmds.so</library></hook-library>
    <hook-library><library>$portable_hook_dir/libdhcp_host_cmds.so</library></hook-library>
    <hook-library><library>$portable_hook_dir/libdhcp_ha.so</library><parameters>{"high-availability":[{"this-server-name":"$local_name","mode":"hot-standby","heartbeat-delay":1000,"max-response-delay":3000,"max-unacked-clients":0,"peers":[{"name":"$primary_name","url":"http://$primary4:18000/","role":"primary","auto-failover":true},{"name":"$standby_name","url":"http://$standby4:18000/","role":"standby","auto-failover":true}]}]}</parameters></hook-library>
    <subnet4><id>9401</id><pool><start-address>192.0.2.100</start-address><end-address>192.0.2.120</end-address></pool><subnet>192.0.2.0/24</subnet></subnet4>
  </config>
  <config xmlns="urn:ietf:params:xml:ns:yang:kea-dhcp6-server">
    <interfaces-config><interfaces>*</interfaces></interfaces-config>
    <control-sockets><socket-type>unix</socket-type><socket-name>$socket6</socket-name></control-sockets>
    <lease-database><database-type>memfile</database-type><persist>false</persist></lease-database>
    <preferred-lifetime>300</preferred-lifetime><valid-lifetime>$lifetime</valid-lifetime>
    <hook-library><library>$portable_hook_dir/libdhcp_lease_cmds.so</library></hook-library>
    <hook-library><library>$portable_hook_dir/libdhcp_stat_cmds.so</library></hook-library>
    <hook-library><library>$portable_hook_dir/libdhcp_host_cmds.so</library></hook-library>
    <hook-library><library>$portable_hook_dir/libdhcp_ha.so</library><parameters>{"high-availability":[{"this-server-name":"$local_name","mode":"hot-standby","heartbeat-delay":1000,"max-response-delay":3000,"max-unacked-clients":0,"peers":[{"name":"$primary_name","url":"http://$primary4:18001/","role":"primary","auto-failover":true},{"name":"$standby_name","url":"http://$standby4:18001/","role":"standby","auto-failover":true}]}]}</parameters></hook-library>
    <subnet6><id>9601</id><rapid-commit>true</rapid-commit><pool><start-address>2001:db8:6::100</start-address><end-address>2001:db8:6::120</end-address></pool><subnet>2001:db8:6::/64</subnet></subnet6>
  </config>
</config>
EOF
}

case "$action" in
  start)
    # start IFACE LOCAL4 LOCAL_NAME LOCAL_ROLE REMOTE4 REMOTE_NAME REMOTE_ROLE
    #       GROUP_ID CERT_DIR CORE_SOURCE CORE_BUILD PLUGIN_BUILD
    [ "$#" -eq 13 ] || fail "invalid start arguments"
    local4=$3
    local_name=$4
    local_role=$5
    remote4=$6
    remote_name=$7
    remote_role=$8
    group_id=$9
    cert_dir=${10}
    core_source=${11}
    core_build=${12}
    plugin_build=${13}
    case "$local4:$remote4" in
      192.0.2.1:192.0.2.2 | 192.0.2.2:192.0.2.1) ;;
      *) fail "addresses must be the documented sterile-VLAN pair" ;;
    esac
    case "$local_role:$remote_role" in
      primary:standby)
        primary_name=$local_name; primary4=$local4
        standby_name=$remote_name; standby4=$remote4
        ;;
      standby:primary)
        primary_name=$remote_name; primary4=$remote4
        standby_name=$local_name; standby4=$local4
        ;;
      *) fail "roles must be one primary and one standby" ;;
    esac
    [ -S "$socket4" ] && [ -S "$socket6" ] || fail "Kea sockets are not ready"
    [ ! -e "$runtime" ] || fail "stale runtime exists at $runtime"
    for file in ca.pem server.pem server.key peer-controller.pem \
                peer-controller.key operator.pem operator.key; do
      [ -r "$cert_dir/$file" ] || fail "missing TLS file $cert_dir/$file"
    done
    [ -x "$core_build/dangd" ] || fail "dangd is not executable"
    [ -x "$core_build/dangctl" ] || fail "dangctl is not executable"
    [ -x "$core_build/dangd-plugin-worker" ] || fail "plugin worker is not executable"
    [ -r "$plugin_build/dangd_kea_plugin.so" ] || fail "Kea plugin is missing"
    mkdir -m 700 "$runtime"
    write_configuration 600 "$runtime/config.xml"
    write_configuration 601 "$runtime/proposed-601.xml"
    write_configuration 602 "$runtime/proposed-602.xml"
    cat >"$runtime/nacm.xml" <<EOF
<nacm xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-acm">
  <enable-nacm>true</enable-nacm><read-default>deny</read-default>
  <write-default>deny</write-default><exec-default>deny</exec-default>
  <groups><group><name>peer-controllers</name><user-name>peer-controller</user-name></group></groups>
  <rule-list><name>peer-controller-access</name><group>peer-controllers</group>
    <rule><name>operations</name><module-name>ietf-netconf</module-name><rpc-name>*</rpc-name><access-operations>exec</access-operations><action>permit</action></rule>
    <rule><name>data</name><module-name>*</module-name><access-operations>*</access-operations><action>permit</action></rule>
  </rule-list>
</nacm>
EOF
    cat >"$runtime/peer-recovery.json" <<EOF
{"version":2,"peers":[
  {"group-id":"$group_id","participant-id":"$primary_name","host":"$primary4","port":$netconf_port,"certificate":"$cert_dir/peer-controller.pem","private-key":"$cert_dir/peer-controller.key","trust-anchor":"$cert_dir/ca.pem","timeout-ms":10000},
  {"group-id":"$group_id","participant-id":"$standby_name","host":"$standby4","port":$netconf_port,"certificate":"$cert_dir/peer-controller.pem","private-key":"$cert_dir/peer-controller.key","trust-anchor":"$cert_dir/ca.pem","timeout-ms":10000}
]}
EOF
    chmod 600 "$runtime/peer-recovery.json"
    env DANG_KEA_INSTANCE_ID="$local_name" \
      DANG_KEA_DHCP4_SOCKET="$socket4" \
      DANG_KEA_DHCP6_SOCKET="$socket6" \
      DANG_KEA_HOOK_DIRECTORY="$hook_dir" \
      "$core_build/dangd" \
      --model "$core_source/dangd/examples/appliance.yang" \
      --config "$runtime/config.xml" --nacm "$runtime/nacm.xml" \
      --state "$runtime/state.json" --peer-journal "$runtime/peer-journal.json" \
      --peer-recovery "$runtime/peer-recovery.json" \
      --peer-transaction-timeout-ms 30000 \
      --peer-controller-user peer-controller \
      --plugin "$plugin_build/dangd_kea_plugin.so" \
      --plugin-worker "$core_build/dangd-plugin-worker" \
      --tls-listen "$local4" --tls-port "$netconf_port" \
      --tls-cert "$cert_dir/server.pem" --tls-key "$cert_dir/server.key" \
      --tls-ca "$cert_dir/ca.pem" >"$runtime/dangd.log" 2>&1 &
    echo $! >"$runtime/pid"
    attempt=0
    while ! grep -q "TLS listening" "$runtime/dangd.log"; do
      attempt=$((attempt + 1))
      if [ "$attempt" -ge 200 ] || ! kill -0 "$(cat "$runtime/pid")" 2>/dev/null; then
        cat "$runtime/dangd.log" >&2 || true
        fail "dangd did not become ready"
      fi
      sleep 0.1
    done
    echo "$local_name dangd participant is ready"
    ;;
  commit)
    # commit IFACE HOST LIFETIME CERT_DIR CORE_BUILD
    [ "$#" -eq 6 ] || fail "invalid commit arguments"
    host=$3
    lifetime=$4
    cert_dir=$5
    core_build=$6
    proposed=$runtime/proposed-${lifetime}.xml
    [ -r "$proposed" ] || fail "unknown proposal $lifetime"
    "$core_build/dangctl" --host "$host" --port "$netconf_port" \
      --cert "$cert_dir/operator.pem" --key "$cert_dir/operator.key" \
      --ca "$cert_dir/ca.pem" --edit-config "$proposed" \
      --default-operation replace
    ;;
  assert-lifetime)
    # assert-lifetime IFACE EXPECTED
    [ "$#" -eq 3 ] || fail "invalid assert-lifetime arguments"
    "$python" -c 'import json,socket,sys
def value(path, service):
 s=socket.socket(socket.AF_UNIX); s.connect(path)
 s.sendall(json.dumps({"command":"config-get"}).encode()); s.shutdown(socket.SHUT_WR)
 data=b""
 while True:
  chunk=s.recv(65536)
  if not chunk: break
  data += chunk
 s.close(); reply=json.loads(data)
 if isinstance(reply,list): reply=reply[0]
 return int(reply["arguments"][service]["valid-lifetime"])
expected=int(sys.argv[3])
for path,service in ((sys.argv[1],"Dhcp4"),(sys.argv[2],"Dhcp6")):
 actual=value(path,service)
 if actual != expected: raise SystemExit(f"{service} lifetime {actual}, expected {expected}")' \
      "$socket4" "$socket6" "$3"
    echo "both Kea families retain valid-lifetime $3"
    ;;
  stop)
    stop_server
    ;;
  logs)
    tail -200 "$runtime/dangd.log" 2>/dev/null || true
    ;;
  *) fail "unknown action $action" ;;
esac

#!/bin/sh
# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

# Proves real Kea hot-standby replication, guarded takeover, and safe recovery
# between disposable Linux and FreeBSD endpoints, then reverses their roles.

set -eu

linux_host=${1:-dev-linux-1}
freebsd_host=${2:-dev-freebsd-1}
linux_interface=${3:-ens19}
freebsd_interface=${4:-vtnet1}
failover_mode=${5:-manual}
transport=${6:-plain}
endpoint=/tmp/dang-kea-ha-cross-endpoint-$$.sh
client=/tmp/dang-kea-ha-cross-client-$$.py
probe=/tmp/dang-kea-ha-cross-probe-$$.py
remote_cert_dir=/tmp/dang-kea-ha-cross-tls-$$
socket_label=${DANG_KEA_HA_SOCKET_LABEL:-}
local_cert_dir=
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ssh_command=${DANG_TEST_SSH:-ssh}
scp_command=${DANG_TEST_SCP:-scp}
linux4=192.0.2.1
linux6=2001:db8:6::1
freebsd4=192.0.2.2
freebsd6=2001:db8:6::2

case "$failover_mode" in
  manual) auto_failover=false ;;
  automatic) auto_failover=true ;;
  # Kea clears the survivor's scope when it reaches partner-down with
  # auto-failover disabled, even after a successful maintenance handshake.
  maintenance) auto_failover=true ;;
  *) echo "failover mode must be manual, automatic, or maintenance" >&2; exit 2 ;;
esac

case "$transport" in
  plain) ;;
  tls) ;;
  *) echo "transport must be plain or tls" >&2; exit 2 ;;
esac

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
  remote "$linux_host" rm -rf "$remote_cert_dir" >/dev/null 2>&1 || true
  remote "$freebsd_host" rm -rf "$remote_cert_dir" >/dev/null 2>&1 || true
  if [ -n "$local_cert_dir" ]; then
    rm -rf "$local_cert_dir"
  fi
}
trap cleanup EXIT INT TERM

write_certificate_config() {
  name=$1
  address=$2
  cat >"$local_cert_dir/$name.cnf" <<EOF
[req]
prompt = no
distinguished_name = subject
req_extensions = extensions
[subject]
CN = $name
[extensions]
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature,keyEncipherment
extendedKeyUsage = serverAuth,clientAuth
subjectAltName = IP:$address
EOF
}

if [ "$transport" = tls ]; then
  local_cert_dir=$(mktemp -d /tmp/dang-kea-ha-certificates.XXXXXX)
  cat >"$local_cert_dir/ca.cnf" <<EOF
[req]
prompt = no
distinguished_name = subject
x509_extensions = extensions
[subject]
CN = dang-kea-test-ca
[extensions]
basicConstraints = critical,CA:TRUE
keyUsage = critical,keyCertSign,cRLSign
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid:always
EOF
  cat >"$local_cert_dir/rogue-ca.cnf" <<EOF
[req]
prompt = no
distinguished_name = subject
x509_extensions = extensions
[subject]
CN = dang-kea-rogue-ca
[extensions]
basicConstraints = critical,CA:TRUE
keyUsage = critical,keyCertSign,cRLSign
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid:always
EOF
  write_certificate_config linux "$linux4"
  write_certificate_config freebsd "$freebsd4"
  write_certificate_config rogue 192.0.2.254
  for host in "$linux_host" "$freebsd_host"; do
    remote "$host" mkdir -m 700 "$remote_cert_dir"
  done
  # Issue from the FreeBSD endpoint so its slightly slower clock cannot see a
  # freshly created certificate as not-yet-valid. All material is disposable.
  "$scp_command" "$local_cert_dir/ca.cnf" \
    "$local_cert_dir/rogue-ca.cnf" "$local_cert_dir/linux.cnf" \
    "$local_cert_dir/freebsd.cnf" "$local_cert_dir/rogue.cnf" \
    "$freebsd_host:$remote_cert_dir/"
  remote "$freebsd_host" openssl req -quiet -new -x509 -newkey rsa:2048 -nodes \
    -days 1 -keyout "$remote_cert_dir/ca.key" \
    -out "$remote_cert_dir/ca.pem" -config "$remote_cert_dir/ca.cnf"
  for name in linux freebsd; do
    remote "$freebsd_host" openssl req -quiet -new -newkey rsa:2048 -nodes \
      -keyout "$remote_cert_dir/$name.key" \
      -out "$remote_cert_dir/$name.csr" \
      -config "$remote_cert_dir/$name.cnf"
    remote "$freebsd_host" openssl x509 -req \
      -in "$remote_cert_dir/$name.csr" -CA "$remote_cert_dir/ca.pem" \
      -CAkey "$remote_cert_dir/ca.key" -CAcreateserial -days 1 \
      -out "$remote_cert_dir/$name.pem" \
      -extfile "$remote_cert_dir/$name.cnf" -extensions extensions
  done
  remote "$freebsd_host" openssl req -quiet -new -x509 -newkey rsa:2048 \
    -nodes -days 1 -keyout "$remote_cert_dir/rogue-ca.key" \
    -out "$remote_cert_dir/rogue-ca.pem" \
    -config "$remote_cert_dir/rogue-ca.cnf"
  remote "$freebsd_host" openssl req -quiet -new -newkey rsa:2048 -nodes \
    -keyout "$remote_cert_dir/rogue.key" \
    -out "$remote_cert_dir/rogue.csr" \
    -config "$remote_cert_dir/rogue.cnf"
  remote "$freebsd_host" openssl x509 -req \
    -in "$remote_cert_dir/rogue.csr" -CA "$remote_cert_dir/rogue-ca.pem" \
    -CAkey "$remote_cert_dir/rogue-ca.key" -CAcreateserial -days 1 \
    -out "$remote_cert_dir/rogue.pem" \
    -extfile "$remote_cert_dir/rogue.cnf" -extensions extensions
  "$scp_command" "$freebsd_host:$remote_cert_dir/ca.pem" \
    "$local_cert_dir/ca.pem"
  "$scp_command" "$freebsd_host:$remote_cert_dir/linux.pem" \
    "$local_cert_dir/linux.pem"
  "$scp_command" "$freebsd_host:$remote_cert_dir/linux.key" \
    "$local_cert_dir/linux.key"
  "$scp_command" "$freebsd_host:$remote_cert_dir/rogue.pem" \
    "$local_cert_dir/rogue.pem"
  "$scp_command" "$freebsd_host:$remote_cert_dir/rogue.key" \
    "$local_cert_dir/rogue.key"
  "$scp_command" "$local_cert_dir/ca.pem" "$local_cert_dir/linux.pem" \
    "$local_cert_dir/linux.key" "$local_cert_dir/rogue.pem" \
    "$local_cert_dir/rogue.key" "$linux_host:$remote_cert_dir/"
  remote "$linux_host" chmod 600 "$remote_cert_dir/linux.key" \
    "$remote_cert_dir/rogue.key"
  remote "$freebsd_host" chmod 600 "$remote_cert_dir/freebsd.key" \
    "$remote_cert_dir/rogue.key"
fi

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
    primary_host=$linux_host
    primary_interface=$linux_interface
    primary_name=linux
    standby_host=$freebsd_host
    standby_interface=$freebsd_interface
    standby_name=freebsd
    standby4=$freebsd4
    standby6=$freebsd6
  else
    linux_role=standby
    freebsd_role=primary
    client_host=$linux_host
    client_interface=$linux_interface
    client4=$linux4
    client6=$linux6
    primary4=$freebsd4
    primary6=$freebsd6
    primary_host=$freebsd_host
    primary_interface=$freebsd_interface
    primary_name=freebsd
    standby_host=$linux_host
    standby_interface=$linux_interface
    standby_name=linux
    standby4=$linux4
    standby6=$linux6
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
    "$first_remote_role" "$auto_failover" "$transport" "$remote_cert_dir" \
    ${socket_label:+"$socket_label"}
  remote "$second_host" sudo "$endpoint" start "$second_interface" \
    "$second4" "$second6" "$second_name" "$second_role" \
    "$second_remote4" "$second_remote6" "$second_remote_name" \
    "$second_remote_role" "$auto_failover" "$transport" "$remote_cert_dir" \
    ${socket_label:+"$socket_label"}

  if ! remote "$linux_host" sudo "$endpoint" ready "$linux_interface" \
       linux freebsd "$linux_role" "$probe"; then
    remote "$linux_host" sudo "$endpoint" logs "$linux_interface" || true
    remote "$freebsd_host" sudo "$endpoint" logs "$freebsd_interface" || true
    return 1
  fi
  if ! remote "$freebsd_host" sudo "$endpoint" ready \
       "$freebsd_interface" freebsd linux "$freebsd_role" "$probe"; then
    remote "$linux_host" sudo "$endpoint" logs "$linux_interface" || true
    remote "$freebsd_host" sudo "$endpoint" logs "$freebsd_interface" || true
    return 1
  fi
  if [ "$transport" = tls ]; then
    remote "$linux_host" sudo "$endpoint" tls-guard "$linux_interface" \
      "$freebsd4" "$probe" "$remote_cert_dir"
    remote "$freebsd_host" sudo "$endpoint" tls-guard "$freebsd_interface" \
      "$linux4" "$probe" "$remote_cert_dir"
  fi

  # An optional local hook may exercise a management plane while the real
  # pair is healthy. Its positional contract is deliberately generic host and
  # topology data; the HA harness has no knowledge of dangd or any plugin.
  if [ -n "${DANG_KEA_HA_READY_HOOK:-}" ]; then
    "$DANG_KEA_HA_READY_HOOK" "$primary" \
      "$linux_host" "$freebsd_host" \
      "$linux_interface" "$freebsd_interface" \
      "$linux_role" "$freebsd_role" \
      "$linux4" "$linux6" "$freebsd4" "$freebsd6"
  fi
  remote "$client_host" sudo "$endpoint" client "$client_interface" \
    "$client4" "$client6" "$primary4" "$primary6" "$client"
  if ! remote "$linux_host" sudo "$endpoint" verify "$linux_interface" \
       "$probe" 192.0.2.100 2001:db8:6::100; then
    remote "$linux_host" sudo "$endpoint" logs "$linux_interface" || true
    remote "$freebsd_host" sudo "$endpoint" logs "$freebsd_interface" || true
    return 1
  fi
  if ! remote "$freebsd_host" sudo "$endpoint" verify \
       "$freebsd_interface" "$probe" 192.0.2.100 2001:db8:6::100; then
    remote "$linux_host" sudo "$endpoint" logs "$linux_interface" || true
    remote "$freebsd_host" sudo "$endpoint" logs "$freebsd_interface" || true
    return 1
  fi

  # Planned maintenance first coordinates both live peers: the survivor must
  # serve the primary scope and the primary must stop serving before shutdown.
  if [ "$failover_mode" = maintenance ]; then
    remote "$standby_host" sudo "$endpoint" maintenance \
      "$standby_interface" "$standby_name" "$primary_name" "$probe"
    remote "$primary_host" sudo "$endpoint" maintenance-target \
      "$primary_interface" "$primary_name" "$standby_name" "$probe"
  fi

  # Manual scopes can cause split-brain if the primary can still answer, and
  # automatic failure detection must not race an incompletely stopped peer.
  # Prove both primary daemons have exited before the outage phase proceeds.
  remote "$primary_host" sudo "$endpoint" halt "$primary_interface"
  if [ "$failover_mode" = automatic ]; then
    remote "$standby_host" sudo "$endpoint" automatic \
      "$standby_interface" "$standby_name" "$primary_name" "$probe"
  elif [ "$failover_mode" = maintenance ]; then
    remote "$standby_host" sudo "$endpoint" maintenance-down \
      "$standby_interface" "$standby_name" "$primary_name" "$probe"
  else
    remote "$standby_host" sudo "$endpoint" takeover "$standby_interface" \
      "$standby_name" "$primary_name" "$probe"
  fi
  remote "$primary_host" sudo "$endpoint" client "$primary_interface" \
    "$primary4" "$primary6" "$standby4" "$standby6" "$client" \
    192.0.2.101 2001:db8:6::101
  if ! remote "$standby_host" sudo "$endpoint" verify \
       "$standby_interface" "$probe" 192.0.2.101 2001:db8:6::101; then
    remote "$standby_host" sudo "$endpoint" logs "$standby_interface" || true
    return 1
  fi

  # Manual mode removes the survivor's assigned scope before the former primary
  # returns, deliberately preferring a bounded service gap to two responders.
  # Automatic partner-down recovery safely retains service while Kea negotiates
  # and synchronizes. In either mode the restarted empty database must recover
  # the outage leases before a fresh allocation proves replication has resumed.
  if [ "$failover_mode" = manual ]; then
    remote "$standby_host" sudo "$endpoint" relinquish \
      "$standby_interface" "$standby_name" "$probe"
  fi
  remote "$primary_host" sudo "$endpoint" resume "$primary_interface"
  remote "$primary_host" sudo "$endpoint" ready "$primary_interface" \
    "$primary_name" "$standby_name" primary "$probe"
  remote "$standby_host" sudo "$endpoint" ready "$standby_interface" \
    "$standby_name" "$primary_name" standby "$probe"
  remote "$primary_host" sudo "$endpoint" verify "$primary_interface" \
    "$probe" 192.0.2.101 2001:db8:6::101
  remote "$standby_host" sudo "$endpoint" client "$standby_interface" \
    "$standby4" "$standby6" "$primary4" "$primary6" "$client" \
    192.0.2.102 2001:db8:6::102 02:00:00:00:94:02
  if ! remote "$primary_host" sudo "$endpoint" verify \
       "$primary_interface" "$probe" 192.0.2.102 2001:db8:6::102 ||
     ! remote "$standby_host" sudo "$endpoint" verify \
       "$standby_interface" "$probe" 192.0.2.102 2001:db8:6::102; then
    remote "$linux_host" sudo "$endpoint" logs "$linux_interface" || true
    remote "$freebsd_host" sudo "$endpoint" logs "$freebsd_interface" || true
    return 1
  fi

  remote "$linux_host" sudo "$endpoint" stop "$linux_interface"
  remote "$freebsd_host" sudo "$endpoint" stop "$freebsd_interface"
  echo "$primary primary completed $transport $failover_mode and recovery"
}

run_phase linux
run_phase freebsd
echo "Bidirectional Linux/FreeBSD Kea HA $transport $failover_mode passed"

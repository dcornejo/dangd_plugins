#!/bin/sh
# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

# Adds live pair-wide NETCONF success and unreachable-peer evidence to the
# bidirectional Linux/FreeBSD Kea HA test. No management interface is touched.

set -eu

linux_host=${1:-dev-linux-1}
freebsd_host=${2:-dev-freebsd-1}
linux_interface=${3:-ens19}
freebsd_interface=${4:-vtnet1}
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ssh_command=${DANG_TEST_SSH:-ssh}
scp_command=${DANG_TEST_SCP:-scp}
endpoint=/tmp/dang-kea-pair-netconf-endpoint-$$.sh
cert_dir=/tmp/dang-kea-pair-netconf-certs-$$
local_cert_dir=$(mktemp -d /tmp/dang-kea-pair-netconf.XXXXXX)

remote() {
  host=$1
  shift
  "$ssh_command" "$host" "$@"
}

cleanup() {
  remote "$linux_host" sudo "$endpoint" stop "$linux_interface" >/dev/null 2>&1 || true
  remote "$freebsd_host" sudo "$endpoint" stop "$freebsd_interface" >/dev/null 2>&1 || true
  remote "$linux_host" rm -rf "$endpoint" "$cert_dir" >/dev/null 2>&1 || true
  remote "$freebsd_host" rm -rf "$endpoint" "$cert_dir" >/dev/null 2>&1 || true
  rm -rf "$local_cert_dir"
}
trap cleanup EXIT INT TERM

cat >"$local_cert_dir/ca.cnf" <<EOF
[req]
prompt = no
distinguished_name = subject
x509_extensions = extensions
[subject]
CN = dang-kea-pair-netconf-ca
[extensions]
basicConstraints = critical,CA:TRUE
keyUsage = critical,keyCertSign,cRLSign
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid:always
EOF

write_certificate_config() {
  name=$1
  usages=$2
  sans=$3
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
extendedKeyUsage = $usages
$sans
EOF
}

write_certificate_config server serverAuth \
  "subjectAltName = IP:192.0.2.1,IP:192.0.2.2"
write_certificate_config peer-controller clientAuth ""
write_certificate_config operator clientAuth ""

openssl req -new -x509 -newkey rsa:2048 -nodes -days 1 \
  -keyout "$local_cert_dir/ca.key" -out "$local_cert_dir/ca.pem" \
  -config "$local_cert_dir/ca.cnf"
for name in server peer-controller operator; do
  openssl req -new -newkey rsa:2048 -nodes \
    -keyout "$local_cert_dir/$name.key" -out "$local_cert_dir/$name.csr" \
    -config "$local_cert_dir/$name.cnf"
  openssl x509 -req -in "$local_cert_dir/$name.csr" \
    -CA "$local_cert_dir/ca.pem" -CAkey "$local_cert_dir/ca.key" \
    -CAcreateserial -days 1 -out "$local_cert_dir/$name.pem" \
    -extfile "$local_cert_dir/$name.cnf" -extensions extensions
done

# The operator uses dangd's explicitly documented, transport-bound recovery
# identity. Peer coordination uses a different non-recovery identity.
sed 's/CN = operator/CN = dangd-superuser/' \
  "$local_cert_dir/operator.cnf" >"$local_cert_dir/operator-final.cnf"
openssl req -new -newkey rsa:2048 -nodes \
  -keyout "$local_cert_dir/operator.key" -out "$local_cert_dir/operator.csr" \
  -config "$local_cert_dir/operator-final.cnf"
openssl x509 -req -in "$local_cert_dir/operator.csr" \
  -CA "$local_cert_dir/ca.pem" -CAkey "$local_cert_dir/ca.key" \
  -CAcreateserial -days 1 -out "$local_cert_dir/operator.pem" \
  -extfile "$local_cert_dir/operator-final.cnf" -extensions extensions

for host in "$linux_host" "$freebsd_host"; do
  remote "$host" mkdir -m 700 "$cert_dir"
  "$scp_command" "$script_dir/kea_pair_netconf_endpoint.sh" "$host:$endpoint"
  "$scp_command" "$local_cert_dir/ca.pem" "$local_cert_dir/server.pem" \
    "$local_cert_dir/server.key" "$local_cert_dir/peer-controller.pem" \
    "$local_cert_dir/peer-controller.key" "$local_cert_dir/operator.pem" \
    "$local_cert_dir/operator.key" "$host:$cert_dir/"
  remote "$host" chmod 755 "$endpoint"
  remote "$host" chmod 600 "$cert_dir/server.key" \
    "$cert_dir/peer-controller.key" "$cert_dir/operator.key"
done

export DANG_KEA_HA_READY_HOOK=$script_dir/kea_pair_netconf_hook.sh
export DANG_KEA_PAIR_NETCONF_ENDPOINT=$endpoint
export DANG_KEA_PAIR_NETCONF_CERT_DIR=$cert_dir
export DANG_KEA_HA_SOCKET_LABEL=dang-netconf
export DANG_TEST_SSH

"$script_dir/run_kea_ha_cross_host.sh" "$linux_host" "$freebsd_host" \
  "$linux_interface" "$freebsd_interface" automatic plain
echo "Bidirectional Linux/FreeBSD pair-wide NETCONF evidence passed"

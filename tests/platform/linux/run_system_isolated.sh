#!/bin/sh
# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

set -eu

root=${1:-/tmp/dang_plugins_system}
fixture=/tmp/dang-system-test-root
service=dangd-rfc7317-test
policy=/etc/pam.d/$service
module=/usr/local/lib/security/pam_dangd_rfc7317_test.so

cleanup() {
  rm -f "$policy"
  rm -f "$module"
  rm -rf "$fixture"
}
trap cleanup EXIT INT TERM
[ ! -e "$policy" ] && [ ! -e "$module" ] || {
  echo "temporary PAM test path already exists; refusing to replace it" >&2
  exit 1
}
cleanup
mkdir -p "$(dirname "$module")"
cp "$root/build/pam_dangd.so" "$module"
chmod 0555 "$module"
printf 'auth required %s socket=%s/auth.sock\naccount required %s socket=%s/auth.sock\n' \
  "$module" "$fixture" "$module" "$fixture" > "$policy"

"$root/build/system_plugin_integration_test" \
  "$root/build/dangd_system_plugin.so" "$root/tests/system-before.xml" \
  "$root/tests/system-proposed.xml" "$fixture" "$service"

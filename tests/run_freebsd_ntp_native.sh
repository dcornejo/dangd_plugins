#!/bin/sh
# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

set -eu

binary=$(realpath "$1")
initial=disable
if service ntpd onestatus >/dev/null 2>&1; then
  initial=enable
fi
cleanup() {
  sudo "$binary" "$initial" >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

sudo "$binary" enable
service ntpd onestatus >/dev/null
sudo "$binary" disable
if service ntpd onestatus >/dev/null 2>&1; then
  echo "ntpd remained active after onestop completed" >&2
  exit 1
fi

cleanup
trap - EXIT INT TERM
if [ "$initial" = enable ]; then
  service ntpd onestatus >/dev/null
else
  ! service ntpd onestatus >/dev/null 2>&1
fi

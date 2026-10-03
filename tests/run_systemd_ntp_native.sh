#!/bin/sh
# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

set -eu

binary=$(realpath "$1")
initial=disable
if systemctl is-active --quiet chrony.service; then
  initial=enable
fi
cleanup() {
  sudo "$binary" "$initial" >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

sudo "$binary" enable
systemctl is-active --quiet chrony.service
sudo "$binary" disable
systemctl is-active --quiet chrony.service && {
  echo "chrony remained active after the systemd stop job completed" >&2
  exit 1
}

cleanup
trap - EXIT INT TERM
if [ "$initial" = enable ]; then
  systemctl is-active --quiet chrony.service
else
  ! systemctl is-active --quiet chrony.service
fi

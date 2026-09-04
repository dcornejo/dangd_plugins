#!/bin/sh
# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

set -eu

if [ "$#" -ne 3 ]; then
  echo "usage: $0 YANGLINT RIB_MODEL_DIR DANG_MODEL_DIR" >&2
  exit 2
fi

yanglint=$1
rib_models=$2
dang_models=$3

"$yanglint" -p "$rib_models" -p "$dang_models" \
  "$rib_models/ietf-i2rs-rib@2018-09-13.yang"

# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

# Require the generic resource-domain guard to reject simultaneous ownership.
execute_process(
  COMMAND "${DANGD}"
    --model "${MODEL}"
    --config "${CONFIG}"
    --search "${SEARCH}"
    --plugin "${NTP_PLUGIN}"
    --plugin "${SYSTEM_PLUGIN}"
    --plugin-worker "${WORKER}"
    --no-default-superuser --check
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error)

set(combined "${output}${error}")
set(expected
  "resource domain ntp is owned by plugins ietf-ntp and ietf-system")
if(result EQUAL 0)
  message(FATAL_ERROR "dangd accepted simultaneous RFC 7317/RFC 9249 ownership")
endif()
string(FIND "${combined}" "${expected}" position)
if(position EQUAL -1)
  message(FATAL_ERROR
    "dangd failed for the wrong reason (status ${result}): ${combined}")
endif()

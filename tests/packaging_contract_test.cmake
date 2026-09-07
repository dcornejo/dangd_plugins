# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

# Verify that mutually exclusive routing providers carry the same package-file
# ownership marker. FreeBSD pkg rejects packages that claim the same path;
# Debian additionally receives explicit symmetric Conflicts metadata.
foreach(component IN ITEMS frr rib)
  set(stage "${BINARY_DIR}/packaging-contract-${component}")
  file(REMOVE_RECURSE "${stage}")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${BINARY_DIR}"
      --prefix "${stage}" --component "${component}"
    RESULT_VARIABLE result
    ERROR_VARIABLE error)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "cannot stage ${component} package: ${error}")
  endif()
  file(GLOB_RECURSE markers
    "${stage}/*/dangd/resource-domains/routing")
  list(LENGTH markers marker_count)
  if(NOT marker_count EQUAL 1)
    message(FATAL_ERROR
      "${component} package has ${marker_count} routing ownership markers")
  endif()
  file(READ "${markers}" contents)
  if(NOT contents STREQUAL "exclusive dangd resource domain: routing\n")
    message(FATAL_ERROR "${component} routing ownership marker is invalid")
  endif()
endforeach()

file(READ "${SOURCE_DIR}/packaging/Packaging.cmake" packaging)
foreach(pair IN ITEMS
    "RIB_PACKAGE_CONFLICTS \"dangd-plugin-frr\""
    "FRR_PACKAGE_CONFLICTS \"dangd-plugin-rib\"")
  string(FIND "${packaging}" "${pair}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "Debian routing package conflict is missing: ${pair}")
  endif()
endforeach()

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

# The Kea component is deliberately self-contained.  Keep unrelated provider
# models and binaries out of its native package as the collection grows.
set(kea_stage "${BINARY_DIR}/packaging-contract-kea")
file(REMOVE_RECURSE "${kea_stage}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${BINARY_DIR}"
    --prefix "${kea_stage}" --component kea
  RESULT_VARIABLE kea_result
  ERROR_VARIABLE kea_error)
if(NOT kea_result EQUAL 0)
  message(FATAL_ERROR "cannot stage Kea package: ${kea_error}")
endif()
file(GLOB_RECURSE kea_files RELATIVE "${kea_stage}" "${kea_stage}/*")
list(SORT kea_files)
if(NOT CMAKE_SHARED_MODULE_SUFFIX)
  set(CMAKE_SHARED_MODULE_SUFFIX ".so")
endif()
set(expected_kea_files
  "lib/dangd/plugins/dangd_kea_plugin${CMAKE_SHARED_MODULE_SUFFIX}"
  "share/doc/dangd-plugins/KEA.md"
  "share/yang/modules/dang-kea-ha@2026-09-28.yang"
  "share/yang/modules/dang-kea-instance@2026-09-28.yang"
  "share/yang/modules/kea-dhcp-types@2026-06-24.yang"
  "share/yang/modules/kea-dhcp4-server@2026-06-24.yang"
  "share/yang/modules/kea-dhcp6-server@2026-06-24.yang"
  "share/yang/modules/kea-types@2025-06-25.yang")
list(SORT expected_kea_files)
if(NOT kea_files STREQUAL expected_kea_files)
  message(FATAL_ERROR
    "Kea component contents differ from its package contract:\n"
    "expected: ${expected_kea_files}\nactual: ${kea_files}")
endif()

file(READ "${SOURCE_DIR}/packaging/Packaging.cmake" packaging)

# Kea is a runtime provider rather than a link dependency, so package metadata
# must name it explicitly. Debian permits either single-stack daemon while the
# shared hooks package is required; FreeBSD supplies both families and hooks in
# one package.
foreach(requirement IN ITEMS
    "CPACK_DEBIAN_KEA_PACKAGE_DEPENDS"
    "isc-kea-hooks (>= 3.2.0)"
    "isc-kea-dhcp4 (>= 3.2.0) | isc-kea-dhcp6 (>= 3.2.0)"
    "devel/nlohmann-json;net/kea")
  string(FIND "${packaging}" "${requirement}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "Kea package dependency is missing: ${requirement}")
  endif()
endforeach()

foreach(pair IN ITEMS
    "RIB_PACKAGE_CONFLICTS \"dangd-plugin-frr\""
    "FRR_PACKAGE_CONFLICTS \"dangd-plugin-rib\"")
  string(FIND "${packaging}" "${pair}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "Debian routing package conflict is missing: ${pair}")
  endif()
endforeach()

# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.24)
project(dang_plugins VERSION 0.1.0 LANGUAGES CXX)

option(DANG_RIB_NATIVE_TESTS
  "Run privileged RFC 8431 tests in disposable network isolation" OFF)

include(CTest)
find_package(LibXml2 REQUIRED)
find_package(nlohmann_json 3.11 REQUIRED)
find_package(Threads REQUIRED)

add_executable(frr-schema-inventory
  plugins/frr/src/schema_inventory.cc
  plugins/frr/src/schema_inventory_probe.cc)
target_compile_features(frr-schema-inventory PRIVATE cxx_std_20)
target_include_directories(frr-schema-inventory PRIVATE plugins/frr/src)
target_compile_options(frr-schema-inventory PRIVATE
  -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

add_executable(frr-mgmtd-session-check
  plugins/frr/src/mgmtd_session_probe.cc
  plugins/frr/src/mgmtd_session.cc
  plugins/frr/src/mgmtd_transport.cc
  plugins/frr/src/mgmtd_wire.cc)
target_compile_features(frr-mgmtd-session-check PRIVATE cxx_std_20)
target_include_directories(frr-mgmtd-session-check PRIVATE plugins/frr/src)
target_compile_options(frr-mgmtd-session-check PRIVATE
  -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

set(DANGD_ROOT "" CACHE PATH "Path to a dang source tree")
set(DANGD_INCLUDE_DIR "" CACHE PATH "Directory containing dangd/plugin_api.h")
if(NOT DANGD_INCLUDE_DIR)
  if(DANGD_ROOT AND NOT IS_ABSOLUTE "${DANGD_ROOT}")
    cmake_path(ABSOLUTE_PATH DANGD_ROOT
      BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" NORMALIZE)
  endif()
  find_path(DANGD_DISCOVERED_INCLUDE_DIR dangd/plugin_api.h
    HINTS "${DANGD_ROOT}" "${DANGD_ROOT}/include")
  if(DANGD_DISCOVERED_INCLUDE_DIR)
    set(DANGD_INCLUDE_DIR "${DANGD_DISCOVERED_INCLUDE_DIR}")
  endif()
endif()
if(NOT DANGD_INCLUDE_DIR)
  message(FATAL_ERROR
    "dangd/plugin_api.h was not found; set DANGD_ROOT or DANGD_INCLUDE_DIR")
endif()

file(READ plugins/example/models/dang-plugins-example.yang
  DANG_PLUGINS_EXAMPLE_YANG)
configure_file(plugins/example/src/model_source.h.in
  generated/example_model_source.h @ONLY)

add_library(dangd_example_external_plugin MODULE
  plugins/example/src/example_plugin.cc)
target_compile_features(dangd_example_external_plugin PRIVATE cxx_std_20)
target_include_directories(dangd_example_external_plugin PRIVATE
  "${DANGD_INCLUDE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_options(dangd_example_external_plugin PRIVATE
  -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
set_target_properties(dangd_example_external_plugin PROPERTIES PREFIX "")

file(READ plugins/kea/models/kea-types@2025-06-25.yang KEA_TYPES_YANG)
file(READ plugins/kea/models/kea-dhcp-types@2026-06-24.yang KEA_DHCP_TYPES_YANG)
file(READ plugins/kea/models/kea-dhcp4-server@2026-06-24.yang KEA_DHCP4_YANG)
file(READ plugins/kea/models/kea-dhcp6-server@2026-06-24.yang KEA_DHCP6_YANG)
configure_file(plugins/kea/src/kea_model_sources.h.in
  generated/kea_model_sources.h @ONLY)

file(READ plugins/system/models/ietf-system@2014-08-06.yang IETF_SYSTEM_YANG)
file(READ plugins/system/models/iana-crypt-hash@2014-08-06.yang
  IANA_CRYPT_HASH_YANG)
configure_file(plugins/system/src/system_model_sources.h.in
  generated/system_model_sources.h @ONLY)

add_library(dangd_kea_plugin MODULE
  plugins/kea/src/kea_adapter.cc
  plugins/kea/src/kea_plugin.cc)
target_compile_features(dangd_kea_plugin PRIVATE cxx_std_20)
target_include_directories(dangd_kea_plugin PRIVATE
  "${DANGD_INCLUDE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/generated"
  plugins/kea/src)
target_link_libraries(dangd_kea_plugin PRIVATE
  LibXml2::LibXml2 nlohmann_json::nlohmann_json)
target_compile_options(dangd_kea_plugin PRIVATE
  -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
set_target_properties(dangd_kea_plugin PROPERTIES PREFIX "")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(DANG_SYSTEM_PLATFORM_SOURCE
      plugins/system/src/linux/platform_linux.cc)
    find_library(DANG_CRYPT_LIBRARY crypt REQUIRED)
  else()
    set(DANG_SYSTEM_PLATFORM_SOURCE
      plugins/system/src/freebsd/platform_freebsd.cc)
    find_library(DANG_CRYPT_LIBRARY crypt REQUIRED)
  endif()
  add_library(dangd_system_plugin MODULE
    plugins/system/src/auth_server.cc
    plugins/system/src/platform.cc
    plugins/system/src/system_config.cc
    plugins/system/src/system_plugin.cc
    ${DANG_SYSTEM_PLATFORM_SOURCE})
  target_compile_features(dangd_system_plugin PRIVATE cxx_std_20)
  target_include_directories(dangd_system_plugin PRIVATE
    "${DANGD_INCLUDE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/generated"
    "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(dangd_system_plugin PRIVATE
    LibXml2::LibXml2 ${DANG_CRYPT_LIBRARY})
  target_compile_options(dangd_system_plugin PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  set_target_properties(dangd_system_plugin PROPERTIES PREFIX "")

  find_path(DANG_PAM_INCLUDE_DIR security/pam_modules.h REQUIRED)
  find_library(DANG_PAM_LIBRARY pam REQUIRED)
  add_library(pam_dangd MODULE pam/pam_dangd.cc)
  target_compile_features(pam_dangd PRIVATE cxx_std_20)
  target_include_directories(pam_dangd PRIVATE
    "${DANG_PAM_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(pam_dangd PRIVATE "${DANG_PAM_LIBRARY}")
  target_compile_options(pam_dangd PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  set_target_properties(pam_dangd PROPERTIES PREFIX "")
endif()

if(BUILD_TESTING)
  find_package(GTest REQUIRED)
  add_executable(plugin_loader_smoke_test tests/plugin_loader_smoke_test.cc)
  target_compile_features(plugin_loader_smoke_test PRIVATE cxx_std_20)
  target_include_directories(plugin_loader_smoke_test PRIVATE
    "${DANGD_INCLUDE_DIR}")
  target_link_libraries(plugin_loader_smoke_test PRIVATE "${CMAKE_DL_LIBS}")
  target_compile_options(plugin_loader_smoke_test PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  add_test(NAME external_example_plugin_discovery
    COMMAND plugin_loader_smoke_test
      $<TARGET_FILE:dangd_example_external_plugin>
      dang-plugins-example dang-plugins-example)
  add_test(NAME kea_plugin_discovery
    COMMAND plugin_loader_smoke_test $<TARGET_FILE:dangd_kea_plugin>
      dang-kea kea-types)

  add_executable(kea_adapter_test
    tests/kea_adapter_test.cc plugins/kea/src/kea_adapter.cc)
  target_compile_features(kea_adapter_test PRIVATE cxx_std_20)
  target_include_directories(kea_adapter_test PRIVATE plugins/kea/src)
  target_link_libraries(kea_adapter_test PRIVATE
    LibXml2::LibXml2 nlohmann_json::nlohmann_json)
  target_compile_options(kea_adapter_test PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  add_test(NAME kea_xml_to_control_json COMMAND kea_adapter_test)

  find_program(DANG_YANGLINT_EXECUTABLE yanglint)
  if(DANG_YANGLINT_EXECUTABLE)
    add_test(NAME rfc8431_schema_interoperability
      COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_rfc8431_yanglint.sh"
        "${DANG_YANGLINT_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/plugins/rib/models"
        "${DANGD_ROOT}/dangd/models")
  endif()

  add_executable(rib_config_test
    tests/rib_config_test.cc
    plugins/rib/src/rib_config.cc
    plugins/rib/src/platform_executor.cc
    plugins/rib/src/linux/platform_linux.cc
    plugins/rib/src/freebsd/platform_freebsd.cc)
  target_compile_features(rib_config_test PRIVATE cxx_std_20)
  target_include_directories(rib_config_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(rib_config_test PRIVATE LibXml2::LibXml2 GTest::gtest_main)
  target_compile_options(rib_config_test PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  add_test(NAME rfc8431_configuration_planning COMMAND rib_config_test)

  add_executable(frr_schema_inventory_test
    tests/frr_schema_inventory_test.cc
    plugins/frr/src/schema_inventory.cc)
  target_compile_features(frr_schema_inventory_test PRIVATE cxx_std_20)
  target_include_directories(frr_schema_inventory_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(frr_schema_inventory_test PRIVATE GTest::gtest_main)
  target_compile_options(frr_schema_inventory_test PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  add_test(NAME frr_installed_schema_inventory COMMAND frr_schema_inventory_test)

  add_executable(frr_mgmtd_wire_test
    tests/frr_mgmtd_wire_test.cc plugins/frr/src/mgmtd_wire.cc)
  target_compile_features(frr_mgmtd_wire_test PRIVATE cxx_std_20)
  target_include_directories(frr_mgmtd_wire_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(frr_mgmtd_wire_test PRIVATE GTest::gtest_main)
  target_compile_options(frr_mgmtd_wire_test PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  add_test(NAME frr_mgmtd_native_wire_contract COMMAND frr_mgmtd_wire_test)

  add_executable(frr_mgmtd_transport_test
    tests/frr_mgmtd_transport_test.cc
    plugins/frr/src/mgmtd_transport.cc
    plugins/frr/src/mgmtd_wire.cc)
  target_compile_features(frr_mgmtd_transport_test PRIVATE cxx_std_20)
  target_include_directories(frr_mgmtd_transport_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(frr_mgmtd_transport_test PRIVATE
    GTest::gtest_main Threads::Threads)
  target_compile_options(frr_mgmtd_transport_test PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  add_test(NAME frr_mgmtd_transport_contract COMMAND frr_mgmtd_transport_test)

  add_executable(frr_mgmtd_session_test
    tests/frr_mgmtd_session_test.cc
    plugins/frr/src/mgmtd_session.cc
    plugins/frr/src/mgmtd_transport.cc
    plugins/frr/src/mgmtd_wire.cc)
  target_compile_features(frr_mgmtd_session_test PRIVATE cxx_std_20)
  target_include_directories(frr_mgmtd_session_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(frr_mgmtd_session_test PRIVATE
    GTest::gtest_main Threads::Threads)
  target_compile_options(frr_mgmtd_session_test PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  add_test(NAME frr_mgmtd_session_lifecycle COMMAND frr_mgmtd_session_test)

  add_executable(frr_transaction_test
    tests/frr_transaction_test.cc
    plugins/frr/src/frr_transaction.cc)
  target_compile_features(frr_transaction_test PRIVATE cxx_std_20)
  target_include_directories(frr_transaction_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(frr_transaction_test PRIVATE GTest::gtest_main)
  target_compile_options(frr_transaction_test PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  add_test(NAME frr_disposable_validation_and_rollback
    COMMAND frr_transaction_test)

  add_executable(rib_native_test
    tests/rib_native_test.cc
    plugins/rib/src/rib_config.cc
    plugins/rib/src/platform_executor.cc
    plugins/rib/src/linux/platform_linux.cc
    plugins/rib/src/freebsd/platform_freebsd.cc)
  target_compile_features(rib_native_test PRIVATE cxx_std_20)
  target_include_directories(rib_native_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(rib_native_test PRIVATE LibXml2::LibXml2)
  target_compile_options(rib_native_test PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  if(DANG_RIB_NATIVE_TESTS AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test(NAME rfc8431_linux_namespace_mutation
      COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_rfc8431_linux_native.sh"
        $<TARGET_FILE:rib_native_test>)
  elseif(DANG_RIB_NATIVE_TESTS AND CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    add_test(NAME rfc8431_freebsd_vnet_mutation
      COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_rfc8431_freebsd_native.sh"
        $<TARGET_FILE:rib_native_test>)
  endif()

  if(TARGET dangd_system_plugin)
    add_test(NAME system_plugin_discovery
      COMMAND plugin_loader_smoke_test $<TARGET_FILE:dangd_system_plugin>
        ietf-system ietf-system)
    add_executable(system_config_test
      tests/system_config_test.cc plugins/system/src/system_config.cc)
    target_compile_features(system_config_test PRIVATE cxx_std_20)
    target_include_directories(system_config_test PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(system_config_test PRIVATE
      LibXml2::LibXml2 ${DANG_CRYPT_LIBRARY})
    target_compile_options(system_config_test PRIVATE
      -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    add_test(NAME system_configuration_translation COMMAND system_config_test)
    add_executable(system_plugin_integration_test
      tests/system_plugin_integration_test.cc)
    target_compile_features(system_plugin_integration_test PRIVATE cxx_std_20)
    target_include_directories(system_plugin_integration_test PRIVATE
      "${DANGD_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(system_plugin_integration_test PRIVATE
      "${CMAKE_DL_LIBS}" "${DANG_PAM_LIBRARY}")
    target_compile_options(system_plugin_integration_test PRIVATE
      -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  endif()

  add_executable(kea_plugin_integration_test
    tests/kea_plugin_integration_test.cc)
  target_compile_features(kea_plugin_integration_test PRIVATE cxx_std_20)
  target_include_directories(kea_plugin_integration_test PRIVATE
    "${DANGD_INCLUDE_DIR}")
  target_link_libraries(kea_plugin_integration_test PRIVATE "${CMAKE_DL_LIBS}")
  target_compile_options(kea_plugin_integration_test PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

include(GNUInstallDirs)
install(TARGETS dangd_example_external_plugin
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/dangd/plugins"
  COMPONENT example)
install(TARGETS dangd_kea_plugin
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/dangd/plugins"
  COMPONENT kea)
install(FILES plugins/example/models/dang-plugins-example.yang
  DESTINATION "${CMAKE_INSTALL_DATADIR}/yang/modules"
  COMPONENT example)
install(FILES plugins/example/README.md
  DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/dangd-plugins"
  RENAME EXAMPLE.md COMPONENT example)
install(FILES
  plugins/kea/models/kea-types@2025-06-25.yang
  plugins/kea/models/kea-dhcp-types@2026-06-24.yang
  plugins/kea/models/kea-dhcp4-server@2026-06-24.yang
  plugins/kea/models/kea-dhcp6-server@2026-06-24.yang
  DESTINATION "${CMAKE_INSTALL_DATADIR}/yang/modules"
  COMPONENT kea)
install(FILES
  plugins/system/models/ietf-system@2014-08-06.yang
  plugins/system/models/iana-crypt-hash@2014-08-06.yang
  DESTINATION "${CMAKE_INSTALL_DATADIR}/yang/modules"
  COMPONENT system)
install(FILES
  plugins/rib/models/ietf-i2rs-rib@2018-09-13.yang
  plugins/rib/models/ietf-interfaces@2018-02-20.yang
  DESTINATION "${CMAKE_INSTALL_DATADIR}/yang/modules"
  COMPONENT rib)
install(FILES LICENSE README.md CHANGELOG.md PACKAGING.md
  DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/dangd-plugins"
  COMPONENT docs)
install(FILES plugins/kea/README.md
  DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/dangd-plugins"
  RENAME KEA.md COMPONENT kea)
install(FILES plugins/system/README.md
  DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/dangd-plugins"
  RENAME SYSTEM.md COMPONENT system)
install(FILES plugins/rib/README.md
  DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/dangd-plugins"
  RENAME RIB.md COMPONENT rib)
if(TARGET dangd_system_plugin)
  install(TARGETS dangd_system_plugin
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/dangd/plugins"
    COMPONENT system)
  install(TARGETS pam_dangd
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/security"
    COMPONENT pam)
  install(FILES pam/README.md
    DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/dangd-plugins"
    RENAME PAM.md COMPONENT pam)
endif()

include(packaging/Packaging.cmake)

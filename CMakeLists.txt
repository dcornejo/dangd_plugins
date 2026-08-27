# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.24)
project(dang_plugins VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
find_package(LibXml2 REQUIRED)
find_package(nlohmann_json 3.11 REQUIRED)

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

if(BUILD_TESTING)
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
  dangd_kea_plugin
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/dangd/plugins")
install(FILES plugins/example/models/dang-plugins-example.yang
  DESTINATION "${CMAKE_INSTALL_DATADIR}/yang/modules")
install(FILES
  plugins/kea/models/kea-types@2025-06-25.yang
  plugins/kea/models/kea-dhcp-types@2026-06-24.yang
  plugins/kea/models/kea-dhcp4-server@2026-06-24.yang
  plugins/kea/models/kea-dhcp6-server@2026-06-24.yang
  DESTINATION "${CMAKE_INSTALL_DATADIR}/yang/modules")

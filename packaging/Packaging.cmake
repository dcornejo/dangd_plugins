# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

set(CPACK_PACKAGE_NAME "dangd-plugins")
set(CPACK_PACKAGE_VENDOR "dang")
set(CPACK_PACKAGE_CONTACT "David Cornejo <dave@dogwood.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
  "Model plugins and PAM authentication module for dangd")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/dcornejo/dang_plugins")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_STRIP_FILES OFF)
set(CPACK_COMPONENTS_GROUPING IGNORE)
set(CPACK_COMPONENTS_ALL example kea system pam frr rib docs)

set(CPACK_COMPONENT_EXAMPLE_DISPLAY_NAME "dangd example plugin")
set(CPACK_COMPONENT_EXAMPLE_DESCRIPTION
  "Minimal external plugin used for dangd ABI development")
set(CPACK_COMPONENT_KEA_DISPLAY_NAME "dangd Kea DHCP plugin")
set(CPACK_COMPONENT_KEA_DESCRIPTION
  "Kea DHCPv4 and DHCPv6 configuration plugin for dangd")
set(CPACK_COMPONENT_SYSTEM_DISPLAY_NAME "dangd RFC 7317 system plugin")
set(CPACK_COMPONENT_SYSTEM_DESCRIPTION
  "RFC 7317 system management and authentication provider for dangd")
set(CPACK_COMPONENT_PAM_DISPLAY_NAME "dangd PAM module")
set(CPACK_COMPONENT_PAM_DESCRIPTION
  "PAM authentication module backed by the dangd system plugin")
set(CPACK_COMPONENT_PAM_DEPENDS system)
set(CPACK_COMPONENT_RIB_DISPLAY_NAME "dangd RFC 8431 RIB plugin")
set(CPACK_COMPONENT_RIB_DESCRIPTION
  "Partial RFC 8431 route configuration provider, schema, and documentation")
set(CPACK_COMPONENT_FRR_DISPLAY_NAME "dangd FRR native-model plugin")
set(CPACK_COMPONENT_FRR_DESCRIPTION
  "FRR-native routing configuration provider for dangd")
set(CPACK_COMPONENT_DOCS_DISPLAY_NAME "dangd plugin collection documentation")
set(CPACK_COMPONENT_DOCS_DESCRIPTION
  "Shared documentation and license for the dangd plugin collection")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(CPACK_GENERATOR "DEB")
  set(CPACK_DEB_COMPONENT_INSTALL ON)
  set(CPACK_DEBIAN_ENABLE_COMPONENT_DEPENDS ON)
  set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
  set(CPACK_DEBIAN_PACKAGE_SECTION "net")
  set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
  set(CPACK_DEBIAN_PACKAGE_DEPENDS "dangd (>= ${PROJECT_VERSION})")
  set(CPACK_DEBIAN_EXAMPLE_PACKAGE_NAME "dangd-plugin-example")
  set(CPACK_DEBIAN_KEA_PACKAGE_NAME "dangd-plugin-kea")
  set(CPACK_DEBIAN_SYSTEM_PACKAGE_NAME "dangd-plugin-system")
  set(CPACK_DEBIAN_PAM_PACKAGE_NAME "dangd-pam")
  set(CPACK_DEBIAN_RIB_PACKAGE_NAME "dangd-plugin-rib")
  set(CPACK_DEBIAN_RIB_PACKAGE_DEPENDS
    "dangd (>= ${PROJECT_VERSION}), iproute2")
  set(CPACK_DEBIAN_RIB_PACKAGE_CONFLICTS "dangd-plugin-frr")
  set(CPACK_DEBIAN_FRR_PACKAGE_CONFLICTS "dangd-plugin-rib")
  set(CPACK_DEBIAN_FRR_PACKAGE_NAME "dangd-plugin-frr")
  set(CPACK_DEBIAN_FRR_PACKAGE_DEPENDS "dangd (>= ${PROJECT_VERSION}), frr")
  set(CPACK_DEBIAN_DOCS_PACKAGE_NAME "dangd-plugins-doc")
  set(CPACK_DEBIAN_DOCS_PACKAGE_DEPENDS "")
  set(CPACK_DEBIAN_DOCS_PACKAGE_ARCHITECTURE "all")
elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
  set(CPACK_GENERATOR "FREEBSD")
  set(CPACK_FREEBSD_PACKAGE_LICENSE "APACHE20")
  set(CPACK_FREEBSD_PACKAGE_MAINTAINER "dave@dogwood.com")
  set(CPACK_FREEBSD_PACKAGE_ORIGIN "net-mgmt/dangd-plugins")
  set(CPACK_FREEBSD_PACKAGE_CATEGORIES "net-mgmt")
  set(CPACK_FREEBSD_PACKAGE_DEPS
    "net-mgmt/dangd;textproc/libxml2;devel/nlohmann-json")
endif()

include(CPack)

# CPack's FreeBSD generator deliberately creates only one package and has no
# component mode.  Emit a complete CPack configuration for each install
# component so native builders can create the same package split as Debian.
if(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
  set(DANG_FREEBSD_PACKAGE_COMPONENTS example kea system pam frr rib docs)
  foreach(DANG_PACKAGE_COMPONENT IN LISTS DANG_FREEBSD_PACKAGE_COMPONENTS)
    if(DANG_PACKAGE_COMPONENT STREQUAL "example")
      set(DANG_FREEBSD_PACKAGE_NAME "dangd-plugin-example")
      set(DANG_FREEBSD_PACKAGE_DEPS "net-mgmt/dangd")
    elseif(DANG_PACKAGE_COMPONENT STREQUAL "kea")
      set(DANG_FREEBSD_PACKAGE_NAME "dangd-plugin-kea")
      set(DANG_FREEBSD_PACKAGE_DEPS
        "net-mgmt/dangd;textproc/libxml2;devel/nlohmann-json")
    elseif(DANG_PACKAGE_COMPONENT STREQUAL "system")
      set(DANG_FREEBSD_PACKAGE_NAME "dangd-plugin-system")
      set(DANG_FREEBSD_PACKAGE_DEPS "net-mgmt/dangd;textproc/libxml2")
    elseif(DANG_PACKAGE_COMPONENT STREQUAL "pam")
      set(DANG_FREEBSD_PACKAGE_NAME "dangd-pam")
      set(DANG_FREEBSD_PACKAGE_DEPS "net-mgmt/dangd-plugin-system")
    elseif(DANG_PACKAGE_COMPONENT STREQUAL "rib")
      set(DANG_FREEBSD_PACKAGE_NAME "dangd-plugin-rib")
      set(DANG_FREEBSD_PACKAGE_DEPS "net-mgmt/dangd;textproc/libxml2")
    elseif(DANG_PACKAGE_COMPONENT STREQUAL "frr")
      set(DANG_FREEBSD_PACKAGE_NAME "dangd-plugin-frr")
      set(DANG_FREEBSD_PACKAGE_DEPS
        "net-mgmt/dangd;textproc/libxml2;net/frr10")
    else()
      set(DANG_FREEBSD_PACKAGE_NAME "dangd-plugins-doc")
      set(DANG_FREEBSD_PACKAGE_DEPS "")
    endif()
    configure_file(
      "${CMAKE_CURRENT_SOURCE_DIR}/packaging/CPackFreeBSDComponent.cmake.in"
      "${CMAKE_CURRENT_BINARY_DIR}/CPackFreeBSD-${DANG_PACKAGE_COMPONENT}.cmake"
      @ONLY)
  endforeach()
endif()

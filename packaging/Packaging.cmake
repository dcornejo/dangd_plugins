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
set(CPACK_MONOLITHIC_INSTALL ON)
set(CPACK_STRIP_FILES OFF)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(CPACK_GENERATOR "DEB")
  set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
  set(CPACK_DEBIAN_PACKAGE_SECTION "net")
  set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
  set(CPACK_DEBIAN_PACKAGE_DEPENDS "dangd (>= ${PROJECT_VERSION})")
elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
  set(CPACK_GENERATOR "FREEBSD")
  set(CPACK_PACKAGE_FILE_NAME "dangd-plugins-${PROJECT_VERSION}")
  set(CPACK_FREEBSD_PACKAGE_LICENSE "APACHE20")
  set(CPACK_FREEBSD_PACKAGE_MAINTAINER "dave@dogwood.com")
  set(CPACK_FREEBSD_PACKAGE_ORIGIN "net-mgmt/dangd-plugins")
  set(CPACK_FREEBSD_PACKAGE_CATEGORIES "net-mgmt")
  set(CPACK_FREEBSD_PACKAGE_DEPS
    "net-mgmt/dangd;textproc/libxml2;devel/nlohmann-json")
endif()

include(CPack)

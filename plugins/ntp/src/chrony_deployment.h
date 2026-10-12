// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0
#ifndef DANG_PLUGINS_NTP_CHRONY_DEPLOYMENT_H_
#define DANG_PLUGINS_NTP_CHRONY_DEPLOYMENT_H_
#include <sys/types.h>

#include <string>
#include <vector>

#include "plugins/common/systemd_unit.h"
namespace dang::ntp {
/** Check a strict direct chronyd argv; reject wrappers, substitutions and
 * extras. */
bool ValidateChronyInvocation(const std::string& executable,
                              const std::vector<std::string>& arguments,
                              const std::string& expected_binary,
                              const std::string& wrapper, bool isolated,
                              std::string* error);
/**
 * Open every path component with O_NOFOLLOW, check ownership and writability,
 * then read the wrapper through its validated descriptor (maximum 1 MiB).
 * The root/wrapper/fragment/key files belong to owner. Ancestors belong to
 * root or owner; sticky system temporary ancestors are allowed, but the
 * dedicated root itself must not be group/other-writable. Private keys must
 * have no group/other permissions. Optional managed files may be absent.
 * This preflight is not a filesystem lock against later privileged edits.
 */
bool ValidateChronyTree(const std::string& root, uid_t owner,
                        std::string* wrapper, std::string* error);
/** Verify an owned, non-writable-by-others regular executable and ancestors. */
bool ValidateChronyBinary(const std::string& path, uid_t owner,
                          std::string* error);
/** Dedicated test-unit policy; reject auxiliary hooks, drop-ins and overrides.
 */
bool ValidateChronyUnitPolicy(const native::UnitPolicy& policy,
                              const std::string& expected_fragment,
                              std::string* error);
/** Check the ownership/type/permissions of a service definition file. */
bool ValidateChronyDefinition(const std::string& path, uid_t owner,
                              std::string* error);
}  // namespace dang::ntp
#endif

// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANG_PLUGINS_SYSTEM_FREEBSD_SERVICE_RUNNER_H_
#define DANG_PLUGINS_SYSTEM_FREEBSD_SERVICE_RUNNER_H_

#include <string>

namespace dang::system {

/** Converts a completed waitpid(2) status into success or a precise error. */
[[nodiscard]] bool InterpretFreeBsdServiceStatus(int status,
                                                 std::string* error);

/** Runs the fixed FreeBSD ntpd rc.d operation without a shell command string. */
[[nodiscard]] bool RunFreeBsdNtpService(bool enabled, std::string* error);

}  // namespace dang::system

#endif  // DANG_PLUGINS_SYSTEM_FREEBSD_SERVICE_RUNNER_H_

// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0
#ifndef DANG_PLUGINS_NTP_CHRONY_PREFLIGHT_H_
#define DANG_PLUGINS_NTP_CHRONY_PREFLIGHT_H_
#include <string>
#include <string_view>
namespace dang::ntp {
/**
 * Validate a dedicated wrapper's ownership boundary before service mutation.
 * Require one literal managed include; reject alternate includes, source
 * directories, unmanaged sources/keys/listeners/local clocks and unknown
 * directives. Infrastructure directives have no modeled source semantics.
 * Disposable tests may additionally bind/allow only IPv4 loopback. This text
 * check does not prove the service's ExecStart selects the wrapper; native
 * command-line, directory ownership and readback checks remain required.
 */
bool ValidateChronyWrapper(std::string_view text, std::string_view fragment,
                           bool isolated_test, std::string* error);
}  // namespace dang::ntp
#endif

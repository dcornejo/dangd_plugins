// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANG_PLUGINS_NTP_CHRONY_CONFIG_H_
#define DANG_PLUGINS_NTP_CHRONY_CONFIG_H_

#include <string>
#include <string_view>

#include "plugins/ntp/src/ntp_config.h"

namespace dang::ntp {

/** Complete managed chrony files produced from one RFC 9249 snapshot. */
struct ChronyConfigImage {
  /** Text included by the host chrony configuration directory. */
  std::string fragment;
  /** Root-only symmetric-key file referenced by `fragment`. */
  std::string key_file;
};

/**
 * Validate and translate the supported RFC 9249 subset to chrony syntax.
 *
 * This function is side-effect free. It deliberately rejects model controls
 * for which chrony has no equivalent semantics instead of weakening intent.
 * Authentication key bytes are returned only in `image->key_file`; errors
 * never contain them.
 */
[[nodiscard]] bool RenderChronyConfig(const Config& config,
                                      std::string_view key_file_path,
                                      ChronyConfigImage* image,
                                      std::string* error,
                                      std::string* error_path);

}  // namespace dang::ntp

#endif  // DANG_PLUGINS_NTP_CHRONY_CONFIG_H_

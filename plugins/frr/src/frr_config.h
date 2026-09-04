// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANG_PLUGINS_FRR_FRR_CONFIG_H_
#define DANG_PLUGINS_FRR_FRR_CONFIG_H_

#include "frr_transaction.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dang::plugins::frr {

/** Identifies one top-level configuration container owned by an FRR module. */
struct RootDescriptor {
  std::string module_name;
  std::string namespace_uri;
  std::string local_name;
  std::string xpath;
};

/**
 * Extracts paired before/proposed XML fragments for every descriptor.
 *
 * An absent container is represented by std::nullopt and therefore remains
 * distinguishable from a present empty container.  On malformed XML or an
 * ambiguous duplicate root, returns std::nullopt and supplies both a diagnostic
 * and the affected model path.
 */
std::optional<std::vector<ConfigurationRoot>> ExtractConfigurationRoots(
    std::string_view before_xml, std::string_view proposed_xml,
    const std::vector<RootDescriptor>& descriptors, std::string* error,
    std::string* error_path);

/**
 * Replaces only described top-level roots in a complete datastore snapshot.
 * Each observed entry corresponds by index to descriptors; std::nullopt means
 * the root is absent from FRR running state.
 */
std::optional<std::string> ReconcileConfigurationRoots(
    std::string_view current_xml,
    const std::vector<RootDescriptor>& descriptors,
    const std::vector<std::optional<std::string>>& observed,
    std::string* error, std::string* error_path);

/** Compares optional XML roots semantically, independent of prefix spelling. */
std::optional<bool> EquivalentConfigurationRoot(
    const std::optional<std::string>& expected,
    const std::optional<std::string>& observed, std::string* error);

}  // namespace dang::plugins::frr

#endif  // DANG_PLUGINS_FRR_FRR_CONFIG_H_

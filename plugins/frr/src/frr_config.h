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

struct RootDescriptor {
  std::string module_name;
  std::string namespace_uri;
  std::string local_name;
  std::string xpath;
};

std::optional<std::vector<ConfigurationRoot>> ExtractConfigurationRoots(
    std::string_view before_xml, std::string_view proposed_xml,
    const std::vector<RootDescriptor>& descriptors, std::string* error,
    std::string* error_path);

}  // namespace dang::plugins::frr

#endif  // DANG_PLUGINS_FRR_FRR_CONFIG_H_

// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANG_PLUGINS_RIB_RIB_MAPPING_H_
#define DANG_PLUGINS_RIB_RIB_MAPPING_H_

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "plugins/rib/src/platform_executor.h"

namespace dang::rib {

/** Strict, bidirectional mapping between YANG RIB names and native numbers. */
class RibMapping {
 public:
  [[nodiscard]] bool Add(std::string modeled_name, NativePlatform platform,
                         std::uint32_t native_number, std::string* error);
  /** Returns a configured mapping, or an unchanged numeric modeled name. */
  [[nodiscard]] std::optional<std::string> ToNative(
      std::string_view modeled_name, NativePlatform platform) const;
  /** Returns the unique modeled alias, or an unchanged native number. */
  [[nodiscard]] std::string ToModeled(std::string_view native_name,
                                     NativePlatform platform) const;

 private:
  using ForwardKey = std::pair<NativePlatform, std::string>;
  using ReverseKey = std::pair<NativePlatform, std::uint32_t>;
  std::map<ForwardKey, std::uint32_t> forward_;
  std::map<ReverseKey, std::string> reverse_;
};

/** Loads a bounded versioned JSON mapping; a missing optional path is empty. */
[[nodiscard]] bool LoadRibMapping(const std::filesystem::path& path,
                                  RibMapping* mapping, std::string* error);

}  // namespace dang::rib

#endif  // DANG_PLUGINS_RIB_RIB_MAPPING_H_

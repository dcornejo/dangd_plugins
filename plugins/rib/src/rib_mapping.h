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
#include <tuple>
#include <vector>

#include "plugins/rib/src/platform_executor.h"
#include "plugins/rib/src/rib_persistence.h"

namespace dang::rib {

/** Strict, bidirectional mapping between YANG RIB names and native numbers. */
class RibMapping {
 public:
  [[nodiscard]] bool Add(std::string modeled_name, std::string address_family,
                         NativePlatform platform, std::uint32_t native_number,
                         std::string* error);
  /** Resolves an alias or a family-qualified built-in name to its number. */
  [[nodiscard]] std::optional<std::string> ToNative(
      std::string_view modeled_name, NativePlatform platform,
      std::string_view address_family = {}) const;
  /** Returns the unique alias or a family-qualified built-in modeled name. */
  [[nodiscard]] std::string ToModeled(std::string_view native_name,
                                     NativePlatform platform,
                                     std::string_view address_family) const;
  /** Returns the sorted native numbers explicitly configured for a platform. */
  [[nodiscard]] std::vector<std::uint32_t> NativeNumbers(
      NativePlatform platform) const;

 private:
  using ForwardKey = std::pair<NativePlatform, std::string>;
  using ForwardValue = std::pair<std::uint32_t, std::string>;
  using ReverseKey =
      std::tuple<NativePlatform, std::uint32_t, std::string>;
  std::map<ForwardKey, ForwardValue> forward_;
  std::map<ReverseKey, std::string> reverse_;
};

/** Rejects durable modeled RIB identities that this mapping cannot resolve. */
[[nodiscard]] bool ValidateRegistryRibMappings(
    const PersistentRegistry& registry, const RibMapping& mapping,
    NativePlatform platform, std::string* error);

/** Loads a bounded versioned JSON mapping; a missing optional path is empty. */
[[nodiscard]] bool LoadRibMapping(const std::filesystem::path& path,
                                  RibMapping* mapping, std::string* error);

}  // namespace dang::rib

#endif  // DANG_PLUGINS_RIB_RIB_MAPPING_H_

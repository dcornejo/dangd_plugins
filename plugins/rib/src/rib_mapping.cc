// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/** @file Strict loading and lookup for modeled-to-native RIB names. */

#include "plugins/rib/src/rib_mapping.h"

#include <charconv>
#include <fstream>
#include <system_error>

#include <nlohmann/json.hpp>

namespace dang::rib {
namespace {
constexpr std::uintmax_t kMaximumMappingBytes = 1024U * 1024U;

bool Fail(std::string message, std::string* error) {
  if (error) *error = std::move(message);
  return false;
}

std::optional<std::uint32_t> Number(std::string_view value) {
  std::uint32_t number = 0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(),
                                      number);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
    return std::nullopt;
  return number;
}
}  // namespace

bool RibMapping::Add(std::string modeled_name, NativePlatform platform,
                     std::uint32_t native_number, std::string* error) {
  if (modeled_name.empty()) return Fail("modeled RIB name is empty", error);
  const ForwardKey forward_key{platform, modeled_name};
  const ReverseKey reverse_key{platform, native_number};
  if (const auto existing = forward_.find(forward_key);
      existing != forward_.end())
    return existing->second == native_number
               ? true
               : Fail("modeled RIB name has conflicting native mappings", error);
  if (reverse_.contains(reverse_key))
    return Fail("native RIB number has multiple modeled names", error);
  forward_.emplace(forward_key, native_number);
  reverse_.emplace(reverse_key, std::move(modeled_name));
  return true;
}

std::optional<std::string> RibMapping::ToNative(
    std::string_view modeled_name, NativePlatform platform) const {
  const auto configured = forward_.find({platform, std::string(modeled_name)});
  if (configured != forward_.end()) return std::to_string(configured->second);
  return Number(modeled_name) ? std::optional(std::string(modeled_name))
                              : std::nullopt;
}

std::string RibMapping::ToModeled(std::string_view native_name,
                                  NativePlatform platform) const {
  const auto number = Number(native_name);
  if (!number) return std::string(native_name);
  const auto configured = reverse_.find({platform, *number});
  return configured == reverse_.end() ? std::string(native_name)
                                      : configured->second;
}

bool LoadRibMapping(const std::filesystem::path& path, RibMapping* mapping,
                    std::string* error) {
  if (!mapping) return false;
  if (path.empty()) {
    *mapping = RibMapping{};
    return true;
  }
  std::error_code filesystem_error;
  if (!std::filesystem::is_regular_file(path, filesystem_error) ||
      filesystem_error)
    return Fail("RIB mapping is not a readable regular file", error);
  const auto size = std::filesystem::file_size(path, filesystem_error);
  if (filesystem_error || size > kMaximumMappingBytes)
    return Fail("RIB mapping exceeds the 1 MiB limit", error);
  try {
    std::ifstream input(path);
    const nlohmann::json document = nlohmann::json::parse(input);
    if (document.at("version") != 1)
      return Fail("unsupported RIB mapping version", error);
    RibMapping loaded;
    for (const auto& item : document.at("ribs")) {
      const std::string name = item.at("name").get<std::string>();
      if (item.contains("linux-table") &&
          !loaded.Add(name, NativePlatform::kLinux,
                      item.at("linux-table").get<std::uint32_t>(), error))
        return false;
      if (item.contains("freebsd-fib") &&
          !loaded.Add(name, NativePlatform::kFreeBsd,
                      item.at("freebsd-fib").get<std::uint32_t>(), error))
        return false;
      if (!item.contains("linux-table") && !item.contains("freebsd-fib"))
        return Fail("RIB mapping entry has no platform number", error);
    }
    *mapping = std::move(loaded);
    return true;
  } catch (const std::exception& exception) {
    return Fail("cannot parse RIB mapping: " + std::string(exception.what()),
                error);
  }
}

}  // namespace dang::rib

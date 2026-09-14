// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/** @file Strict loading and lookup for modeled-to-native RIB names. */

#include "plugins/rib/src/rib_mapping.h"

#include <algorithm>
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

std::optional<std::uint32_t> QualifiedNumber(
    std::string_view value, std::string_view requested_family) {
  const auto separator = value.find('-');
  if (separator == std::string_view::npos) return std::nullopt;
  const auto family = value.substr(0, separator);
  if (family != "ipv4" && family != "ipv6") return std::nullopt;
  if (!requested_family.empty() && family != requested_family)
    return std::nullopt;
  return Number(value.substr(separator + 1));
}
}  // namespace

bool RibMapping::Add(std::string modeled_name, std::string address_family,
                     NativePlatform platform, std::uint32_t native_number,
                     std::string* error) {
  if (modeled_name.empty()) return Fail("modeled RIB name is empty", error);
  if (address_family != "ipv4" && address_family != "ipv6")
    return Fail("modeled RIB address family must be ipv4 or ipv6", error);
  const ForwardKey forward_key{platform, modeled_name};
  const ForwardValue forward_value{native_number, address_family};
  const ReverseKey reverse_key{platform, native_number, address_family};
  if (const auto existing = forward_.find(forward_key);
      existing != forward_.end())
    return existing->second == forward_value
               ? true
               : Fail("modeled RIB name has conflicting native mappings", error);
  if (reverse_.contains(reverse_key))
    return Fail("native RIB number has multiple modeled names", error);
  forward_.emplace(forward_key, std::move(forward_value));
  reverse_.emplace(reverse_key, std::move(modeled_name));
  return true;
}

std::optional<std::string> RibMapping::ToNative(
    std::string_view modeled_name, NativePlatform platform,
    std::string_view address_family) const {
  const auto configured = forward_.find({platform, std::string(modeled_name)});
  if (configured != forward_.end()) {
    if (!address_family.empty() && configured->second.second != address_family)
      return std::nullopt;
    return std::to_string(configured->second.first);
  }
  const auto number = QualifiedNumber(modeled_name, address_family);
  return number ? std::optional(std::to_string(*number)) : std::nullopt;
}

std::string RibMapping::ToModeled(std::string_view native_name,
                                  NativePlatform platform,
                                  std::string_view address_family) const {
  const auto number = Number(native_name);
  if (!number) return std::string(native_name);
  const auto configured = reverse_.find(
      {platform, *number, std::string(address_family)});
  if (configured != reverse_.end()) return configured->second;
  return address_family == "ipv4" || address_family == "ipv6"
             ? std::string(address_family) + "-" + std::string(native_name)
             : std::string(native_name);
}

std::vector<std::uint32_t> RibMapping::NativeNumbers(
    NativePlatform platform) const {
  std::vector<std::uint32_t> result;
  for (const auto& [key, modeled_name] : reverse_) {
    (void)modeled_name;
    if (std::get<0>(key) == platform) result.push_back(std::get<1>(key));
  }
  std::ranges::sort(result);
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
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
    if (document.at("version") != 2)
      return Fail("unsupported RIB mapping version", error);
    RibMapping loaded;
    for (const auto& item : document.at("ribs")) {
      const std::string name = item.at("name").get<std::string>();
      const std::string family = item.at("address-family").get<std::string>();
      if (item.contains("linux-table") &&
          !loaded.Add(name, family, NativePlatform::kLinux,
                      item.at("linux-table").get<std::uint32_t>(), error))
        return false;
      if (item.contains("freebsd-fib") &&
          !loaded.Add(name, family, NativePlatform::kFreeBsd,
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

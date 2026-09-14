// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/** @file Crash-safe persistence for RFC 8431 reusable nexthop objects. */

#include "plugins/rib/src/rib_persistence.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <set>

#include <nlohmann/json.hpp>

namespace dang::rib {
namespace {
constexpr std::uintmax_t kMaximumBytes = 16U * 1024U * 1024U;

bool Fail(std::string value, std::string* error) {
  if (error) *error = std::move(value);
  return false;
}

nlohmann::json OptionalJson(const std::optional<std::string>& value) {
  return value ? nlohmann::json(*value) : nlohmann::json(nullptr);
}

nlohmann::json Encode(const PersistentRegistry& value) {
  nlohmann::json result{{"version", 2}, {"next-id", value.next_id},
                        {"ribs", nlohmann::json::array()},
                        {"nexthops", nlohmann::json::array()},
                        {"bindings", nlohmann::json::array()}};
  for (const auto& item : value.ribs)
    result["ribs"].push_back(
        {{"name", item.name}, {"address-family", item.address_family}});
  for (const auto& item : value.nexthops)
    result["nexthops"].push_back({{"rib", item.rib}, {"id", item.id},
      {"gateway", OptionalJson(item.gateway)},
      {"interface", OptionalJson(item.interface)},
      {"address-family", OptionalJson(item.address_family)},
      {"sharable", item.sharable}});
  for (const auto& item : value.bindings)
    result["bindings"].push_back({{"rib", item.rib},
      {"address-family", item.address_family}, {"destination", item.destination},
      {"route-index", item.route_index},
      {"nexthop-id", item.nexthop_id}});
  return result;
}

bool WriteAll(int descriptor, std::string_view data, std::string* error) {
  while (!data.empty()) {
    const ssize_t written = write(descriptor, data.data(), data.size());
    if (written < 0) {
      if (errno == EINTR) continue;
      return Fail("cannot write registry state: " + std::string(std::strerror(errno)), error);
    }
    data.remove_prefix(static_cast<std::size_t>(written));
  }
  return true;
}

std::optional<std::string> OptionalString(const nlohmann::json& value) {
  return value.is_null() ? std::nullopt
                         : std::optional(value.get<std::string>());
}
}  // namespace

bool SaveRegistry(const std::filesystem::path& path,
                  const PersistentRegistry& registry, std::string* error) {
  if (path.empty()) return Fail("registry state path is empty", error);
  const std::string data = Encode(registry).dump(2) + "\n";
  const auto temporary = path.string() + ".tmp." + std::to_string(getpid());
  const int descriptor = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (descriptor < 0)
    return Fail("cannot create registry temporary file: " +
                std::string(std::strerror(errno)), error);
  bool ok = WriteAll(descriptor, data, error);
  if (ok && fsync(descriptor) != 0)
    ok = Fail("cannot synchronize registry temporary file: " +
              std::string(std::strerror(errno)), error);
  if (close(descriptor) != 0 && ok)
    ok = Fail("cannot close registry temporary file", error);
  if (ok && rename(temporary.c_str(), path.c_str()) != 0)
    ok = Fail("cannot replace registry state: " + std::string(std::strerror(errno)), error);
  if (!ok) unlink(temporary.c_str());
  if (!ok) return false;
  const int directory = open(path.parent_path().empty() ? "." : path.parent_path().c_str(), O_RDONLY);
  if (directory < 0 || fsync(directory) != 0) {
    if (directory >= 0) close(directory);
    return Fail("cannot synchronize registry state directory", error);
  }
  close(directory);
  return true;
}

bool LoadRegistry(const std::filesystem::path& path, PersistentRegistry* registry,
                  std::string* error) {
  if (!registry) return false;
  std::error_code filesystem_error;
  if (!std::filesystem::exists(path, filesystem_error)) {
    if (filesystem_error) return Fail("cannot inspect registry state", error);
    *registry = {};
    return true;
  }
  const auto status = std::filesystem::status(path, filesystem_error);
  if (filesystem_error || !std::filesystem::is_regular_file(status))
    return Fail("registry state is not a regular file", error);
  struct stat metadata {};
  if (stat(path.c_str(), &metadata) != 0 || (metadata.st_mode & 0077) != 0)
    return Fail("registry state must not be accessible by group or other", error);
  const auto size = std::filesystem::file_size(path, filesystem_error);
  if (filesystem_error || size > kMaximumBytes)
    return Fail("registry state exceeds the 16 MiB limit", error);
  try {
    std::ifstream input(path);
    nlohmann::json json = nlohmann::json::parse(input);
    const unsigned version = json.at("version").get<unsigned>();
    if (version != 1 && version != 2)
      return Fail("unsupported registry state version", error);
    PersistentRegistry loaded;
    loaded.next_id = json.at("next-id").get<std::uint32_t>();
    if (loaded.next_id == 0) return Fail("registry next-id must be nonzero", error);
    std::set<std::string> rib_names;
    // The field is optional so version-1 files written by older builds remain
    // readable. Such files simply have no modeled family for interface-only
    // nexthops until rib-add is invoked again. Version-1 route bindings also
    // receive index zero below because that format could retain only one
    // binding per RIB/family/prefix.
    for (const auto& item : json.value("ribs", nlohmann::json::array())) {
      PersistentRib value{item.at("name").get<std::string>(),
                          item.at("address-family").get<std::string>()};
      if (value.name.empty() ||
          (value.address_family != "ipv4" && value.address_family != "ipv6") ||
          !rib_names.emplace(value.name).second)
        return Fail("registry contains an invalid or duplicate RIB", error);
      loaded.ribs.push_back(std::move(value));
    }
    std::set<std::pair<std::string, std::uint32_t>> ids;
    for (const auto& item : json.at("nexthops")) {
      PersistentNexthop value{item.at("rib").get<std::string>(), item.at("id").get<std::uint32_t>(),
        OptionalString(item.at("gateway")), OptionalString(item.at("interface")),
        OptionalString(item.at("address-family")), item.at("sharable").get<bool>()};
      if (value.rib.empty() || value.id == 0 || !ids.emplace(value.rib, value.id).second)
        return Fail("registry contains an invalid or duplicate nexthop", error);
      loaded.nexthops.push_back(std::move(value));
    }
    std::set<std::tuple<std::string, std::string, std::string, std::uint64_t>> routes;
    for (const auto& item : json.at("bindings")) {
      PersistentRouteBinding value{item.at("rib").get<std::string>(), item.at("address-family").get<std::string>(),
        item.at("destination").get<std::string>(),
        version == 1 ? 0U : item.at("route-index").get<std::uint64_t>(),
        item.at("nexthop-id").get<std::uint32_t>()};
      if (!ids.contains({value.rib, value.nexthop_id}) || value.destination.empty() ||
          !routes.emplace(value.rib, value.address_family, value.destination,
                          value.route_index).second)
        return Fail("registry contains an invalid route binding", error);
      loaded.bindings.push_back(std::move(value));
    }
    *registry = std::move(loaded);
    return true;
  } catch (const std::exception& exception) {
    return Fail("cannot parse registry state: " + std::string(exception.what()), error);
  }
}

}  // namespace dang::rib

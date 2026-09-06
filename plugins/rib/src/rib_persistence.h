// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANG_PLUGINS_RIB_RIB_PERSISTENCE_H_
#define DANG_PLUGINS_RIB_RIB_PERSISTENCE_H_

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace dang::rib {

/** One durable reusable nexthop, independent of transient reference counts. */
struct PersistentNexthop {
  std::string rib;
  std::uint32_t id = 0;
  std::optional<std::string> gateway;
  std::optional<std::string> interface;
  std::optional<std::string> address_family;
  bool sharable = false;
  bool operator==(const PersistentNexthop&) const = default;
};

/** One imperative route-to-nexthop binding retained across restart. */
struct PersistentRouteBinding {
  std::string rib;
  std::string address_family;
  std::string destination;
  std::uint32_t nexthop_id = 0;
  bool operator==(const PersistentRouteBinding&) const = default;
};

struct PersistentRegistry {
  std::uint32_t next_id = 1;
  std::vector<PersistentNexthop> nexthops;
  std::vector<PersistentRouteBinding> bindings;
  bool operator==(const PersistentRegistry&) const = default;
};

/** Atomically writes a private, versioned registry sidecar. */
[[nodiscard]] bool SaveRegistry(const std::filesystem::path& path,
                                const PersistentRegistry& registry,
                                std::string* error);

/** Loads a private sidecar; a missing file is an empty registry. */
[[nodiscard]] bool LoadRegistry(const std::filesystem::path& path,
                                PersistentRegistry* registry,
                                std::string* error);

}  // namespace dang::rib

#endif

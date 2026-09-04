// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANG_PLUGINS_FRR_SCHEMA_INVENTORY_H_
#define DANG_PLUGINS_FRR_SCHEMA_INVENTORY_H_

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace dang::plugins::frr {

/** Installed YANG module metadata plus its immutable source text. */
struct YangSchema {
  std::string module_name;
  std::string revision;
  std::string namespace_uri;
  std::vector<std::string> imports;
  std::string source;
  std::filesystem::path path;
};

/** Discovery roots and denial-of-service bounds for schema loading. */
struct SchemaInventoryOptions {
  std::optional<std::filesystem::path> explicit_directory;
  std::vector<std::filesystem::path> search_directories{
      "/usr/share/yang", "/usr/local/share/yang"};
  std::vector<std::filesystem::path> supplemental_directories;
  std::size_t maximum_source_bytes = 4 * 1024 * 1024;
  std::size_t maximum_total_bytes = 64 * 1024 * 1024;
};

// Loads one self-consistent, installed FRR YANG directory plus standard-model
// dependency directories. Search directories are alternatives, not an overlay:
// combining schemas from two FRR installations could create a model set that
// corresponds to neither running daemon.
std::optional<std::vector<YangSchema>> DiscoverSchemaInventory(
    const SchemaInventoryOptions& options, std::string* error);

// Returns the transitive import closure for roots, in inventory order.
std::optional<std::vector<std::size_t>> ResolveImportClosure(
    const std::vector<YangSchema>& inventory,
    const std::vector<std::string>& roots, std::string* error);

}  // namespace dang::plugins::frr

#endif  // DANG_PLUGINS_FRR_SCHEMA_INVENTORY_H_

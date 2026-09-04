// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "schema_inventory.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <set>
#include <string_view>
#include <unordered_map>

namespace dang::plugins::frr {
namespace {

std::vector<std::string> Tokens(std::string_view source, std::string* error) {
  std::vector<std::string> result;
  for (std::size_t offset = 0; offset < source.size();) {
    const char byte = source[offset];
    if (byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n') {
      ++offset;
      continue;
    }
    if (byte == '/' && offset + 1 < source.size() && source[offset + 1] == '/') {
      offset = source.find('\n', offset + 2);
      if (offset == std::string_view::npos) break;
      continue;
    }
    if (byte == '/' && offset + 1 < source.size() && source[offset + 1] == '*') {
      const std::size_t end = source.find("*/", offset + 2);
      if (end == std::string_view::npos) {
        if (error) *error = "unterminated block comment";
        return {};
      }
      offset = end + 2;
      continue;
    }
    if (byte == '"' || byte == '\'') {
      const char quote = byte;
      ++offset;
      bool closed = false;
      while (offset < source.size()) {
        if (source[offset] == quote) {
          ++offset;
          closed = true;
          break;
        }
        if (quote == '"' && source[offset] == '\\' && offset + 1 < source.size())
          offset += 2;
        else
          ++offset;
      }
      if (!closed) {
        if (error) *error = "unterminated quoted string";
        return {};
      }
      result.emplace_back("<string>");
      continue;
    }
    if (byte == '{' || byte == '}' || byte == ';') {
      result.emplace_back(1, byte);
      ++offset;
      continue;
    }
    const std::size_t begin = offset;
    while (offset < source.size()) {
      const char current = source[offset];
      if (current == ' ' || current == '\t' || current == '\r' ||
          current == '\n' || current == '{' || current == '}' ||
          current == ';' || current == '"' || current == '\'')
        break;
      if (current == '/' && offset + 1 < source.size() &&
          (source[offset + 1] == '/' || source[offset + 1] == '*'))
        break;
      ++offset;
    }
    if (offset == begin) {
      if (error) *error = "invalid token near byte " + std::to_string(offset);
      return {};
    }
    result.emplace_back(source.substr(begin, offset - begin));
  }
  return result;
}

std::optional<YangSchema> Parse(const std::filesystem::path& path,
                                std::string source, std::string* error) {
  std::string token_error;
  const std::vector<std::string> tokens = Tokens(source, &token_error);
  if (!token_error.empty()) {
    if (error) *error = path.string() + ": " + token_error;
    return std::nullopt;
  }
  if (tokens.size() < 3 ||
      (tokens[0] != "module" && tokens[0] != "submodule") ||
      tokens[2] != "{") {
    if (error) *error = path.string() + ": missing module declaration";
    return std::nullopt;
  }
  YangSchema schema;
  schema.module_name = tokens[1];
  schema.source = std::move(source);
  schema.path = path;
  int depth = 1;
  for (std::size_t index = 3; index < tokens.size(); ++index) {
    if (tokens[index] == "{") {
      ++depth;
      continue;
    }
    if (tokens[index] == "}") {
      --depth;
      continue;
    }
    if (depth != 1 || index + 1 >= tokens.size()) continue;
    if (tokens[index] == "revision" && schema.revision.empty())
      schema.revision = tokens[index + 1] == "<string>" ? "" : tokens[index + 1];
    if (tokens[index] == "import" && tokens[index + 1] != "<string>")
      schema.imports.push_back(tokens[index + 1]);
  }
  if (depth != 0) {
    if (error) *error = path.string() + ": unbalanced module braces";
    return std::nullopt;
  }
  return schema;
}

}  // namespace

std::optional<std::vector<YangSchema>> DiscoverSchemaInventory(
    const SchemaInventoryOptions& options, std::string* error) {
  const std::vector<std::filesystem::path> candidates =
      options.explicit_directory
      ? std::vector<std::filesystem::path>{*options.explicit_directory}
      : options.search_directories;
  std::filesystem::path selected;
  for (const auto& candidate : candidates) {
    std::error_code status_error;
    if (std::filesystem::is_directory(candidate, status_error)) {
      selected = candidate;
      break;
    }
  }
  if (selected.empty()) {
    if (error) *error = "no installed FRR YANG directory was found";
    return std::nullopt;
  }
  std::vector<std::filesystem::path> directories{selected};
  if (options.supplemental_directories.empty())
    directories.push_back(selected / "modules" / "libyang");
  else
    directories.insert(directories.end(), options.supplemental_directories.begin(),
                       options.supplemental_directories.end());
  std::vector<std::filesystem::path> paths;
  for (const auto& directory : directories) {
    std::error_code exists_error;
    if (!std::filesystem::is_directory(directory, exists_error)) continue;
    std::error_code iteration_error;
    for (std::filesystem::directory_iterator iterator(directory, iteration_error),
         end;
         !iteration_error && iterator != end;
         iterator.increment(iteration_error)) {
      std::error_code type_error;
      if (!iterator->is_regular_file(type_error) || type_error ||
          iterator->path().extension() != ".yang")
        continue;
      paths.push_back(iterator->path());
    }
    if (iteration_error) {
      if (error) *error = directory.string() + ": " + iteration_error.message();
      return std::nullopt;
    }
  }
  std::ranges::sort(paths);
  std::vector<YangSchema> inventory;
  std::set<std::string> names;
  std::size_t total = 0;
  for (const auto& path : paths) {
    std::error_code size_error;
    const auto bytes = std::filesystem::file_size(path, size_error);
    if (size_error || bytes > options.maximum_source_bytes ||
        bytes > options.maximum_total_bytes - total) {
      if (error) *error = path.string() + ": YANG source size limit exceeded";
      return std::nullopt;
    }
    std::ifstream input(path, std::ios::binary);
    std::string source{std::istreambuf_iterator<char>(input), {}};
    if (!input.good() && !input.eof()) {
      if (error) *error = path.string() + ": cannot read YANG source";
      return std::nullopt;
    }
    auto schema = Parse(path, std::move(source), error);
    if (!schema) return std::nullopt;
    if (!names.insert(schema->module_name).second) {
      if (error) *error = "duplicate YANG module " + schema->module_name;
      return std::nullopt;
    }
    total += static_cast<std::size_t>(bytes);
    inventory.push_back(std::move(*schema));
  }
  if (inventory.empty()) {
    if (error) *error = selected.string() + ": contains no YANG sources";
    return std::nullopt;
  }
  return inventory;
}

std::optional<std::vector<std::size_t>> ResolveImportClosure(
    const std::vector<YangSchema>& inventory,
    const std::vector<std::string>& roots, std::string* error) {
  std::unordered_map<std::string, std::size_t> indexes;
  for (std::size_t index = 0; index < inventory.size(); ++index)
    indexes.emplace(inventory[index].module_name, index);
  std::set<std::size_t> selected;
  std::vector<std::string> pending = roots;
  while (!pending.empty()) {
    std::string name = std::move(pending.back());
    pending.pop_back();
    const auto found = indexes.find(name);
    if (found == indexes.end()) {
      if (error) *error = "required YANG module " + name + " is not installed";
      return std::nullopt;
    }
    if (!selected.insert(found->second).second) continue;
    for (const std::string& dependency : inventory[found->second].imports)
      pending.push_back(dependency);
  }
  return std::vector<std::size_t>(selected.begin(), selected.end());
}

}  // namespace dang::plugins::frr

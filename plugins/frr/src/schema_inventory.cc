// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/**
 * @file
 * Discovers an installed, internally consistent FRR schema set and computes
 * import closure without trying to implement a complete YANG parser.  The
 * tokenizer understands enough lexical structure to ignore comments and
 * quoted text safely while reading module metadata and imports.
 */

#include "schema_inventory.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <set>
#include <string_view>
#include <unordered_map>

#include <libxml/parser.h>
#include <libxml/tree.h>

namespace dang::plugins::frr {
namespace {

using Document = std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)>;

std::string Text(const xmlNode* node) {
  xmlChar* value = node ? xmlNodeGetContent(node) : nullptr;
  std::string result = value ? reinterpret_cast<const char*>(value) : "";
  if (value) xmlFree(value);
  return result;
}

const xmlNode* Child(const xmlNode* parent, std::string_view name) {
  for (const xmlNode* child = parent ? parent->children : nullptr; child;
       child = child->next)
    if (child->type == XML_ELEMENT_NODE &&
        name == reinterpret_cast<const char*>(child->name))
      return child;
  return nullptr;
}

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
      const std::size_t content_begin = offset;
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
      result.emplace_back(1, '\1');
      result.back().append(source.substr(content_begin, offset - content_begin - 1));
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
  schema.is_submodule = tokens[0] == "submodule";
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
    const bool quoted = !tokens[index + 1].empty() && tokens[index + 1][0] == '\1';
    const std::string value =
        quoted ? tokens[index + 1].substr(1) : tokens[index + 1];
    if (tokens[index] == "revision" && schema.revision.empty())
      schema.revision = value;
    if (tokens[index] == "namespace" && schema.namespace_uri.empty())
      schema.namespace_uri = value;
    if (tokens[index] == "import")
      schema.imports.push_back(value);
    if (tokens[index] == "include")
      schema.includes.push_back(value);
    if (tokens[index] == "belongs-to" && schema.belongs_to.empty())
      schema.belongs_to = value;
  }
  if (depth != 0) {
    if (error) *error = path.string() + ": unbalanced module braces";
    return std::nullopt;
  }
  if (schema.is_submodule && schema.belongs_to.empty()) {
    if (error) *error = path.string() + ": submodule has no belongs-to statement";
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
    for (const std::string& dependency : inventory[found->second].includes)
      pending.push_back(dependency);
    if (inventory[found->second].is_submodule)
      pending.push_back(inventory[found->second].belongs_to);
  }
  return std::vector<std::size_t>(selected.begin(), selected.end());
}

bool ApplyRuntimeYangLibrary(std::string_view xml,
                             std::vector<YangSchema>* schemas,
                             std::string* error) {
  if (!schemas || xml.empty() ||
      xml.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    if (error) *error = "invalid FRR YANG Library document";
    return false;
  }
  Document document(xmlReadMemory(
      xml.data(), static_cast<int>(xml.size()), "frr-yang-library.xml", nullptr,
      XML_PARSE_NONET | XML_PARSE_NOBLANKS | XML_PARSE_NOERROR |
          XML_PARSE_NOWARNING), xmlFreeDoc);
  const xmlNode* root = document ? xmlDocGetRootElement(document.get()) : nullptr;
  if (!root || std::string_view(reinterpret_cast<const char*>(root->name)) !=
                   "yang-library" ||
      !root->ns || !root->ns->href ||
      std::string_view(reinterpret_cast<const char*>(root->ns->href)) !=
          "urn:ietf:params:xml:ns:yang:ietf-yang-library") {
    if (error) *error = "FRR returned an invalid YANG Library root";
    return false;
  }
  std::unordered_map<std::string, const xmlNode*> modules;
  const xmlNode* module_set = Child(root, "module-set");
  if (!module_set) {
    if (error) *error = "FRR YANG Library has no module-set";
    return false;
  }
  for (const xmlNode* node = module_set ? module_set->children : nullptr; node;
       node = node->next) {
    if (node->type != XML_ELEMENT_NODE) continue;
    const std::string_view kind(reinterpret_cast<const char*>(node->name));
    if (kind != "module" && kind != "import-only-module") continue;
    const xmlNode* name = Child(node, "name");
    if (name && !modules.emplace(Text(name), node).second) {
      if (error) *error = "FRR YANG Library repeats a module name";
      return false;
    }
  }
  std::vector<std::vector<std::string>> enabled;
  enabled.reserve(schemas->size());
  for (const YangSchema& schema : *schemas) {
    const xmlNode* advertised = nullptr;
    if (schema.is_submodule) {
      const auto owner = modules.find(schema.belongs_to);
      if (owner != modules.end())
        for (const xmlNode* child = owner->second->children; child;
             child = child->next)
          if (child->type == XML_ELEMENT_NODE &&
              std::string_view(reinterpret_cast<const char*>(child->name)) ==
                  "submodule" &&
              Text(Child(child, "name")) == schema.module_name) {
            advertised = child;
            break;
          }
    } else {
      const auto found = modules.find(schema.module_name);
      if (found != modules.end()) advertised = found->second;
    }
    if (!advertised) {
      if (error)
        *error = "running FRR omits YANG " +
            std::string(schema.is_submodule ? "submodule " : "module ") +
            schema.module_name;
      return false;
    }
    const std::string revision = Text(Child(advertised, "revision"));
    const std::string namespace_uri = Text(Child(advertised, "namespace"));
    if ((!schema.revision.empty() && revision != schema.revision) ||
        (!schema.is_submodule && !schema.namespace_uri.empty() &&
         namespace_uri != schema.namespace_uri)) {
      if (error)
        *error = "installed and running FRR disagree on YANG " +
            std::string(schema.is_submodule ? "submodule " : "module ") +
            schema.module_name;
      return false;
    }
    std::vector<std::string> features;
    for (const xmlNode* child = advertised->children; child;
         child = child->next)
      if (child->type == XML_ELEMENT_NODE &&
          std::string_view(reinterpret_cast<const char*>(child->name)) ==
              "feature")
        features.push_back(Text(child));
    enabled.push_back(std::move(features));
  }
  for (std::size_t index = 0; index < schemas->size(); ++index)
    (*schemas)[index].enabled_features = std::move(enabled[index]);
  return true;
}

}  // namespace dang::plugins::frr

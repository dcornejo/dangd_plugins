// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/**
 * @file
 * Extracts the FRR-owned top-level subtrees from complete dangd datastore
 * snapshots.  Namespace-aware matching is essential here because different
 * FRR modules may reuse the same local container name.
 */

#include "frr_config.h"

#include <libxml/parser.h>
#include <libxml/tree.h>

#include <memory>
#include <limits>

namespace dang::plugins::frr {
namespace {

using Document = std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)>;
using Buffer = std::unique_ptr<xmlBuffer, decltype(&xmlBufferFree)>;

bool Matches(const xmlNode* node, const RootDescriptor& descriptor) {
  return node && node->type == XML_ELEMENT_NODE && node->ns && node->ns->href &&
      descriptor.local_name == reinterpret_cast<const char*>(node->name) &&
      descriptor.namespace_uri == reinterpret_cast<const char*>(node->ns->href);
}

std::optional<std::string> Serialize(const xmlNode* node, std::string* error) {
  Buffer buffer(xmlBufferCreate(), xmlBufferFree);
  if (!buffer || xmlNodeDump(buffer.get(), node->doc,
                             const_cast<xmlNode*>(node), 0, 0) < 0) {
    if (error) *error = "cannot serialize FRR configuration root";
    return std::nullopt;
  }
  const auto length = xmlBufferLength(buffer.get());
  return std::string(reinterpret_cast<const char*>(xmlBufferContent(buffer.get())),
                     static_cast<std::size_t>(length));
}

std::optional<std::vector<std::optional<std::string>>> Extract(
    std::string_view xml, const std::vector<RootDescriptor>& descriptors,
    std::string* error, std::string* error_path) {
  if (xml.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    if (error) *error = "configuration snapshot is too large for XML parsing";
    return std::nullopt;
  }
  Document document(xmlReadMemory(xml.data(), static_cast<int>(xml.size()),
                                  "dangd-frr-snapshot.xml", nullptr,
                                  XML_PARSE_NONET | XML_PARSE_NOBLANKS |
                                      XML_PARSE_NOERROR | XML_PARSE_NOWARNING),
                    xmlFreeDoc);
  if (!document) {
    if (error) *error = "configuration snapshot is not well-formed XML";
    return std::nullopt;
  }
  std::vector<std::optional<std::string>> roots(descriptors.size());
  xmlNode* document_root = xmlDocGetRootElement(document.get());
  for (std::size_t index = 0; index < descriptors.size(); ++index) {
    const RootDescriptor& descriptor = descriptors[index];
    xmlNode* found = Matches(document_root, descriptor) ? document_root : nullptr;
    if (!found)
      for (xmlNode* child = document_root ? document_root->children : nullptr;
           child; child = child->next)
        if (Matches(child, descriptor)) {
          if (found) {
            if (error) *error = "duplicate FRR configuration root";
            if (error_path) *error_path = descriptor.xpath;
            return std::nullopt;
          }
          found = child;
        }
    if (found) {
      roots[index] = Serialize(found, error);
      if (!roots[index]) {
        if (error_path) *error_path = descriptor.xpath;
        return std::nullopt;
      }
    }
  }
  return roots;
}

}  // namespace

std::optional<std::vector<ConfigurationRoot>> ExtractConfigurationRoots(
    std::string_view before_xml, std::string_view proposed_xml,
    const std::vector<RootDescriptor>& descriptors, std::string* error,
    std::string* error_path) {
  auto before = Extract(before_xml, descriptors, error, error_path);
  if (!before) return std::nullopt;
  auto proposed = Extract(proposed_xml, descriptors, error, error_path);
  if (!proposed) return std::nullopt;
  std::vector<ConfigurationRoot> roots;
  for (std::size_t index = 0; index < descriptors.size(); ++index) {
    if ((*before)[index] == (*proposed)[index]) continue;
    roots.push_back({descriptors[index].xpath, std::move((*before)[index]),
                     std::move((*proposed)[index])});
  }
  return roots;
}

std::optional<std::string> ReconcileConfigurationRoots(
    std::string_view current_xml,
    const std::vector<RootDescriptor>& descriptors,
    const std::vector<std::optional<std::string>>& observed,
    std::string* error, std::string* error_path) {
  if (descriptors.size() != observed.size()) {
    if (error) *error = "FRR reconciliation root count is inconsistent";
    return std::nullopt;
  }
  if (current_xml.size() >
      static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    if (error) *error = "the applied configuration snapshot is too large";
    return std::nullopt;
  }
  Document document(xmlReadMemory(
                        current_xml.data(), static_cast<int>(current_xml.size()),
                        "dangd-applied.xml", nullptr,
                        XML_PARSE_NONET | XML_PARSE_NOBLANKS |
                            XML_PARSE_NOERROR | XML_PARSE_NOWARNING),
                    xmlFreeDoc);
  if (!document) {
    if (error) *error = "cannot parse the applied configuration snapshot";
    return std::nullopt;
  }
  xmlNode* root = xmlDocGetRootElement(document.get());
  if (!root) {
    if (error) *error = "the applied configuration snapshot has no root";
    return std::nullopt;
  }
  for (std::size_t index = 0; index < descriptors.size(); ++index) {
    const RootDescriptor& descriptor = descriptors[index];
    for (xmlNode* child = root->children; child;) {
      xmlNode* next = child->next;
      if (Matches(child, descriptor)) {
        xmlUnlinkNode(child);
        xmlFreeNode(child);
      }
      child = next;
    }
    if (!observed[index] || observed[index]->empty()) continue;
    if (observed[index]->size() >
        static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      if (error) *error = "FRR running reply is too large for XML parsing";
      if (error_path) *error_path = descriptor.xpath;
      return std::nullopt;
    }
    Document fragment(xmlReadMemory(
                          observed[index]->data(),
                          static_cast<int>(observed[index]->size()),
                          "frr-running.xml", nullptr,
                          XML_PARSE_NONET | XML_PARSE_NOBLANKS |
                              XML_PARSE_NOERROR | XML_PARSE_NOWARNING),
                      xmlFreeDoc);
    xmlNode* observed_root =
        fragment ? xmlDocGetRootElement(fragment.get()) : nullptr;
    if (!Matches(observed_root, descriptor)) {
      if (error) *error = "FRR running reply has an unexpected root";
      if (error_path) *error_path = descriptor.xpath;
      return std::nullopt;
    }
    xmlNode* copy = xmlDocCopyNode(observed_root, document.get(), 1);
    if (!copy || !xmlAddChild(root, copy)) {
      if (copy) xmlFreeNode(copy);
      if (error) *error = "cannot copy FRR running root into applied state";
      if (error_path) *error_path = descriptor.xpath;
      return std::nullopt;
    }
  }
  xmlChar* bytes = nullptr;
  int size = 0;
  xmlDocDumpMemory(document.get(), &bytes, &size);
  if (!bytes || size <= 0) {
    if (bytes) xmlFree(bytes);
    if (error) *error = "cannot serialize reconciled FRR configuration";
    return std::nullopt;
  }
  std::string result(reinterpret_cast<const char*>(bytes),
                     static_cast<std::size_t>(size));
  xmlFree(bytes);
  return result;
}

}  // namespace dang::plugins::frr

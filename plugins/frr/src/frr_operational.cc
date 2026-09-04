// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/** @file Ownership-preserving filtering for FRR zebra operational augments. */

#include "frr_operational.h"

#include <libxml/parser.h>
#include <libxml/tree.h>

#include <algorithm>
#include <memory>

namespace dang::plugins::frr {
namespace {

constexpr std::string_view kZebraNamespace =
    "http://frrouting.org/yang/zebra";

std::string_view LocalName(const xmlNode* node) {
  return node && node->name
      ? std::string_view(reinterpret_cast<const char*>(node->name))
      : std::string_view{};
}

std::string_view Namespace(const xmlNode* node) {
  return node && node->ns && node->ns->href
      ? std::string_view(reinterpret_cast<const char*>(node->ns->href))
      : std::string_view{};
}

bool Is(const xmlNode* node, std::string_view name,
        std::string_view namespace_uri) {
  return node && node->type == XML_ELEMENT_NODE && LocalName(node) == name &&
         Namespace(node) == namespace_uri;
}

}  // namespace

std::optional<std::string> ExtractZebraAugments(
    std::string_view xml, const AugmentedList& descriptor,
    std::string* error) {
  xmlDocPtr raw = xmlReadMemory(xml.data(), static_cast<int>(xml.size()),
                                "frr-operational.xml", nullptr,
                                XML_PARSE_NONET | XML_PARSE_NOBLANKS |
                                    XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
  std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> input(raw, xmlFreeDoc);
  if (!input) {
    if (error) *error = "cannot parse FRR augmented operational XML";
    return std::nullopt;
  }
  const xmlNode* root = xmlDocGetRootElement(input.get());
  if (!Is(root, descriptor.root_name, descriptor.namespace_uri)) {
    if (error) *error = "FRR operational reply has an unexpected root";
    return std::nullopt;
  }

  xmlDocPtr output_raw = xmlNewDoc(BAD_CAST "1.0");
  if (!output_raw) {
    if (error) *error = "cannot allocate FRR operational document";
    return std::nullopt;
  }
  std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> output(output_raw, xmlFreeDoc);
  xmlNode* output_root = xmlDocCopyNode(const_cast<xmlNode*>(root), output.get(), 2);
  if (!output_root) {
    if (error) *error = "cannot copy FRR operational root";
    return std::nullopt;
  }
  xmlDocSetRootElement(output.get(), output_root);
  bool retained = false;
  for (const xmlNode* instance = root->children; instance;
       instance = instance->next) {
    if (!Is(instance, descriptor.list_name, descriptor.namespace_uri)) continue;
    const xmlNode* zebra = nullptr;
    for (const xmlNode* child = instance->children; child; child = child->next)
      if (child->type == XML_ELEMENT_NODE && Namespace(child) == kZebraNamespace) {
        zebra = child;
        break;
      }
    if (!zebra) continue;
    xmlNode* output_instance =
        xmlDocCopyNode(const_cast<xmlNode*>(instance), output.get(), 2);
    if (!output_instance) continue;
    xmlAddChild(output_root, output_instance);
    for (const std::string& key : descriptor.keys) {
      for (const xmlNode* child = instance->children; child; child = child->next)
        if (Is(child, key, descriptor.namespace_uri)) {
          xmlAddChild(output_instance,
                      xmlDocCopyNode(const_cast<xmlNode*>(child), output.get(), 1));
          break;
        }
    }
    xmlAddChild(output_instance,
                xmlDocCopyNode(const_cast<xmlNode*>(zebra), output.get(), 1));
    retained = true;
  }
  if (!retained) return std::string{};
  xmlChar* buffer = nullptr;
  int size = 0;
  xmlDocDumpMemory(output.get(), &buffer, &size);
  if (!buffer || size <= 0) {
    if (buffer) xmlFree(buffer);
    if (error) *error = "cannot serialize FRR zebra augments";
    return std::nullopt;
  }
  std::string result(reinterpret_cast<const char*>(buffer),
                     static_cast<std::size_t>(size));
  xmlFree(buffer);
  const auto declaration = result.find("?>");
  if (declaration != std::string::npos) result.erase(0, declaration + 2);
  return result;
}

std::string OperationalDocument(const std::vector<std::string>& fragments) {
  std::string result =
      "<data xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">";
  for (const std::string& fragment : fragments) result += fragment;
  result += "</data>";
  return result;
}

}  // namespace dang::plugins::frr

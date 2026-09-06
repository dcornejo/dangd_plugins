// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/** @file Parsing and execution for imperative RFC 8431 RIB operations. */

#include "plugins/rib/src/rib_rpc.h"

#include <cstring>
#include <memory>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

#include <libxml/parser.h>
#include <libxml/tree.h>

#include "plugins/rib/src/rib_config.h"

namespace dang::rib {
namespace {
constexpr std::string_view kNamespace =
    "urn:ietf:params:xml:ns:yang:ietf-i2rs-rib";

bool Is(xmlNodePtr node, std::string_view name) {
  return node && node->type == XML_ELEMENT_NODE && node->ns && node->ns->href &&
         name == reinterpret_cast<const char*>(node->name) &&
         kNamespace == reinterpret_cast<const char*>(node->ns->href);
}

xmlNodePtr Child(xmlNodePtr parent, std::string_view name) {
  for (xmlNodePtr node = parent ? parent->children : nullptr; node;
       node = node->next)
    if (Is(node, name)) return node;
  return nullptr;
}

std::string Text(xmlNodePtr node) {
  if (!node) return {};
  xmlChar* raw = xmlNodeGetContent(node);
  if (!raw) return {};
  std::string value(reinterpret_cast<const char*>(raw));
  xmlFree(raw);
  return value;
}

bool Boolean(xmlNodePtr node) {
  const std::string value = Text(node);
  return value == "true" || value == "1";
}

std::string Output(unsigned success,
                   const std::vector<std::pair<std::uint64_t, unsigned>>& failed,
                   bool details) {
  std::ostringstream xml;
  xml << "<success-count xmlns=\"" << kNamespace << "\">" << success
      << "</success-count><failed-count xmlns=\"" << kNamespace << "\">"
      << failed.size() << "</failed-count>";
  if (details && !failed.empty()) {
    xml << "<failure-detail xmlns=\"" << kNamespace << "\">";
    for (const auto& [index, code] : failed)
      xml << "<failed-routes><route-index>" << index
          << "</route-index><error-code>" << code
          << "</error-code></failed-routes>";
    xml << "</failure-detail>";
  }
  return xml.str();
}

}  // namespace

bool InvokeRouteAdd(NativePlatform platform, const char* input_xml,
                    std::string* output_xml, std::string* error,
                    std::string* error_path, const CommandRunner& runner) {
  if (!input_xml || !output_xml || !error || !error_path) return false;
  xmlDocPtr raw = xmlReadMemory(input_xml, static_cast<int>(std::strlen(input_xml)),
                                "route-add.xml", nullptr,
                                XML_PARSE_NONET | XML_PARSE_NOBLANKS |
                                    XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
  std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> input(raw, xmlFreeDoc);
  xmlNodePtr root = input ? xmlDocGetRootElement(input.get()) : nullptr;
  if (!Is(root, "route-add")) {
    *error = "route-add input is not RFC 8431 XML";
    *error_path = "/ietf-i2rs-rib:route-add";
    return false;
  }
  const std::string rib_name = Text(Child(root, "rib-name"));
  xmlNodePtr routes_node = Child(root, "routes");
  if (rib_name.empty() || !routes_node) {
    *error = "route-add requires rib-name and routes";
    *error_path = "/ietf-i2rs-rib:route-add";
    return false;
  }
  const bool details = Boolean(Child(root, "return-failure-detail"));
  unsigned success = 0;
  std::vector<std::pair<std::uint64_t, unsigned>> failed;
  for (xmlNodePtr node = routes_node->children; node; node = node->next) {
    if (!Is(node, "route-list")) continue;
    // Reuse the configuration parser by constructing the equivalent RIB
    // context around this route. This keeps RPC and datastore validation from
    // drifting as the supported route projection grows.
    xmlDocPtr synthetic_raw = xmlNewDoc(BAD_CAST "1.0");
    std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> synthetic(synthetic_raw,
                                                            xmlFreeDoc);
    xmlNodePtr instance = xmlNewNode(nullptr, BAD_CAST "routing-instance");
    xmlNsPtr ns = xmlNewNs(instance, BAD_CAST kNamespace.data(), nullptr);
    xmlSetNs(instance, ns);
    xmlDocSetRootElement(synthetic.get(), instance);
    xmlNewTextChild(instance, ns, BAD_CAST "name", BAD_CAST "default");
    xmlNodePtr rib = xmlNewChild(instance, ns, BAD_CAST "rib-list", nullptr);
    xmlNewTextChild(rib, ns, BAD_CAST "name", BAD_CAST rib_name.c_str());
    const bool ipv6 = Child(Child(node, "match"), "ipv6") != nullptr;
    xmlNewTextChild(rib, ns, BAD_CAST "address-family",
                    BAD_CAST(ipv6 ? "ipv6" : "ipv4"));
    xmlAddChild(rib, xmlDocCopyNode(node, synthetic.get(), 1));
    xmlChar* serialized = nullptr;
    int serialized_size = 0;
    xmlDocDumpMemory(synthetic.get(), &serialized, &serialized_size);
    Config config;
    std::string parse_error;
    std::string parse_path;
    const bool parsed = serialized && ParseConfig(
        reinterpret_cast<const char*>(serialized), &config, &parse_error,
        &parse_path);
    if (serialized) xmlFree(serialized);
    std::uint64_t index = 0;
    const std::string index_text = Text(Child(node, "route-index"));
    try { index = std::stoull(index_text); } catch (...) { index = 0; }
    if (!parsed || config.routes.size() != 1) {
      failed.emplace_back(index, 3U);
      continue;
    }
    ExecutionResult result = ExecuteChanges(
        platform, {{ChangeKind::kInstall, config.routes.front()}}, runner);
    if (result.ok) ++success;
    else failed.emplace_back(index, 0U);
  }
  *output_xml = Output(success, failed, details);
  return true;
}

}  // namespace dang::rib

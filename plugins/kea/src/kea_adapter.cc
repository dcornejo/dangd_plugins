// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/**
 * @file
 * Translates the official Kea DHCPv4/DHCPv6 YANG configuration containers to
 * Kea control-agent JSON and exchanges bounded commands over local UNIX
 * sockets.  Translation is deliberately explicit so unsupported YANG shapes
 * fail instead of silently producing a plausible but different configuration.
 */

#include "kea_adapter.h"

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace dang::plugins::kea {
namespace {

constexpr std::size_t kMaximumReplyBytes = 16 * 1024 * 1024;
constexpr auto kSocketTimeout = std::chrono::seconds(5);

std::string LocalName(const xmlNode* node) {
  return node && node->name
      ? reinterpret_cast<const char*>(node->name)
      : std::string{};
}

std::string Namespace(const xmlNode* node) {
  return node && node->ns && node->ns->href
      ? reinterpret_cast<const char*>(node->ns->href)
      : std::string{};
}

std::vector<const xmlNode*> ElementChildren(const xmlNode* parent) {
  std::vector<const xmlNode*> result;
  if (!parent) return result;
  for (const xmlNode* child = parent->children; child; child = child->next)
    if (child->type == XML_ELEMENT_NODE) result.push_back(child);
  return result;
}

std::string Text(const xmlNode* node) {
  xmlChar* content = xmlNodeGetContent(const_cast<xmlNode*>(node));
  if (!content) return {};
  std::string result(reinterpret_cast<const char*>(content));
  xmlFree(content);
  return result;
}

bool IsList(std::string_view name) {
  static const std::set<std::string, std::less<>> lists{
      "client-class", "clients", "config-database", "control-sockets",
      "hook-library", "host", "hosts-database", "http-headers", "lease",
      "logger", "option-data", "option-def", "output-option", "pd-pool",
      "pool", "shared-network", "subnet", "subnet4", "subnet6"};
  return lists.contains(name);
}

bool IsLeafList(std::string_view name) {
  static const std::set<std::string, std::less<>> leaf_lists{
      "client-classes", "evaluate-additional-classes", "excluded-prefixes",
      "host-reservation-identifiers", "interfaces", "ip-addresses",
      "mac-sources", "prefixes", "relay-addresses",
      "relay-supplied-options", "require-client-classes"};
  return leaf_lists.contains(name);
}

std::string JsonName(std::string_view yang_name) {
  static const std::map<std::string, std::string, std::less<>> names{
      {"config-database", "config-databases"},
      {"hook-library", "hooks-libraries"},
      {"hosts-database", "hosts-databases"},
      {"logger", "loggers"},
      {"output-option", "output-options"},
      {"pd-pool", "pd-pools"},
      {"pool", "pools"},
      {"shared-network", "shared-networks"}};
  const auto found = names.find(yang_name);
  return found == names.end() ? std::string(yang_name) : found->second;
}

nlohmann::json Scalar(const xmlNode* node, const std::string& value) {
  const std::string name = LocalName(node);
  // Kea models carry deliberately JSON-valued string leaves. Other scalar
  // types are reconstructed from their canonical XML lexical forms.
  const bool http_header_value = name == "value" && node && node->parent &&
      LocalName(node->parent) == "http-headers";
  if (name == "user-context" || name == "parameters" ||
      name == "dhcp-queue-control" || http_header_value) {
    try {
      return nlohmann::json::parse(value);
    } catch (...) {
      return value;
    }
  }
  static const std::set<std::string, std::less<>> decimal_names{
      "adaptive-lease-time-threshold", "cache-threshold", "ddns-ttl-percent",
      "t1-percent", "t2-percent"};
  if (decimal_names.contains(name)) {
    double decimal = 0.0;
    const auto [decimal_end, decimal_error] =
        std::from_chars(value.data(), value.data() + value.size(), decimal);
    if (decimal_error == std::errc{} &&
        decimal_end == value.data() + value.size())
      return decimal;
  }
  if (value == "true") return true;
  if (value == "false") return false;
  std::int64_t integer = 0;
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), integer);
  if (error == std::errc{} && end == value.data() + value.size()) return integer;
  return value;
}

nlohmann::json ConvertNode(const xmlNode* node);

nlohmann::json ConvertPool(const xmlNode* node) {
  nlohmann::json result = ConvertNode(node);
  if (result.contains("prefix")) {
    result["pool"] = result["prefix"];
  } else if (result.contains("start-address") && result.contains("end-address")) {
    result["pool"] = result["start-address"].get<std::string>() + " - " +
        result["end-address"].get<std::string>();
  }
  result.erase("prefix");
  result.erase("start-address");
  result.erase("end-address");
  return result;
}

nlohmann::json ConvertNode(const xmlNode* node) {
  const auto children = ElementChildren(node);
  if (children.empty()) return Scalar(node, Text(node));
  // Group siblings before conversion because singleton and repeated YANG
  // nodes require different JSON shapes even when their child syntax matches.
  std::map<std::string, std::vector<const xmlNode*>, std::less<>> grouped;
  for (const xmlNode* child : children) grouped[LocalName(child)].push_back(child);
  nlohmann::json result = nlohmann::json::object();
  for (const auto& [name, values] : grouped) {
    // Several Kea models reuse a name for a list in one context and a scalar
    // leaf in another (notably host, subnet, and client-class). A list entry
    // always has element children because its YANG key is mandatory, whereas
    // a scalar has only text. Use that schema-guaranteed shape to disambiguate
    // singleton instances without turning the database "host" or subnet
    // prefix leaves into arrays.
    const bool list_instance =
        IsList(name) && !ElementChildren(values.front()).empty();
    const std::string json_name =
        name == "host" && list_instance ? "reservations" : JsonName(name);
    if (values.size() > 1 || list_instance || IsLeafList(name)) {
      nlohmann::json array = nlohmann::json::array();
      for (const xmlNode* value : values)
        array.push_back(name == "pool" ? ConvertPool(value)
                                        : ConvertNode(value));
      result[json_name] = std::move(array);
    } else {
      result[json_name] = ConvertNode(values.front());
    }
  }
  return result;
}

const xmlNode* FindConfiguration(const xmlNode* node,
                                 std::string_view expected_namespace) {
  if (!node) return nullptr;
  if (node->type == XML_ELEMENT_NODE && LocalName(node) == "config" &&
      Namespace(node) == expected_namespace)
    return node;
  for (const xmlNode* child = node->children; child; child = child->next)
    if (const xmlNode* found = FindConfiguration(child, expected_namespace))
      return found;
  return nullptr;
}

bool WaitFor(int descriptor, short events,
             std::chrono::steady_clock::time_point deadline,
             std::string* error) {
  while (true) {
    const auto remaining = deadline - std::chrono::steady_clock::now();
    if (remaining <= std::chrono::steady_clock::duration::zero()) {
      if (error) *error = "Kea control socket timed out";
      return false;
    }
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        remaining + std::chrono::milliseconds(1));
    pollfd descriptor_state{descriptor, events, 0};
    const int ready = poll(&descriptor_state, 1, static_cast<int>(milliseconds.count()));
    if (ready > 0) return true;
    if (ready == 0) {
      if (error) *error = "Kea control socket timed out";
      return false;
    }
    if (errno != EINTR) {
      if (error) *error = std::string("cannot poll Kea control socket: ") +
          std::strerror(errno);
      return false;
    }
  }
}

}  // namespace

std::optional<ServerConfiguration> TranslateConfiguration(
    std::string_view datastore_xml, std::string_view module_name,
    std::string_view socket_path, std::string* error) {
  const bool dhcp4 = module_name == "kea-dhcp4-server";
  const bool dhcp6 = module_name == "kea-dhcp6-server";
  if (!dhcp4 && !dhcp6) {
    if (error) *error = "unsupported Kea module";
    return std::nullopt;
  }
  if (socket_path.empty() || socket_path.size() >= sizeof(sockaddr_un::sun_path)) {
    if (error) *error = "Kea control socket path is empty or too long";
    return std::nullopt;
  }
  xmlDocPtr document = xmlReadMemory(datastore_xml.data(),
                                     static_cast<int>(datastore_xml.size()),
                                     "datastore.xml", nullptr,
                                     XML_PARSE_NONET | XML_PARSE_NOBLANKS);
  if (!document) {
    if (error) *error = "cannot parse the validated datastore snapshot";
    return std::nullopt;
  }
  const std::string expected_namespace =
      "urn:ietf:params:xml:ns:yang:" + std::string(module_name);
  const xmlNode* config = FindConfiguration(xmlDocGetRootElement(document),
                                             expected_namespace);
  nlohmann::json body = config ? ConvertNode(config) : nlohmann::json::object();
  xmlFreeDoc(document);
  const std::string service = dhcp4 ? "Dhcp4" : "Dhcp6";
  return ServerConfiguration{std::string(module_name), service,
                             std::string(socket_path),
                             nlohmann::json{{service, std::move(body)}}};
}

std::optional<nlohmann::json> SendControlCommand(
    const ServerConfiguration& server, std::string_view command,
    std::string* error) {
  const int descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
  if (descriptor < 0) {
    if (error) *error = std::string("cannot create Kea control socket: ") +
        std::strerror(errno);
    return std::nullopt;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, server.socket_path.c_str(),
              server.socket_path.size() + 1);
  if (connect(descriptor, reinterpret_cast<const sockaddr*>(&address),
              sizeof(address)) != 0) {
    if (error) *error = "cannot connect to " + server.socket_path + ": " +
        std::strerror(errno);
    close(descriptor);
    return std::nullopt;
  }
  const std::string request =
      nlohmann::json{{"command", command}, {"arguments", server.arguments}}.dump();
  const auto deadline = std::chrono::steady_clock::now() + kSocketTimeout;
  std::size_t sent = 0;
  while (sent < request.size()) {
    if (!WaitFor(descriptor, POLLOUT, deadline, error)) {
      close(descriptor);
      return std::nullopt;
    }
    const ssize_t count = send(descriptor, request.data() + sent,
                               request.size() - sent, 0);
    if (count > 0) {
      sent += static_cast<std::size_t>(count);
    } else if (count < 0 && errno != EINTR) {
      if (error) *error = std::string("cannot write Kea command: ") +
          std::strerror(errno);
      close(descriptor);
      return std::nullopt;
    }
  }
  // EOF terminates Kea's request.  Keep the read half open for its one reply.
  (void)shutdown(descriptor, SHUT_WR);
  std::string reply;
  char buffer[4096];
  while (true) {
    if (!WaitFor(descriptor, POLLIN, deadline, error)) {
      close(descriptor);
      return std::nullopt;
    }
    const ssize_t count = recv(descriptor, buffer, sizeof(buffer), 0);
    if (count == 0) break;
    if (count < 0) {
      if (errno == EINTR) continue;
      if (error) *error = std::string("cannot read Kea response: ") +
          std::strerror(errno);
      close(descriptor);
      return std::nullopt;
    }
    if (reply.size() + static_cast<std::size_t>(count) > kMaximumReplyBytes) {
      if (error) *error = "Kea response exceeds the plugin limit";
      close(descriptor);
      return std::nullopt;
    }
    reply.append(buffer, static_cast<std::size_t>(count));
  }
  close(descriptor);
  try {
    nlohmann::json parsed = nlohmann::json::parse(reply);
    return std::optional<nlohmann::json>(std::move(parsed));
  } catch (const std::exception& exception) {
    if (error) *error = std::string("invalid Kea response: ") + exception.what();
    return std::nullopt;
  }
}

bool CommandSucceeded(const nlohmann::json& response, std::string* reason) {
  const nlohmann::json* answer = &response;
  if (response.is_array() && !response.empty()) answer = &response.front();
  if (!answer->is_object() || !answer->contains("result") ||
      !(*answer)["result"].is_number_integer()) {
    if (reason) *reason = "Kea returned a response without an integer result";
    return false;
  }
  if ((*answer)["result"].get<int>() == 0) return true;
  if (reason)
    *reason = answer->value("text", std::string("Kea rejected the command"));
  return false;
}

}  // namespace dang::plugins::kea

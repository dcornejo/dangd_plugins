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
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace dang::plugins::kea {
namespace {

constexpr std::size_t kMaximumControlBytes = 16 * 1024 * 1024;
constexpr auto kSocketTimeout = std::chrono::seconds(5);

bool ValidateSocketPath(std::string_view socket_path, std::string* error) {
  if (socket_path.empty() ||
      socket_path.size() >= sizeof(sockaddr_un::sun_path) ||
      socket_path.find('\0') != std::string_view::npos) {
    if (error)
      *error = "Kea control socket path is empty, contains NUL, or is too long";
    return false;
  }
  return true;
}

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
      {"database-type", "type"},
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
  // These leaves are declared directly as YANG string in the pinned module
  // family. Preserve their type even when their lexical value resembles a
  // JSON boolean or number (for example hostname "true" or server-tag "123").
  static const std::set<std::string, std::less<>> string_names{
      "allocator", "auth-key", "boot-file-name", "cert-file", "cipher-list",
      "client-class", "client-classes", "data", "data-directory",
      "database-type", "ddns-generated-prefix", "ddns-qualifying-suffix",
      "ddns-replace-client-name", "directory", "encapsulate",
      "evaluate-additional-classes", "host", "hostname",
      "hostname-char-replacement", "hostname-char-set", "hw-address",
      "identifier", "interface", "interface-id", "interfaces", "key-file",
      "library", "mac-sources", "name", "on-fail", "output", "password",
      "password-file", "pattern", "pd-allocator", "realm", "record-types",
      "relay-supplied-options", "require-client-classes", "server-hostname",
      "server-tag", "socket-address", "socket-name", "space", "ssl-mode",
      "subnet-4o6-interface", "subnet-4o6-interface-id", "template-test",
      "test", "trust-anchor", "type", "user", "user-file", "value"};
  if (string_names.contains(name)) return value;
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

nlohmann::json ConvertHost(const xmlNode* node) {
  nlohmann::json result = ConvertNode(node);
  const auto type = result.find("identifier-type");
  const auto identifier = result.find("identifier");
  if (type != result.end() && type->is_string() &&
      identifier != result.end() && identifier->is_string()) {
    result[type->get<std::string>()] = *identifier;
    result.erase("identifier-type");
    result.erase("identifier");
  }
  return result;
}

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
                        : name == "host" ? ConvertHost(value)
                                         : ConvertNode(value));
      result[json_name] = std::move(array);
    } else {
      result[json_name] = ConvertNode(values.front());
    }
  }
  return result;
}

void FindConfigurations(const xmlNode* node,
                        std::string_view expected_namespace,
                        const xmlNode** first, std::size_t* count) {
  if (!node) return;
  if (node->type == XML_ELEMENT_NODE && LocalName(node) == "config" &&
      Namespace(node) == expected_namespace) {
    if (*count == 0) *first = node;
    ++*count;
  }
  for (const xmlNode* child = node->children; child; child = child->next)
    FindConfigurations(child, expected_namespace, first, count);
}

std::optional<std::string> ForeignElement(
    const xmlNode* node, std::string_view expected_namespace) {
  if (!node) return std::nullopt;
  if (node->type == XML_ELEMENT_NODE && Namespace(node) != expected_namespace)
    return LocalName(node);
  for (const xmlNode* child = node->children; child; child = child->next)
    if (auto foreign = ForeignElement(child, expected_namespace); foreign)
      return foreign;
  return std::nullopt;
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

const nlohmann::json* Answer(const nlohmann::json& response) {
  if (response.is_array() && response.size() == 1) return &response.front();
  return response.is_object() ? &response : nullptr;
}

bool IsResultCode(const nlohmann::json& value, int expected) {
  if (value.is_number_unsigned())
    return expected >= 0 && value.get<std::uint64_t>() ==
        static_cast<std::uint64_t>(expected);
  return value.is_number_integer() &&
      value.get<std::int64_t>() == static_cast<std::int64_t>(expected);
}

std::optional<std::uint64_t> UnsignedValue(const nlohmann::json& value) {
  if (value.is_number_unsigned()) return value.get<std::uint64_t>();
  if (!value.is_number_integer()) return std::nullopt;
  const auto signed_value = value.get<std::int64_t>();
  if (signed_value < 0) return std::nullopt;
  return static_cast<std::uint64_t>(signed_value);
}

std::string RejectionReason(const nlohmann::json& answer,
                            std::string_view fallback) {
  const auto text = answer.find("text");
  if (text == answer.end()) return std::string(fallback);
  if (text->is_string()) return text->get<std::string>();
  return std::string(fallback) + " (Kea reply contains non-string text)";
}

bool IsRequiredControlSocket(const nlohmann::json& socket,
                             std::string_view socket_path) {
  if (!socket.is_object()) return false;
  const auto type = socket.find("socket-type");
  const auto name = socket.find("socket-name");
  return type != socket.end() && type->is_string() && *type == "unix" &&
      name != socket.end() && name->is_string() && *name == socket_path;
}

bool PreservesControlSocket(const nlohmann::json& body,
                            std::string_view socket_path) {
  const auto sockets = body.find("control-sockets");
  if (sockets != body.end() && sockets->is_array())
    for (const auto& socket : *sockets)
      if (IsRequiredControlSocket(socket, socket_path)) return true;
  const auto deprecated = body.find("control-socket");
  return deprecated != body.end() &&
      IsRequiredControlSocket(*deprecated, socket_path);
}

std::string_view BaseName(std::string_view path) {
  const auto separator = path.find_last_of('/');
  return separator == std::string_view::npos ? path : path.substr(separator + 1);
}

std::optional<std::string> MissingRequiredHook(const nlohmann::json& body) {
  static constexpr std::string_view required[]{
      "libdhcp_lease_cmds.so", "libdhcp_stat_cmds.so",
      "libdhcp_host_cmds.so"};
  std::set<std::string, std::less<>> libraries;
  const auto hooks = body.find("hooks-libraries");
  if (hooks != body.end() && hooks->is_array()) {
    for (const auto& hook : *hooks) {
      if (!hook.is_object()) continue;
      const auto library = hook.find("library");
      if (library != hook.end() && library->is_string())
        libraries.emplace(BaseName(library->get_ref<const std::string&>()));
    }
  }
  for (const auto library : required)
    if (!libraries.contains(library)) return std::string(library);
  return std::nullopt;
}

bool IsXmlText(std::string_view value) {
  for (std::size_t offset = 0; offset < value.size();) {
    const auto first = static_cast<unsigned char>(value[offset]);
    std::uint32_t codepoint = 0;
    std::size_t length = 0;
    if (first <= 0x7f) {
      codepoint = first;
      length = 1;
    } else if (first >= 0xc2 && first <= 0xdf) {
      codepoint = first & 0x1f;
      length = 2;
    } else if (first >= 0xe0 && first <= 0xef) {
      codepoint = first & 0x0f;
      length = 3;
    } else if (first >= 0xf0 && first <= 0xf4) {
      codepoint = first & 0x07;
      length = 4;
    } else {
      return false;
    }
    if (offset + length > value.size()) return false;
    for (std::size_t index = 1; index < length; ++index) {
      const auto continuation =
          static_cast<unsigned char>(value[offset + index]);
      if ((continuation & 0xc0) != 0x80) return false;
      codepoint = (codepoint << 6) | (continuation & 0x3f);
    }
    if ((length == 3 && first == 0xe0 && codepoint < 0x800) ||
        (length == 3 && first == 0xed && codepoint >= 0xd800) ||
        (length == 4 && first == 0xf0 && codepoint < 0x10000) ||
        (length == 4 && first == 0xf4 && codepoint > 0x10ffff))
      return false;
    const bool xml_character = codepoint == 0x9 || codepoint == 0xa ||
        codepoint == 0xd || (codepoint >= 0x20 && codepoint <= 0xd7ff) ||
        (codepoint >= 0xe000 && codepoint <= 0xfffd) ||
        (codepoint >= 0x10000 && codepoint <= 0x10ffff);
    if (!xml_character) return false;
    offset += length;
  }
  return true;
}

std::optional<std::string> XmlText(std::string_view value,
                                   std::string_view description,
                                   std::string* error) {
  if (!IsXmlText(value)) {
    if (error)
      *error = "Kea response field " + std::string(description) +
          " is not valid XML text";
    return std::nullopt;
  }
  std::string escaped;
  escaped.reserve(value.size());
  for (const char character : value) {
    switch (character) {
      case '&': escaped += "&amp;"; break;
      case '<': escaped += "&lt;"; break;
      case '>': escaped += "&gt;"; break;
      case '\"': escaped += "&quot;"; break;
      case '\'': escaped += "&apos;"; break;
      default: escaped += character;
    }
  }
  return std::optional<std::string>(std::move(escaped));
}

std::optional<std::string> JsonText(const nlohmann::json& value,
                                    std::string_view description,
                                    std::string* error) {
  try {
    return value.dump();
  } catch (const std::exception& exception) {
    if (error)
      *error = "invalid Kea " + std::string(description) + ": " +
          exception.what();
    return std::nullopt;
  }
}

std::optional<nlohmann::json> RunControlQuery(
    const ControlQuery& query, std::string_view socket_path,
    std::string_view command, const nlohmann::json& arguments,
    std::string* error) {
  try {
    return query(socket_path, command, arguments, error);
  } catch (const std::exception& exception) {
    if (error)
      *error = std::string("Kea control query threw: ") + exception.what();
    return std::nullopt;
  } catch (...) {
    if (error) *error = "Kea control query threw an unknown exception";
    return std::nullopt;
  }
}

std::optional<std::string> BinaryBase64(std::string_view hexadecimal) {
  // Kea normally renders binary identities as colon-separated octets, while
  // some control-command producers use one contiguous hexadecimal string.
  // Accept either complete spelling, but never normalize missing, empty, or
  // repeated octets into a different identity.
  if (hexadecimal.empty()) return std::nullopt;
  const bool separated = hexadecimal.find(':') != std::string_view::npos;
  std::vector<std::uint8_t> bytes;
  for (std::size_t offset = 0; offset < hexadecimal.size();) {
    if (offset + 2 > hexadecimal.size()) return std::nullopt;
    unsigned int byte = 0;
    const auto [end, error] = std::from_chars(
        hexadecimal.data() + offset, hexadecimal.data() + offset + 2, byte, 16);
    if (error != std::errc{} || end != hexadecimal.data() + offset + 2 ||
        byte > 255)
      return std::nullopt;
    bytes.push_back(static_cast<std::uint8_t>(byte));
    offset += 2;
    if (offset == hexadecimal.size()) break;
    if (separated) {
      if (hexadecimal[offset] != ':') return std::nullopt;
      ++offset;
      if (offset == hexadecimal.size()) return std::nullopt;
    }
  }
  static constexpr char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string encoded;
  for (std::size_t offset = 0; offset < bytes.size(); offset += 3) {
    const std::uint32_t first = bytes[offset];
    const std::uint32_t second =
        offset + 1 < bytes.size() ? bytes[offset + 1] : 0;
    const std::uint32_t third =
        offset + 2 < bytes.size() ? bytes[offset + 2] : 0;
    const std::uint32_t block = (first << 16) | (second << 8) | third;
    encoded += alphabet[(block >> 18) & 63];
    encoded += alphabet[(block >> 12) & 63];
    encoded += offset + 1 < bytes.size() ? alphabet[(block >> 6) & 63] : '=';
    encoded += offset + 2 < bytes.size() ? alphabet[block & 63] : '=';
  }
  return encoded;
}

bool AppendStringLeaf(std::string* xml, std::string_view name,
                      const nlohmann::json& object, std::string_view key,
                      bool mandatory, std::string* error) {
  const auto found = object.find(key);
  if (found == object.end() || found->is_null()) {
    if (!mandatory) return true;
    if (error) *error = "Kea response omits mandatory field " + std::string(key);
    return false;
  }
  if (!found->is_string()) {
    if (error) *error = "Kea response field " + std::string(key) +
        " is not a string";
    return false;
  }
  auto escaped = XmlText(found->get_ref<const std::string&>(), key, error);
  if (!escaped) return false;
  *xml += "<" + std::string(name) + ">" + *escaped + "</" +
      std::string(name) + ">";
  return true;
}

bool AppendBooleanLeaf(std::string* xml, std::string_view name,
                       const nlohmann::json& object, std::string_view key,
                       bool mandatory, std::string* error) {
  const auto found = object.find(key);
  if (found == object.end() || found->is_null()) {
    if (!mandatory) return true;
    if (error) *error = "Kea response omits mandatory field " + std::string(key);
    return false;
  }
  if (!found->is_boolean()) {
    if (error) *error = "Kea response field " + std::string(key) +
        " is not a boolean";
    return false;
  }
  *xml += "<" + std::string(name) + ">" +
      std::string(*found ? "true" : "false") + "</" + std::string(name) + ">";
  return true;
}

bool AppendUnsignedLeaf(std::string* xml, std::string_view name,
                        const nlohmann::json& object, std::string_view key,
                        bool mandatory, std::uint64_t maximum,
                        std::string* error) {
  const auto found = object.find(key);
  if (found == object.end() || found->is_null()) {
    if (!mandatory) return true;
    if (error) *error = "Kea response omits mandatory field " + std::string(key);
    return false;
  }
  const auto value = UnsignedValue(*found);
  if (!value || *value > maximum) {
    if (error) *error = "Kea response field " + std::string(key) +
        " is not an in-range unsigned integer";
    return false;
  }
  *xml += "<" + std::string(name) + ">" + std::to_string(*value) + "</" +
      std::string(name) + ">";
  return true;
}

std::optional<std::string> BuildLeases(const nlohmann::json& response,
                                       bool dhcp6, std::string* error) {
  const nlohmann::json* answer = Answer(response);
  if (!answer || !answer->contains("result") ||
      !answer->at("result").is_number_integer()) {
    if (error) *error = "Kea lease reply omits an integer result";
    return std::nullopt;
  }
  if (IsResultCode(answer->at("result"), 3)) return "<leases/>";
  if (!IsResultCode(answer->at("result"), 0)) {
    if (error) *error = RejectionReason(*answer, "Kea rejected the lease query");
    return std::nullopt;
  }
  const auto arguments = answer->find("arguments");
  if (arguments == answer->end() || !arguments->is_object()) {
    if (error) *error = "Kea lease reply omits arguments";
    return std::nullopt;
  }
  const auto leases = arguments->find("leases");
  if (leases == arguments->end() || !leases->is_array()) {
    if (error) *error = "Kea lease reply omits the leases array";
    return std::nullopt;
  }
  std::string xml = "<leases>";
  std::set<std::string, std::less<>> addresses;
  for (const auto& lease : *leases) {
    if (!lease.is_object()) {
      if (error) *error = "Kea lease reply contains a non-object entry";
      return std::nullopt;
    }
    const auto address = lease.find("ip-address");
    if (address == lease.end() || !address->is_string() ||
        !addresses.emplace(address->get<std::string>()).second) {
      if (error)
        *error =
            "Kea lease reply contains a missing, invalid, or duplicate IP address";
      return std::nullopt;
    }
    xml += "<lease>";
    if (!AppendStringLeaf(&xml, "ip-address", lease, "ip-address", true,
                          error))
      return std::nullopt;
    const std::string binary_key = dhcp6 ? "duid" : "hw-address";
    const auto binary = lease.find(binary_key);
    if (binary == lease.end() || !binary->is_string()) {
      if (error) *error = "Kea lease reply omits mandatory field " + binary_key;
      return std::nullopt;
    }
    auto encoded = BinaryBase64(binary->get<std::string>());
    if (!encoded) {
      if (error) *error = "Kea lease reply contains malformed " + binary_key;
      return std::nullopt;
    }
    xml += "<" + binary_key + ">" + *encoded + "</" + binary_key + ">";
    if (!dhcp6) {
      const auto client = lease.find("client-id");
      if (client != lease.end()) {
        if (!client->is_string()) {
          if (error) *error = "Kea lease reply contains malformed client-id";
          return std::nullopt;
        }
        auto client_id = BinaryBase64(client->get<std::string>());
        if (!client_id) {
          if (error) *error = "Kea lease reply contains malformed client-id";
          return std::nullopt;
        }
        xml += "<client-id>" + *client_id + "</client-id>";
      }
    }
    if (!AppendUnsignedLeaf(&xml, "valid-lifetime", lease, "valid-lft", true,
                            std::numeric_limits<std::uint32_t>::max(), error) ||
        !AppendUnsignedLeaf(&xml, "cltt", lease, "cltt", true,
                            std::numeric_limits<std::uint32_t>::max(), error) ||
        !AppendUnsignedLeaf(&xml, "subnet-id", lease, "subnet-id", true,
                            std::numeric_limits<std::uint32_t>::max(), error))
      return std::nullopt;
    if (dhcp6) {
      if (!AppendUnsignedLeaf(&xml, "preferred-lifetime", lease,
                              "preferred-lft", true,
                              std::numeric_limits<std::uint32_t>::max(), error))
        return std::nullopt;
      const auto type = lease.find("type");
      if (type == lease.end()) {
        if (error) *error = "Kea lease reply omits mandatory field type";
        return std::nullopt;
      }
      std::string lease_type;
      if (type->is_string() &&
          (*type == "IA_NA" || *type == "IA_PD"))
        lease_type = type->get<std::string>();
      else if (UnsignedValue(*type) == 0)
        lease_type = "IA_NA";
      else if (UnsignedValue(*type) == 2)
        lease_type = "IA_PD";
      else {
        if (error) *error = "Kea lease reply contains an unknown lease type";
        return std::nullopt;
      }
      xml += "<lease-type>" + lease_type + "</lease-type>";
      if (!AppendUnsignedLeaf(&xml, "iaid", lease, "iaid", true,
                              std::numeric_limits<std::uint32_t>::max(), error) ||
          !AppendUnsignedLeaf(&xml, "prefix-length", lease, "prefix-len",
                              false, 128, error))
        return std::nullopt;
    }
    if (!AppendBooleanLeaf(&xml, "fqdn-fwd", lease, "fqdn-fwd", false,
                           error) ||
        !AppendBooleanLeaf(&xml, "fqdn-rev", lease, "fqdn-rev", false,
                           error) ||
        !AppendStringLeaf(&xml, "hostname", lease, "hostname", false, error))
      return std::nullopt;
    const auto state = lease.find("state");
    if (state != lease.end()) {
      static constexpr const char* states[]{"default", "declined",
                                             "expired-reclaimed"};
      const auto state_value = UnsignedValue(*state);
      if (!state_value || *state_value > 2) {
        if (error) *error = "Kea lease reply contains an unknown state";
        return std::nullopt;
      }
      xml += "<state>" + std::string(states[*state_value]) +
          "</state>";
    }
    if (const auto context = lease.find("user-context");
        context != lease.end()) {
      auto context_text = JsonText(*context, "lease user-context", error);
      if (!context_text) return std::nullopt;
      auto escaped = XmlText(*context_text, "lease user-context", error);
      if (!escaped) return std::nullopt;
      xml += "<user-context>" + *escaped + "</user-context>";
    }
    if (dhcp6)
      if (!AppendStringLeaf(&xml, "hw-address", lease, "hw-address", false,
                            error))
        return std::nullopt;
    xml += "</lease>";
  }
  return xml + "</leases>";
}

std::optional<std::string> BuildStatistics(const nlohmann::json& response,
                                           bool dhcp6, std::string* error) {
  const nlohmann::json* answer = Answer(response);
  if (!answer || !answer->contains("result") ||
      !answer->at("result").is_number_integer()) {
    if (error) *error = "Kea statistics reply omits an integer result";
    return std::nullopt;
  }
  if (IsResultCode(answer->at("result"), 3)) return "<lease-stats/>";
  if (!IsResultCode(answer->at("result"), 0)) {
    if (error)
      *error = RejectionReason(*answer, "Kea rejected the statistics query");
    return std::nullopt;
  }
  try {
    const auto& result_set = answer->at("arguments").at("result-set");
    const auto& columns = result_set.at("columns");
    const auto& rows = result_set.at("rows");
    if (!columns.is_array() || !rows.is_array()) throw std::runtime_error("not arrays");
    std::map<std::string, std::size_t, std::less<>> indexes;
    for (std::size_t index = 0; index < columns.size(); ++index) {
      if (!columns[index].is_string())
        throw std::runtime_error("column name is not a string");
      const std::string name = columns[index].get<std::string>();
      if (!indexes.emplace(name, index).second)
        throw std::runtime_error("duplicate column " + name);
    }
    const std::vector<std::string_view> required = dhcp6
        ? std::vector<std::string_view>{"subnet-id", "total-nas", "assigned-nas",
                                        "declined-addresses", "total-pds",
                                        "assigned-pds"}
        : std::vector<std::string_view>{"subnet-id", "total-addresses",
                                        "assigned-addresses", "declined-addresses"};
    std::string xml = "<lease-stats>";
    std::set<std::uint32_t> subnet_ids;
    for (const auto& row : rows) {
      if (!row.is_array()) throw std::runtime_error("row is not an array");
      const auto subnet_position = indexes.find("subnet-id");
      const auto subnet_id = subnet_position == indexes.end() ||
              subnet_position->second >= row.size()
          ? std::nullopt
          : UnsignedValue(row[subnet_position->second]);
      if (subnet_position == indexes.end() ||
          subnet_position->second >= row.size() ||
          !subnet_id || *subnet_id >
              std::numeric_limits<std::uint32_t>::max() ||
          !subnet_ids.emplace(static_cast<std::uint32_t>(*subnet_id)).second)
        throw std::runtime_error("missing, invalid, or duplicate subnet-id");
      xml += "<subnet>";
      for (const auto name : required) {
        const auto position = indexes.find(name);
        const auto value = position == indexes.end() ||
                position->second >= row.size()
            ? std::nullopt
            : UnsignedValue(row[position->second]);
        if (!value || *value > std::numeric_limits<std::uint32_t>::max())
          throw std::runtime_error("missing unsigned column " + std::string(name));
        xml += "<" + std::string(name) + ">" + std::to_string(*value) +
            "</" + std::string(name) + ">";
      }
      xml += "</subnet>";
    }
    return xml + "</lease-stats>";
  } catch (const std::exception& exception) {
    if (error) *error = std::string("invalid Kea statistics reply: ") + exception.what();
    return std::nullopt;
  }
}

bool AppendOptionData(std::string* xml, const nlohmann::json& host, bool dhcp6,
                      std::string* error) {
  const auto options = host.find("option-data");
  if (options == host.end()) return true;
  if (!options->is_array()) {
    if (error) *error = "Kea host reply has non-array option-data";
    return false;
  }
  std::set<std::tuple<std::uint64_t, std::string, std::string>> option_keys;
  for (const auto& option : *options) {
    if (!option.is_object()) {
      if (error) *error = "Kea host reply has a non-object option-data entry";
      return false;
    }
    const auto code = option.contains("code")
        ? UnsignedValue(option.at("code")) : std::nullopt;
    const auto space = option.find("space");
    const auto data = option.find("data");
    const std::uint64_t maximum_code = dhcp6
        ? std::numeric_limits<std::uint16_t>::max()
        : std::numeric_limits<std::uint8_t>::max();
    if (!code || *code > maximum_code || space == option.end() ||
        !space->is_string() || data == option.end() || !data->is_string() ||
        !option_keys.emplace(*code, space->get<std::string>(),
                             data->get<std::string>()).second) {
      if (error)
        *error = "Kea host option-data contains a missing, invalid, or "
                 "duplicate key";
      return false;
    }
    *xml += "<option-data>";
    if (!AppendUnsignedLeaf(xml, "code", option, "code", true,
                            maximum_code, error) ||
        !AppendStringLeaf(xml, "space", option, "space", true, error) ||
        !AppendStringLeaf(xml, "name", option, "name", false, error) ||
        !AppendStringLeaf(xml, "data", option, "data", true, error) ||
        !AppendBooleanLeaf(xml, "csv-format", option, "csv-format", false,
                           error) ||
        !AppendBooleanLeaf(xml, "always-send", option, "always-send", false,
                           error) ||
        !AppendBooleanLeaf(xml, "never-send", option, "never-send", false,
                           error))
      return false;
    const auto classes = option.find("client-classes");
    if (classes != option.end()) {
      if (!classes->is_array()) {
        if (error)
          *error = "Kea host option-data has non-array client-classes";
        return false;
      }
      std::set<std::string, std::less<>> class_names;
      for (const auto& value : *classes) {
        if (!value.is_string() ||
            !class_names.emplace(value.get<std::string>()).second) {
          if (error)
            *error = "Kea host option-data has invalid or duplicate "
                     "client-classes";
          return false;
        }
        auto escaped = XmlText(value.get_ref<const std::string&>(),
                               "option client-classes", error);
        if (!escaped) return false;
        *xml += "<client-classes>" + *escaped + "</client-classes>";
      }
    }
    if (const auto context = option.find("user-context");
        context != option.end()) {
      auto context_text = JsonText(*context, "option user-context", error);
      if (!context_text) return false;
      auto escaped = XmlText(*context_text, "option user-context", error);
      if (!escaped) return false;
      *xml += "<user-context>" + *escaped + "</user-context>";
    }
    *xml += "</option-data>";
  }
  return true;
}

std::optional<std::string> BuildHosts(const nlohmann::json& response,
                                      bool dhcp6, std::string* error) {
  const nlohmann::json* answer = Answer(response);
  if (!answer || !answer->contains("result") ||
      !answer->at("result").is_number_integer()) {
    if (error) *error = "Kea host reply omits an integer result";
    return std::nullopt;
  }
  if (IsResultCode(answer->at("result"), 3)) return "<hosts/>";
  if (!IsResultCode(answer->at("result"), 0)) {
    if (error) *error = RejectionReason(*answer, "Kea rejected the host query");
    return std::nullopt;
  }
  const auto arguments = answer->find("arguments");
  if (arguments == answer->end() || !arguments->is_object()) {
    if (error) *error = "Kea host reply omits arguments";
    return std::nullopt;
  }
  const auto hosts = arguments->find("hosts");
  if (hosts == arguments->end() || !hosts->is_array()) {
    if (error) *error = "Kea host reply omits the hosts array";
    return std::nullopt;
  }
  std::string xml = "<hosts>";
  std::set<std::tuple<std::uint32_t, std::string, std::string>> identities;
  for (const auto& host : *hosts) {
    if (!host.is_object()) {
      if (error) *error = "Kea host reply contains a non-object entry";
      return std::nullopt;
    }
    std::string identifier_type;
    std::string identifier;
    for (const std::string_view candidate :
         {"duid", "hw-address", "circuit-id", "client-id", "flex-id"}) {
      const auto found = host.find(candidate);
      if (found == host.end()) continue;
      // The DHCPv6 model deliberately has no circuit-id or client-id enum.
      // Publishing either under a complete result would create invalid XML.
      if (dhcp6 && (candidate == "circuit-id" || candidate == "client-id")) {
        if (error)
          *error = "Kea DHCPv6 host reply contains unsupported identifier " +
              std::string(candidate);
        return std::nullopt;
      }
      if (!found->is_string() || found->get_ref<const std::string&>().empty()) {
        if (error)
          *error = "Kea host reply contains malformed identifier " +
              std::string(candidate);
        return std::nullopt;
      }
      if (!identifier_type.empty()) {
        if (error) *error = "Kea host reply contains multiple identifiers";
        return std::nullopt;
      }
      identifier_type = candidate;
      identifier = found->get<std::string>();
    }
    if (identifier_type.empty()) {
      if (error) *error = "Kea host reply omits its identifier";
      return std::nullopt;
    }
    const auto subnet = host.find("subnet-id");
    const auto subnet_id = subnet == host.end()
        ? std::nullopt
        : UnsignedValue(*subnet);
    if (!subnet_id ||
        *subnet_id > std::numeric_limits<std::uint32_t>::max() ||
        !identities.emplace(static_cast<std::uint32_t>(*subnet_id), identifier_type,
                            identifier).second) {
      if (error)
        *error = "Kea host reply contains a missing, invalid, or duplicate key";
      return std::nullopt;
    }
    xml += "<host>";
    if (!AppendUnsignedLeaf(&xml, "subnet-id", host, "subnet-id", true,
                            std::numeric_limits<std::uint32_t>::max(), error))
      return std::nullopt;
    xml += "<identifier-type>" + identifier_type + "</identifier-type>";
    auto escaped_identifier = XmlText(identifier, "host identifier", error);
    if (!escaped_identifier) return std::nullopt;
    xml += "<identifier>" + *escaped_identifier + "</identifier>";
    if (dhcp6) {
      for (const std::string_view name :
           {"ip-addresses", "prefixes", "excluded-prefixes"}) {
        const auto values = host.find(name);
        if (values == host.end()) continue;
        if (!values->is_array()) {
          if (error) *error = "Kea host reply has a non-array " + std::string(name);
          return std::nullopt;
        }
        std::set<std::string, std::less<>> unique_values;
        for (const auto& value : *values) {
          if (!value.is_string() ||
              !unique_values.emplace(value.get<std::string>()).second) {
            if (error)
              *error = "Kea host reply has an invalid or duplicate " +
                  std::string(name);
            return std::nullopt;
          }
          auto escaped = XmlText(value.get_ref<const std::string&>(), name,
                                 error);
          if (!escaped) return std::nullopt;
          xml += "<" + std::string(name) + ">" + *escaped + "</" +
                 std::string(name) + ">";
        }
      }
    } else if (!AppendStringLeaf(&xml, "ip-address", host, "ip-address", false,
                                  error)) {
      return std::nullopt;
    }
    for (const std::string_view name :
         {"hostname", "next-server", "server-hostname", "boot-file-name",
          "auth-key"})
      if (!AppendStringLeaf(&xml, name, host, name, false, error))
        return std::nullopt;
    if (!AppendOptionData(&xml, host, dhcp6, error)) return std::nullopt;
    const auto classes = host.find("client-classes");
    if (classes != host.end()) {
      if (!classes->is_array()) {
        if (error) *error = "Kea host reply has non-array client-classes";
        return std::nullopt;
      }
      std::set<std::string, std::less<>> class_names;
      for (const auto& value : *classes) {
        if (!value.is_string() ||
            !class_names.emplace(value.get<std::string>()).second) {
          if (error)
            *error = "Kea host reply has invalid or duplicate client-classes";
          return std::nullopt;
        }
        auto escaped = XmlText(value.get_ref<const std::string&>(),
                               "host client-classes", error);
        if (!escaped) return std::nullopt;
        xml += "<client-classes>" + *escaped + "</client-classes>";
      }
    }
    if (const auto context = host.find("user-context"); context != host.end()) {
      auto context_text = JsonText(*context, "host user-context", error);
      if (!context_text) return std::nullopt;
      auto escaped = XmlText(*context_text, "host user-context", error);
      if (!escaped) return std::nullopt;
      xml += "<user-context>" + *escaped + "</user-context>";
    }
    xml += "</host>";
  }
  return xml + "</hosts>";
}

void FindSubnetIds(const nlohmann::json& value, std::string_view list_name,
                   std::vector<std::uint32_t>* result) {
  if (value.is_array()) {
    for (const auto& child : value) FindSubnetIds(child, list_name, result);
    return;
  }
  if (!value.is_object()) return;
  for (const auto& [name, child] : value.items()) {
    if (name == list_name && child.is_array()) {
      for (const auto& subnet : child) {
        if (!subnet.is_object() || !subnet.contains("id")) continue;
        const auto& id = subnet.at("id");
        if (id.is_number_unsigned()) {
          const auto unsigned_id = id.get<std::uint64_t>();
          if (unsigned_id <= std::numeric_limits<std::uint32_t>::max())
            result->push_back(static_cast<std::uint32_t>(unsigned_id));
        } else if (id.is_number_integer()) {
          const auto signed_id = id.get<std::int64_t>();
          if (signed_id >= 0 && static_cast<std::uint64_t>(signed_id) <=
                                    std::numeric_limits<std::uint32_t>::max())
            result->push_back(static_cast<std::uint32_t>(signed_id));
        }
      }
    }
    FindSubnetIds(child, list_name, result);
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
  if (!ValidateSocketPath(socket_path, error)) return std::nullopt;
  if (datastore_xml.size() >
      static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    if (error) *error = "Kea datastore snapshot exceeds the XML parser limit";
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
  const xmlNode* config = nullptr;
  std::size_t configuration_count = 0;
  FindConfigurations(xmlDocGetRootElement(document), expected_namespace,
                     &config, &configuration_count);
  if (configuration_count > 1) {
    xmlFreeDoc(document);
    if (error) *error = "Kea datastore contains multiple configuration containers";
    return std::nullopt;
  }
  if (config) {
    if (const auto foreign = ForeignElement(config, expected_namespace);
        foreign) {
      xmlFreeDoc(document);
      if (error)
        *error = "Kea configuration contains foreign-namespace element " +
            *foreign;
      return std::nullopt;
    }
  }
  nlohmann::json body;
  try {
    body = config ? ConvertNode(config) : nlohmann::json::object();
  } catch (const std::exception& exception) {
    xmlFreeDoc(document);
    if (error)
      *error = std::string("invalid Kea configuration structure: ") +
          exception.what();
    return std::nullopt;
  }
  xmlFreeDoc(document);
  if (!PreservesControlSocket(body, socket_path)) {
    if (error)
      *error = "Kea configuration must preserve the managed UNIX control "
               "socket " +
          std::string(socket_path);
    return std::nullopt;
  }
  if (const auto missing_hook = MissingRequiredHook(body); missing_hook) {
    if (error)
      *error = "Kea configuration must preserve required command hook " +
          *missing_hook;
    return std::nullopt;
  }
  const std::string service = dhcp4 ? "Dhcp4" : "Dhcp6";
  return ServerConfiguration{std::string(module_name), service,
                             std::string(socket_path),
                             nlohmann::json{{service, std::move(body)}}};
}

std::optional<nlohmann::json> SendControlCommand(
    const ServerConfiguration& server, std::string_view command,
    std::string* error) {
  return SendControlQuery(server.socket_path, command, server.arguments, error);
}

std::optional<nlohmann::json> SendControlQuery(
    std::string_view socket_path, std::string_view command,
    const nlohmann::json& arguments, std::string* error) {
  if (!ValidateSocketPath(socket_path, error)) return std::nullopt;
  nlohmann::json request_object{{"command", command}};
  if (!arguments.is_null()) request_object["arguments"] = arguments;
  std::string request;
  try {
    request = request_object.dump();
  } catch (const std::exception& exception) {
    if (error)
      *error = std::string("invalid Kea request: ") + exception.what();
    return std::nullopt;
  }
  if (request.size() > kMaximumControlBytes) {
    if (error) *error = "Kea request exceeds the plugin limit";
    return std::nullopt;
  }
  const int descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
  if (descriptor < 0) {
    if (error) *error = std::string("cannot create Kea control socket: ") +
        std::strerror(errno);
    return std::nullopt;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, socket_path.data(), socket_path.size());
  address.sun_path[socket_path.size()] = '\0';
  const auto address_size = static_cast<socklen_t>(
      offsetof(sockaddr_un, sun_path) + socket_path.size() + 1);
  const int descriptor_flags = fcntl(descriptor, F_GETFL, 0);
  if (descriptor_flags < 0 ||
      fcntl(descriptor, F_SETFL, descriptor_flags | O_NONBLOCK) != 0) {
    if (error) *error = std::string("cannot make Kea control socket nonblocking: ") +
        std::strerror(errno);
    close(descriptor);
    return std::nullopt;
  }
  const auto deadline = std::chrono::steady_clock::now() + kSocketTimeout;
  if (connect(descriptor, reinterpret_cast<const sockaddr*>(&address),
              address_size) != 0) {
    if (errno != EINPROGRESS || !WaitFor(descriptor, POLLOUT, deadline, error)) {
      if (errno != EINPROGRESS && error)
        *error = "cannot connect to " + std::string(socket_path) + ": " +
            std::strerror(errno);
      close(descriptor);
      return std::nullopt;
    }
    int connect_error = 0;
    socklen_t connect_error_size = sizeof(connect_error);
    if (getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &connect_error,
                   &connect_error_size) != 0 || connect_error != 0) {
      if (error)
        *error = "cannot connect to " + std::string(socket_path) + ": " +
            std::strerror(connect_error != 0 ? connect_error : errno);
      close(descriptor);
      return std::nullopt;
    }
  }
  // Keep the descriptor nonblocking for the complete exchange. Restoring the
  // blocking mode here would allow a large send or a readiness race on recv to
  // outlive the single deadline even though poll itself is bounded.
#if !defined(MSG_NOSIGNAL) && defined(SO_NOSIGPIPE)
  // Darwin and some BSD socket stacks suppress SIGPIPE per descriptor rather
  // than per send. A disappearing Kea peer must become a plugin error, never a
  // process-wide signal that terminates dangd.
  const int suppress_sigpipe = 1;
  if (setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &suppress_sigpipe,
                 sizeof(suppress_sigpipe)) != 0) {
    if (error) *error = std::string("cannot protect Kea control socket: ") +
        std::strerror(errno);
    close(descriptor);
    return std::nullopt;
  }
#endif
  std::size_t sent = 0;
  while (sent < request.size()) {
    if (!WaitFor(descriptor, POLLOUT, deadline, error)) {
      close(descriptor);
      return std::nullopt;
    }
    const ssize_t count = send(descriptor, request.data() + sent,
                               request.size() - sent,
#ifdef MSG_NOSIGNAL
                               MSG_NOSIGNAL
#else
                               0
#endif
    );
    if (count > 0) {
      sent += static_cast<std::size_t>(count);
    } else if (count == 0) {
      if (error) *error = "Kea control socket write made no progress";
      close(descriptor);
      return std::nullopt;
    } else if (count < 0 && errno != EINTR && errno != EAGAIN &&
               errno != EWOULDBLOCK) {
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
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
      if (error) *error = std::string("cannot read Kea response: ") +
          std::strerror(errno);
      close(descriptor);
      return std::nullopt;
    }
    if (static_cast<std::size_t>(count) >
        kMaximumControlBytes - reply.size()) {
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

std::optional<nlohmann::json> CollectLeasePages(
    std::string_view socket_path, bool dhcp6, const ControlQuery& query,
    std::string* error, const PageLimits& limits) {
  if (!query || limits.page_size == 0 || limits.maximum_pages == 0 ||
      limits.maximum_items == 0 || limits.maximum_bytes == 0 ||
      limits.maximum_duration <= std::chrono::milliseconds::zero()) {
    if (error) *error = "invalid Kea lease paging configuration";
    return std::nullopt;
  }
  const std::string command = dhcp6 ? "lease6-get-page" : "lease4-get-page";
  std::string cursor = "start";
  std::set<std::string, std::less<>> cursors{cursor};
  nlohmann::json collected = nlohmann::json::array();
  std::size_t collected_bytes = 0;
  const auto deadline = std::chrono::steady_clock::now() +
                        limits.maximum_duration;
  for (std::size_t page = 0; page < limits.maximum_pages; ++page) {
    if (std::chrono::steady_clock::now() >= deadline) {
      if (error) *error = "Kea lease enumeration exceeded its deadline";
      return std::nullopt;
    }
    const nlohmann::json arguments{{"from", cursor},
                                   {"limit", limits.page_size}};
    auto response = RunControlQuery(query, socket_path, command, arguments,
                                    error);
    if (!response) return std::nullopt;
    if (std::chrono::steady_clock::now() >= deadline) {
      if (error) *error = "Kea lease enumeration exceeded its deadline";
      return std::nullopt;
    }
    const nlohmann::json* answer = Answer(*response);
    if (!answer || !answer->contains("result") ||
        !answer->at("result").is_number_integer()) {
      if (error) *error = "Kea lease page omits an integer result";
      return std::nullopt;
    }
    if (IsResultCode(answer->at("result"), 3)) {
      return std::optional<nlohmann::json>(nlohmann::json{
          {"result", collected.empty() ? 3 : 0},
          {"arguments", {{"leases", collected}}}});
    }
    if (!IsResultCode(answer->at("result"), 0)) {
      if (error) *error = RejectionReason(*answer, "Kea rejected the lease page");
      return std::nullopt;
    }
    const auto arguments_node = answer->find("arguments");
    if (arguments_node == answer->end() || !arguments_node->is_object()) {
      if (error) *error = "Kea lease page omits arguments";
      return std::nullopt;
    }
    const auto leases = arguments_node->find("leases");
    const auto count = arguments_node->find("count");
    const auto count_value = count == arguments_node->end()
        ? std::nullopt
        : UnsignedValue(*count);
    if (leases == arguments_node->end() || !leases->is_array() ||
        !count_value || *count_value != leases->size() ||
        leases->size() > limits.page_size) {
      if (error) *error = "Kea lease page has an invalid leases/count result";
      return std::nullopt;
    }
    if (collected.size() + leases->size() > limits.maximum_items) {
      if (error) *error = "Kea lease enumeration exceeds the item limit";
      return std::nullopt;
    }
    for (const auto& lease : *leases) {
      auto encoded = JsonText(lease, "lease page entry", error);
      if (!encoded) return std::nullopt;
      if (encoded->size() > limits.maximum_bytes - collected_bytes) {
        if (error) *error = "Kea lease enumeration exceeds the byte limit";
        return std::nullopt;
      }
      collected_bytes += encoded->size();
      collected.push_back(lease);
    }
    if (leases->size() < limits.page_size) {
      return std::optional<nlohmann::json>(nlohmann::json{
          {"result", collected.empty() ? 3 : 0},
          {"arguments", {{"leases", collected}}}});
    }
    const auto& last = leases->back();
    if (!last.is_object() || !last.contains("ip-address") ||
        !last.at("ip-address").is_string()) {
      if (error) *error = "Kea lease page omits its continuation address";
      return std::nullopt;
    }
    const std::string next = last.at("ip-address").get<std::string>();
    if (next.empty() || !cursors.emplace(next).second) {
      if (error) *error = "Kea lease paging cursor is empty or repeated";
      return std::nullopt;
    }
    cursor = next;
  }
  if (error) *error = "Kea lease enumeration exceeds the page limit";
  return std::nullopt;
}

std::optional<nlohmann::json> CollectHostPages(
    std::string_view socket_path, const ControlQuery& query,
    std::string* error, const PageLimits& limits) {
  if (!query || limits.page_size == 0 || limits.maximum_pages == 0 ||
      limits.maximum_items == 0 || limits.maximum_bytes == 0 ||
      limits.maximum_duration <= std::chrono::milliseconds::zero()) {
    if (error) *error = "invalid Kea host paging configuration";
    return std::nullopt;
  }
  nlohmann::json collected = nlohmann::json::array();
  nlohmann::json cursor = nlohmann::json::object();
  std::set<std::pair<std::uint64_t, std::uint64_t>> cursors;
  std::size_t collected_bytes = 0;
  const auto deadline = std::chrono::steady_clock::now() + limits.maximum_duration;
  for (std::size_t page = 0; page < limits.maximum_pages; ++page) {
    if (std::chrono::steady_clock::now() >= deadline) {
      if (error) *error = "Kea host enumeration exceeded its deadline";
      return std::nullopt;
    }
    nlohmann::json arguments{{"limit", limits.page_size}};
    arguments.update(cursor);
    auto response = RunControlQuery(query, socket_path, "reservation-get-page",
                                    arguments, error);
    if (!response) return std::nullopt;
    if (std::chrono::steady_clock::now() >= deadline) {
      if (error) *error = "Kea host enumeration exceeded its deadline";
      return std::nullopt;
    }
    const nlohmann::json* answer = Answer(*response);
    if (!answer || !answer->contains("result") ||
        !answer->at("result").is_number_integer()) {
      if (error) *error = "Kea host page omits an integer result";
      return std::nullopt;
    }
    if (IsResultCode(answer->at("result"), 3))
      return std::optional<nlohmann::json>(nlohmann::json{
          {"result", collected.empty() ? 3 : 0},
          {"arguments", {{"hosts", collected}}}});
    if (!IsResultCode(answer->at("result"), 0)) {
      if (error) *error = RejectionReason(*answer, "Kea rejected the host page");
      return std::nullopt;
    }
    const auto arguments_node = answer->find("arguments");
    if (arguments_node == answer->end() || !arguments_node->is_object() ||
        !arguments_node->contains("hosts") ||
        !arguments_node->at("hosts").is_array() ||
        !arguments_node->contains("count")) {
      if (error) *error = "Kea host page has an invalid hosts result";
      return std::nullopt;
    }
    const auto& hosts = arguments_node->at("hosts");
    const auto count = UnsignedValue(arguments_node->at("count"));
    if (!count || *count != hosts.size() || hosts.size() > limits.page_size) {
      if (error) *error = "Kea host page has an invalid hosts result";
      return std::nullopt;
    }
    if (collected.size() + hosts.size() > limits.maximum_items) {
      if (error) *error = "Kea host enumeration exceeds the item limit";
      return std::nullopt;
    }
    for (const auto& host : hosts) {
      auto encoded = JsonText(host, "host page entry", error);
      if (!encoded) return std::nullopt;
      if (encoded->size() > limits.maximum_bytes - collected_bytes) {
        if (error) *error = "Kea host enumeration exceeds the byte limit";
        return std::nullopt;
      }
      collected_bytes += encoded->size();
      collected.push_back(host);
    }
    const auto next = arguments_node->find("next");
    const auto from = next == arguments_node->end() || !next->is_object() ||
            !next->contains("from")
        ? std::nullopt
        : UnsignedValue(next->at("from"));
    const auto source_index =
        next == arguments_node->end() || !next->is_object() ||
                !next->contains("source-index")
            ? std::nullopt
            : UnsignedValue(next->at("source-index"));
    if (next == arguments_node->end() || !next->is_object() ||
        !from || !source_index ||
        *from > static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max()) ||
        *source_index > static_cast<std::uint64_t>(
                            std::numeric_limits<std::int64_t>::max()) ||
        !cursors.emplace(*from, *source_index).second) {
      if (error) *error = "Kea host paging cursor is missing or repeated";
      return std::nullopt;
    }
    cursor = {{"from", next->at("from")},
              {"source-index", next->at("source-index")}};
  }
  if (error) *error = "Kea host enumeration exceeds the page limit";
  return std::nullopt;
}

std::vector<std::uint32_t> ExtractSubnetIds(
    const ServerConfiguration& server) {
  std::vector<std::uint32_t> result;
  const std::string list_name =
      server.module_name == "kea-dhcp6-server" ? "subnet6" : "subnet4";
  FindSubnetIds(server.arguments, list_name, &result);
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

std::optional<nlohmann::json> CollectStatistics(
    std::string_view socket_path, bool dhcp6,
    const std::vector<std::uint32_t>& subnet_ids, const ControlQuery& query,
    std::string* error, const PageLimits& limits) {
  if (!query || limits.maximum_pages == 0 || limits.maximum_items == 0 ||
      limits.maximum_bytes == 0 ||
      limits.maximum_duration <= std::chrono::milliseconds::zero()) {
    if (error) *error = "invalid Kea statistics collection configuration";
    return std::nullopt;
  }
  if (subnet_ids.size() > limits.maximum_pages) {
    if (error) *error = "Kea statistics collection exceeds the query limit";
    return std::nullopt;
  }
  const std::string command = dhcp6 ? "stat-lease6-get" : "stat-lease4-get";
  const auto deadline = std::chrono::steady_clock::now() + limits.maximum_duration;
  nlohmann::json columns;
  nlohmann::json rows = nlohmann::json::array();
  std::size_t collected_bytes = 0;
  for (const std::uint32_t subnet_id : subnet_ids) {
    if (std::chrono::steady_clock::now() >= deadline) {
      if (error) *error = "Kea statistics collection exceeded its deadline";
      return std::nullopt;
    }
    auto response = RunControlQuery(
        query, socket_path, command, nlohmann::json{{"subnet-id", subnet_id}},
        error);
    if (!response) return std::nullopt;
    if (std::chrono::steady_clock::now() >= deadline) {
      if (error) *error = "Kea statistics collection exceeded its deadline";
      return std::nullopt;
    }
    const nlohmann::json* answer = Answer(*response);
    if (!answer || !answer->contains("result") ||
        !answer->at("result").is_number_integer()) {
      if (error) *error = "Kea statistics reply omits an integer result";
      return std::nullopt;
    }
    if (IsResultCode(answer->at("result"), 3)) continue;
    if (!IsResultCode(answer->at("result"), 0)) {
      if (error)
        *error = RejectionReason(*answer, "Kea rejected statistics query");
      return std::nullopt;
    }
    try {
      const auto& set = answer->at("arguments").at("result-set");
      const auto& response_columns = set.at("columns");
      const auto& response_rows = set.at("rows");
      if (!response_columns.is_array() || !response_rows.is_array())
        throw std::runtime_error("columns or rows are not arrays");
      std::optional<std::size_t> subnet_column;
      std::set<std::string, std::less<>> column_names;
      for (std::size_t index = 0; index < response_columns.size(); ++index) {
        if (!response_columns[index].is_string())
          throw std::runtime_error("column name is not a string");
        const std::string name = response_columns[index].get<std::string>();
        if (!column_names.emplace(name).second)
          throw std::runtime_error("duplicate column " + name);
        if (name == "subnet-id") subnet_column = index;
      }
      if (!subnet_column)
        throw std::runtime_error("subnet-id column is missing");
      if (response_rows.size() != 1 || !response_rows.front().is_array() ||
          *subnet_column >= response_rows.front().size())
        throw std::runtime_error("expected one complete subnet row");
      const auto& returned_id = response_rows.front()[*subnet_column];
      const bool matching_unsigned = returned_id.is_number_unsigned() &&
          returned_id.get<std::uint64_t>() == subnet_id;
      const bool matching_signed = returned_id.is_number_integer() &&
          !returned_id.is_number_unsigned() &&
          returned_id.get<std::int64_t>() >= 0 &&
          static_cast<std::uint64_t>(returned_id.get<std::int64_t>()) ==
              subnet_id;
      if (!matching_unsigned && !matching_signed)
        throw std::runtime_error("subnet-id does not match the query");
      if (columns.is_null()) columns = response_columns;
      if (columns != response_columns)
        throw std::runtime_error("columns changed between subnet queries");
      if (rows.size() + response_rows.size() > limits.maximum_items)
        throw std::runtime_error("row limit exceeded");
      for (const auto& row : response_rows) {
        const std::string encoded = row.dump();
        if (encoded.size() > limits.maximum_bytes - collected_bytes)
          throw std::runtime_error("byte limit exceeded");
        collected_bytes += encoded.size();
        rows.push_back(row);
      }
    } catch (const std::exception& exception) {
      if (error)
        *error = std::string("invalid Kea statistics reply: ") + exception.what();
      return std::nullopt;
    }
  }
  if (columns.is_null())
    return std::optional<nlohmann::json>(nlohmann::json{{"result", 3}});
  return std::optional<nlohmann::json>(nlohmann::json{
      {"result", 0},
      {"arguments", {{"result-set", {{"columns", columns}, {"rows", rows}}}}}});
}

std::optional<std::string> TranslateOperationalState(
    std::string_view module_name, const nlohmann::json& leases,
    const nlohmann::json& statistics, const nlohmann::json& hosts,
    std::string* error) {
  const bool dhcp6 = module_name == "kea-dhcp6-server";
  if (!dhcp6 && module_name != "kea-dhcp4-server") {
    if (error) *error = "unsupported Kea module";
    return std::nullopt;
  }
  auto lease_xml = BuildLeases(leases, dhcp6, error);
  if (!lease_xml) return std::nullopt;
  auto statistic_xml = BuildStatistics(statistics, dhcp6, error);
  if (!statistic_xml) return std::nullopt;
  auto host_xml = BuildHosts(hosts, dhcp6, error);
  if (!host_xml) return std::nullopt;
  const std::string ns = "urn:ietf:params:xml:ns:yang:" +
      std::string(module_name);
  return "<state xmlns=\"" + ns + "\">" + *lease_xml + *statistic_xml +
      *host_xml + "</state>";
}

bool CommandSucceeded(const nlohmann::json& response, std::string* reason) {
  const nlohmann::json* answer = &response;
  if (response.is_array()) {
    if (response.size() != 1) {
      if (reason) *reason = "Kea returned an ambiguous command response";
      return false;
    }
    answer = &response.front();
  }
  if (!answer->is_object() || !answer->contains("result") ||
      !(*answer)["result"].is_number_integer()) {
    if (reason) *reason = "Kea returned a response without an integer result";
    return false;
  }
  if (IsResultCode((*answer)["result"], 0)) return true;
  if (reason) *reason = RejectionReason(*answer, "Kea rejected the command");
  return false;
}

bool RunConfigurationCommand(const ConfigurationCommand& command,
                             const ServerConfiguration& server,
                             std::string_view operation,
                             std::string* reason) {
  try {
    return command(server, operation, reason);
  } catch (const std::exception& exception) {
    if (reason)
      *reason = std::string("configuration command threw: ") + exception.what();
    return false;
  } catch (...) {
    if (reason) *reason = "configuration command threw an unknown exception";
    return false;
  }
}

bool ApplyWithCompensation(
    const std::vector<ServerConfiguration>& before,
    const std::vector<ServerConfiguration>& proposed,
    const ConfigurationCommand& command, std::string* failed_module,
    std::string* reason) {
  if (failed_module) failed_module->clear();
  if (reason) reason->clear();
  if (!command || before.size() != proposed.size() || proposed.empty()) {
    if (reason) *reason = "invalid Kea configuration transaction";
    return false;
  }
  std::set<std::string, std::less<>> modules;
  for (std::size_t index = 0; index < proposed.size(); ++index) {
    const auto& old_server = before[index];
    const auto& new_server = proposed[index];
    if (old_server.module_name.empty() || old_server.service_name.empty() ||
        old_server.socket_path.empty() ||
        old_server.module_name != new_server.module_name ||
        old_server.service_name != new_server.service_name ||
        old_server.socket_path != new_server.socket_path ||
        !modules.emplace(old_server.module_name).second) {
      if (reason) *reason = "invalid Kea configuration transaction pairing";
      return false;
    }
  }
  for (std::size_t index = 0; index < proposed.size(); ++index) {
    std::string apply_error;
    if (RunConfigurationCommand(command, proposed[index], "config-set",
                                &apply_error))
      continue;
    if (apply_error.empty()) apply_error = "configuration command failed";
    if (failed_module) *failed_module = proposed[index].module_name;
    std::string failure = proposed[index].module_name + ": " + apply_error;
    // A missing or malformed response does not prove config-set was rejected.
    // Restore the failed target as well as all earlier successful targets.
    for (std::size_t restore = index + 1; restore > 0; --restore) {
      const auto& server = before[restore - 1];
      std::string rollback_error;
      if (!RunConfigurationCommand(command, server, "config-set",
                                   &rollback_error)) {
        if (rollback_error.empty())
          rollback_error = "configuration rollback command failed";
        failure += "; rollback of " + server.module_name + " failed: " +
            rollback_error;
      }
    }
    if (reason) *reason = std::move(failure);
    return false;
  }
  return true;
}

}  // namespace dang::plugins::kea

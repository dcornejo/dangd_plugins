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
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
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

const nlohmann::json* Answer(const nlohmann::json& response) {
  if (response.is_array() && response.size() == 1) return &response.front();
  return response.is_object() ? &response : nullptr;
}

std::string XmlEscape(std::string_view value) {
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
  return escaped;
}

std::optional<std::string> BinaryBase64(std::string_view hexadecimal) {
  std::vector<std::uint8_t> bytes;
  for (std::size_t offset = 0; offset < hexadecimal.size();) {
    if (hexadecimal[offset] == ':') {
      ++offset;
      continue;
    }
    if (offset + 2 > hexadecimal.size()) return std::nullopt;
    unsigned int byte = 0;
    const auto [end, error] = std::from_chars(
        hexadecimal.data() + offset, hexadecimal.data() + offset + 2, byte, 16);
    if (error != std::errc{} || end != hexadecimal.data() + offset + 2 ||
        byte > 255)
      return std::nullopt;
    bytes.push_back(static_cast<std::uint8_t>(byte));
    offset += 2;
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

bool AppendLeaf(std::string* xml, std::string_view name,
                const nlohmann::json& object, std::string_view key,
                bool mandatory, std::string* error) {
  const auto found = object.find(key);
  if (found == object.end() || found->is_null()) {
    if (!mandatory) return true;
    if (error) *error = "Kea response omits mandatory field " + std::string(key);
    return false;
  }
  std::string value;
  if (found->is_string()) value = found->get<std::string>();
  else if (found->is_boolean()) value = *found ? "true" : "false";
  else if (found->is_number()) value = found->dump();
  else value = found->dump();
  *xml += "<" + std::string(name) + ">" + XmlEscape(value) + "</" +
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
  const int result = answer->at("result").get<int>();
  if (result == 3) return "<leases/>";
  if (result != 0) {
    if (error) *error = answer->value("text", "Kea rejected the lease query");
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
  for (const auto& lease : *leases) {
    if (!lease.is_object()) {
      if (error) *error = "Kea lease reply contains a non-object entry";
      return std::nullopt;
    }
    xml += "<lease>";
    if (!AppendLeaf(&xml, "ip-address", lease, "ip-address", true, error))
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
      if (client != lease.end() && client->is_string()) {
        auto client_id = BinaryBase64(client->get<std::string>());
        if (!client_id) {
          if (error) *error = "Kea lease reply contains malformed client-id";
          return std::nullopt;
        }
        xml += "<client-id>" + *client_id + "</client-id>";
      }
    }
    if (!AppendLeaf(&xml, "valid-lifetime", lease, "valid-lft", true, error) ||
        !AppendLeaf(&xml, "cltt", lease, "cltt", true, error) ||
        !AppendLeaf(&xml, "subnet-id", lease, "subnet-id", true, error))
      return std::nullopt;
    if (dhcp6) {
      if (!AppendLeaf(&xml, "preferred-lifetime", lease, "preferred-lft", true,
                      error))
        return std::nullopt;
      const auto type = lease.find("type");
      if (type == lease.end()) {
        if (error) *error = "Kea lease reply omits mandatory field type";
        return std::nullopt;
      }
      std::string lease_type;
      if (type->is_string()) lease_type = type->get<std::string>();
      else if (type->is_number_unsigned() && type->get<unsigned int>() == 0)
        lease_type = "IA_NA";
      else if (type->is_number_unsigned() && type->get<unsigned int>() == 2)
        lease_type = "IA_PD";
      else {
        if (error) *error = "Kea lease reply contains an unknown lease type";
        return std::nullopt;
      }
      xml += "<lease-type>" + lease_type + "</lease-type>";
      if (!AppendLeaf(&xml, "iaid", lease, "iaid", true, error) ||
          !AppendLeaf(&xml, "prefix-length", lease, "prefix-len", false, error))
        return std::nullopt;
    }
    for (const auto& [name, key] :
         {std::pair{"fqdn-fwd", "fqdn-fwd"}, {"fqdn-rev", "fqdn-rev"},
          {"hostname", "hostname"}})
      if (!AppendLeaf(&xml, name, lease, key, false, error)) return std::nullopt;
    const auto state = lease.find("state");
    if (state != lease.end()) {
      static constexpr const char* states[]{"default", "declined",
                                             "expired-reclaimed"};
      if (!state->is_number_unsigned() || state->get<unsigned int>() > 2) {
        if (error) *error = "Kea lease reply contains an unknown state";
        return std::nullopt;
      }
      xml += "<state>" + std::string(states[state->get<unsigned int>()]) +
          "</state>";
    }
    if (const auto context = lease.find("user-context");
        context != lease.end())
      xml += "<user-context>" + XmlEscape(context->dump()) +
          "</user-context>";
    if (dhcp6)
      if (!AppendLeaf(&xml, "hw-address", lease, "hw-address", false, error))
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
  const int result = answer->at("result").get<int>();
  if (result == 3) return "<lease-stats/>";
  if (result != 0) {
    if (error)
      *error = answer->value("text", "Kea rejected the statistics query");
    return std::nullopt;
  }
  try {
    const auto& result_set = answer->at("arguments").at("result-set");
    const auto& columns = result_set.at("columns");
    const auto& rows = result_set.at("rows");
    if (!columns.is_array() || !rows.is_array()) throw std::runtime_error("not arrays");
    std::map<std::string, std::size_t, std::less<>> indexes;
    for (std::size_t index = 0; index < columns.size(); ++index)
      if (columns[index].is_string()) indexes.emplace(columns[index], index);
    const std::vector<std::string_view> required = dhcp6
        ? std::vector<std::string_view>{"subnet-id", "total-nas", "assigned-nas",
                                        "declined-addresses", "total-pds",
                                        "assigned-pds"}
        : std::vector<std::string_view>{"subnet-id", "total-addresses",
                                        "assigned-addresses", "declined-addresses"};
    std::string xml = "<lease-stats>";
    for (const auto& row : rows) {
      if (!row.is_array()) throw std::runtime_error("row is not an array");
      xml += "<subnet>";
      for (const auto name : required) {
        const auto position = indexes.find(name);
        if (position == indexes.end() || position->second >= row.size() ||
            !row[position->second].is_number_unsigned())
          throw std::runtime_error("missing unsigned column " + std::string(name));
        xml += "<" + std::string(name) + ">" +
            row[position->second].dump() + "</" + std::string(name) + ">";
      }
      xml += "</subnet>";
    }
    return xml + "</lease-stats>";
  } catch (const std::exception& exception) {
    if (error) *error = std::string("invalid Kea statistics reply: ") + exception.what();
    return std::nullopt;
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
  return SendControlQuery(server.socket_path, command, server.arguments, error);
}

std::optional<nlohmann::json> SendControlQuery(
    std::string_view socket_path, std::string_view command,
    const nlohmann::json& arguments, std::string* error) {
  const int descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
  if (descriptor < 0) {
    if (error) *error = std::string("cannot create Kea control socket: ") +
        std::strerror(errno);
    return std::nullopt;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (socket_path.empty() || socket_path.size() >= sizeof(address.sun_path)) {
    if (error) *error = "Kea control socket path is empty or too long";
    close(descriptor);
    return std::nullopt;
  }
  std::memcpy(address.sun_path, socket_path.data(), socket_path.size());
  address.sun_path[socket_path.size()] = '\0';
  if (connect(descriptor, reinterpret_cast<const sockaddr*>(&address),
              sizeof(address)) != 0) {
    if (error) *error = "cannot connect to " + std::string(socket_path) + ": " +
        std::strerror(errno);
    close(descriptor);
    return std::nullopt;
  }
  nlohmann::json request_object{{"command", command}};
  if (!arguments.is_null()) request_object["arguments"] = arguments;
  const std::string request = request_object.dump();
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

std::optional<std::string> TranslateOperationalState(
    std::string_view module_name, const nlohmann::json& leases,
    const nlohmann::json& statistics, std::string* error) {
  const bool dhcp6 = module_name == "kea-dhcp6-server";
  if (!dhcp6 && module_name != "kea-dhcp4-server") {
    if (error) *error = "unsupported Kea module";
    return std::nullopt;
  }
  auto lease_xml = BuildLeases(leases, dhcp6, error);
  if (!lease_xml) return std::nullopt;
  auto statistic_xml = BuildStatistics(statistics, dhcp6, error);
  if (!statistic_xml) return std::nullopt;
  const std::string ns = "urn:ietf:params:xml:ns:yang:" +
      std::string(module_name);
  return "<state xmlns=\"" + ns + "\">" + *lease_xml + *statistic_xml +
      "</state>";
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

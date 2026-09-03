// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "plugins/system/src/system_config.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <string_view>

#include <libxml/parser.h>
#include <libxml/tree.h>
#if defined(__FreeBSD__)
#include <unistd.h>
#else
#include <crypt.h>
#endif

namespace dang::system {
namespace {

constexpr std::string_view kNamespace =
    "urn:ietf:params:xml:ns:yang:ietf-system";

bool Is(xmlNodePtr node, std::string_view name) {
  return node && node->type == XML_ELEMENT_NODE && node->ns &&
         node->ns->href && name == reinterpret_cast<const char*>(node->name) &&
         kNamespace == reinterpret_cast<const char*>(node->ns->href);
}

xmlNodePtr Child(xmlNodePtr parent, std::string_view name) {
  for (xmlNodePtr node = parent ? parent->children : nullptr; node;
       node = node->next)
    if (Is(node, name)) return node;
  return nullptr;
}

std::vector<xmlNodePtr> Children(xmlNodePtr parent, std::string_view name) {
  std::vector<xmlNodePtr> result;
  for (xmlNodePtr node = parent ? parent->children : nullptr; node;
       node = node->next)
    if (Is(node, name)) result.push_back(node);
  return result;
}

std::optional<std::string> Text(xmlNodePtr node) {
  if (!node) return std::nullopt;
  xmlChar* value = xmlNodeGetContent(node);
  if (!value) return std::nullopt;
  std::string result(reinterpret_cast<const char*>(value));
  xmlFree(value);
  return result;
}

template <typename Integer>
bool IntegerValue(xmlNodePtr node, Integer* value) {
  const auto text = Text(node);
  if (!text) return false;
  const char* begin = text->data();
  const char* end = begin + text->size();
  const auto parsed = std::from_chars(begin, end, *value);
  return parsed.ec == std::errc() && parsed.ptr == end;
}

bool BooleanValue(xmlNodePtr node, bool default_value) {
  const auto value = Text(node);
  if (!value) return default_value;
  return *value == "true" || *value == "1";
}

xmlNodePtr FindSystem(xmlDocPtr document) {
  xmlNodePtr root = xmlDocGetRootElement(document);
  if (Is(root, "system")) return root;
  for (xmlNodePtr node = root ? root->children : nullptr; node;
       node = node->next)
    if (Is(node, "system")) return node;
  return nullptr;
}

bool Fail(std::string message, std::string path, std::string* error,
          std::string* error_path) {
  *error = std::move(message);
  *error_path = std::move(path);
  return false;
}

}  // namespace

bool ParseConfig(const char* xml, Config* config, std::string* error,
                 std::string* error_path) {
  if (!xml || !config || !error || !error_path)
    return false;
  xmlDocPtr raw = xmlReadMemory(xml, static_cast<int>(std::strlen(xml)),
                                "datastore.xml", nullptr,
                                XML_PARSE_NONET | XML_PARSE_NOBLANKS |
                                    XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
  std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> document(raw, xmlFreeDoc);
  if (!document)
    return Fail("cannot parse complete datastore XML", "/ietf-system:system",
                error, error_path);
  *config = Config{};
  xmlNodePtr system = FindSystem(document.get());
  if (!system) return true;
  config->contact = Text(Child(system, "contact"));
  config->hostname = Text(Child(system, "hostname"));
  config->location = Text(Child(system, "location"));
  xmlNodePtr clock = Child(system, "clock");
  config->timezone_name = Text(Child(clock, "timezone-name"));
  if (xmlNodePtr offset = Child(clock, "timezone-utc-offset")) {
    int parsed = 0;
    if (!IntegerValue(offset, &parsed))
      return Fail("invalid timezone UTC offset",
                  "/ietf-system:system/clock/timezone-utc-offset", error,
                  error_path);
    config->timezone_offset_minutes = parsed;
  }

  if (xmlNodePtr ntp = Child(system, "ntp")) {
    config->ntp_present = true;
    config->ntp_enabled = BooleanValue(Child(ntp, "enabled"), true);
    std::set<std::string> names;
    for (xmlNodePtr server : Children(ntp, "server")) {
      NtpServer parsed;
      parsed.name = Text(Child(server, "name")).value_or("");
      xmlNodePtr udp = Child(server, "udp");
      parsed.address = Text(Child(udp, "address")).value_or("");
      if (xmlNodePtr port = Child(udp, "port")) {
        if (!IntegerValue(port, &parsed.port))
          return Fail("invalid NTP UDP port",
                      "/ietf-system:system/ntp/server/udp/port", error,
                      error_path);
      }
      parsed.association =
          Text(Child(server, "association-type")).value_or("server");
      parsed.iburst = BooleanValue(Child(server, "iburst"), false);
      parsed.prefer = BooleanValue(Child(server, "prefer"), false);
      if (parsed.name.empty() || parsed.address.empty() ||
          !names.insert(parsed.name).second)
        return Fail("invalid or duplicate NTP server",
                    "/ietf-system:system/ntp/server", error, error_path);
      config->ntp_servers.push_back(std::move(parsed));
    }
  }

  if (xmlNodePtr dns = Child(system, "dns-resolver")) {
    config->dns_present = true;
    for (xmlNodePtr search : Children(dns, "search"))
      if (const auto value = Text(search)) config->dns_search.push_back(*value);
    std::set<std::string> names;
    for (xmlNodePtr server : Children(dns, "server")) {
      DnsServer parsed;
      parsed.name = Text(Child(server, "name")).value_or("");
      xmlNodePtr transport = Child(server, "udp-and-tcp");
      parsed.address = Text(Child(transport, "address")).value_or("");
      if (xmlNodePtr port = Child(transport, "port")) {
        if (!IntegerValue(port, &parsed.port))
          return Fail("invalid DNS port",
                      "/ietf-system:system/dns-resolver/server/udp-and-tcp/port",
                      error, error_path);
      }
      if (parsed.name.empty() || parsed.address.empty() ||
          !names.insert(parsed.name).second)
        return Fail("invalid or duplicate DNS server",
                    "/ietf-system:system/dns-resolver/server", error,
                    error_path);
      config->dns_servers.push_back(std::move(parsed));
    }
    xmlNodePtr options = Child(dns, "options");
    if (xmlNodePtr timeout = Child(options, "timeout"))
      (void)IntegerValue(timeout, &config->dns_timeout);
    if (xmlNodePtr attempts = Child(options, "attempts"))
      (void)IntegerValue(attempts, &config->dns_attempts);
  }

  if (xmlNodePtr authentication = Child(system, "authentication")) {
    for (xmlNodePtr order : Children(authentication,
                                     "user-authentication-order")) {
      const auto value = Text(order);
      if (value && (*value == "local-users" ||
                    value->ends_with(":local-users")))
        config->local_password_authentication = true;
    }
    std::set<std::string> names;
    for (xmlNodePtr user : Children(authentication, "user")) {
      User parsed;
      parsed.name = Text(Child(user, "name")).value_or("");
      parsed.password_hash = Text(Child(user, "password"));
      if (parsed.name.empty() || !names.insert(parsed.name).second)
        return Fail("invalid or duplicate local user",
                    "/ietf-system:system/authentication/user", error,
                    error_path);
      if (parsed.password_hash && parsed.password_hash->starts_with("$0$"))
        return Fail("cleartext $0$ passwords are not accepted; configure a "
                    "SHA-256 or SHA-512 crypt hash",
                    "/ietf-system:system/authentication/user/password", error,
                    error_path);
      for (xmlNodePtr key : Children(user, "authorized-key")) {
        AuthorizedKey item;
        item.name = Text(Child(key, "name")).value_or("");
        item.algorithm = Text(Child(key, "algorithm")).value_or("");
        item.key_data = Text(Child(key, "key-data")).value_or("");
        parsed.authorized_keys.push_back(std::move(item));
      }
      config->users.push_back(std::move(parsed));
    }
  }
  return true;
}

bool VerifyPassword(std::string_view password, std::string_view stored_hash) {
  if (password.size() > 4096 || stored_hash.size() > 4096 ||
      !(stored_hash.starts_with("$5$") || stored_hash.starts_with("$6$")))
    return false;
  const std::string secret(password);
  const std::string hash(stored_hash);
  const char* calculated = nullptr;
#if defined(__FreeBSD__)
  static std::mutex crypt_mutex;
  std::lock_guard lock(crypt_mutex);
  calculated = crypt(secret.c_str(), hash.c_str());
#else
  crypt_data data{};
  calculated = crypt_r(secret.c_str(), hash.c_str(), &data);
#endif
  if (!calculated) return false;
  const std::size_t actual = std::strlen(calculated);
  if (actual != hash.size()) return false;
  unsigned difference = 0;
  for (std::size_t index = 0; index < actual; ++index)
    difference |= static_cast<unsigned>(calculated[index] ^ hash[index]);
  return difference == 0;
}

}  // namespace dang::system

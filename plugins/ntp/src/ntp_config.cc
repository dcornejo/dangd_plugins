// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/**
 * @file
 * Side-effect-free RFC 9249 configuration projection and change planning.
 * Native daemon syntax, files, sockets, and service lifecycle deliberately do
 * not appear here; Linux and FreeBSD adapters consume the resulting values.
 */

#include "plugins/ntp/src/ntp_config.h"

#include <arpa/inet.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <charconv>
#include <climits>
#include <cstring>
#include <memory>
#include <set>
#include <string_view>

namespace dang::ntp {
namespace {

constexpr std::string_view kNtpNamespace =
    "urn:ietf:params:xml:ns:yang:ietf-ntp";
constexpr std::string_view kSystemNamespace =
    "urn:ietf:params:xml:ns:yang:ietf-system";

bool Is(xmlNodePtr node, std::string_view name, std::string_view name_space) {
  return node && node->type == XML_ELEMENT_NODE && node->ns && node->ns->href &&
         name == reinterpret_cast<const char*>(node->name) &&
         name_space == reinterpret_cast<const char*>(node->ns->href);
}

xmlNodePtr Child(xmlNodePtr parent, std::string_view name) {
  for (xmlNodePtr node = parent ? parent->children : nullptr; node;
       node = node->next)
    if (Is(node, name, kNtpNamespace)) return node;
  return nullptr;
}

std::vector<xmlNodePtr> Children(xmlNodePtr parent, std::string_view name) {
  std::vector<xmlNodePtr> result;
  for (xmlNodePtr node = parent ? parent->children : nullptr; node;
       node = node->next)
    if (Is(node, name, kNtpNamespace)) result.push_back(node);
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

bool Fail(std::string message, std::string path, std::string* error,
          std::string* error_path) {
  *error = std::move(message);
  *error_path = std::move(path);
  return false;
}

template <typename Integer>
bool IntegerValue(xmlNodePtr node, Integer minimum, Integer maximum,
                  Integer* value) {
  const auto text = Text(node);
  if (!text) return false;
  Integer parsed{};
  const auto result =
      std::from_chars(text->data(), text->data() + text->size(), parsed);
  if (result.ec != std::errc() || result.ptr != text->data() + text->size() ||
      parsed < minimum || parsed > maximum)
    return false;
  *value = parsed;
  return true;
}

bool BooleanValue(xmlNodePtr node, bool default_value, bool* value) {
  if (!node) {
    *value = default_value;
    return true;
  }
  const auto text = Text(node);
  if (!text) return false;
  if (*text == "true" || *text == "1") {
    *value = true;
    return true;
  }
  if (*text == "false" || *text == "0") {
    *value = false;
    return true;
  }
  return false;
}

std::string LocalIdentity(std::string value) {
  if (const std::size_t separator = value.find(':');
      separator != std::string::npos)
    value.erase(0, separator + 1);
  return value;
}

bool IpAddress(std::string_view value) {
  in_addr ipv4{};
  in6_addr ipv6{};
  const std::string owned(value);
  return inet_pton(AF_INET, owned.c_str(), &ipv4) == 1 ||
         inet_pton(AF_INET6, owned.c_str(), &ipv6) == 1;
}

bool KnownChildren(xmlNodePtr parent, const std::set<std::string>& known,
                   std::string_view path, std::string* error,
                   std::string* error_path) {
  for (xmlNodePtr node = parent ? parent->children : nullptr; node;
       node = node->next) {
    if (node->type != XML_ELEMENT_NODE) continue;
    const std::string name(reinterpret_cast<const char*>(node->name));
    const bool ntp_node =
        node->ns && node->ns->href &&
        kNtpNamespace == reinterpret_cast<const char*>(node->ns->href);
    if (!ntp_node || !known.contains(name))
      return Fail("unsupported configured or augmented RFC 9249 content",
                  std::string(path) + "/" + name, error, error_path);
  }
  return true;
}

xmlNodePtr FindTopLevel(xmlDocPtr document, std::string_view name,
                        std::string_view name_space) {
  xmlNodePtr root = xmlDocGetRootElement(document);
  if (Is(root, name, name_space)) return root;
  for (xmlNodePtr node = root ? root->children : nullptr; node;
       node = node->next)
    if (Is(node, name, name_space)) return node;
  return nullptr;
}

bool ParseCommon(xmlNodePtr parent, std::string_view path,
                 CommonAttributes* common, std::string* error,
                 std::string* error_path) {
  if (xmlNodePtr value = Child(parent, "minpoll")) {
    int parsed = 0;
    if (!IntegerValue(value, -128, 127, &parsed))
      return Fail("invalid minimum poll interval",
                  std::string(path) + "/minpoll", error, error_path);
    common->minpoll = parsed;
  }
  if (xmlNodePtr value = Child(parent, "maxpoll")) {
    int parsed = 0;
    if (!IntegerValue(value, -128, 127, &parsed))
      return Fail("invalid maximum poll interval",
                  std::string(path) + "/maxpoll", error, error_path);
    common->maxpoll = parsed;
  }
  if (common->minpoll > common->maxpoll)
    return Fail("minimum poll interval exceeds maximum poll interval",
                std::string(path) + "/minpoll", error, error_path);
  if (xmlNodePtr value = Child(parent, "port")) {
    unsigned parsed = 0;
    if (!IntegerValue(value, 1U, 65535U, &parsed) ||
        (parsed != 123U && parsed < 1024U))
      return Fail("NTP port must be 123 or in the range 1024..65535",
                  std::string(path) + "/port", error, error_path);
    common->port = static_cast<std::uint16_t>(parsed);
  }
  if (xmlNodePtr value = Child(parent, "version")) {
    unsigned parsed = 0;
    if (!IntegerValue(value, 3U, 255U, &parsed))
      return Fail("NTP version must be at least 3",
                  std::string(path) + "/version", error, error_path);
    common->version = parsed;
  }
  if (xmlNodePtr authentication = Child(parent, "authentication")) {
    if (xmlNodePtr key = Child(authentication, "keyid")) {
      std::uint32_t parsed = 0;
      if (!IntegerValue(key, std::uint32_t{1}, UINT32_MAX, &parsed))
        return Fail("invalid authentication key identifier",
                    std::string(path) + "/authentication/keyid", error,
                    error_path);
      common->key_id = parsed;
    }
  }
  return true;
}

std::string UnicastKey(const UnicastAssociation& association) {
  return association.address + "|" + association.type;
}

template <typename Value, typename Key>
const Value* FindBy(const std::vector<Value>& values, const std::string& key,
                    Key key_of) {
  const auto found =
      std::find_if(values.begin(), values.end(),
                   [&](const Value& value) { return key_of(value) == key; });
  return found == values.end() ? nullptr : &*found;
}

}  // namespace

bool ParseConfig(const char* xml, Config* config, std::string* error,
                 std::string* error_path) {
  if (!xml || !config || !error || !error_path) return false;
  const std::size_t length = std::strlen(xml);
  if (length > static_cast<std::size_t>(INT_MAX))
    return Fail("datastore XML is too large", "/ietf-ntp:ntp", error,
                error_path);
  xmlDocPtr raw =
      xmlReadMemory(xml, static_cast<int>(length), "datastore.xml", nullptr,
                    XML_PARSE_NONET | XML_PARSE_NOBLANKS | XML_PARSE_NOERROR |
                        XML_PARSE_NOWARNING);
  std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> document(raw, xmlFreeDoc);
  if (!document)
    return Fail("cannot parse complete datastore XML", "/ietf-ntp:ntp", error,
                error_path);

  *config = Config{};
  xmlNodePtr ntp = FindTopLevel(document.get(), "ntp", kNtpNamespace);
  if (!ntp) return true;
  config->present = true;
  if (!KnownChildren(ntp,
                     {"port", "refclock-master", "authentication",
                      "access-rules", "unicast-configuration", "interfaces"},
                     "/ietf-ntp:ntp", error, error_path))
    return false;
  if (xmlNodePtr system =
          FindTopLevel(document.get(), "system", kSystemNamespace);
      system) {
    for (xmlNodePtr node = system->children; node; node = node->next)
      if (Is(node, "ntp", kSystemNamespace))
        return Fail(
            "RFC 9249 /ntp and RFC 7317 /system/ntp are mutually "
            "exclusive",
            "/ietf-ntp:ntp", error, error_path);
  }

  if (xmlNodePtr port = Child(ntp, "port")) {
    unsigned parsed = 0;
    if (!IntegerValue(port, 1U, 65535U, &parsed) ||
        (parsed != 123U && parsed < 1024U))
      return Fail("NTP port must be 123 or in the range 1024..65535",
                  "/ietf-ntp:ntp/port", error, error_path);
    config->port = static_cast<std::uint16_t>(parsed);
  }
  if (xmlNodePtr refclock = Child(ntp, "refclock-master")) {
    if (!KnownChildren(refclock, {"master-stratum"},
                       "/ietf-ntp:ntp/refclock-master", error, error_path))
      return false;
    unsigned stratum = 16;
    if (xmlNodePtr value = Child(refclock, "master-stratum");
        value && !IntegerValue(value, 1U, 16U, &stratum))
      return Fail("reference-clock stratum must be in the range 1..16",
                  "/ietf-ntp:ntp/refclock-master/master-stratum", error,
                  error_path);
    config->refclock_master_stratum = stratum;
  }

  if (xmlNodePtr authentication = Child(ntp, "authentication")) {
    if (!KnownChildren(authentication, {"auth-enabled", "authentication-keys"},
                       "/ietf-ntp:ntp/authentication", error, error_path))
      return false;
    config->authentication_present = true;
    if (!BooleanValue(Child(authentication, "auth-enabled"), false,
                      &config->authentication_enabled))
      return Fail("invalid authentication enabled value",
                  "/ietf-ntp:ntp/authentication/auth-enabled", error,
                  error_path);
    std::set<std::uint32_t> ids;
    for (xmlNodePtr key : Children(authentication, "authentication-keys")) {
      if (!KnownChildren(key, {"keyid", "algorithm", "key", "istrusted"},
                         "/ietf-ntp:ntp/authentication/authentication-keys",
                         error, error_path))
        return false;
      AuthenticationKey parsed;
      if (!IntegerValue(Child(key, "keyid"), std::uint32_t{1}, UINT32_MAX,
                        &parsed.id) ||
          !ids.insert(parsed.id).second)
        return Fail("invalid or duplicate authentication key identifier",
                    "/ietf-ntp:ntp/authentication/authentication-keys/keyid",
                    error, error_path);
      parsed.algorithm =
          LocalIdentity(Text(Child(key, "algorithm")).value_or(""));
      static const std::set<std::string> algorithms{
          "hmac-sha-256", "hmac-sha-384", "hmac-sha-512", "aes-cmac"};
      if (!algorithms.contains(parsed.algorithm))
        return Fail(
            "unsupported or missing authentication algorithm",
            "/ietf-ntp:ntp/authentication/authentication-keys/algorithm", error,
            error_path);
      xmlNodePtr material = Child(key, "key");
      if (!KnownChildren(material, {"keystring", "hexadecimal-string"},
                         "/ietf-ntp:ntp/authentication/authentication-keys/key",
                         error, error_path))
        return false;
      if (Child(material, "keystring"))
        return Fail("deprecated cleartext keystring is not supported",
                    "/ietf-ntp:ntp/authentication/authentication-keys/key/"
                    "keystring",
                    error, error_path);
      parsed.hexadecimal = Text(Child(material, "hexadecimal-string"));
      if (!parsed.hexadecimal || parsed.hexadecimal->empty())
        return Fail("hexadecimal authentication key material is required",
                    "/ietf-ntp:ntp/authentication/authentication-keys/key/"
                    "hexadecimal-string",
                    error, error_path);
      if (!BooleanValue(Child(key, "istrusted"), false, &parsed.trusted))
        return Fail(
            "invalid trusted-key value",
            "/ietf-ntp:ntp/authentication/authentication-keys/istrusted", error,
            error_path);
      config->authentication_keys.push_back(std::move(parsed));
    }
  }

  if (xmlNodePtr rules = Child(ntp, "access-rules")) {
    if (!KnownChildren(rules, {"access-rule"}, "/ietf-ntp:ntp/access-rules",
                       error, error_path))
      return false;
    std::set<std::string> modes;
    for (xmlNodePtr rule : Children(rules, "access-rule")) {
      if (!KnownChildren(rule, {"access-mode", "acl"},
                         "/ietf-ntp:ntp/access-rules/access-rule", error,
                         error_path))
        return false;
      AccessRule parsed;
      parsed.mode =
          LocalIdentity(Text(Child(rule, "access-mode")).value_or(""));
      parsed.acl = Text(Child(rule, "acl")).value_or("");
      static const std::set<std::string> supported_modes{
          "peer-access-mode", "server-access-mode", "server-only-access-mode",
          "query-only-access-mode"};
      if (!supported_modes.contains(parsed.mode) || parsed.acl.empty() ||
          !modes.insert(parsed.mode).second)
        return Fail("invalid or duplicate NTP access rule",
                    "/ietf-ntp:ntp/access-rules/access-rule", error,
                    error_path);
      config->access_rules.push_back(std::move(parsed));
    }
  }

  std::set<std::string> associations;
  for (xmlNodePtr association : Children(ntp, "unicast-configuration")) {
    if (!KnownChildren(
            association,
            {"address", "type", "authentication", "prefer", "burst", "iburst",
             "source", "minpoll", "maxpoll", "port", "version"},
            "/ietf-ntp:ntp/unicast-configuration", error, error_path))
      return false;
    UnicastAssociation parsed;
    parsed.address = Text(Child(association, "address")).value_or("");
    parsed.type = LocalIdentity(Text(Child(association, "type")).value_or(""));
    if (!IpAddress(parsed.address) ||
        (parsed.type != "uc-server" && parsed.type != "uc-peer") ||
        !associations.insert(UnicastKey(parsed)).second)
      return Fail("invalid or duplicate unicast association",
                  "/ietf-ntp:ntp/unicast-configuration", error, error_path);
    if (!BooleanValue(Child(association, "prefer"), false, &parsed.prefer) ||
        !BooleanValue(Child(association, "burst"), false, &parsed.burst) ||
        !BooleanValue(Child(association, "iburst"), false, &parsed.iburst))
      return Fail("invalid unicast boolean value",
                  "/ietf-ntp:ntp/unicast-configuration", error, error_path);
    parsed.source_interface = Text(Child(association, "source"));
    if (xmlNodePtr association_authentication =
            Child(association, "authentication");
        association_authentication &&
        !KnownChildren(association_authentication, {"keyid"},
                       "/ietf-ntp:ntp/unicast-configuration/authentication",
                       error, error_path))
      return false;
    if (!ParseCommon(association, "/ietf-ntp:ntp/unicast-configuration",
                     &parsed.common, error, error_path))
      return false;
    if (parsed.common.key_id &&
        std::none_of(config->authentication_keys.begin(),
                     config->authentication_keys.end(), [&](const auto& key) {
                       return key.id == *parsed.common.key_id;
                     }))
      return Fail("unicast association references an unknown key identifier",
                  "/ietf-ntp:ntp/unicast-configuration/authentication/keyid",
                  error, error_path);
    config->unicast.push_back(std::move(parsed));
  }

  if (Child(ntp, "interfaces"))
    return Fail(
        "interface broadcast, multicast, and manycast modes are not "
        "implemented yet",
        "/ietf-ntp:ntp/interfaces", error, error_path);
  std::ranges::sort(config->authentication_keys, {}, &AuthenticationKey::id);
  std::ranges::sort(config->access_rules, {}, &AccessRule::mode);
  std::ranges::sort(config->unicast, {}, UnicastKey);
  return true;
}

std::vector<PlannedAction> PlanChanges(const Config& before,
                                       const Config& proposed) {
  std::vector<PlannedAction> actions;
  if (before == proposed) return actions;

  std::vector<std::string> shutdown_dependency;
  if (before.present) {
    const std::string id =
        proposed.present ? "stop-ntp-for-reconfiguration" : "disable-ntp";
    const std::string description =
        proposed.present ? "Stop the NTP service before reconfiguration"
                         : "Disable the NTP service";
    actions.push_back({id, description, {}});
    shutdown_dependency.push_back(id);
  }
  std::vector<std::string> removals;
  for (std::size_t index = 0; index < before.unicast.size(); ++index) {
    const auto& old = before.unicast[index];
    const std::string key = UnicastKey(old);
    const auto* replacement = FindBy(proposed.unicast, key, UnicastKey);
    if (replacement && *replacement == old) continue;
    const std::string id = "remove-unicast-" + std::to_string(index);
    actions.push_back({id, "Remove " + old.type + " association " + old.address,
                       shutdown_dependency});
    removals.push_back(id);
  }

  std::vector<std::string> prerequisites;
  std::vector<std::string> activation_dependencies = removals;
  if (before.authentication_present != proposed.authentication_present ||
      before.authentication_enabled != proposed.authentication_enabled ||
      before.authentication_keys != proposed.authentication_keys) {
    actions.push_back({"configure-authentication",
                       "Replace NTP authentication policy and key set "
                       "without logging key material",
                       removals.empty() ? shutdown_dependency : removals});
    prerequisites.push_back("configure-authentication");
    activation_dependencies.push_back("configure-authentication");
  }
  if (before.access_rules != proposed.access_rules) {
    actions.push_back({"configure-access-rules",
                       "Replace NTP access-rule bindings",
                       shutdown_dependency});
    prerequisites.push_back("configure-access-rules");
    activation_dependencies.push_back("configure-access-rules");
  }
  if (before.port != proposed.port) {
    actions.push_back({"configure-listener-port",
                       "Change the NTP listener port from " +
                           std::to_string(before.port) + " to " +
                           std::to_string(proposed.port),
                       removals.empty() ? shutdown_dependency : removals});
    prerequisites.push_back("configure-listener-port");
    activation_dependencies.push_back("configure-listener-port");
  }
  if (before.refclock_master_stratum != proposed.refclock_master_stratum) {
    actions.push_back(
        {"configure-reference-clock",
         proposed.refclock_master_stratum
             ? "Enable the local reference clock at stratum " +
                   std::to_string(*proposed.refclock_master_stratum)
             : "Disable the local reference clock",
         shutdown_dependency});
    activation_dependencies.push_back("configure-reference-clock");
  }

  for (std::size_t index = 0; index < proposed.unicast.size(); ++index) {
    const auto& current = proposed.unicast[index];
    const std::string key = UnicastKey(current);
    const auto* old = FindBy(before.unicast, key, UnicastKey);
    if (old && *old == current) continue;
    const std::string id = "configure-unicast-" + std::to_string(index);
    actions.push_back({id,
                       std::string(old ? "Replace " : "Add ") + current.type +
                           " association " + current.address,
                       prerequisites});
    activation_dependencies.push_back(id);
  }

  if (proposed.present) {
    const std::string id =
        before.present ? "restart-ntp-after-reconfiguration" : "enable-ntp";
    const std::string description =
        before.present ? "Restart the NTP service after reconfiguration"
                       : "Enable the NTP service";
    actions.push_back({id, description, activation_dependencies});
  }
  return actions;
}

}  // namespace dang::ntp

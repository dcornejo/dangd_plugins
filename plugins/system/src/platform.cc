// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "plugins/system/src/platform.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace dang::system {
namespace {

std::filesystem::path Root() {
  const char* value = std::getenv("DANG_SYSTEM_ROOT");
  return value && *value ? value : "/";
}

std::filesystem::path Below(const std::filesystem::path& root,
                            const std::filesystem::path& absolute) {
  return root == "/" ? absolute : root / absolute.relative_path();
}

std::string EscapeXml(std::string_view value) {
  std::string result;
  for (const char byte : value) {
    if (byte == '&') result += "&amp;";
    else if (byte == '<') result += "&lt;";
    else if (byte == '>') result += "&gt;";
    else if (byte == '\"') result += "&quot;";
    else result += byte;
  }
  return result;
}

std::string FormatTime(std::chrono::system_clock::time_point value) {
  const std::time_t raw = std::chrono::system_clock::to_time_t(value);
  std::tm utc{};
  gmtime_r(&raw, &utc);
  std::ostringstream output;
  output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  return output.str();
}

std::string Resolver(const Config& config) {
  std::ostringstream output;
  output << "# Managed by dangd ietf-system plugin\n";
  if (!config.dns_search.empty()) {
    output << "search";
    for (const auto& domain : config.dns_search) output << ' ' << domain;
    output << '\n';
  }
  for (const auto& server : config.dns_servers)
    output << "nameserver " << server.address << '\n';
  output << "options timeout:" << config.dns_timeout
         << " attempts:" << config.dns_attempts << '\n';
  return output.str();
}

std::string Ntp(const Config& config) {
  std::ostringstream output;
  output << "# Managed by dangd ietf-system plugin\n";
  if (!config.ntp_present || !config.ntp_enabled) return output.str();
  for (const auto& server : config.ntp_servers) {
    output << server.association << ' ' << server.address;
    if (server.port != 123) output << " port " << server.port;
    if (server.iburst) output << " iburst";
    if (server.prefer) output << " prefer";
    output << '\n';
  }
  return output.str();
}

std::string PersistentHostname(const PlatformLayout& layout,
                               std::string_view hostname) {
  if (layout.hostname_configuration == "/etc/hostname")
    return std::string(hostname) + "\n";
  return "hostname=\"" + std::string(hostname) + "\"\n";
}

void AppendBigEndian(std::string* output, std::int32_t value) {
  const auto bits = static_cast<std::uint32_t>(value);
  for (int shift : {24, 16, 8, 0})
    output->push_back(static_cast<char>((bits >> shift) & 0xffU));
}

std::string FixedOffsetTimezone(int minutes) {
  std::string output("TZif", 4);
  output.push_back('\0');
  output.append(15, '\0');
  for (int count : {0, 0, 0, 0, 1, 4}) AppendBigEndian(&output, count);
  AppendBigEndian(&output, minutes * 60);
  output.push_back('\0');
  output.push_back('\0');
  output.append("UTC", 3);
  output.push_back('\0');
  return output;
}

bool Snapshot(const std::filesystem::path& path, FileSnapshot* snapshot,
              std::string* error) {
  snapshot->path = path;
  std::error_code filesystem_error;
  if (std::filesystem::is_symlink(path, filesystem_error)) {
    snapshot->existed = true;
    snapshot->symbolic_link = true;
    snapshot->link_target = std::filesystem::read_symlink(path, filesystem_error);
    if (!filesystem_error) return true;
    *error = "cannot inspect symbolic link " + path.string();
    return false;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    if (!std::filesystem::exists(path)) return true;
    *error = "cannot read " + path.string();
    return false;
  }
  snapshot->existed = true;
  snapshot->contents.assign(std::istreambuf_iterator<char>(input), {});
  return true;
}

bool WriteAtomic(const std::filesystem::path& path, std::string_view contents,
                 std::string* error) {
  std::error_code filesystem_error;
  std::filesystem::create_directories(path.parent_path(), filesystem_error);
  if (filesystem_error) {
    *error = "cannot create directory for " + path.string() + ": " +
             filesystem_error.message();
    return false;
  }
  const std::filesystem::path temporary = path.string() + ".dangd.tmp";
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output || !(output << contents) || !output.flush()) {
      *error = "cannot write " + temporary.string();
      return false;
    }
  }
  std::filesystem::rename(temporary, path, filesystem_error);
  if (filesystem_error) {
    std::filesystem::remove(temporary);
    *error = "cannot replace " + path.string() + ": " +
             filesystem_error.message();
    return false;
  }
  return true;
}

bool Restore(const FileSnapshot& snapshot, std::string* error) {
  if (snapshot.symbolic_link) {
    std::error_code filesystem_error;
    std::filesystem::remove(snapshot.path, filesystem_error);
    filesystem_error.clear();
    std::filesystem::create_symlink(snapshot.link_target, snapshot.path,
                                    filesystem_error);
    if (!filesystem_error) return true;
    *error = "cannot restore symbolic link " + snapshot.path.string() + ": " +
             filesystem_error.message();
    return false;
  }
  if (snapshot.existed) return WriteAtomic(snapshot.path, snapshot.contents, error);
  std::error_code filesystem_error;
  std::filesystem::remove(snapshot.path, filesystem_error);
  if (filesystem_error) {
    *error = "cannot remove " + snapshot.path.string() + ": " +
             filesystem_error.message();
    return false;
  }
  return true;
}

bool RealRoot(const PreparedPlatform& prepared) { return prepared.root == "/"; }

}  // namespace

bool PreparePlatform(const Config& before, const Config& proposed,
                     PreparedPlatform* prepared, std::string* error,
                     std::string* path) {
  prepared->before = before;
  prepared->proposed = proposed;
  prepared->root = Root();
  prepared->layout = NativePlatformLayout();
  prepared->files.clear();
  for (const auto& file : {std::filesystem::path("/etc/resolv.conf"),
                           prepared->layout.ntp_configuration,
                           prepared->layout.hostname_configuration,
                           std::filesystem::path("/etc/localtime")}) {
    FileSnapshot snapshot;
    if (!Snapshot(Below(prepared->root, file), &snapshot, error)) {
      *path = "/ietf-system:system";
      return false;
    }
    prepared->files.push_back(std::move(snapshot));
  }
  char hostname[256]{};
  if (gethostname(hostname, sizeof(hostname)) == 0)
    prepared->old_hostname = hostname;
  return true;
}

bool ValidatePlatform(const PreparedPlatform& prepared, std::string* error,
                      std::string* path) {
  if (prepared.proposed.timezone_name) {
    const auto timezone = Below(prepared.root,
        std::filesystem::path("/usr/share/zoneinfo") /
        *prepared.proposed.timezone_name);
    if (!std::filesystem::is_regular_file(timezone)) {
      *error = "timezone is not present in the host TZ database";
      *path = "/ietf-system:system/clock/timezone-name";
      return false;
    }
  }
  for (const DnsServer& server : prepared.proposed.dns_servers) {
    if (server.port != 53) {
      *error = "the native resolver does not support a non-default DNS port";
      *path = "/ietf-system:system/dns-resolver/server/udp-and-tcp/port";
      return false;
    }
  }
  if (RealRoot(prepared) &&
      (prepared.before.dns_present || prepared.proposed.dns_present) &&
      std::filesystem::is_symlink("/etc/resolv.conf")) {
    *error = "refusing to replace a resolver-manager symbolic link; native "
             "systemd-resolved/resolvconf integration is required";
    *path = "/ietf-system:system/dns-resolver";
    return false;
  }
  if (prepared.before.hostname && !prepared.proposed.hostname) {
    *error = "removing hostname is not supported because the native default "
             "hostname is platform policy";
    *path = "/ietf-system:system/hostname";
    return false;
  }
  if ((prepared.before.timezone_name ||
       prepared.before.timezone_offset_minutes) &&
      !prepared.proposed.timezone_name &&
      !prepared.proposed.timezone_offset_minutes) {
    *error = "removing timezone configuration is not supported because the "
             "native default timezone is platform policy";
    *path = "/ietf-system:system/clock";
    return false;
  }
  return true;
}

bool ApplyPlatform(PreparedPlatform* prepared, std::string* error,
                   std::string* path) {
  if ((prepared->before.dns_present || prepared->proposed.dns_present) &&
      !WriteAtomic(Below(prepared->root, "/etc/resolv.conf"),
                   Resolver(prepared->proposed), error)) {
    *path = "/ietf-system:system/dns-resolver";
    return false;
  }
  if ((prepared->before.ntp_present || prepared->proposed.ntp_present) &&
      !WriteAtomic(Below(prepared->root, prepared->layout.ntp_configuration),
                   Ntp(prepared->proposed), error)) {
    *path = "/ietf-system:system/ntp";
    (void)Restore(prepared->files[0], error);
    return false;
  }
  if (prepared->proposed.hostname &&
      !WriteAtomic(Below(prepared->root,
                         prepared->layout.hostname_configuration),
                   PersistentHostname(prepared->layout,
                                      *prepared->proposed.hostname),
                   error)) {
    *path = "/ietf-system:system/hostname";
    (void)RollbackPlatform(prepared, error, path);
    return false;
  }
  if (prepared->proposed.timezone_name ||
      prepared->proposed.timezone_offset_minutes) {
    std::string timezone;
    if (prepared->proposed.timezone_name) {
      std::ifstream input(Below(prepared->root,
          std::filesystem::path("/usr/share/zoneinfo") /
          *prepared->proposed.timezone_name), std::ios::binary);
      timezone.assign(std::istreambuf_iterator<char>(input), {});
    } else {
      timezone = FixedOffsetTimezone(
          *prepared->proposed.timezone_offset_minutes);
    }
    if (timezone.empty() ||
        !WriteAtomic(Below(prepared->root, "/etc/localtime"), timezone,
                     error)) {
      *path = "/ietf-system:system/clock";
      (void)RollbackPlatform(prepared, error, path);
      return false;
    }
  }
  if (prepared->proposed.hostname && RealRoot(*prepared) &&
      sethostname(prepared->proposed.hostname->c_str(),
#if defined(__FreeBSD__)
                  static_cast<int>(prepared->proposed.hostname->size())) != 0) {
#else
                  prepared->proposed.hostname->size()) != 0) {
#endif
    *error = "sethostname failed";
    *path = "/ietf-system:system/hostname";
    (void)RollbackPlatform(prepared, error, path);
    return false;
  }
  prepared->applied = true;
  if (RealRoot(*prepared) &&
      (prepared->before.ntp_present || prepared->proposed.ntp_present)) {
    const std::string& command =
        prepared->proposed.ntp_present && prepared->proposed.ntp_enabled
            ? prepared->layout.ntp_reload_command
            : prepared->layout.ntp_stop_command;
    if (std::system(command.c_str()) != 0) {
      *error = "cannot apply native NTP service state";
      *path = "/ietf-system:system/ntp";
      (void)RollbackPlatform(prepared, error, path);
      return false;
    }
  }
  return true;
}

bool RollbackPlatform(PreparedPlatform* prepared, std::string* error,
                      std::string* path) {
  bool okay = true;
  for (auto snapshot = prepared->files.rbegin(); snapshot != prepared->files.rend();
       ++snapshot) {
    std::string restore_error;
    if (!Restore(*snapshot, &restore_error)) {
      okay = false;
      *error = restore_error;
      *path = "/ietf-system:system";
    }
  }
  if (RealRoot(*prepared) && !prepared->old_hostname.empty() &&
      sethostname(prepared->old_hostname.c_str(),
#if defined(__FreeBSD__)
                  static_cast<int>(prepared->old_hostname.size())) != 0) {
#else
                  prepared->old_hostname.size()) != 0) {
#endif
    okay = false;
    *error = "cannot restore hostname";
    *path = "/ietf-system:system/hostname";
  }
  prepared->applied = false;
  if (RealRoot(*prepared) &&
      (prepared->before.ntp_present || prepared->proposed.ntp_present)) {
    const std::string& command =
        prepared->before.ntp_present && prepared->before.ntp_enabled
            ? prepared->layout.ntp_reload_command
            : prepared->layout.ntp_stop_command;
    if (std::system(command.c_str()) != 0) {
      okay = false;
      *error = "cannot restore native NTP service state";
      *path = "/ietf-system:system/ntp";
    }
  }
  return okay;
}

std::string OperationalStateXml() {
  utsname identity{};
  if (uname(&identity) != 0) return {};
  timespec uptime{};
  clock_gettime(CLOCK_BOOTTIME, &uptime);
  const auto now = std::chrono::system_clock::now();
  const auto boot = now - std::chrono::seconds(uptime.tv_sec);
  std::ostringstream output;
  output << "<system-state xmlns=\"urn:ietf:params:xml:ns:yang:ietf-system\">"
         << "<platform><os-name>" << EscapeXml(identity.sysname)
         << "</os-name><os-release>" << EscapeXml(identity.release)
         << "</os-release><os-version>" << EscapeXml(identity.version)
         << "</os-version><machine>" << EscapeXml(identity.machine)
         << "</machine></platform><clock><current-datetime>"
         << FormatTime(now) << "</current-datetime><boot-datetime>"
         << FormatTime(boot) << "</boot-datetime></clock></system-state>";
  return output.str();
}

bool SetCurrentDatetime(std::string_view xml, std::string* error) {
  xmlDocPtr document = xmlReadMemory(xml.data(), static_cast<int>(xml.size()),
                                     "rpc.xml", nullptr,
                                     XML_PARSE_NONET | XML_PARSE_NOBLANKS);
  if (!document) {
    *error = "invalid set-current-datetime input";
    return false;
  }
  std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> owner(document, xmlFreeDoc);
  xmlNodePtr root = xmlDocGetRootElement(document);
  xmlNodePtr leaf = root;
  if (!root || std::string_view(reinterpret_cast<const char*>(root->name)) !=
                   "current-datetime") {
    leaf = nullptr;
    for (xmlNodePtr node = root ? root->children : nullptr; node;
         node = node->next)
      if (node->type == XML_ELEMENT_NODE &&
          std::string_view(reinterpret_cast<const char*>(node->name)) ==
              "current-datetime")
        leaf = node;
  }
  xmlChar* text = leaf ? xmlNodeGetContent(leaf) : nullptr;
  if (!text) {
    *error = "current-datetime is missing";
    return false;
  }
  std::tm parsed{};
  std::istringstream input(reinterpret_cast<const char*>(text));
  xmlFree(text);
  input >> std::get_time(&parsed, "%Y-%m-%dT%H:%M:%SZ");
  if (input.fail()) {
    *error = "only canonical UTC date-and-time values are currently supported";
    return false;
  }
  const std::time_t seconds = timegm(&parsed);
  const timespec requested{seconds, 0};
  if (clock_settime(CLOCK_REALTIME, &requested) != 0) {
    *error = "clock_settime failed";
    return false;
  }
  return true;
}

bool RequestPowerOperation(bool restart, std::string* error) {
  const char* allowed = std::getenv("DANG_SYSTEM_ALLOW_POWER");
  if (!allowed || std::string_view(allowed) != "1") {
    *error = "power operations are disabled; set DANG_SYSTEM_ALLOW_POWER=1 "
             "for a deliberately privileged deployment";
    return false;
  }
  const PlatformLayout layout = NativePlatformLayout();
  const int status = std::system(
      (restart ? layout.restart_command : layout.shutdown_command).c_str());
  if (status != 0) {
    *error = restart ? "restart command failed" : "shutdown command failed";
    return false;
  }
  return true;
}

}  // namespace dang::system

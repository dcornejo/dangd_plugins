// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANG_PLUGINS_KEA_ADAPTER_H_
#define DANG_PLUGINS_KEA_ADAPTER_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace dang::plugins::kea {

/** One Kea daemon endpoint and the JSON arguments translated for it. */
struct ServerConfiguration {
  std::string module_name;
  std::string service_name;
  std::string socket_path;
  nlohmann::json arguments;
};

/** Resource bounds applied to a complete logical result assembled by pages. */
struct PageLimits {
  std::size_t page_size = 256;
  std::size_t maximum_pages = 512;
  std::size_t maximum_items = 65536;
  std::size_t maximum_bytes = 8U * 1024U * 1024U;
  std::size_t maximum_xml_bytes = 16U * 1024U * 1024U;
  std::chrono::milliseconds maximum_duration{30000};
};

/** Injectable control query used to test page boundaries and failures. */
using ControlQuery = std::function<std::optional<nlohmann::json>(
    std::string_view socket_path, std::string_view command,
    const nlohmann::json& arguments, std::string* error)>;

/** Injectable configuration command used to verify apply compensation. */
using ConfigurationCommand = std::function<bool(
    const ServerConfiguration& server, std::string_view command,
    std::string* error)>;

/**
 * Converts one official Kea configuration container into control JSON.
 * Returns no value when XML is malformed, the requested module is unsupported,
 * or values cannot be represented by Kea's control API.
 */
[[nodiscard]] std::optional<ServerConfiguration> TranslateConfiguration(
    std::string_view datastore_xml, std::string_view module_name,
    std::string_view socket_path, std::string* error);

/** Sends one command over a bounded local Kea UNIX control socket. */
[[nodiscard]] std::optional<nlohmann::json> SendControlCommand(
    const ServerConfiguration& server, std::string_view command,
    std::string* error);

/** Sends a read-only command whose optional arguments are already Kea JSON. */
[[nodiscard]] std::optional<nlohmann::json> SendControlQuery(
    std::string_view socket_path, std::string_view command,
    const nlohmann::json& arguments, std::string* error);

/**
 * Reads one daemon's effective configuration and verifies every managed value.
 * Kea-added defaults and response metadata are ignored. System-ordered YANG
 * lists and leaf-lists are matched by key or value; user-ordered lists and
 * arbitrary embedded JSON arrays retain positional comparison. A missing or
 * changed value from the authoritative dangd image fails reconciliation.
 */
[[nodiscard]] bool VerifyLiveConfiguration(
    const ServerConfiguration& expected, const ControlQuery& query,
    std::string* error);

/**
 * Retrieves a complete lease result using Kea's stable address cursor.
 * Repeated cursors, malformed pages, or configured resource limits fail closed.
 */
[[nodiscard]] std::optional<nlohmann::json> CollectLeasePages(
    std::string_view socket_path, bool dhcp6, const ControlQuery& query,
    std::string* error, const PageLimits& limits = {});

/** Retrieves all host reservations using Kea's source-index/host-id cursor. */
[[nodiscard]] std::optional<nlohmann::json> CollectHostPages(
    std::string_view socket_path, const ControlQuery& query,
    std::string* error, const PageLimits& limits = {});

/** Returns the unique subnet IDs present in one translated configuration. */
[[nodiscard]] std::vector<std::uint32_t> ExtractSubnetIds(
    const ServerConfiguration& server);

/** Queries supplemental statistics once per accepted subnet under total limits. */
[[nodiscard]] std::optional<nlohmann::json> CollectStatistics(
    std::string_view socket_path, bool dhcp6,
    const std::vector<std::uint32_t>& subnet_ids, const ControlQuery& query,
    std::string* error, const PageLimits& limits = {});

/** Converts native lease and supplemental-statistic replies to modeled XML. */
[[nodiscard]] std::optional<std::string> TranslateOperationalState(
    std::string_view module_name, const nlohmann::json& leases,
    const nlohmann::json& statistics, const nlohmann::json& hosts,
    std::string* error);

/**
 * Collects one daemon's modeled state between two authority checks.
 * failure_path identifies the configuration or state subtree responsible for
 * a failure. The closing check prevents publication when Kea's managed
 * configuration changed or became unavailable during the multi-command read.
 * PageLimits bound the aggregate state entries, native bytes, modeled XML,
 * queries, and duration rather than restarting for leases, statistics, and
 * reservations.
 */
[[nodiscard]] std::optional<std::string> CollectAuthoritativeOperationalState(
    const ServerConfiguration& expected, bool dhcp6,
    const std::vector<std::uint32_t>& subnet_ids, const ControlQuery& query,
    std::string* failure_path, std::string* error,
    const PageLimits& limits = {});

/** Extracts Kea's result/text fields and accepts only result code zero. */
[[nodiscard]] bool CommandSucceeded(const nlohmann::json& response,
                                    std::string* reason);

/**
 * Applies paired server images in order and restores every possibly changed
 * server in reverse order after failure, including the ambiguous failed call.
 */
[[nodiscard]] bool ApplyWithCompensation(
    const std::vector<ServerConfiguration>& before,
    const std::vector<ServerConfiguration>& proposed,
    const ConfigurationCommand& command, std::string* failed_module,
    std::string* reason);

/**
 * Restores changed paired server images in reverse order. All restorations are
 * attempted, and failed_module identifies the first failure in execution order.
 */
[[nodiscard]] bool RollbackChanged(
    const std::vector<ServerConfiguration>& before,
    const std::vector<ServerConfiguration>& proposed,
    const ConfigurationCommand& command, std::string* failed_module,
    std::string* reason);

}  // namespace dang::plugins::kea

#endif  // DANG_PLUGINS_KEA_ADAPTER_H_

// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANG_PLUGINS_KEA_ADAPTER_H_
#define DANG_PLUGINS_KEA_ADAPTER_H_

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

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
  std::chrono::milliseconds maximum_duration{30000};
};

/** Injectable control query used to test page boundaries and failures. */
using ControlQuery = std::function<std::optional<nlohmann::json>(
    std::string_view socket_path, std::string_view command,
    const nlohmann::json& arguments, std::string* error)>;

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

/** Converts native lease and supplemental-statistic replies to modeled XML. */
[[nodiscard]] std::optional<std::string> TranslateOperationalState(
    std::string_view module_name, const nlohmann::json& leases,
    const nlohmann::json& statistics, const nlohmann::json& hosts,
    std::string* error);

/** Extracts Kea's result/text fields and accepts only result code zero. */
[[nodiscard]] bool CommandSucceeded(const nlohmann::json& response,
                                    std::string* reason);

}  // namespace dang::plugins::kea

#endif  // DANG_PLUGINS_KEA_ADAPTER_H_

// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANG_PLUGINS_KEA_ADAPTER_H_
#define DANG_PLUGINS_KEA_ADAPTER_H_

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

/** Extracts Kea's result/text fields and accepts only result code zero. */
[[nodiscard]] bool CommandSucceeded(const nlohmann::json& response,
                                    std::string* reason);

}  // namespace dang::plugins::kea

#endif  // DANG_PLUGINS_KEA_ADAPTER_H_

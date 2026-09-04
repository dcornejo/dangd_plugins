// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANG_PLUGINS_FRR_FRR_OPERATIONAL_H_
#define DANG_PLUGINS_FRR_FRR_OPERATIONAL_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dang::plugins::frr {

/** One imported FRR list whose keyed instances may carry zebra augments. */
struct AugmentedList {
  std::string namespace_uri;
  std::string root_name;
  std::string list_name;
  std::vector<std::string> keys;
};

/**
 * Retains list keys and direct frr-zebra augment subtrees while discarding
 * state owned by the imported parent module. Returns an empty string when no
 * instance contains a zebra augment.
 */
std::optional<std::string> ExtractZebraAugments(
    std::string_view xml, const AugmentedList& descriptor,
    std::string* error);

/** Wraps independently retrieved fragments in one NETCONF data document. */
std::string OperationalDocument(const std::vector<std::string>& fragments);

}  // namespace dang::plugins::frr

#endif  // DANG_PLUGINS_FRR_FRR_OPERATIONAL_H_

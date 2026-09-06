// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANG_PLUGINS_VPP_LOOPBACK_PLAN_H_
#define DANG_PLUGINS_VPP_LOOPBACK_PLAN_H_

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dang::vpp {

struct LoopbackConfiguration {
  uint32_t instance = 0;
  bool enabled = false;
};

enum class LoopbackOperationKind { kCreate, kSetAdminState, kDelete };

/** One native VPP operation and the exact inverse needed for compensation. */
struct LoopbackOperation {
  LoopbackOperationKind kind;
  uint32_t instance;
  bool enabled = false;
  std::string instance_path;
};

using LoopbackConfigurationMap = std::map<uint32_t, LoopbackConfiguration>;

/** Extracts the VPP loopback subtree from a complete datastore snapshot. */
[[nodiscard]] bool ParseLoopbackConfiguration(
    const char* xml, LoopbackConfigurationMap* configuration,
    std::string* error, std::string* error_path);

/** Builds forward operations in safe order from two complete snapshots. */
[[nodiscard]] std::vector<LoopbackOperation> PlanLoopbackChanges(
    const LoopbackConfigurationMap& before,
    const LoopbackConfigurationMap& proposed);

}  // namespace dang::vpp

#endif

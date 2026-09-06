// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANG_PLUGINS_VPP_LOOPBACK_TRANSACTION_H_
#define DANG_PLUGINS_VPP_LOOPBACK_TRANSACTION_H_

#include <optional>
#include <string>

#include "plugins/vpp/src/vpp_client.h"

namespace dang::vpp {

/** Retained before-image for one VPP-created loopback transaction. */
class LoopbackTransaction {
 public:
  explicit LoopbackTransaction(VppClient* client, uint32_t instance = 0)
      : client_(client), instance_(instance) {}

  /** Creates the loopback and brings it up, compensating partial failure. */
  [[nodiscard]] bool Apply(std::string* error);

  /** Restores absence by bringing the retained interface down and deleting it. */
  [[nodiscard]] bool Rollback(std::string* error);

  [[nodiscard]] const std::optional<CreatedInterface>& created() const {
    return created_;
  }

 private:
  VppClient* client_;
  uint32_t instance_;
  std::optional<CreatedInterface> created_;
};

}  // namespace dang::vpp

#endif

// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0
#ifndef DANG_PLUGINS_NTP_LINUX_CHRONY_SERVICE_H_
#define DANG_PLUGINS_NTP_LINUX_CHRONY_SERVICE_H_
#include <utility>

#include "plugins/common/systemd_unit.h"
#include "plugins/ntp/src/chrony_transaction.h"
namespace dang::ntp {
/** Observed-state service adapter using native systemd D-Bus calls. */
class SystemdChronyService final : public ChronyService {
 public:
  /** Unit override permits disposable-service native tests. */
  explicit SystemdChronyService(std::string unit = "chrony.service")
      : unit_(std::move(unit)) {}
  bool Active(bool* active, std::string* error) override {
    return native::SystemdUnitActive(unit_, active, error);
  }
  bool SetActive(bool active, std::string* error) override {
    return native::ChangeSystemdUnit(
        unit_,
        active ? native::UnitOperation::kStart : native::UnitOperation::kStop,
        error);
  }

 private:
  std::string unit_;
};
}  // namespace dang::ntp
#endif

// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/** @file Fail-closed development platform selection for non-target hosts. */

#include "plugins/ntp/src/ntp_adapter.h"

namespace dang::ntp {

Platform NativePlatform() { return Platform::kUnsupported; }

}  // namespace dang::ntp

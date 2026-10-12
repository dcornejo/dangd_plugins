// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/** @file Selects base ntpd as the sole FreeBSD RFC 9249 daemon contract. */

#include <memory>
#include <utility>

#include "plugins/ntp/src/ntp_adapter.h"

namespace dang::ntp {

Platform NativePlatform() { return Platform::kFreeBsd; }

std::unique_ptr<NativeAdapter> CreatePlatformAdapter(
    AdapterDescriptor descriptor) {
  return CreateFailClosedAdapter(std::move(descriptor));
}

}  // namespace dang::ntp

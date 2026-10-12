// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0
#ifndef DANG_PLUGINS_NTP_CHRONY_ACTION_ADAPTER_H_
#define DANG_PLUGINS_NTP_CHRONY_ACTION_ADAPTER_H_
#include "plugins/ntp/src/chrony_transaction.h"
#include "plugins/ntp/src/ntp_adapter.h"
namespace dang::ntp {
/** Compose ordered ABI actions with an explicit service and trusted file pair.
 */
std::unique_ptr<NativeAdapter> CreateChronyActionAdapter(
    AdapterDescriptor descriptor, std::unique_ptr<ChronyService> service,
    std::string fragment_path, std::string key_path);
}  // namespace dang::ntp
#endif

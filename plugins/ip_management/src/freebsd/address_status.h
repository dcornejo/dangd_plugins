// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANGD_PLUGINS_IP_MANAGEMENT_FREEBSD_ADDRESS_STATUS_H_
#define DANGD_PLUGINS_IP_MANAGEMENT_FREEBSD_ADDRESS_STATUS_H_

#include <string_view>

#include <net/if.h>
#include <netinet/in.h>
#include <netinet6/in6_var.h>

namespace dangd::ip_management {

/** Map FreeBSD IPv6 per-address flags to the RFC 8344 status enumeration. */
inline std::string_view FreeBsdAddressStatus(int flags) {
  if (flags & IN6_IFF_DUPLICATED) return "duplicate";
  if (flags & IN6_IFF_TENTATIVE) return "tentative";
  if (flags & IN6_IFF_DEPRECATED) return "deprecated";
  if (flags & IN6_IFF_DETACHED) return "inaccessible";
  return "preferred";
}

}  // namespace dangd::ip_management

#endif  // DANGD_PLUGINS_IP_MANAGEMENT_FREEBSD_ADDRESS_STATUS_H_

<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Changelog

All notable changes to the external dangd plugin collection are recorded here.

## [Unreleased]

### Added

- Added native Debian and FreeBSD package generation for the plugin collection,
  YANG models, and PAM module, with explicit dangd dependencies and deployment
  documentation that leaves PAM, SSH, Kea, and daemon activation under
  administrator control.
- Created the independent `dang_plugins` repository.
- Added a buildable ABI-v1 example with an embedded YANG source, retained
  transaction state, validation failure attribution, apply, and rollback.
- Added a loader smoke test that verifies the shared library entry point and
  copied model descriptor.
- Required native Linux and FreeBSD development and validation for every plugin,
  with an explicit prohibition on using host LAN interfaces in tests.
- Added the Kea DHCP plugin with pinned official 3.2.0 DHCPv4/DHCPv6 models,
  bounded UNIX control communication, two-server validation and apply, partial
  failure compensation, rollback, translation tests, and native isolated
  interactions on Ubuntu 26.04 and FreeBSD 16-CURRENT.
- Added the RFC 7317 `ietf-system` plugin with official embedded models,
  transactional Linux and FreeBSD hostname, timezone, NTP, and static DNS
  backends, local-user password verification, platform/clock operational
  state, and guarded system-control RPCs.
- Added `pam_dangd`, a fail-closed PAM password module using a bounded,
  root-peer-only UNIX verification service owned by the supervised plugin.
- Added native Linux and FreeBSD integration tests covering successful and
  failed authentication through PAM, system configuration application,
  operational state, and rollback without using any network interface.

### Changed

- Updated the shared plugin loader smoke test to discover and validate every
  currently supported dangd plugin ABI version rather than ABI v1 only.

<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Changelog

All notable changes to the external dangd plugin collection are recorded here.

## [Unreleased]

### Added

- Added the FRR provider's bounded installed-schema inventory and transitive
  import-closure resolver. It selects one version-consistent Linux or FreeBSD
  YANG directory, rejects malformed, duplicate, oversized, and incomplete
  model sets, and does not yet advertise unsupported runtime functionality.
- Added a header-independent codec for FRR's public native `mgmtd` frontend
  session, datastore lock, XML edit, validate, apply, abort, and unlock wire
  messages, with strict frame and error-reply decoding tests.
- Updated the external plugin discovery smoke test for dangd plugin ABI v7.
  The loader now exercises v7 entry-point precedence and accepts the exclusive
  resource-domain ABI used by the forthcoming FRR provider.
- Added installable operator guides for every provider and for `pam_dangd`,
  covering dependencies, native and source installation, service wiring,
  verification, troubleshooting, safe rollback, and removal on Linux and
  FreeBSD. The RFC 8431 guide clearly identifies its package as schema-only.
- Removed the obsolete system-plugin limitation claiming that initial and
  restored running configuration was not hydrated at daemon startup.
- Staged the unmodified RFC 8431 `ietf-i2rs-rib` revision 2018-09-13 model and
  RFC 8343 `ietf-interfaces` dependency, with pinned checksums, installation,
  and independent libyang schema validation. No runtime implementation is
  advertised by this schema-only checkpoint.
- Install the Kea, system, and RIB plugin guides under distinct names instead
  of silently overwriting them as a shared `README.md`.
- Added the RFC 8431 runtime foundation: strict parsing for the portable
  destination-prefix/base-nexthop subset, deterministic delete-before-install
  replacement planning, attributed rejection of unsupported forwarding
  semantics, and shell-free Linux and FreeBSD command construction.
- Added direct `posix_spawnp` execution with stop-on-failure and reverse-order
  compensation, deterministic rollback-failure coverage, and opt-in native
  mutation tests confined to Linux network namespaces and FreeBSD VNET jails.
  Native validation also corrected and narrowed FreeBSD interface-only route
  handling instead of issuing an invalid `route -ifp` operation.
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

- Required new plugins to prefer programmatic operating-system and daemon APIs
  over command execution, including consideration of netlink on both Linux and
  FreeBSD. Documented, argv-only command fallbacks require failure and rollback
  coverage.
- Split the monolithic native distribution into independent example, Kea,
  RFC 7317 system, PAM, RFC 8431 model, and documentation packages. Debian uses
  native CPack components; FreeBSD receives one generated package configuration
  per component because its CPack generator has no component mode. The PAM
  package alone depends on the system provider that supplies authentication.
- Refreshed collection-wide documentation for native packaging, RFC 7317
  startup hydration, PAM opt-in behavior, and current deployment boundaries.
- Updated the shared plugin loader smoke test to discover and validate every
  currently supported dangd plugin ABI version rather than ABI v1 only.

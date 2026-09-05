<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Changelog

All notable changes to the external dangd plugin collection are recorded here.

## [Unreleased]

### Added

- Added complete YANG import/include closure handling to FRR schema discovery.
  Quoted dependencies, submodule `belongs-to` relationships, and nested RFC
  8525 submodule revisions are validated before a schema set is advertised.
- Documented installation of FRR from its signed Debian/Ubuntu repository,
  including fixed-series selection, required daemon enablement, package-origin
  checks, mgmtd readiness checks, and upgrade precautions.
- Added the loadable ABI-v7 RFC 8431 provider for the portable destination-route
  slice. It publishes pinned models, claims exclusive `routing` ownership,
  validates deltas, and applies or reverses them as one compensated action.
  Plugin lifecycle and native tests pass in Linux namespaces and FreeBSD jails.
- Fixed FRR hardware-coordinator integration by advertising its complete mgmtd
  candidate transaction as one normal action. Commits now schedule the existing
  atomic apply/rollback callbacks instead of treating a zero-action plan as
  already complete; discovery tests lock down the descriptor contract.
- Documented live FRR 10.5.1 RPC probing: active mgmtd and zebra adapters do
  not register `/frr-zebra` in the backend RPC registry, so successful native
  RPC interoperability remains an upstream capability boundary.
- Completed the pinned Kea DHCPv4/DHCPv6 state trees by translating host option
  data and promoting the provider to ABI v5 complete operational publication.
  The ABI-v4 action plan retains Kea's full transaction as one indivisible
  action, and native Linux/FreeBSD tests verify option-data round trips.
- Bounded Kea supplemental statistics by querying each exact subnet ID from
  the last successfully applied configuration and enforcing aggregate query,
  row, byte, and duration limits. Candidate validation cannot leak uncommitted
  subnet state into operational replies, and successful rollback restores the
  retained before-image inventory.
- Added paged DHCPv4/DHCPv6 host-reservation state through Kea's native
  `reservation-get-page` continuation map. Host identifiers and modeled address,
  prefix, class, and context fields are translated under the same page, item,
  byte, and deadline safeguards as leases. Native tests exercise populated
  reservations, and configuration translation now emits Kea's concrete
  identifier keys instead of the YANG key pair.
- Replaced Kea's unbounded all-lease operational queries with native paged
  retrieval. Address cursors are carried across 256-entry pages, while page,
  item, aggregate-byte, and total-duration limits fail closed on malformed,
  stalled, or excessive enumerations. Tests cover page assembly and adversarial
  count and continuation behavior, with live packaged-daemon validation.
- Added ABI-v3 Kea operational state for DHCPv4 and DHCPv6 leases and
  supplemental per-subnet lease statistics. The adapter maps Kea lease types,
  states, lifetimes, prefixes, and binary identifiers into schema-shaped XML,
  bounds control replies and deadlines, and handles empty result sets. Native
  Linux and FreeBSD tests load the required hook libraries and use unique PID
  and socket paths without stopping the host's packaged Kea services.
- Completed configuration-shape translation for the pinned Kea DHCPv4 and
  DHCPv6 models. Singleton list and leaf-list nodes now retain JSON array
  shape; reservations, host/config databases, and hook libraries use Kea's
  native keys; decimal64 values remain numeric; and all modeled JSON-valued
  leaves are decoded. Regression coverage distinguishes scalar `host`,
  `subnet`, and `client-class` leaves from same-named lists, and isolated Kea
  validation exercises DHCPv4/DHCPv6 validate, apply, and rollback.
- Added generic dispatch for all RPCs declared by the installed `frr-zebra`
  schema through FRR's public native mgmtd RPC and RPC-reply messages. The wire
  codec validates format, correlation, framing, and embedded NULs; the session
  and plugin layers preserve dangd schema validation and NACM ownership.
- Added ABI-v8 unsolicited FRR configuration-drift notification. The provider
  publishes an embedded monitoring model and, after reconciliation, a read-only
  background watcher compares managed running roots through mgmtd. Each changed
  path is reported once until a successful commit resets expected state; dangd
  performs schema validation, subscription filtering, and NACM authorization.
- Added opt-in, privileged FRR native mutation tests for Linux and FreeBSD. A
  disposable `dangd-test` mgmtd pathspace receives an empty staticd protocol
  instance, exposes the committed value, and is restored to its exact
  before-image without attaching or changing LAN interfaces.

- Added ABI-v6 applied-state reconciliation for FRR. After a successful commit,
  the provider reads both managed roots back from mgmtd's running datastore and
  replaces only those roots in dangd's complete applied snapshot; absent roots
  are removed and unrelated module data is preserved.
- Added read-triggered detection of FRR running-configuration changes made
  outside dangd. The provider retains reconciled roots, compares subsequent
  mgmtd running reads semantically, and reports drift with the affected model
  path through dangd's operational-provider failure telemetry.
- Added runtime FRR feature discovery from mgmtd's RFC 8525 YANG Library. The
  plugin advertises only daemon-enabled features and rejects installed-file
  versus running-daemon revision or namespace skew during discovery.
- Added live FRR operational-state publication through the native mgmtd
  `GET_DATA`/`TREE_DATA` API. The codec and session layer require correlated,
  complete XML results, reject partial or continued replies, and expose a
  read-only diagnostic mode without changing FRR configuration. Publication is
  initially scoped to the provider-owned `/frr-zebra:zebra` root so broad
  replies cannot collide with core or other plugins' operational trees.
- Added ownership-preserving publication of zebra augments beneath imported
  `frr-interface` and `frr-vrf` lists. The filter retains only list keys and
  `frr-zebra` subtrees, preventing unrelated parent-module state from being
  claimed by the routing provider.
- Added the FRR provider's bounded installed-schema inventory and transitive
  import-closure resolver. It selects one version-consistent Linux or FreeBSD
  YANG directory, rejects malformed, duplicate, oversized, and incomplete
  model sets, and does not yet advertise unsupported runtime functionality.
- Added a header-independent codec for FRR's public native `mgmtd` frontend
  session, datastore lock, XML edit, validate, apply, abort, and unlock wire
  messages, with strict frame and error-reply decoding tests.
- Added a nonblocking, close-on-exec local `mgmtd` transport with bounded
  monotonic deadlines, complete-write/read handling, request/session reply
  correlation, and explicit timeout, disconnect, protocol, and daemon-error
  failures.
- Added a stateful `mgmtd` session layer that requires ordered create, candidate
  lock, XML replacement, validation/apply or abort, unlock, and destruction,
  while validating every operation-specific reply body.
- Added FRR transaction orchestration with a disposable validation session, a
  separately repeated real apply, candidate cleanup on pre-commit failures,
  and true applied-state rollback by validating and committing the retained
  before-image.
- Added the loadable ABI-v7 `dang-frr` provider for the initial native
  `frr-routing`, `frr-zebra`, and `frr-staticd` scope. It publishes the exact
  installed import closure, claims exclusive `routing` ownership, extracts
  atomic replace/delete roots from full dangd snapshots, and connects the
  validated transaction and before-image rollback callbacks.
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

- Lock both FRR candidate and running datastores around validation, apply, and
  abort, as required by mgmtd configuration transactions. Also accept FRR's
  frontend behavior of reporting a successful abort as the generic
  non-validation apply action while retaining correlation and field checks.

- Audited every plugin and PAM source file for maintainability. Added Doxygen
  file summaries and public-contract documentation covering ownership,
  transaction lifetime, rollback and compensation, protocol trust boundaries,
  platform behavior, model translation, and non-obvious security invariants.
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

<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Changelog

All notable changes to the external dangd plugin collection are recorded here.

## [Unreleased]

### Added

- Added a bidirectional cross-host Kea interaction for one Linux and one
  FreeBSD secondary interface on an isolated VLAN. A minimal socket-level
  client sends DHCPv4 INIT-REBOOT and DHCPv6 Rapid Commit requests, then the
  serving host must prove completed native lease allocations before roles are
  reversed. Default-route interfaces fail closed and all test addresses,
  daemons, and memory-only leases are removed afterward.
  Client identifiers are derived from the selected interface's real MAC, Kea
  uses UDP socket mode for IPv4, and both platforms require a client-visible
  DHCPv6 Rapid Commit reply in addition to the server allocation evidence.
  Visible replies are correlated by endpoint and transaction and must carry
  the expected server/client identifiers, IAID, and an address from the exact
  configured pool.

- Extended the isolated native Kea interaction to inject real DHCPv4 and
  DHCPv6 leases through the packaged lease-command hook and require their
  addresses, binary client identities, DHCPv6 IAID, and corresponding assigned
  lease counters in the plugin's complete operational XML.

### Fixed

- Rejected malformed JSON in Kea's JSON-bearing configuration leaves instead
  of silently changing it to a native string. Enforced JSON-object shape for
  modeled user contexts and DHCP queue control while retaining arbitrary valid
  JSON values for hook parameters and HTTP header values.

- Enforced collection shape for singleton Kea list and leaf-list nodes. A
  scalar can no longer masquerade as an unambiguous list entry, and a
  structured object can no longer masquerade as a leaf-list value.

- Rejected duplicate singleton nodes in Kea configuration snapshots. Repeated
  sibling names acquire JSON-array semantics only when the pinned models
  declare a list or leaf-list with the corresponding structural form.

- Rejected XML attributes in selected Kea configuration trees. Edit-style or
  metadata attributes can no longer be silently discarded by the generic
  element-to-JSON converter.

- Rejected mixed XML content in Kea configuration containers. Non-whitespace
  character data alongside child elements can no longer be silently discarded
  while the neighboring elements are translated.

- Bounded each Kea datastore snapshot to 16 MiB before XML parsing. Malformed
  or hostile transaction input can no longer make the plugin allocate an XML
  tree approaching libxml's signed input-length limit.

- Rejected DTD declarations in Kea datastore snapshots before reading any node
  text. Internal entities can no longer expand during XML-to-JSON conversion;
  external subsets are also rejected in addition to the existing network-fetch
  prohibition.

- Required a selected Kea configuration container to be a standalone document
  root or a direct child of a NETCONF `config` or `data` envelope. A matching
  container buried inside an unrelated wrapper can no longer be translated as
  the module's datastore root.

- Reported a missing Kea module configuration container directly instead of
  translating an empty object and misdiagnosing it as a missing control socket.
  Configuration selection now enforces exactly one match in both directions.

- Rejected datastore snapshots containing multiple matching Kea configuration
  containers. Translation no longer silently selects the first module tree and
  ignores a conflicting duplicate.

- Rejected foreign-namespace descendants in Kea configuration containers.
  Elements from another model can no longer acquire Kea control-socket, hook,
  or configuration semantics merely by reusing a recognized local name.

- Rejected embedded NUL bytes in Kea UNIX control-socket paths during both
  configuration translation and direct queries. The plugin's validated socket
  identity can no longer differ from the pathname interpreted by the kernel.

- Contained exceptions from Kea operational control-query implementations for
  lease, reservation, and statistics collection. Transport implementation
  failures now become attributed retrieval errors instead of escaping the
  plugin callback boundary.

- Enforced Kea aggregate operational deadlines after every lease, reservation,
  and statistics control call as well as before it. A slow final reply can no
  longer be accepted after the configured collection budget expires.

- Contained exceptions from Kea configuration command implementations. Apply
  exceptions now enter outcome-unknown reverse compensation, while rollback
  exceptions are reported without preventing restoration of earlier daemons.

- Validated Kea before/proposed transaction pairings before issuing any native
  command. Reordered, identity-mismatched, or duplicate daemon targets now fail
  without side effects instead of risking compensation against the wrong
  before-image.

- Validated UTF-8 and XML 1.0 character ranges for every Kea string emitted as
  operational XML. Native control characters, malformed byte sequences, and
  forbidden code points now fail complete retrieval with the affected field
  identified instead of producing malformed XML.

- Kept Kea control sockets nonblocking through request writes and response
  reads, with retry handling for readiness races. A connected peer that stops
  consuming a large request can no longer hold dangd beyond the exchange's
  shared five-second deadline.

- Contained Kea configuration-conversion exceptions and released the parsed
  XML document on the failure path. Structurally invalid scalar/container
  substitutions now return a plugin error, and snapshots too large for
  libxml's signed input-length interface are rejected before parsing.

- Contained JSON serialization failures for Kea lease, host, and option
  user-context values and for paged lease/host byte accounting. Malformed
  in-memory native values now fail their operational request instead of
  throwing across the plugin boundary.

- Moved Kea UNIX-socket connection establishment under the exchange's single
  five-second deadline using a nonblocking connect and `SO_ERROR` completion
  check. A stalled socket backlog can no longer block dangd before the bounded
  write/read phases begin.

- Contained Kea request-serialization failures such as malformed UTF-8 before
  opening the control socket, preventing JSON exceptions from crossing the
  plugin boundary. Reply-limit accounting now also avoids additive overflow.

- Added a 16 MiB ceiling to serialized Kea control requests before opening a
  socket, matching the existing reply ceiling, and made zero-progress writes
  fail immediately. Oversized configurations and stalled peers can no longer
  consume unbounded transport memory or spin until the deadline.

- Suppressed `SIGPIPE` on Kea control-socket writes using the native Linux/
  FreeBSD send flag or the per-socket BSD fallback. If Kea disconnects during a
  command, dangd now receives a controlled plugin error instead of risking
  process termination. Kea callback fixtures now also construct optional JSON
  replies explicitly for compatibility with the packaged nlohmann JSON 3.11.

- Rejected duplicate Kea reservation option-data keys and duplicate values in
  reservation address, prefix, excluded-prefix, and client-class leaf-lists.
  Complete operational state can no longer contain repeated YANG list or
  leaf-list instances inherited from malformed native replies.

- Enforced YANG scalar types for Kea host reservations and their option data.
  Reservation subnet IDs, addresses, names, boot metadata, option codes,
  strings, and boolean flags now reject wrong types and out-of-range values
  instead of stringifying them into a claimed-complete result.

- Validated the final Kea statistics table independently of its collector.
  Duplicate or non-string column names and negative or oversized `uint32`
  counters now fail complete operational retrieval instead of being ignored or
  emitted as schema-invalid state.

- Enforced the pinned YANG scalar types and integer ranges while translating
  native Kea leases. Wrong-type optional client IDs, flags, hostnames, hardware
  addresses, lifetimes, subnet IDs, IAIDs, and prefix lengths now fail complete
  operational retrieval instead of being omitted or stringified.

- Rejected empty or non-string Kea host-reservation identifiers even when a
  second valid identifier is present, rather than silently omitting malformed
  native state. DHCPv6 reservation state now also rejects the DHCPv4-only
  `circuit-id` and `client-id` types before publishing a complete result.

- Required native Kea hardware addresses, client IDs, and DUIDs to use either
  strict colon-separated hexadecimal octets or a strict contiguous hexadecimal
  string. Empty values and leading, trailing, doubled, or missing separators
  now fail closed instead of being silently normalized to another identity.

- Translated the YANG `database-type` leaf to Kea's native `type` member for
  lease, host, and configuration databases. The isolated transaction fixtures
  now explicitly retain memory-only non-persistent lease databases across
  `config-set`, preventing one native test run from contaminating the next.

- Rejected non-adjacent cursor cycles during Kea lease and reservation paging.
  Every cursor in a logical retrieval is now tracked, so multi-step cycles fail
  immediately instead of consuming the full page or duration budget.

- Replaced narrowing conversions for Kea page counts, host cursors, lease
  states, and DHCPv6 lease types with one checked unsigned decoder. Oversized or
  negative native values now fail validation without throwing across a callback.

- Handled malformed non-string Kea error `text` fields through a shared safe
  fallback across transaction, paging, and operational replies. Native error
  metadata can no longer trigger a JSON type exception at the plugin boundary.

- Rejected oversized native Kea result codes without narrowing them to C++
  `int`. Malformed transaction, paging, and operational replies now produce
  controlled plugin failures instead of allowing numeric conversion exceptions
  to cross the callback boundary.

- Correlated every successful Kea supplemental-statistics reply with the exact
  configured subnet ID that was queried. Missing or duplicate columns, multiple
  rows, and mismatched subnet IDs now fail closed instead of contaminating a
  complete operational result.

- Treated every failed Kea `config-set` as outcome-unknown and reapplied that
  daemon's before-image before compensating earlier daemons. Lost replies can no
  longer leave an unacknowledged configuration active, and restoration failures
  remain attached to the primary transaction error.

- Prevented Kea commits from removing the lease-command, host-command, or
  supplemental-statistics hook libraries required by complete operational
  retrieval. Validation uses portable library basenames across Linux and
  FreeBSD installation directories.

- Prevented Kea commits from removing, changing, or replacing the local UNIX
  control socket used by dangd. Both current and deprecated Kea socket shapes
  are recognized, but the configured management path must remain reachable.

- Rejected duplicate YANG list keys in complete Kea operational publication.
  Repeated lease addresses, per-subnet statistic IDs, or composite reservation
  identities now fail closed instead of producing schema-invalid complete XML.

- Preserved the declared YANG string type when translating pinned Kea
  configuration leaves. Boolean-looking and numeric-looking hostnames, tags,
  identifiers, names, paths, and other string leaves no longer become JSON
  booleans or numbers through lexical guessing.

- Required exactly one Kea answer for transactional `config-test` and
  `config-set` commands. Empty or multi-answer arrays now fail closed instead
  of accepting the first success and ignoring ambiguous trailing results.

- Upgraded the Kea provider to ABI v6 and rebuild accepted DHCPv4/DHCPv6
  subnet inventories from dangd's applied snapshot during startup
  reconciliation. Supplemental statistics are no longer silently empty after
  a process restart before the next configuration commit.

- Kept RFC 8431 operational route status internally consistent. An observed
  uninstalled route is now serialized as `inactive` instead of the impossible
  `active`/`uninstalled` combination; installed routes remain `active`.

- Enforced canonical family-qualified RIB identities: a configured alias
  disables the alternate built-in spelling for its native number/family,
  built-in names cannot be remapped to another number or family, and numeric
  suffixes with leading zeroes are rejected. Native operational readback now
  returns the same identity accepted for configuration and durable objects.

- Applied family-aware RIB identity validation to live `nh-add` before
  identifier allocation or persistence. Unknown aliases, bare numeric names,
  and wrong-family names no longer create state that fails recovery. Updated
  the loadable and isolated native provider harnesses to use family-qualified
  names and the supported `default` routing instance.

- Made modeled-to-native RIB mapping address-family aware. One dual-stack
  Linux table or FreeBSD FIB can now map to distinct IPv4 and IPv6 RFC 8431
  names without producing duplicate `rib-list` keys. Mapping format version 2
  requires `address-family`; safe built-in names are `ipv4-N` and `ipv6-N`,
  and ambiguous bare numeric modeled names and version-1 maps are rejected.

- Made startup reject durable RIB, nexthop, or route-binding identities that
  cannot be resolved through the active family-aware mapping. Legacy numeric
  registry entries now produce an explicit `ipv4-N`/`ipv6-N` migration error
  instead of silently diverging from operational RIB names.

- Made reusable-nexthop route bindings multipath-safe by including the modeled
  `route-index` in process and durable identities. Parallel referenced routes
  for one RIB/family/prefix no longer replace each other's binding. Registry
  format version 2 persists the index; version 1 remains readable with its
  historical single-binding behavior represented by index zero.

- Tightened reusable-nexthop resolution to the installed native path. A route
  with the same RIB, family, and prefix no longer resolves a binding unless its
  gateway and/or interface also match the referenced nexthop, preventing false
  `resolved` transitions in multipath and replacement scenarios.

- Rejected non-default RFC 8431 routing instances at the modeled name path.
  The Linux and FreeBSD backends do not yet map instances to VRFs or VNETs;
  accepting another name previously installed its routes in the host default
  instance and silently violated isolation.

- Made external route-change tracking multipath-safe by including the gateway,
  interface, and special-nexthop identity in the native route key. Distinct
  paths for one prefix no longer overwrite each other. Managed confirmation
  ignores the expected modeled-versus-synthetic route-index difference, so a
  kernel readback does not duplicate an already published success event.

- Derived RFC 8431 `local-only` operational state from native route metadata:
  Linux `RT_SCOPE_HOST` and FreeBSD `RTF_LOCAL`. Connected and remote host
  routes are no longer incorrectly conflated with destinations owned locally.

- Corrected operational RFC 8431 nexthops that have both a gateway and an
  outgoing interface. They now use the modeled combined IPv4 or IPv6 container
  instead of emitting two mutually exclusive choice leaves. Both forms are
  covered by direct YANG data validation.

- Corrected RFC 8431 XML address-family identity values from the internal
  `ipv4`/`ipv6` shorthand to the schema-defined `ipv4-address-family` and
  `ipv6-address-family`. Independent `yanglint -t notif` fixtures now validate
  both notification shapes, including interface leafref context.

- Enabled FreeBSD interface-only RFC 8431 nexthops by resolving exactly one
  usable local address in the route family through `getifaddrs(3)`. Automatic
  IPv6 link-local addresses are excluded and zero or multiple candidates fail
  at the modeled nexthop path instead of selecting an arbitrary address.

### Added

- Published native receive, discard, and discard-with-error routes as RFC 8431
  special nexthops. Linux local/blackhole/unreachable/prohibit routes and
  FreeBSD local/blackhole/reject routes now appear in operational state.
  Kernel-owned special routes are explicitly non-mutable; route delete/update
  and whole-RIB deletion fail closed instead of approximating them as ordinary
  forwarding routes.

- Completed mapped FreeBSD FIB observation. Operational reads and notification
  polling now query FIB 0 plus each explicitly configured FreeBSD FIB and map
  every result back to its unique RFC 8431 RIB name.

- Wired modeled RIB names through configuration validation and reversible
  native plans, imperative route/RIB RPCs, Linux operational translation, and
  notification tracking. Registries and emitted XML retain modeled names while
  platform commands receive the mapped number; an unmapped arbitrary name
  fails at the RFC 8431 RIB path.

- Added the strict bidirectional foundation for RFC 8431 modeled RIB names.
  Its bounded versioned JSON format supports distinct Linux table and FreeBSD
  FIB numbers, preserves numeric identity fallback, and rejects ambiguous
  forward or reverse aliases before the mapping is used.

- Implemented RFC 8431 `nexthop-resolution-status-change` for reusable
  nexthops. Live imperative and datastore route bindings are joined with the
  native installed-route inventory, producing quiet-baseline resolved and
  unresolved transitions with complete portable nexthop payloads.

- Added native-route snapshot tracking for externally initiated RFC 8431
  `route-change` notifications. The first observation establishes a quiet
  baseline, later add/change/remove transitions are reported deterministically,
  and managed operations advance that baseline to suppress duplicate events.

- Upgraded the RFC 8431 RIB provider to plugin ABI v8 and added bounded
  `route-change` delivery for successfully reconciled datastore changes and
  durable imperative route RPCs. Failed native execution, persistence, and
  rolled-back tentative changes do not publish success events.

- Persisted the address family established by RFC 8431 `rib-add` and use it as
  the explicit family for interface-only reusable nexthops. Such nexthops now
  appear in operational data even before a native route exists; missing and
  conflicting RIB family context fails closed instead of being inferred from
  host interfaces.

- Added lossless reusable-nexthop registry export and recovery. The recovery
  boundary rebuilds allocation state, route bindings, and their reference
  counts, rejects dangling or duplicate durable data before mutation, and
  refuses to overwrite a live process registry.
- Connected reusable-nexthop startup recovery and `nh-add`/`nh-delete`
  acknowledgement to the private atomic registry sidecar. RPC mutation is
  serialized, corrupt state fails closed, and an injected durable-write failure
  restores the exact prior registry before returning an attributed error.
- Made imperative `route-add`, `route-delete`, `route-update`, and `rib-delete`
  bindings durable before acknowledgement. A sidecar failure runs the complete
  inverse native plan, restores the prior registry checkpoint, and reports any
  native or registry compensation failure alongside the write error.
- Added a guarded Linux/FreeBSD two-peer FRR RIPng interaction. Each endpoint
  uses disposable ULA interface and loopback addresses, verifies a live
  link-local neighbor and learned `/128`, invokes `clear-ripng-route` on one
  peer, and requires the route to disappear and be learned again. Reversed
  roles prove the native operational and RPC behavior on both platforms.
- Added a guarded Linux/FreeBSD two-peer FRR RIP interaction that verifies
  native neighbor state and an actually learned route over a sterile private
  LAN. A designated peer then invokes `clear-rip-route` and requires the route
  to disappear and be learned again from a holding peer. Configuration, RPC,
  and operational reads use native mgmtd.
- Extended the isolated FRR mutation diagnostic with an optional operational
  read performed while the committed configuration is live. The RIP and RIPng
  native interaction now requires their default instances to appear through
  native mgmtd `GET_DATA` before restoring the exact before-image.
- Added isolated RIP and RIPng instance transaction tests for Linux and
  FreeBSD. Each live backend validates and commits an interface-free `default`
  instance, returns the accepted running XML, and restores the exact empty
  before-image without creating addresses, routes, neighbors, or packets. Both
  interactions pass independently on all four validation hosts.
- Added cross-platform, profile-only BFD transaction evidence and hardened all
  FRR commits with post-commit running-datastore verification in a fresh mgmtd
  session. A commit that FRR acknowledges but silently drops now fails at the
  affected module path while retaining rollback eligibility. Tests cover that
  failure and successful compensation. FRR 10.7.0/10.7.1 on all four hosts
  advertises `frr-bfdd` but retains no BFD profile because `bfdd` registers no
  mgmtd backend; the guarded native test skips only after proving the no-op and
  restoring the before-image. Round-trip observation failures now report
  phase-specific, bounded before/applied/final readback evidence.
- Added a Linux and FreeBSD read-only FRR optional-daemon inventory test. Each
  installed protocol daemon is started with mgmtd and zebra in its own disposable
  pathspace, without creating interfaces, addresses, or routes, and is
  classified by whether its expected module appears in the live RFC 8525 YANG
  Library. Missing binaries, early daemon exits, and absent mgmtd backends are
  reported separately. Independent Ubuntu 26.04.1 runs found BFD, RIP, and
  RIPng advertised by FRR 10.7.1; the other six installed optional daemons did
  not register their models. Daemon startup diagnostics are retained in the
  per-pathspace logs instead of obscuring the inventory summary.
  Independent FreeBSD 16.0-CURRENT runs found the same three advertised modules
  among six installed optional daemons. The CTest fixture uses `/bin/sh`
  explicitly so it works when the source checkout is on a no-execute mount.
- Enforced FRR/RFC 8431 routing-provider package exclusivity. Debian retains
  symmetric package conflicts, while both components now install the same
  routing-domain ownership marker so FreeBSD `pkg` rejects co-installation even
  though CPack cannot emit the native FreeBSD conflicts field. A portable test
  stages both components and verifies the marker and Debian metadata contract.
- Corrected the FreeBSD packaging guide's component count and included the
  previously omitted IP-management package in its all-component build loop.
- Audited the FRR operator guide and collection overview against the ABI-v8
  implementation. Corrected stale module scope, operational ownership,
  notification selector, reconciliation, isolated-test safety, and installed
  versus live-schema descriptions. Discovery now enforces the documented
  contract that routing, zebra, and staticd appear as implemented—not merely
  import-only—in FRR's live module-set.

- Added the native FRR notification wire foundation. The codec now constructs
  bounded on-change and periodic `NOTIFY_SELECT` requests and decodes only
  modeled XML `NOTIFY` events, rejecting datastore synchronization operations,
  invalid XPath splits, embedded NULs, empty payloads, and non-XML formats.
  Portable tests use the real public mgmtd structure layout; a long-lived
  asynchronous session and end-to-end IS-IS/RIP delivery remain follow-up work.
- Added the long-lived mgmtd session primitives needed by the FRR event reader.
  One-way selector transmission is separated from correlated RPC exchanges,
  idle receive timeouts preserve the stream, and unsolicited frames must carry
  the selected session identifier before modeled notification decoding. Tests
  cover idle-and-resume behavior and a complete select/receive/close exchange.
- Added the FRR plugin-owned notification reader and runtime-gated RIP/IS-IS
  model loading. Installed sources become implemented only when FRR's live RFC
  8525 module-set includes the module; the reader negotiates XML, selects only
  those module prefixes, checks event/session/module identity, bounds its queue,
  and reconnects after daemon restarts. Native protocol RPCs follow the same
  live-module gate. Captured-library tests never start background I/O.
- Corrected conditional protocol ownership so the separate
  `/frr-ripd:ripd` and `/frr-isisd:isis` roots participate in extraction,
  validation, atomic commit, rollback, applied-state reconciliation, drift
  detection, and operational retrieval. Live testing also replaced invalid
  bare module-prefix subscriptions with exact modeled event XPaths and added a
  guarded Linux namespace reproducer. FRR 10.7.1 successfully emits the RIP
  event but mgmtd aborts while encoding it; the opt-in test reports that exact
  upstream assertion as skipped and fails for any other error.
- Corrected FRR parent-tree ownership by publishing live `frr-interface` and
  `frr-vrf` modules as implemented and including their complete `lib` roots in
  atomic candidate operations, rollback, reconciliation, drift detection, and
  operational output. Protocol configuration augmenting an interface can no
  longer be advertised while silently falling outside the provider transaction.
- Extended runtime-gated FRR protocol support to BFD, EIGRP, OSPFv2, Pathd,
  PIM, RIPng, and VRRP in addition to RIP and IS-IS. The provider loads and
  advertises a daemon schema only when FRR's live YANG Library implements it,
  owns each modeled standalone root, retains augment-only modules inside the
  routing/interface parent transaction, publishes standalone operational data,
  and permits their native modeled RPCs through the common mgmtd path. BGP
  remains deliberately excluded until its backend appears in the live library.
- Added the Linux-only VPP provider architecture and its physical-interface
  ownership safety boundary. Documented stable PCI identity, empty-by-default
  allowlisting, trusted management-path denial, independent recovery, and the
  read-only test-host checkpoint that permanently protects `ens18` and limits
  future physical experiments to explicitly authorized `ens19` devices.
- Added a shell-free Linux ownership inventory that maps interfaces to stable
  PCI identity and drivers through sysfs, detects the live SSH destination with
  `getifaddrs`, and reads IPv4/IPv6 default-route evidence from procfs. It
  excludes Linux's unreachable IPv6 loopback sentinel and passed independently
  on both authorized Linux hosts without changing network state.
- Added the validation-only `dang-vpp-interface-ownership` YANG model and a
  fail-closed ownership evaluator. VPP claims now require an exact live PCI,
  MAC, and vendor/device match and reject unknown, default-route, and current
  management-session interfaces with an attributed model path. Portable policy
  tests and read-only live validation pass on both Linux hosts.
- Added an API-only, reversible VPP loopback transaction seam with immediate
  compensation for partial creation failures and retained recovery identity
  when compensation fails. Failure-injected tests pass on both Linux hosts;
  live VAPI testing remains deferred because FD.io does not publish an Ubuntu
  26.04 repository for the current test systems.
- Added the real FD.io generated C++ VAPI adapter for loopback creation,
  administrative state, and deletion, including message-availability and
  bounded correlated-response checks. Unix-domain-socket transport rejects an
  absent daemon immediately and avoids stale shared-memory attachment. An
  isolated, plugin-free VPP 26.06 lifecycle now passes on both Ubuntu 26.04
  hosts using unpacked Ubuntu 24.04 packages without changing their package
  databases or exposing PCI devices.
- Added the `dang-vpp-interfaces` model and deterministic loopback planner.
  Configured identity now uses VPP's stable loopback user instance instead of
  ephemeral `sw_if_index`; safe plans create before activation and deactivate
  before deletion. Schema, parser, ordering, and live `loop0` rollback tests
  pass on both Linux hosts.
- Added the loadable ABI-v7 VPP provider with embedded model retrieval,
  exclusive resource declarations, live loopback lookup after restart, and one
  compensated software-interface hardware action. Real provider-level
  create/enable and rollback-to-absence interactions pass on both Linux hosts;
  physical ownership remains deliberately fail-closed.
- Added complete live loopback operational publication and ABI-v6 applied-state
  reconciliation. A single bounded interface dump supplies stable instances and
  administrative state; reconciliation replaces only the VPP subtree and
  preserves unrelated configuration. Both hosts verify presence after apply
  and absence after rollback through the exported provider ABI.

- Implemented RFC 8431 `nh-add` and `nh-delete` with thread-safe, per-RIB
  nexthop identifier allocation, strict portable base-nexthop validation, and
  modeled failures for unknown identifiers and unsupported forms. Added
  lifecycle, RIB-isolation, repeated-delete, and unsupported-composite tests,
  and documented the remaining persistence, operational-state, and
  route-reference gaps for reusable nexthops.
- Resolved registered `nexthop-ref` identifiers during configuration commits
  and `route-add`, scoped lookup to the containing RIB, and added attributed
  missing-reference failures plus native-planner projection tests.
- Extended `nexthop-ref` resolution to prefix-selected `route-update`, retaining
  its observed before-image rollback while reporting unknown references through
  per-route RFC failure detail.
- Added transaction-lifetime reservations and active reference counts for
  datastore-managed nexthops. `nh-delete` now fails closed while a prepared or
  committed route retains the requested RIB/identifier pair, including across
  apply and rollback transitions.
- Extended reference lifetime enforcement to imperative route RPCs. Successful
  add/update binds the route key to its retained nexthop, delete/RIB deletion
  releases bindings, failed native mutations release reservations, and a new
  lifecycle test caught and fixed moved-from route-key cleanup.
- Published live reusable-nexthop identifiers in RFC 8431 operational RIB
  state from a consistent registry snapshot, including gateway-typed RIBs with
  no observed routes. Family-ambiguous interface-only entries fail closed by
  remaining absent until their containing RIB establishes a family.
- Rebuilt datastore-owned nexthop reference counts from the ABI-v6 applied
  configuration reconciliation callback. Restart and restored-snapshot paths
  now establish the exact active set instead of depending on pre-restart
  in-memory counts, while transaction reservations still close deletion races.
- Added the durable registry sidecar foundation: a bounded, versioned JSON
  codec validates unique nexthops and referentially intact imperative route
  bindings, rejects non-private files, and writes through a mode-0600 temporary
  file with file and parent-directory synchronization before acknowledgement.

- Adopted the existing ABI-v4 RFC 8343/8344 IP-management provider from dangd,
  including its direct Linux rtnetlink and FreeBSD ioctl/route-netlink
  backends, pinned `ietf-ip` model, parser and native tests, documentation, and
  independent Debian/FreeBSD package component. The plugin name and ABI remain
  unchanged.
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
- Added RFC 8431 observed route publication. The plugin reads Linux rtnetlink
  and FreeBSD `NET_RT_DUMP` data directly, emits IPv4/IPv6 unicast routes with
  active/installed status and stable synthetic indexes, and reports attributed
  kernel-read failures. Native tests require an installed isolated route to
  appear in the plugin's operational XML before rollback.
- Added the portable RFC 8431 `route-add` RPC with strict reuse of datastore
  route parsing, independent batch-member execution, schema-shaped success and
  failure counts, optional per-route error details, and attributed malformed
  envelope errors. Native Linux namespace and FreeBSD VNET tests exercise the
  RPC installation and transaction-backed cleanup without touching LAN routes.
- Added RFC 8431 `route-delete`. It resolves prefix-only requests from a fresh
  native route inventory, deletes only unique matches using their observed
  nexthop data, reports missing routes with error code 2, and fails closed on
  ambiguous multipath matches. Native tests now exercise RPC add and delete as
  one isolated lifecycle.
- Added prefix-selected RFC 8431 `route-update` for portable base-nexthop and
  complete route-attribute replacements. Updates use the observed route as a
  before-image and compensate a failed installation by restoring it. Portable
  tests inject the failure boundary, and native tests exercise add, update, and
  delete in Linux namespaces and FreeBSD VNET jails.
- Added `rib-add` and `rib-delete`. RIB creation validates Linux logical table
  identifiers or preallocated FreeBSD `net.fibs` entries and rejects unimplemented
  RPF enforcement. RIB deletion empties the observed table as one compensated
  plan, restoring completed deletions after a later failure. Linux native tests
  exercise logical table validation and emptying; FreeBSD avoids emptying FIB 0.
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

### Fixed

- Accepted Debian's canonical `/run/frr` spelling in the guarded native
  notification mutation probe. The safety check previously started with a
  valid `/var/run/frr` socket but rejected it after resolving `/var/run` to its
  `/run` symlink target.

### Changed

- RFC 8431 reusable-nexthop persistence now encodes absent optional values as
  explicit JSON nulls. This avoids relying on optional-value conversions added
  by newer nlohmann-json releases and restores compilation with Debian 13's
  supported 3.11 series. The persistence round-trip test now covers an entry
  with every optional field absent. The collection build guide also lists the
  previously omitted Debian PAM development headers and complete baseline
  dependency commands for Debian/Ubuntu and FreeBSD.
- FRR operational filtering now preserves every direct zebra-owned augment
  beneath an imported parent-list instance instead of silently returning only
  the first. Copy failures are reported as provider errors, and regression
  coverage verifies that multiple sibling augment nodes survive while
  parent-owned state remains excluded.
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

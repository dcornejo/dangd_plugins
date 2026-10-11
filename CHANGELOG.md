<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Changelog

All notable changes to the external dangd plugin collection are recorded here.

## [Unreleased]

### Changed

- Expanded the isolated RFC 8431 NETCONF session to cover every implemented
  imperative operation after the candidate lifecycle: `rib-add`, `nh-add`,
  `nh-delete`, `route-add`, duplicate `route-add`, `route-update`,
  `route-delete`, and `rib-delete`. The test verifies multi-element operation
  output, durable nexthop allocation, operational preference readback, and RFC
  duplicate-route error code 1. Linux deletes its disposable table; FreeBSD
  FIB 0 proves the fail-closed modeled refusal required while immutable native
  routes remain. Both systems finish without the test-created routes.

- Made the RFC 8431 configuration boundary fail closed for modeled controls
  the portable backend does not enforce. Datastore commits now reject routing
  instance interface membership, router ID, lookup limit, enabled RPF checks,
  and directly configured reusable-nexthop identifiers at their exact schema
  paths instead of acknowledging and ignoring them. Explicitly disabled RPF
  remains a valid no-op. The native NETCONF test now commits an RFC 8343/8344
  interface and an RFC 8431 gateway route that references it in one generic
  two-plugin transaction, proving cross-module leafref resolution on Linux and
  FreeBSD rather than merely loading both providers beside a discard route.

- Completed a real RFC 8431 NETCONF candidate transaction on isolated Linux
  and FreeBSD kernels. The native harness now loads both the IP-management and
  RIB plugins, locks and edits candidate, validates, commits a native route,
  reads its modeled operational state, deletes it, commits again, and verifies
  the native table or FIB is empty. This exposed and fixed common RFC 8343/8344
  provider gaps: empty startup state is valid, `if-mib` and the official
  import-only `iana-if-type@2026-03-17` identity library are advertised,
  IPv4/IPv6 augment namespaces are explicit, IPv4 no longer receives the
  IPv6-only address-status leaf, and system-created interfaces are published
  in the NMDA `/interfaces` tree as well as deprecated `/interfaces-state` so
  cross-model leafrefs resolve. The shared discovery checker now accepts
  quoted or unquoted YANG revision arguments.

- Unified RFC 8431 native route notifications with the operational projection.
  Polling now restores applied route indexes and collapses representable ECMP
  members before change tracking, so one modeled weighted route produces one
  route-level transition instead of synthetic per-path events. Reusable
  nexthop resolution still evaluates the uncollapsed native paths. Unit tests
  cover direct and weighted identity, and the isolated loadable-plugin test
  establishes a quiet baseline before apply and requires exactly one managed
  route-change plus the two expected member-resolution transitions.

- Preserved modeled RFC 8431 identities for ordinary native route readback.
  The RIB plugin now snapshots dangd's reconciled applied routes and restores
  a uniquely matching configured `route-index`; unmatched external routes keep
  deterministic synthetic indexes. Resolved reusable routes additionally
  publish their `nexthop-id`, sharing policy, and expanded native definition.
  The snapshot is transient, remains subordinate to dangd's authoritative
  datastore, and uses only the generic applied-configuration reconciliation
  contract. Unit and schema fixtures cover both direct and reusable routes,
  while the isolated loadable-plugin lifecycle requires the modeled index.

- Completed the RFC 8431 weighted-nexthop round trip and now advertise the
  optional `nexthop-load-balance` feature. Native paths for one destination
  are projected as one schema-valid `nexthop-lb` route with exact weights;
  managed routes reuse their durable route index and reusable-nexthop IDs,
  while external routes receive deterministic snapshot-local IDs. Linux and
  FreeBSD loadable-plugin tests now require weighted operational XML after
  apply and absence after rollback, and the feature-enabled operational
  fixture passes direct YANG validation. Operational reads now use the
  registry's resolution view so bindings reconstructed from dangd's
  authoritative datastore participate alongside persistent RPC bindings.

- Added reversible native RFC 8431 weighted-nexthop mutation without yet
  advertising the optional `nexthop-load-balance` feature. Linux encodes one
  bounded `RTA_MULTIPATH` request with the exact modeled 1-through-99 weights;
  FreeBSD sends acknowledged per-path route-netlink requests and compensates
  completed members if a later member fails. Both backends validate resolved
  gateways and interfaces before mutation. Isolated namespace and VNET tests
  now exercise direct weighted install/readback/delete plus a loadable-plugin
  transaction from reusable-nexthop RPC allocation through apply,
  operational observation, and rollback. The feature remains unadvertised
  until weighted operational XML preserves the modeled structure.

- Preserved exact native RFC 8431 ECMP path weights in the route-observation
  contract without prematurely advertising the optional load-balance feature.
  Linux now decodes classic `rtnh_hops` weights and both bytes of persistent
  nexthop-group weights, while FreeBSD reads the distinct `rmx_weight` field.
  Weight-only changes reach the generic route-change tracker without changing
  route identity. Isolated native tests require `{1,1}` and `{2,3}` readback on
  Linux and `{2,3}` readback from a two-interface FreeBSD VNET. Weighted
  operational XML remains explicit follow-on work, so the YANG feature is
  still unadvertised.

- Corrected FreeBSD RFC 8431 route-preference observation. The routing sysctl
  exposes path weight and route metric separately; the provider now reads
  `rmx_metric`, matching the `NL_RTA_PRIORITY` value it writes, instead of
  misreporting `rmx_weight`. The isolated Linux and FreeBSD native lifecycle
  tests now require preference 10 to round-trip after every ordinary and
  special route installation.

- Preserved the RFC 8431 reason for native nexthop resolution transitions.
  Linux dead multipath members now publish `unresolved-nexthop` in operational
  route status. A later installed-state transition emits `resolved-nexthop` or
  `unresolved-nexthop` in `route-change`; ordinary additions, removals, and
  preference changes remain unattributed rather than receiving a guessed
  cause. The notification example is schema-validated with the reason list.

- Made RFC 8431 `route-add` non-destructive when a destination already
  exists. Production now inventories the modeled RIB before mutation, reports
  the RFC-defined repeated-route error code 1, and uses exclusive native
  create flags to close the inventory-to-apply race instead of inheriting
  replace semantics. Duplicate RIB/family/destination keys in one datastore
  or RPC batch also fail closed, because the portable base-nexthop slice
  cannot preserve them as independent native routes. FreeBSD inventory now
  covers every kernel FIB, including unaliased built-in RIB names,
  rather than only FIB 0 and explicit mappings. Isolated Linux and
  FreeBSD lifecycle tests prove that a second add leaves the first route
  available for update and deletion.

- Extended RFC 8431 reusable nexthops to the writable `discard` and
  `discard-with-error` identities. `nh-add` now obtains their family from a
  prior `rib-add`, `nexthop-ref` resolves them into the exact native special
  route, persistence format version 3 retains them across restart, and
  resolution notifications match and serialize the special identity.
  Kernel-owned `receive`, mixed choice forms, unknown identities, missing
  family context, and conflicting recovery state fail closed. Versions 1 and
  2 of the private sidecar remain readable.

- Made the RFC 8431 direct `discard` and `discard-with-error` special
  nexthops fully writable and reversible through configuration commits,
  `route-add`, `route-update`, `route-delete`, and `rib-delete` on Linux and
  FreeBSD. Native observation now classifies those exact route kinds as
  mutable while preserving kernel-owned `receive` routes as read-only.
  Route updates also clear every obsolete member of the base-nexthop choice,
  so a gateway cannot leak into an interface-only or special replacement.
  Isolated dual-stack native tests install, observe, and delete both special
  identities; the FreeBSD VNET fixture now initializes its otherwise empty
  loopback addresses and the backend supplies the platform-required loopback
  gateway without exposing that detail in the YANG configuration.

- Completed guarded pair-wide Kea NETCONF validation on Kea 3.2 Linux and
  FreeBSD. The plugin now marks its local participant through the generic ABI,
  maps portable hook basenames into each host's package directory, reports
  transient `waiting`/synchronization states as pending convergence, and emits
  exact expected/actual HA diagnostics on permanent failure. The bidirectional
  harness proves successful lifetime changes on both daemons, unavailable-peer
  fail-closed behavior with unchanged authoritative state, DHCPv4/DHCPv6
  allocation, replication, automatic failover, and recovery. Its authoritative
  fixture now includes DHCPv6 Rapid Commit instead of relying on out-of-band
  daemon state.

- Completed native Kea package validation on Linux and FreeBSD. The generated
  packages contain only the provider, its six YANG modules, and `KEA.md`, carry
  the platform Kea 3.2 dependencies, install beside a freshly generated
  `dangd` package, load through the packaged worker, and pass an installed
  `dangd --check` run. A staging regression now protects the exact Kea
  component boundary. Kea peer-candidate serialization also uses the supported
  libxml2 buffer accessors, and the system authentication server now delivers
  its fixed response with bounded, SIGPIPE-safe complete writes.

- Updated the Kea HA operator workflow for the guarded generic
  `dangctl --edit-config` transaction. Kea configuration now has an explicit
  CLI path through ordinary candidate lock, edit, validation, commit, and
  unlock without adding provider awareness to the client or dangd core. Live
  Linux/FreeBSD pair-wide NETCONF evidence now passes in both primary-role
  directions.

- Reconciled the Kea HA documentation with dangd's completed normal NETCONF
  peer-commit integration. The documented contract now includes recursive-plan
  suppression, datastore-before-decision ordering, pre-mutation multi-group
  rejection, the fail-closed degraded-peer behavior, and the supported
  local-primary initiation boundary.

- Reclassified the FRR-native provider as the lowest-priority deferred work and
  made native BGP a hard completion gate. The existing transaction, rollback,
  reconciliation, operational, RIP, and RIPng foundation remains available for
  experimentation, but the collection does not claim a supported FRR offering
  until bgpd exposes a usable `frr-bgp` mgmtd backend and complete BGP behavior
  passes on Linux and FreeBSD. CLI execution is not an acceptable workaround.

- Expanded Linux routes backed by persistent nexthop object IDs. The observer
  now inventories objects through `RTM_GETNEXTHOP`, recursively resolves simple
  and grouped IDs, and publishes each representable gateway/interface path in
  the existing RFC 8431 base-nexthop view. Missing, cyclic, encapsulated, FDB,
  wrong-family, or interrupted results fail closed or remain omitted rather
  than producing a partial route. The isolated namespace suite now verifies a
  real weighted two-member nexthop group and its read-only safety boundary
  while retaining the explicit variance that weights are not modeled without
  the load-balance feature.

- Closed an RFC 8431 route-attribute fidelity hole on Linux and FreeBSD.
  Configured routes, `route-add`, and `route-update` now reject
  `local-only=true` before native mutation because neither portable backend can
  safely create the kernel-owned receive-route semantics represented by that
  value. Genuine `RT_SCOPE_HOST`/`RTF_LOCAL` routes remain available as
  read-only operational state. Parser, platform validation, RPC, and loadable
  plugin-contract tests cover the rejection and exact attributed path.

- Reconfirmed the FRR 10.7 optional-daemon inventory on Linux and FreeBSD.
  RIP and RIPng remain the only usable registered protocol backends and already
  have bidirectional peer state and RPC evidence. BFD remains an
  advertised-but-unapplied backend, while OSPFv2, IS-IS, PIM, EIGRP, Pathd,
  VRRP, and BGP do not register their models with mgmtd; further live protocol
  work is therefore gated on a newly usable upstream backend.

- Completed the production command-execution audit by replacing the FreeBSD
  RFC 7317 NTP provider's `std::system()` call with `posix_spawn(3)` of the
  absolute service(8) path and fixed base-ntpd argv. Spawn, wait, exit-status,
  and signal failures are now attributed precisely. Unit coverage verifies
  wait-status interpretation, and the opt-in FreeBSD lifecycle test starts,
  stops, verifies, and restores the service's initial state. The final source
  scan contains no shell execution; the only remaining production process
  boundary is this documented FreeBSD rc.d fallback.

- Replaced Linux RFC 7317 NTP service commands with the systemd manager's
  native sd-bus API. The provider subscribes before enqueueing a chrony job,
  correlates the returned object path with `JobRemoved`, accepts only `done`,
  and bounds the wait to 30 seconds. Enabled state now uses
  `ReloadOrRestartUnit`, which also corrects rollback after a prior stop;
  disabled state uses `StopUnit`. Injectable policy tests cover dispatch and
  error fidelity, while an opt-in live test enables, disables, verifies, and
  restores chrony's initial state. The source and package metadata now require
  libsystemd on Linux, and the obsolete RIB `iproute2` package dependency was
  removed after both RIB backends moved to route netlink.

- Replaced the RFC 8431 FreeBSD production `route(8)` executor with bounded
  route-netlink mutation. IPv4 and IPv6 installs and deletions now carry an
  explicit FIB attribute and require the correlated kernel acknowledgement;
  gateways, interface indexes, and preference remain native attributes.
  Interface-only routes no longer require a local address, and the VNET suite
  verifies an unnumbered epair, both address families, plugin integration,
  deletion, and preflight rejection. Together with the Linux rtnetlink path,
  this removes all production process creation from the RIB provider while
  preserving injectable argv planning for deterministic unit tests.

- Replaced the RFC 8431 Linux production `ip(8)` executor with bounded direct
  rtnetlink mutation. Each install or deletion resolves its interface index,
  sends one sequenced request, and requires the correlated kernel ACK under a
  receive timeout; kernel rejection enters the existing reverse-compensation
  path. The namespace suite now covers plugin mutation with a combined gateway
  and interface, direct IPv4 and IPv6 mutation, observation, rollback,
  deletion, and an unreachable-gateway rejection that leaves no route behind.
  The Linux argv builder remains only for deterministic validation and
  unit-test injection. FreeBSD is now covered by the native route-netlink
  implementation recorded above.

- Started the production command-execution audit with a provider-by-provider
  inventory and ordered Linux/FreeBSD remediation plan. RFC 7317 restart and
  shutdown now use the documented native PID 1 signal interfaces on Linux and
  FreeBSD behind the existing deployment guard instead of passing
  service-manager commands through a shell. This preserves orderly service and
  filesystem shutdown rather than invoking an immediate kernel reboot.
  Injectable policy tests cover guard enforcement, restart/power-off selection,
  and native error propagation. The remaining FreeBSD NTP service boundary is
  explicitly tracked; the Linux NTP and both RFC 8431 route utility executors
  have since been removed.

- Added a separately packaged RFC 9249 NTP provider to the roadmap. The plan
  pins `ietf-ntp@2022-07-05`, requires Linux and FreeBSD validation, prefers
  native programmatic daemon APIs, and requires explicit exclusive ownership
  and migration relative to the RFC 7317 system plugin's NTP support.

- Synchronized the Kea pair-wide deployment guide with dangd's version-2 peer
  endpoint identity and generic execution controller. Endpoint mappings now
  use exact group/participant pairs, and the core can bind one composed group
  to authenticated participants, cryptographic persistent tokens, retained
  plugin verifiers, and the crash-safe journal. The guide now identifies the
  precise remaining production boundaries: authenticated recursion
  suppression for participant commits, ordering local snapshot durability with
  the distributed decision, and atomic handling or rejection of multi-group
  commits.

- Upgraded the Kea provider to the generic ABI-v9 peer transaction contract.
  One authoritative two-member hot-standby change now produces complete,
  member-specific DHCPv4 and DHCPv6 module images and an opaque verifier
  identity without exposing endpoints, credentials, sessions, or transport to
  the plugin. Managed families must agree on one portable primary/standby
  roster; unsafe identities, role or roster ambiguity, multiple HA hooks, and
  unsupported relationship counts fail closed. Authenticated running and
  operational replies are routed through the existing strict image and health
  verifier. Portable tests cover dual-stack plans, participant specialization,
  exact steady-state scopes, altered verifier identity, mismatched rosters,
  unsafe names, and duplicate primary roles. Load-balancing and passive-backup
  remain local-member modes until their distinct coordinated health policies
  are defined.

- Added a strict Kea HA peer-transaction verifier for dangd's authenticated
  running and operational replies. It binds every official Kea configuration
  readback to the complete proposed managed image, binds every `dang-kea-ha`
  relationship back to that configuration's mode and member identities, and
  requires exact stable states and scopes, an in-touch uninterrupted active
  peer, and a caller-bounded status age. Duplicate, missing, stale, malformed,
  DTD-bearing, cross-member, and configuration-drifted replies fail closed.
  Portable tests cover healthy, stale, disconnected, drifted, and XML-unsafe
  results. Production transaction initiation still needs to construct the
  per-member candidates and invoke this verifier through dangd.

- Updated the Kea HA deployment boundary for dangd's transport-neutral peer
  transaction coordinator and private crash-safe journal. The guide
  records fail-closed startup and `SIGHUP` journal inspection and distinguishes
  those tested building blocks from the mutual-TLS recovery adapter and the
  private, validated group-and-participant-to-endpoint and trust
  mapping. Startup and reload now use that mapping to finish a durable COMMIT
  decision before serving requests. A reusable authenticated session now
  retains locks and framing across candidate RPCs. The guide now records the
  complete generic participant mapping for lock, complete candidate transfer,
  validation, confirmed apply, authenticated running and operational readback,
  confirmation, reconnecting cancellation, and release, with live two-peer
  commit and rollback coverage. It narrows the remaining Kea work to per-member candidate
  translation, HA health policy, and the production entry point, and records
  the safe behavior on each side of the durable group decision.

- Raised the managed Kea baseline to 3.2.0. Reconciliation now rejects older
  daemon identities, portable tests cover that boundary, Linux validation uses
  ISC's supported `kea-3-2` packages and their required `/var/run/kea` socket
  directory, and the installation guide names the current ISC repository,
  packages, and services. Debian and FreeBSD plugin package metadata now also
  declares the corresponding Kea runtime. Linux and FreeBSD native workflows
  validate Kea 3.2.x.

- Recorded the complete mutual-TLS HA matrix against Kea 3.2.1 on Linux and
  Kea 3.2.0 on FreeBSD. Both primary-role assignments pass dual-stack lease
  replication, guarded manual takeover, automatic partner-down service,
  planned maintenance, recovery, and post-recovery replication. Both DHCP
  family listeners also reject clients with no certificate, a certificate
  signed by an untrusted CA, and plaintext HTTP.

### Fixed

- Tightened the live Kea HA readiness gate to require the peer's last observed
  state to reach `hot-standby`, not merely report an established control
  connection. This prevents the first DHCPv4/DHCPv6 exchange from racing the
  final HA state transition on mixed Linux and FreeBSD pairs.

- Expanded Linux rtnetlink `RTA_MULTIPATH` observations into one RFC 8431
  route entry per native base nexthop. Operational state now preserves both
  gateway/interface path identity and dead-path installed state instead of
  emitting one route with an empty, schema-invalid nexthop. The isolated Linux
  workflow creates a real two-interface ECMP route and requires both paths;
  unresolved nexthop-object IDs are omitted until they can be expanded safely.

- Preserved empty RFC 8431 RIB registrations in operational state. A durable
  successful `rib-add` now remains visible before any native route or reusable
  nexthop exists and after plugin restart. Operational route observation and
  registry capture are serialized against imperative RPCs, preventing a reply
  from mixing two transaction epochs. Portable regressions plus native Linux
  network-namespace and FreeBSD VNET tests cover the behavior.

### Added

- Added reconciled native daemon versions to the `dang-kea-instance`
  operational tree. The plugin obtains each version through `version-get`,
  accepts only a bounded three-component numeric identity, retains it with the
  accepted configuration snapshot, and publishes it beside the corresponding
  DHCPv4 or DHCPv6 family. Malformed version replies fail reconciliation at
  the owning module path. Native Linux and FreeBSD Kea 3.2.0 tests
  require the versioned instance entries in both ordinary and HA state reads.

- Added native Kea command-capability attestation to applied-state
  reconciliation. Before accepting a daemon, the plugin now requires its
  `list-commands` inventory to contain the core configuration, version,
  family-specific lease/statistics, reservation, and—when configured—HA
  status commands used by the managed model. Malformed inventories and a
  configured hook that failed to register its command now fail at the owning
  module's configuration path. Portable negative tests and the complete Linux
  and FreeBSD native workflows cover the contract.

- Added explicit multiple-instance Kea support through separate dangd process
  boundaries. `DANG_KEA_INSTANCE_ID` supplies a stable portable identifier
  while retaining `default` for existing single-instance deployments. The new
  read-only `dang-kea-instance` model publishes that identity and the immutable
  DHCPv4/DHCPv6 target inventory, allowing a client to verify which independently
  persisted NETCONF endpoint it reached. Invalid identifiers fail plugin
  startup use; schema, discovery, negative identity, and operational-output
  assertions cover the contract. Deployment documentation now requires unique
  Kea sockets, datastore files, NETCONF endpoints, service identities, and
  runtime paths, and states the absence of cross-instance atomicity.
  The native Linux and FreeBSD workflows now start two DHCPv4 daemons at once,
  reconcile and query independently identified plugin processes concurrently,
  stop only the secondary daemon, require its failure to name the DHCPv4
  module and exact configuration path, and prove the primary remains usable.

- Corrected Kea HA YANG discovery metadata to advertise the packaged
  `2026-09-28` revision and retrieval URL rather than the superseded initial
  revision. The generic plugin discovery smoke test can now assert an expected
  source revision against both the ABI metadata and embedded module text, so a
  future model-file rename cannot silently leave YANG Library stale.

- Added effective Kea HA transport security to the read-only operational
  model. Each relationship now reports whether its local dedicated listener
  and singular active-remote channel use plaintext or TLS, whether the TLS
  listener requires client certificates, and whether non-HA commands are
  restricted. The values are resolved from the accepted configuration using
  Kea's global-to-peer TLS inheritance and empty-string override rules; an
  incomplete effective credential triplet fails closed, and certificate and
  private-key paths are never published. Portable tests cover plaintext,
  inheritance, peer disablement, non-default security flags, redaction, and
  malformed partial TLS configuration.

- Hardened the Kea HA TLS matrix with negative trust and downgrade tests. Each
  dedicated DHCPv4 and DHCPv6 listener must reject three non-peer clients from
  both hosts: a CA-validating client with no certificate, a client presenting a
  certificate from a separately generated rogue CA, and plaintext HTTP sent to
  the HTTPS port. The untrusted CA and key use the same restricted disposable
  lifecycle as the valid fixtures and are included in cleanup audits.

- Added real mutual-TLS coverage for Kea's dedicated HA listeners. The
  cross-host harness can issue a disposable CA plus IP-bound Linux and FreeBSD
  server/client certificates, require client certificates, and run the full
  automatic failover, lease synchronization, recovery, and role-reversal proof
  over HTTPS. Both address-family listeners must also reject a CA-validating
  client that presents no certificate. Certificate issuance occurs on the
  slightly slower FreeBSD clock to avoid a not-yet-valid race, private keys use
  restricted temporary directories, and all certificate material is removed
  during cleanup. Initial convergence failures now retain bounded unfiltered
  native logs so TLS errors are not accidentally hidden by a message-name
  allowlist.

- Added a planned-maintenance mode to the real Kea HA matrix. The survivor must
  accept `ha-maintenance-start`, enter `partner-in-maintenance`, and own the
  primary scope while the primary must enter `in-maintenance` with no scope
  before its processes may stop. The survivor must then enter `partner-down`,
  serve outage traffic, synchronize the restarted primary, and resume normal
  replication. The tested Linux/FreeBSD package pair clears the survivor scope
  after shutdown when `auto-failover` is false, so the continuous-service
  maintenance profile explicitly enables it and documents that requirement.

- Added bidirectional automatic-failover coverage to the cross-host Kea HA
  matrix. The isolated test enables native `auto-failover`, uses a zero
  unacknowledged-client threshold so bounded peer-channel loss immediately
  selects `partner-down`, and requires the survivor to own the primary scope
  without an administrative `ha-scopes` command. Outage allocation, primary
  resynchronization, restored replication, cleanup, and both Linux/FreeBSD role
  assignments use the same assertions as the separately retained manual path.

- Extended the guarded Kea HA takeover matrix through safe primary recovery.
  The survivor relinquishes its manually assigned scope before the stopped
  primary restarts, accepting a bounded service gap instead of risking two
  responders. The restarted memory-only primary must resynchronize the leases
  issued during its outage, both members must return to their normal
  hot-standby roles, and a distinct third-client dual-stack allocation must
  replicate to both databases. The complete recovery proof runs with Linux and
  FreeBSD in both roles.

- Extended the cross-host Kea HA matrix through guarded manual takeover. After
  proving dual-stack replication, the harness stops and reaps both primary
  daemons, requires the survivor to report interrupted communication and the
  partner unavailable, and only then assigns the stopped primary's scope with
  local `ha-scopes` commands. Distinct second-client DHCPv4 and DHCPv6 leases
  must appear on the survivor before the operating-system roles are reversed.
  Automatic failover remains deliberately disabled, and the documentation
  preserves the split-brain and pair-wide transaction boundaries.

- Added a real cross-host Kea HA integration test on the guarded sterile VLAN.
  Packaged Linux and FreeBSD Kea servers establish restricted dedicated
  hot-standby listeners for DHCPv4 and DHCPv6, complete initial synchronization,
  activate the primary service scopes, and replicate real client leases into
  both memory-only databases. The workflow reverses the operating-system roles
  and repeats the proof, rejects management interfaces, bounds every wait, and
  removes all aliases, processes, sockets, and helpers through its cleanup trap.
  Documentation distinguishes this native replication proof from the still
  unimplemented pair-wide dangd transaction.

- Required coherent Kea HA active-peer health samples. The peer-information
  age and four traffic-monitoring counters must all be present as unsigned
  values, while the local time, remote time, and signed clock skew must be
  either all measured or all null. Partial native samples now fail closed
  instead of publishing misleading relationship state. The YANG contract,
  source documentation, deployment guidance, and portable regressions describe
  and enforce the same invariant; passive-backup remains unaffected.

- Extended Kea HA authority binding to the active partner. For hot-standby and
  load-balancing relationships managed by an active local member, native remote
  server name and role must match the singular configured active peer.
  Passive-backup status must omit the remote map as Kea specifies. Mismatched
  partner identity and fabricated passive-backup peers fail closed with empty
  output; portable tests cover each case and both native platform workflows
  retain their complete HA matrix.

- Bound Kea HA operational replies to the accepted hook configuration. Before
  publishing status, the collector now requires the native relationship count,
  mode, local server name, and local role to match the authoritative JSON at
  the same relationship position. Extra, missing, stale, or cross-member
  status fails closed at the HA model path and leaves output empty. Portable
  tests cover every identity field and relationship-count drift; native Linux
  and FreeBSD workflows retain full hot-standby and passive-backup coverage.

- Expanded `dang-kea-ha` with the stable active-peer health fields from Kea's
  native `status-get` reply: peer-information age, analyzed packets, connecting
  and unacknowledged client counts, the remaining partner-down threshold, UTC
  clock samples, and signed clock skew. Counters are range-checked, malformed
  types fail closed, and Kea's null not-yet-measured clock values are omitted.
  Portable tests cover values, nulls, and invalid counters; native Linux and
  FreeBSD HA workflows require the packaged daemons to publish the counters.

- Completed the portable Kea HA status contract for passive-backup members.
  Kea legitimately omits the singular `remote` map in that mode because a
  primary may feed zero or several backups; the translator now publishes the
  required local state without fabricating a peer while continuing to reject a
  missing remote in load-balancing and hot-standby modes. Portable regression
  tests cover both branches, and the native Linux and FreeBSD workflows now
  apply, inspect, and roll back passive-backup DHCPv4 and DHCPv6 members in
  addition to their hot-standby coverage.

- Added modeled Kea local-member HA operational state. The new read-only
  `dang-kea-ha` module publishes DHCP family and relationship identity, HA mode,
  local role/state/scopes, and peer role/reachability/last-state/scopes from
  native `status-get` replies. Collection occurs inside the existing opening
  and closing configuration-authority checks, shares the complete query, item,
  byte, time, and XML budgets, and fails closed with a model path for malformed
  or unavailable status. Portable tests cover translation, XML escaping,
  malformed types, output clearing, and limits; native Linux and FreeBSD Kea
  3.2.0 workflows verify the same schema against real hot-standby
  members.

- Added explicit local-member Kea HA support and validation. HA hook parameters
  remain authoritative JSON in the pinned Kea model and are preserved exactly
  in native configuration. Portable tests verify nested relationship and peer
  translation. The Linux and FreeBSD workflows now load each platform's real
  `libdhcp_ha.so`, apply and reconcile DHCPv4 and DHCPv6 hot-standby member
  configurations, invoke the registered `ha-heartbeat` command, and roll back
  both daemons. Documentation distinguishes this supported one-dangd-per-member
  layout from unimplemented pair-wide atomic management and records the
  remaining pair-wide transaction and synchronization-progress gaps.

- Added real single-stack Kea support. The plugin now captures a stable target
  inventory containing DHCPv4, DHCPv6, or both, and all accepted state,
  mutation, reconciliation, operational, compensation, and rollback paths
  operate only on those targets. An unset socket variable disables its family;
  empty endpoints, an empty inventory, or retained configuration for a disabled
  family fail explicitly. Portable tests cover both one-family inventories and
  disabled-family rejection, while the Linux and FreeBSD native workflows run
  full IPv4-only and IPv6-only transactions. A deployment design documents the
  remaining model boundary for multiple instances and the stronger distributed
  transaction required for pair-wide Kea HA management.

- Bound Kea's pending-mutation marker to the exact proposed daemon images and
  accepted before-image. Apply now rejects stale baselines and any second
  mutation while prior hardware remains unresolved. Reconciliation rejects a
  changed proposal that was never applied and cannot use a different or no-op
  transaction to clear the marker; rollback likewise must own either the
  pending proposal or accepted transaction. Portable tests cover unapplied
  reconciliation, while native failure injection proves retry, no-op
  reconciliation, and no-op rollback all fail before contacting the lost
  daemon.

- Bound Kea ABI-v6 reconciliation to its prepared transaction. Before any live
  readback or authority promotion, the translated applied XML must exactly
  match the prepared proposed DHCPv4 and DHCPv6 images, including module,
  service, socket, and managed arguments. A stale, mismatched, or incorrectly
  paired callback now fails at the first owning module configuration path and
  leaves caller outputs cleared. A daemonless integration test proves this
  rejection occurs before control-socket access.

- Added authoritative Kea rollback readback. Successful compensation after a
  failed apply and successful explicit rollback now perform `config-get` on
  every changed daemon in reverse rollback order and require the complete
  managed prior dangd image to match before operational publication resumes. A
  missing, unavailable, or still-candidate daemon keeps the pending mutation
  marker set and reports the exact module configuration path. Portable tests
  cover full, selective, and drifted rollback readback; the native Linux and
  FreeBSD transaction matrices exercise successful readback against packaged
  daemons.

- Made Kea startup activation honor dangd as the definitive configuration
  authority. When dangd restores persisted intent from an initially empty
  datastore, the plugin captures each daemon's complete live configuration as
  the rollback image, applies the persisted snapshot, and verifies it through
  normal post-apply reconciliation. Direct restart recovery also reads both
  live daemon configurations before accepting persisted state, rejecting an
  unavailable or drifted DHCPv4/DHCPv6 image at its owning configuration path.
  Native Linux and FreeBSD tests cover empty-to-persisted apply and rollback
  plus drift rejection in both daemon directions.

- Delayed Kea operational-authority promotion until ABI-v6 post-apply readback
  succeeds. Hardware apply no longer makes candidate intent visible through
  the plugin before dangd accepts the commit. A marker installed before the
  first changing `config-set` suppresses concurrent reads through partial apply
  and remains set after incomplete compensation; reconciliation, complete
  compensation, or rollback resolves it against an atomic accepted snapshot.
  Operational collection retains the shared side of the authority lock through
  every native query, while mutations acquire the exclusive side before their
  first command, preventing an already-started read from overlapping apply.
  Native integration now proves suppression before reconciliation and
  publication after reconciliation on Linux and FreeBSD.

- Made every Kea callback output deterministic on failure. YANG-source,
  prepared-transaction, hardware-action, operational-data, and applied-state
  outputs are cleared before input validation, and every guarded invocation
  clears the caller's error descriptor before dispatch. Reused ABI structures
  can no longer expose stale pointers, counts, completeness flags, or error
  strings after either a rejected or successful call.

- Added a final exception barrier around every stateful integer-returning Kea
  plugin callback: YANG source retrieval, transaction preparation, validation,
  apply, rollback, hardware actions, operational retrieval, and applied-state
  reconciliation. Standard and unknown C++ exceptions become controlled plugin
  failures through a fixed thread-local error buffer instead of crossing the C
  ABI or allocating while the exception is handled.

- Applied the Kea operational-XML allowance during translation after every
  lease, statistics row, and reservation, then assembled the bounded fragments
  without chained temporary strings. Oversized modeled state is now stopped
  during construction instead of only after a complete document is allocated.

- Enforced dangd's 16 MiB operational-XML ceiling inside the Kea provider.
  DHCPv4 consumes from the complete document budget before DHCPv6 receives the
  remainder, so JSON escaping or binary-to-base64 expansion cannot produce an
  oversized plugin result for dangd to reject only after handoff.

- Bounded each authoritative Kea daemon-state read by shared limits of 512
  state queries, 65,536 state entries, and 8 MiB of encoded native entry data
  across lease pages, per-subnet statistics, and reservation pages. Those
  queries and both configuration-authority checks also share one 30-second
  deadline. A sequence of individually valid collectors can no longer
  multiply the complete operational request's resource allowance.

- Added continuous Kea configuration-authority checks to complete operational
  retrieval. Before and after collecting each daemon's multi-command state,
  the plugin now compares its live `config-get` image with the accepted dangd
  snapshot and fails at the owning module's `config` path on unavailability or
  drift. The closing check prevents publication when Kea changes during the
  read, and the configuration and subnet inventories are captured atomically.

- Added authoritative post-apply Kea configuration readback. ABI-v6
  reconciliation now uses `config-get` for every daemon changed by the
  transaction and requires all values translated from dangd's accepted
  snapshot to match. Kea-added defaults, response metadata, and object-list
  serialization order are ignored; missing, changed, or additional managed
  values fail with the owning module and configuration path.

- Added native Linux and FreeBSD rejection evidence for post-apply drift in
  both directions. Both Kea daemons first accept the candidate, then separate
  cases remove each daemon's managed host-command hook before ABI-v6 readback.
  Reconciliation must reject at the owning configuration path and hardware
  rollback must restore the complete before-image; the DHCPv6 case first
  completes DHCPv4 readback.

- Forced native DHCPv6 host-reservation retrieval across its 256-row page
  boundary on Linux and FreeBSD. The isolated workflow adds 256 reservations
  to the authoritative dangd candidate and requires the final second-page
  identifier and hostname in the complete modeled state.

- Forced native DHCPv6 operational retrieval across its 256-row lease page
  boundary on Linux and FreeBSD. The isolated workflow injects 257 leases and
  requires the final address, IAID, and exact assigned-NA counter, proving that
  a real packaged daemon supplies and the plugin publishes the second page.

- Extended the Linux and FreeBSD native Kea workflows with DHCPv4-only and
  DHCPv6-only transactions whose opposite unchanged image points at a
  nonexistent socket. Successful validation, apply, and rollback now provide
  packaged-daemon proof in both directions that selective transactions never
  contact the unchanged service.

- Added native Linux and FreeBSD coverage for explicit Kea rollback failure
  attribution. The isolated workflow removes the changed DHCPv6 daemon's UNIX
  socket after apply and requires rollback to report both its module and exact
  configuration instance path.

- Added native coverage for the outcome-unknown Kea apply path. After a
  successful `config-test`, the workflow removes the changed DHCPv4 daemon's
  socket before `config-set` and requires the apply error to identify its
  module and configuration path while retaining the failed compensation
  diagnostic.

- Added native validation-failure attribution coverage with both daemon sockets
  absent. A DHCPv6-only candidate must fail at the DHCPv6 module and exact
  configuration path without attempting the unchanged DHCPv4 image.

- Added native operational-failure attribution coverage in both directions.
  After no-op transactions, unavailable DHCPv4 and DHCPv6 sockets must fail
  complete state retrieval at their matching configuration paths. The DHCPv6
  case first completes all DHCPv4 state queries.

- Added native statistics-drift coverage in both directions. A configured
  subnet absent from either live daemon now fails complete retrieval at the
  owning module's `config` path; the DHCPv6 case first completes every DHCPv4
  state query.

- Added native host-state drift coverage in both directions by removing each
  Kea daemon's required host-command hook through its control API. Complete
  state retrieval must fail at the owning module's `config` path before state
  from that daemon is published; the following full transaction restores both
  hooks.

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

- Made the Kea stalled-peer deadline fixture scheduler-tolerant. The peer now
  remains stalled behind an explicit release for up to 30 seconds, while the
  client must still report its five-second transport timeout within a generous
  ten-second wall-clock bound. Successful runs release the peer immediately,
  eliminating the fixed six-second test delay and its one-second race margin.

- Matched system-ordered Kea configuration lists by their pinned YANG keys and
  system-ordered leaf-lists by value. Native Kea reservation and leaf-list
  reordering no longer creates false drift, while invalid or duplicate keys,
  duplicate values, and missing, additional, or changed entries fail closed.
  User-ordered subnet, pool, prefix-pool, and client-class lists still require
  exact order, as do arrays inside arbitrary JSON values.

- Made the Kea adapter test's synthetic control replies construct their
  optional JSON result explicitly, avoiding ambiguous-conversion warnings with
  the nlohmann JSON version packaged by the supported Linux environment.

- Failed complete Kea operational retrieval when the daemon reports no
  statistics for an exact subnet present in dangd's accepted configuration.
  Native configuration drift can no longer masquerade as a complete empty
  `lease-stats` tree; Linux and FreeBSD workflows exercise the mismatch.

- Added module and configuration-instance-path attribution to explicit Kea
  rollback failures. Rollback now shares the transaction pairing checks used
  by apply, skips unchanged daemons, continues restoring earlier changed
  daemons after an error, and reports every restoration failure while naming
  the first failed module structurally.

- Limited Kea `config-test`, `config-set`, and reverse rollback calls to daemon
  images whose translated configuration actually changed. A DHCPv4-only or
  DHCPv6-only commit no longer rewrites or compensates the unaffected service,
  while changed targets retain their established ordering and outcome-unknown
  restoration guarantees.

- Enforced unsigned parsing and exact modeled width for directly typed Kea
  integer leaves. Negative, overflowing, partially parsed, or malformed values
  can no longer fall through to signed numbers or strings; DHCPv4 and DHCPv6
  option `code` widths are selected from the module namespace.

- Rejected non-YANG text in modeled Kea boolean configuration leaves instead
  of silently changing malformed values to native strings.

- Rejected malformed and non-finite text in Kea decimal64 configuration leaves
  instead of silently changing the value to a native string.

- Preserved an explicitly empty DHCPv6 `server-id` presence container as an
  empty JSON object. It no longer acquires empty-string scalar semantics when
  all of its optional children are absent.

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

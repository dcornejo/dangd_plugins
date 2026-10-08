# Kea deployment topology

This document separates the deployment shapes the Kea plugin supports today
from designs that still require additional model or distributed-transaction
work. Dangd remains the definitive authority for every supported shape.

## Supported target inventory

The plugin captures one immutable target inventory when its shared library is
loaded. One plugin process permits one DHCPv4 target, one DHCPv6 target, or one
of each:

- `DANG_KEA_INSTANCE_ID` assigns the process boundary a stable identity and
  defaults to `default` for an existing single-instance deployment;
- `DANG_KEA_DHCP4_SOCKET` enables the local DHCPv4 daemon;
- `DANG_KEA_DHCP6_SOCKET` enables the local DHCPv6 daemon;
- `DANG_KEA_HOOK_DIRECTORY` optionally maps every modeled hook-library
  basename into the local package's absolute hook directory;
- an unset variable disables that family; and
- a present but empty variable is invalid rather than another spelling of
disabled.

Set `DANG_KEA_HOOK_DIRECTORY` on every member of a cross-platform pair so one
authoritative peer image remains portable across package layouts. Linux uses
`/usr/lib/x86_64-linux-gnu/kea/hooks` in the validated ISC packages and
FreeBSD uses `/usr/local/lib/kea/hooks`. The mapping is plugin-owned local
deployment state: the YANG image retains logical library basenames, the plugin
performs the rewrite, and dangd remains unaware of Kea or operating-system
paths.

Each enabled family must have exactly one corresponding top-level Kea
configuration container. A disabled family must have none. This symmetric rule
prevents an operator from retaining authoritative configuration that the
plugin would otherwise ignore. YANG Library continues to advertise both Kea
modules because the plugin implements both schemas; the host inventory decides
which modeled resource is currently deployable.

Accepted configuration, pending proposals, subnet inventories, operational
collection, compensation, and rollback are all indexed by the enabled target
order. The current order is DHCPv4 followed by DHCPv6 when both are enabled.
Single-stack operation therefore uses the same transaction machinery without
a placeholder or unreachable target for the absent family.

Applied-state reconciliation also queries each enabled daemon's native
`list-commands` inventory. The daemon must advertise the core configuration
and version commands, its family-specific paged lease and statistics commands,
the reservation paging command, and `status-get` when the accepted image loads
the HA hook. A hook pathname in configuration is not treated as proof that the
library loaded and registered successfully. Missing or malformed capability
inventories prevent that daemon from becoming authoritative.
The read-only instance tree records the strict three-component version returned
by each accepted daemon's native `version-get` command. This lets automation
verify the implementation behind an endpoint instead of inferring it from a
package name or host role. A malformed reply or a version older than the
supported Kea 3.2.0 baseline prevents reconciliation; a valid version is
identity evidence, while the daemon's `config-test` remains the final
compatibility check for a particular candidate.

## Multiple local instances

Multiple local instances are supported as separate dangd process boundaries.
The official Kea YANG modules each describe one top-level server, so repeating
a module root in one datastore would be invalid. Each process instead owns one
independent datastore and manages at most one daemon of each address family.
This preserves the native model without an RFC 8528 schema-mount dependency or
an ambiguous rule for sharding global Kea settings.

Every process must have all of the following unique values:

1. `DANG_KEA_INSTANCE_ID` (for example, `access-east` or `guest-west`);
2. every enabled Kea UNIX control-socket path;
3. the dangd `--state` file and initial `--config` file;
4. the NETCONF listening address/port combination; and
5. service-manager identity, PID tracking, and writable runtime directory.

The root model, plugin binary, module search path, NACM seed, host keys, and
trust anchors may be shared when site policy permits, but private-key access
must still follow the service account's normal protections. Configure each Kea
daemon with the same UNIX socket named in its corresponding process environment
and keep its native configuration, PID file, lease file, and HA listener
separate from every other local instance.

At runtime, read `/kea-instance` from the `urn:dang:kea:instance` namespace.
It returns the stable instance ID plus `dhcpv4`, `dhcpv6`, or both in the exact
transaction order. An unset ID reports `default`; an explicitly empty,
overlong, slash-containing, or otherwise non-portable ID prevents plugin
transactions and operational publication. The ID is not inferred from array
position or a socket pathname.

Use a service-manager template or one explicit service definition per instance.
For example, the `access-east` service environment can name
`DANG_KEA_INSTANCE_ID=access-east`, `/var/run/kea/access-east-4.sock`, and
`/var/run/kea/access-east-6.sock`, while its dangd command names
`/var/lib/dangd/access-east/state.json` and a unique NETCONF port. A second
service repeats that pattern with a different identifier and paths. Run each
complete command with `--check` before enabling either service, then query the
instance tree on both NETCONF endpoints before making a configuration change.

Transactions are atomic only inside one process boundary. There is no
cross-instance prepare/commit coordinator, so an operation spanning two local
instances must treat them as independent NETCONF servers and define its own
failure recovery. Schema mount and logical sharding remain possible future
deployment models, not implied behavior of this implementation.

The isolated Linux and FreeBSD validation workflows enforce this boundary with
two live DHCPv4 daemons. Independently identified plugin processes reconcile
and collect state from them concurrently. The test then stops only the
secondary daemon, requires a module- and path-attributed control-socket error
from that instance, and verifies that the primary instance still reconciles
and serves operational state. This is an executable isolation proof; it does
not add distributed atomicity.

## Kea HA pairs

There are two distinct HA support levels.

The first is local-member support, which is implemented: one dangd manages one
local Kea daemon whose ordinary Kea configuration includes the HA hook and peer
parameters. The official model represents the hook `parameters` as a JSON-valued
leaf. The plugin parses that leaf as JSON, preserves the complete relationship
and peer structures during translation and reconciliation, and lets Kea's
native `config-test` enforce HA-specific semantics. The local member continues
to use the plugin's UNIX control path. Each peer has its own dangd and no
pair-wide atomicity is claimed.

The platform package must provide both `libdhcp_lease_cmds.so` and
`libdhcp_ha.so`. The peer URLs, certificates, roles, and each member's
`this-server-name` remain part of authoritative Kea configuration. Follow the
[Kea 3.2 HA hook documentation](https://kea.readthedocs.io/en/kea-3.2.0/arm/hooks.html)
when constructing that JSON.

Native Linux and FreeBSD tests load the packaged HA hook for DHCPv4 and DHCPv6,
apply and read back hot-standby and passive-backup local-member images, call
`ha-heartbeat` through the managed UNIX socket, retrieve modeled status, and
restore the non-HA image. An unreachable hot-standby peer is published with
false reachability. Passive-backup has no singular active peer, so Kea omits
the native `remote` map and `dang-kea-ha` correspondingly omits that container.
This proves local hook lifecycle, command support, and status translation, not
replication or failover.

A separate sterile-VLAN interaction now proves native hot-standby protocol and
lease replication between the packaged Linux and FreeBSD servers. It uses
Kea's restricted dedicated HA listeners, never an unauthenticated general
management socket. Both members must reach `hot-standby`; the active primary
must hold its service scope; and one real DHCPv4 and DHCPv6 allocation must
appear in both local lease databases. The test then destroys the pair, reverses
the operating-system roles, and repeats the same proof. In each phase it also
stops both primary daemons and proves their exit before changing scopes. The
survivor must report communication interruption and the partner unavailable;
only then does the test issue `ha-scopes` over the survivor's local UNIX
socket, allocate a distinct DHCPv4 and DHCPv6 client, and require both new
leases locally. Before restarting the former primary, the harness removes the
survivor's manually assigned scope and proves that removal. This intentionally
accepts a bounded service gap rather than allowing both members to answer the
same scope. The empty memory-only primary then restarts, both members must
return to normal hot-standby, and the former primary must recover the leases
issued during its outage. A distinct third-client DHCPv4 and DHCPv6 allocation
must subsequently replicate to both members. This validates the peer data
plane, replication, guarded manual takeover, rejoin synchronization, and
post-recovery replication in both directions. This manual mode does not turn
two independently managed dangd instances into one atomic transaction.

The same harness also has an explicit `automatic` mode. On the sterile VLAN it
enables Kea's native `auto-failover` setting and sets `max-unacked-clients` to
zero. After the primary processes have exited, the survivor must enter
`partner-down` and acquire the primary scope without an administrative
`ha-scopes` command. It must serve the second client, synchronize the restarted
primary, return to normal hot-standby, and replicate the third client. Both
operating-system role assignments are repeated. This setting is intentionally
specific to a network where loss of the dedicated peer channel is accepted as
proof that the peer is down. A production deployment exposed to partitions
must select failure-detection thresholds for its topology and traffic; copying
the test value without that analysis can make both sides enter `partner-down`.

The automatic proof removes the previous native-failover test gap. It still
does not turn two independently managed dangd instances into one atomic
transaction.

For planned work, the harness also provides a `maintenance` mode. It sends
`ha-maintenance-start` to the member that will stay online and does not stop
the other member until both address families report the coordinated states:
the survivor is `partner-in-maintenance` with the primary scope, while the
maintained primary is `in-maintenance` with no scope. After shutdown, the
survivor must enter `partner-down`, retain the primary scope, and serve the
second client. Restart, resynchronization, normal hot-standby, and third-client
replication use the same requirements as the failure tests.

The tested Linux and FreeBSD packages clear the survivor's service scope after
the maintained peer exits if `auto-failover` is disabled, despite completing
the maintenance handshake. The harness therefore enables `auto-failover` for
the continuous-service maintenance profile. This is a recorded interoperability
constraint of the tested versions, not a claim that the handshake failed: both
peers reached the expected pre-shutdown maintenance states. Operators should
validate their exact Kea versions and failure thresholds before relying on a
maintenance procedure.

The HA+MT peer transport is also validated with mutual TLS. The harness creates
a one-day disposable CA and separate Linux and FreeBSD certificates containing
their documentation-prefix IP subject alternative names. Each peer uses HTTPS,
validates the other certificate against that CA, presents its own certificate,
and sets `require-client-certs` so the dedicated listener rejects an
unauthenticated client. Before DHCP traffic begins, a CA-validating client that
presents no certificate must fail against both address-family listeners from
both hosts. A client presenting a validly formed certificate from an unrelated
disposable CA must also fail, proving that merely presenting a certificate is
insufficient. Finally, plaintext HTTP sent to each HTTPS port must not receive
an HTTP response, guarding against an accidental cleartext listener. The full
automatic-failover and recovery matrix then runs over the authenticated
channels. Certificate material is stored only in mode-0700 temporary
directories, private keys are mode 0600, and both hosts and the orchestrator
remove all artifacts through the same cleanup trap used for processes and
addresses.

Certificate issuance intentionally runs on the slower FreeBSD test clock. A
certificate created on a peer whose clock is tens of seconds ahead may still
be `not yet valid` on the other host even when both clocks are within ordinary
operational tolerance. Production deployments should use synchronized clocks,
durable certificates from their normal PKI, protected private keys, and their
usual rotation and revocation procedures; the disposable test CA is not a
deployment pattern.

The pinned ISC modules do not expose HA relationship status. The companion
`dang-kea-ha` module fills that model gap without changing their configuration
schema. Its read-only `high-availability/relationship` list is keyed by address
family and Kea's zero-based relationship position, and publishes mode, local
name/role/state/scopes, and remote name/role/reachability/last-state/scopes.
For active-peer modes it also publishes peer-information age, the native
traffic-monitoring counters, and, once measured, both UTC samples and signed
peer-minus-local clock skew. Nullable clock fields remain absent until Kea has
calculated them. Age and the four traffic-monitoring counters are one required
sample. The local UTC sample, remote UTC sample, and signed skew must likewise
be all measured or all null. Any partial set fails closed rather than exposing
an internally inconsistent relationship.
Remote status is present for load-balancing and hot-standby and absent for
passive-backup, matching Kea's native response contract.
The collector also requires the reported relationship count and mode plus the
local and singular active-remote server names and roles to match the accepted
HA hook parameters before it publishes any entry. Passive-backup must omit the
remote map. This prevents stale status or a response from another member or
partner from being attributed to the authoritative dangd configuration.
Each accepted relationship also supplies an effective transport-security
posture. The model reports plaintext or TLS independently for the local
dedicated listener and the singular active-remote channel, the local TLS
client-certificate requirement, and dedicated-listener command restriction.
Kea selects TLS from a complete `trust-anchor`, `cert-file`, and `key-file`
triplet. Relationship-level values are inherited unless a peer overrides them,
and empty peer strings disable inheritance. The adapter resolves those rules,
fails closed on a partial effective triplet, and never exposes file paths.
These leaves describe accepted policy; `in-touch` and the other live status
leaves remain the evidence that the peer channel is actually operating.
The numeric relationship ID is stable only until that daemon's accepted HA
configuration changes; use the reported server names for operator-facing
identity. Collection occurs between the same two complete configuration checks
as lease state and fails closed if the native status is missing or malformed.
Kea's portable status reply does not contain a synchronization percentage, so
that detail remains unavailable.

The second is pair-wide management: one logical commit controls both peers.
It is now connected to dangd's normal NETCONF commit path through the tested,
transport-neutral
[peer transaction coordinator](https://github.com/dcornejo/dang/blob/main/docs/PEER_TRANSACTIONS.md)
that prepares every peer before mutation, applies standbys before the primary,
verifies the group, establishes a durable decision boundary, reverses
pre-decision cancellation, and resumes post-decision confirmation. The Kea
plugin exports ABI-v9 plan and verification callbacks for that generic
contract. Dangd also has the private
crash-safe journal and distinguishes a definite pre-decision failure from an
unknown post-replacement outcome. Configured application startup and `SIGHUP`
reload now load and validate that record, then fail closed with a token-free
pending-peer summary. Dangd also has a complete programmatic NETCONF/TLS
participant that verifies mutual-TLS identity and hostname, checks candidate,
validate, and persistent confirmed-commit capabilities before sending an RPC,
bounds individual I/O waits and message sizes, correlates every reply, and
safely serializes persistent commit tokens. It invokes no shell command.

The private `--peer-recovery` version 2 file maps each exact generic
`(group-id, participant-id)` pair to a host, port, client credentials, trust
anchor, and per-I/O timeout. Dangd owns the canonical
`group-id/participant-id` journal identity; participant names may repeat in
different groups, while duplicate pairs and legacy participant-only version 1
mappings fail closed. Startup and reload validate the complete mapping,
including safe ownership and permissions, before accepting it. Relative
credential paths resolve from the mapping file.

Startup and reload now use the validated mapping to retry only pending
confirmations, atomically persist each acknowledgement, and remove the journal
before serving requests. A failed peer leaves the reduced journal in place and
blocks startup; recovery never contradicts durable COMMIT by cancelling. An
exclusive private sibling lock prevents concurrent daemons from replaying the
same journal.

This completes the generic transport, recovery, provider planning,
single-group execution, and production commit path for a locally owned primary.
Dangd's generic controller resolves every remote target before session
construction, creates independent cryptographic persistent commit tokens,
routes authenticated readback to only the contributing plugin verifiers,
creates the crash-safe journal, and invokes the coordinator. The stateful
participant locks candidate, reads running, replaces only the contributed
module namespaces, transfers the reconstructed complete configuration with
`copy-config`, validates it, starts a persistent
confirmed commit, retrieves both running configuration and operational state
for supplied health callbacks, confirms or reconnects to cancel, and discards,
unlocks, and releases candidate/session resources. A verifier can distinguish
pending convergence from permanent rejection; dangd obtains fresh authenticated
readback until success or the original deadline. Live two-peer commit and live rollback tests
cover that generic adapter. The Kea plugin translates a shared hot-standby
change into complete per-member module images and supplies a strict pair-health
callback that proves the expected roles, relationship, scopes, and peer
freshness. All managed families must use one two-member roster. Load-balancing
and passive-backup deliberately publish no coordinated plan until their
distinct scope policy is implemented. A peer with
an uncertain outcome must retain the transaction's pending marker; another
transaction or a no-op callback must not clear it.

`VerifyPeerTransactionReplies` now implements that strict Kea-side health
decision once the controller supplies the complete proposed daemon images and
policy. It compares the authenticated running reply with every proposed DHCPv4
and DHCPv6 image, binds each operational relationship to the configured mode
and member identities, and requires exact stable states and scopes. An active
remote must be in touch, uninterrupted, and no older than the caller's limit.
The helper rejects stale, disconnected, drifted, duplicate, malformed,
DTD-bearing, and cross-member replies. ABI v9 invokes it with an opaque member
identity that the plugin validates against the retained prepared plan. The
normal commit path now supplies the safe production lifecycle: participant
commits cannot recursively initiate another peer plan, the local snapshot is
durable before the distributed COMMIT decision, and a commit affecting several
peer groups fails before mutation rather than committing groups sequentially.
A transport or permanent health-verification failure cancels every attempted remote
confirmed commit before local apply, sends no confirmation, removes a fully
cancelled PREPARED journal, and leaves local running unchanged. Native Debian
and FreeBSD package installation and packaged-plugin loading are validated.
The guarded cross-platform interaction makes Linux and FreeBSD primary in turn,
commits `valid-lifetime` 601 through ordinary NETCONF, proves both daemon images
changed, stops the remote dangd endpoint, and proves the next proposal is
rejected without changing either image. The same phases retain DHCPv4 and
DHCPv6 service, replication, automatic failover, and recovery. The guarded
`dangctl --edit-config` workflow now provides the operator-facing path: it
submits a complete or partial Kea `<config>` document through standard
candidate lock, edit, validation, commit, and unlock operations. All Kea
planning and verification remains in this plugin behind ABI v9; the client and
dangd core contain no Kea-specific branch.

The implemented production pair controller treats the peers as one transaction
group:

1. translate the authoritative candidate into explicit per-peer images;
2. validate every image without mutation;
3. apply persistent confirmed commits in a role-aware order that preserves
   service;
4. read back every peer and verify HA health;
5. durably record the group COMMIT decision before confirming any member;
6. before that decision, cancel every attempted apply in reverse order and
   retain unresolved state if any peer cannot prove restoration; and
7. after that decision, retry pending confirmations without attempting a
   contradictory rollback.

The controller does not permit a degraded pair to accept a commit. This
fail-closed policy is deliberate: there is no exception mode that could
silently overwrite a returning peer.

The participant whose `DANG_KEA_INSTANCE_ID` matches its roster name is marked
local through ABI v9. The currently supported initiation point is that pair's
primary; all remote standbys are verified before the outer local apply. Sending
the same change to a standby endpoint fails before mutation with
`peer-local-standby-unsupported`. This limitation is generic dangd ordering
policy and does not add Kea-specific logic to the daemon.

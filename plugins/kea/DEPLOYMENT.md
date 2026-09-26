# Kea deployment topology

This document separates the deployment shapes the Kea plugin supports today
from designs that still require additional model or distributed-transaction
work. Dangd remains the definitive authority for every supported shape.

## Supported target inventory

The plugin captures one immutable target inventory when its shared library is
loaded. It currently permits one DHCPv4 target, one DHCPv6 target, or one of
each:

- `DANG_KEA_DHCP4_SOCKET` enables the local DHCPv4 daemon;
- `DANG_KEA_DHCP6_SOCKET` enables the local DHCPv6 daemon;
- an unset variable disables that family; and
- a present but empty variable is invalid rather than another spelling of
  disabled.

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

## Multiple local instances

Multiple daemons of one address family are not yet supported. The internal
target inventory is intended to grow stable instance identifiers, but the
official Kea YANG module still describes one top-level server configuration.
Repeating that module container in one datastore would violate the schema and
cannot be used as an instance selector.

Before multiple instances are enabled, dangd needs one explicit model-boundary
choice:

1. a deployment model whose instance list uses YANG schema mount for each Kea
   server;
2. a defined sharding model that derives several native daemon images from one
   logical Kea configuration; or
3. separate dangd processes and datastores, one for each Kea instance.

Schema mount preserves the native Kea model most directly but adds an RFC 8528
dependency. Sharding is appropriate only if ownership of global settings,
subnets, and shared networks can be specified without ambiguity. Separate
processes are operationally simple but do not provide one atomic configuration
transaction across instances. The plugin must not assign implicit array
positions or socket names as durable instance identity.

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
leases locally. This validates the peer data plane, replication, and guarded
manual takeover in both directions, but it does not test automatic failover or
turn two independently managed dangd instances into one atomic transaction.

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
The numeric relationship ID is stable only until that daemon's accepted HA
configuration changes; use the reported server names for operator-facing
identity. Collection occurs between the same two complete configuration checks
as lease state and fails closed if the native status is missing or malformed.
Kea's portable status reply does not contain a synchronization percentage, so
that detail remains unavailable.

The second is pair-wide management: one logical commit controls both peers.
That is not implemented. It requires authenticated remote control, stable peer
identity, role-aware ordering, a prepare result from every required peer,
post-apply pair-health verification, and durable recovery when a reply is lost
or one peer becomes unreachable. A peer with an uncertain outcome must retain
the transaction's pending marker; another transaction or a no-op callback must
not clear it.

A future pair controller should treat the peers as one transaction group:

1. translate the authoritative candidate into explicit per-peer images;
2. validate every image without mutation;
3. record the complete group proposal durably;
4. apply in a role-aware order that preserves service;
5. read back every peer and verify HA health before accepting the commit; and
6. compensate in reverse order, retaining unresolved state if any peer cannot
   prove restoration.

The controller must also define whether a degraded pair may accept a commit.
The safe default is no. Any exception needs an explicit policy, a surfaced
degraded outcome, and recovery behavior that cannot silently overwrite the
returning peer.

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
apply and read back a hot-standby local-member image, call `ha-heartbeat` through
the managed UNIX socket, and restore the non-HA image. The remote peer is
deliberately absent, so this proves local hook lifecycle and command support,
not replication or failover.

The pinned YANG modules do not expose HA relationship status. Peer liveness,
state-machine phase, service scopes, and synchronization progress are therefore
not yet available as modeled operational data. Adding that state requires a
documented augmentation rather than placing unmodeled JSON in NETCONF replies.

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

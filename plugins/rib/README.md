<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# RFC 8431 RIB plugin

This directory stages the exact `ietf-i2rs-rib` revision 2018-09-13 module from
RFC 8431 and its `ietf-interfaces` revision 2018-02-20 dependency. The RFC 6991
`ietf-inet-types` and `ietf-yang-types` dependencies are supplied by dangd.

The source files are unmodified copies from the IETF YangModels RFC registry:

- `ietf-i2rs-rib@2018-09-13.yang` SHA-256
  `3a76977e6c6d72f001d1ccd83ea5bb08a6a0cdc3f6d417c84be6234a2f9800d7`;
- `ietf-interfaces@2018-02-20.yang` SHA-256
  `f6faea9938f0341ed48fda93dba9a69aa32ee7142c463342efec3d38f4eb3621`.

At this checkpoint the files are installed and independently compiled, but no
plugin advertises `ietf-i2rs-rib` yet. Runtime ownership waits for transaction,
RPC, notification, and Linux/FreeBSD backend semantics. In particular, simply
accepting the writable tree without programming and observing the RIB would be
a false implementation claim.

The runtime foundation currently parses destination-prefix IPv4 and IPv6
routes whose base nexthop is a gateway, an outgoing interface, or both. It
requires the RFC 8431 route preference and local-only fields, rejects source,
MPLS, MAC, interface-match, chained, replicated, protected, load-balanced, and
tunnel routes with an attributed model path, and computes replacements as an
old-route deletion followed by a new-route installation. Separate Linux `ip`
and FreeBSD `route` argv planners require numeric RIB/FIB names and never invoke
a shell. They do not execute commands yet, so the module remains unadvertised.

Numeric names are an intentional temporary variance: RFC 8431 RIB names are
arbitrary strings, while Linux policy tables and FreeBSD FIBs need an explicit
platform mapping. A future plugin option must supply that mapping before this
restriction can be removed safely.

Tests must use Linux network namespaces or FreeBSD VNET jails with only
disposable loopback/epair interfaces. They must never add, remove, or replace a
route on a host LAN interface or in the host's default routing table.

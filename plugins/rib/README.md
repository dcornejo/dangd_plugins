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

Tests must use Linux network namespaces or FreeBSD VNET jails with only
disposable loopback/epair interfaces. They must never add, remove, or replace a
route on a host LAN interface or in the host's default routing table.

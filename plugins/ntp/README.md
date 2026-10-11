<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# RFC 9249 NTP plugin

This directory contains the pinned standards foundation and platform-neutral
configuration planner for a separately packaged `ietf-ntp@2022-07-05`
provider. The loadable plugin and native Linux/FreeBSD adapters are not
implemented yet; installing the current source tree does not advertise
RFC 9249 support.

RFC 9249 defines a top-level `/ntp` presence container for NTPv4 and NTPv3
compatibility.  It covers unicast, broadcast, multicast, and manycast modes;
symmetric-key authentication; ACL references; interface configuration; clock
and association state; statistics; and the
`statistics-reset` RPC.  Each feature will be advertised only after its native
configuration, operational projection, rollback, and platform tests exist.

## Model provenance

The files in `models/` are unmodified IANA-published modules:

- `ietf-ntp@2022-07-05.yang`, SHA-256
  `f49f1c9a6bcb3c06950f5beecbb90a4aced98bfd6195f009382b35583a8dc16d`;
- `ietf-access-control-list@2019-03-04.yang`, SHA-256
  `d32d1c4c8a4bef34d86295d78aa9aadb888a7a0c38b56048f71fea4b1962d2f1`;
- `ietf-packet-fields@2019-03-04.yang`, SHA-256
  `312a4b58a55714411ff9a3a36088be3aeaabe720fb663a56fea70fa00b4bcd03`;
- `ietf-ethertypes@2019-03-04.yang`, SHA-256
  `952dcb78026852e77ef01a7acdc38aab2bae81f7c4142a2dbd862c640c23cc20`;
- `ietf-routing-types@2017-12-04.yang`, SHA-256
  `45b2207ec63173760e10e1dd5a9d6023ff03a0427904208fe86c0bc747df59a1`.

The complete import closure also uses the pinned `ietf-system`,
`iana-crypt-hash`, and `ietf-interfaces` sources already shipped by the system
and interface providers, plus dangd's pinned `ietf-inet-types`,
`ietf-yang-types`, and `ietf-netconf-acm` library modules.  The CMake schema
test passes those locations explicitly and loads `ietf-system` as implemented,
which checks RFC 9249's mutual-exclusion constraint against RFC 7317 NTP.

## Ownership and migration contract

RFC 9249 `/ntp` and RFC 7317 `/system/ntp` are intentionally alternative
configuration surfaces.  Both ultimately control the same daemon, native
configuration files, sockets, and clock discipline.  The existing system
provider therefore claims dangd's generic exclusive `ntp` resource domain.
The future RFC 9249 provider must claim the same domain.  Loading both is a
configuration error detected by dangd's ordinary plugin contract; dangd needs
no knowledge of either module.

Migration is an explicit operator action:

1. read and archive the current `/system/ntp` configuration;
2. remove `/system/ntp`, validate, and commit while the system provider is
   still loaded;
3. stop dangd, replace the system package or load set with the RFC 9249
   provider, and run `dangd --check`;
4. translate the archived intent to `/ntp`, validate it, and commit;
5. verify `/ntp/clock-state`, associations, and statistics before declaring
   the migration complete.

There is deliberately no automatic translation: the models have different
feature sets and translating an unsupported mode or authentication choice
silently would violate dangd's fail-closed transaction policy.

## Configuration and transaction-planning boundary

`src/ntp_config.cc` parses complete datastore snapshots without reading or
changing host state. It currently projects the `/ntp` presence state, listener
port, local reference-clock stratum, non-deprecated symmetric authentication
keys, access-rule bindings, and unicast server/peer associations. YANG defaults
are explicit, identity prefixes are normalized, system-ordered lists are
canonicalized, key references are checked, and malformed or augmented content
fails with an attributed RFC 9249 path. The deprecated cleartext `keystring`
case is deliberately rejected; only the hexadecimal representation and
non-deprecated HMAC-SHA-2 or AES-CMAC identities are accepted.

The planner compares two parsed snapshots and emits deterministic English
actions plus dependency edges. It never includes key bytes in descriptions.
Service shutdown precedes destructive association removal, old associations
precede obsolete key replacement, authentication/access/port prerequisites
precede new associations, and service startup depends on every configuration
step. These are plans only: they make transaction ordering testable before a
daemon-specific adapter is allowed to act.

Broadcast, multicast, and manycast interface modes currently fail closed at
`/ietf-ntp:ntp/interfaces`. They require explicit cross-daemon mappings before
the parser accepts them. VRF behavior is also still pending because the base
RFC 9249 module does not contain a VRF node; any supporting augmentation must
be selected and pinned rather than inferred.

## Development and validation

Run the schema test through CTest after configuring against a dang checkout:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DDANGD_ROOT=/path/to/dang
cmake --build build
ctest --test-dir build -R rfc9249 --output-on-failure
```

The next implementation increment must wrap the parser and planner in the
loadable plugin transaction lifecycle without advertising unimplemented
features. Linux and FreeBSD adapters must then implement acknowledged apply,
retained before-image rollback, NMDA operational readback, and isolated tests.
Programmatic daemon or service-manager APIs are required where the platform
supplies them; a process boundary is acceptable only when no supported API
exists and must use an absolute executable path and fixed argument vector
without a shell.

<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# RFC 9249 NTP plugin

This directory contains the pinned standards foundation, platform-neutral
configuration planner, and development-stage loadable target for an eventual
separately packaged `ietf-ntp@2022-07-05` provider. Native Linux/FreeBSD
adapters are not implemented yet; installing the current source tree does not
install this target or advertise RFC 9249 support.

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
The development RFC 9249 provider claims the same domain.  Loading both is a
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

`dangd_ntp_plugin` wraps this layer in ABI v7 for development and contract
testing. It returns `ietf-ntp` as the implemented source and the pinned RFC
8519/RFC 8294, `ietf-interfaces`, and `ietf-system` closure as import-only
sources, claims the exclusive `ntp` resource domain, retains complete
before/proposed projections during prepare, validates the dependency graph,
and publishes every planned step through the generic hardware-action
interface. Deactivation and activation steps use the corresponding generic
action classes, and all errors retain an RFC 9249 path.

The development target advertises only `ntp-port`, `authentication`,
`hex-key-string`, and `unicast-configuration`. In particular,
`access-rules` stays disabled even though its syntax can be parsed: dangd does
not yet have an RFC 8519 ACL datastore owner, so enabling that feature would
offer a leafref target users cannot manage. Deprecated authentication and all
interface-mode features also remain disabled.

Apply, rollback, and ABI-v5 operational callbacks deliberately return an
attributed `... unavailable` error. Linux validation additionally translates
the proposed tree to complete managed chrony file images, so daemon-specific
semantic gaps fail before any hardware action can run. The operational
callback exists because a complete ABI-v7 implementation must provide it, but
it never fabricates clock state. This is a safety boundary, not a mock backend.
The target is not part of any install or package component until both supported
operating systems have acknowledged apply, readback, and rollback
implementations.

## Native adapter and daemon selection contract

The development plugin now creates one typed native adapter per prepared
transaction. The adapter receives immutable before/proposed projections and
one dependency-ordered action at a time. Each action has a retained checkpoint
for its native before-image and acknowledgement state. A production adapter
must capture reversible state before mutation, set `applied` only after the
daemon or service manager acknowledges the change, compensate only an
acknowledged checkpoint, and erase retained secrets when the transaction is
released. Operational data is returned through the same selected adapter and
must be a complete observed image rather than a projection of intended config.

Daemon selection is explicit through `DANG_NTP_DAEMON`:

- unset or `auto` chooses `chrony` on Linux and base `ntpd` on FreeBSD;
- `chrony` is accepted only on Linux;
- `ntpd` is accepted only on FreeBSD;
- every other value, including a daemon from the other platform, fails prepare
  at `/ietf-ntp:ntp` before any hardware action is available.

This is intentionally a narrow support declaration, not daemon autodetection.
It prevents an installed but unintended implementation from silently taking
ownership of the host clock. The Linux adapter now also renders a deterministic
chrony include fragment and separate key-file image. It maps listener port,
`local stratum`, 128- or 256-bit AES-CMAC keys, and
unicast server/peer associations. It normalizes colon-separated hexadecimal
keys without placing their bytes in errors. These semantics fail closed at
their model path:

- `source` on an association, because chrony's interface binding is global;
- `burst` or `iburst` on a peer, because chrony supports them only on servers;
- an association key unless authentication is enabled and the key is trusted;
- AES-CMAC material that is not exactly 128 or 256 bits;
- modeled HMAC identities, because chrony's SHA MACs use a different algorithm;
- `master-stratum` 16, because chrony's local reference supports 1 through 15;
- access rules until an RFC 8519 datastore owner and exact ACL mapping exist.

The generated key-file path is
`/etc/chrony/dangd-rfc9249.keys`; native installation will use mode 0600 and
must verify that the host configuration includes the managed fragment before
restart. Neither file is written yet. Linux and FreeBSD readiness flags remain
false, and apply, rollback, and operational readback still fail closed. Native
code must prefer daemon or service-manager APIs and must never invoke a shell.

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
cmake --build build --target dangd_ntp_plugin ntp_config_test \
  ntp_chrony_config_test ntp_plugin_contract_test
ctest --test-dir build -R rfc9249 --output-on-failure
```

`src/managed_file.cc` now supplies a tested per-file replacement primitive for
both POSIX platforms. It retains bytes, owner/group and permissions, atomically
renames a synchronized temporary file, synchronizes the directory, and can
restore either an existing file or absence. It refuses symlinks, hard links,
non-regular files and changes made outside its transaction. A directory-sync
failure after rename retains rollback state because the replacement is already
visible. Captured secret buffers are erased on release. This helper expects a
trusted existing parent directory and is not wired into daemon actions yet;
it does not provide crash recovery or atomicity across multiple files.

The next Linux increment is adapter integration for installation/removal of the two rendered
files, retained before-image rollback, and acknowledged chrony lifecycle
control over systemd D-Bus. FreeBSD still needs its base-ntpd translation.
Both adapters then need NMDA operational readback and isolated tests.
Programmatic daemon or service-manager APIs are required where the platform
supplies them; a process boundary is acceptable only when no supported API
exists and must use an absolute executable path and fixed argument vector
without a shell.

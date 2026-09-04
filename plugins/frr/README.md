<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# FRR native-model provider

This directory contains the in-progress provider that will manage FRRouting
through FRR's own YANG model family and the programmatic `mgmtd` frontend API.
It is intentionally separate from the RFC 8431 provider: deployments select
one routing owner, and both packages claim dangd's exclusive `routing` resource
domain.

The first implementation layer discovers one installed FRR YANG directory and
loads its sources with bounded reads. Linux normally installs that directory at
`/usr/share/yang`; FreeBSD packages normally use `/usr/local/share/yang`. These
locations are alternatives and are never overlaid, because mixing native
schemas from different FRR installations would publish a model set matching
neither daemon. The selected root's `modules/libyang` directory supplements it
with standard IETF imports packaged by libyang. The loader resolves a transitive
import closure and rejects missing imports, duplicate modules, malformed input,
and size-limit violations.

No loadable FRR plugin is installed by this checkpoint. Schema publication,
configuration validation, apply, rollback, operational data, and drift
reconciliation remain disabled until the `mgmtd` adapter can uphold dangd's
transaction contract. This prevents a schema-only component from falsely
advertising runtime support.

The transport foundation encodes FRR's public native frontend session, lock,
XML edit, validate, apply, abort, and unlock messages without requiring FRR's
development headers. This is a local ABI protocol: it uses host byte order and
natural C layout and is permitted only over the local `mgmtd_fe.sock` UNIX
socket. Frame length, protocol marker, reply correlation, message type, and
NUL-terminated error data must be checked before a reply reaches transaction
logic. The transport now connects nonblockingly with close-on-exec protection,
uses one monotonic deadline for the complete exchange, handles partial I/O, and
requires the response type plus request/session identifiers to match. Session
lifecycle orchestration enforces create, candidate lock, XML replacement,
validation, apply or candidate abort, unlock, and destruction in protocol
order. Transaction orchestration performs validation in a disposable session,
then repeats the replacement for a real apply. Rollback is a new validated
commit of the retained before-image; candidate abort is used only to discard
uncommitted edits. Wiring this orchestration into ABI v7 and extracting the FRR
subtrees from dangd snapshots remain the next implementation layer.

## Development dependencies

- A C++20 compiler and CMake 3.24 or later
- GoogleTest when tests are enabled
- The `dang` source tree or installed `dangd/plugin_api.h`
- FRR and its native YANG files for host-inventory tests

Build and run the portable inventory tests with:

```sh
cmake -S . -B build -DDANGD_ROOT=/path/to/dang
cmake --build build
ctest --test-dir build -R frr --output-on-failure
```

Inspect the installed model closure that will back the first zebra/static
routing milestone with:

```sh
./build/frr-schema-inventory /usr/share/yang \
  frr-routing frr-zebra frr-staticd
```

Use `/usr/local/share/yang` on FreeBSD. A missing imported module is a hard
failure rather than a partially advertised YANG library.

Verify local frontend protocol compatibility without locking or changing any
FRR datastore:

```sh
sudo ./build/frr-mgmtd-session-check /var/run/frr/mgmtd_fe.sock
```

The command creates and immediately destroys one frontend session. Socket
access normally requires the FRR service account or root.

On FreeBSD, ensure the package's runtime state directory exists before starting
`mgmtd`; a missing directory produces an FRR startup warning and can prevent
later persistence work:

```sh
sudo install -d -o frr -g frr -m 0750 /var/lib/frr
sudo service frr start mgmtd
```

Native validation must cover both Linux and FreeBSD and use only loopback or
disposable network namespaces/VNET jails. Host LAN interfaces must never be
attached to a test.

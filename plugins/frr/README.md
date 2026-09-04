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

Native validation must cover both Linux and FreeBSD and use only loopback or
disposable network namespaces/VNET jails. Host LAN interfaces must never be
attached to a test.

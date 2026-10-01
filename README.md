<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# dang_plugins

`dang_plugins` is the development home for YANG models and their implementation
plugins for the `dangd` NETCONF server. It is intentionally separate from the
server repository so model-specific experiments, platform integrations, and
release schedules do not become part of the trusted server core.

The plugin ABI and runtime contract remain owned by `dang`. This repository
includes `dangd/plugin_api.h` from an installed `dang` development package or a
specified `dang` source tree; it does not carry a private copy of the ABI.

## Repository layout

```text
plugins/
  example/
    models/       YANG modules, imports, and deviations owned by the plugin
    src/          provider implementation
  kea/            Kea DHCPv4/DHCPv6 provider and deployment guide
  frr/            FRR-native provider and mgmtd integration (in progress)
  rib/            RFC 8431 schema and in-progress native backend
  vpp/            Linux-only VPP ownership safety architecture
  system/         RFC 7317 provider and platform implementations
pam/              PAM adapter and recovery-first installation guide
tests/            collection-wide loader and contract tests
```

Each production plugin should have its own directory under `plugins`, keep its
normative YANG files under `models`, and embed the exact source bytes returned
through `DangYangSourceV1`. Platform-specific implementations should live below
that plugin rather than in common server code.

Every plugin is required to build and pass native tests on both Linux and
FreeBSD. Network-facing tests must create disposable isolation and must never
attach a host LAN interface. See `plugins/kea/README.md` for the reference
network-namespace and VNET-jail pattern.

New provider code must prefer programmatic library, kernel, socket, or daemon
APIs over invoking command-line tools. If no suitable interface exists, the
exception must be documented and use fixed validated argv without a shell,
with explicit failure and rollback tests. This includes investigating netlink
on both Linux and FreeBSD before adding command-driven route or interface code.

## Build and test

Install the complete collection's build dependencies first. PAM development
headers are required even when testing another provider because the default
build includes `pam_dangd`:

```sh
# Debian/Ubuntu
sudo apt install build-essential cmake ninja-build git pkg-config \
  libxml2-dev nlohmann-json3-dev libpugixml-dev libgtest-dev libpam0g-dev

# FreeBSD (PAM headers are part of the base system)
sudo pkg install cmake ninja git pkgconf libxml2 nlohmann-json pugixml googletest
```

Point CMake at either the `dang` source tree or an installed include directory:

```sh
cmake -S . -B build -DDANGD_ROOT=/path/to/dang
cmake --build build
ctest --test-dir build --output-on-failure
```

For an installed development package, omit `DANGD_ROOT` when
`dangd/plugin_api.h` is already on CMake's search path, or set
`DANGD_INCLUDE_DIR` directly.

Native Debian and FreeBSD package generation is documented in `PACKAGING.md`.
Each provider, the PAM module, and shared
documentation have separate packages. Packages install their own plugin and
models but do not enable PAM, modify sshd, configure Kea, or start dangd.

The example produces `dangd_example_external_plugin.so` on ELF systems or the
corresponding module suffix on another supported POSIX platform. Load it with:

```sh
dangd --model /path/to/root.yang --config /path/to/config.xml \
  --plugin ./build/dangd_example_external_plugin.so --check
```

## Adding a model plugin

1. Create `plugins/<name>/models` and `plugins/<name>/src`.
2. Pin every implemented, imported, and deviation YANG source used by the
   plugin. Do not download schemas during daemon startup.
3. Export the highest completely implemented ABI initializer and retain the
   complete v1 prefix required by that ABI.
4. Add the module target and tests to the root `CMakeLists.txt`.
5. Test discovery, validation, action ordering, rollback, operational-data
   completeness, reconciliation, worker timeout, and worker crash behavior as
   applicable.
6. Record user-visible changes in `CHANGELOG.md`.

See the `dang` plugin author guide for the complete ownership, security,
transaction, and worker-recovery contract.

A deployment must have exactly one runtime owner for each implemented module.
Separate provider packages make that choice explicit and allow two competing
backends—such as the native RIB and FRR providers—to conflict
only with one another. Packaging does not weaken dangd's duplicate-owner check.

## Included plugins

- `example` is the minimal ABI-v1 development template. Its setup guide is
  `plugins/example/README.md`.
- `kea` implements the pinned Kea DHCPv4 and DHCPv6 YANG models through native
  local control sockets, publishes complete ABI-v5 lease and local-member HA
  operational state, and includes isolated Linux and FreeBSD interactions. Its
  ABI-v9 contract also generates complete hot-standby member candidates and
  verifies authenticated peer readback. Server and socket setup is in
  `plugins/kea/README.md`.
- `system` implements RFC 7317 system identity, hostname, clock/timezone, NTP,
  DNS, local authentication, platform state, and control RPCs. It also supplies
  the root-only verifier used by `pam_dangd`; its explicit compliance gaps are
  maintained in `plugins/system/README.md`.
- `pam_dangd` is packaged independently. Follow `pam/README.md`, including its
  recovery-first SSH/PAM procedure.
- `frr` is the top-priority routing provider. It implements native routing,
  zebra, staticd, live interface/VRF parents, and runtime-gated protocol models
  through `mgmtd`, with disposable validation, before-image rollback,
  reconciliation, operational state, native RPC plumbing, and deduplicated
  unsolicited events. See `plugins/frr/README.md` for the tested limitations.
- `rib` provides the ABI-v8 RFC 8431 configuration and notification provider,
  claims the
  `routing` resource against FRR, and applies its supported route slice on Linux
  and FreeBSD. RPC, notification, and operational work remains in progress.

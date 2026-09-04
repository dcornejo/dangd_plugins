<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# FRR native-model provider

This directory contains the initial provider that manages FRRouting
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

Discovery then opens a short-lived mgmtd session and reads FRR's RFC 8525 YANG
Library. Enabled `feature` values are copied into dangd's source descriptors,
so the compiled schema matches the running daemon rather than every feature
statement present in a source file. Startup fails closed if mgmtd is
unavailable, a required module is absent, or its revision or namespace differs
from the installed source. `DANG_FRR_YANG_LIBRARY_FILE` is a test-only seam for
supplying a captured library document without a daemon.

The loadable `dangd_frr_plugin` currently implements the `frr-routing`,
`frr-staticd`, and `frr-zebra` configuration modules. It publishes their exact
installed import closure and claims ABI-v8 resource domain `routing`, so dangd
will reject simultaneous use of another routing provider. Staticd augments the
`frr-routing:routing` root; its nodes are retained inside that atomic edit. The
separate `frr-zebra:zebra` root participates in the same candidate transaction.

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
uncommitted edits. Wiring this orchestration into the plugin ABI and extracting the FRR
subtrees from dangd snapshots are converted to atomic root replace/delete
edits.

The provider retrieves the implemented top-level `/frr-zebra:zebra` state and
zebra augments below `/frr-interface:lib` and `/frr-vrf:lib` from FRR's
operational datastore with native `GET_DATA`. For augmented lists it retains
only the parent keys needed to identify each instance and children in the
`frr-zebra` namespace; base interface and VRF state remains owned by those
modules. It deliberately avoids a broad `/*` request, which also returns mgmtd
and YANG-library trees owned by other providers. Message type, request and
session correlation, XML format, partial-error status, and continuation state
are checked before any bytes reach dangd. A partial result fails the retrieval
rather than presenting an incomplete tree as authoritative.

All top-level RPCs declared by the installed `frr-zebra` module are dispatched
through mgmtd's public native RPC request/reply API. Dangd validates input and
output against the runtime-matched schema and applies NACM before invoking the
provider. The provider does not translate RPCs into shell commands. Live RPC
interoperability still requires a running zebra backend; the validation hosts
currently expose either mgmtd without a zebra RPC backend or an inactive
socket, so only the portable native-wire and correlated-session contract is
confirmed here.

Native FRR notifications and protocols beyond zebra/staticd are not yet
implemented. The provider additionally publishes its small implemented
`dang-frr-monitoring` model. Its `configuration-drift` notification is provider
health telemetry rather than an alteration of FRR's native models.
The operational callback publishes FRR's observed tree; it does not substitute
requested configuration for observed state.

After every successful apply, ABI-v6 reconciliation reads
`/frr-routing:routing` and `/frr-zebra:zebra` back from FRR's running datastore.
Those observed roots replace only the corresponding roots in dangd's complete
applied snapshot. An absent FRR root removes the requested root, a wrong
namespace or malformed reply fails closed, and configuration belonging to
other plugins is preserved byte-for-tree rather than reconstructed from FRR.

The reconciled roots become the provider's expected running state. Each later
operational retrieval reads those roots again and compares XML element names,
namespace URIs, attributes, values, and child order independent of namespace
prefix spelling. A semantic mismatch is reported at the affected FRR root and
causes the operational callback to fail closed. Dangd records that provider
failure in its modeled reconciliation telemetry, making out-of-band edits
visible without publishing stale FRR state. After the first successful
reconciliation, a read-only background watcher also compares the same roots at
one-second intervals. It queues one `configuration-drift` event per affected
path until a later dangd commit establishes a new expected state. ABI v8 carries
that event through worker isolation; dangd validates it against
`dang-frr-monitoring` and applies subscription filters and NACM before delivery.
Set `DANG_FRR_DRIFT_POLL_MS` to an integer from 100 through 60000 only when a
different monitoring interval is operationally justified.

## Installation and configuration

Install FRR with `mgmtd`, zebra, and staticd enabled. Build and stage the plugin:

```sh
cmake -S . -B build -DDANGD_ROOT=/path/to/dang
cmake --build build --target dangd_frr_plugin
sudo cmake --install build --component frr
```

Linux normally needs no path overrides. A typical launch is:

```sh
sudo dangd --plugin /usr/lib/dangd/plugins/dangd_frr_plugin.so
```

FreeBSD normally installs the module below `/usr/local/lib/dangd/plugins` and
schemas below `/usr/local/share/yang`. Use these environment variables only
when the package uses nonstandard locations:

```sh
export DANG_FRR_YANG_DIR=/custom/share/yang
export DANG_FRR_MGMTD_SOCKET=/custom/run/frr/mgmtd_fe.sock
export DANG_FRR_TIMEOUT_MS=5000
```

The timeout must be 1 through 60000 milliseconds. Dangd must run as a user able
to open FRR's mode-0600 frontend socket. Do not broaden the socket permissions;
use a narrowly privileged service identity.

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

The default suite never changes FRR. The opt-in native test creates its own
`dangd-test-PID` mgmtd pathspace, commits an empty staticd control-plane
protocol instance, reads it back, restores the exact before-image, and removes
the daemon and its runtime/state directories. It requires root only to create
the FRR-owned socket directories and change the disposable daemon's identity:

```sh
cmake -S . -B build -DDANGD_ROOT=../dang -DDANG_FRR_NATIVE_TESTS=ON
cmake --build build
sudo ctest --test-dir build -R frr_isolated_native_mutation \
  --output-on-failure
```

The mutation diagnostic refuses sockets whose path does not contain
`dangd-test`. The fixture starts only mgmtd and does not create, attach, or
modify any network interface or route.

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

Retrieve the live operational XML without changing FRR configuration:

```sh
sudo ./build/frr-mgmtd-session-check /var/run/frr/mgmtd_fe.sock --operational
```

Inspect one running configuration root through the same read-only path:

```sh
sudo ./build/frr-mgmtd-session-check /var/run/frr/mgmtd_fe.sock \
  --running /frr-routing:routing
```

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

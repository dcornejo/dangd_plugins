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
import-and-include closure and rejects missing imports or submodules, duplicate
source names, malformed input, and size-limit violations.

Discovery then opens a short-lived mgmtd session and reads FRR's RFC 8525 YANG
Library. Enabled `feature` values are copied into dangd's source descriptors,
so the compiled schema matches the running daemon rather than every feature
statement present in a source file. Startup fails closed if mgmtd is
unavailable, a required module is absent, or its revision or namespace differs
from the installed source. `DANG_FRR_YANG_LIBRARY_FILE` is a test-only seam for
supplying a captured library document without a daemon.

The loadable `dangd_frr_plugin` currently implements the `frr-routing`,
`frr-staticd`, and `frr-zebra` configuration modules. It publishes their exact
installed import-and-include closure and claims ABI-v8 resource
domain `routing`, so dangd will reject simultaneous use of another routing
provider. Staticd augments the `frr-routing:routing` root; its nodes are
retained inside that atomic edit. The separate `frr-zebra:zebra` root
participates in the same candidate transaction. Installed submodules are
matched to their owning module and revision in FRR's live RFC 8525 library;
missing or mismatched submodules make discovery fail closed.

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
uncommitted edits. FRR subtrees extracted from dangd snapshots become atomic
root replace/delete edits. The hardware coordinator sees the complete mgmtd
candidate as one normal `configuration` action. It is intentionally not split
by root or leaf: mgmtd's validated candidate commit is the native atomicity and
rollback boundary, and the single descriptor ensures dangd schedules the
provider apply.

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
provider. The provider does not translate RPCs into shell commands. On the
Linux validation host, FRR 10.5.1 runs active mgmtd and zebra adapters but its
live RPC registry contains no `/frr-zebra` subtree; invoking
`/frr-zebra:get-vrf-info` reaches mgmtd and is rejected with `No backends
implement xpath`. The codec and correlated session contract are verified, but
successful live interoperability remains blocked until an FRR release or
backend actually registers these modeled RPCs. The plugin preserves FRR's
rejection instead of substituting CLI behavior.

Native FRR protocols beyond zebra/staticd are enabled only when FRR's live
library and backend advertise them. The conditional set is BFD, EIGRP, IS-IS,
OSPFv2, Pathd, PIM, RIP, RIPng, and VRRP. Modules with standalone roots own
those roots; OSPFv2 and VRRP consist of augments within already-owned routing
or interface parents. The provider additionally publishes its small implemented
`dang-frr-monitoring` model. Its `configuration-drift` notification is
provider health telemetry rather than an alteration of FRR's native models.
The operational callback publishes FRR's observed tree; it does not substitute
requested configuration for observed state.

The public native mgmtd notification framing is now implemented as the first
protocol-notification layer. It can construct `NOTIFY_SELECT` requests for
on-change or periodic XPath-prefix subscriptions and strictly decode modeled
XML `NOTIFY` frames. Datastore replace/delete/patch synchronization messages
are deliberately rejected by that decoder so they cannot be mislabeled as
RFC 5277 events. The installed FRR 10.7 model set declares notifications only
in `frr-isisd` and `frr-ripd`; each is advertised only when the running FRR
module-set implements it.
The provider now starts a dedicated long-lived notification connection when
FRR's live RFC 8525 module-set advertises `frr-ripd` or `frr-isisd`. Installed
source files alone do not enable either module. The matching complete schema
closure is then published as implemented, its configuration augments remain
inside the same atomic mgmtd transaction. The provider explicitly owns the
modules' separate `/frr-ripd:ripd` and `/frr-isisd:isis` roots for extraction,
rollback, reconciliation, drift checks, and operational retrieval; its native
RPCs use the same mgmtd dispatch path. The session explicitly negotiates XML,
selects every enabled modeled event by its exact schema XPath, distinguishes
safe idle timeouts, checks session and module identity, places at most 1,024
events in the nonblocking ABI-v8 queue, and reconnects after mgmtd restarts.
Dangd performs the final schema validation, subscription filtering, and NACM
authorization.
Captured YANG-library fixtures deliberately disable the reader so discovery
tests cannot contact a production socket.

FRR protocol models also place configuration beneath keyed instances in
`/frr-interface:lib`, and zebra places configuration beneath both that root and
`/frr-vrf:lib`. When the live library implements these parent modules, the
provider publishes them as implemented and owns each complete root in the same
atomic transaction. This includes interface descriptions, zebra address and
VRF settings, RIP interface authentication, and IS-IS circuit configuration.
Operational retrieval likewise returns the complete implemented parent roots;
the older zebra-only filter remains a compatibility path when a runtime exposes
zebra's augments without implementing the parent module.

The same runtime gate applies to native RPC dispatch and standalone operational
retrieval. BFD, EIGRP, IS-IS, Pathd, PIM, RIP, and RIPng roots are fetched only
when their module is live. RPCs declared by any enabled conditional protocol use
the same correlated mgmtd request/reply path as zebra. OSPFv2 and VRRP data is
carried by the routing/interface parent roots. BGP is deliberately absent from
this list: FRR 10.7.1 installs its source family, but the tested bgpd does not
register it as an implemented mgmtd module.

The opt-in Linux interaction creates two network namespaces and a disposable
veth, commits `/frr-ripd:ripd` through mgmtd, subscribes to
`authentication-type-failure`, and sends a malformed RIPv2 authentication
record from the peer namespace. It never attaches a host LAN interface. FRR
10.7.1 registers the RIP backend, applies the modeled configuration, opens UDP
520, and emits the notification, but mgmtd then reports `Unexpected notification
element "authentication-type-failure"` and aborts at
`assure_notify_msg_cache()`. The CTest case uses skip result 77 only for that
exact signature; any other failure remains a failed test. Successful forwarding
is therefore blocked upstream. FreeBSD compilation passes, but repeating an
already identified platform-independent mgmtd assertion there would not supply
successful interoperability evidence.

After every successful apply, ABI-v6 reconciliation reads every managed root
back from FRR's running datastore. This always includes
`/frr-routing:routing` and `/frr-zebra:zebra`, and conditionally includes the
live interface, VRF, RIP, and IS-IS roots. Those observed roots replace only
their corresponding roots in dangd's complete applied snapshot. An absent FRR
root removes the requested root, a wrong namespace or malformed reply fails
closed, and configuration belonging to other plugins is preserved
byte-for-tree rather than reconstructed from FRR.

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

### Installing FRR on Debian and Ubuntu

Use FRR's official Debian package repository at
<https://deb.frrouting.org/>. The following procedure installs the repository
signing key in a dedicated keyring and selects the FRR 10.7 release series. A
fixed series is recommended for dangd deployments because it receives updates
within that series without unexpectedly crossing a major-version boundary.

```sh
sudo apt-get update
sudo apt-get install -y ca-certificates curl
curl -fsSL https://deb.frrouting.org/frr/keys.gpg \
  | sudo tee /usr/share/keyrings/frrouting.gpg >/dev/null
echo "deb [signed-by=/usr/share/keyrings/frrouting.gpg] https://deb.frrouting.org/frr $(. /etc/os-release && echo \"$VERSION_CODENAME\") frr-10.7" \
  | sudo tee /etc/apt/sources.list.d/frr.list >/dev/null
sudo apt-get update
sudo apt-get install -y frr frr-pythontools
```

FRR also publishes the moving `frr-stable` channel. Replace `frr-10.7` in the
repository line with `frr-stable` only when automatically moving to a newer
stable FRR series is acceptable. Consult the repository page for the currently
supported Debian and Ubuntu releases and FRR series rather than substituting an
unrelated distribution codename.

Enable the daemons required by this plugin in `/etc/frr/daemons`:

```text
zebra=yes
mgmtd=yes
staticd=yes
```

Then restart FRR and verify that the installed package comes from the FRR
repository, the service is running, and mgmtd has created its frontend socket:

```sh
sudo systemctl restart frr
apt-cache policy frr
systemctl is-active frr
sudo vtysh -c 'show mgmt backend-adapter all'
sudo test -S /run/frr/mgmtd_fe.sock
```

The policy output should identify `https://deb.frrouting.org/frr` as the source
of the installed and candidate version. The backend-adapter output should list
at least `mgmtd`, `zebra`, and `staticd`. Debian-family packages also make the
socket available through the compatible `/var/run/frr/mgmtd_fe.sock` path.

Before changing repository series or upgrading an existing FRR deployment,
back up `/etc/frr` and review FRR's release notes. Package installation normally
preserves locally modified configuration, but the daemon restart activates the
new binaries and should be scheduled like any routing-service maintenance.

### Installing the dangd plugin

With FRR installed and `mgmtd`, zebra, and staticd enabled, build and stage the
plugin:

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
  frr-routing frr-zebra frr-staticd frr-bgp
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

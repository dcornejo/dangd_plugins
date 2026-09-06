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

The `dangd_rib_plugin` advertises the pinned model and implements the documented
destination-prefix configuration slice. It claims ABI-v7 exclusive ownership
of `routing`, so dangd rejects loading it together with the FRR provider. The
portable `route-add`, `route-delete`, prefix-selected `route-update`, `rib-add`,
`rib-delete`, `nh-add`, and `nh-delete` RPCs are implemented; both
notifications remain incomplete. Operational reads enumerate host IPv4
and IPv6 unicast routes through native kernel APIs and publish active and
installed status as partial RFC 8431 state.

## Installation status and dependencies

The `dangd-plugin-rib` package contains the provider, pinned sources, and guide.
Loading it authorizes supported route changes and therefore requires routing
privilege.

Install it for model development or interoperability testing. Debian/Ubuntu:

```sh
sudo apt install ./dangd-plugin-rib_0.1.0_amd64.deb
dpkg -L dangd-plugin-rib
```

FreeBSD:

```sh
sudo pkg add ./dangd-plugin-rib-0.1.0.pkg
pkg info -l dangd-plugin-rib
```

For a source-tree schema check, install `yanglint`/libyang, point the build at
dangd's model directory, and install only the RIB component:

```sh
cmake -S . -B build -G Ninja -DDANGD_ROOT=/path/to/dang
ctest --test-dir build -R rfc8431_schema_interoperability \
  --output-on-failure
sudo cmake --install build --component rib
```

Load `dangd_rib_plugin.so` from dangd's plugin directory. The backend requires
Linux `iproute2` or FreeBSD base `route(8)`, route-management privilege, and
numeric RIB/FIB names. It must not be loaded together with the FRR plugin.

The runtime foundation currently parses destination-prefix IPv4 and IPv6
routes whose base nexthop is a gateway, an outgoing interface, or both. It
requires the RFC 8431 route preference and local-only fields, rejects source,
MPLS, MAC, interface-match, chained, replicated, protected, load-balanced, and
tunnel routes with an attributed model path, and computes replacements as an
old-route deletion followed by a new-route installation. Separate Linux `ip`
and FreeBSD `route` argv planners require numeric RIB/FIB names and never invoke
a shell. The shared executor uses `posix_spawnp(3)`, stops on the first failed
operation, and compensates completed changes in reverse order. Linux state is
read through rtnetlink and FreeBSD state through `NET_RT_DUMP`; command output
is never parsed. Kernel routes receive deterministic synthetic `route-index`
values because neither native API exposes the model's list key. Unit tests cover
successful execution, apply failure, complete rollback, and incomplete
rollback reporting.

The complete delta is one ABI-v4 hardware action. Apply compensates partial
failure, and coordinator rollback applies inverse changes in reverse order.

Privileged native tests are opt-in with `-DDANG_RIB_NATIVE_TESTS=ON`. Linux
creates a disposable network namespace and dummy interface. FreeBSD creates a
disposable VNET jail and epair, assigns only documentation-prefix addresses,
and destroys both afterward. Each interaction installs the test route, verifies
it in the plugin's operational XML and through the native kernel route
inventory, deletes it, and verifies absence. No host LAN interface or host
default route is used.

`route-add` accepts the same destination-prefix/base-nexthop subset as
configuration commits. Each member is attempted independently, as required by
the RPC's success/failed-count result shape. Optional failure detail reports
code 3 for malformed supported-slice input and reserved code 0 when a native
operation fails without an RFC-defined error-code equivalent. Schema-invalid
RPC envelopes fail through dangd before plugin dispatch; direct malformed
plugin calls fail with the attributed `/ietf-i2rs-rib:route-add` path.

`route-delete` resolves each requested RIB and destination prefix against a
fresh native route inventory. A unique match is deleted with its observed
gateway and interface, a missing route returns RFC error code 2, and an
ambiguous multipath match fails closed with reserved code 0.

`route-update` supports per-prefix replacement of a base nexthop or the complete
portable route-attributes pair. It captures the matching observed route as the
before-image, deletes it, installs the replacement, and restores that exact
before-image if installation fails. Attribute-wide, nexthop-wide, and vendor
selectors remain explicitly unsupported.

`rib-add` validates a numeric native namespace. Linux tables are created by
their first route, while FreeBSD FIBs must already exist in `net.fibs`; no
synthetic kernel object is created. Requests for `ip-rpf-check=true` return a
modeled failure because RPF enforcement is not implemented. `rib-delete`
removes every observed route in the selected namespace as one compensated
plan, restoring earlier deletions if a later native operation fails.

`nh-add` allocates an identifier for a base IP-address, outgoing-interface, or
combined nexthop and retains it in a mutex-protected registry scoped by RIB.
`nh-delete` removes exactly that RIB/identifier pair and reports a modeled
failure for an unknown pair. This portable registry is intentionally owned by
the plugin because Linux and FreeBSD do not expose equivalent standalone
nexthop objects. It is currently volatile and is not yet emitted in operational
state. Configuration commits, `route-add`, and prefix-selected `route-update`
resolve `nexthop-ref` against the
containing RIB and fail closed for absent or cross-RIB identifiers. Reference
lifetime is enforced for prepared and active datastore configurations, and
`nh-delete` reports a modeled failure while such a route retains the object.
Imperative `route-add` and `route-update` operations retain the same bindings;
successful `route-delete` and `rib-delete` release them. Registry persistence
remains before reusable-nexthop semantics are compliant. Operational reads
publish registered identifiers under their containing RIB. Gateway nexthops
provide their family directly; interface-only entries require an unambiguous
observed RIB family and are omitted until one is available.

Numeric names are an intentional temporary variance: RFC 8431 RIB names are
arbitrary strings, while Linux policy tables and FreeBSD FIBs need an explicit
platform mapping. A future plugin option must supply that mapping before this
restriction can be removed safely.

FreeBSD interface-only nexthops remain rejected because `route(8)` needs a
local interface address as its gateway argument for an Ethernet route. The
operational adapter must resolve that address unambiguously before this case is
enabled; gateway-plus-interface routes are natively validated now.

Tests must use Linux network namespaces or FreeBSD VNET jails with only
disposable loopback/epair interfaces. They must never add, remove, or replace a
route on a host LAN interface or in the host's default routing table.

Remove managed routes through dangd before unloading the provider, then use
`sudo apt remove dangd-plugin-rib` or `sudo pkg delete dangd-plugin-rib`.
Package removal does not clean up routes left in the kernel.

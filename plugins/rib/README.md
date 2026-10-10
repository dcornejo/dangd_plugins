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
destination-prefix configuration slice. It claims ABI-v8 exclusive ownership
of `routing`, so dangd rejects loading it together with the FRR provider. The
portable `route-add`, `route-delete`, prefix-selected `route-update`, `rib-add`,
`rib-delete`, `nh-add`, and `nh-delete` RPCs are implemented. Managed
`route-change` delivery covers managed and externally observed changes, and
`nexthop-resolution-status-change` covers the portable reusable-nexthop subset.
Operational reads enumerate host IPv4
and IPv6 unicast routes through native kernel APIs and publish active and
installed status as partial RFC 8431 state.
They also publish every durable `rib-add` registration, including a RIB with
no native routes or reusable nexthops. Native observation and the registry
snapshot share the imperative-RPC serialization boundary, so one reply cannot
combine kernel state from before a route RPC with registry state from after it.
Gateway-plus-interface nexthops use the schema-defined combined IPv4 or IPv6
container; generated examples for both address families are validated as YANG
operational data with their interface leafrefs resolved.
Observed `local-only` state comes from Linux `RT_SCOPE_HOST` and FreeBSD
`RTF_LOCAL`; it is not guessed from prefix length or interface scope. Those
routes are kernel-owned receive paths. Portable configuration, `route-add`,
and `route-update` therefore require `local-only=false` and reject `true`
before mutation instead of installing an ordinary forwarding route with a
false claim.
Native receive, blackhole, and error-reject routes are published with the RFC
8431 `receive`, `discard`, and `discard-with-error` special nexthop identities.
This covers Linux `RTN_LOCAL`, `RTN_BLACKHOLE`, `RTN_UNREACHABLE`, and
`RTN_PROHIBIT`, plus FreeBSD `RTF_LOCAL`, `RTF_BLACKHOLE`, and `RTF_REJECT`.
The portable provider can configure, add, update, delete, and roll back direct
`discard` and `discard-with-error` nexthops. The `receive` identity remains
kernel-owned and read-only: route delete/update returns reserved error code 0
for it, and `rib-delete` fails before changing anything if the selected RIB
contains one. Linux maps the two writable identities to blackhole and
unreachable routes. FreeBSD maps them to blackhole and reject routes through
the corresponding IPv4 or IPv6 loopback gateway, which must exist as it does
on a normally booted host.
Linux `RTA_MULTIPATH` paths for one destination are projected into one stable
route containing the RFC 8431 `nexthop-lb` structure and exact native weights.
Managed routes retain the configured route index and durable reusable-nexthop
IDs. External routes have no datastore identity, so each operational snapshot
assigns deterministic local member IDs above the durable registry range.
The operational snapshot uses the registry resolution view, which combines
imperative RPC bindings with bindings reconstructed from dangd's authoritative
configuration during reconciliation.
Native weights outside the schema's 1-through-99 range remain separate base
routes rather than producing invalid weighted XML. A dead native member is
inactive and carries the exact RFC 8431
`unresolved-nexthop` route reason; a subsequent installed-state transition is
reported as `resolved-nexthop` or `unresolved-nexthop` in `route-change`.
Ordinary route creation, removal, or metric changes do not expose a reliable
native cause and are deliberately left without a reason rather than guessed.
Routes carrying Linux `RTA_NH_ID` are now joined with a direct
`RTM_GETNEXTHOP` dump. Simple objects and recursively referenced multipath
groups expand into the same base-nexthop view, including gateway, interface,
special discard identity, and installed state. Cycles, missing members,
encapsulation, FDB objects, wrong-family objects, interrupted dumps, and other
unrepresentable forms are omitted rather than emitted as partial or invalid
routes. Native ECMP weights are retained exactly in the internal observation
contract: Linux decodes both classic multipath weights and the complete
two-byte persistent-group weight, while FreeBSD reads `rmx_weight` separately
from route preference. A weight-only change is visible to the generic change
tracker without changing route identity. The portable
configuration parser now understands the optional `nexthop-lb` structure: it
resolves every reusable-nexthop member within its RIB, canonicalizes members by
identifier, retains all references for the route lifetime, and enforces the
YANG type's normative weight range of 1 through 99. The model description
mentions zero, but zero is outside that typedef range and therefore fails
schema and runtime validation. Linux applies one load-balanced route with a
bounded `RTA_MULTIPATH` request. FreeBSD applies acknowledged per-member
route-netlink requests and reverses completed members if a later member fails;
the outer plugin transaction also retains the exact modeled member list for
rollback. The retained FreeBSD route(8) argv planner is a deterministic test
adapter and rejects this multi-request form rather than pretending it is
atomic. The plugin advertises `nexthop-load-balance` because configuration,
native mutation, operational projection, schema validation, and rollback are
covered end to end on both native backends. Object-backed
routes are operational/read-only: the base view cannot retain the Linux object
ID and group topology required to recreate the exact route during rollback, so
imperative mutation fails closed instead of approximating the original object.

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
route-management privilege and numeric native RIB/FIB mappings. Production
mutation uses the kernel route-netlink API directly and does not require
Linux `iproute2` or FreeBSD `route(8)`. It must not be loaded together with the
FRR plugin.

The runtime foundation currently parses destination-prefix IPv4 and IPv6
routes whose base nexthop is a gateway, an outgoing interface, both, or the
direct `discard`/`discard-with-error` special identity. It
accepts only the routing instance named `default`; VRF/VNET instance mapping is
not implemented, so any other name fails at `/routing-instance/name` rather
than being applied to the host default instance. It
requires the RFC 8431 route preference and local-only fields. The portable
configuration slice accepts only `local-only=false`; native local receive
routes remain observable read-only state. It rejects source, MPLS, MAC,
interface-match, chained, replicated, protected, and tunnel routes with an
attributed model path. Load-balanced routes pass portable parsing, reference
lifetime, native validation, acknowledged mutation, and compensated rollback
on both platforms. Supported replacements are computed as an old-route
deletion followed by a new-route installation. Linux and FreeBSD
production mutation use bounded route-netlink messages and wait for the
correlated kernel acknowledgement. Retained argv planners are deterministic
unit-test adapters and are never executed by production. The shared executor stops
on the first failed operation and compensates completed changes in reverse
order. Linux state is read through rtnetlink and FreeBSD state through
`NET_RT_DUMP`; command output is never parsed. Installed observations are
`active`; an explicitly
uninstalled observation is `inactive`, so contradictory status pairs are never
emitted. Linux `RTA_PRIORITY` and FreeBSD `rmx_metric` round-trip the RFC 8431
route preference; FreeBSD `rmx_weight` is a separate ECMP path weight and is
not conflated with that attribute. Because neither native API exposes the
model's list key, the plugin correlates an exact native route with dangd's
reconciled applied route and restores its configured `route-index`. Imperative
routes using a reusable nexthop recover the same identity from their registry
binding. Resolved reusable routes publish the modeled `nexthop-id`, its
`sharing-flag`, and the expanded native nexthop definition. Routes not managed
through either source receive deterministic synthetic `route-index` values.
The applied-route snapshot is transient and is rebuilt from dangd's
authoritative configuration through the generic reconciliation contract; it
is not a second configuration database. Unit tests cover successful execution,
apply failure, complete rollback, and incomplete rollback reporting.

The current portable projection permits one modeled route for each RIB,
address family, and destination prefix. Multiple route indexes that collapse
onto that same native key are rejected during datastore preparation; use one
route with a supported nexthop representation instead. This prevents a valid
YANG list from being silently reduced to one kernel entry before complete
multipath support exists.

The complete delta is one ABI-v4 hardware action. Apply compensates partial
failure, and coordinator rollback applies inverse changes in reverse order.

Privileged native tests are opt-in with `-DDANG_RIB_NATIVE_TESTS=ON`. Linux
creates a disposable network namespace and dummy interface. FreeBSD creates a
disposable VNET jail and epairs, assigns only documentation-prefix addresses,
initializes standard loopback addresses, and destroys both afterward. Each
interaction installs ordinary and direct special routes, verifies
it in the plugin's operational XML and through the native kernel route
inventory, requires the configured route preference to round-trip, verifies
that a repeated `route-add` cannot replace it, deletes it, and verifies
absence. No host LAN interface or host
default route is used. The Linux workflow also creates a two-interface ECMP
route in another private table and requires both paths in operational XML. A
second weighted ECMP route references a persistent Linux nexthop group; both
member objects must be expanded through route netlink into the same safe base
view, reported as non-mutable, and retain weights 2 and 3. The FreeBSD workflow
also creates two equal-metric ECMP paths over separate jail interfaces and
requires weights 2 and 3 to remain distinct from route preference. Both native
workflows require the resulting operational XML to contain one weighted route;
the loadable-plugin workflows additionally verify durable member IDs through
apply and rollback.

`route-add` accepts the same destination-prefix/base-nexthop subset as
configuration commits. Each member is attempted independently, as required by
the RPC's success/failed-count result shape. Optional failure detail reports
code 3 for malformed supported-slice input and reserved code 0 when a native
operation fails without an RFC-defined error-code equivalent. Before applying
the batch, production reads the live modeled inventory. An existing route, or
a destination successfully added earlier in the same batch, returns RFC error
code 1 and is never replaced. Linux and FreeBSD then use an exclusive native
create request so a route racing into existence after the inventory read is
also preserved. Schema-invalid
RPC envelopes fail through dangd before plugin dispatch; direct malformed
plugin calls fail with the attributed `/ietf-i2rs-rib:route-add` path.

`route-delete` resolves each requested RIB and destination prefix against a
fresh native route inventory. A unique match is deleted with its observed
gateway and interface, a missing route returns RFC error code 2, and an
ambiguous multipath match fails closed with reserved code 0.

`route-update` supports per-prefix replacement of a base nexthop or the complete
portable route-attributes pair when `local-only` remains false. It captures the
matching observed route as the before-image, rejects an unrepresentable
local-only replacement before deletion, installs the supported replacement,
and restores the exact before-image if installation fails. Attribute-wide,
nexthop-wide, and vendor selectors remain explicitly unsupported.

`rib-add` validates a numeric native namespace. Linux tables are created by
their first route, while FreeBSD FIBs must already exist in `net.fibs`; no
synthetic kernel object is created. Requests for `ip-rpf-check=true` return a
modeled failure because RPF enforcement is not implemented. `rib-delete`
removes every observed route in the selected namespace as one compensated
plan, restoring earlier deletions if a later native operation fails.

`nh-add` allocates an identifier for a base IP-address, outgoing-interface,
combined nexthop, `discard`, or `discard-with-error` identity and retains it in
a mutex-protected registry scoped by RIB. `receive` remains kernel-owned and is
rejected as a reusable configured object.
The live provider validates that RIB name against the active platform mapping
and nexthop family before allocating an identifier or writing the registry.
Unknown aliases, wrong-family aliases, and bare numeric names return a modeled
failure without changing allocation state or durable data.
`nh-delete` removes exactly that RIB/identifier pair and reports a modeled
failure for an unknown pair. This portable registry is intentionally owned by
the plugin because Linux and FreeBSD do not expose equivalent standalone
nexthop objects. Configuration commits, `route-add`,
and prefix-selected `route-update`
resolve `nexthop-ref` against the
containing RIB and fail closed for absent or cross-RIB identifiers. Reference
lifetime is enforced for prepared and active datastore configurations, and
`nh-delete` reports a modeled failure while such a route retains the object.
Imperative `route-add` and `route-update` operations retain the same bindings;
successful `route-delete` and `rib-delete` release them. Registry persistence
is completed before reusable-object and imperative-route RPCs are acknowledged.
Operational reads publish registered identifiers under their containing RIB.
Gateway nexthops provide their family directly. `rib-add` durably records the
modeled family, so an interface-only or special `nh-add` can be published
before the RIB contains any observed route. Such a family-neutral request for
a RIB that has not first been registered fails as a modeled operation rather
than guessing from interface addresses or host defaults. Re-registering a name
with a different family likewise fails closed.

Applied datastore reference counts are rebuilt from dangd's reconciled
configuration snapshot. This makes restart restoration independent of prior
process memory; prepare-time reservations cover the interval between validation
and reconciliation.

The persistence layer uses a versioned JSON sidecar containing modeled RIB
families, reusable objects, the next allocation identifier, and imperative
route bindings. Loading is
bounded to 16 MiB and rejects non-regular, group/world-accessible, duplicate,
or dangling-reference state. Saving uses a private temporary file, `fsync`,
atomic rename, and parent-directory `fsync`. The in-memory registry now exports
and restores that complete representation, rebuilding route reference counts
and rejecting inconsistent recovery data before changing live state. Wiring
is now active at plugin startup. By default the sidecar is
`/var/lib/dangd/rib-nexthops.json`; `DANG_RIB_REGISTRY_FILE` selects a different
private path for packaging or isolated tests. `nh-add` and `nh-delete` are
serialized and acknowledged only after the new state is durable. A failed write
restores the prior objects and allocation cursor before returning an attributed
RPC error. Imperative route-binding compensation is implemented for
`route-add`, `route-delete`,
`route-update`, and `rib-delete`: a failed write executes the inverse native
plan in reverse order, restores the prior registry checkpoint, and includes
any compensation failure in the attributed RPC error. Reusable objects and all
imperative bindings therefore survive restart without acknowledging a split
kernel/sidecar state.
An otherwise empty RIB registered by `rib-add` is part of that durable state
and reappears in operational data after restart before its first route or
nexthop is created.
Registry format version 3 retains the complete reusable special identity.
Version 2 introduced the modeled `route-index` in every binding, allowing
parallel referenced routes for the same RIB, family, and prefix to remain
independent across reconciliation and restart. Versions 1 and 2 remain
readable; version-1 bindings receive their historically unique index zero.

The ABI-v8 provider publishes `route-change` notifications for successful
managed route installation, replacement, and removal. Imperative RPC events
are queued only after native execution and any registry sidecar update have
succeeded. Datastore events are queued from successful applied-configuration
reconciliation rather than tentative hardware apply, so a failed or rolled-back
commit does not leak a success event. Native installed-state transitions
include the exact RFC `resolved-nexthop` or `unresolved-nexthop` reason; events
whose cause is not available from the kernel omit the optional reason list.
The bounded queue contains at most 1024 events. When dangd drains
notifications, the provider also compares a fresh native route inventory with
a synchronized baseline. It first applies the same modeled projection used by
operational retrieval: applied ordinary routes recover their configured
indexes and representable ECMP members collapse into one weighted route with
durable or deterministic member identities. The initial inventory is quiet;
later external additions, removals, and route-property changes produce
`route-change` events. Managed changes advance the same modeled baseline when
their event is queued, preventing both a duplicate when the kernel confirms
the operation and synthetic per-path events for one weighted route. External
base routes remain distinguished by RIB, family, prefix, gateway, interface,
and special identity before any representable ECMP group is collapsed. The
separate `nexthop-resolution-status-change` notification is also implemented
for reusable nexthops and deliberately evaluates the uncollapsed native paths.
Resolution means at
least one imperative or datastore route bound through `nexthop-ref` is present
and installed in the observed native RIB with the referenced gateway,
interface, and/or special identity. A different parallel path for the same
prefix does not resolve the binding. Creating an unused object is not a
status change; losing the final installed binding transitions it to
`unresolved`. The notification contains the allocated ID, sharing flag, and
complete supported base nexthop. Complex nexthops outside the portable subset
remain unsupported rather than receiving an approximate status.

RFC 8431 RIB names are arbitrary strings, while Linux policy tables and
FreeBSD FIBs use native numbers. The plugin accepts a bounded versioned JSON file
with independent Linux and FreeBSD numbers, for example:

```json
{
  "version": 2,
  "ribs": [
    {"name": "blue-v4", "address-family": "ipv4",
     "linux-table": 100, "freebsd-fib": 2},
    {"name": "blue-v6", "address-family": "ipv6",
     "linux-table": 100, "freebsd-fib": 2}
  ]
}
```

Modeled names must be unique on each platform. Native numbers must be unique
within an address family, but the same native table or FIB may have separate
IPv4 and IPv6 names. This distinction is required because an RFC 8431 RIB has
one address family and its list key is only `name`, while native RIBs are often
dual-stack. Duplicate or conflicting entries fail loading. Mapping format 1 is
rejected because its family-neutral aliases cannot represent this distinction.
Set
`DANG_RIB_MAP_FILE` to the absolute path of this file before starting dangd.
Without a configured alias, use the unambiguous built-in names `ipv4-N` and
`ipv6-N`, where `N` is the native table or FIB number; bare numeric modeled
names are rejected.
If a native number/family has a configured alias, use that alias rather than
its built-in spelling. Built-in numeric suffixes must be canonical (no leading
zeroes), and a configured built-in name cannot refer to another number or
family. These rules preserve one identity through native operational readback.
Configuration
validation and apply/rollback, imperative route and RIB RPCs, reusable-nexthop
scope and persistence, operational reads, and notification payloads all
preserve the modeled name while native commands receive its platform number.
On FreeBSD, operational, notification, and imperative safety polling query
every kernel FIB independently with `NET_RT_DUMP`; results are then translated
back to their configured alias or unique built-in modeled name. This ensures
that repeat detection also covers unaliased `ipv4-N` and `ipv6-N` RIBs.

When upgrading from mapping format 1 or numeric identity names, stop dangd and
update both the mapping file and every RIB name in the private nexthop registry
before restarting. The registry defaults to
`/var/lib/dangd/rib-nexthops.json` and may be overridden with
`DANG_RIB_REGISTRY_FILE`. Startup validates all persisted RIB, nexthop, and
route-binding identities against the active platform mapping and fails with a
migration message instead of loading ambiguous state. An unused registry may
instead be removed while dangd is stopped.

FreeBSD interface-only nexthops encode the selected interface index directly
as `RTA_OIF`; they neither guess a local gateway address nor require the
interface to be numbered. The retained `route(8)` argv adapter still exercises
legacy address-resolution behavior in portable unit tests, but production
never calls it. A FreeBSD 16.0-CURRENT VNET test installs and removes IPv4 and
IPv6 gateway routes, an interface-only IPv4 route after removing every address
from the epair, and verifies that a nonexistent interface fails before a
netlink request is sent.

Tests must use Linux network namespaces or FreeBSD VNET jails with only
disposable loopback/epair interfaces. They must never add, remove, or replace a
route on a host LAN interface or in the host's default routing table.

Remove managed routes through dangd before unloading the provider, then use
`sudo apt remove dangd-plugin-rib` or `sudo pkg delete dangd-plugin-rib`.
Package removal does not clean up routes left in the kernel.

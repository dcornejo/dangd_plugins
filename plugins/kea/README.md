<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Kea DHCP plugin

`dangd_kea_plugin` manages one Kea DHCPv4 server, one Kea DHCPv6 server, or
one of either family through local UNIX control sockets. It embeds the official
Kea 3.2.0
`kea-dhcp4-server` and `kea-dhcp6-server` modules at revision 2026-06-24 plus
their pinned Kea type modules. It also provides the read-only `dang-kea-ha`
module for local-member HA status. The original model files retain ISC's
MPL-2.0 license notices. The read-only `dang-kea-instance` companion identifies
the independently managed process boundary and its enabled address families;
the companion models and adapter code are Apache-2.0.

## Dependencies and installation

The runtime requires dangd 0.1.0 or newer, the Kea 3.2.x server package for
every enabled address family, the Kea lease-command, host-command, and
supplemental-statistics hook libraries, and local UNIX control sockets
accessible by the plugin worker. On Debian/Ubuntu install `kea-dhcp4-server`,
`kea-dhcp6-server`, or both as required; on FreeBSD install `kea`. A source
build additionally needs CMake 3.24+, a C++20 compiler, libxml2 development
files, nlohmann-json 3.11+, and GoogleTest.

Debian/Ubuntu package installation:

```sh
sudo apt update
sudo apt install kea-dhcp4-server kea-dhcp6-server
sudo apt install ./dangd-plugin-kea_0.1.0_amd64.deb
dpkg -L dangd-plugin-kea
```

FreeBSD package installation:

```sh
sudo pkg install kea
sudo pkg add ./dangd-plugin-kea-0.1.0.pkg
pkg info -l dangd-plugin-kea
```

To build and install only this component from the repository:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DDANGD_ROOT=/path/to/dang
cmake --build build --target dangd_kea_plugin kea_adapter_test
ctest --test-dir build -R 'kea_' --output-on-failure
sudo cmake --install build --component kea
```

The plugin does not require Kea Control Agent or a database lease backend.
See [DEPLOYMENT.md](DEPLOYMENT.md) for supported single-stack, dual-stack,
multiple-instance, and HA layouts.
When the local daemon participates in Kea HA, the operating-system Kea package
must also provide `libdhcp_ha.so`; pair communication and any TLS files remain
ordinary Kea runtime dependencies.

## Transaction behavior

For every affected commit, the plugin processes its enabled targets in stable
DHCPv4-then-DHCPv6 order and:

1. converts the complete before and proposed XML snapshots to Kea's native JSON;
2. detects which translated daemon images actually changed and sends
   `config-test` only to those servers before making any change;
3. sends `config-set` to changed DHCPv4 and then changed DHCPv6 images;
4. treats a failed call as outcome-unknown and restores that daemon followed by
   every earlier changed daemon in reverse order;
5. reads every changed daemon back with `config-get` before dangd accepts the
   commit, first requiring the applied XML to match that prepared proposal and
   then requiring every value managed by dangd to match while ignoring
   Kea-added defaults and response metadata;
6. retains both prior configurations for dangd-triggered reverse rollback,
   while reapplying only the changed subset; and
7. reads every restored daemon back after compensation or explicit rollback
   before making the prior snapshot available for operational publication.

This conservative restoration includes a daemon that returned an explicit
error because the same path must also be safe when a reply is lost after Kea
accepted the request. Reapplying the complete before-image is idempotent, and
any restoration failure is appended to the primary transaction error.
An explicit dangd-triggered rollback attempts every changed daemon in reverse
order even if one restoration fails. Its error identifies the first failing
module and carries that module's configuration instance path, while the message
retains failures from every attempted restoration.
Successful rollback commands are not sufficient proof of restoration. The
plugin follows them with `config-get` in the same reverse daemon order and
projects each result onto the prior dangd image. Missing or changed managed
values make rollback fail at the owning module's configuration path and leave
operational publication suppressed. Apply compensation uses the same readback
gate before clearing its pending-mutation marker.
Before sending any command, the transaction helper requires each before-image
and proposed image to have the same module, service, and socket identity at its
index and requires module targets to be unique. A malformed or reordered plan
therefore cannot apply one daemon and compensate a different daemon.
Apply also requires its before-image to match the plugin's accepted snapshot.
Once a changing apply begins, its pending marker retains the complete proposed
daemon images. Until that transaction is reconciled or fully restored, a
second apply, stale rollback, unapplied reconciliation, or unrelated no-op
transaction cannot replace or clear the marker.
An exception from the command implementation is contained as an outcome-
unknown failure and triggers reverse compensation. An exception while restoring
one daemon is reported without preventing restoration of earlier daemons.
Every stateful integer-returning plugin entry point also has a final exception
barrier. Unexpected standard or nonstandard C++ exceptions from schema
retrieval, transaction, hardware-action, operational, or reconciliation work
become attributed plugin failures and cannot cross dangd's C ABI. This last-
resort reporter uses fixed thread-local storage so handling an allocation
failure does not require another allocation.
Every guarded call clears its error descriptor before dispatch. Callbacks with
caller-owned output storage also zero that output before validating inputs, so
a failed call cannot expose pointers, counts, completeness flags, or prepared
transaction handles left by an earlier invocation. A successful call likewise
cannot appear to retain an older diagnostic when the host reuses descriptors.
Dangd remains the definitive configuration authority. Kea lease or host
database backends provide runtime data and storage mechanics; they never
replace the applied dangd snapshot as configuration intent. ABI-v6
reconciliation projects each live `config-get` image onto the corresponding
translated dangd image, so Kea defaults and its read-only content hash do not
create false drift. System-ordered YANG lists are compared by their declared
keys and system-ordered leaf-lists by value because Kea may serialize either in
a different order. The pinned models' user-ordered subnet, pool, prefix-pool,
and client-class lists retain positional comparison, as do arrays embedded in
arbitrary JSON values. Missing, duplicated, changed, or additional managed
values reject the commit with the owning module and configuration path.
Reconciliation also binds the applied XML to the prepared proposal before it
uses that proposal to decide which daemons require readback. A stale prepared
handle, a different applied snapshot, or a reordered target cannot skip the
correct authority check or clear a pending mutation marker.

Every control exchange must return exactly one answer. Empty or multi-answer
transaction replies fail closed, so an ambiguous response can never be treated
as a successful `config-test` or `config-set`. Native result codes are compared
without narrowing integer conversions; oversized malformed values fail through
the ordinary error path rather than escaping the plugin callback. A malformed
non-string native error `text` field is replaced with a descriptive fallback
instead of triggering a JSON type exception. Page counts, host cursors, lease
states, and DHCPv6 lease types use checked unsigned decoding so negative and
oversized values are rejected without narrowing.
Control-socket writes suppress `SIGPIPE`; a Kea process that disconnects while
receiving a command produces a normal plugin error and cannot terminate dangd.
Serialized requests and replies each have a 16 MiB ceiling, and a socket write
that reports zero progress fails immediately rather than spinning. Request JSON
serialization errors are contained before connecting to Kea. The descriptor
remains nonblocking through connect, write, and read, so a peer that stops
consuming a large request cannot extend the exchange past its shared deadline.

Each complete replacement must retain a `unix` control socket whose
`socket-name` exactly matches the corresponding `DANG_KEA_*_SOCKET` path. The
plugin accepts both Kea's current `control-sockets` list and its deprecated
singular `control-socket` container, but rejects a candidate that would remove
or redirect its own management channel before sending `config-test`.
It likewise requires `libdhcp_lease_cmds.so`, `libdhcp_host_cmds.so`, and
`libdhcp_stat_cmds.so` in every replacement because complete operational
retrieval depends on their native commands. Only the basenames are fixed;
Linux and FreeBSD may install them in different directories.

The translator handles ordinary scalar leaves, decimal values, containers,
every list and leaf-list declared by the pinned configuration models,
JSON-valued user contexts, hook parameters, HTTP header values, and DHCP queue
control, IPv4 and IPv6 address pools, and Kea's JSON naming differences for
reservations, databases, hooks, shared networks, loggers, output options, and
prefix-delegation pools. Singleton lists and leaf-lists remain JSON arrays.
The DHCPv6 `server-id` presence container may be explicitly empty because all
of its children are optional; that form translates to an empty JSON object,
not an empty string.
JSON-bearing leaves must contain valid JSON rather than falling back to an
ordinary string. `user-context` and `dhcp-queue-control` additionally require
JSON objects as specified by their pinned model descriptions; hook parameters
and HTTP header values may contain any valid JSON value.
The modeled `database-type` leaf is emitted as Kea's native `type` member for
lease, host, and configuration database objects.
Scalar typing follows the pinned YANG declarations: string leaves remain JSON
strings even when their value looks like `true`, `false`, or a number. Known
decimal64 leaves must parse completely to finite numeric values and cannot fall
back to strings. Boolean leaves accept only the YANG lexical forms `true` and
`false`; other text cannot acquire string semantics. Directly typed unsigned
integer leaves are parsed to unsigned JSON numbers with their modeled 8-, 16-,
or 32-bit range enforced, including the module-specific width of option
`code`.
Kea remains the final implementation-specific validator; a newly introduced
model structure must gain a focused translation test before it is treated as
production-supported.
Malformed XML structures that cannot be converted to Kea's required JSON
types fail with a controlled plugin error. Datastore snapshots have a 16 MiB
ceiling and are rejected before parsing, bounding the XML tree's input rather
than relying on libxml's much larger signed-length interface.
Every descendant of a selected Kea `config` container must remain in that
module's namespace; a foreign element cannot acquire Kea semantics merely by
reusing a recognized local name.
Exactly one configuration container may match each Kea module. Ambiguous
snapshots with duplicate module containers fail instead of silently selecting
the first tree and ignoring the second, and an absent module reports a direct
selection error rather than a misleading missing-socket error.
The container must be the document root or a direct child of a NETCONF
`config` or `data` envelope. A matching local name and namespace buried in an
unrelated wrapper cannot be mistaken for the module's datastore root.
DTD declarations are forbidden, including internal subsets. This prevents
entity references from expanding while configuration text is converted even
when the XML parser's network access is already disabled.
Containers may contain child elements or scalar character data, but not both.
Rejecting mixed content prevents malformed text from being silently discarded
while its neighboring configuration elements are translated.
The selected module tree must not contain XML attributes. The pinned Kea data
models define none, and silently ignoring edit-style or metadata attributes in
a complete datastore snapshot could change the apparent request semantics.
Repeated sibling names are accepted only for lists and leaf-lists declared by
the pinned models. Duplicate singleton leaves or containers fail instead of
being converted into an invented JSON array.
Declared leaf-list entries must be scalar, and unambiguous list entries must
contain their modeled child structure even when only one entry is present.
The `client-class`, `host`, and `subnet` names retain their documented
context-sensitive scalar/list interpretation.

The provider implements the complete configuration and state trees of the two
pinned modules. Through ABI v6 it owns configuration and publishes each
server's complete lease inventory, host reservations (including option data),
and supplemental per-subnet lease statistics in its `state` container. Operational
queries use the local control sockets and convert Kea identifiers, lease types,
states, lifetimes, prefix lengths, and binary identifiers to their modeled XML
forms. Native hardware addresses, client IDs, and DUIDs must be either complete
colon-separated hexadecimal octets or a complete contiguous hexadecimal
string. Empty values and malformed separators fail the complete retrieval
instead of being normalized into a different identity. Lease scalars retain
their pinned YANG types and integer ranges; arrays, objects, wrong primitive
types, and out-of-range integers fail rather than being omitted or stringified.
Kea's empty-set result is exposed as an empty collection.
JSON serialization of lease, host, and option user contexts and paged entries
is exception-contained; malformed native strings fail the operational request.
Every native string is also checked for valid UTF-8 and the XML 1.0 character
range before escaping, so JSON control characters cannot make the claimed-
complete operational result malformed XML.
Duplicate lease addresses, statistic subnet IDs, or composite reservation keys
fail the retrieval rather than producing schema-invalid complete state.
Statistics column names must be unique strings, and every modeled counter must
fit its `uint32` YANG type. These checks are repeated by the final translator so
even state supplied outside the normal bounded collector cannot bypass them.
Reservation identifiers must be non-empty strings, including every identifier
field present in a native host record; malformed secondary identifiers are not
silently discarded. DHCPv6 reservations reject the DHCPv4-only `circuit-id`
and `client-id` types because they cannot be represented by the pinned DHCPv6
model. Reservation scalar fields and option data likewise retain their modeled
string, boolean, `uint32`, DHCPv4 `uint8`, or DHCPv6 `uint16` types and ranges.
Option-data composite keys and reservation address, prefix, excluded-prefix,
and client-class leaf-list values must also be unique within their YANG scope.

The provider marks this operational result complete. Lease enumeration uses
Kea's `lease4-get-page` and `lease6-get-page`
commands with a 256-entry page size and the last returned address as the opaque
continuation cursor. It rejects malformed counts, oversized pages, repeated
cursors including non-adjacent cycles, and oversized inventories. One
production daemon-state read has shared limits of 512 state control queries,
65,536 state entries, 8 MiB of encoded native entry data, and 30 seconds across
lease pages, per-subnet statistics, reservation pages, and HA status; the two
authority checks are also inside that deadline. The deadline is checked both
before and after every control call, so a slow final reply cannot be accepted
after the budget. Each individual control exchange retains its five-second and
16 MiB limits. The final modeled document is independently limited to 16 MiB
to match dangd's operational callback boundary. DHCPv4 and DHCPv6 consume one shared
XML allowance, including the NETCONF data wrapper, so escaping and base64
expansion cannot multiply the accepted native-data budget into an oversized
provider result. Translation checks the remaining allowance after every lease,
statistics row, reservation, and HA relationship. It assembles the
already-bounded fragments directly into the final state string, avoiding an
unbounded complete document or chained concatenation temporaries before
rejection.
Unexpected exceptions from the control-query implementation are contained as
retrieval failures and cannot cross the plugin callback boundary.
The portable adapter test holds a synthetic peer open until the client reports
its five-second deadline, then releases it immediately. This distinguishes a
real transport timeout from peer-initiated EOF without a scheduler-sensitive
fixed sleep.

Before and after collecting operational state for a daemon, the provider reads
its complete live configuration and projects it onto the accepted dangd image.
Missing sockets and out-of-band changes therefore fail at the owning module's
`config` path instead of allowing state from a configuration that dangd did not
accept. The closing check also rejects an assembled result when configuration
drifted during its lease, statistics, and reservation queries. Kea does not
offer one snapshot transaction spanning those commands, so a transient change
that is restored before the closing check cannot be distinguished; the two
authority checks are the strongest available consistency boundary without
stopping the daemon. The accepted configuration and subnet inventory are held
under one shared authority lock for the complete operational collection.
Changing apply and rollback take the exclusive side before their first native
operation, so they wait for existing reads and later reads see the pending
marker. One request therefore cannot overlap hardware mutation or combine
snapshots from two dangd commits. DHCPv4 is verified and collected before
DHCPv6; a later DHCPv6 failure cannot discard the failure attribution or cause
a partial result to be published.
Successful hardware apply does not promote the proposed snapshot. Until ABI-v6
readback succeeds, a retained pending marker makes operational retrieval fail
closed before querying state. The marker is installed before the first changing
`config-set`, so a concurrent read cannot publish a partial apply or candidate
intent before dangd accepts the commit. Reconciliation atomically promotes the
verified image and clears the marker. Complete reverse compensation or rollback
clears it against the prior image; incomplete compensation keeps reads closed.
The marker is transaction-bound: reconciliation must present the proposal that
created it, and a changed prepared proposal without a marker is rejected as
unapplied. Callback reordering or stale prepared handles therefore cannot turn
intent into authority without a corresponding native mutation.

Host reservations use Kea's `reservation-get-page` continuation map and the
same full cursor-cycle detection as leases. Every returned reservation
contributes to the complete read's shared entry and byte limits.
Supplemental statistics are queried once per exact subnet ID in the last
successfully applied configuration, so Kea cannot return an unbounded
all-subnet result. Every row contributes to the complete read's shared entry
and byte limits. Every successful reply must contain one unambiguous row whose
`subnet-id` matches the exact query; mismatched identities, duplicate columns,
multiple rows, or Kea reporting no statistics for an accepted configured
subnet fail the complete retrieval. The latter detects datastore/daemon drift
instead of publishing a misleading complete empty statistics tree.
Candidate validation does not change that inventory; successful reconciliation
and rollback callbacks update it atomically. On initial startup, dangd presents
the restored datastore as an empty-to-persisted transaction. The plugin reads
each daemon's complete native configuration as the rollback image, applies
dangd's persisted authority, and accepts it only after post-apply readback.
If startup fails, normal transaction rollback restores those captured native
images. During direct applied-state recovery without a prepared transaction,
ABI-v6 reconciliation instead reads both live daemon configurations and
requires their managed values to match dangd's persisted snapshot before
rebuilding the subnet inventories. Unavailable or drifted startup state
therefore blocks operational publication instead of silently blessing intent
as applied state.
The pinned ISC modules declare no RPC or notification surface and do not model
HA runtime status. The plugin therefore advertises its own read-only
`dang-kea-ha` module. When the accepted image loads `libdhcp_ha.so`, the same
authoritative read that collects leases and reservations also calls
`status-get` and publishes each DHCPv4 or DHCPv6 relationship's mode, local
name, role, state-machine phase and service scopes, plus the peer name, role,
reachability, interrupted-communication flag, status age, packet/client
monitoring counters, last state, and last scopes. When Kea has completed a
clock comparison, the tree also carries each active member's native UTC sample
and the signed peer-minus-local clock skew; null, not-yet-measured values are
omitted. The age and four traffic-monitoring counters form one required native
sample, while the two UTC samples and signed skew are published together or
omitted together. A partial sample fails the complete operational request
instead of presenting incomplete peer health as authoritative state.
Before publishing those entries, the collector binds every native relationship
to the accepted HA hook configuration by position and mode, then binds both the
local and singular active-remote server names and roles to the configured peer
list. Passive-backup must not report a remote member. A missing, extra, stale,
or cross-member response therefore fails at the HA operational path instead of
being labeled with the current datastore identity.
The same relationship entry publishes an effective `transport-security`
policy from that accepted image. `local-listener` and, where a singular active
peer exists, `active-remote` distinguish plaintext from TLS without exposing
trust-anchor, certificate, or private-key paths. For a TLS listener the model
also reports whether client certificates are required; every listener reports
whether non-HA commands are restricted. These are configuration-derived
posture values, not claims that a handshake is presently healthy. The adapter
applies Kea's per-peer override and empty-string disable rules to the three TLS
files and rejects an incomplete effective triplet rather than labeling the
channel incorrectly.
The status query is omitted when the accepted image has no HA hook. A missing,
rejected, malformed, oversized, or late HA reply fails the complete operational
request at `/{urn:dang:kea:ha}high-availability`; raw native JSON is never
inserted into NETCONF data. Kea does not report a portable synchronization
percentage in this response, so synchronization progress beyond these states
and scopes remains unmodeled.

The plugin deliberately exposes one ABI-v4 hardware action for the entire Kea
transaction. This preserves atomic compensation across Kea's own
complete-configuration `config-set` operation and does not pretend that
individual YANG leaves can be independently committed when Kea accepts
configuration as a unit.

## Configuration

The worker process must inherit a socket path for each enabled family. A
dual-stack deployment uses both:

```sh
export DANG_KEA_INSTANCE_ID=default
export DANG_KEA_DHCP4_SOCKET=/run/kea/kea4-ctrl-socket
export DANG_KEA_DHCP6_SOCKET=/run/kea/kea6-ctrl-socket
```

`DANG_KEA_INSTANCE_ID` defaults to `default` for an existing single-instance
installation. Set it explicitly when more than one local Kea instance is
managed. It must contain 1 through 64 ASCII letters, digits, dots, underscores,
or hyphens and must begin with a letter or digit. The read-only
`dang-kea-instance` tree publishes this identifier and the enabled families so
a NETCONF client can verify the management boundary it reached.

For IPv4-only operation, set only `DANG_KEA_DHCP4_SOCKET` and omit the DHCPv6
variable. For IPv6-only operation, do the reverse. An unset variable explicitly
disables that family; a present but empty variable is an error. At least one
family must be enabled. A disabled family must also be absent from the dangd
datastore, otherwise preparation fails at that module's `config` path rather
than silently dropping its configuration. The inventory is captured when the
plugin library loads and does not change during the process lifetime.

Use `/var/run/kea` on the tested FreeBSD package. The same paths must appear
in each modeled `control-sockets` list so a successful `config-set` keeps the
management channel available. The plugin rejects a replacement that omits or
changes that UNIX socket, and refuses missing or overlong environment paths and
uses a single five-second deadline covering nonblocking connect, write, and read
plus 16 MiB request and response ceilings for each local exchange.
Socket paths containing an embedded NUL are also rejected because the kernel
would otherwise resolve a different, truncated pathname from the identity
validated by the plugin.

On Ubuntu, install `kea-dhcp4-server` and `kea-dhcp6-server`. On FreeBSD,
install the `kea` package. The isolated tests use unique socket and PID paths,
so the packaged services may remain running.

Add a UNIX control socket to each existing Kea configuration. This is the
relevant DHCPv4 fragment, not a complete Kea configuration:

```json
{
  "Dhcp4": {
    "control-sockets": [
      { "socket-type": "unix", "socket-name": "/run/kea/kea4-ctrl-socket" }
    ],
    "hooks-libraries": [
      { "library": "/usr/lib/x86_64-linux-gnu/kea/hooks/libdhcp_lease_cmds.so" },
      { "library": "/usr/lib/x86_64-linux-gnu/kea/hooks/libdhcp_stat_cmds.so" },
      { "library": "/usr/lib/x86_64-linux-gnu/kea/hooks/libdhcp_host_cmds.so" }
    ]
  }
}
```

Use the equivalent `Dhcp6` object and `kea6-ctrl-socket` in the DHCPv6 file
when DHCPv6 is enabled.
On FreeBSD use `/var/run/kea/...` consistently and find the hooks under
`/usr/local/lib/kea/hooks`. Distribution paths can differ; verify the installed
locations rather than copying these examples blindly. All hook entries must
also be represented in the modeled configuration; the plugin rejects a
replacement missing any required command hook so `config-set` cannot silently
remove operational retrieval. Validate both native files before restarting the
servers:

```sh
kea-dhcp4 -t /etc/kea/kea-dhcp4.conf
kea-dhcp6 -t /etc/kea/kea-dhcp6.conf
```

FreeBSD normally stores them under `/usr/local/etc/kea`; pass those paths to
the same checks. Verify both socket files after restart. Put the two
`DANG_KEA_*_SOCKET` variables in the dangd service environment so they survive
reboot, then add the absolute installed `dangd_kea_plugin.so` pathname as a
repeatable `--plugin` option. Find that path with the package listing command
instead of guessing it. Validate the complete launch first:

```sh
dangd --model /path/to/root.yang --config /path/to/config.xml \
  --plugin /absolute/path/dangd_kea_plugin.so --check
```

Start the service with the same environment and arguments. Confirm YANG
Library advertises both Kea server modules plus `dang-kea-instance`. Make and
commit a small candidate change while watching both Kea logs before attempting
a production migration. For multiple local instances, follow the complete
isolation checklist in [DEPLOYMENT.md](DEPLOYMENT.md#multiple-local-instances).

## Troubleshooting and removal

- An unavailable socket means the path is wrong, Kea is stopped, or worker
  permissions do not allow access.
- A successful `config-test` followed by failed `config-set` is a Kea runtime
  rejection. The plugin reports the response and compensates an already
  changed DHCPv4 server.
- Never remove the control socket in the modeled replacement configuration;
  that would cut off the next management operation.

Remove the plugin from dangd and migrate or delete its datastore nodes before
running `sudo apt remove dangd-plugin-kea` or
`sudo pkg delete dangd-plugin-kea`. Package removal leaves Kea and its last
accepted configuration in place.

## Network-safe platform tests

All plugins in this repository must build and test on Linux and FreeBSD. A test
must never select a host LAN interface, wildcard interface, or production DHCP
socket.

The Linux interaction creates a private network namespace and an unconnected
veth pair. Only `lo` and `dangkea0` enter the namespace. It copies the packaged
Kea executables to a temporary pathname to avoid Ubuntu's service-specific
AppArmor attachment, then runs them on high test ports. The FreeBSD interaction
creates an unconnected epair and moves one end into a temporary VNET jail. Only
`lo0` and `dangkea0` enter the jail. Both scripts use documentation-only
addresses (`192.0.2.0/24` and `2001:db8:6::/64`), memory-only lease databases,
and local UNIX control sockets.

Run the scripts as root on disposable validation hosts:

```sh
sudo tests/platform/linux/run_kea_isolated.sh "$PWD"
sudo tests/platform/freebsd/run_kea_isolated.sh "$PWD"
```

Each interaction proves DHCPv4 and DHCPv6 `config-test`, `config-set`, rollback,
paged-command lease and host retrieval, and supplemental-statistics retrieval
against the native packaged daemon. After apply, it injects one real lease into
DHCPv4 and 257 real leases into DHCPv6 through the lease-command hook. The
workflow also adds 256 DHCPv6 reservations to the authoritative dangd
candidate; together with the existing modeled reservation, both native
inventories cross their 256-row page boundaries without introducing
out-of-band configuration. The test requires the final lease and reservation
from the second pages plus the binary hardware address and DUID, DHCPv6 IAID,
and exact per-subnet assigned-lease counters in the modeled operational XML.
It verifies that no
other interface entered the isolation boundary and removes the temporary
memory-backed lease databases, unique sockets, PID storage, and namespace or
jail afterward.
The same native workflow also runs real IPv4-only and IPv6-only transactions.
Each enabled daemon must complete validation, mutation, readback, operational
collection, and rollback while the other family has no configured target or
datastore tree.
It additionally loads the packaged HA hook into both daemons, applies local
hot-standby and passive-backup member configurations, verifies native
`ha-heartbeat` command registration after reconciliation, retrieves both
relationships through the modeled `dang-kea-ha` operational tree, and rolls
each transaction back. The peer endpoint stays unreachable on the isolated
documentation subnet, so hot-standby requires `in-touch=false`.
Passive-backup has no singular active peer, so Kea omits its remote status map
and the modeled relationship correspondingly omits `remote`. These cases test
local-member management without implying pair-wide success.
Before that transaction matrix, each script exercises dangd's real startup
shape with an empty before-image and the persisted Kea datastore as the
candidate. Success proves that both native boot configurations were captured,
the persisted snapshot was applied and read back, and explicit rollback
restored the captured images. Separate direct-recovery cases then drift the
DHCPv4 and DHCPv6 daemons in turn and require rejection at the exact module
configuration path; the fixture restores each complete native image before
continuing.
Each platform script then runs DHCPv4-only and DHCPv6-only transactions with
the opposite, unchanged daemon deliberately pointed at a nonexistent socket.
Successful validation, apply, and rollback therefore prove against packaged
Kea that the plugin does not contact either unchanged daemon. With both sockets
absent, each script also requires a DHCPv6-only validation failure to carry the
DHCPv6 module and exact configuration path, proving the unchanged DHCPv4 image
is not consulted. No-op transactions with first DHCPv4 and then DHCPv6
unavailable require complete operational requests to fail at the matching
configuration path. The DHCPv6 case first completes every DHCPv4 state query,
proving structural attribution in both directions.
The native workflow also lets both daemons accept a full candidate, then in
separate cases removes each daemon's managed host-command hook before ABI-v6
readback. Post-apply reconciliation must reject the commit at the owning
module's configuration path; the DHCPv6 case first proves that DHCPv4 readback
succeeds. Each ensuing hardware rollback must restore the complete before-image.
The workflow also reconciles configured DHCPv4 and DHCPv6 subnets absent from
the live boot daemons and requires complete state to fail at the owning
module's `config` path, proving that native configuration drift cannot
masquerade as complete state. The DHCPv6 case first completes all DHCPv4 state
queries.
It then removes each daemon's required host-command hook through Kea's control
API and requires the authority check to fail at that module's `config` path
before any state from the drifted daemon is published. The DHCPv6 case first
completes all DHCPv4 state queries. The following full transaction restores
both hooks.
Each script then
lets a DHCPv4-only `config-test` succeed, removes the changed daemon's socket
before apply, and requires both the failed apply and failed conservative
compensation to carry the DHCPv4 module and configuration instance path.
Finally, it applies a DHCPv6-only change, removes that daemon's socket, and
requires rollback to fail with the corresponding DHCPv6 attribution. These
failure cases run last because removing bound UNIX sockets intentionally makes
the test daemons unreachable until cleanup.

### Cross-host VLAN interaction

The bidirectional interaction additionally uses a Linux host and a FreeBSD host
whose secondary interfaces share a sterile VLAN. It refuses an interface that
carries an IPv4 or IPv6 default route, adds only temporary documentation-prefix
addresses, starts memory-only Kea servers on nonstandard ports, and reverses
the server/client roles. The client is a small Python socket implementation,
not an operating-system DHCP client, so it cannot replace addresses, routes,
DNS configuration, or other host state.

The client sends a DHCPv4 INIT-REBOOT request and a DHCPv6 Rapid Commit solicit.
The authoritative assertion is the serving Kea process recording completed
IPv4 and IPv6 lease allocations. This remains reliable on isolated
hypervisors that filter return UDP packets to the disposable nonstandard client
ports. The test therefore proves cross-host request delivery and native server
allocation in both role directions. Client identifiers use the selected
interface's actual MAC address, IPv4 uses Kea's UDP socket mode, and a visible
DHCPv6 Rapid Commit reply is validated whenever it reaches the client. On the
current VLAN the IPv6 reply passes in both directions, while the nonstandard-
port IPv4 return packet remains filtered; completed Kea allocation is therefore
the portable IPv4 pass criterion. A visible reply must match the request
transaction and expected server endpoint. DHCPv4 additionally checks the
server identifier and exact configured address range; DHCPv6 checks the client
DUID, server identifier presence, IAID, and allocated address range.

Both hosts need Python 3, Kea DHCPv4/DHCPv6, passwordless test-only `sudo`, and
SSH/SCP access from the orchestrating system. For the current validation pair:

```sh
tests/platform/run_kea_cross_host.sh \
  dev-linux-1 dev-freebsd-1 ens19 vtnet1
```

The orchestrator copies its two endpoint helpers to unique `/tmp` paths, runs
Linux-server/FreeBSD-client and FreeBSD-server/Linux-client phases, and removes
all remote helpers and runtime state through its exit trap.

The HA interaction uses those same guarded interfaces for a real hot-standby
pair. Each Kea daemon keeps its management API on a local UNIX socket and uses
only the hook's restricted dedicated HTTP listener for peer traffic. The
orchestrator waits for both DHCPv4 and DHCPv6 relationships to enter
`hot-standby`, sends one allocation of each family to the primary, and requires
the same deterministic leases in both members' local databases. It then stops
both primary daemons while retaining only the disposable interface aliases.
The standby must report interrupted communication and the partner unavailable
before the harness uses the local `ha-scopes` command to activate the stopped
primary's scope. A second client must then create distinct IPv4 and IPv6 leases
on the survivor. For recovery, the survivor must relinquish that manual scope
before the primary restarts. This introduces a bounded service gap while
ensuring that two servers never answer the same scope. The restarted primary
has an empty memory database, so it must recover the outage leases from its
partner, return to normal hot-standby, and replicate a distinct third-client
allocation to both members. The harness then destroys the memory-only pair,
reverses the Linux and FreeBSD roles, and repeats:

```sh
tests/platform/run_kea_ha_cross_host.sh \
  dev-linux-1 dev-freebsd-1 ens19 vtnet1
```

This proves cross-version peer communication, initial synchronization, normal
service scope activation, bidirectional lease replication, outage recognition,
guarded manual takeover, rejoin synchronization, and restored replication. It
deliberately keeps automatic failover disabled, never adds the primary scope
until the primary processes have exited, and removes that scope before they
restart, avoiding the split-brain risk documented for `ha-scopes`. It does not
exercise a pair-wide dangd commit or claim distributed transaction atomicity.
As with the basic VLAN interaction, the cleanup trap removes the
documentation-prefix aliases, sockets, processes, configurations, and copied
helpers after success or failure.

The optional `automatic` mode exercises Kea's native failure transition rather
than issuing `ha-scopes`:

```sh
tests/platform/run_kea_ha_cross_host.sh \
  dev-linux-1 dev-freebsd-1 ens19 vtnet1 automatic
```

This isolated test sets `auto-failover` on both peers and uses
`max-unacked-clients: 0`, so a survivor that loses the dedicated HA channel
enters `partner-down` after the bounded response delay. The test requires that
state and the primary scope before sending outage traffic, then proves the same
rejoin synchronization and post-recovery replication as the manual mode. A
zero threshold is not a general production recommendation: deployments where
the peer channel can partition while both servers remain alive must select
traffic-aware failure thresholds to prevent both members entering
`partner-down`.

Planned maintenance uses the same harness with a third mode:

```sh
tests/platform/run_kea_ha_cross_host.sh \
  dev-linux-1 dev-freebsd-1 ens19 vtnet1 maintenance
```

Before stopping anything, this mode requires the survivor to enter
`partner-in-maintenance` with the primary scope and the maintained primary to
enter `in-maintenance` with no scope. It then proves survivor service,
restart-time lease synchronization, and restored replication in both operating
system role assignments. The tested package pair completed the handshake but
cleared the survivor scope after shutdown when `auto-failover` was disabled;
the continuous-service profile therefore enables it. Validate the chosen Kea
versions and production failure thresholds before adopting this procedure.

Append `tls` after the HA mode to run peer communication over mutually
authenticated HTTPS:

```sh
tests/platform/run_kea_ha_cross_host.sh \
  dev-linux-1 dev-freebsd-1 ens19 vtnet1 automatic tls
```

The harness generates a disposable CA and IP-bound certificate for each host,
requires client certificates on the dedicated HA listeners, and applies the
same replication, failover, resynchronization, and role-reversal assertions.
Each DHCPv4 and DHCPv6 listener must additionally reject a CA-validating client
that presents no certificate, a client certificate issued by a separate rogue
CA, and plaintext HTTP. This distinguishes client-certificate presence from
actual trust and checks that the TLS ports do not silently allow a cleartext
downgrade. Issuance occurs on the slower test-host clock to avoid a transient
not-yet-valid failure. Temporary keys and certificates are removed from the
orchestrator and both hosts on success or failure. These credentials are only
test fixtures; production installations need synchronized clocks and their
normal protected PKI lifecycle.

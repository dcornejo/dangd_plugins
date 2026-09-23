<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Kea DHCP plugin

`dangd_kea_plugin` manages one Kea DHCPv4 server and one Kea DHCPv6 server
through their local UNIX control sockets. It embeds the official Kea 3.2.0
`kea-dhcp4-server` and `kea-dhcp6-server` modules at revision 2026-06-24 plus
their pinned Kea type modules. The original model files retain ISC's MPL-2.0
license notices; the adapter code is Apache-2.0.

## Dependencies and installation

The runtime requires dangd 0.1.0 or newer, Kea 3.2.x DHCPv4 and DHCPv6
servers, the Kea lease-command, host-command, and supplemental-statistics hook
libraries, and local UNIX control sockets accessible by the plugin worker. On
Debian/Ubuntu install `kea-dhcp4-server` and `kea-dhcp6-server`; on FreeBSD
install `kea`. A source build additionally needs CMake 3.24+, a C++20 compiler,
libxml2 development files, nlohmann-json 3.11+, and GoogleTest.

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

## Transaction behavior

For every affected commit, the plugin:

1. converts the complete before and proposed XML snapshots to Kea's native JSON;
2. detects which translated daemon images actually changed and sends
   `config-test` only to those servers before making any change;
3. sends `config-set` to changed DHCPv4 and then changed DHCPv6 images;
4. treats a failed call as outcome-unknown and restores that daemon followed by
   every earlier changed daemon in reverse order; and
5. retains both prior configurations for dangd-triggered reverse rollback,
   while reapplying only the changed subset.

This conservative restoration includes a daemon that returned an explicit
error because the same path must also be safe when a reply is lost after Kea
accepted the request. Reapplying the complete before-image is idempotent, and
any restoration failure is appended to the primary transaction error.
An explicit dangd-triggered rollback attempts every changed daemon in reverse
order even if one restoration fails. Its error identifies the first failing
module and carries that module's configuration instance path, while the message
retains failures from every attempted restoration.
Before sending any command, the transaction helper requires each before-image
and proposed image to have the same module, service, and socket identity at its
index and requires module targets to be unique. A malformed or reordered plan
therefore cannot apply one daemon and compensate a different daemon.
An exception from the command implementation is contained as an outcome-
unknown failure and triggers reverse compensation. An exception while restoring
one daemon is reported without preventing restoration of earlier daemons.

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
cursors including non-adjacent cycles, more than 512 pages or 65,536 leases,
more than 8 MiB of accumulated native lease data, and enumeration lasting more
than 30 seconds. The aggregate deadline is checked both before and after every
control call, so a slow final reply cannot be accepted after the budget. Each
individual control exchange retains its five-second and 16 MiB limits.
Unexpected exceptions from the control-query implementation are contained as
retrieval failures and cannot cross the plugin callback boundary.

Host reservations use Kea's `reservation-get-page` continuation map and the
same aggregate safeguards and full cursor-cycle detection as leases.
Supplemental statistics are queried once per exact subnet ID in the last
successfully applied configuration, so Kea cannot return an unbounded
all-subnet result. The provider combines those results under the same 512-query,
65,536-row, 8 MiB, and 30-second aggregate limits. Every successful reply must
contain one unambiguous row whose
`subnet-id` matches the exact query; mismatched identities, duplicate columns,
or multiple rows fail the complete retrieval. Candidate validation does not
change that inventory; successful apply
and rollback callbacks update it atomically. On startup, ABI-v6 applied-state
reconciliation rebuilds both subnet inventories from dangd's accepted snapshot
before operational retrieval, so a restart cannot silently omit statistics.
The pinned modules declare no
RPC or notification surface.

The plugin deliberately exposes one ABI-v4 hardware action for the entire Kea
transaction. This preserves atomic compensation across Kea's own
complete-configuration `config-set` operation and does not pretend that
individual YANG leaves can be independently committed when Kea accepts
configuration as a unit.

## Configuration

The worker process must inherit two explicit socket paths:

```sh
export DANG_KEA_DHCP4_SOCKET=/run/kea/kea4-ctrl-socket
export DANG_KEA_DHCP6_SOCKET=/run/kea/kea6-ctrl-socket
```

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

Use the equivalent `Dhcp6` object and `kea6-ctrl-socket` in the DHCPv6 file.
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
Library advertises both Kea server modules. Make and commit a small candidate
change while watching both Kea logs before attempting a production migration.

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
each daemon through the lease-command hook and requires both addresses plus the
binary hardware address and DUID, DHCPv6 IAID, and incremented per-subnet
assigned-lease counters in the modeled operational XML. It verifies that no
other interface entered the isolation boundary and removes the temporary
memory-backed lease databases, unique sockets, PID storage, and namespace or
jail afterward.
Each platform script then runs a DHCPv6-only transaction with the unchanged
DHCPv4 configuration deliberately pointed at a nonexistent socket. Successful
validation, apply, and rollback therefore prove against packaged Kea that the
plugin does not contact an unchanged daemon.

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

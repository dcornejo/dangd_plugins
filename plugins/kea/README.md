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
2. sends `config-test` to both servers before making any change;
3. sends `config-set` to DHCPv4 and then DHCPv6;
4. restores DHCPv4 immediately if the DHCPv6 application fails; and
5. retains both prior configurations for dangd-triggered reverse rollback.

The translator handles ordinary scalar leaves, decimal values, containers,
every list and leaf-list declared by the pinned configuration models,
JSON-valued user contexts, hook parameters, HTTP header values, and DHCP queue
control, IPv4 and IPv6 address pools, and Kea's JSON naming differences for
reservations, databases, hooks, shared networks, loggers, output options, and
prefix-delegation pools. Singleton lists and leaf-lists remain JSON arrays.
Kea remains the final implementation-specific validator; a newly introduced
model structure must gain a focused translation test before it is treated as
production-supported.

This is not yet a complete implementation of the two modules. Through ABI v3,
the provider owns configuration and publishes each server's lease inventory,
host reservations, and supplemental per-subnet lease statistics in its `state`
container. Operational
queries use the local control sockets and convert Kea identifiers, lease types,
states, lifetimes, prefix lengths, and binary identifiers to their modeled XML
forms. Kea's empty-set result is exposed as an empty collection.

The provider intentionally reports selected rather than complete operational
data. Lease enumeration uses Kea's `lease4-get-page` and `lease6-get-page`
commands with a 256-entry page size and the last returned address as the opaque
continuation cursor. It rejects malformed counts, oversized pages, repeated
cursors, more than 512 pages or 65,536 leases, more than 8 MiB of accumulated
native lease data, and enumeration lasting more than 30 seconds. Each individual
control exchange retains its five-second and 16 MiB limits.

Host reservations use Kea's `reservation-get-page` continuation map and the
same aggregate safeguards as leases. The supplemental statistics command can
still produce a large single reply when a server has many subnets. Completing
the provider requires bounded statistics ranges and full-schema conformance and
interoperability coverage. The pinned modules declare no notification surface.

The plugin deliberately uses ABI v1's transaction-wide action. This preserves
atomic compensation across Kea's own complete-configuration `config-set`
operation. A future ABI-v4 implementation must not pretend that individual YANG
leaves can be independently committed when Kea accepts configuration as a unit.

## Configuration

The worker process must inherit two explicit socket paths:

```sh
export DANG_KEA_DHCP4_SOCKET=/run/kea/kea4-ctrl-socket
export DANG_KEA_DHCP6_SOCKET=/run/kea/kea6-ctrl-socket
```

Use `/var/run/kea` on the tested FreeBSD package. The same paths should appear
in each modeled `control-sockets` list so a successful `config-set` keeps the
management channel available. The plugin refuses missing or overlong paths and
uses a single five-second deadline plus a 16 MiB response ceiling for each local
exchange.

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
also be represented in the modeled configuration so `config-set` retains the
operational commands. Validate both native files before restarting the servers:

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
against the native packaged daemon. It verifies that no other interface entered the isolation
boundary and removes its unique sockets, PID storage, and namespace or jail
afterward.

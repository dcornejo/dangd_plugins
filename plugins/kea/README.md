<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Kea DHCP plugin

`dangd_kea_plugin` manages one Kea DHCPv4 server and one Kea DHCPv6 server
through their local UNIX control sockets. It embeds the official Kea 3.2.0
`kea-dhcp4-server` and `kea-dhcp6-server` modules at revision 2026-06-24 plus
their pinned Kea type modules. The original model files retain ISC's MPL-2.0
license notices; the adapter code is Apache-2.0.

## Transaction behavior

For every affected commit, the plugin:

1. converts the complete before and proposed XML snapshots to Kea's native JSON;
2. sends `config-test` to both servers before making any change;
3. sends `config-set` to DHCPv4 and then DHCPv6;
4. restores DHCPv4 immediately if the DHCPv6 application fails; and
5. retains both prior configurations for dangd-triggered reverse rollback.

The current translator handles ordinary scalar leaves, containers, leaf-lists,
the model's common lists, JSON-valued user contexts and hook parameters, IPv4
and IPv6 address pools, and the naming differences used by shared networks,
loggers, output options, and prefix-delegation pools. Kea remains the final
implementation-specific validator. More specialized Kea structures must gain a
focused translation test before being treated as production-supported.

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
install the `kea` package. Package services must be stopped while running the
isolated test because the tests start private instances.

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

Each interaction proves DHCPv4 and DHCPv6 `config-test`, `config-set`, and
rollback against the native packaged daemon, verifies that no other interface
entered the isolation boundary, and removes the namespace or jail afterward.

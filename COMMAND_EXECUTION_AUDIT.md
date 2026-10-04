<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Production command-execution audit

This audit tracks operating-system work performed by spawning command-line
programs from production plugin code. Test harnesses, packaging scripts, and
operator examples are inventoried separately because they do not execute in a
loaded dangd provider. The required end state is a programmatic library,
kernel, socket, or daemon API wherever one exists. An unavoidable process
fallback must use fixed validated argv without a shell and retain explicit
failure and rollback tests.

## Current production findings

- `example`: no operating-system mutation and no process creation.
- `kea`: no process creation. Configuration, status, leases, hosts,
  statistics, and HA operations use Kea's authenticated control socket.
- `frr`: no process creation. Schema discovery, configuration, operational
  reads, RPCs, and notifications use FRR's native mgmtd socket protocol.
- `ip_management`: no process creation. Linux uses rtnetlink; FreeBSD uses
  interface ioctls and route netlink.
- `vpp`: no process creation. Inventory uses system interfaces and mutations
  use VPP's generated VAPI client.
- `system`: restart and shutdown request an orderly transition through the
  documented PID 1 signal interface on each platform: systemd real-time
  signals on Linux and init signals on FreeBSD. The existing explicit
  deployment guard remains. Linux NTP service control now calls the systemd
  manager over sd-bus, subscribes before enqueueing the unit job, correlates
  its object path with `JobRemoved`, and accepts only the `done` result under a
  bounded timeout. FreeBSD base ntpd exposes no service-manager socket or
  library API; its documented lifecycle boundary is service(8) and rc.d. The
  provider therefore uses `posix_spawn(3)` with the absolute
  `/usr/sbin/service` path and the fixed argv `ntpd onerestart` or
  `ntpd onestop`. It never constructs or invokes a shell command.
- `rib`: no process creation. Linux and FreeBSD production mutation construct
  bounded route-netlink requests, resolve interfaces to native indexes, and
  wait for the matching kernel acknowledgement under a receive timeout.
  Kernel rejection is returned as the transaction failure and completed work
  is still compensated in reverse order. The argv builders remain only as
  deterministic unit-test adapters; production executes neither `ip(8)` nor
  `route(8)`. FreeBSD interface-only routes now use `RTA_OIF` directly and no
  longer require a local address on the selected interface.

## Ordered remediation

1. [x] Replace Linux RIB mutation with bounded rtnetlink requests and
   correlated kernel acknowledgements while preserving action ordering, error
   attribution, and reverse compensation.
2. [x] Replace FreeBSD RIB mutation with bounded route-netlink requests and
   correlated acknowledgements. The installed ABI expresses destinations,
   FIBs, gateways, interface indexes, and preference for the complete supported
   slice, so no routing-socket fallback is required. Native VNET tests cover
   IPv4, IPv6, interface-only mutation on an unnumbered interface, plugin
   integration, deletion, and attributed preflight rejection.
3. [x] Replace Linux NTP service control with the systemd D-Bus manager API.
   The provider uses `ReloadOrRestartUnit` when enabled and `StopUnit` when
   disabled, waits for the correlated completion signal, and propagates both
   method and job-result failures. The lifecycle test restores chrony's exact
   initial active state. This does not change the documented requirement for
   exclusive ownership and migration when the future RFC 9249 plugin arrives.
4. [x] Inspect FreeBSD's base ntpd service boundary and replace `std::system()`.
   FreeBSD provides service(8) and rc.d as the documented stable lifecycle
   interface, not a service-manager library or socket API. The narrow fallback
   now executes only the absolute path and fixed argv described above, reports
   spawn, wait, exit-status, and signal failures, and is covered by status
   interpretation and live start/stop/restore tests.
5. [x] Re-run the source scan and native Linux/FreeBSD suites after the final
   removal. The production scan finds no shell execution and exactly one
   process boundary: the documented FreeBSD service(8) fallback above. The
   complete suites pass on Linux and FreeBSD, including native route and NTP
   lifecycle tests. The production plugin command-execution audit is complete.

Core dangd worker supervision is outside this plugin audit: it intentionally
uses `posix_spawn(3)` as the programmatic process API for privilege and crash
isolation, not to perform a plugin's operating-system configuration work.

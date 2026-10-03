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
- `system`: restart and shutdown now request an orderly transition through the
  documented PID 1 signal interface on each platform: systemd real-time
  signals on Linux and init signals on FreeBSD. The existing explicit
  deployment guard remains. Two `std::system()` sites remain for NTP
  reload/stop during apply and rollback. The command text is host-owned and
  fixed rather than derived from modeled input, but it still invokes a shell
  and must be removed.
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
3. [ ] Replace Linux NTP service control with the systemd D-Bus manager API. This
   must not make the RFC 7317 provider silently claim ownership when the future
   RFC 9249 plugin owns the same service.
4. [ ] Determine whether FreeBSD exposes a stable service-management interface for
   ntpd. If rc scripts remain the only supported boundary, replace
   `std::system()` with an absolute, fixed argv execution helper and document
   that narrow exception, its exit/signal behavior, and rollback coverage.
5. [ ] Re-run the source scan and native Linux/FreeBSD suites after each removal.
   The current scan finds only the two recorded RFC 7317 NTP `std::system()`
   calls. The audit is complete only when every remaining process boundary is
   listed here with evidence that no suitable programmatic interface exists.

Core dangd worker supervision is outside this plugin audit: it intentionally
uses `posix_spawn(3)` as the programmatic process API for privilege and crash
isolation, not to perform a plugin's operating-system configuration work.

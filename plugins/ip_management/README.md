<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# RFC 8343/8344 IP-management plugin

`dangd_ip_management_plugin` owns `ietf-interfaces` revision 2018-02-20 and
`ietf-ip` revision 2018-02-22. It preserves the historical plugin name
`dangd-ip-management` and ABI-v4 contract after moving from the dangd source
repository. It also supplies the official `iana-if-type` revision 2026-03-17
as an import-only identity module; it is schema support, not a second runtime
owner.

The Linux backend uses acknowledged rtnetlink operations. The FreeBSD backend
uses native interface ioctls and route netlink. Both reconcile enabled state,
MTUs, IPv4/IPv6 addresses, and static neighbors, retain rollback information,
and publish live interface and IP operational data. Interface creation and
deletion are intentionally unsupported.

Operational replies include both the RFC 8343 NMDA `/interfaces` tree and the
deprecated `/interfaces-state` compatibility tree. System-created interfaces
appear in `/interfaces` even when they have no intended configuration. This is
required by RFC 8343 and lets current models such as RFC 8431 resolve
`interface-ref` leafrefs through `/interfaces/interface/name`. IPv4 and IPv6
address and neighbor nodes carry the `ietf-ip` namespace explicitly, and only
IPv6 address entries publish the model's address-status leaf.

## Build and install

Install a C++20 compiler, CMake, pugixml, nlohmann-json, GoogleTest for tests,
and an installed dangd development header. Then run:

```sh
cmake -S . -B build -DDANGD_ROOT=/path/to/dang -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build -R ip_management --output-on-failure
sudo cmake --install build --component ip-management
```

Load `dangd_ip_management_plugin.so` from dangd's plugin directory. Applying
configuration requires the operating-system privileges needed to modify the
selected interfaces. Never validate mutations against a management or LAN
interface; use a Linux network namespace or FreeBSD VNET jail with disposable
interfaces.

The native test executable is safe by default: mutation cases skip unless
`DANG_RUN_PRIVILEGED_IP_TESTS=1` is set inside such isolation. Read-only live
operational-state tests run normally.

The native RFC 8431 integration test loads this plugin beside the RIB plugin
inside a Linux network namespace or FreeBSD VNET jail. It proves that a real
NETCONF candidate commit can compose the live interface inventory with native
route state without adding plugin-specific behavior to dangd.

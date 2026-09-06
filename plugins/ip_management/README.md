<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# RFC 8343/8344 IP-management plugin

`dangd_ip_management_plugin` owns `ietf-interfaces` revision 2018-02-20 and
`ietf-ip` revision 2018-02-22. It preserves the historical plugin name
`dangd-ip-management` and ABI-v4 contract after moving from the dangd source
repository.

The Linux backend uses acknowledged rtnetlink operations. The FreeBSD backend
uses native interface ioctls and route netlink. Both reconcile enabled state,
MTUs, IPv4/IPv6 addresses, and static neighbors, retain rollback information,
and publish live interface and IP operational data. Interface creation and
deletion are intentionally unsupported.

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

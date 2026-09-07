<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Native packages

The plugin collection produces independent native packages from one
componentized CMake install manifest. Installing one provider no longer pulls
in unrelated models, daemons, or authentication code:

- `dangd-plugin-example` contains only the ABI example and its model.
- `dangd-plugin-kea` contains the Kea provider, its models, and its guide.
- `dangd-plugin-ip-management` contains the RFC 8343/RFC 8344 interface and IP
  management provider, its models, and its guide.
- `dangd-plugin-system` contains the RFC 7317 provider, models, and guide.
- `dangd-plugin-frr` contains the version-matched native FRR routing provider
  and depends on the platform FRR package.
- `dangd-pam` contains only `pam_dangd` and depends on
  `dangd-plugin-system`, which owns its authentication service.
- `dangd-plugin-rib` contains the partial RFC 8431 runtime provider, schema, and
  guide. It conflicts with `dangd-plugin-frr` through the shared `routing`
  resource domain.
- `dangd-plugins-doc` contains collection-wide documentation and the license.

Each component package also installs its operator guide as `EXAMPLE.md`,
`KEA.md`, `IP-MANAGEMENT.md`, `SYSTEM.md`, `PAM.md`, `FRR.md`, or `RIB.md`.
Read it before connecting the component to a live dangd service. Package
filenames below use version 0.1.0 as an example; substitute the version and
architecture actually built.

Provider packages depend on dangd for the plugin ABI. The Debian FRR and RIB
packages declare symmetric `Conflicts`. Both packages also own the same
`share/dangd/resource-domains/routing` marker, which makes FreeBSD `pkg` reject
co-installation even though CPack's FreeBSD generator cannot emit its native
conflicts field. Package metadata is only an administrative aid: dangd still
rejects duplicate runtime ownership when providers are installed manually.

Build on Debian with:

```sh
cmake -S . -B build-package -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DDANGD_ROOT=/path/to/dang
cmake --build build-package
ctest --test-dir build-package --output-on-failure
cpack --config build-package/CPackConfig.cmake -G DEB
dpkg-deb --info dangd-plugin-kea_0.1.0_amd64.deb
```

The CPack FreeBSD generator cannot emit component packages in one invocation.
The configure step therefore writes one complete configuration per package.
Build all eight with:

```sh
for component in example kea ip-management system pam frr rib docs; do
  cpack --config "build-package/CPackFreeBSD-${component}.cmake"
done
pkg info -F dangd-plugin-kea-0.1.0.pkg
```

Before publishing FRR or RIB packages, inspect their manifests and verify that
both contain `share/dangd/resource-domains/routing`. Installing one while the
other is present must be rejected by `pkg` as a file conflict.

To validate an individual component before packaging it, install into a fresh
staging prefix with `cmake --install build-package --prefix /tmp/stage
--component kea` and inspect the resulting tree. A component installation must
not contain files belonging to any other package.

Packaging does not enable PAM, alter sshd, configure Kea, or start dangd.
Administrators must explicitly connect each installed plugin and PAM policy to
their deployment. Package validation must follow the repository rule that no
test attaches to a LAN interface.

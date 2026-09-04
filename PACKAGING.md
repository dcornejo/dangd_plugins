<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Native packages

The plugin collection produces independent native packages from one
componentized CMake install manifest. Installing one provider no longer pulls
in unrelated models, daemons, or authentication code:

- `dangd-plugin-example` contains only the ABI example and its model.
- `dangd-plugin-kea` contains the Kea provider, its models, and its guide.
- `dangd-plugin-system` contains the RFC 7317 provider, models, and guide.
- `dangd-pam` contains only `pam_dangd` and depends on
  `dangd-plugin-system`, which owns its authentication service.
- `dangd-rfc8431-models` contains the schema and guide for the RIB provider
  while that provider remains under development.
- `dangd-plugins-doc` contains collection-wide documentation and the license.

Provider packages depend on dangd for the plugin ABI. These boundaries also
allow mutually exclusive implementations of the same YANG module to declare a
package conflict without preventing installation of unrelated plugins. Package
metadata is only an administrative aid: dangd must still reject duplicate
runtime ownership of a module.

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
Build all six with:

```sh
for component in example kea system pam rib docs; do
  cpack --config "build-package/CPackFreeBSD-${component}.cmake"
done
pkg info -F dangd-plugin-kea-0.1.0.pkg
```

To validate an individual component before packaging it, install into a fresh
staging prefix with `cmake --install build-package --prefix /tmp/stage
--component kea` and inspect the resulting tree. A component installation must
not contain files belonging to any other package.

Packaging does not enable PAM, alter sshd, configure Kea, or start dangd.
Administrators must explicitly connect each installed plugin and PAM policy to
their deployment. Package validation must follow the repository rule that no
test attaches to a LAN interface.

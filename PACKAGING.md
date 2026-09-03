<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Native packages

The plugin collection produces native Debian and FreeBSD packages from its
CMake install manifest. The package contains the example, Kea, and RFC 7317
system plugins, their YANG models, and `pam_dangd` on Linux and FreeBSD. It
depends on the matching or newer `dangd` package for the plugin ABI.

Build on Debian with:

```sh
cmake -S . -B build-package -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DDANGD_ROOT=/path/to/dang
cmake --build build-package
ctest --test-dir build-package --output-on-failure
cpack --config build-package/CPackConfig.cmake -G DEB
dpkg-deb --info dangd-plugins_0.1.0_amd64.deb
```

Build on FreeBSD with the same configure, build, and test steps followed by:

```sh
cpack --config build-package/CPackConfig.cmake -G FREEBSD
pkg info -F dangd-plugins-0.1.0.pkg
```

Packaging does not enable PAM, alter sshd, configure Kea, or start dangd.
Administrators must explicitly connect each installed plugin and PAM policy to
their deployment. Package validation must follow the repository rule that no
test attaches to a LAN interface.

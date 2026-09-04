<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Example plugin installation and use

`dangd_example_external_plugin` is a deliberately small ABI-v1 provider for
developers and evaluators. It owns the `dang-plugins-example` module, validates
its configuration, describes the resulting action, and exercises commit and
rollback. It does not configure an operating-system service or network device.

## Dependencies

- a compatible `dangd` installation (version 0.1.0 or newer for these
  packages);
- Linux or FreeBSD; and
- only when building from source: CMake 3.24+, a C++20 compiler, Ninja or Make,
  libxml2 development files, nlohmann-json 3.11+, and GoogleTest for tests.

## Install a native package

On Debian or Ubuntu, copy the generated package to the host and run:

```sh
sudo apt install ./dangd-plugin-example_0.1.0_amd64.deb
dpkg -L dangd-plugin-example
```

On FreeBSD:

```sh
sudo pkg add ./dangd-plugin-example-0.1.0.pkg
pkg info -l dangd-plugin-example
```

The package installs the shared object below the platform's
`lib/dangd/plugins` directory and installs `dang-plugins-example.yang` below
`share/yang/modules`. The listing command above gives the exact paths on the
installed host.

## Build and install only this plugin

From the `dang_plugins` checkout:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DDANGD_ROOT=/path/to/dang
cmake --build build --target dangd_example_external_plugin
ctest --test-dir build -R external_example_plugin_discovery \
  --output-on-failure
sudo cmake --install build --component example
```

Use `-DDANGD_INCLUDE_DIR=/path/containing/dangd` instead of `DANGD_ROOT` when
building against installed development headers.

## Load and verify

Add one `--plugin` option to the normal dangd command. Obtain the authoritative
shared-object pathname with `dpkg -L` or `pkg info -l`, then run dangd's
non-mutating check first:

```sh
dangd --model /path/to/root.yang --config /path/to/config.xml \
  --plugin /absolute/path/dangd_example_external_plugin.so --check
```

Start dangd with the same option after the check succeeds. Confirm that YANG
Library advertises `dang-plugins-example` and use NETCONF `get-schema` to
retrieve it. A duplicate-ownership startup error means another plugin claims
the same implemented module; remove one of the two `--plugin` options.

## Remove

Remove the plugin option from the service or launch command before uninstalling
the package. Then use `sudo apt remove dangd-plugin-example` or
`sudo pkg delete dangd-plugin-example`. Existing datastore nodes from this
module must be removed or migrated before restarting without its schema.

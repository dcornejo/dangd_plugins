<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# RFC 7317 system plugin and PAM adapter

`dangd_system_plugin` implements the 2014-08-06 `ietf-system` module from
RFC 7317. The exact IETF source and its `iana-crypt-hash` import are embedded
in the plugin and returned through the dangd model-source interface.

The plugin advertises `authentication`, `local-users`, `ntp`, and
`timezone-name`. RADIUS and configurable DNS/NTP ports are intentionally not
advertised. It builds only on Linux and FreeBSD, and both native variants are
required to pass the same parsing, transaction, operational-state, and PAM
tests.

## Dependencies, privileges, and installation

The runtime requires dangd 0.1.0 or newer, libxml2, the platform `crypt(3)`
library, and either chrony plus libsystemd on Linux or base ntpd on FreeBSD.
Production changes to hostname, clock, resolver, accounts, and services require
the privileged dangd deployment identity. Protect NETCONF with authenticated
transport and NACM; do not reuse this identity for ordinary shell or
application access.

Debian/Ubuntu:

```sh
sudo apt update
sudo apt install chrony
sudo apt install ./dangd-plugin-system_0.1.0_amd64.deb
dpkg -L dangd-plugin-system
```

FreeBSD:

```sh
sudo pkg add ./dangd-plugin-system-0.1.0.pkg
pkg info -l dangd-plugin-system
```

Source builds also need CMake 3.24+, a C++20 compiler, PAM/crypt/libxml2
development headers, nlohmann-json 3.11+, and GoogleTest. Linux builds also
need `pkg-config` and the `libsystemd-dev` package. Install only this component
with:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DDANGD_ROOT=/path/to/dang
cmake --build build --target dangd_system_plugin system_config_test
ctest --test-dir build -R 'system_' --output-on-failure
sudo cmake --install build --component system
```

Use the package listing to find `dangd_system_plugin.so`, add its absolute path
to dangd with `--plugin`, and run the complete command with `--check` before
restarting the service.

## Implemented behavior

- `contact` and `location` are retained in the dangd datastore.
- `hostname` changes the live kernel hostname and the native persistent
  hostname file (`/etc/hostname` on Linux or a dedicated rc.conf fragment on
  FreeBSD).
- named timezones copy a validated TZ database file to `/etc/localtime`;
  UTC offsets generate a fixed-offset TZif file, including minute offsets.
- NTP configuration is rendered for chrony on Linux and ntpd on FreeBSD. Linux
  asks systemd to reload or restart chrony over D-Bus when enabled and to stop
  it when disabled, then waits for the correlated job completion. FreeBSD
  performs the equivalent base-ntpd lifecycle through its documented
  service(8)/rc.d boundary using an absolute executable path and fixed argv;
  it does not invoke a shell.
- DNS search domains, servers, timeout, and attempts are rendered to a static
  `/etc/resolv.conf`. Non-default DNS ports are not advertised.
- local users, SHA-256/SHA-512 crypt password hashes, and authorized SSH key
  records are parsed and retained. Password verification is constant-time
  after the host `crypt(3)` operation.
- platform identity, current time, and estimated boot time are published as
  `system-state` operational data.
- `set-current-datetime`, `system-restart`, and `system-shutdown` are owned by
  the plugin. Power operations signal the native PID 1 service manager for an
  orderly shutdown rather than invoking a command, and require the explicit
  `DANG_SYSTEM_ALLOW_POWER=1` deployment guard plus appropriate host privilege.
- every file modified during a configuration transaction is snapshotted and
  restored on failure or dangd rollback. Symbolic links are restored as links.

Set `DANG_SYSTEM_ROOT` to a disposable filesystem root for testing. Production
deployments must omit it or configuration will be redirected away from the
real host. Set `DANG_SYSTEM_AUTH_SOCKET` to enable the root-only PAM
verification service, normally:

```sh
export DANG_SYSTEM_AUTH_SOCKET=/run/dangd/auth.sock
```

Use `/var/run/dangd/auth.sock` on FreeBSD. Create a private parent directory
owned by the dangd service identity and place the variable in the service
environment. After starting dangd, verify the socket owner and mode before
adding a PAM policy.

The socket is mode 0600, accepts only root peers using native peer
credentials, bounds usernames and passwords, applies a five-second I/O
timeout, and returns only accepted/denied/unavailable. Password hashes and
datastore contents never cross the socket.

Install `pam_dangd.so` in the platform PAM module directory. A minimal sshd
PAM policy entry is:

```text
auth required pam_dangd.so socket=/run/dangd/auth.sock
account required pam_dangd.so socket=/run/dangd/auth.sock
```

`pam_dangd` behaves as a conventional password authentication module: it uses
an existing `PAM_AUTHTOK` or prompts with echo disabled, fails closed when the
socket is unavailable, and supplies no credentials or session side effects.
It is suitable for sshd's PAM password path. SSH public-key authentication is
not a PAM operation and remains separate.

## Known functional and compliance gaps

1. RADIUS client configuration and RADIUS authentication are not advertised or
   implemented, as requested.
2. RFC 7317 permits `$0$` cleartext password input if the server replaces it
   with a hash before storage. The present plugin ABI cannot rewrite a secret
   before datastore persistence, so `$0$` is rejected. MD5 crypt hashes are
   also not advertised.
3. Authorized-key records are validated and retained but are not yet consumed
   by dangd's embedded SSH server or an OpenSSH `AuthorizedKeysCommand` helper.
4. Linux systems whose `/etc/resolv.conf` is a resolver-manager symbolic link
   are rejected to avoid breaking systemd-resolved or resolvconf ownership.
   Native manager APIs are not implemented. Static resolver files work on both
   tested platforms.
   Removing an explicitly managed hostname or timezone is rejected because
   RFC 7317 does not define which platform default should replace it.
5. Linux NTP integration targets chrony through the systemd D-Bus manager and
   FreeBSD targets base ntpd. Other daemons require a platform adapter. The
   `ntp-udp-port` feature is disabled. Base ntpd exposes no service-manager
   library or socket API on FreeBSD, so the provider uses the documented
   service(8)/rc.d process boundary with fixed, host-owned arguments.
6. `set-current-datetime` currently accepts canonical UTC values ending in
   `Z`; fractional seconds and explicit numeric offsets allowed by
   `yang:date-and-time` are not translated. The plugin ABI cannot return RFC
   7317's exact `ntp-active` NETCONF error-app-tag, so it includes that token in
   the attributed error message.
7. Restart and shutdown use guarded native PID 1 signals: systemd's documented
   real-time signals on Linux and init's documented signals on FreeBSD. dangd
   currently invokes plugin RPCs synchronously, so it cannot guarantee the
   RFC's recommended reply-before-power-transition sequencing.
8. Operational data is published through ABI v3. Its contents are complete,
   but ABI v5 completeness cannot be selected without also implementing the
   complete ABI v4 fine-grained hardware action contract.
9. PAM account management returns success after authentication because RFC
    7317 defines no expiry, lockout, login-class, credential, or session data.
    Password aging and OS account provisioning are outside this model.
10. The embedded module intentionally remains byte-for-byte compatible with
    the published 2014-08-06 source. RFC 7317 erratum 6245 is classified as
    "Held for Document Update" and is therefore documented but not patched
    into the advertised model revision.

These gaps are functional declarations, not silently accepted configuration.
Unsupported advertised choices are rejected during validation whenever the
native backend cannot honor them.

## Native validation

The platform scripts create only a disposable filesystem root and temporary
PAM service entry. They do not create, select, or transmit through any network
interface, and therefore cannot touch a LAN interface.

```sh
sudo tests/platform/linux/run_system_isolated.sh "$PWD"
sudo tests/platform/freebsd/run_system_isolated.sh "$PWD"
```

Each script checks successful and failed password authentication through the
host PAM framework, verifies generated DNS and NTP files, reads live platform
state, rolls the transaction back, and removes its temporary PAM policy.

Both platforms additionally have an opt-in live service-manager check.
Configure with `-DDANG_SYSTEM_NATIVE_TESTS=ON`; the test records the native NTP
service's initial state, enables and disables it through sd-bus on Linux or the
fixed service(8) invocation on FreeBSD, verifies each completed state, and
restores the initial state even after failure.

Native packages deliberately do not enable a PAM policy or alter sshd. The
administrator must opt into the local verification path described above.

The packages are now separate: `dangd-plugin-system` installs this provider;
`dangd-pam` installs the adapter and depends on it. Follow the installed
`PAM.md` guide before editing sshd.

## Safe rollout and removal

Start with non-disruptive `contact` and `location` data and retrieve the
advertised `system-state`. Apply hostname, DNS, timezone, and NTP one category
at a time with local console recovery available. Leave
`DANG_SYSTEM_ALLOW_POWER` unset unless remote power RPCs are explicitly needed
and restricted by NACM.

Linux NTP reload/restart and stop use the systemd D-Bus manager and wait for
the exact asynchronous job outcome. FreeBSD uses `posix_spawn(3)` to execute
the absolute `/usr/sbin/service` path with only `ntpd onerestart` or
`ntpd onestop`. This is the documented base-system lifecycle interface; no
modeled data enters its argv and no shell is involved. See the collection-wide
[`COMMAND_EXECUTION_AUDIT.md`](../../COMMAND_EXECUTION_AUDIT.md).

Before uninstalling, remove the plugin option and migrate or delete all
`ietf-system` datastore nodes. Remove `dangd-pam` and its PAM policy first.
Then use `sudo apt remove dangd-plugin-system` or
`sudo pkg delete dangd-plugin-system`. Package removal does not undo the last
hostname, timezone, resolver, or NTP configuration applied to the host.

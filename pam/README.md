<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# pam_dangd installation and SSH integration

`pam_dangd` authenticates a PAM username and password against the local,
root-peer-only verification socket supplied by `dangd-plugin-system`. It is an
adapter, not a YANG provider, and cannot work without the system plugin.

## Dependencies

- `dangd-plugin-system` at the same package version;
- the host PAM runtime (`libpam` on Linux, base PAM on FreeBSD);
- OpenSSH configured for PAM if SSH password authentication is desired; and
- a local root recovery path before changing PAM or sshd.

The module supports PAM `auth` and `account`. It does not create operating
system accounts, home directories, groups, shells, credentials, password-aging
rules, or sessions. Public-key authentication remains outside PAM.

## Install

Install and verify `dangd-plugin-system` first. Configure its service with
`DANG_SYSTEM_AUTH_SOCKET=/run/dangd/auth.sock`; use
`/var/run/dangd/auth.sock` on FreeBSD. Start dangd, confirm that the socket is
present and mode 0600, and configure an RFC 7317 local user with a supported
crypt password hash.

Debian/Ubuntu:

```sh
sudo apt install ./dangd-pam_0.1.0_amd64.deb
dpkg -L dangd-pam
```

FreeBSD:

```sh
sudo pkg add ./dangd-pam-0.1.0.pkg
pkg info -l dangd-pam
```

Source installation:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DDANGD_ROOT=/path/to/dang
cmake --build build --target pam_dangd system_plugin_integration_test
ctest --test-dir build -R system_plugin --output-on-failure
sudo cmake --install build --component pam
```

## Test before changing SSH

Keep an existing root console or SSH session open. Create a temporary PAM
service policy using the exact module path if the platform cannot find modules
by basename:

```text
auth required pam_dangd.so socket=/run/dangd/auth.sock
account required pam_dangd.so socket=/run/dangd/auth.sock
```

Use `/var/run/dangd/auth.sock` on FreeBSD. Test this policy with a PAM test
program before touching sshd. Try a correct password, wrong password, unknown
user, stopped dangd, and inaccessible socket. Every case except the first must
fail closed.

## Add it to OpenSSH

Back up the active sshd PAM policy. Add the two lines above to `/etc/pam.d/sshd`
on the tested systems. Position is significant: review the whole stack so a
`sufficient` rule cannot bypass the intended policy. Ensure sshd is configured
to use PAM and the desired password or keyboard-interactive path. Validate the
sshd configuration with its native test mode before reloading it. Prove both a
successful and failed login in a second connection before closing the recovery
session.

The username need not be an operating-system account for this password check,
but later PAM modules or sshd session setup may require one. `pam_dangd` does
not provision it. For NETCONF rather than an interactive shell, bind the
authenticated identity through dangd's SSH path and NACM instead of granting a
general-purpose login.

## Troubleshooting and rollback

- Service unavailable means a stopped plugin, mismatched path, permission
  failure, or timeout.
- Denial with a healthy socket normally means an unknown user, unsupported
  hash, or wrong password; the socket intentionally reveals no distinction.
- This module may accept a password that another PAM account/session rule
  subsequently rejects.

To roll back, remove the `pam_dangd` policy lines, validate and reload sshd,
and prove a new login through the previous method. Then run
`sudo apt remove dangd-pam` or `sudo pkg delete dangd-pam`.

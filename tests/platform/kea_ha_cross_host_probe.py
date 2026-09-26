#!/usr/bin/env python3
# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

"""Query disposable Kea UNIX sockets for cross-host HA assertions."""

import json
import socket
import sys
import time
from typing import Any, Callable, Optional


def query(path: str, command: str, arguments: dict[str, Any]) -> dict[str, Any]:
    """Send one native Kea command and return its single answer object."""
    request = json.dumps({"command": command, "arguments": arguments}).encode()
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.settimeout(2)
    try:
        client.connect(path)
        client.sendall(request)
        client.shutdown(socket.SHUT_WR)
        chunks: list[bytes] = []
        while True:
            chunk = client.recv(65536)
            if not chunk:
                break
            chunks.append(chunk)
    finally:
        client.close()
    reply = json.loads(b"".join(chunks))
    # UNIX sockets normally return an object. Accept the one-element HTTP form
    # as well so this diagnostic remains useful if the transport is changed.
    if isinstance(reply, list) and len(reply) == 1:
        reply = reply[0]
    if not isinstance(reply, dict):
        raise RuntimeError(f"{command} returned no single answer object")
    return reply


def successful(reply: dict[str, Any], command: str) -> dict[str, Any]:
    """Require success and return the command's arguments map."""
    if reply.get("result") != 0:
        raise RuntimeError(f"{command} failed: {reply.get('text', reply)!r}")
    arguments = reply.get("arguments", {})
    if not isinstance(arguments, dict):
        raise RuntimeError(f"{command} returned malformed arguments")
    return arguments


def ha_ready(path: str, local_name: str, remote_name: str,
             local_role: str) -> bool:
    """Return whether one daemon identifies and reaches its configured peer."""
    arguments = successful(query(path, "status-get", {}), "status-get")
    relationships = arguments.get("high-availability")
    if not isinstance(relationships, list) or len(relationships) != 1:
        return False
    servers = relationships[0].get("ha-servers", {})
    local = servers.get("local", {})
    remote = servers.get("remote", {})
    scopes = local.get("scopes")
    expected_scopes = [local_name] if local_role == "primary" else []
    return (local.get("server-name") == local_name and
            local.get("state") == "hot-standby" and
            scopes == expected_scopes and
            remote.get("server-name") == remote_name and
            remote.get("in-touch") is True)


def leases_present(socket4: str, socket6: str) -> bool:
    """Require the deterministic first lease in both local databases."""
    def lease_page(path: str, command: str) -> list[dict[str, Any]]:
        reply = query(path, command, {"from": "start", "limit": 16})
        if reply.get("result") == 3:
            return []
        return successful(reply, command).get("leases", [])

    # Use the same bounded paging API as the plugin. Unlike lease*-get-all,
    # this command has one portable shape across the tested Kea releases.
    leases4 = lease_page(socket4, "lease4-get-page")
    leases6 = lease_page(socket6, "lease6-get-page")
    return (any(lease.get("ip-address") == "192.0.2.100" for lease in leases4)
            and any(lease.get("ip-address") == "2001:db8:6::100"
                    for lease in leases6))


def wait_for(predicate: Callable[[], bool], description: str) -> None:
    """Bound asynchronous HA startup and replication to twenty seconds."""
    deadline = time.monotonic() + 20
    last_error: Optional[Exception] = None
    while time.monotonic() < deadline:
        try:
            if predicate():
                return
        except (OSError, ValueError, RuntimeError) as error:
            last_error = error
        time.sleep(0.2)
    suffix = f": {last_error}" if last_error else ""
    raise RuntimeError(f"timed out waiting for {description}{suffix}")


def main() -> None:
    if len(sys.argv) < 4:
        raise SystemExit(
            f"usage: {sys.argv[0]} ready SOCKET NAME PEER ROLE | "
            "leases SOCKET4 SOCKET6"
        )
    if sys.argv[1] == "ready" and len(sys.argv) == 6:
        try:
            wait_for(lambda: ha_ready(sys.argv[2], sys.argv[3], sys.argv[4],
                                      sys.argv[5]),
                     f"{sys.argv[3]} to reach {sys.argv[4]}")
        except RuntimeError:
            print(json.dumps(query(sys.argv[2], "status-get", {}), indent=2),
                  file=sys.stderr)
            raise
    elif sys.argv[1] == "leases" and len(sys.argv) == 4:
        wait_for(lambda: leases_present(sys.argv[2], sys.argv[3]),
                 "replicated DHCPv4 and DHCPv6 leases")
    else:
        raise SystemExit("invalid HA probe arguments")


if __name__ == "__main__":
    main()

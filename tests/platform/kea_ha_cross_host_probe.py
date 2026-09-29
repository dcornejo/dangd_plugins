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


def leases_present(socket4: str, socket6: str, address4: str,
                   address6: str) -> bool:
    """Require the selected leases in both local databases."""
    def lease_page(path: str, command: str) -> list[dict[str, Any]]:
        reply = query(path, command, {"from": "start", "limit": 16})
        if reply.get("result") == 3:
            return []
        return successful(reply, command).get("leases", [])

    # Use the same bounded paging API as the plugin. Unlike lease*-get-all,
    # this command has one portable shape across the tested Kea releases.
    leases4 = lease_page(socket4, "lease4-get-page")
    leases6 = lease_page(socket6, "lease6-get-page")
    return (any(lease.get("ip-address") == address4 for lease in leases4)
            and any(lease.get("ip-address") == address6 for lease in leases6))


def activate_takeover(path: str, local_name: str,
                      primary_name: str) -> None:
    """Assign the stopped primary's only hot-standby scope locally."""
    successful(query(path, "ha-scopes", {
        "server-name": local_name,
        "scopes": [primary_name],
    }), "ha-scopes")


def peer_is_down(path: str, local_name: str, primary_name: str) -> bool:
    """Return whether the survivor reports its stopped partner unavailable."""
    arguments = successful(query(path, "status-get", {}), "status-get")
    relationships = arguments.get("high-availability")
    if not isinstance(relationships, list) or len(relationships) != 1:
        return False
    servers = relationships[0].get("ha-servers", {})
    local = servers.get("local", {})
    remote = servers.get("remote", {})
    return (local.get("server-name") == local_name and
            remote.get("server-name") == primary_name and
            remote.get("communication-interrupted") is True and
            remote.get("last-state") == "unavailable")


def takeover_ready(path: str, local_name: str, primary_name: str) -> bool:
    """Return whether the survivor now owns the primary service scope."""
    arguments = successful(query(path, "status-get", {}), "status-get")
    relationships = arguments.get("high-availability")
    if not isinstance(relationships, list) or len(relationships) != 1:
        return False
    local = relationships[0].get("ha-servers", {}).get("local", {})
    return (local.get("server-name") == local_name and
            local.get("scopes") == [primary_name])


def relinquish_takeover(path: str, local_name: str) -> None:
    """Disable the survivor's manual scope before its partner restarts."""
    successful(query(path, "ha-scopes", {
        "server-name": local_name,
        "scopes": [],
    }), "ha-scopes")


def scopes_relinquished(path: str, local_name: str) -> bool:
    """Return whether the local server has stopped serving manual scopes."""
    arguments = successful(query(path, "status-get", {}), "status-get")
    relationships = arguments.get("high-availability")
    if not isinstance(relationships, list) or len(relationships) != 1:
        return False
    local = relationships[0].get("ha-servers", {}).get("local", {})
    return (local.get("server-name") == local_name and
            local.get("scopes") == [])


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
            "takeover SOCKET NAME PRIMARY | "
            "relinquish SOCKET NAME | "
            "leases SOCKET4 SOCKET6 ADDRESS4 ADDRESS6"
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
    elif sys.argv[1] == "takeover" and len(sys.argv) == 5:
        try:
            wait_for(
                lambda: peer_is_down(sys.argv[2], sys.argv[3], sys.argv[4]),
                f"{sys.argv[3]} to observe {sys.argv[4]} down")
        except RuntimeError:
            print(json.dumps(query(sys.argv[2], "status-get", {}), indent=2),
                  file=sys.stderr)
            raise
        activate_takeover(sys.argv[2], sys.argv[3], sys.argv[4])
        wait_for(lambda: takeover_ready(sys.argv[2], sys.argv[3], sys.argv[4]),
                 f"{sys.argv[3]} to activate the {sys.argv[4]} scope")
    elif sys.argv[1] == "relinquish" and len(sys.argv) == 4:
        relinquish_takeover(sys.argv[2], sys.argv[3])
        wait_for(lambda: scopes_relinquished(sys.argv[2], sys.argv[3]),
                 f"{sys.argv[3]} to relinquish its manual scopes")
    elif sys.argv[1] == "leases" and len(sys.argv) == 6:
        wait_for(lambda: leases_present(sys.argv[2], sys.argv[3], sys.argv[4],
                                        sys.argv[5]),
                 f"DHCPv4 {sys.argv[4]} and DHCPv6 {sys.argv[5]} leases")
    else:
        raise SystemExit("invalid HA probe arguments")


if __name__ == "__main__":
    main()

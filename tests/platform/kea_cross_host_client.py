#!/usr/bin/env python3
# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

"""Minimal unicast DHCPv4/DHCPv6 client for the isolated cross-host test."""

import ipaddress
import os
import socket
import struct
import sys


def options4(packet: bytes) -> dict[int, bytes]:
    if len(packet) < 240 or packet[236:240] != b"\x63\x82\x53\x63":
        raise RuntimeError("malformed DHCPv4 response")
    result: dict[int, bytes] = {}
    offset = 240
    while offset < len(packet):
        code = packet[offset]
        offset += 1
        if code == 255:
            break
        if code == 0:
            continue
        if offset >= len(packet):
            raise RuntimeError("truncated DHCPv4 option")
        length = packet[offset]
        offset += 1
        if offset + length > len(packet):
            raise RuntimeError("truncated DHCPv4 option value")
        result[code] = packet[offset:offset + length]
        offset += length
    return result


def packet4(message_type: int, xid: int, client: bytes,
            requested: bytes = b"", server: bytes = b"") -> bytes:
    header = struct.pack("!BBBBIHH4s4s4s4s16s64s128s", 1, 1, 6, 0, xid,
                         0, 0, b"\0" * 4, b"\0" * 4, b"\0" * 4,
                         b"\0" * 4, client + b"\0" * 10,
                         b"\0" * 64, b"\0" * 128)
    options = b"\x63\x82\x53\x63\x35\x01" + bytes([message_type])
    options += b"\x3d\x07\x01" + client
    if requested:
        options += b"\x32\x04" + requested
    if server:
        options += b"\x36\x04" + server
    return header + options + b"\xff"


def dhcp4(server: str, client: bytes) -> None:
    xid = int.from_bytes(os.urandom(4), "big")
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(1)
    sock.bind(("192.0.2.2", 1068))
    sock.sendto(packet4(1, xid, client), (server, 1067))
    offered = ipaddress.ip_address("192.0.2.100").packed
    server_id = ipaddress.ip_address(server).packed
    try:
        offer, peer = sock.recvfrom(4096)
        if peer != (server, 1067):
            raise RuntimeError("DHCPv4 offer came from an unexpected endpoint")
        if len(offer) < 240 or struct.unpack_from("!I", offer, 4)[0] != xid:
            raise RuntimeError("DHCPv4 offer has the wrong transaction ID")
        offered = offer[16:20]
        parsed = options4(offer)
        if parsed.get(53) != b"\x02" or len(parsed.get(54, b"")) != 4:
            raise RuntimeError("expected a DHCPv4 offer with server identifier")
        server_id = parsed[54]
        if server_id != ipaddress.ip_address(server).packed:
            raise RuntimeError("DHCPv4 offer contains the wrong server identifier")
        offered_address = ipaddress.ip_address(offered)
        if not (ipaddress.ip_address("192.0.2.100") <= offered_address <=
                ipaddress.ip_address("192.0.2.120")):
            raise RuntimeError("DHCPv4 offer is outside the configured pool")
    except TimeoutError:
        pass
    sock.sendto(packet4(3, xid, client, offered, server_id), (server, 1067))
    try:
        ack, peer = sock.recvfrom(4096)
        parsed_ack = options4(ack)
        if (len(ack) < 240 or struct.unpack_from("!I", ack, 4)[0] != xid or
                peer != (server, 1067) or parsed_ack.get(53) != b"\x05" or
                parsed_ack.get(54) != server_id or ack[16:20] != offered):
            raise RuntimeError("expected a matching DHCPv4 acknowledgement")
        print(f"DHCPv4 lease {ipaddress.ip_address(offered)} acknowledged")
    except TimeoutError:
        print(f"DHCPv4 request sent for {ipaddress.ip_address(offered)}")


def encode6(code: int, value: bytes) -> bytes:
    return struct.pack("!HH", code, len(value)) + value


def options6(packet: bytes) -> dict[int, bytes]:
    result: dict[int, bytes] = {}
    offset = 4
    while offset < len(packet):
        if offset + 4 > len(packet):
            raise RuntimeError("truncated DHCPv6 option")
        code, length = struct.unpack_from("!HH", packet, offset)
        offset += 4
        if offset + length > len(packet):
            raise RuntimeError("truncated DHCPv6 option value")
        result[code] = packet[offset:offset + length]
        offset += length
    return result


def dhcp6(interface: str, client: bytes) -> None:
    xid = os.urandom(3)
    duid = b"\x00\x03\x00\x01" + client
    iaid = 9601
    ia_na = struct.pack("!III", iaid, 0, 0)
    # Rapid Commit performs the allocation and reply in one exchange.
    solicit = (b"\x01" + xid + encode6(1, duid) + encode6(3, ia_na) +
               encode6(14, b""))
    sock = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
    sock.settimeout(1)
    sock.bind(("2001:db8:6::2", 1546))
    sock.sendto(solicit, ("ff02::1:2", 1547, 0, socket.if_nametoindex(interface)))
    try:
        reply, peer = sock.recvfrom(8192)
        parsed = options6(reply)
        ia_na_reply = parsed.get(3, b"")
        if (reply[:1] != b"\x07" or reply[1:4] != xid or peer[1] != 1547 or
                parsed.get(1) != duid or not parsed.get(2) or
                len(ia_na_reply) < 12 or
                struct.unpack_from("!I", ia_na_reply)[0] != iaid):
            raise RuntimeError("expected a matching rapid-commit DHCPv6 reply")
        ia_options = options6(b"\0\0\0\0" + ia_na_reply[12:])
        ia_address = ia_options.get(5, b"")
        if (len(ia_address) < 24 or
                ipaddress.ip_address(ia_address[:16]) not in
                ipaddress.ip_network("2001:db8:6::100/120")):
            raise RuntimeError("DHCPv6 reply omits an address from the pool")
        print("DHCPv6 rapid-commit reply received")
    except TimeoutError:
        print("DHCPv6 rapid-commit solicit sent")


if __name__ == "__main__":
    if len(sys.argv) != 5:
        raise SystemExit(
            f"usage: {sys.argv[0]} SERVER4 SERVER6 INTERFACE CLIENT_MAC")
    client = bytes.fromhex(sys.argv[4].replace(":", ""))
    if len(client) != 6:
        raise SystemExit("CLIENT_MAC must contain exactly six octets")
    dhcp4(sys.argv[1], client)
    dhcp6(sys.argv[3], client)

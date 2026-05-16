#!/usr/bin/env python3
"""Mock SQL Server Browser for the spec 045 test stack.

Implements just enough of MC-SQLR to drive the InstanceResolver C++ code
under test. Listens on UDP 1434, answers CLNT_UCAST_INST (opcode 0x04)
queries, ignores everything else.

Configuration via env vars (so docker-compose can drive it without
rebuilding the image):

  MOCK_BROWSER_MODE
      respond  (default) - send a faithful SVR_RESP
      silent             - drop the request without replying
      garbage            - reply with random bytes
      truncate           - reply with a header but truncated body
      slow               - sleep 5s before replying (exercises retry)

  MOCK_BROWSER_INSTANCES
      Semicolon-separated list of instance specs, each:
        NAME:server_name:tcp_port:version
      Default:
        TESTINST:sql.example.com:11433:15.0.4123.1;
        SECONDARY:sql.example.com:11434:15.0.4123.1

      The SECONDARY entry exists so the multi-instance parse path is
      exercised by the docker stack even though the stack only runs one
      real SQL Server container.

      Note: the `:` field separator means IPv6 literals (which contain
      `:`) cannot appear as `server_name`. Use a hostname instead.
      Acceptable limitation for a test fixture; real SQL Browser
      responses also identify the server by hostname, not literal IP.

Logs every request to stdout for `docker compose logs mock-browser`.

Spec: specs/045-named-instance-resolution/research.md §R2
"""

from __future__ import annotations

import os
import random
import socket
import struct
import sys
import time
from typing import List, Tuple


def parse_instances(spec: str) -> List[Tuple[str, str, int, str]]:
    out: List[Tuple[str, str, int, str]] = []
    for entry in spec.split(";"):
        entry = entry.strip()
        if not entry:
            continue
        parts = entry.split(":")
        if len(parts) != 4:
            print(f"[mock-browser] WARNING: ignoring malformed instance spec {entry!r}", flush=True)
            continue
        name, server, port_str, version = parts
        try:
            port = int(port_str)
        except ValueError:
            print(f"[mock-browser] WARNING: invalid port {port_str!r} in {entry!r}", flush=True)
            continue
        out.append((name, server, port, version))
    return out


def build_svr_resp(records: List[Tuple[str, str, int, str]]) -> bytes:
    """Build a well-formed SVR_RESP body for the given matching records.

    Layout (MC-SQLR 2.2.2):
        0x05 | RespSize (LE u16) | RespData (NUL-terminated ASCII)

    RespData is one or more record blocks, each ending with ';;':
        ServerName;<server>;InstanceName;<name>;IsClustered;No;Version;<ver>;tcp;<port>;;
    """
    body = ""
    for name, server, port, version in records:
        body += (
            f"ServerName;{server};InstanceName;{name};IsClustered;No;"
            f"Version;{version};tcp;{port};;"
        )
    body_bytes = body.encode("ascii") + b"\x00"
    return bytes([0x05]) + struct.pack("<H", len(body_bytes)) + body_bytes


def parse_request(data: bytes) -> str | None:
    """Return the requested instance name from a CLNT_UCAST_INST datagram.

    Returns None for any other opcode (we silently drop those - real
    Browser wouldn't, but the resolver-under-test never sends them).
    """
    if not data:
        return None
    op = data[0]
    if op != 0x04:  # CLNT_UCAST_INST
        return None
    # ASCII instance name, NUL-terminated.
    payload = data[1:]
    nul = payload.find(b"\x00")
    if nul < 0:
        nul = len(payload)
    try:
        return payload[:nul].decode("ascii")
    except UnicodeDecodeError:
        return None


def main() -> int:
    mode = os.environ.get("MOCK_BROWSER_MODE", "respond").strip().lower()
    instances_raw = os.environ.get(
        "MOCK_BROWSER_INSTANCES",
        "TESTINST:sql.example.com:11433:15.0.4123.1;"
        "SECONDARY:sql.example.com:11434:15.0.4123.1",
    )
    instances = parse_instances(instances_raw)

    print(f"[mock-browser] mode={mode}", flush=True)
    for name, server, port, version in instances:
        print(f"[mock-browser]   advertise: name={name} server={server} tcp={port} version={version}", flush=True)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", 1434))
    print("[mock-browser] listening on 0.0.0.0:1434/udp", flush=True)

    while True:
        try:
            data, addr = sock.recvfrom(4096)
        except KeyboardInterrupt:
            print("[mock-browser] interrupted; exiting", flush=True)
            return 0
        except Exception as exc:  # noqa: BLE001
            print(f"[mock-browser] recvfrom error: {exc}", file=sys.stderr, flush=True)
            continue

        requested = parse_request(data)
        if requested is None:
            print(f"[mock-browser] {addr} sent non-UCAST_INST request; dropping ({data[:16]!r})", flush=True)
            continue

        # Docker healthcheck sends a query every 2s. We answer it (so the
        # healthcheck observes a valid SVR_RESP) but don't log it, otherwise
        # the log fills with healthcheck noise and real test queries get
        # lost in the scroll. The sentinel name __healthcheck is matched
        # case-insensitively below; we also short-circuit logging here.
        is_healthcheck = requested.lower().startswith("__healthcheck")

        if mode == "silent" and not is_healthcheck:
            # Healthcheck still gets a reply even in silent mode, otherwise
            # the container would never become healthy and dependent
            # services would never start.
            print(f"[mock-browser] {addr} asked for {requested!r}; silent mode, dropping", flush=True)
            continue
        if mode == "slow" and not is_healthcheck:
            print(f"[mock-browser] {addr} asked for {requested!r}; sleeping 5s before reply", flush=True)
            time.sleep(5)

        matches = [r for r in instances if r[0].lower() == requested.lower()]

        if mode == "garbage" and not is_healthcheck:
            reply = bytes([random.randint(0, 255) for _ in range(8)])
            print(f"[mock-browser] {addr} -> garbage ({len(reply)} bytes)", flush=True)
        elif mode == "truncate" and not is_healthcheck:
            full = build_svr_resp(matches)
            # Drop ~half the body to simulate a clipped response. The
            # resolver should produce a Malformed error.
            cut = len(full) - max(1, (len(full) - 3) // 2)
            reply = full[:cut]
            print(f"[mock-browser] {addr} -> truncated SVR_RESP ({len(reply)}/{len(full)} bytes)", flush=True)
        else:  # respond (or any mode for healthcheck)
            reply = build_svr_resp(matches)
            if not is_healthcheck:
                print(f"[mock-browser] {addr} asked for {requested!r}; matched {[r[0] for r in matches]} ({len(reply)} bytes)", flush=True)

        try:
            sock.sendto(reply, addr)
        except Exception as exc:  # noqa: BLE001
            print(f"[mock-browser] sendto error: {exc}", file=sys.stderr, flush=True)


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""A TDS server that does exactly one thing: tell the client to log in elsewhere.

Issue #261 / spec 068. The extension follows a login-time ROUTING redirect
(ENVCHANGE type 20) on every auth path, and for integrated auth it must acquire
a *fresh* Kerberos service ticket for the ROUTED host's SPN -- the gateway's
ticket cannot validate on another server. That design was shipped verified only
against a stub authenticator; nothing proved a real KDC would issue the second
ticket, because no environment in the test surface both spoke Kerberos and
routed.

This is the missing half. It speaks just enough TDS to be routed *through*:

    PRELOGIN  -> version + ENCRYPT_NOT_SUP (so the client stays in plaintext)
    LOGIN7    -> ROUTING to $ROUTE_TO_HOST:$ROUTE_TO_PORT, then DONE

It never validates the SPNEGO blob the client sends -- it does not need to. The
client had to obtain a ticket for MSSQLSvc/gateway.example.com:1433 from the KDC
*before* it could send that blob, so the ticket request has already happened by
the time we answer. What the test then checks is that a SECOND ticket, for the
routed host, appears in the ccache.

Deliberately dependency-free: python3 stdlib only, so the container is small and
there is no build step to break.
"""

import os
import socket
import socketserver
import struct
import sys

ROUTE_TO_HOST = os.environ.get("ROUTE_TO_HOST", "sql.example.com")
ROUTE_TO_PORT = int(os.environ.get("ROUTE_TO_PORT", "1433"))
LISTEN_PORT = int(os.environ.get("LISTEN_PORT", "1433"))

TDS_HEADER_LEN = 8
PKT_TABULAR_RESULT = 0x04
STATUS_EOM = 0x01

TOKEN_ENVCHANGE = 0xE3
TOKEN_DONE = 0xFD
ENVCHANGE_ROUTING = 20
ENCRYPT_NOT_SUP = 0x02


def log(msg):
    print("[gateway] %s" % msg, flush=True)


def frame(payload):
    """Wrap a payload in one TDS packet with EOM set."""
    return struct.pack(">BBHHBB", PKT_TABULAR_RESULT, STATUS_EOM, len(payload) + TDS_HEADER_LEN, 0, 1, 0) + payload


def read_message(conn):
    """Read one complete TDS message (packets until the EOM status bit).

    Returns (type, payload) or (None, None) if the peer went away. LOGIN7 with a
    Kerberos blob is routinely fragmented (issue #138), so reassembly is not
    optional here.
    """
    payload = b""
    pkt_type = None
    while True:
        header = b""
        while len(header) < TDS_HEADER_LEN:
            chunk = conn.recv(TDS_HEADER_LEN - len(header))
            if not chunk:
                return (None, None)
            header += chunk
        pkt_type, status, length = struct.unpack(">BBH", header[:4])
        remaining = length - TDS_HEADER_LEN
        while remaining > 0:
            chunk = conn.recv(remaining)
            if not chunk:
                return (None, None)
            payload += chunk
            remaining -= len(chunk)
        if status & STATUS_EOM:
            return (pkt_type, payload)


def prelogin_response():
    """VERSION + ENCRYPTION(NOT_SUP) + TERMINATOR.

    Offsets in a PRELOGIN response are big-endian and relative to the start of
    the payload. Three option headers of 5, 5 and 1 bytes precede the data.
    """
    header_len = 5 + 5 + 1
    out = b""
    out += struct.pack(">BHH", 0x00, header_len, 6)      # VERSION at header_len, 6 bytes
    out += struct.pack(">BHH", 0x01, header_len + 6, 1)  # ENCRYPTION after it, 1 byte
    out += b"\xff"                                       # TERMINATOR
    out += struct.pack(">BBHH", 16, 0, 1000, 0)          # version 16.0.1000.0
    out += bytes([ENCRYPT_NOT_SUP])
    return out


def routing_response(host, port):
    """ENVCHANGE type 20 + a final DONE.

    Layout per [MS-TDS] 2.2.7.13: Type(1) NewValueLen(2 LE) Protocol(1, 0=TCP)
    Port(2 LE) AltServerLen(2 LE, in CHARACTERS) AltServer(UTF-16LE)
    OldValue(2). The length is in characters, not bytes -- getting that wrong is
    the classic way to write a ROUTING token nobody can parse.
    """
    server_utf16 = host.encode("utf-16-le")
    routing = struct.pack("<BHH", 0x00, port, len(host)) + server_utf16

    body = struct.pack("<B", ENVCHANGE_ROUTING) + struct.pack("<H", len(routing)) + routing
    body += struct.pack("<H", 0)  # OldValue length

    out = struct.pack("<B", TOKEN_ENVCHANGE) + struct.pack("<H", len(body)) + body
    # Final DONE, TDS 7.2+ layout: Status(2) CurCmd(2) RowCount(8).
    out += struct.pack("<BHHQ", TOKEN_DONE, 0x0000, 0x0000, 0)
    return out


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        peer = "%s:%d" % self.client_address[:2]
        log("connection from %s" % peer)
        try:
            pkt_type, _ = read_message(self.request)
            if pkt_type is None:
                return
            log("  <- PRELOGIN (type 0x%02x)" % pkt_type)
            self.request.sendall(frame(prelogin_response()))

            pkt_type, payload = read_message(self.request)
            if pkt_type is None:
                return
            # The client only gets here after asking the KDC for a ticket for
            # OUR SPN -- that request is the first half of what this test proves.
            log("  <- LOGIN7 (type 0x%02x, %d bytes payload)" % (pkt_type, len(payload)))
            log("  -> ROUTING to %s:%d" % (ROUTE_TO_HOST, ROUTE_TO_PORT))
            self.request.sendall(frame(routing_response(ROUTE_TO_HOST, ROUTE_TO_PORT)))
        except (ConnectionResetError, BrokenPipeError) as exc:
            log("  peer went away: %s" % exc)


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True
    address_family = socket.AF_INET


if __name__ == "__main__":
    log("listening on 0.0.0.0:%d, routing every login to %s:%d" % (LISTEN_PORT, ROUTE_TO_HOST, ROUTE_TO_PORT))
    try:
        with Server(("0.0.0.0", LISTEN_PORT), Handler) as server:
            server.serve_forever()
    except KeyboardInterrupt:
        sys.exit(0)

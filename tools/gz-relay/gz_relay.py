#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Relay server for shadPS4 P2P traffic (Godzilla PS4 Versus and similar).

Two players who cannot reach each other directly both send their packets here; the relay
forwards each packet to the other player in the same session. See src/core/net/p2p_relay.h for
the wire format. No game data is read or stored.

Usage: python3 gz_relay.py [--port 3659]
"""

import argparse
import asyncio
import logging
import struct
import time

MAGIC = b"GZRY"
VERSION = 1
T_BIND, T_ACK, T_DATA = 1, 2, 3
HEADER = struct.Struct("<4sBBBBQ")  # magic, version, type, info, reserved, session

SESSION_IDLE = 120.0      # forget a session after this long without packets
SLOT_STALE = 15.0         # a player silent this long can be replaced by a new one
MAX_SESSIONS = 5000
MAX_PACKET = 1600
RATE_PER_SEC = 1500       # packets per second per sender before dropping


class Session:
    __slots__ = ("players", "last", "forwarded")

    def __init__(self):
        self.players = {}  # (ip, port) -> last seen
        self.last = time.monotonic()
        self.forwarded = 0


class Relay(asyncio.DatagramProtocol):
    def __init__(self):
        self.sessions = {}
        self.rate = {}  # addr -> [window_start, count]
        self.transport = None

    def connection_made(self, transport):
        self.transport = transport

    def _allowed(self, addr, now):
        w = self.rate.get(addr)
        if w is None or now - w[0] >= 1.0:
            self.rate[addr] = [now, 1]
            return True
        w[1] += 1
        return w[1] <= RATE_PER_SEC

    def _join(self, session_id, addr, now):
        s = self.sessions.get(session_id)
        if s is None:
            if len(self.sessions) >= MAX_SESSIONS:
                return None
            s = Session()
            self.sessions[session_id] = s
            logging.info("session %016x opened by %s:%d", session_id, *addr)
        if addr not in s.players:
            if len(s.players) >= 2:
                stale = [a for a, t in s.players.items() if now - t > SLOT_STALE]
                if not stale:
                    return None
                del s.players[min(stale, key=lambda a: s.players[a])]
            logging.info("session %016x: player %s:%d joined (%d/2)", session_id, *addr,
                         len(s.players) + 1)
        s.players[addr] = now
        s.last = now
        return s

    def datagram_received(self, data, addr):
        now = time.monotonic()
        if len(data) < HEADER.size or len(data) > MAX_PACKET or not self._allowed(addr, now):
            return
        magic, version, ptype, _info, _res, session_id = HEADER.unpack_from(data)
        if magic != MAGIC or version != VERSION:
            return
        s = self._join(session_id, addr, now)
        if s is None:
            return
        if ptype == T_BIND:
            others = len(s.players) - 1
            self.transport.sendto(HEADER.pack(MAGIC, VERSION, T_ACK, others, 0, session_id), addr)
        elif ptype == T_DATA:
            for other in s.players:
                if other != addr:
                    self.transport.sendto(data, other)
                    s.forwarded += 1

    def cleanup(self):
        now = time.monotonic()
        for sid in [k for k, s in self.sessions.items() if now - s.last > SESSION_IDLE]:
            s = self.sessions.pop(sid)
            logging.info("session %016x closed (%d packets relayed)", sid, s.forwarded)
        self.rate = {a: w for a, w in self.rate.items() if now - w[0] < 5.0}


async def main():
    parser = argparse.ArgumentParser(description="shadPS4 P2P relay")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=3659)
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")
    loop = asyncio.get_running_loop()
    _, relay = await loop.create_datagram_endpoint(Relay, local_addr=(args.host, args.port))
    logging.info("Relay running on UDP port %d", args.port)
    while True:
        await asyncio.sleep(10)
        relay.cleanup()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass

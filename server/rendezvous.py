#!/usr/bin/env python3
"""n64net rendezvous server: room codes, UDP hole punching help, relay fallback.

Game-neutral. Two players meet in a room identified by a short code; the
server tells each side the other's public (as seen by the server) and LAN
address so they can open a direct UDP path ("hole punching"). If that fails,
the clients ask the server to relay: it then forwards every non-control
datagram between the two registered endpoints, unchanged.

Protocol: one ASCII datagram per message, starting with "N64R1 ".
  client -> server
    HOST <game> <code|-> <lan_ip:port>   claim a room (repeat ~1/s as keepalive)
    JOIN <game> <code> <lan_ip:port>     join a room (repeat until PEER)
    RELAY <code>                         switch this room to relaying
    BYE <code>                           leave
  server -> client
    ROOM <code>                          room held for this host
    PEER <code> <public_ip:port> <lan_ip:port> <host|guest>
    RELAYOK <code>
    ERR <reason>                         nosuchroom | full | taken | game | badcode | busy
  client <-> client (not seen by the server unless relayed)
    PUNCH <code> <role> / PUNCHACK <code> <role>

Anything that does not start with "N64R1 " and comes from an endpoint in a
relaying room is forwarded to the other member of that room.

Usage: rendezvous.py [--port 7780] [--bind 0.0.0.0]
Stdlib only, Python 3.8+.
"""
import argparse
import asyncio
import logging
import secrets
import time

PREFIX = b"N64R1 "
CODE_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"  # no I/O/0/1
CODE_LEN = 4
ROOM_IDLE_SECS = 600        # forget rooms idle this long
RELAY_IDLE_SECS = 120       # forget a relaying room with no traffic this long
MAX_ROOMS = 5000
MAX_PACKET = 1500

log = logging.getLogger("n64net")


def fmt_addr(addr):
    return f"{addr[0]}:{addr[1]}"


def valid_code(code):
    return len(code) == CODE_LEN and all(c in CODE_ALPHABET for c in code)


class Room:
    def __init__(self, code, game, host_addr, host_lan):
        self.code = code
        self.game = game
        self.host = host_addr
        self.host_lan = host_lan
        self.guest = None
        self.guest_lan = None
        self.relay = False
        self.touched = time.monotonic()
        self.relayed_packets = 0

    def other(self, addr):
        if addr == self.host:
            return self.guest
        if addr == self.guest:
            return self.host
        return None


class Rendezvous(asyncio.DatagramProtocol):
    def __init__(self):
        self.rooms = {}       # code -> Room
        self.by_addr = {}     # endpoint -> Room
        self.transport = None

    def connection_made(self, transport):
        self.transport = transport

    def send(self, addr, text):
        self.transport.sendto(PREFIX + text.encode(), addr)

    def new_code(self):
        for _ in range(100):
            code = "".join(secrets.choice(CODE_ALPHABET) for _ in range(CODE_LEN))
            if code not in self.rooms:
                return code
        return None

    def drop_room(self, room, why):
        log.info("room %s closed (%s, relayed=%d)", room.code, why, room.relayed_packets)
        self.rooms.pop(room.code, None)
        for a in (room.host, room.guest):
            if a is not None and self.by_addr.get(a) is room:
                del self.by_addr[a]

    def send_peers(self, room):
        self.send(room.host, f"PEER {room.code} {fmt_addr(room.guest)} {room.guest_lan} guest")
        self.send(room.guest, f"PEER {room.code} {fmt_addr(room.host)} {room.host_lan} host")

    def datagram_received(self, data, addr):
        if len(data) > MAX_PACKET:
            return
        if not data.startswith(PREFIX):
            room = self.by_addr.get(addr)
            if room is not None and room.relay:
                dest = room.other(addr)
                if dest is not None:
                    room.touched = time.monotonic()
                    room.relayed_packets += 1
                    self.transport.sendto(data, dest)
            return
        try:
            parts = data[len(PREFIX):].decode("ascii").split()
        except UnicodeDecodeError:
            return
        if not parts:
            return
        cmd, args = parts[0], parts[1:]
        handler = getattr(self, "cmd_" + cmd, None)
        if handler is not None:
            try:
                handler(addr, args)
            except (IndexError, ValueError):
                self.send(addr, "ERR bad")

    def cmd_HOST(self, addr, args):
        game, code, lan = args[0], args[1].upper(), args[2]
        room = self.by_addr.get(addr)
        if room is not None and room.host == addr and (code == "-" or code == room.code):
            room.touched = time.monotonic()  # keepalive
            self.send(addr, f"ROOM {room.code}")
            if room.guest is not None:
                self.send_peers(room)
            return
        if code == "-":
            code = self.new_code()
            if code is None:
                self.send(addr, "ERR busy")
                return
        elif not valid_code(code):
            self.send(addr, "ERR badcode")
            return
        old = self.rooms.get(code)
        if old is not None:
            # A host that restarted (new port) may reclaim its own code while
            # nobody has joined yet; otherwise the code is taken.
            if old.guest is None and old.host[0] == addr[0]:
                self.drop_room(old, "reclaimed")
            else:
                self.send(addr, "ERR taken")
                return
        if len(self.rooms) >= MAX_ROOMS:
            self.send(addr, "ERR busy")
            return
        if room is not None:
            self.drop_room(room, "host moved")
        room = Room(code, game, addr, lan)
        self.rooms[code] = room
        self.by_addr[addr] = room
        log.info("room %s opened by %s game=%s", code, fmt_addr(addr), game)
        self.send(addr, f"ROOM {code}")

    def cmd_JOIN(self, addr, args):
        game, code, lan = args[0], args[1].upper(), args[2]
        room = self.rooms.get(code)
        if room is None:
            self.send(addr, "ERR nosuchroom")
            return
        if room.game != game:
            self.send(addr, "ERR game")
            return
        if room.guest is not None and room.guest != addr:
            if room.guest[0] == addr[0] and not room.relay:
                # Same guest restarted on a new port before play began.
                self.by_addr.pop(room.guest, None)
            else:
                self.send(addr, "ERR full")
                return
        if room.guest is None or room.guest != addr:
            log.info("room %s joined by %s", code, fmt_addr(addr))
        room.guest = addr
        room.guest_lan = lan
        room.touched = time.monotonic()
        self.by_addr[addr] = room
        self.send_peers(room)

    def cmd_RELAY(self, addr, args):
        room = self.by_addr.get(addr)
        if room is None or room.code != args[0].upper() or room.guest is None:
            self.send(addr, "ERR nosuchroom")
            return
        if not room.relay:
            log.info("room %s relaying (asked by %s)", room.code, fmt_addr(addr))
        room.relay = True
        room.touched = time.monotonic()
        self.send(addr, f"RELAYOK {room.code}")

    def cmd_BYE(self, addr, args):
        room = self.by_addr.get(addr)
        if room is not None and room.code == args[0].upper():
            self.drop_room(room, "bye")

    def expire(self):
        now = time.monotonic()
        for room in list(self.rooms.values()):
            limit = RELAY_IDLE_SECS if room.relay else ROOM_IDLE_SECS
            if now - room.touched > limit:
                self.drop_room(room, "idle")


async def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=7780)
    ap.add_argument("-v", "--verbose", action="store_true")
    opts = ap.parse_args()
    logging.basicConfig(level=logging.DEBUG if opts.verbose else logging.INFO,
                        format="%(asctime)s %(message)s")
    loop = asyncio.get_running_loop()
    transport, proto = await loop.create_datagram_endpoint(
        Rendezvous, local_addr=(opts.bind, opts.port))
    log.info("n64net rendezvous listening on %s:%d", opts.bind, opts.port)
    try:
        while True:
            await asyncio.sleep(10)
            proto.expire()
    finally:
        transport.close()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass

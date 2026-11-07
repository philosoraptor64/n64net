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

Password (optional, --password-file): HOST/JOIN/RELAY/BYE then carry two more
fields, `<unix_time> <mac>`, where mac is the first 32 hex digits of
HMAC-SHA256(password, "<CMD> <fields...> <unix_time>"). The password never
crosses the network; the time limits replay to a few minutes. A server with a
password answers bad or missing signatures with "ERR auth" and ignores an
address after repeated failures.

Usage: rendezvous.py [--port 7780] [--bind 0.0.0.0] [--public-ip A.B.C.D]
                     [--password-file PATH]
Stdlib only, Python 3.8+.
"""
import argparse
import asyncio
import hashlib
import hmac
import ipaddress
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
AUTH_FIELDS = {"HOST": 3, "JOIN": 3, "RELAY": 1, "BYE": 1}  # fields before <time> <mac>
AUTH_WINDOW_SECS = 300      # allowed clock difference
AUTH_MAX_FAILURES = 10      # per address within AUTH_BAN_SECS ...
AUTH_BAN_SECS = 600         # ... then that address is ignored this long

log = logging.getLogger("n64net")


def fmt_addr(addr):
    return f"{addr[0]}:{addr[1]}"


def is_private(addr):
    try:
        ip = ipaddress.ip_address(addr[0])
    except ValueError:
        return False
    return ip.is_private or ip.is_loopback or ip.is_link_local


def valid_code(code):
    # Random codes avoid look-alikes (CODE_ALPHABET); codes players choose
    # themselves may use any letter or digit.
    return len(code) == CODE_LEN and code.isascii() and code.isalnum()


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
    def __init__(self, public_ip=None, password=None):
        # Set when the server runs inside a home network: players on that
        # network are seen at private addresses, so outside players are told
        # <public_ip>:<same port> instead (most routers keep the port).
        self.public_ip = public_ip
        self.password = password.encode() if password else None
        self.auth_failures = {}  # ip -> [first_failure_time, count]
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

    def visible_addr(self, member, viewer):
        """`member`'s address as `viewer` should dial it."""
        if self.public_ip and is_private(member) and not is_private(viewer):
            return f"{self.public_ip}:{member[1]}"
        return fmt_addr(member)

    def send_peers(self, room):
        self.send(room.host, f"PEER {room.code} {self.visible_addr(room.guest, room.host)} {room.guest_lan} guest")
        self.send(room.guest, f"PEER {room.code} {self.visible_addr(room.host, room.guest)} {room.host_lan} host")

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
        if cmd in AUTH_FIELDS:
            args = self.check_auth(addr, cmd, args)
            if args is None:
                return
        handler = getattr(self, "cmd_" + cmd, None)
        if handler is not None:
            try:
                handler(addr, args)
            except (IndexError, ValueError):
                self.send(addr, "ERR bad")

    def check_auth(self, addr, cmd, args):
        """Returns the fields without the signature, or None if refused."""
        n = AUTH_FIELDS[cmd]
        if self.password is None:
            return args[:n] if len(args) >= n + 2 else args
        now = time.time()
        ip = addr[0]
        fail = self.auth_failures.get(ip)
        if fail is not None and now - fail[0] > AUTH_BAN_SECS:
            del self.auth_failures[ip]
            fail = None
        if fail is not None and fail[1] >= AUTH_MAX_FAILURES:
            return None  # silently ignored while banned
        ok = False
        if len(args) == n + 2:
            try:
                stamp = int(args[n])
            except ValueError:
                stamp = None
            if stamp is not None and abs(now - stamp) <= AUTH_WINDOW_SECS:
                text = " ".join([cmd] + args[:n + 1]).encode()
                want = hmac.new(self.password, text, hashlib.sha256).hexdigest()[:32]
                ok = hmac.compare_digest(want, args[n + 1].lower())
        if ok:
            return args[:n]
        if fail is None:
            self.auth_failures[ip] = [now, 1]
        else:
            fail[1] += 1
            if fail[1] == AUTH_MAX_FAILURES:
                log.warning("ignoring %s for %ds after %d bad passwords", ip, AUTH_BAN_SECS, fail[1])
        self.send(addr, "ERR auth")
        return None

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
    ap.add_argument("--public-ip", help="this network's public IP, when hosting behind a home router")
    ap.add_argument("--password-file", help="file whose first line is the server password")
    ap.add_argument("-v", "--verbose", action="store_true")
    opts = ap.parse_args()
    logging.basicConfig(level=logging.DEBUG if opts.verbose else logging.INFO,
                        format="%(asctime)s %(message)s")
    password = None
    if opts.password_file:
        with open(opts.password_file) as f:
            password = f.readline().strip()
        if not password:
            ap.error("password file is empty")
    loop = asyncio.get_running_loop()
    transport, proto = await loop.create_datagram_endpoint(
        lambda: Rendezvous(opts.public_ip, password), local_addr=(opts.bind, opts.port))
    log.info("n64net rendezvous listening on %s:%d%s%s", opts.bind, opts.port,
             f" (public {opts.public_ip})" if opts.public_ip else "",
             ", password required" if password else ", NO password")
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

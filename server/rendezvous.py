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
    ERR <reason>                         nosuchroom | full | taken | game | badcode | busy | auth
    ERR version <host_build>             same game, different build (see below)

<game> may end in "+<build>": players of the same game with different builds
cannot stay in sync, so JOIN is refused with "ERR version" (naming the host's
build) instead of the generic "ERR game".
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
                     [--password-file PATH] [--natpmp]
Stdlib only, Python 3.8+.
"""
import argparse
import asyncio
import hashlib
import hmac
import ipaddress
import logging
import secrets
import socket
import struct
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
            # A host that restarted (new port, same address) reclaims its own
            # code, even mid-session: a phone that iOS stopped in the
            # background comes back as a new process. Its guest re-pairs on
            # its next JOIN. A different address cannot take the code.
            if old.host[0] == addr[0]:
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
            host_game, _, host_build = room.game.partition("+")
            if host_game == game.partition("+")[0]:
                self.send(addr, f"ERR version {host_build or 'unknown'}")
            else:
                self.send(addr, "ERR game")
            return
        if room.guest is not None and room.guest != addr:
            if room.guest[0] == addr[0]:
                # The same guest restarted (new port): replace the old one and
                # start over (the relay re-arms if they still need it).
                self.by_addr.pop(room.guest, None)
                room.relay = False
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


NATPMP_PORT = 5351
NATPMP_LIFETIME = 3600       # seconds; renewed at half-life


def natpmp_map(gateway, port, lifetime):
    """Ask a home router (NAT-PMP, RFC 6886) to forward UDP `port` to this
    machine. lifetime=0 removes the mapping. Returns the granted lifetime,
    or None if the router did not answer or refused."""
    req = struct.pack("!BBHHHI", 0, 1, 0, port, port if lifetime else 0, lifetime)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        timeout = 0.25
        for _ in range(4):  # RFC 6886 retry schedule, shortened
            sock.settimeout(timeout)
            sock.sendto(req, (gateway, NATPMP_PORT))
            try:
                data, _ = sock.recvfrom(16)
            except socket.timeout:
                timeout *= 2
                continue
            if len(data) < 16:
                return None
            _ver, op, result, _epoch, _internal, external, granted = struct.unpack("!BBHIHHI", data)
            if op != 129 or result != 0:
                log.warning("router refused the port mapping (NAT-PMP result %d)", result)
                return None
            if lifetime and external != port:
                log.warning("router mapped external port %d instead of %d", external, port)
            return granted
        return None
    finally:
        sock.close()


def default_gateway():
    """IPv4 default gateway (macOS/BSD `route`, Linux /proc)."""
    try:
        import subprocess
        out = subprocess.run(["route", "-n", "get", "default"], capture_output=True, text=True, timeout=5).stdout
        for line in out.splitlines():
            if line.strip().startswith("gateway:"):
                return line.split(":", 1)[1].strip()
    except (OSError, ValueError):
        pass
    try:
        with open("/proc/net/route") as f:
            for line in f.readlines()[1:]:
                fields = line.split()
                if fields[1] == "00000000":
                    return socket.inet_ntoa(struct.pack("<I", int(fields[2], 16)))
    except OSError:
        pass
    return None


async def keep_port_mapped(gateway, port):
    while True:
        granted = await asyncio.get_running_loop().run_in_executor(None, natpmp_map, gateway, port, NATPMP_LIFETIME)
        if granted:
            log.info("router %s forwards UDP %d here (NAT-PMP, renewing)", gateway, port)
            await asyncio.sleep(max(60, granted // 2))
        else:
            log.warning("NAT-PMP port mapping failed; players outside this network cannot connect")
            await asyncio.sleep(60)


async def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=7780)
    ap.add_argument("--public-ip", help="this network's public IP, when hosting behind a home router")
    ap.add_argument("--password-file", help="file whose first line is the server password")
    ap.add_argument("--natpmp", action="store_true",
                    help="ask the home router (NAT-PMP) to forward the port while running")
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
    # Closing the terminal (SIGHUP) or `kill` (SIGTERM) should still run the
    # cleanup below, which removes the router port mapping.
    import signal
    main_task = asyncio.current_task()
    for sig in (signal.SIGHUP, signal.SIGTERM):
        loop.add_signal_handler(sig, main_task.cancel)
    mapper = None
    gateway = default_gateway() if opts.natpmp else None
    if opts.natpmp:
        if gateway:
            mapper = asyncio.create_task(keep_port_mapped(gateway, opts.port))
        else:
            log.warning("--natpmp: no default gateway found")
    try:
        while True:
            await asyncio.sleep(10)
            proto.expire()
    finally:
        if mapper is not None:
            mapper.cancel()
            if natpmp_map(gateway, opts.port, 0) is not None:
                log.info("router port mapping removed")
        transport.close()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except (KeyboardInterrupt, asyncio.CancelledError):
        pass

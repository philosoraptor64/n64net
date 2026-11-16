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
    MOVE <code> <host|guest> <token> [relay]
                                         a paired member's address changed (a phone
                                         went from Wi-Fi to cellular): the token from
                                         its PEER message proves it; the server re-
                                         introduces both (PEER) and keeps relaying to
                                         the new address. Also a keepalive (~1/min).
                                         "relay": switch the room to relaying (both
                                         members get RELAYOK).
    BYE <code>                           leave
    INVITE <code> <ttl_secs>             (password only) get an invite pass
    HOSTN <game> <code|-> <lan_ip:port>  like HOST, for a room of up to 4 machines (below)
    JOINN <game> <code> <lan_ip:port>    like JOIN, into such a room
  server -> client
    ROOM <code>                          room held for this host
    PEER <code> <public_ip:port> <lan_ip:port> <host|guest> <token>
                                         token: the RECIPIENT's own move token (MOVE)
    MOVEOK <code>                        MOVE from an unchanged address (keepalive)
    RELAYOK <code>
    INVITE <code> <pass>                 a pass for that room (see below)
    ERR <reason>                         nosuchroom | full | taken | game | badcode | busy | auth
    ERR version <host_build>             same game, different build (see below)
    MEMBER <code> <index> <public_ip:port> <lan_ip:port> <your_index> <your_token>
                                         (rooms for up to 4) one other member; index 0 = host
    LEFT <code> <index>                  (rooms for up to 4) that member left (BYE or idle)

Rooms for up to 4 machines (HOSTN): the host is member 0, each JOINN takes the
lowest free index 1-3. On every join the server introduces every pair: each
member gets a MEMBER message for every other member (repeated with the host's
keepalive, so a lost one is resent), and they hole-punch pairwise. MOVE finds
the member by its token (any role word). In a relaying room the server forwards
each non-control datagram to EVERY other member (clients receive their peers'
broadcasts as usual). An old client may JOIN such a room while only the host is
in it; it then gets PEER as in a two-player room, and the room takes no one else.

<game> may end in "+<build>": players of the same game with different builds
cannot stay in sync, so JOIN is refused with "ERR version" (naming the host's
build) instead of the generic "ERR game".
Matchmaking ("look for a match"; every message starts with the version word M1, signed like HOST):
  client -> server
    PRES M1 <device> <name> <state> <info>   presence, ~every 30 s while the app is open (gone after 90 s)
                                         device: 6-32 letters/digits, random and kept by the app
                                         name: percent-encoded UTF-8 (no spaces), <= 96 bytes
                                         state: lobby | seeking | playing | away; info: "-" or e.g. a game id
    SEEK M1 <device> <name> <session> <games> <lan_ip:port>
                                         look for a match (repeat ~every 3 s; dropped after 12 s of silence):
                                         session = the app's room game id (both must match, e.g.
                                         "i64-session-1"); games = comma list of "<game>+<build>" it would play
                                         (a pairing needs at least one in common)
    UNSEEK M1 <device>                   stop looking
    LIST M1 <device> <page>              who is online and which rooms are open, page 0..
    OPEN M1 <device> <code> <0|1>        the host lists its room (HOSTN) as open to join, or not
  server -> client
    PRESOK M1 <online>                   people online in the caller's group
    SEEKING M1 <secs_waited> <others_seeking>
    MATCH M1 <code> <host|guest> <game> <peer_device> <peer_name> <relay 0|1>
                                         paired: the host opens <code> with HOSTN (the code is held for it 60 s),
                                         the guest joins it with JOINN; relay 1 = connect through the server only
                                         (always 0 between friends; for strangers later). Sent again if either side
                                         repeats SEEK in the next 60 s.
    PEOPLE M1 <page> <pages> <name,state,info>...
    ROOMS M1 <page> <pages> <code,host_name,info,members,max>...
    OPENOK M1 <code> <0|1>
  Groups: who may see and meet whom. A caller signed with the server password is in "friends"; on a server
  without a password everyone is in "open". Invite passes don't reach matchmaking. (Later: a "public" group,
  where every MATCH says relay 1.)
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

Invite passes: so invite links needn't carry the password, a client that
knows it asks `INVITE <code> <ttl>` and gets `INVITE <code> <expiry>-<mac>`,
mac = first 24 hex digits of HMAC-SHA256(key, "invite <code> <expiry>"), key =
HMAC-SHA256(password, "n64net invite key"). A guest may then sign JOIN, RELAY
and BYE for THAT room with `pass <pass>` in place of `<unix_time> <mac>`,
until the expiry. A pass cannot HOST or make passes, and bad passes count as
failed passwords. The key follows the password, so passes survive restarts.

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
AUTH_FIELDS = {"HOST": 3, "JOIN": 3, "RELAY": 1, "BYE": 1, "INVITE": 2, "HOSTN": 3, "JOINN": 3,  # before <time> <mac>
               "PRES": 5, "SEEK": 6, "UNSEEK": 2, "LIST": 3, "OPEN": 4}
PASS_COMMANDS = {"JOIN": 1, "RELAY": 0, "BYE": 0, "JOINN": 1}  # may use an invite pass; index of <code>
INVITE_MAX_TTL = 7 * 24 * 3600
AUTH_WINDOW_SECS = 300      # allowed clock difference
AUTH_MAX_FAILURES = 10      # per address within AUTH_BAN_SECS ...
AUTH_BAN_SECS = 600         # ... then that address is ignored this long
MATCH_VERSION = "M1"
PRESENCE_SECS = 90          # forget a device this long after its last PRES/SEEK
SEEK_SECS = 12              # a seeker that stopped repeating SEEK is dropped
MATCH_HOLD_SECS = 60        # a paired code is held for its host; MATCH is repeated to both meanwhile
LIST_PER_PAGE = 12
MAX_PEOPLE = 20000
STATES = ("lobby", "seeking", "playing", "away")

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


MAX_MEMBERS = 4


class Member:
    def __init__(self, addr, lan):
        self.addr = addr
        self.lan = lan
        self.token = secrets.token_hex(8)


class Person:
    """Matchmaking presence: one per device (its random id), refreshed by PRES / SEEK."""
    def __init__(self, device):
        self.device = device
        self.addr = None
        self.name = "-"
        self.state = "lobby"
        self.info = "-"
        self.group = None
        self.seen = 0.0


class Seeker:
    def __init__(self, device, since):
        self.device = device
        self.since = since
        self.last = since
        self.addr = None
        self.name = "-"
        self.group = None
        self.session = ""
        self.games = []
        self.lan = "-"


class Room:
    def __init__(self, code, game, host_addr, host_lan, multi=False):
        self.code = code
        self.game = game
        self.host = host_addr
        self.host_lan = host_lan
        self.guest = None
        self.guest_lan = None
        self.relay = False
        self.touched = time.monotonic()
        self.relayed_packets = 0
        self.host_token = secrets.token_hex(8)
        self.guest_token = None
        # rooms for up to 4 (HOSTN): members[index]; index 0 = the host. `legacy`: an old client joined
        # with JOIN, so the room is a two-player room from then on.
        self.multi = multi
        self.legacy = False
        self.open_group = None   # matchmaking: listed as open to this group (OPEN), with the host's device
        self.open_device = None
        self.members = [Member(host_addr, host_lan)] + [None] * (MAX_MEMBERS - 1) if multi else None
        if multi:
            self.members[0].token = self.host_token

    def other(self, addr):
        if addr == self.host:
            return self.guest
        if addr == self.guest:
            return self.host
        return None

    def index_of(self, addr):
        if self.members:
            for i, m in enumerate(self.members):
                if m is not None and m.addr == addr:
                    return i
        return None

    def others(self, addr):
        """Every other member's address (relaying)."""
        if self.multi and not self.legacy:
            return [m.addr for m in self.members if m is not None and m.addr != addr]
        o = self.other(addr)
        return [o] if o is not None else []

    def addresses(self):
        if self.multi and not self.legacy:
            return [m.addr for m in self.members if m is not None]
        return [a for a in (self.host, self.guest) if a is not None]


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
        # matchmaking
        self.people = {}      # device -> Person
        self.seekers = {}     # device -> Seeker
        self.matched = {}     # device -> (MATCH text, expiry)
        self.reserved = {}    # code -> (host ip, expiry): held for a matched host

    def connection_made(self, transport):
        self.transport = transport

    def send(self, addr, text):
        self.transport.sendto(PREFIX + text.encode(), addr)

    def new_code(self):
        for _ in range(100):
            code = "".join(secrets.choice(CODE_ALPHABET) for _ in range(CODE_LEN))
            if code not in self.rooms and code not in self.reserved:
                return code
        return None

    def drop_room(self, room, why):
        log.info("room %s closed (%s, relayed=%d)", room.code, why, room.relayed_packets)
        self.rooms.pop(room.code, None)
        for a in room.addresses() + [room.host, room.guest]:
            if a is not None and self.by_addr.get(a) is room:
                del self.by_addr[a]

    def send_members(self, room):
        """Rooms for up to 4: every member learns every other member."""
        ms = [(i, m) for i, m in enumerate(room.members) if m is not None]
        for i, m in ms:
            for j, o in ms:
                if j != i:
                    self.send(m.addr, f"MEMBER {room.code} {j} {self.visible_addr(o.addr, m.addr)} {o.lan} {i} {m.token}")

    def drop_member(self, room, index, why):
        m = room.members[index]
        if m is None:
            return
        log.info("room %s: member %d left (%s)", room.code, index, why)
        room.members[index] = None
        if self.by_addr.get(m.addr) is room:
            del self.by_addr[m.addr]
        for o in room.members:
            if o is not None:
                self.send(o.addr, f"LEFT {room.code} {index}")

    def visible_addr(self, member, viewer):
        """`member`'s address as `viewer` should dial it."""
        if self.public_ip and is_private(member) and not is_private(viewer):
            return f"{self.public_ip}:{member[1]}"
        return fmt_addr(member)

    def send_peers(self, room):
        self.send(room.host, f"PEER {room.code} {self.visible_addr(room.guest, room.host)} {room.guest_lan} guest {room.host_token}")
        self.send(room.guest, f"PEER {room.code} {self.visible_addr(room.host, room.guest)} {room.host_lan} host {room.guest_token}")

    def datagram_received(self, data, addr):
        if len(data) > MAX_PACKET:
            return
        if not data.startswith(PREFIX):
            room = self.by_addr.get(addr)
            if room is not None and room.relay:
                dests = room.others(addr)
                if dests:
                    room.touched = time.monotonic()
                    room.relayed_packets += 1
                    for dest in dests:
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
        if len(args) == n + 2 and args[n] == "pass" and cmd in PASS_COMMANDS:
            ok = self.pass_ok(args[PASS_COMMANDS[cmd]].upper(), args[n + 1])
        elif len(args) == n + 2:
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

    def invite_mac(self, code, expiry):
        key = hmac.new(self.password, b"n64net invite key", hashlib.sha256).digest()
        return hmac.new(key, f"invite {code} {expiry}".encode(), hashlib.sha256).hexdigest()[:24]

    def pass_ok(self, code, token):
        expiry, _, mac = token.partition("-")
        try:
            expiry_n = int(expiry)
        except ValueError:
            return False
        if expiry_n < time.time() or not valid_code(code):
            return False
        return hmac.compare_digest(self.invite_mac(code, expiry_n), mac.lower())

    def cmd_INVITE(self, addr, args):
        code = args[0].upper()
        if not valid_code(code):
            self.send(addr, "ERR badcode")
            return
        ttl = max(60, min(int(args[1]), INVITE_MAX_TTL))
        expiry = int(time.time()) + ttl
        if self.password is None:
            self.send(addr, f"INVITE {code} {expiry}-open")  # open server: any pass works
            return
        log.info("invite pass for room %s (%d s) to %s", code, ttl, fmt_addr(addr))
        self.send(addr, f"INVITE {code} {expiry}-{self.invite_mac(code, expiry)}")

    def cmd_HOSTN(self, addr, args):
        self.cmd_HOST(addr, args, multi=True)

    def cmd_HOST(self, addr, args, multi=False):
        game, code, lan = args[0], args[1].upper(), args[2]
        room = self.by_addr.get(addr)
        if room is not None and room.host == addr and (code == "-" or code == room.code):
            room.touched = time.monotonic()  # keepalive
            self.send(addr, f"ROOM {room.code}")
            if room.multi and not room.legacy:
                if sum(m is not None for m in room.members) > 1:
                    self.send_members(room)
            elif room.guest is not None:
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
        held = self.reserved.get(code)
        if held is not None and held[0] != addr[0] and held[1] > time.monotonic():
            self.send(addr, "ERR taken")   # held for the host a MATCH named
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
        room = Room(code, game, addr, lan, multi)
        self.rooms[code] = room
        self.by_addr[addr] = room
        log.info("room %s opened by %s game=%s%s", code, fmt_addr(addr), game, " (up to 4)" if multi else "")
        self.send(addr, f"ROOM {code}")

    def cmd_JOINN(self, addr, args):
        self.cmd_JOIN(addr, args, multi=True)

    def join_multi(self, room, addr, lan):
        """JOINN into a room for up to 4: the lowest free index (or the member's own, when repeated)."""
        idx = room.index_of(addr)
        if idx is None:
            free = [i for i in range(1, MAX_MEMBERS) if room.members[i] is None]
            if not free:
                self.send(addr, "ERR full")
                return
            idx = free[0]
            room.members[idx] = Member(addr, lan)
            log.info("room %s joined by %s as member %d", room.code, fmt_addr(addr), idx)
        else:
            room.members[idx].lan = lan
        room.touched = time.monotonic()
        self.by_addr[addr] = room
        self.send_members(room)

    def cmd_JOIN(self, addr, args, multi=False):
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
        if room.multi and room.legacy and multi and room.guest is not None and room.guest != addr:
            self.send(addr, "ERR full")   # an old client made it a two-player room
            return
        if room.multi and not room.legacy:
            if multi:
                self.join_multi(room, addr, lan)
                return
            # an old client: only into a room with just its host; the room is two-player from now on
            if any(m is not None for m in room.members[1:]):
                self.send(addr, "ERR full")
                return
            room.legacy = True
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
            room.guest_token = secrets.token_hex(8)
        room.guest = addr
        room.guest_lan = lan
        room.touched = time.monotonic()
        self.by_addr[addr] = room
        self.send_peers(room)

    def cmd_RELAY(self, addr, args):
        room = self.by_addr.get(addr)
        if room is not None and room.multi and not room.legacy and room.code == args[0].upper():
            if not room.relay:
                log.info("room %s relaying for every member (asked by %s)", room.code, fmt_addr(addr))
            room.relay = True
            room.touched = time.monotonic()
            for a in room.addresses():
                self.send(a, f"RELAYOK {room.code}")
            return
        if room is None or room.code != args[0].upper() or room.guest is None:
            self.send(addr, "ERR nosuchroom")
            return
        if not room.relay:
            log.info("room %s relaying (asked by %s)", room.code, fmt_addr(addr))
        room.relay = True
        room.touched = time.monotonic()
        self.send(addr, f"RELAYOK {room.code}")

    def cmd_MOVE(self, addr, args):
        code, role, token = args[0].upper(), args[1], args[2]
        want_relay = len(args) > 3 and args[3] == "relay"
        room = self.rooms.get(code)
        if room is not None and room.multi and not room.legacy:
            self.move_multi(room, addr, token, want_relay)
            return
        if room is None or room.guest is None:
            self.send(addr, "ERR nosuchroom")
            return
        if role == "host" and hmac.compare_digest(token, room.host_token):
            old = room.host
        elif role == "guest" and room.guest_token and hmac.compare_digest(token, room.guest_token):
            old = room.guest
        else:
            self.send(addr, "ERR auth")
            return
        room.touched = time.monotonic()
        moved = old != addr
        if moved:
            if self.by_addr.get(old) is room:
                del self.by_addr[old]
            if role == "host":
                room.host = addr
            else:
                room.guest = addr
            self.by_addr[addr] = room
            log.info("room %s: %s moved %s -> %s", room.code, role, fmt_addr(old), fmt_addr(addr))
        if want_relay and not room.relay:
            room.relay = True
            log.info("room %s relaying (after a move, asked by %s)", room.code, role)
        if moved:
            self.send_peers(room)
        else:
            self.send(addr, f"MOVEOK {room.code}")
        if room.relay:
            # (again on every MOVE: a member that moved after the switch never got the first one)
            self.send(room.host, f"RELAYOK {room.code}")
            self.send(room.guest, f"RELAYOK {room.code}")

    def move_multi(self, room, addr, token, want_relay):
        idx = None
        for i, m in enumerate(room.members):
            if m is not None and hmac.compare_digest(token, m.token):
                idx = i
                break
        if idx is None:
            self.send(addr, "ERR auth")
            return
        m = room.members[idx]
        room.touched = time.monotonic()
        moved = m.addr != addr
        if moved:
            if self.by_addr.get(m.addr) is room:
                del self.by_addr[m.addr]
            log.info("room %s: member %d moved %s -> %s", room.code, idx, fmt_addr(m.addr), fmt_addr(addr))
            m.addr = addr
            if idx == 0:
                room.host = addr
            self.by_addr[addr] = room
        if want_relay and not room.relay:
            room.relay = True
            log.info("room %s relaying for every member (after a move of member %d)", room.code, idx)
        if moved:
            self.send_members(room)
        else:
            self.send(addr, f"MOVEOK {room.code}")
        if room.relay:
            for a in room.addresses():
                self.send(a, f"RELAYOK {room.code}")

    def cmd_BYE(self, addr, args):
        room = self.by_addr.get(addr)
        if room is not None and room.code == args[0].upper():
            if room.multi and not room.legacy:
                idx = room.index_of(addr)
                if idx not in (None, 0):
                    self.drop_member(room, idx, "bye")
                    return
            self.drop_room(room, "bye")

    # ---------------------------------------------------------------- matchmaking

    def group_of(self, addr):
        """Who the caller may see and meet. Every matchmaking command is signed with the password (check_auth),
        so on a password server the caller is a friend; without one, everyone is in "open"."""
        return "friends" if self.password is not None else "open"

    @staticmethod
    def valid_device(d):
        return 6 <= len(d) <= 32 and d.isascii() and d.isalnum()

    @staticmethod
    def valid_word(w, limit):
        return 0 < len(w) <= limit and w.isascii() and "," not in w and all(32 < ord(c) < 127 for c in w)

    def touch_person(self, addr, device, name, state, info):
        if device not in self.people and len(self.people) >= MAX_PEOPLE:
            return None
        p = self.people.get(device)
        if p is None:
            p = self.people[device] = Person(device)
        p.addr, p.name, p.state, p.info = addr, name, state, info
        p.group = self.group_of(addr)
        p.seen = time.monotonic()
        return p

    def check_version(self, addr, args):
        if not args or args[0] != MATCH_VERSION:
            self.send(addr, "ERR matchversion")
            return False
        return True

    def cmd_PRES(self, addr, args):
        if not self.check_version(addr, args):
            return
        device, name, state, info = args[1], args[2], args[3], args[4]
        if not self.valid_device(device) or not self.valid_word(name, 96) or state not in STATES or \
                not self.valid_word(info, 64):
            self.send(addr, "ERR bad")
            return
        p = self.touch_person(addr, device, name, state, info)
        if p is None:
            self.send(addr, "ERR busy")
            return
        if state != "seeking":
            self.seekers.pop(device, None)
        online = sum(1 for o in self.people.values() if o.group == p.group)
        self.send(addr, f"PRESOK {MATCH_VERSION} {online}")

    def cmd_SEEK(self, addr, args):
        if not self.check_version(addr, args):
            return
        device, name, session, games, lan = args[1], args[2], args[3], args[4], args[5]
        if not self.valid_device(device) or not self.valid_word(name, 96) or not self.valid_word(session, 64):
            self.send(addr, "ERR bad")
            return
        game_list = [g for g in games.split(",") if g][:64]
        if not game_list or any(len(g) > 80 for g in game_list):
            self.send(addr, "ERR bad")
            return
        now = time.monotonic()
        m = self.matched.get(device)
        if m is not None and m[1] > now:
            self.send(addr, m[0])       # already paired: say it again
            return
        p = self.touch_person(addr, device, name, "seeking", "-")
        if p is None:
            self.send(addr, "ERR busy")
            return
        me = self.seekers.get(device)
        if me is None:
            me = self.seekers[device] = Seeker(device, now)
            log.info("matchmaking: %s is looking (%s)", device, games)
        me.addr, me.name, me.group, me.session, me.games, me.lan, me.last = \
            addr, name, p.group, session, game_list, lan, now
        # the longest-waiting compatible seeker of the same group
        best, common = None, None
        for o in self.seekers.values():
            if o.device == device or o.group != me.group or o.session != me.session or now - o.last > SEEK_SECS:
                continue
            shared = [g for g in o.games if g in me.games]
            if shared and (best is None or o.since < best.since):
                best, common = o, shared[0]
        if best is None:
            others = sum(1 for o in self.seekers.values()
                         if o.device != device and o.group == me.group and now - o.last <= SEEK_SECS)
            self.send(addr, f"SEEKING {MATCH_VERSION} {int(now - me.since)} {others}")
            return
        host, guest = best, me      # the one who waited longer hosts
        code = self.new_code()
        if code is None:
            self.send(addr, "ERR busy")
            return
        relay = 1 if host.group == "public" else 0
        expiry = now + MATCH_HOLD_SECS
        self.reserved[code] = (host.addr[0], expiry)
        for a, b, role in ((host, guest, "host"), (guest, host, "guest")):
            text = f"MATCH {MATCH_VERSION} {code} {role} {common} {b.device} {b.name} {relay}"
            self.matched[a.device] = (text, expiry)
            self.seekers.pop(a.device, None)
            if a.device in self.people:
                self.people[a.device].state = "playing"
            self.send(a.addr, text)
        log.info("matchmaking: %s + %s -> room %s (%s)", host.device, guest.device, code, common)

    def cmd_UNSEEK(self, addr, args):
        if not self.check_version(addr, args):
            return
        device = args[1]
        self.seekers.pop(device, None)
        self.matched.pop(device, None)
        p = self.people.get(device)
        if p is not None and p.addr == addr:
            p.state = "lobby"
        self.send(addr, f"SEEKING {MATCH_VERSION} 0 -1")   # -1: not looking any more

    def cmd_OPEN(self, addr, args):
        if not self.check_version(addr, args):
            return
        device, code, flag = args[1], args[2].upper(), args[3]
        room = self.rooms.get(code)
        if room is None or room.host != addr or not self.valid_device(device):
            self.send(addr, "ERR nosuchroom")
            return
        if flag == "1":
            room.open_group, room.open_device = self.group_of(addr), device
        else:
            room.open_group, room.open_device = None, None
        self.send(addr, f"OPENOK {MATCH_VERSION} {code} {1 if room.open_group else 0}")

    def cmd_LIST(self, addr, args):
        if not self.check_version(addr, args):
            return
        device, page = args[1], max(0, int(args[2]))
        group = self.group_of(addr)
        people = sorted((p for p in self.people.values() if p.group == group and p.device != device),
                        key=lambda p: (p.state != "seeking", p.name))
        rooms = []
        for r in self.rooms.values():
            if r.open_group != group or not r.multi or r.legacy:
                continue
            members = sum(m is not None for m in r.members)
            if members >= MAX_MEMBERS:
                continue
            host = self.people.get(r.open_device)
            rooms.append((r.code, host.name if host else "-", host.info if host else "-", members))

        def pages(n):
            return max(1, (n + LIST_PER_PAGE - 1) // LIST_PER_PAGE)

        sl = slice(page * LIST_PER_PAGE, (page + 1) * LIST_PER_PAGE)
        self.send(addr, " ".join([f"PEOPLE {MATCH_VERSION} {page} {pages(len(people))}"] +
                                 [f"{p.name},{p.state},{p.info}" for p in people[sl]]))
        self.send(addr, " ".join([f"ROOMS {MATCH_VERSION} {page} {pages(len(rooms))}"] +
                                 [f"{c},{n},{i},{m},{MAX_MEMBERS}" for c, n, i, m in rooms[sl]]))

    def expire(self):
        now = time.monotonic()
        for room in list(self.rooms.values()):
            limit = RELAY_IDLE_SECS if room.relay else ROOM_IDLE_SECS
            if now - room.touched > limit:
                self.drop_room(room, "idle")
        for d, p in list(self.people.items()):
            if now - p.seen > PRESENCE_SECS:
                del self.people[d]
        for d, sk in list(self.seekers.items()):
            if now - sk.last > SEEK_SECS:
                del self.seekers[d]
        for d, m in list(self.matched.items()):
            if m[1] <= now:
                del self.matched[d]
        for c, h in list(self.reserved.items()):
            if h[1] <= now:
                del self.reserved[c]


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

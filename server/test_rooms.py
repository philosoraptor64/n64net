#!/usr/bin/env python3
"""Tests for rendezvous.py rooms (two-player and up to 4): starts a local server with a password and talks
to it over UDP. Run: python3 server/test_rooms.py  (exit 0 = all pass)."""
import hashlib
import hmac
import os
import socket
import subprocess
import sys
import tempfile
import time

PW = "testpw"
HERE = os.path.dirname(os.path.abspath(__file__))


def sign(cmd, fields):
    t = str(int(time.time()))
    mac = hmac.new(PW.encode(), " ".join([cmd] + fields + [t]).encode(), hashlib.sha256).hexdigest()[:32]
    return " ".join(fields + [t, mac])


class Client:
    def __init__(self, port):
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.bind(("127.0.0.1", 0))
        self.s.settimeout(0.3)
        self.server = ("127.0.0.1", port)

    def cmd(self, cmd, fields, signed=True):
        text = f"{cmd} {sign(cmd, fields) if signed else ' '.join(fields)}"
        self.s.sendto(b"N64R1 " + text.encode(), self.server)

    def raw(self, data):
        self.s.sendto(data, self.server)

    def recv_all(self, wait=0.4):
        out = []
        end = time.time() + wait
        while time.time() < end:
            try:
                d, _ = self.s.recvfrom(2000)
                out.append(d)
            except socket.timeout:
                pass
        return out

    def lan(self):
        return f"127.0.0.1:{self.s.getsockname()[1]}"


def texts(msgs):
    return [m[6:].decode() for m in msgs if m.startswith(b"N64R1 ")]


failures = []


def check(name, ok, detail=""):
    print(f"{'PASS' if ok else 'FAIL'}  {name}{'' if ok else '  ' + detail}")
    if not ok:
        failures.append(name)


def main():
    port = 7799
    pwfile = tempfile.NamedTemporaryFile("w", delete=False)
    pwfile.write(PW + "\n")
    pwfile.close()
    srv = subprocess.Popen([sys.executable, os.path.join(HERE, "rendezvous.py"), "--port", str(port), "--bind",
                            "127.0.0.1", "--password-file", pwfile.name], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
    time.sleep(0.8)
    try:
        G = "game+1"
        # 1. two-player rooms work as before
        h, g = Client(port), Client(port)
        h.cmd("HOST", [G, "AAAA", h.lan()])
        check("HOST gets ROOM", "ROOM AAAA" in texts(h.recv_all()))
        g.cmd("JOIN", [G, "AAAA", g.lan()])
        gt, ht = texts(g.recv_all()), texts(h.recv_all())
        check("JOIN: guest gets PEER host", any(t.startswith("PEER AAAA") and " host " in t for t in gt), str(gt))
        check("JOIN: host gets PEER guest", any(t.startswith("PEER AAAA") and " guest " in t for t in ht), str(ht))

        # 2. a room for up to 4: everyone learns everyone
        cs = [Client(port) for _ in range(5)]
        cs[0].cmd("HOSTN", [G, "BBBB", cs[0].lan()])
        check("HOSTN gets ROOM", "ROOM BBBB" in texts(cs[0].recv_all()))
        for i in (1, 2, 3):
            cs[i].cmd("JOINN", [G, "BBBB", cs[i].lan()])
            time.sleep(0.1)
        got = [texts(c.recv_all(0.6)) for c in cs[:4]]
        tokens = {}
        for i in range(4):
            seen = set()
            for t in got[i]:
                p = t.split()
                if p[0] == "MEMBER" and p[1] == "BBBB":
                    seen.add(int(p[2]))
                    check_idx = int(p[5])
                    if check_idx != i:
                        check(f"member {i} told its own index", False, t)
                    tokens[i] = p[6]
            check(f"member {i} knows the 3 others", seen == {0, 1, 2, 3} - {i}, str(sorted(seen)))
        cs[4].cmd("JOINN", [G, "BBBB", cs[4].lan()])
        check("a 5th machine: ERR full", "ERR full" in texts(cs[4].recv_all()))

        # 3. relaying: one member's datagram reaches every other member
        cs[1].cmd("RELAY", ["BBBB"])
        relayok = [("RELAYOK BBBB" in texts(c.recv_all())) for c in cs[:4]]
        check("RELAY: every member gets RELAYOK", all(relayok), str(relayok))
        cs[2].raw(b"hello-from-2")
        rec = [b"hello-from-2" in c.recv_all() for c in cs[:4]]
        check("relayed to every other member", rec == [True, True, False, True], str(rec))

        # 4. a member leaves: the others hear it; its index is free again
        cs[2].cmd("BYE", ["BBBB"])
        left = [("LEFT BBBB 2" in texts(c.recv_all())) for c in (cs[0], cs[1], cs[3])]
        check("BYE: LEFT to the others", all(left), str(left))
        cs[4].cmd("JOINN", [G, "BBBB", cs[4].lan()])
        mine = [t for t in texts(cs[4].recv_all(0.6)) if t.startswith("MEMBER")]
        check("a new member takes the free index 2", mine and all(t.split()[5] == "2" for t in mine), str(mine))

        # 5. a member moves (Wi-Fi -> cellular): same token from a new socket
        moved = Client(port)
        moved.cmd("MOVE", ["BBBB", "guest", tokens[1]], signed=False)
        mt = texts(moved.recv_all(0.6))
        check("MOVE: the moved member is re-introduced", any(t.startswith("MEMBER BBBB") and t.split()[5] == "1" for t in mt), str(mt))
        h0 = texts(cs[0].recv_all())
        port_new = moved.lan().split(":")[1]
        check("MOVE: others learn the new address", any(t.startswith("MEMBER BBBB 1 ") and port_new in t for t in h0), str(h0))

        # 6. an old client into a new room (only the host there): two-player from then on
        n0, old, late = Client(port), Client(port), Client(port)
        n0.cmd("HOSTN", [G, "CCCC", n0.lan()])
        n0.recv_all()
        old.cmd("JOIN", [G, "CCCC", old.lan()])
        check("an old client joins a new room (PEER)", any(t.startswith("PEER CCCC") for t in texts(old.recv_all())))
        late.cmd("JOINN", [G, "CCCC", late.lan()])
        check("...which then takes no one else", "ERR full" in texts(late.recv_all()))

        # 7. host leaves a room for up to 4: the room closes (host migration comes later)
        cs[0].cmd("BYE", ["BBBB"])
        cs[1].recv_all()
        cs[3].cmd("JOINN", [G, "BBBB", cs[3].lan()])
        check("host BYE closes the room", "ERR nosuchroom" in texts(cs[3].recv_all()))
    finally:
        srv.terminate()
        srv.wait(timeout=5)

    # 8. a member whose keepalives (MOVE) stop is dropped after --member-idle-secs; one that never sent any
    #    (an older client) stays until the room itself goes idle
    port2 = 7798
    srv = subprocess.Popen([sys.executable, os.path.join(HERE, "rendezvous.py"), "--port", str(port2), "--bind",
                            "127.0.0.1", "--password-file", pwfile.name, "--member-idle-secs", "2"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.8)
    try:
        G = "game+1"
        d = [Client(port2) for _ in range(3)]
        d[0].cmd("HOSTN", [G, "DDDD", d[0].lan()])
        d[0].recv_all()
        toks = {}
        for i in (1, 2):
            d[i].cmd("JOINN", [G, "DDDD", d[i].lan()])
            for t in texts(d[i].recv_all(0.6)):
                p = t.split()
                if p[0] == "MEMBER":
                    toks[i] = p[6]
        d[1].cmd("MOVE", ["DDDD", "guest", toks[1]], signed=False)   # member 1: a keepalive client, then silent
        d[1].recv_all()
        d[0].recv_all()
        heard = []
        end = time.time() + 7
        while time.time() < end and not any(t == "LEFT DDDD 1" for t in heard):
            d[0].cmd("HOSTN", [G, "DDDD", d[0].lan()])                # the host keeps the room alive
            heard += texts(d[0].recv_all(0.5))
        check("silent keepalive member: LEFT after the idle time", "LEFT DDDD 1" in heard, str(heard))
        check("member that never sent keepalives stays", "LEFT DDDD 2" not in heard, str(heard))
    finally:
        srv.terminate()
        srv.wait(timeout=5)

    # 9. ghost hosts (2026-11-24): a waiting host whose app is gone (HOST repeats stop) loses its room soon;
    #    a host whose MOVE keepalives stop loses it after the member idle time, like any member
    port3 = 7797
    srv = subprocess.Popen([sys.executable, os.path.join(HERE, "rendezvous.py"), "--port", str(port3), "--bind",
                            "127.0.0.1", "--password-file", pwfile.name, "--member-idle-secs", "2",
                            "--waiting-idle-secs", "2"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.8)
    try:
        G = "game+1"
        h = Client(port3)
        h.cmd("HOSTN", [G, "EEEE", h.lan()])
        h.recv_all()
        time.sleep(1.0)
        g = Client(port3)
        g.cmd("JOINN", [G, "FFFF", g.lan()])   # (no such room: just shows the server answers)
        g.recv_all()
        alive = Client(port3)
        alive.cmd("HOSTN", [G, "GGGG", alive.lan()])
        end = time.time() + 4.5
        while time.time() < end:
            alive.cmd("HOSTN", [G, "GGGG", alive.lan()])   # this host keeps waiting
            alive.recv_all(0.5)
        late = Client(port3)
        late.cmd("JOINN", [G, "EEEE", late.lan()])
        got = texts(late.recv_all(0.6))
        check("waiting host gone: its room closes", any(t.startswith("ERR nosuchroom") for t in got), str(got))
        late2 = Client(port3)
        late2.cmd("JOINN", [G, "GGGG", late2.lan()])
        got = texts(late2.recv_all(0.6))
        check("waiting host still repeating: its room stays", any(t.startswith("MEMBER GGGG") for t in got), str(got))

        d = [Client(port3) for _ in range(2)]
        d[0].cmd("HOSTN", [G, "HHHH", d[0].lan()])
        d[0].recv_all()
        toks = {}
        d[1].cmd("JOINN", [G, "HHHH", d[1].lan()])
        for t in texts(d[1].recv_all(0.6)):
            p = t.split()
            if p[0] == "MEMBER":
                toks[1] = p[6]
        for t in texts(d[0].recv_all(0.3)):
            p = t.split()
            if p[0] == "MEMBER":
                toks[0] = p[6]
        d[0].cmd("MOVE", ["HHHH", "host", toks.get(0, "x")], signed=False)   # the host: a keepalive client
        d[0].recv_all(0.3)
        end = time.time() + 6
        while time.time() < end:
            d[1].cmd("MOVE", ["HHHH", "guest", toks.get(1, "x")], signed=False)   # only the guest stays alive
            d[1].recv_all(0.5)
        late = Client(port3)
        late.cmd("JOINN", [G, "HHHH", late.lan()])
        got = texts(late.recv_all(0.6))
        check("host's keepalives stopped: the room closes", any(t.startswith("ERR nosuchroom") for t in got), str(got))
    finally:
        srv.terminate()
        srv.wait(timeout=5)
        os.unlink(pwfile.name)
    print("ALL PASS" if not failures else f"{len(failures)} FAILED")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())

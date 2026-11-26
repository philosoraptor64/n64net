#!/usr/bin/env python3
"""Tests for rendezvous.py matchmaking (PRES / SEEK / UNSEEK / LIST / OPEN): starts a local server with a password
and talks to it over UDP. Run: python3 server/test_match.py  (exit 0 = all pass)."""
import os
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_rooms import Client, texts, check, failures, HERE, PW  # noqa: E402

V = "M1"
SESSION = "i64-session-1"


def first(msgs, word):
    for t in texts(msgs):
        if t.split()[0] == word:
            return t.split()
    return None


def main():
    port = 7797
    pwfile = tempfile.NamedTemporaryFile("w", delete=False)
    pwfile.write(PW)
    pwfile.close()
    srv = subprocess.Popen([sys.executable, os.path.join(HERE, "rendezvous.py"), "--port", str(port), "--bind",
                            "127.0.0.1", "--password-file", pwfile.name], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
    try:
        time.sleep(0.8)
        a, b, c, d = (Client(port) for _ in range(4))

        # presence
        a.cmd("PRES", [V, "devAAAA01", "Alice", "lobby", "-"])
        r = first(a.recv_all(), "PRESOK")
        check("PRES answers PRESOK with the online count", r is not None and r[2] == "1", str(r))
        b.cmd("PRES", [V, "devBBBB02", "Bob%20B", "playing", "mk64-us+abc"])
        r = first(b.recv_all(), "PRESOK")
        check("a second device counts 2 online", r is not None and r[2] == "2", str(r))

        # unsigned and wrong-version commands are refused
        c.cmd("PRES", [V, "devCCCC03", "Eve", "lobby", "-"], signed=False)
        check("unsigned PRES refused (ERR auth)", first(c.recv_all(), "ERR") is not None)
        c.cmd("PRES", ["M9", "devCCCC03", "Eve", "lobby", "-"])
        r = first(c.recv_all(), "ERR")
        check("unknown matchmaking version refused", r is not None and r[1] == "matchversion", str(r))

        # LIST: the others, seekers first
        a.cmd("LIST", [V, "devAAAA01", "0"])
        msgs = a.recv_all()
        people, rooms = first(msgs, "PEOPLE"), first(msgs, "ROOMS")
        check("LIST shows Bob (not Alice herself)", people is not None and any(e.startswith("Bob%20B,playing")
              for e in people[4:]) and not any(e.startswith("Alice") for e in people[4:]), str(people))
        check("LIST rooms: none open yet", rooms is not None and rooms[4:] == [], str(rooms))

        # SEEK: alone first
        a.cmd("SEEK", [V, "devAAAA01", "Alice", SESSION, "mk64-us+abc,ssb64-us+def", a.lan()])
        r = first(a.recv_all(), "SEEKING")
        check("lone seeker gets SEEKING", r is not None and r[3] == "0", str(r))

        # no game in common: no match
        c.cmd("SEEK", [V, "devCCCC03", "Carl", SESSION, "pd-us+zzz", c.lan()])
        r = first(c.recv_all(), "SEEKING")
        check("no common game: still SEEKING (sees 1 other seeker)", r is not None and r[3] == "1", str(r))

        # a different session id never pairs
        d.cmd("SEEK", [V, "devDDDD04", "Dee", "i64-session-2", "mk64-us+abc", d.lan()])
        r = first(d.recv_all(), "SEEKING")
        check("other session id: SEEKING", r is not None, str(r))

        # Bob seeks with a game in common with Alice: MATCH to both, Alice (waited longer) hosts
        b.cmd("SEEK", [V, "devBBBB02", "Bob%20B", SESSION, "ssb64-us+def,mk64-us+abc", b.lan()])
        mb, ma = first(b.recv_all(), "MATCH"), first(a.recv_all(), "MATCH")
        check("Bob gets MATCH as guest", mb is not None and mb[3] == "guest" and mb[5] == "devAAAA01" and
              mb[6] == "Alice" and mb[7] == "0", str(mb))
        check("Alice gets MATCH as host, same code", ma is not None and mb is not None and ma[2] == mb[2] and
              ma[3] == "host" and ma[6] == "Bob%20B", str(ma))
        check("the common game is the earlier seeker's first shared choice", ma is not None and ma[4] == "mk64-us+abc",
              str(ma))
        code = ma[2] if ma else "XXXX"

        # repeating SEEK repeats the MATCH
        b.cmd("SEEK", [V, "devBBBB02", "Bob%20B", SESSION, "ssb64-us+def", b.lan()])
        r = first(b.recv_all(), "MATCH")
        check("repeated SEEK gets the same MATCH", r is not None and r[2] == code, str(r))

        # the code is held for Alice: someone else can't HOST it, Alice can; Bob joins with JOINN
        # (the hold is by IP address, and every test client is 127.0.0.1: the refusal of another address can't be
        # shown here; the matched host opening its held code can)
        a.cmd("HOSTN", [SESSION, code, a.lan()])
        r = first(a.recv_all(), "ROOM")
        check("the matched host opens the held code", r is not None and r[1] == code, str(r))
        b.cmd("JOINN", [SESSION, code, b.lan()])
        r = first(b.recv_all(), "MEMBER")
        check("the matched guest joins it", r is not None and r[1] == code, str(r))
    finally:
        srv.terminate()
        srv.wait()
        os.unlink(pwfile.name)


def main_rooms():
    """OPEN / LIST rooms and the hold, with the host at its own address."""
    port = 7796
    pwfile = tempfile.NamedTemporaryFile("w", delete=False)
    pwfile.write(PW)
    pwfile.close()
    srv = subprocess.Popen([sys.executable, os.path.join(HERE, "rendezvous.py"), "--port", str(port), "--bind",
                            "127.0.0.1", "--password-file", pwfile.name], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
    try:
        time.sleep(0.8)
        h, g, x = Client(port), Client(port), Client(port)
        h.cmd("PRES", [V, "hostdev01", "Hana", "lobby", "mk64-us+abc"])
        h.recv_all()
        h.cmd("HOSTN", [SESSION, "-", h.lan()])
        r = first(h.recv_all(), "ROOM")
        code = r[1] if r else "XXXX"
        x.cmd("OPEN", [V, "xdev0001", code, "1"])
        r = first(x.recv_all(), "ERR")
        check("only the room's host may OPEN it", r is not None and r[1] == "nosuchroom", str(r))
        h.cmd("OPEN", [V, "hostdev01", code, "1"])
        r = first(h.recv_all(), "OPENOK")
        check("host OPENs its room", r is not None and r[2] == code and r[3] == "1", str(r))
        g.cmd("LIST", [V, "guestdev1", "0"])
        rooms = first(g.recv_all(), "ROOMS")
        check("LIST shows the open room with the host's name and info",
              rooms is not None and f"{code},Hana,mk64-us+abc,1,4" in rooms[4:], str(rooms))
        g.cmd("JOINN", [SESSION, code, g.lan()])
        check("a lister can JOINN the open room", first(g.recv_all(), "MEMBER") is not None)
        h.cmd("OPEN", [V, "hostdev01", code, "0"])
        h.recv_all()
        g.cmd("LIST", [V, "guestdev1", "0"])
        rooms = first(g.recv_all(), "ROOMS")
        check("closed room no longer listed", rooms is not None and rooms[4:] == [], str(rooms))

        # UNSEEK
        g.cmd("SEEK", [V, "guestdev1", "Gus", SESSION, "pd-us+1", g.lan()])
        g.recv_all()
        g.cmd("UNSEEK", [V, "guestdev1"])
        r = first(g.recv_all(), "SEEKING")
        check("UNSEEK answers SEEKING ... -1", r is not None and r[3] == "-1", str(r))
        x.cmd("SEEK", [V, "xdev0001", "Xi", SESSION, "pd-us+1", x.lan()])
        r = first(x.recv_all(), "SEEKING")
        check("after UNSEEK nobody pairs with the one who left", r is not None and r[3] == "0", str(r))

        # a seeker that goes silent is dropped (server expiry runs every 10 s; SEEK_SECS 12)
    finally:
        srv.terminate()
        srv.wait()
        os.unlink(pwfile.name)


def main_calls():
    """CALL / ANSWER / UNCALL: invite one person; the room exists only after their yes."""
    port = 7795
    pwfile = tempfile.NamedTemporaryFile("w", delete=False)
    pwfile.write(PW)
    pwfile.close()
    srv = subprocess.Popen([sys.executable, os.path.join(HERE, "rendezvous.py"), "--port", str(port), "--bind",
                            "127.0.0.1", "--password-file", pwfile.name], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
    try:
        time.sleep(0.8)
        a, b, c = Client(port), Client(port), Client(port)
        a.cmd("PRES", [V, "calleraa1", "Ann", "lobby", "-"])
        b.cmd("PRES", [V, "calleebb2", "Ben", "lobby", "-"])
        c.cmd("PRES", [V, "playercc3", "Cy", "playing", "mk64-us+1"])
        a.recv_all(), b.recv_all(), c.recv_all()
        a.cmd("LIST", [V, "calleraa1", "0"])
        people = first(a.recv_all(), "PEOPLE")
        check("PEOPLE entries carry the device (for CALL)", people is not None and "Ben,lobby,-,calleebb2"
              in people[4:], str(people))

        a.cmd("CALL", [V, "calleraa1", "Ann", "calleebb2", SESSION, "-"])
        r = first(a.recv_all(), "CALLING")
        check("CALL answers CALLING ringing", r is not None and r[2:] == ["Ben", "ringing"], str(r))
        r = first(b.recv_all(), "CALLED")
        check("the invited person gets CALLED with the caller's name", r is not None and r[2:] ==
              ["calleraa1", "Ann", "-"], str(r))

        b.cmd("ANSWER", [V, "calleebb2", "Ben", "calleraa1", "no"])
        check("a no closes the question (CALLDONE)", first(b.recv_all(), "CALLDONE") is not None)
        r = first(a.recv_all(), "CALLING")
        check("the caller hears declined", r is not None and r[3] == "declined", str(r))
        a.cmd("CALL", [V, "calleraa1", "Ann", "calleebb2", SESSION, "-"])
        r = first(a.recv_all(), "CALLING")
        check("repeating a declined CALL keeps saying declined", r is not None and r[3] == "declined", str(r))
        check("... and doesn't ring again", first(b.recv_all(), "CALLED") is None)
        a.cmd("UNCALL", [V, "calleraa1"])
        a.recv_all()

        a.cmd("CALL", [V, "calleraa1", "Ann", "calleebb2", SESSION, "-"])
        a.recv_all(), b.recv_all()
        b.cmd("ANSWER", [V, "calleebb2", "Ben", "calleraa1", "yes"])
        rb = first(b.recv_all(), "MATCH")
        ra = first(a.recv_all(), "MATCH")
        check("a yes pairs them: the invited person joins", rb is not None and rb[3] == "guest", str(rb))
        check("... the caller hosts the same code", ra is not None and ra[3] == "host" and rb is not None and
              ra[2] == rb[2], str(ra))
        a.cmd("CALL", [V, "calleraa1", "Ann", "calleebb2", SESSION, "-"])
        r = first(a.recv_all(), "MATCH")
        check("a repeated CALL after the yes repeats MATCH", r is not None and ra is not None and r[2] == ra[2], str(r))
        b.cmd("ANSWER", [V, "calleebb2", "Ben", "calleraa1", "yes"])
        r = first(b.recv_all(), "MATCH")
        check("a repeated yes repeats MATCH", r is not None and rb is not None and r[2] == rb[2], str(r))
        a.cmd("HOSTN", [SESSION, ra[2] if ra else "-", a.lan()])
        r = first(a.recv_all(), "ROOM")
        check("the caller opens the held code", r is not None and ra is not None and r[1] == ra[2], str(r))

        # someone playing is busy; an unknown device is offline
        d = Client(port)
        d.cmd("CALL", [V, "dialerdd4", "Di", "playercc3", SESSION, "-"])
        r = first(d.recv_all(), "CALLING")
        check("calling someone in a game: busy", r is not None and r[3] == "busy", str(r))
        d.cmd("CALL", [V, "dialerdd4", "Di", "nobody999", SESSION, "-"])
        r = first(d.recv_all(), "CALLING")
        check("calling someone not online: offline", r is not None and r[3] == "offline", str(r))

        # UNCALL while ringing closes the question on the other side
        e = Client(port)
        e.cmd("PRES", [V, "calleeee5", "Eve", "lobby", "-"])
        e.recv_all()
        d.cmd("CALL", [V, "dialerdd4", "Di", "calleeee5", SESSION, "-"])
        d.recv_all()
        check("Eve is rung", first(e.recv_all(), "CALLED") is not None)
        d.cmd("UNCALL", [V, "dialerdd4"])
        d.recv_all()
        check("UNCALL sends CALLDONE to the one rung", first(e.recv_all(), "CALLDONE") is not None)
        e.cmd("ANSWER", [V, "calleeee5", "Eve", "dialerdd4", "yes"])
        msgs = e.recv_all()
        check("a late yes gets CALLDONE, no MATCH", first(msgs, "CALLDONE") is not None and
              first(msgs, "MATCH") is None, str(texts(msgs)))
    finally:
        srv.terminate()
        srv.wait()
        os.unlink(pwfile.name)


if __name__ == "__main__":
    main()
    main_rooms()
    main_calls()
    print("ALL PASS" if not failures else f"{len(failures)} FAILED")
    sys.exit(1 if failures else 0)

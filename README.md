# n64net

Shared netplay pieces for native N64 ports (work in progress).

- `server/rendezvous.py`: room codes, UDP hole punching help, relay fallback. Python 3.8+, stdlib only. Run: `python3 server/rendezvous.py --port 7780`. The protocol is documented at the top of the file.
- Client: currently `BattleShip/port/net/n64net_rendezvous.{h,cpp}` (game-neutral C API); it moves here when the n64net library is set up.

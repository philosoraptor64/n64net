# n64net

Shared netplay pieces for native N64 ports (PC/Mac/iOS ports built on libultraship), written for
[BattleShip](https://github.com/philosoraptor64/BattleShip) (Smash 64) and
[SpaghettiKart](https://github.com/philosoraptor64/SpaghettiKart) (Mario Kart 64). Game-neutral C APIs;
each game keeps a small adapter. Work in progress.

| Piece | Where | What |
|---|---|---|
| Room-code server | `server/rendezvous.py` | Room codes, UDP hole punching help, relay fallback, optional password (HMAC-signed, never sent), NAT-PMP port opening. Python 3.8+, stdlib only. Protocol at the top of the file. |
| Rendezvous client | `client/include/n64net_rendezvous.h` | Finds the other player through a room code, on the game's own UDP socket. |
| Voice chat | `client/include/n64net_voice.h` | Opus speech over the game's socket (direct or relayed); mixed into the game's audio output. |
| Build fingerprint | `cmake/N64NetBuildId.cmake` | Hashes the game's simulation code into a build id, so the server refuses to pair builds that would desync. |

Planned next: the input ledger, prediction and rollback trigger, frame-advantage time sync,
sync-check exchange and away/back notices, extracted from BattleShip's netcode.

## Run the server
```
python3 server/rendezvous.py --port 7780 [--password-file PATH] [--natpmp] [--public-ip A.B.C.D]
```
Players at home use the server machine's LAN address; friends elsewhere use the public address
(`--natpmp` asks the router to forward the port while the server runs, if it supports NAT-PMP).

## Add it to a game
```cmake
add_subdirectory(n64net)                  # after your SDL2 target exists (voice uses it)
target_link_libraries(<game> PRIVATE n64net)
n64net_add_build_id(<game>
    HEADER  ${CMAKE_BINARY_DIR}/generated/netplay_build_id.h
    MACRO   MYGAME_NETPLAY_BUILD_ID
    ENTRIES "${CMAKE_SOURCE_DIR}::src" "${CMAKE_SOURCE_DIR}/torch::src")   # <checkout>::<path>, simulation code only
```
Options: `N64NET_VOICE` (default ON; builds Opus 1.5.2 via FetchContent), `N64NET_SDL2_TARGET` (default: the
first existing of `SDL2::SDL2-static`, `SDL2::SDL2`, `SDL2-static`, `SDL2`).

### Connecting with a room code
1. Open one non-blocking IPv4 UDP socket and bind it (port 0 is fine). Use this same socket for gameplay:
   the NAT mapping the rendezvous opens is the one your packets need.
2. Fill `N64NetRendezvousConfig`: `server` ("host:port"), `game` (`"<game id>+" MYGAME_NETPLAY_BUILD_ID`),
   `room` (4 characters; empty lets the host get a random code), `password`, `is_host`, `wait_secs`,
   `log`, and `yield` (called every ~10 ms while waiting: pump your OS event loop / watchdog here, because
   iOS kills apps that stop answering).
3. `n64net_rendezvous_connect(sock, &config, &result)` blocks until connected; then send gameplay packets
   to `result.peer`. If `result.relayed` is 1 that address is the server, which forwards everything that
   doesn't start with `N64R1 `. On failure, `result.error` says why (`version <host build>`, `auth`,
   `noroom`, `timeout`, ...): tell the player.
4. A host leaving a room (e.g. after a match) calls `n64net_rendezvous_bye`.

### Voice chat
- When connected: `n64net_voice_start(&config)` (`enabled`, `push_to_talk`, `test_tone` for tests without a
  microphone, `log`); `n64net_voice_stop()` when the session ends. Call both from the game thread.
- Every game tick (and while waiting for the peer): `n64net_voice_pump(send)`, where `send` writes the
  datagram to your socket/peer.
- Every received datagram: first `if (n64net_voice_handle_packet(data, size)) continue;` (voice packets start
  with the magic `N64V`, so they share the game's socket and relay).
- Your audio output: `n64net_voice_mix(stereo, frames, rate)` on each block you play (interleaved s16). The
  voice is paced by your audio clock. If your game can stall its audio (waiting for the peer), keep calling
  the mixer with silence in real time or the voice stalls too.
- UI: `n64net_voice_set_muted` / `set_talking` (push-to-talk) / `remote_speaking` / `local_speaking`.
- iOS: needs `NSMicrophoneUsageDescription` in Info.plist. For echo cancellation, set
  `AVAudioSessionModeVoiceChat` after the microphone opens.

## License
MIT (see `LICENSE`). Opus is BSD-licensed and fetched at build time.

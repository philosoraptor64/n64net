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
| Input exchange | `client/include/n64net_input.h` | Input ledger with input delay, prediction and the rollback trigger (or lockstep), time sync, sync checks (desync detection), away/back notices, run-time delay changes, and an optional rollback driver (save/load/advance callbacks). The header documents the per-tick calls. |

Tests: `cmake -S . -B build -DN64NET_BUILD_TESTS=ON -DN64NET_VOICE=OFF && cmake --build build && build/n64net_input_test`
(two peers over a simulated network: lockstep and rollback, loss/latency/jitter, delay changes, a late start, and
a deliberate desync).

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
   `noroom`, `timeout`, ...): tell the player. **Accept the peer's packets from either `result.peer` or
   `result.server`** (non-control datagrams from the server are the peer's, relayed): rarely one side ends
   up direct and the other relayed, and accepting both keeps that working.
4. A host leaving a room (e.g. after a match) calls `n64net_rendezvous_bye`.
5. Invite passes (links without the password): the host calls `n64net_rendezvous_request_invite(sock,
   &config, ttl_secs)`; whoever reads the socket passes the server's answer to
   `n64net_rendezvous_parse_invite()`, which yields the pass. The guest sets `config.invite_pass` (and no
   password): it may then join that room only, until the pass expires (default 24 h, max 7 days).

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
- iOS: needs `NSMicrophoneUsageDescription` in Info.plist. **Echo cancellation needs one change in your audio
  output:** while voice runs with the microphone, Apple's voice-processing unit records the mic *and* plays the
  game, so it knows what to subtract. Wherever you play a block (after `n64net_voice_mix`):
  ```c
  if (n64net_voice_output_active()) n64net_voice_output_write(stereo, frames, rate);  /* any rate */
  else  /* your normal device, e.g. AudioPlayerPlayFrame */;
  ```
  and wherever you ask the device how much is queued, use `n64net_voice_output_buffered(rate)` while active.
  Both are no-ops returning 0 on other platforms. The unit uses the loudspeaker (headphones win when
  connected), restarts itself after interruptions, and logs `n64net voice: unit: ...` with the stats line.
  Without the routing, voice-chat mode alone turns the player's own voice down ~20 dB, so n64net then
  uses the plain microphone without echo cancellation. Debug: `N64NET_VOICE_DUMP=<file.wav>` (first 90 s of
  the mic; on iOS relative to Documents), `N64NET_VOICE_UNIT=1` (unit also in test-tone mode; the mic is then
  only measured), `N64NET_VOICE_BYPASS=1` (unit without processing, for A/B), `N64NET_VOICE_PROCESSING=0`.
- Levels: the sender levels speech before encoding (`client/src/n64net_voice_dsp.h`: high-pass, noise-floor
  tracker, automatic gain to -20 dBFS speech up to +18 dB, limiter), so quiet talking arrives like a raised
  voice; the receiver ducks the game's audio by 6 dB while the other player talks (inside `n64net_voice_mix`).
  Debug: `N64NET_VOICE_LEVELER=0`, `N64NET_VOICE_DUCK=0`. Check a recording offline with
  `n64net_voice_dsp_tool in.wav [out.wav] [pre-gain]` (built with `N64NET_BUILD_TESTS`).

### Input exchange (lockstep or rollback)
1. After connecting, `n64net_input_create(&config)`: `num_players`, `local_player`, `remote_mask` (players driven
   by other machines; others read as neutral, e.g. CPUs), `session_id` (equal on both sides), `start_tick`,
   `input_delay`, `rollback_window` (0 = lockstep), `ticks_per_second`, and `send` (writes to your socket/peer).
2. Pass every received datagram to `n64net_input_handle_packet` (returns 0 for packets that aren't its own).
3. Each tick: `n64net_input_set_local(net, tick, pad)` (never during a resimulation), wait while
   `!n64net_input_ready(net, tick)` (receive packets, `n64net_input_resend`), then read every player with
   `n64net_input_get(net, p, tick, &pad)`.
4. Rollback: at the start of each simulated frame, either `n64net_rollback_frame(net, tick, &callbacks)` (your
   `save(slot)`, `load(slot)`, `advance()`), or your own snapshot ring with `n64net_take_rollback`. Once per
   displayed frame, `if (n64net_timesync_should_skip(net))` skip the game step.
5. Desync detection: keep a state hash per tick (rewritten by resimulations) and `n64net_sync_record` it once the
   tick is confirmed (`tick <= n64net_input_confirmed_tick(net) + 1`); check `n64net_sync_desynced`.
6. App switching (iOS): `n64net_input_set_away(net, 1/0)`; on the other side `n64net_input_peer_away` and
   `n64net_input_peer_silence_usecs` tell you whether to keep waiting.

## License
MIT (see `LICENSE`). Opus is BSD-licensed and fetched at build time.

## Adding a game

The standard process from a fresh ROM to a full i64 game (stages, gates and the per-game scorecard):
[docs/new-game-process.md](docs/new-game-process.md).

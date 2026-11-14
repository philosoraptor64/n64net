# New game process: from a fresh ROM to a full i64 game

The standard path every game takes to be playable in **i64** (the launcher that runs native N64 ports, not
emulation, on iPhone and Mac) with online play. It applies to everyone: the project's agents, contributors,
anyone working in any of the repos. Each stage ends in a **gate**: a list of checks that must pass, with the
evidence posted on the coordination board (commit hashes, log lines, numbers). A game's progress is recorded in
its repo's `docs/i64-status.md` (the scorecard; template at the end).

Canonical copy: this file in `n64net` (public). Other repos link here; don't fork the text, change it here.
Details behind each rule: i64 `docs/porting-a-game.md`, `docs/game-memory-arena.md`,
`docs/module-memory-checklist.md` (private repo).

---

## Ground rules (every stage)

1. **No game files, ever, in git or uploads:** no ROMs, no ROM-derived output (recompiled C, extracted assets,
   `.o2r`/`.otr`, converted data, screenshots of gameplay), no built apps/dylibs. `.gitignore` them on day one.
   Everyone uses their own legally dumped ROM; ROMs are never shared.
   Built i64 apps reach friends only privately, as releases of a private repo the owner controls: never
   committed, never on a public link. Contributors don't distribute builds.
2. **No emulators.** Native ports only: an existing decomp/port when one exists, else our own static
   recompilation (N64Recomp + N64ModernRuntime + RT64).
3. **Respect upstream:** keep licences and credits; don't copy code without a licence (reference only);
   keep our changes separable so fixes can go back upstream.
4. **Identity:** commit with your GitHub account's noreply address; no real names, emails, IPs, server
   addresses or passwords anywhere.
5. **Shared code changes by proposal** (i64, n64net, runtime/renderer patches, libultraship forks, the module
   ABI). A game's own repo is its owner's to change.
6. **Report each gate** on the board with evidence. Claim a game before starting; one owner per game.
   The coordination board is where claims live: read it before claiming.
7. **Plan for 4 players from the start** (see "Players and devices" below): a game that hard-codes
   "host = port 1, guest = port 2" has to be reworked later.
8. **Stand-alone is not embedded.** A game that runs well on its own can still be silent, leak or crash inside
   i64: it must not open its own audio device or window, call `exit`/`abort`/`atexit`/`SDL_Quit`, or change
   anything process-wide. Build the embedded seams (Stage 3b) early, not at Stage 6.

---

## Stage 0: Intake

- Identify the exact ROM (title, region, revision, SHA-1) and the version the route supports.
- Research routes: existing decomp, native port, recomp project; licences; known iOS/Apple issues. Choose:
  **A** existing native port, **B** decomp + our port layer, **C** our own static recompilation.
- Create the repo (private unless the owner decides otherwise), `.gitignore`, `docs/notes.md`,
  `docs/i64-status.md`.

**Gate 0:** route proposed on the board and approved; ROM SHA-1 recorded; repo exists with the ignore list.

## Stage 1: Code map (route C; for A/B: read the port's architecture instead)

- Boot and memory layout: entry, segments (ROM range -> vram, bss), overlays (all of them, with the routine
  that loads them), file systems/compression, RSP microcodes, TLB use.
- splat config splitting code from data; library functions named (n64sym signatures).

**Gate 1:** `docs/notes.md` lists every code region with ROM range, vram and loader; the function count;
named library calls (at least every `os*` the runtime replaces).

## Stage 2: Builds

- Route C: N64Recomp over every code region (overlays as exclusive sections), RSP microcodes via RSPRecomp or
  a known HLE; hand-written exception/cache/TLB pieces ignored and replaced by the runtime.
  - An existing recomp project for the game (e.g. Wave Race 64, Bomberman 64, Donkey Kong 64): take only its
    recompiler config, symbols and game patches, and put them on **our** runtime stack (next stage); its own
    runtime, menus and launcher aren't i64-compatible.
  - Pitfalls seen: splat's `textbin` segments (RSP microcode images) come out typed as functions, so the
    recompiler tries to translate them (ignore them, or check how the project handles them); a script writing to
    `/tmp` fails silently in sandboxed shells (use `$TMPDIR`); always build optimized (an unoptimized recompiled
    audio microcode runs late enough to corrupt the game's double-buffered command lists).
- Route A/B: builds from a clean clone on macOS (and the contributor's platform).

**Gate 2:** clean build from a fresh clone + ROM following the README; 0 recompiler errors.

## Stage 3: Boots and plays (Mac, stand-alone)

- Host from the template: route C on the shared runtime stack = **`WaveRace64/port`** (newest, smallest:
  `lib/i64/N64ModernRuntime` + `lib/i64/rt64` at Mario Party 2's pins with `port/patches`, `lib/n64net`; the
  game's own glue in one file) or `MarioTennis/port` (rollback, host save, TLB, overlays: `docs/handoff.md`);
  LUS ports: SpaghettiKart/Starship. Window, input, audio, renderer. Overlay registration on the game's load
  routine. Busy-wait loops yield.
- RT64 presentation mode **Console** (not PresentEarly). Audio: report the host queue minus ~2 VIs to
  `osAiGetLength` (or the game makes too little sound).
- **Sound needs evidence**, not "seems fine": a 30 s recording of real play (`<GAME>_AUDIO_DUMP`) with its
  RMS/peak level logged, and a person listening.
- On machines with unusual audio devices (virtual/aggregate devices), test runs may hang in SDL's audio init:
  use `SDL_AUDIODRIVER=dummy` for automated runs.

### Stage 3b: Embedded seams (all routes; build them now)

Each one small and documented with file:line; a `<GAME>_EMBEDDED`-style build flag where the port has its own
`main`:
- **Frame yield:** the one place a frame is presented, so the main loop can run as a coroutine/thread step.
- **Window/surface adoption:** draw into the window/layer handed in; never create or destroy a window.
- **Audio sink:** `push(stereo s16, frames, rate)` + `queued_frames()`; no audio device of its own.
- **Pads:** the one per-tick pad read, plus a presence override (which ports exist is session data).
- **Save:** load from a memory image, plus a "don't write" switch (the guest never writes).
- **Memory:** game memory from a host-supplied block.
- **Exit:** "end the match with this message" instead of `exit()`/`abort()`.
- **No process-wide state:** constructors that matter, `atexit`, `SDL_Quit`, signal handlers, `setenv`, hints,
  method swizzles: none, or each one undone at stop (re-entry snapshots of writable globals: SpaghettiKart
  `Reentry.cpp`, DKR `reentry_reset`).
- **Always-on state hash** (not behind a debug switch), and a list of which moments are rollback-safe and what
  Start/pause does (online, pause must not stop the simulation under rollback).
- **Portability list** for Mac/iOS: dependencies found by pkg-config (add a prebuilt-path override),
  desktop-GL-only code, macOS-only APIs under plain `__APPLE__` (use `TARGET_OS_OSX`), prebuilt libraries and
  whether an iOS arm64 build exists, every file read at run time besides the ROM.
A contributor without a Mac delivers these plus a tiny fake-host test program (start, N frames, stop, start
again; writes the sink's audio and the per-tick hash; run 1 == run 2).

**Gate 3:** reaches the title, menus and gameplay with a controller; a full match/level/round to its end;
sound without dropouts (`ran dry` ~0 after start-up); no flicker in captures of 30+ consecutive frames.

## Stage 4: Correctness pass

- Every mode reachable from the menus played once (single player, each multiplayer mode, options, saves).
- Saves load and persist; save files written crash-safely (temporary file + rename).

**Gate 4:** checklist of modes in `docs/i64-status.md` all ticked; known issues listed.

## Stage 5: Determinism

- `-ffp-contract=off` for all game code; never `-ffast-math`. No clock/OS randomness or frame-rate dependence
  in game logic (route C: the runtime's deterministic mode, virtual clock, coroutine threads). Rendering never
  feeds the simulation (aspect ratio, "visible" flags, window size).
- Input record/replay with a per-frame state hash.

**Gate 5:** the same recording replayed twice gives identical hashes on every frame; then Mac vs iPhone
identical (and x86 vs arm64 when a contributor can run it).

## Stage 6: i64 module

- Module ABI (`n64_game.h`): import from the ROM in i64, start on i64's window, frame, stop; pause/back to i64
  (game opens its own pause menu); exit messages; host-paced frames (60 Hz pictures).
- **Memory:** every allocation of 16 MB or more (including reserved address space) from i64's game memory
  arena, reserved in `start()` and handed out from there (`host_memory.cpp`); nothing kept across `stop()`.
- **Re-entry:** start/stop repeatedly in one process, alternating with other games.
- **Home-screen picture:** offer a frame to `host->offer_title_art` 30 s into a run, then each minute (test:
  `I64_TEST_PREVIEW_AT="400,900"` saves `i64-preview-<frame>.rgba` in the game's data folder: the easiest way to
  see what the game draws inside i64, since screenshots of Metal windows often come out black).
- Build pitfalls: link SDL only from i64's `deps-out` (pass `-DSDL2_INCLUDE_DIRS` / `-DSDL2_LIBRARIES` too, or
  a dependency links Homebrew's SDL as well and `dlopen` fails with "duplicate linked dylib"); for iOS, RT64's
  shader tools must be host builds (`-DRT64_HOST_TOOLS_DIR=<MarioParty2>/build-host-tools`).
- Test input: `I64_TEST_PAD=1` taps Start/A on a fixed rhythm that not every game's menus accept; give the game
  its own bot switch (`<GAME>_TEST_BOT=1`) that drives menus into a match.

**Gate 6 (numbers):** 3 runs + A-B-A with another game in one process, replay hashes identical to a cold run;
arena high-water recorded and within budget; heap left per run < 1 MB (`I64_TEST_HEAP_DIFF`); no `mmap`
fallback logged; "largest free low gap" during play > 300 MB on the reference iPhone.

## Stage 7: Online (lockstep)

- n64net on i64's channel (`n64net_input`); handshake with the host's save (guest plays on it in memory and
  never writes its own), rules/settings and seed; build id (both sides must run the same code, clear message
  otherwise).
- Sync checks every ~60 frames; two mismatches = stop the match cleanly with an exit message. Host-only test
  switch `<GAME>_NET_TEST_DESYNC=<frame>`.

**Gate 7:** i64 pair test (two copies on one Mac) in sync for a full match; desync test ends both cleanly with
the message; a peer quitting ends the other cleanly.

## Stage 8: Rollback

- Snapshots of the whole simulation (game RAM + runtime/port state), a ring of 12-16 slots; prediction +
  silent re-simulation; lockstep where re-simulation is unsafe (loading, scene changes); time sync; adaptive
  input delay; saves reach disk only for confirmed frames.

**Gate 8 (numbers):** SyncTest 0 mismatches over a full match; pair test at 60 ms + 30 ms jitter + 4% loss in
sync with rollbacks happening; snapshot save/load < 1 ms on a Mac; no stalls on a clean link.

## Players and devices (up to 4): design every stage for it

The owner's requirement: any split of up to 4 players across devices: 4 on one phone, 4 phones with one each,
or anything in between (2 on one phone plus 2 others). Design: i64 `docs/sessions-more-than-two.md`.
- **Who plays where comes from i64** (ABI v2, `n64_launch.players[4]`: for each player its machine, its local
  pad index, present or not; `num_machines`, `local_machine`). Never hard-code host = port 1, guest = port 2.
  Ports are assigned in machine order, each machine taking consecutive ports for its pads, so one machine's
  players are neighbours in the game's split screen.
- **Ports that exist are session data:** report the same controller presence on every machine (games ask at
  boot); no live hot-plugging mid-session.
- **Inputs:** `n64net_input` with `local_mask` (several local players per machine; one packet carries all of a
  machine's pads). Host-only decisions (drop-out choices) travel as input events on an agreed tick.
- **What a device shows:** the simulation always runs all players identically; a device draws only its own
  players' views:
  - 1 local player: full screen. 2-4: a layout of their views (the game's own split when all are local).
  - Two ways to do it, pick by whether viewport geometry feeds the simulation (aim rays, culling, "visible"
    flags):
    - **The game draws only the local viewports** in a layout (Starship: `SfNet_ViewRect`, the HUD moved into
      each view; DKR: upstream's roster with one viewport per local seat). Restore any state drawing leaves
      behind (Starship sets `gPlayerNum` back).
    - **The game draws its normal split everywhere and the renderer crops** to the box around the local
      viewports (GoldenEye: `genetFullViewRect`): needed when viewports feed aiming.
  - HUD, screen flashes and sounds follow each local player (sounds: pan per local view).
  - A test switch forcing the local list (`SF64_TEST_VIEWS=0,1`, `GE_NET_FULLVIEW=1,2`) so 1-4 views can be
    checked in a 2-copy pair test; gate: per-tick hashes identical with each view count (drawing never touches
    the simulation), plus a screenshot per layout.
- **Drop-outs** (later stage, plan for it): the host chooses CPU takeover / remove the player / idle / end; the
  choice is applied at an agreed tick and is part of the snapshot. Host migration and joining mid-game (catch up
  by fast-forwarding the input log) build on the same hooks.

## Stage 9: Multiplayer features

- Every multiplayer mode works online; the host's choices (rules, teams, handicaps, CPU players and level,
  gameplay options) reach every player (session data); gameplay-changing options are part of the match
  config or forced off online.
- Up to 4 players across up to 4 devices (see "Players and devices"); until i64's 4-device sessions land:
  2 devices with up to 2 controllers each, and the per-device views already working behind the test switch.
- Drop-out handling per the session design (the host decides: CPU takes over, remove the player where the
  game allows, or end the match); voice chat works alongside.

**Gate 9:** each multiplayer mode played online to its end with the host's settings visible on both sides;
1, 2, 3 and 4 local players per device shown correctly (pair test + screenshots, hashes identical); a 4-player
session (4 devices once available, else 2 x 2); a mid-match drop handled as designed.

## Stage 10: iPhone

- iOS module build (no JIT, no code patching, 16 KB pages, arm64), embedded in i64; landscape; controllers.

**Gate 10 (numbers):** on the reference iPhone: 60 pictures/s with the game's logic at its own rate, no audio
dropouts, memory within the arena budget; **two phones online across a full match with one of them on
cellular** (the other on home Wi-Fi, through the room server: real internet latency, not a simulation); a
Wi-Fi <-> cellular switch mid-match survives.

## Hand-off

- Push source only. Post on the board: repo, commit, build commands (Mac module, iOS module), test results with
  numbers, known issues, what was not verified.
- The iOS module (`lib<game>_i64.dylib` + any `.resources/`) goes to the owner's agents through the owner's
  private build folder; they embed it, sign i64 and test on the phones. Nobody else installs on the owner's
  phones or touches the room server.

---

## Scorecard template (`docs/i64-status.md` in each game repo)

```
# <Game> in i64: status
ROM: <title, region, revision> SHA-1 <...>   Route: <A/B/C + upstream>   Owner: <who>
Process: https://github.com/philosoraptor64/n64net/blob/main/docs/new-game-process.md

| Gate | Status | Evidence (commit, numbers, date) |
|---|---|---|
| 0 Intake | | |
| 1 Code map | | |
| 2 Builds | | |
| 3 Boots and plays | | |
| 4 Correctness | | |
| 5 Determinism | | |
| 6 i64 module | | |
| 7 Online (lockstep) | | |
| 8 Rollback | | |
| 9 Multiplayer features (incl. 1-4 local views, drop-outs) | | |
| 10 iPhone | | |

Status: done / partial / not started / n/a (single-player games skip 7-9).
Known issues:
```

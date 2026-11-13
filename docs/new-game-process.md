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
2. **No emulators.** Native ports only: an existing decomp/port when one exists, else our own static
   recompilation (N64Recomp + N64ModernRuntime + RT64).
3. **Respect upstream:** keep licences and credits; don't copy code without a licence (reference only);
   keep our changes separable so fixes can go back upstream.
4. **Identity:** commit with your GitHub account's noreply address; no real names, emails, IPs, server
   addresses or passwords anywhere.
5. **Shared code changes by proposal** (i64, n64net, runtime/renderer patches, libultraship forks, the module
   ABI). A game's own repo is its owner's to change.
6. **Report each gate** on the board with evidence. Claim a game before starting; one owner per game.

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
- Route A/B: builds from a clean clone on macOS (and the contributor's platform).

**Gate 2:** clean build from a fresh clone + ROM following the README; 0 recompiler errors.

## Stage 3: Boots and plays (Mac, stand-alone)

- Host from the template (route C: `MarioTennis/port`; LUS ports: SpaghettiKart/Starship): window, input,
  audio, renderer. Overlay registration on the game's load routine. Busy-wait loops yield.
- RT64 presentation mode **Console** (not PresentEarly). Audio: report the host queue minus ~2 VIs to
  `osAiGetLength` (or the game makes too little sound).

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

## Stage 9: Multiplayer features

- Every multiplayer mode works online; the host's choices (rules, teams, handicaps, CPU players and level,
  gameplay options) reach every player (session data); gameplay-changing options are part of the match
  config or forced off online.
- Up to 4 players (today: 2 devices with up to 2 controllers each; more devices when i64 sessions support it).
- Drop-out handling per the session design (the host decides: CPU takes over, remove the player where the
  game allows, or end the match); voice chat works alongside.

**Gate 9:** each multiplayer mode played online to its end with the host's settings visible on both sides;
a mid-match drop handled as designed.

## Stage 10: iPhone

- iOS module build (no JIT, no code patching, 16 KB pages, arm64), embedded in i64; landscape; controllers.

**Gate 10 (numbers):** on the reference iPhone: 60 pictures/s with the game's logic at its own rate, no audio
dropouts, memory within the arena budget; two phones online across a full match; a Wi-Fi <-> cellular switch
mid-match survives.

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
| 9 Multiplayer features | | |
| 10 iPhone | | |

Status: done / partial / not started / n/a (single-player games skip 7-9).
Known issues:
```

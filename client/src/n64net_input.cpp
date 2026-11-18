/* n64net input exchange (see n64net_input.h).
 *
 * Wire format (version 3), all integers big endian:
 *   input:  u32 magic 'N64I' | u8 version | u8 type=0 (0x80 = extended frames, 0x40 = event bytes) |
 *           u8 player | u8 count | u32 session | u32 ack[4] (next tick this sender needs from each
 *           player) | u32 first_tick |
 *           count x (u16 buttons, s8 x, s8 y[, s8 rx, s8 ry, u16 buttons_hi if type & 0x80]
 *                    [, u8 event length, event bytes if type & 0x40]) |
 *           u32 stamp (sender clock, usecs) | u32 echo[4] (newest stamp received from each
 *           player's machine) | u32 hold[4] (usecs since that stamp arrived) |
 *           s32 advantage | u8 wished delay (auto delay; 0 = none) |
 *           [u8 lag flags (bit 0: this machine is the laggy one) | u8 its input delay now] | u32 checksum
 *           (the two lag bytes were added later: older builds ignore them, and a packet without them
 *           counts as "not laggy, delay unknown")
 *   A machine with several local players (local_mask) sends one input stream per player; events
 *   ride in its first local player's stream. Up to 4 machines: every packet goes to every peer.
 *   sync:   magic | version | type=1 | player | count | session |
 *           count x (u32 tick, u8 kind, u32 hash) | checksum
 *   away:   magic | version | type=2 (away) or 3 (back) | player | 0 | session | checksum
 *   retire: magic | version | type=4 | sender | 0 | session | u8 player | u32 last | u32 apply |
 *           u8 action length, action | u32 tail first | u8 tail count, tail pads (8 bytes) | checksum
 *   log:    magic | version | type=5 (0x80: 8-byte pads) | sender | count (ticks) | session |
 *           u8 to (the joiner's first player) | u32 first tick | u8 players |
 *           players x (u8 state: 1 retired, 2 rejoined | u32 last | u32 apply | u32 rejoin |
 *                      u8 action length, action) |
 *           count x players x pad (4 or 8 bytes) | u8 events, events x (u16 tick offset, u8 player,
 *           u8 length, bytes) | checksum                                   (host -> joiner: the input log)
 *   have:   magic | version | type=6 | sender | 0 | session | u32 log ticks received | checksum (joiner -> host)
 *   rejoin: magic | version | type=7 | sender | 0 | session | u8 player mask | u32 live tick | checksum
 * Inputs are sent oldest-unacknowledged first, up to 16 per packet, so a
 * lost packet is covered by the next one and a long gap still drains. */
#include "n64net_input.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace {

constexpr uint8_t kVersion = 3;   // 3: several players per machine, events, per-peer echo
constexpr uint32_t kRttRing = 90;  // round trips kept (auto delay uses their 90th percentile)
constexpr uint8_t kTypeInput = 0, kTypeSync = 1, kTypeAway = 2, kTypeBack = 3, kTypeRetire = 4, kTypeLog = 5,
                  kTypeHave = 6, kTypeRejoin = 7;
// Joining a running match: the host streams the log to the joiner, up to this many packets per send; when the
// joiner has all but kJoinNear ticks of it, the players go live kJoinMargin ticks after the host's newest tick.
constexpr uint32_t kLogPacketsPerSend = 16, kJoinNear = 60, kJoinMargin = 45;
constexpr uint32_t kRejoinRepeats = 200;
constexpr uint32_t kRetireTail = 64;      // the leaver's last inputs carried in a RETIRE packet
constexpr uint32_t kRetireRepeats = 200;  // RETIRE packets sent with the next inputs (UDP: no ack needed)
// Input packets whose frames carry the extended fields (right stick, buttons_hi). Only used when a
// frame in the packet has one of them non-zero, so other games send exactly the old format.
constexpr uint8_t kTypeFlagRightStick = 0x80;
// Input packets whose frames carry an event length byte (+ bytes): only when one of them has an event.
constexpr uint8_t kTypeFlagEvents = 0x40;
constexpr uint32_t kHeaderBytes = 32, kTrailerBytes = 4 + 16 + 16 + 4 + 1 + 2 + 4;
// One laggy machine (todo item 11): see LagTarget(). Its delay may grow to this much time, and it carries up to
// this many ticks of rollback for the others (its time sync lets it run that far "ahead").
// In lockstep (no rollback to hide anything) the delay alone must cover the round trip: a higher limit.
constexpr uint32_t kLaggyMaxDelayMs = 250, kLaggyMaxDelayLockstepMs = 450, kLaggyBudget = 4;
// After a machine becomes laggy it waits this long before trusting its peers' laggy flags (they arrive a
// one-way trip later; a peer on the same slow link is about to say it is laggy too).
constexpr uint64_t kLaggySettleUsecs = 1500000;
constexpr uint32_t kLaggyMaxExcess = 6;
constexpr uint8_t kLagFlagLaggy = 1;
constexpr uint32_t kRing = 1024;           // ticks of history kept per player
// Frames per input packet, from the oldest tick the peer still needs (its ack) up to the newest.
// With rollback a sender runs window + delay ticks ahead of what it has confirmed, and acks arrive
// a round trip late; past this many frames the newest inputs wait for the acks to catch up, which
// stalls both sides in bursts. 64 covers window 8 + a laggy machine's delay of up to 250 ms + a
// ~400 ms round trip.
constexpr uint32_t kMaxFramesPerPacket = 64;
constexpr uint32_t kSyncRing = 64;
constexpr uint32_t kSyncPerPacket = 4;
constexpr uint32_t kNoTick = UINT32_MAX;

struct Slot {
    uint32_t tick = kNoTick;
    n64net_pad pad = {0, 0, 0};
    uint8_t ev_len = 0;
    uint8_t ev[N64NET_INPUT_MAX_EVENT] = {};
};
struct Used {
    uint32_t tick = kNoTick;
    n64net_pad pad = {0, 0, 0};
    bool predicted = false;
};
struct LogEvent {
    uint32_t tick;
    uint8_t player, len;
    uint8_t ev[N64NET_INPUT_MAX_EVENT];
};
struct SyncEntry {
    uint32_t tick = kNoTick;
    uint8_t kind = 0;
    uint32_t hash = 0;
};

bool PadEq(const n64net_pad &a, const n64net_pad &b) {
    return a.buttons == b.buttons && a.stick_x == b.stick_x && a.stick_y == b.stick_y && a.rstick_x == b.rstick_x &&
           a.rstick_y == b.rstick_y && a.buttons_hi == b.buttons_hi;
}

uint64_t SteadyUsecs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

uint32_t Fnv(const uint8_t *data, uint32_t size) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < size; i++) {
        h = (h ^ data[i]) * 16777619u;
    }
    return h;
}

struct Writer {
    uint8_t *p;
    void U8(uint8_t v) { *p++ = v; }
    void U16(uint16_t v) { U8((uint8_t)(v >> 8)); U8((uint8_t)v); }
    void U32(uint32_t v) { U16((uint16_t)(v >> 16)); U16((uint16_t)v); }
};
struct Reader {
    const uint8_t *p;
    const uint8_t *end;
    bool ok = true;
    uint8_t U8() {
        if (p >= end) { ok = false; return 0; }
        return *p++;
    }
    uint16_t U16() { uint16_t hi = U8(); return (uint16_t)((hi << 8) | U8()); }
    uint32_t U32() { uint32_t hi = U16(); return (hi << 16) | U16(); }
};

} // namespace

struct n64net_input {
    n64net_input_config cfg;
    uint32_t remote_mask = 0;

    // Local players' scheduled inputs (tick = sample tick + delay); every local player's ring
    // is filled up to the same tick.
    uint32_t local_mask = 0;
    std::vector<Slot> local[N64NET_INPUT_MAX_PLAYERS];
    bool local_started = false;
    uint32_t local_last = 0;        // newest scheduled tick
    uint32_t last_sample_tick = 0;  // newest tick passed to set_local
    n64net_pad last_pad[N64NET_INPUT_MAX_PLAYERS] = {};
    uint32_t delay;
    // An event for the next scheduled tick (n64net_input_set_event).
    bool event_pending = false;
    uint8_t event_len = 0;
    uint8_t event[N64NET_INPUT_MAX_EVENT] = {};

    // Remote players: confirmed inputs, what the simulation used, progress.
    std::vector<Slot> remote[N64NET_INPUT_MAX_PLAYERS];
    std::vector<Used> used[N64NET_INPUT_MAX_PLAYERS];
    uint32_t next_needed[N64NET_INPUT_MAX_PLAYERS] = {}; // all ticks below are confirmed
    uint32_t highest[N64NET_INPUT_MAX_PLAYERS] = {};     // newest tick received (+1; 0 = none)
    // ack_from[s][p]: the next tick remote player s's machine needs from our local player p.
    uint32_t ack_from[N64NET_INPUT_MAX_PLAYERS][N64NET_INPUT_MAX_PLAYERS] = {};

    bool rollback_pending = false;
    uint32_t rollback_tick = 0;
    void RequestRollback(uint32_t tick) {
        if (!rollback_pending || tick < rollback_tick) rollback_tick = tick;
        rollback_pending = true;
    }
    uint32_t highest_got = 0;   // newest tick (+1) a remote player's input was asked for (simulated)

    // Drop-outs (n64net_input_retire): a retired player's inputs are real up to retire_last, neutral and
    // confirmed after it; the game applies retire_action at retire_apply. It stops being "remote" (nothing
    // waits for it), so its stragglers are ignored.
    bool retired[N64NET_INPUT_MAX_PLAYERS] = {};
    // A player nobody played so far (a CPU from the start, an empty slot) that a joiner takes over
    // (n64net_input_add): kept as "retired since the start" inside (neutral until the joiner's tick, the
    // admit machinery as is), but n64net_input_retired() doesn't report it (nobody left).
    bool open_slot[N64NET_INPUT_MAX_PLAYERS] = {};
    void MarkOpenSlot(int p) {
        retired[p] = true;
        open_slot[p] = true;
        retire_last[p] = UINT32_MAX;            // no real inputs at all (the slots never hold any)
        retire_apply[p] = cfg.start_tick;
        retire_action_len[p] = 0;
    }
    uint32_t retire_last[N64NET_INPUT_MAX_PLAYERS] = {}, retire_apply[N64NET_INPUT_MAX_PLAYERS] = {};
    uint8_t retire_action[N64NET_INPUT_MAX_PLAYERS][N64NET_INPUT_MAX_EVENT] = {};
    uint8_t retire_action_len[N64NET_INPUT_MAX_PLAYERS] = {};
    uint32_t retire_repeats[N64NET_INPUT_MAX_PLAYERS] = {};   // announcements still to send (the announcer)
    void ApplyRetire(int p, uint32_t last, uint32_t apply, const uint8_t *action, uint32_t action_len,
                     uint32_t tail_first, uint32_t tail_count, const n64net_pad *tail);
    void SendRetire(int p);
    // Joining a running match. join_from[p]: the tick a joiner's machine took player p over (kNoTick: not
    // rejoined); before it, p follows the retirement rule.
    uint32_t join_from[N64NET_INPUT_MAX_PLAYERS];
    void ApplyRejoin(uint32_t mask, uint32_t tick);
    void SendRejoin();
    uint32_t rejoin_mask = 0, rejoin_tick = kNoTick, rejoin_repeats = 0;   // the announcer's last announcement
    // The log: every player's confirmed inputs (and events) from start_tick to log_end - 1.
    std::vector<n64net_pad> log_pad[N64NET_INPUT_MAX_PLAYERS];
    std::vector<LogEvent> log_events;   // by tick
    uint32_t log_end = 0;
    bool log_ext = false;   // a logged pad uses the extended fields
    void ExtendLog();
    bool LogPad(int p, uint32_t tick, n64net_pad *out) const {
        if (tick < cfg.start_tick || tick >= log_end) return false;
        *out = log_pad[p][tick - cfg.start_tick];
        return true;
    }
    const LogEvent *LogEventAt(int p, uint32_t tick) const {
        auto it = std::lower_bound(log_events.begin(), log_events.end(), tick,
                                   [](const LogEvent &e, uint32_t t) { return e.tick < t; });
        for (; it != log_events.end() && it->tick == tick; ++it) if (it->player == p) return &*it;
        return nullptr;
    }
    // the host serving a joiner
    uint32_t admit_mask = 0;        // its players (0: none)
    uint32_t admit_have = 0;        // log ticks it has (from start_tick: the next it needs)
    uint32_t admit_sent = 0;        // next log tick to send
    uint64_t admit_progress_at = 0; // when admit_have last grew
    bool admit_heard = false;       // its game runs (a "have" arrived): until then one piece now and then
    void ServeJoiner();
    void SendLog(uint32_t first, uint32_t *next);
    // the joiner
    uint32_t live_from = kNoTick;   // its players' first live tick (kNoTick: not known yet)
    uint32_t local_base = 0;        // the first tick of its local streams (start_tick, or live_from)
    // pieces of the log that arrived ahead of a missing one (first tick -> pads, events)
    struct Piece { std::vector<n64net_pad> pads; std::vector<LogEvent> events; bool ext; };
    std::map<uint32_t, Piece> log_ahead;
    void AppendLog(uint32_t first, uint32_t count, const std::vector<n64net_pad> &pads, const std::vector<LogEvent> &evs,
                   bool ext);
    uint64_t have_sent_at = 0;
    uint32_t have_sent = 0;
    void SendHave(bool force);
    void HandleLog(Reader &r, uint8_t count, bool ext);
    bool CatchingUp() const { return cfg.joining && (live_from == kNoTick || log_end < live_from); }
    bool FromLog(uint32_t tick) const { return cfg.joining && (live_from == kNoTick || tick < live_from); }
    int resimulating = 0;
    uint32_t driver_tick = 0;  // the tick n64net_rollback_frame's current callback is about
    std::vector<uint32_t> slot_tick;

    // Timing.
    uint64_t last_send_usecs = 0;
    uint64_t last_recv_usecs = 0;
    // per remote player (its machine): newest stamp, when it arrived, its advantage and delay wish
    uint32_t peer_stamp[N64NET_INPUT_MAX_PLAYERS] = {};
    uint64_t peer_stamp_at[N64NET_INPUT_MAX_PLAYERS] = {};
    bool have_peer_stamp[N64NET_INPUT_MAX_PLAYERS] = {};
    int32_t peer_advantage[N64NET_INPUT_MAX_PLAYERS] = {};
    bool peer_advantage_valid[N64NET_INPUT_MAX_PLAYERS] = {};
    uint32_t peer_wish_of[N64NET_INPUT_MAX_PLAYERS] = {};
    bool peer_laggy[N64NET_INPUT_MAX_PLAYERS] = {};      // that machine says it is the laggy one
    int32_t peer_delay_of[N64NET_INPUT_MAX_PLAYERS] = {-1, -1, -1, -1};  // its input delay (-1: unknown)
    int32_t remote_advantage = 0;      // the most-behind peer's advantage (time sync)
    bool remote_advantage_valid = false;
    int32_t ahead16 = 0;
    uint32_t timesync_cooldown = 0;

    // Sync checks.
    SyncEntry local_sync[kSyncRing];
    // per sending machine (indexed by its first player), in arrival order: with 3-4 machines every
    // peer's hash for a tick is compared with ours, not only the first to arrive
    SyncEntry remote_sync[N64NET_INPUT_MAX_PLAYERS][kSyncRing];
    uint32_t local_sync_count = 0;
    uint32_t remote_sync_count[N64NET_INPUT_MAX_PLAYERS] = {};
    bool desynced = false;
    uint32_t desync_tick = 0;
    uint8_t desync_kind = 0;
    uint32_t sync_matched = 0;

    bool peer_away = false;

    n64net_input_stats stats = {};
    uint32_t rtt_samples[kRttRing] = {};
    uint32_t rtt_count = 0;
    uint32_t rtt_of[N64NET_INPUT_MAX_PLAYERS][kRttRing] = {};   // per remote player (its machine)
    uint32_t rtt_count_of[N64NET_INPUT_MAX_PLAYERS] = {};
    bool laggy = false;          // this machine is the laggy one (hysteresis in UpdateAutoDelay)
    uint64_t laggy_since = 0;
    uint32_t lag_excess = 0;     // ticks of rollback this laggy machine carries for the others (time sync)

    // Auto delay (n64net_input_set_auto_delay).
    uint32_t auto_min = 0, auto_max = 0;  // 0/0 = off
    uint32_t peer_wish = 0;               // the largest wish of any peer
    uint64_t calm_since = 0;              // when the wish last stopped being above the delay

    uint64_t Now() const { return cfg.now_usecs ? cfg.now_usecs() : SteadyUsecs(); }
    void Log(const char *fmt, ...) const {
        if (!cfg.log) return;
        char buf[256];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        cfg.log("%s", buf);
    }
    bool IsRemote(int p) const { return p >= 0 && p < cfg.num_players && (remote_mask & (1u << p)); }
    bool IsLocal(int p) const { return p >= 0 && p < cfg.num_players && (local_mask & (1u << p)); }
    // The oldest tick any remote machine still needs from local player p.
    uint32_t FirstNeededFrom(int p) const {
        uint32_t m = kNoTick;
        for (int s = 0; s < cfg.num_players; s++) {
            if (IsRemote(s)) m = std::min(m, ack_from[s][p]);
        }
        return m == kNoTick ? local_base : m;
    }
    void Send(const uint8_t *data, uint32_t size) {
        if (cfg.send) {
            cfg.send(cfg.user, data, size);
            stats.packets_sent++;
        }
    }
    uint32_t MinNextNeeded() const {
        uint32_t m = kNoTick;
        for (int p = 0; p < cfg.num_players; p++) {
            if (IsRemote(p)) m = std::min(m, next_needed[p]);
        }
        return m;
    }
    int32_t LocalAdvantage() const {
        uint32_t newest = kNoTick;
        for (int p = 0; p < cfg.num_players; p++) {
            if (IsRemote(p)) newest = std::min(newest, highest[p]);
        }
        if (newest == kNoTick) return 0;
        // highest[] is newest received tick + 1 (0 = none): advantage = tick - newest.
        return (int32_t)last_sample_tick - ((int32_t)newest - 1);
    }

    void StoreLocal(int p, uint32_t tick, n64net_pad pad) {
        Slot &s = local[p][tick % kRing];
        s.tick = tick;
        s.pad = pad;
        s.ev_len = 0;
    }
    void StoreAllLocal(uint32_t tick, const n64net_pad *pads) {  // pads == nullptr: neutral
        for (int p = 0; p < cfg.num_players; p++) {
            if (IsLocal(p)) StoreLocal(p, tick, pads ? pads[p] : n64net_pad{0, 0, 0});
        }
    }
    const Slot *LocalSlot(int p, uint32_t tick) const {
        const Slot &s = local[p][tick % kRing];
        return s.tick == tick ? &s : nullptr;
    }
    bool LocalAt(int p, uint32_t tick, n64net_pad *out) const {
        const Slot *s = LocalSlot(p, tick);
        if (!s) return false;
        *out = s->pad;
        return true;
    }
    void SetLocalPads(uint32_t tick, const n64net_pad *pads);

    // The delay this side wants: enough ticks to cover the one-way time of
    // the 90th percentile of recent round trips, less one tick (each side
    // polls once per tick), within [auto_min, auto_max]. 0 when off.
    // Ticks of delay that cover the one-way time from a set of round trips: the one-way time of their
    // 90th percentile (half the round trip, less the frame of polling measured in it) plus 8 ms of
    // margin, rounded up, plus one tick for the phase (the two frame clocks are up to a tick apart).
    // (Proposed by the Mario Kart port.) 0 if there are fewer than 8 samples.
    uint32_t OneWayTicks(const uint32_t *samples, uint32_t count) const {
        const uint32_t n = std::min(count, kRttRing);
        if (n < 8) return 0;
        uint32_t sorted[kRttRing];
        std::copy(samples, samples + n, sorted);
        std::sort(sorted, sorted + n);
        const uint32_t p90 = sorted[(n * 9) / 10 < n ? (n * 9) / 10 : n - 1];
        const uint32_t tick_usecs = 1000000u / cfg.ticks_per_second;
        const uint32_t one_way = (p90 > tick_usecs ? p90 - tick_usecs : 0) / 2 + 8000;
        return (one_way + tick_usecs - 1) / tick_usecs + 1;
    }
    uint32_t OneWayTo(int p) const { return OneWayTicks(rtt_of[p], rtt_count_of[p]); }
    // The machine's best link: the fewest ticks to any remote machine (0 = not measured yet). A machine whose
    // best link needs more delay than the normal limit is "laggy": every one of its links is bad, so it, not
    // its peers, should carry the lag.
    uint32_t BestOneWay() const {
        uint32_t best = 0;
        for (int p = 0; p < cfg.num_players; p++) {
            if (!IsRemote(p)) continue;
            const uint32_t w = OneWayTo(p);
            if (w && (best == 0 || w < best)) best = w;
        }
        return best;
    }
    uint32_t LaggyMaxDelay() const {
        const uint32_t ms = cfg.rollback_window ? kLaggyMaxDelayMs : kLaggyMaxDelayLockstepMs;
        return std::max(auto_max, (ms * cfg.ticks_per_second + 999) / 1000);
    }
    // The delay this side wants against its normal peers (the laggy ones carry their own lag), within
    // [auto_min, auto_max]. 0 when off.
    uint32_t Wish(bool with_laggy = false) const {
        if (auto_max == 0) return 0;
        uint32_t w = 0;
        bool any = false;
        for (int p = 0; p < cfg.num_players; p++) {
            if (!IsRemote(p) || (peer_laggy[p] && !with_laggy)) continue;
            const uint32_t t = OneWayTo(p);
            if (t) any = true;
            w = std::max(w, t);
        }
        if (!any && rtt_count < 8) return std::max(auto_min, delay);
        return std::min(std::max(w, auto_min), auto_max);
    }
    // The laggy machine's delay: enough that each link to a normal peer works without anyone waiting, given
    // that peer's own delay (the two delays together cover the round trip), less the rollback this machine
    // carries itself; against another laggy machine each side covers its half. Within the laggy limit; what
    // still isn't covered becomes extra rollback here (lag_excess, time sync), never a stall elsewhere.
    // With no normal peer at all (two machines on one slow link, or everyone far apart) nobody is the odd one
    // out: they share the lag as before (the usual wish, within max_delay).
    uint32_t SharedTarget() const {
        uint32_t peers = 0;
        for (int p = 0; p < cfg.num_players; p++) {
            if (IsRemote(p)) peers = std::max(peers, peer_wish_of[p]);
        }
        return std::min(std::max(Wish(true), peers), auto_max);
    }
    uint32_t LagTarget(uint32_t *excess, bool settled) const {
        *excess = 0;
        bool any_normal = false;
        for (int p = 0; p < cfg.num_players; p++) {
            any_normal |= IsRemote(p) && !peer_laggy[p] && OneWayTo(p) != 0;
        }
        if (!settled || !any_normal) return SharedTarget();
        const uint32_t budget = std::min(kLaggyBudget, cfg.rollback_window / 2);
        uint32_t need_normal = 0, need_laggy = 0;
        for (int p = 0; p < cfg.num_players; p++) {
            if (!IsRemote(p)) continue;
            const uint32_t w = OneWayTo(p);
            if (!w) continue;
            if (peer_laggy[p]) {
                need_laggy = std::max(need_laggy, std::min(w, auto_max));
            } else {
                const int32_t theirs = peer_delay_of[p] >= 0 ? peer_delay_of[p] : (int32_t)w;
                need_normal = std::max<int32_t>(need_normal, (int32_t)(2 * w) - theirs);
            }
        }
        const uint32_t want_normal = need_normal > budget ? need_normal - budget : 0;
        const uint32_t target =
            std::min(std::max({ want_normal, need_laggy, Wish() }), LaggyMaxDelay());
        // the laggy machine runs ahead of its normal peers by everything their delays don't cover
        // (at most window - 3, so it stays clear of the window: past it, it would wait instead of rolling back;
        // and at most kLaggyMaxExcess, whatever the window: a game with a deep window for spikes must not roll
        // back that deep every frame)
        *excess = need_normal > target ? std::min({ need_normal - target, kLaggyMaxExcess,
                                                    cfg.rollback_window > 3 ? cfg.rollback_window - 3 : 0u }) : 0;
        return target;
    }
    void UpdateAutoDelay(uint64_t now);
    void SendInputs();
    void SendInputsFor(int p);
    void SendSync();
    void HandleInput(Reader &r, uint8_t player, uint8_t count, bool right_stick, bool events);
    void HandleSync(Reader &r, uint8_t player, uint8_t count);
    void CompareSync(const SyncEntry &a, const SyncEntry &b);
    static const SyncEntry *FindSync(const SyncEntry *ring, uint32_t count, uint32_t tick, uint8_t kind) {
        for (uint32_t j = 0; j < std::min(count, kSyncRing); j++) {
            if (ring[j].tick == tick && ring[j].kind == kind) return &ring[j];
        }
        return nullptr;
    }
};

void n64net_input::UpdateAutoDelay(uint64_t now) {
    if (auto_max == 0) return;
    // Laggy or not (with a tick of hysteresis so it doesn't flicker at the edge).
    const uint32_t best = BestOneWay();
    const bool was = laggy;
    if (best && best > auto_max + (laggy ? 0 : 1)) {
        if (!laggy) laggy_since = now;
        laggy = true;
    } else if (best && best < auto_max) {
        laggy = false;
    }
    if (laggy != was) Log("n64net input: %s (best link %u ticks one way)\n",
                          laggy ? "this machine's links are all slow: it takes the lag itself" : "links normal again",
                          best);
    uint32_t target;
    if (laggy) {
        target = LagTarget(&lag_excess, now - laggy_since >= kLaggySettleUsecs);
    } else {
        lag_excess = 0;
        // Normal machines harmonise with the larger wish of their normal peers, so their delays match.
        uint32_t peers = 0;
        for (int p = 0; p < cfg.num_players; p++) {
            if (IsRemote(p) && !peer_laggy[p]) peers = std::max(peers, peer_wish_of[p]);
        }
        target = std::min(std::max(Wish(), peers), auto_max);
    }
    if (target > delay) {
        delay = target;  // up at once
        calm_since = now;
        Log("n64net input: auto delay up to %u\n", delay);
    } else if (target < delay) {
        if (now - calm_since >= 8000000) {  // down one step after 8 calm seconds
            delay--;
            calm_since = now;
            Log("n64net input: auto delay down to %u\n", delay);
        }
    } else {
        calm_since = now;
    }
}

void n64net_input::SendInputs() {
    for (int p = 0; p < cfg.num_players; p++) {
        if (IsLocal(p)) SendInputsFor(p);
        if (retire_repeats[p]) {
            retire_repeats[p]--;
            SendRetire(p);
        }
    }
    if (rejoin_repeats) {
        rejoin_repeats--;
        SendRejoin();
    }
    ExtendLog();
    ServeJoiner();
    if (CatchingUp()) SendHave(false);
    last_send_usecs = Now();
}

// One local player's stream: its inputs from the oldest tick any peer still needs, as many as fit.
void n64net_input::SendInputsFor(int player) {
    uint8_t buf[N64NET_INPUT_MAX_PACKET];
    Writer w{buf};
    const uint32_t first = std::max(FirstNeededFrom(player), local_base);
    uint32_t avail = 0;
    if (local_started && first <= local_last) {
        avail = std::min(kMaxFramesPerPacket, local_last - first + 1);
    }
    bool right_stick = false, events = false;
    for (uint32_t i = 0; i < avail; i++) {
        const Slot *sl = LocalSlot(player, first + i);
        if (!sl) continue;
        right_stick |= sl->pad.rstick_x != 0 || sl->pad.rstick_y != 0 || sl->pad.buttons_hi != 0;
        events |= sl->ev_len != 0;
    }
    // frames that fit the packet (events make them bigger)
    uint32_t count = 0, bytes = kHeaderBytes + kTrailerBytes;
    for (; count < avail; count++) {
        const Slot *sl = LocalSlot(player, first + count);
        const uint32_t size = 4 + (right_stick ? 4 : 0) + (events ? 1 + (sl ? sl->ev_len : 0) : 0);
        if (bytes + size > N64NET_INPUT_MAX_PACKET) break;
        bytes += size;
    }
    w.U32(N64NET_INPUT_MAGIC);
    w.U8(kVersion);
    w.U8(kTypeInput | (right_stick ? kTypeFlagRightStick : 0) | (events ? kTypeFlagEvents : 0));
    w.U8((uint8_t)player);
    w.U8((uint8_t)count);
    w.U32(cfg.session_id);
    for (int p = 0; p < N64NET_INPUT_MAX_PLAYERS; p++) {
        w.U32(IsRemote(p) ? next_needed[p] : 0);
    }
    w.U32(first);
    for (uint32_t i = 0; i < count; i++) {
        const Slot *sl = LocalSlot(player, first + i);
        const n64net_pad pad = sl ? sl->pad : n64net_pad{0, 0, 0};
        w.U16(pad.buttons);
        w.U8((uint8_t)pad.stick_x);
        w.U8((uint8_t)pad.stick_y);
        if (right_stick) {
            w.U8((uint8_t)pad.rstick_x);
            w.U8((uint8_t)pad.rstick_y);
            w.U16(pad.buttons_hi);
        }
        if (events) {
            const uint8_t n = sl ? sl->ev_len : 0;
            w.U8(n);
            for (uint8_t k = 0; k < n; k++) w.U8(sl->ev[k]);
        }
    }
    const uint64_t now = Now();
    w.U32((uint32_t)now);
    for (int q = 0; q < N64NET_INPUT_MAX_PLAYERS; q++) w.U32(have_peer_stamp[q] ? peer_stamp[q] : 0);
    for (int q = 0; q < N64NET_INPUT_MAX_PLAYERS; q++) {
        w.U32(have_peer_stamp[q] ? (uint32_t)(now - peer_stamp_at[q]) : 0);
    }
    w.U32((uint32_t)LocalAdvantage());
    w.U8((uint8_t)std::min<uint32_t>(Wish(), 255));
    w.U8(laggy ? kLagFlagLaggy : 0);
    w.U8((uint8_t)std::min<uint32_t>(delay, 255));
    w.U32(Fnv(buf, (uint32_t)(w.p - buf)));
    Send(buf, (uint32_t)(w.p - buf));
}

void n64net_input::SendSync() {
    uint8_t buf[N64NET_INPUT_MAX_PACKET];
    Writer w{buf};
    const uint32_t n = std::min(local_sync_count, kSyncPerPacket);
    w.U32(N64NET_INPUT_MAGIC);
    w.U8(kVersion);
    w.U8(kTypeSync);
    w.U8((uint8_t)cfg.local_player);
    w.U8((uint8_t)n);
    w.U32(cfg.session_id);
    for (uint32_t i = 0; i < n; i++) {
        const SyncEntry &e = local_sync[(local_sync_count - 1 - i) % kSyncRing];
        w.U32(e.tick);
        w.U8(e.kind);
        w.U32(e.hash);
    }
    w.U32(Fnv(buf, (uint32_t)(w.p - buf)));
    Send(buf, (uint32_t)(w.p - buf));
}

void n64net_input::CompareSync(const SyncEntry &a, const SyncEntry &b) {
    if (a.tick != b.tick || a.kind != b.kind || a.tick == kNoTick) return;
    if (a.hash == b.hash) {
        sync_matched++;
        return;
    }
    if (!desynced) {
        desynced = true;
        desync_tick = a.tick;
        desync_kind = a.kind;
        Log("n64net input: DESYNC at tick %u (kind %u): 0x%08X here, 0x%08X on the peer\n", a.tick, a.kind, a.hash,
            b.hash);
    }
}

void n64net_input::HandleInput(Reader &r, uint8_t player, uint8_t count, bool right_stick, bool events) {
    uint32_t acks[N64NET_INPUT_MAX_PLAYERS];
    for (uint32_t &a : acks) a = r.U32();
    const uint32_t first = r.U32();
    if (count > kMaxFramesPerPacket) {
        stats.packets_dropped++;
        return;
    }
    Slot frames[kMaxFramesPerPacket];
    for (uint32_t i = 0; i < count; i++) {
        n64net_pad &pad = frames[i].pad;
        pad.buttons = r.U16();
        pad.stick_x = (int8_t)r.U8();
        pad.stick_y = (int8_t)r.U8();
        pad.rstick_x = right_stick ? (int8_t)r.U8() : 0;
        pad.rstick_y = right_stick ? (int8_t)r.U8() : 0;
        pad.buttons_hi = right_stick ? r.U16() : 0;
        frames[i].ev_len = 0;
        if (events) {
            const uint8_t n = r.U8();
            if (n > N64NET_INPUT_MAX_EVENT) { r.ok = false; break; }
            frames[i].ev_len = n;
            for (uint8_t k = 0; k < n; k++) frames[i].ev[k] = r.U8();
        }
    }
    const uint32_t stamp = r.U32();
    uint32_t echoes[N64NET_INPUT_MAX_PLAYERS], holds[N64NET_INPUT_MAX_PLAYERS];
    for (uint32_t &e : echoes) e = r.U32();
    for (uint32_t &h : holds) h = r.U32();
    const int32_t advantage = (int32_t)r.U32();
    const uint8_t wish = r.U8();
    const bool has_lag = r.p + 2 <= r.end;   // newer builds: lag flags + delay
    const uint8_t lag_flags = has_lag ? r.U8() : 0;
    const int32_t their_delay = has_lag ? (int32_t)r.U8() : -1;
    if (!r.ok) {
        stats.packets_dropped++;
        return;
    }
    const uint64_t now = Now();
    stats.packets_received++;
    last_recv_usecs = now;
    peer_stamp[player] = stamp;
    peer_stamp_at[player] = now;
    have_peer_stamp[player] = true;
    peer_advantage[player] = advantage;
    peer_advantage_valid[player] = true;
    peer_wish_of[player] = wish;
    peer_laggy[player] = (lag_flags & kLagFlagLaggy) != 0;
    peer_delay_of[player] = their_delay;
    // aggregates over every peer: the most-behind advantage (time sync), the largest wish (auto delay)
    remote_advantage_valid = false;
    peer_wish = 0;
    for (int q = 0; q < cfg.num_players; q++) {
        if (!IsRemote(q)) continue;
        if (!peer_laggy[q]) peer_wish = std::max(peer_wish, peer_wish_of[q]);
        if (!peer_advantage_valid[q]) continue;
        if (!remote_advantage_valid || peer_advantage[q] < remote_advantage) remote_advantage = peer_advantage[q];
        remote_advantage_valid = true;
    }
    const uint32_t echo = echoes[cfg.local_player], hold = holds[cfg.local_player];
    if (echo != 0) {
        const uint32_t rtt = (uint32_t)now - echo - hold;
        if (rtt < 10000000u) {
            rtt_samples[rtt_count++ % kRttRing] = rtt;
            rtt_of[player][rtt_count_of[player]++ % kRttRing] = rtt;
            stats.rtt_max_usecs = std::max(stats.rtt_max_usecs, rtt);
        }
    }
    // What this sender's machine still needs from each of our local players.
    for (int p = 0; p < cfg.num_players; p++) {
        if (IsLocal(p)) ack_from[player][p] = std::max(ack_from[player][p], acks[p]);
    }

    for (uint32_t i = 0; i < count; i++) {
        const uint32_t tick = first + i;
        if (tick < next_needed[player] || tick >= next_needed[player] + kRing) continue;
        Slot &s = remote[player][tick % kRing];
        if (s.tick == tick) continue;  // already have it
        s = frames[i];
        s.tick = tick;
        highest[player] = std::max(highest[player], tick + 1);
        if (peer_away) {
            peer_away = false;  // its inputs flow again (even if BACK was lost)
        }
        const Used &u = used[player][tick % kRing];
        // a prediction is "the last input repeats, no event": wrong if either differs
        if (u.tick == tick && u.predicted && (!PadEq(u.pad, s.pad) || s.ev_len != 0)) {
            stats.mispredictions++;
            if (!rollback_pending || tick < rollback_tick) {
                rollback_tick = tick;
            }
            rollback_pending = true;
        }
    }
    while (remote[player][next_needed[player] % kRing].tick == next_needed[player]) {
        next_needed[player]++;
    }
}

void n64net_input::HandleSync(Reader &r, uint8_t player, uint8_t count) {
    SyncEntry got[kSyncPerPacket];
    if (count > kSyncPerPacket) {
        stats.packets_dropped++;
        return;
    }
    for (uint32_t i = 0; i < count; i++) {
        got[i].tick = r.U32();
        got[i].kind = r.U8();
        got[i].hash = r.U32();
    }
    if (!r.ok) {
        stats.packets_dropped++;
        return;
    }
    stats.packets_received++;
    last_recv_usecs = Now();
    for (uint32_t i = 0; i < count; i++) {
        SyncEntry *ring = remote_sync[player];
        uint32_t &n = remote_sync_count[player];
        if (FindSync(ring, n, got[i].tick, got[i].kind)) continue;  // seen (repeats)
        ring[n++ % kSyncRing] = got[i];
        if (const SyncEntry *mine = FindSync(local_sync, local_sync_count, got[i].tick, got[i].kind)) {
            CompareSync(*mine, got[i]);
        }
    }
}

extern "C" {

void n64net_input::SendRetire(int p) {
    uint8_t buf[N64NET_INPUT_MAX_PACKET];
    Writer w{buf};
    w.U32(N64NET_INPUT_MAGIC);
    w.U8(kVersion);
    w.U8(kTypeRetire);
    w.U8((uint8_t)cfg.local_player);
    w.U8(0);
    w.U32(cfg.session_id);
    w.U8((uint8_t)p);
    w.U32(retire_last[p]);
    w.U32(retire_apply[p]);
    w.U8(retire_action_len[p]);
    for (uint32_t i = 0; i < retire_action_len[p]; i++) w.U8(retire_action[p][i]);
    // the leaver's last inputs, for machines that missed some of them
    const uint32_t last = retire_last[p];
    uint32_t first = last + 1 >= kRetireTail ? last + 1 - kRetireTail : 0;
    first = std::max(first, cfg.start_tick);
    uint32_t count = 0;
    for (uint32_t t = first; t <= last && last != kNoTick; t++) {
        if (remote[p][t % kRing].tick != t) {   // (the announcer has every one of them: see n64net_input_retire)
            first = t + 1;
            count = 0;
            continue;
        }
        count++;
    }
    w.U32(first);
    w.U8((uint8_t)count);
    for (uint32_t i = 0; i < count; i++) {
        const n64net_pad &pad = remote[p][(first + i) % kRing].pad;
        w.U16(pad.buttons);
        w.U8((uint8_t)pad.stick_x);
        w.U8((uint8_t)pad.stick_y);
        w.U8((uint8_t)pad.rstick_x);
        w.U8((uint8_t)pad.rstick_y);
        w.U16(pad.buttons_hi);
    }
    w.U32(Fnv(buf, (uint32_t)(w.p - buf)));
    Send(buf, (uint32_t)(w.p - buf));
}

// Player p leaves: inputs up to `last` are real (missing ones taken from the tail), neutral after it.
// Anything already simulated with other inputs, and anything simulated at or after `apply` (where the
// game changes the player), is rolled back.
void n64net_input::ApplyRetire(int p, uint32_t last, uint32_t apply, const uint8_t *action, uint32_t action_len,
                               uint32_t tail_first, uint32_t tail_count, const n64net_pad *tail) {
    for (uint32_t i = 0; i < tail_count; i++) {
        const uint32_t t = tail_first + i;
        if (t < next_needed[p] || t > last) continue;
        Slot &s = remote[p][t % kRing];
        if (s.tick == t) continue;
        s.tick = t;
        s.pad = tail[i];
        s.ev_len = 0;
        const Used &u = used[p][t % kRing];
        if (u.tick == t && u.predicted && !PadEq(u.pad, s.pad)) RequestRollback(t);
    }
    while (remote[p][next_needed[p] % kRing].tick == next_needed[p] && next_needed[p] <= last) next_needed[p]++;
    if (last != kNoTick && next_needed[p] <= last) {
        Log("n64net input: player %d left; ticks %u..%u of its inputs never arrived here (played as guesses)\n",
            p + 1, next_needed[p], last);
    }
    // ticks after `last` already simulated with a guess that wasn't "nothing pressed"
    const uint32_t from = last == kNoTick ? cfg.start_tick : last + 1;
    for (uint32_t t = from; t < highest_got; t++) {
        const Used &u = used[p][t % kRing];
        if (u.tick == t && (u.pad.buttons || u.pad.stick_x || u.pad.stick_y || u.pad.rstick_x || u.pad.rstick_y ||
                            u.pad.buttons_hi)) {
            RequestRollback(t);
            break;
        }
    }
    if (highest_got > apply) RequestRollback(apply);   // the game must change the player at `apply`
    retired[p] = true;
    open_slot[p] = false;
    join_from[p] = kNoTick;
    retire_last[p] = last;
    retire_apply[p] = apply;
    retire_action_len[p] = (uint8_t)std::min<uint32_t>(action_len, N64NET_INPUT_MAX_EVENT);
    memcpy(retire_action[p], action, retire_action_len[p]);
    remote_mask &= ~(1u << p);
    peer_advantage_valid[p] = false;
    peer_wish_of[p] = 0;
    peer_laggy[p] = false;
    peer_delay_of[p] = -1;
    rtt_count_of[p] = 0;
    Log("n64net input: player %d left: real inputs up to tick %d, the game changes it at tick %u\n", p + 1,
        last == kNoTick ? -1 : (int)last, apply);
}

// --- Joining a running match ---------------------------------------------------------------------------

// Append every newly confirmed tick to the log (what n64net_input_get returns for it, for every player).
void n64net_input::ExtendLog() {
    if (CatchingUp()) return;   // the joiner: the host's log comes first
    const uint32_t conf = n64net_input_confirmed_tick(this);
    if (conf == kNoTick) return;
    while (log_end <= conf) {
        const uint32_t t = log_end;
        for (int p = 0; p < cfg.num_players; p++) {
            n64net_pad pad = {0, 0, 0};
            const Slot *sl = nullptr;
            if (IsLocal(p)) {
                sl = LocalSlot(p, t);
            } else if (retired[p] && t < join_from[p]) {
                const Slot &r = remote[p][t % kRing];
                if (t <= retire_last[p] && r.tick == t) pad = r.pad;
            } else if (IsRemote(p)) {
                const Slot &r = remote[p][t % kRing];
                if (r.tick == t) sl = &r;
            }
            if (sl) pad = sl->pad;
            log_pad[p].push_back(pad);
            log_ext |= pad.rstick_x != 0 || pad.rstick_y != 0 || pad.buttons_hi != 0;
            if (sl && sl->ev_len) {
                LogEvent e = {};
                e.tick = t;
                e.player = (uint8_t)p;
                e.len = sl->ev_len;
                memcpy(e.ev, sl->ev, sl->ev_len);
                log_events.push_back(e);
            }
        }
        log_end++;
    }
}

// One log packet from tick `first` (as many ticks as fit); *next = the tick after the last one sent.
void n64net_input::SendLog(uint32_t first, uint32_t *next) {
    uint8_t buf[N64NET_INPUT_MAX_PACKET];
    Writer w{buf};
    const int np = cfg.num_players;
    const uint32_t padsz = log_ext ? 8 : 4;
    w.U32(N64NET_INPUT_MAGIC);
    w.U8(kVersion);
    w.U8(kTypeLog | (log_ext ? kTypeFlagRightStick : 0));
    w.U8((uint8_t)cfg.local_player);
    uint8_t *count_at = w.p;
    w.U8(0);
    w.U32(cfg.session_id);
    w.U8((uint8_t)__builtin_ctz(admit_mask));
    w.U32(first);
    w.U8((uint8_t)np);
    for (int p = 0; p < np; p++) {
        w.U8((uint8_t)((retired[p] ? 1 : 0) | (join_from[p] != kNoTick ? 2 : 0) | (open_slot[p] ? 4 : 0)));
        w.U32(retire_last[p]);
        w.U32(retire_apply[p]);
        w.U32(join_from[p]);
        w.U8(retire_action_len[p]);
        for (uint32_t i = 0; i < retire_action_len[p]; i++) w.U8(retire_action[p][i]);
    }
    // ticks that fit, with their events
    auto ev_begin = std::lower_bound(log_events.begin(), log_events.end(), first,
                                     [](const LogEvent &e, uint32_t t) { return e.tick < t; });
    uint32_t used = (uint32_t)(w.p - buf) + 1 + 4;   // + event count + checksum
    uint32_t count = 0;
    auto ev_end = ev_begin;
    while (count < 255 && first + count < log_end) {
        const uint32_t t = first + count;
        uint32_t cost = (uint32_t)np * padsz;
        auto e = ev_end;
        for (; e != log_events.end() && e->tick == t; ++e) cost += 4 + e->len;
        if (used + cost > N64NET_INPUT_MAX_PACKET) break;
        used += cost;
        ev_end = e;
        count++;
    }
    *count_at = (uint8_t)count;
    for (uint32_t i = 0; i < count; i++) {
        for (int p = 0; p < np; p++) {
            const n64net_pad &pad = log_pad[p][first + i - cfg.start_tick];
            w.U16(pad.buttons);
            w.U8((uint8_t)pad.stick_x);
            w.U8((uint8_t)pad.stick_y);
            if (log_ext) {
                w.U8((uint8_t)pad.rstick_x);
                w.U8((uint8_t)pad.rstick_y);
                w.U16(pad.buttons_hi);
            }
        }
    }
    w.U8((uint8_t)(ev_end - ev_begin));
    for (auto e = ev_begin; e != ev_end; ++e) {
        w.U16((uint16_t)(e->tick - first));
        w.U8(e->player);
        w.U8(e->len);
        for (uint32_t k = 0; k < e->len; k++) w.U8(e->ev[k]);
    }
    w.U32(Fnv(buf, (uint32_t)(w.p - buf)));
    const int to = __builtin_ctz(admit_mask);
    if (cfg.send_to) {
        cfg.send_to(cfg.user, to, buf, (uint32_t)(w.p - buf));
        stats.packets_sent++;
    } else {
        Send(buf, (uint32_t)(w.p - buf));
    }
    *next = first + count;
}

// The joiner: a piece of the host's log (and every player's retirement state).
void n64net_input::HandleLog(Reader &r, uint8_t count, bool ext) {
    const uint8_t to = r.U8();
    const uint32_t first = r.U32();
    const uint8_t np = r.U8();
    if (!cfg.joining || to != cfg.local_player || np != cfg.num_players) return;
    struct Info { uint8_t state; uint32_t last, apply, rejoin; uint8_t alen; uint8_t action[N64NET_INPUT_MAX_EVENT]; };
    Info info[N64NET_INPUT_MAX_PLAYERS] = {};
    for (int p = 0; p < np; p++) {
        info[p].state = r.U8();
        info[p].last = r.U32();
        info[p].apply = r.U32();
        info[p].rejoin = r.U32();
        info[p].alen = r.U8();
        if (info[p].alen > N64NET_INPUT_MAX_EVENT) { r.ok = false; break; }
        for (uint32_t i = 0; i < info[p].alen; i++) info[p].action[i] = r.U8();
    }
    std::vector<n64net_pad> pads((size_t)count * np);
    for (auto &pad : pads) {
        pad = n64net_pad{0, 0, 0};
        pad.buttons = r.U16();
        pad.stick_x = (int8_t)r.U8();
        pad.stick_y = (int8_t)r.U8();
        if (ext) {
            pad.rstick_x = (int8_t)r.U8();
            pad.rstick_y = (int8_t)r.U8();
            pad.buttons_hi = r.U16();
        }
    }
    const uint8_t nev = r.U8();
    std::vector<LogEvent> evs(nev);
    for (auto &e : evs) {
        e.tick = first + r.U16();
        e.player = r.U8();
        e.len = r.U8();
        if (e.len > N64NET_INPUT_MAX_EVENT || e.player >= np) { r.ok = false; break; }
        for (uint32_t k = 0; k < e.len; k++) e.ev[k] = r.U8();
    }
    if (!r.ok) {
        stats.packets_dropped++;
        return;
    }
    stats.packets_received++;
    last_recv_usecs = Now();
    // the retirement state as the host has it (for this machine's game: who is a CPU from when)
    for (int p = 0; p < np; p++) {
        const Info &in = info[p];
        if (!(in.state & 1)) continue;
        retired[p] = true;
        open_slot[p] = (in.state & 4) != 0;
        retire_last[p] = in.last;
        retire_apply[p] = in.apply;
        retire_action_len[p] = in.alen;
        memcpy(retire_action[p], in.action, in.alen);
        if (!IsLocal(p)) {
            join_from[p] = (in.state & 2) ? in.rejoin : kNoTick;
            if (join_from[p] == kNoTick) remote_mask &= ~(1u << p);
            else remote_mask |= 1u << p;
        }
    }
    if (first > log_end) {   // a piece before it is missing: keep this one for later
        if (log_ahead.size() < 4096) log_ahead[first] = Piece{ std::move(pads), std::move(evs), ext };
    } else {
        AppendLog(first, count, pads, evs, ext);
        // and the pieces that were waiting for it
        for (auto it = log_ahead.begin(); it != log_ahead.end() && it->first <= log_end;) {
            const uint32_t n = (uint32_t)(it->second.pads.size() / cfg.num_players);
            AppendLog(it->first, n, it->second.pads, it->second.events, it->second.ext);
            it = log_ahead.erase(it);
        }
    }
    SendHave(false);
}

void n64net_input::AppendLog(uint32_t first, uint32_t count, const std::vector<n64net_pad> &pads,
                             const std::vector<LogEvent> &evs, bool ext) {
    const int np = cfg.num_players;
    if (first > log_end || first + count <= log_end) return;
    for (uint32_t i = log_end - first; i < count; i++) {
        for (int p = 0; p < np; p++) log_pad[p].push_back(pads[i * np + p]);
    }
    for (const LogEvent &e : evs) {
        if (e.tick >= log_end) log_events.push_back(e);
    }
    log_end = first + count;
    log_ext |= ext;
}

void n64net_input::SendHave(bool force) {
    const uint64_t now = Now();
    if (!force && !(log_end != have_sent && now - have_sent_at >= 3000) && now - have_sent_at < 50000) return;
    uint8_t buf[32];
    Writer w{buf};
    w.U32(N64NET_INPUT_MAGIC);
    w.U8(kVersion);
    w.U8(kTypeHave);
    w.U8((uint8_t)cfg.local_player);
    w.U8(0);
    w.U32(cfg.session_id);
    w.U32(log_end);
    w.U32(Fnv(buf, (uint32_t)(w.p - buf)));
    Send(buf, (uint32_t)(w.p - buf));
    have_sent = log_end;
    have_sent_at = now;
}

// The host: stream the log to the joiner; when it is nearly caught up, announce the live tick.
void n64net_input::ServeJoiner() {
    if (!admit_mask) return;
    const uint64_t now = Now();
    if (admit_sent < admit_have) admit_sent = admit_have;
    uint64_t rtt = 100000;
    if (const uint32_t n = std::min(rtt_count, kRttRing)) {
        uint64_t sum = 0;
        for (uint32_t i = 0; i < n; i++) sum += rtt_samples[i];
        rtt = sum / n;
    }
    if (now - admit_progress_at > std::max<uint64_t>(60000, 2 * rtt + 30000)) {
        // nothing new arrived there for a while (a lost piece): resend from what it has
        admit_sent = admit_have;
        admit_progress_at = now;
    }
    if (!admit_heard) {   // the joiner's game is still starting: no flood it would drop
        if (now - admit_progress_at > 200000 && admit_have < log_end) {
            uint32_t next = 0;
            SendLog(admit_have, &next);
            admit_progress_at = now;
        }
        return;
    }
    for (uint32_t k = 0; k < kLogPacketsPerSend && admit_sent < log_end; k++) SendLog(admit_sent, &admit_sent);
    if (rejoin_tick == kNoTick && log_end - std::min(admit_have, log_end) <= kJoinNear) {
        uint32_t live = std::max(highest_got, last_sample_tick + 1);
        live = std::max(live, local_started ? local_last + 1 : local_base);
        live += kJoinMargin;
        rejoin_mask = admit_mask;
        rejoin_tick = live;
        rejoin_repeats = kRejoinRepeats;
        ApplyRejoin(admit_mask, live);
        SendRejoin();
    }
    if (rejoin_tick != kNoTick && admit_have >= rejoin_tick) {
        Log("n64net input: the joiner has caught up (players %x live from tick %u)\n", admit_mask, rejoin_tick);
        admit_mask = 0;
    }
}

void n64net_input::SendRejoin() {
    uint8_t buf[32];
    Writer w{buf};
    w.U32(N64NET_INPUT_MAGIC);
    w.U8(kVersion);
    w.U8(kTypeRejoin);
    w.U8((uint8_t)cfg.local_player);
    w.U8(0);
    w.U32(cfg.session_id);
    w.U8((uint8_t)rejoin_mask);
    w.U32(rejoin_tick);
    w.U32(Fnv(buf, (uint32_t)(w.p - buf)));
    Send(buf, (uint32_t)(w.p - buf));
}

// Players in `mask` are human again from `tick`, driven by the joiner's machine.
void n64net_input::ApplyRejoin(uint32_t mask, uint32_t tick) {
    bool mine = false;
    for (int p = 0; p < cfg.num_players; p++) {
        if (!(mask & (1u << p)) || join_from[p] == tick) continue;
        if (!retired[p] && !IsRemote(p) && !IsLocal(p)) MarkOpenSlot(p);   // n64net_input_add on the host
        join_from[p] = tick;
        if (IsLocal(p)) {   // this machine is the joiner
            mine = true;
            continue;
        }
        remote_mask |= 1u << p;
        next_needed[p] = std::max(next_needed[p], tick);
        highest[p] = std::max(highest[p], tick);
        peer_advantage_valid[p] = false;
        for (int q = 0; q < cfg.num_players; q++) {
            if (IsLocal(q)) ack_from[p][q] = std::max(ack_from[p][q], tick);
        }
        if (highest_got > tick) RequestRollback(tick);
        Log("n64net input: player %d is back (a new machine) from tick %u\n", p + 1, tick);
    }
    if (mine && live_from == kNoTick) {
        live_from = tick;
        local_base = tick;
        for (int q = 0; q < cfg.num_players; q++) {
            if (!IsRemote(q)) continue;
            next_needed[q] = std::max(next_needed[q], tick);
            while (remote[q][next_needed[q] % kRing].tick == next_needed[q]) next_needed[q]++;
        }
        Log("n64net input: joining: live from tick %u (log %u ticks so far)\n", tick, log_end - cfg.start_tick);
    }
}

n64net_input *n64net_input_create(const n64net_input_config *config) {
    if (!config || config->num_players < 1 || config->num_players > N64NET_INPUT_MAX_PLAYERS ||
        config->local_player < 0 || config->local_player >= config->num_players) {
        return nullptr;
    }
    n64net_input *net = new n64net_input();
    net->cfg = *config;
    if (net->cfg.ticks_per_second == 0) net->cfg.ticks_per_second = 60;
    const uint32_t all = (1u << config->num_players) - 1;
    net->local_mask = (config->local_mask ? config->local_mask : 0) | (1u << config->local_player);
    net->local_mask &= all;
    net->remote_mask = config->remote_mask ? config->remote_mask : (all & ~net->local_mask);
    net->remote_mask &= ~net->local_mask;
    net->delay = config->input_delay;
    net->local_base = config->start_tick;
    net->log_end = config->start_tick;
    net->admit_have = net->admit_sent = config->start_tick;
    for (int p = 0; p < N64NET_INPUT_MAX_PLAYERS; p++) {
        net->join_from[p] = kNoTick;
        net->local[p].resize(kRing);
        net->remote[p].resize(kRing);
        net->used[p].resize(kRing);
        net->next_needed[p] = config->start_tick;
        for (int q = 0; q < N64NET_INPUT_MAX_PLAYERS; q++) net->ack_from[p][q] = config->start_tick;
    }
    if (net->delay > 0 && !config->joining) {
        // The first `delay` ticks are neutral on every machine, and known now:
        // a side waiting for them must not depend on the other's first sample
        // (both would wait for each other in lockstep).
        for (uint32_t t = config->start_tick; t < config->start_tick + net->delay; t++) {
            net->StoreAllLocal(t, nullptr);
        }
        net->local_started = true;
        net->local_last = config->start_tick + net->delay - 1;
    }
    return net;
}

void n64net_input_destroy(n64net_input *net) { delete net; }

void n64net_input::SetLocalPads(uint32_t tick, const n64net_pad *pads) {
    UpdateAutoDelay(Now());
    const uint32_t target = tick + delay;
    if (cfg.joining && (live_from == kNoTick || target < live_from)) {
        // a joiner before its live tick: its players' inputs there are in the host's log
        for (int p = 0; p < cfg.num_players; p++) {
            if (IsLocal(p)) last_pad[p] = pads[p];
        }
        last_sample_tick = tick;
        SendInputs();
        return;
    }
    if (!local_started) {
        // Ticks before the first sample lands are neutral on every machine.
        for (uint32_t t = local_base; t < target; t++) StoreAllLocal(t, nullptr);
        local_started = true;
        local_last = target - 1;
    }
    if (target > local_last) {
        // A grown delay leaves a gap: repeat the last samples there.
        for (uint32_t t = local_last + 1; t < target; t++) StoreAllLocal(t, last_pad);
        StoreAllLocal(target, pads);
        if (event_pending) {   // the event rides on the first local player's stream at this tick
            Slot &sl = local[cfg.local_player][target % kRing];
            sl.ev_len = event_len;
            memcpy(sl.ev, event, event_len);
            event_pending = false;
        }
        local_last = target;
    }
    // else: a shrunk delay; that tick is already scheduled (and maybe sent): drop (an event waits).
    for (int p = 0; p < cfg.num_players; p++) {
        if (IsLocal(p)) last_pad[p] = pads[p];
    }
    last_sample_tick = tick;
    SendInputs();
}

void n64net_input_set_local(n64net_input *net, uint32_t tick, n64net_pad pad) {
    n64net_pad pads[N64NET_INPUT_MAX_PLAYERS] = {};
    pads[net->cfg.local_player] = pad;
    net->SetLocalPads(tick, pads);
}

void n64net_input_set_local_pads(n64net_input *net, uint32_t tick, const n64net_pad pads[N64NET_INPUT_MAX_PLAYERS]) {
    net->SetLocalPads(tick, pads);
}

int n64net_input_set_event(n64net_input *net, const void *data, uint32_t size) {
    if (net->event_pending || size == 0 || size > N64NET_INPUT_MAX_EVENT) return 0;
    memcpy(net->event, data, size);
    net->event_len = (uint8_t)size;
    net->event_pending = true;
    return 1;
}

uint32_t n64net_input_get_event(n64net_input *net, int player, uint32_t tick, void *out, uint32_t cap) {
    const Slot *sl = nullptr;
    if (net->FromLog(tick)) {
        const LogEvent *e = player >= 0 && player < net->cfg.num_players ? net->LogEventAt(player, tick) : nullptr;
        if (!e) return 0;
        const uint32_t n = std::min<uint32_t>(e->len, cap);
        memcpy(out, e->ev, n);
        return e->len;
    }
    if (net->IsLocal(player)) {
        sl = net->LocalSlot(player, tick);
    } else if (net->IsRemote(player)) {
        const Slot &r = net->remote[player][tick % kRing];
        if (r.tick == tick) sl = &r;   // not received yet: no event (a prediction; a late one rolls back)
    }
    if (!sl || sl->ev_len == 0) return 0;
    const uint32_t n = std::min<uint32_t>(sl->ev_len, cap);
    memcpy(out, sl->ev, n);
    return sl->ev_len;
}

int n64net_input_ready(n64net_input *net, uint32_t tick) {
    if (net->FromLog(tick)) return tick < net->log_end ? 1 : 0;
    const uint32_t needed = net->MinNextNeeded();
    if (needed == kNoTick) return 1;  // no remote players
    // Confirmed up to needed - 1; ready if that is >= tick - window.
    return (uint64_t)needed + net->cfg.rollback_window >= (uint64_t)tick + 1 ? 1 : 0;
}

int n64net_input_get(n64net_input *net, int player, uint32_t tick, n64net_pad *out) {
    *out = n64net_pad{0, 0, 0};
    if (player < 0 || player >= net->cfg.num_players) return 0;
    if (net->FromLog(tick)) {   // a joiner catching up: the host's log
        net->LogPad(player, tick, out);
        return 0;
    }
    if (net->IsLocal(player)) {
        net->LocalAt(player, tick, out);  // neutral if not scheduled (before the first sample)
        return 0;
    }
    if (net->retired[player] && tick < net->join_from[player]) {   // dropped out: its real inputs up to the agreed tick, then nothing (until a joiner takes it over)
        const Slot &s = net->remote[player][tick % kRing];
        if (tick <= net->retire_last[player] && s.tick == tick) *out = s.pad;
        return 0;
    }
    if (!net->IsRemote(player)) return 0;  // CPU / empty: neutral
    net->highest_got = std::max(net->highest_got, tick + 1);
    const Slot &s = net->remote[player][tick % kRing];
    Used &u = net->used[player][tick % kRing];
    u.tick = tick;
    if (s.tick == tick) {
        *out = s.pad;
        u.pad = s.pad;
        u.predicted = false;
        return 0;
    }
    // Predict: the newest confirmed input before this tick repeats.
    const uint32_t last = net->next_needed[player];
    if (last > net->cfg.start_tick && net->remote[player][(last - 1) % kRing].tick == last - 1 && last - 1 < tick) {
        *out = net->remote[player][(last - 1) % kRing].pad;
    }
    u.pad = *out;
    u.predicted = true;
    net->stats.predictions++;
    return 1;
}

int n64net_input_handle_packet(n64net_input *net, const uint8_t *data, uint32_t size) {
    if (!net || size < 16) return 0;
    Reader r{data, data + size};
    if (r.U32() != N64NET_INPUT_MAGIC) return 0;
    const uint8_t version = r.U8();
    const uint8_t type = r.U8();
    const uint8_t player = r.U8();
    const uint8_t count = r.U8();
    const uint32_t session = r.U32();
    // Checksum: the last 4 bytes cover everything before them.
    const uint32_t sum = ((uint32_t)data[size - 4] << 24) | ((uint32_t)data[size - 3] << 16) |
                         ((uint32_t)data[size - 2] << 8) | data[size - 1];
    // (a joiner's "have" packets come from a player that is still retired)
    const uint8_t base_type = type & ~(kTypeFlagRightStick | kTypeFlagEvents);
    if (version != kVersion || session != net->cfg.session_id || sum != Fnv(data, size - 4) ||
        player >= N64NET_INPUT_MAX_PLAYERS || (!net->IsRemote(player) && base_type != kTypeHave)) {
        net->stats.packets_dropped++;
        return 1;
    }
    r.end = data + size - 4;
    switch (type & ~(kTypeFlagRightStick | kTypeFlagEvents)) {
    case kTypeInput:
        net->HandleInput(r, player, count, (type & kTypeFlagRightStick) != 0, (type & kTypeFlagEvents) != 0);
        break;
    case kTypeSync:
        net->HandleSync(r, player, count);
        break;
    case kTypeRetire: {
        const uint8_t who = r.U8();
        const uint32_t last = r.U32(), apply = r.U32();
        const uint8_t alen = r.U8();
        uint8_t action[N64NET_INPUT_MAX_EVENT] = {};
        for (uint32_t i = 0; i < alen && i < N64NET_INPUT_MAX_EVENT; i++) action[i] = r.U8();
        const uint32_t tail_first = r.U32();
        const uint8_t tail_count = r.U8();
        n64net_pad tail[kRetireTail];
        for (uint32_t i = 0; i < tail_count && i < kRetireTail; i++) {
            tail[i].buttons = r.U16();
            tail[i].stick_x = (int8_t)r.U8();
            tail[i].stick_y = (int8_t)r.U8();
            tail[i].rstick_x = (int8_t)r.U8();
            tail[i].rstick_y = (int8_t)r.U8();
            tail[i].buttons_hi = r.U16();
        }
        if (!r.ok || alen > N64NET_INPUT_MAX_EVENT || tail_count > kRetireTail || who >= N64NET_INPUT_MAX_PLAYERS) {
            net->stats.packets_dropped++;
            break;
        }
        net->stats.packets_received++;
        net->last_recv_usecs = net->Now();
        if ((!net->retired[who] || net->join_from[who] != kNoTick) && !net->IsLocal(who)) {
            net->ApplyRetire(who, last, apply, action, alen, tail_first, tail_count, tail);
        }
        break;
    }
    case kTypeLog:
        net->HandleLog(r, count, (type & kTypeFlagRightStick) != 0);
        break;
    case kTypeHave: {
        const uint32_t have = r.U32();
        if (!r.ok) break;
        if (net->admit_mask && player == (uint8_t)__builtin_ctz(net->admit_mask)) {
            if (!net->admit_heard) net->Log("n64net input: the joiner's game is up (it has %u ticks of the log)\n", have);
            net->admit_heard = true;
            if (have > net->admit_have) {
                net->admit_have = have;
                net->admit_progress_at = net->Now();
            }
        }
        break;
    }
    case kTypeRejoin: {
        const uint8_t mask = r.U8();
        const uint32_t tick = r.U32();
        if (!r.ok) break;
        net->stats.packets_received++;
        net->last_recv_usecs = net->Now();
        net->ApplyRejoin(mask, tick);
        break;
    }
    case kTypeAway:
    case kTypeBack:
        net->stats.packets_received++;
        net->last_recv_usecs = net->Now();
        if (net->peer_away != (type == kTypeAway)) {
            net->Log("n64net input: the other player %s\n",
                     type == kTypeAway ? "switched to another app" : "is back");
        }
        net->peer_away = (type == kTypeAway);
        break;
    default:
        net->stats.packets_dropped++;
        break;
    }
    return 1;
}

void n64net_input_resend(n64net_input *net) {
    if (net->Now() - net->last_send_usecs >= 10000) {
        net->SendInputs();
    }
}

int n64net_peek_rollback(n64net_input *net, uint32_t *tick) {
    if (!net->rollback_pending) return 0;
    *tick = net->rollback_tick;
    return 1;
}

void n64net_input_set_window(n64net_input *net, uint32_t rollback_window) {
    net->cfg.rollback_window = rollback_window;
}

int n64net_take_rollback(n64net_input *net, uint32_t *tick) {
    if (!net->rollback_pending) return 0;
    *tick = net->rollback_tick;
    net->rollback_pending = false;
    return 1;
}

uint32_t n64net_input_confirmed_tick(n64net_input *net) {
    if (net->CatchingUp()) return net->log_end > net->cfg.start_tick ? net->log_end - 1 : kNoTick;
    uint32_t needed = net->MinNextNeeded();
    if (needed == kNoTick) needed = net->local_started ? net->local_last + 1 : net->cfg.start_tick;
    if (net->local_started) needed = std::min(needed, net->local_last + 1);
    else needed = std::min(needed, net->local_base);
    return needed == 0 ? kNoTick : needed - 1;
}

void n64net_input_set_delay(n64net_input *net, uint32_t input_delay) {
    if (input_delay != net->delay) {
        net->Log("n64net input: input delay %u -> %u\n", net->delay, input_delay);
        net->delay = input_delay;
    }
}

void n64net_input_set_auto_delay(n64net_input *net, uint32_t min_delay, uint32_t max_delay) {
    net->auto_min = min_delay;
    net->auto_max = max_delay;
    net->calm_since = net->Now();
}

uint32_t n64net_input_get_delay(n64net_input *net) { return net->delay; }

int n64net_timesync_should_skip(n64net_input *net) {
    if (net->cfg.rollback_window == 0 || !net->remote_advantage_valid || net->CatchingUp()) return 0;
    // Twice the ticks we run ahead (our advantage minus the peer's), smoothed
    // over ~16 frames so jitter doesn't make both sides take turns.
    const int32_t ahead2 = net->LocalAdvantage() - net->remote_advantage;
    net->ahead16 += ahead2 - net->ahead16 / 16;
    if (net->timesync_cooldown != 0) {
        net->timesync_cooldown--;
        return 0;
    }
    // A laggy machine carries the rollback its peers' delays don't cover: it may run lag_excess ticks further
    // ahead before it waits (its normal peers then never wait or roll back for it).
    if (net->ahead16 < (3 + (int32_t)net->lag_excess) * 16) return 0;  // 1.5+ ticks ahead
    net->timesync_cooldown = net->cfg.ticks_per_second / 2;
    net->ahead16 -= 2 * 16;  // a skip closes the gap by one tick
    net->stats.timesync_skips++;
    return 1;
}

void n64net_sync_record(n64net_input *net, uint32_t tick, uint8_t kind, uint32_t hash) {
    SyncEntry e;
    e.tick = tick;
    e.kind = kind;
    e.hash = hash;
    net->local_sync[net->local_sync_count % kSyncRing] = e;
    net->local_sync_count++;
    for (int q = 0; q < N64NET_INPUT_MAX_PLAYERS; q++) {
        if (const SyncEntry *theirs = n64net_input::FindSync(net->remote_sync[q], net->remote_sync_count[q], tick, kind)) {
            net->CompareSync(e, *theirs);
        }
    }
    net->SendSync();
}

int n64net_sync_desynced(n64net_input *net, uint32_t *tick, uint8_t *kind) {
    if (!net->desynced) return 0;
    if (tick) *tick = net->desync_tick;
    if (kind) *kind = net->desync_kind;
    return 1;
}

uint32_t n64net_sync_matched(n64net_input *net) { return net->sync_matched; }

void n64net_input_set_away(n64net_input *net, int away) {
    uint8_t buf[32];
    Writer w{buf};
    w.U32(N64NET_INPUT_MAGIC);
    w.U8(kVersion);
    w.U8(away ? kTypeAway : kTypeBack);
    w.U8((uint8_t)net->cfg.local_player);
    w.U8(0);
    w.U32(net->cfg.session_id);
    // Padding so every packet is at least 16 bytes (the cheapest size check).
    w.U32(0);
    w.U32(Fnv(buf, (uint32_t)(w.p - buf)));
    for (int i = 0; i < 3; i++) net->Send(buf, (uint32_t)(w.p - buf));  // UDP: a few copies
    net->SendInputs();
}

int n64net_input_peer_away(n64net_input *net) { return net->peer_away ? 1 : 0; }

uint64_t n64net_input_peer_silence_usecs(n64net_input *net) {
    return net->last_recv_usecs ? net->Now() - net->last_recv_usecs : 0;
}

void n64net_input_get_stats(n64net_input *net, n64net_input_stats *out) {
    *out = net->stats;
    const uint32_t n = std::min(net->rtt_count, kRttRing);
    uint64_t sum = 0;
    for (uint32_t i = 0; i < n; i++) sum += net->rtt_samples[i];
    out->rtt_avg_usecs = n ? (uint32_t)(sum / n) : 0;
    out->local_advantage = net->LocalAdvantage();
    out->remote_advantage = net->remote_advantage;
    out->input_delay = net->delay;
    out->laggy = net->laggy ? 1 : 0;
    out->lag_excess = net->lag_excess;
}

int n64net_rollback_frame(n64net_input *net, uint32_t tick, const n64net_rollback_callbacks *cb) {
    const uint32_t n = cb->slot_count;
    const bool anchored = cb->swap != nullptr;
    const uint32_t A = n;   // the anchor's slot (with swap)
    if (net->slot_tick.size() != n + 1) net->slot_tick.assign(n + 1, kNoTick);
    // Save the state before tick t into its ring slot. The state there before (an older tick) may still be needed
    // by a rollback (a deep window): keep it as the anchor when the anchor would otherwise not lie at or before
    // every tick a rollback can start at (the first unconfirmed one), or when it is a newer such state.
    auto save = [&](uint32_t t) {
        const uint32_t k = t % n;
        uint32_t &anchor = net->slot_tick[A];
        if (anchor != kNoTick && anchor >= t) anchor = kNoTick;   // from a timeline being replaced
        const uint32_t s = net->slot_tick[k];
        if (anchored && s != kNoTick && s != t && s < t) {
            const uint32_t c = n64net_input_confirmed_tick(net);
            const uint32_t need = c == kNoTick ? 0 : c + 1;
            if (anchor == kNoTick || (anchor > need && s < anchor) || (s <= need && s > anchor)) {
                cb->swap(cb->user, k, A);
                anchor = s;
                net->slot_tick[k] = kNoTick;
            }
        }
        cb->save(cb->user, k);
        net->slot_tick[k] = t;
    };
    int depth = 0;
    uint32_t from;
    if (n64net_take_rollback(net, &from) && from < tick) {
        uint32_t slot = kNoTick;
        if (net->slot_tick[from % n] == from) {
            slot = from % n;
        } else if (anchored && net->slot_tick[A] != kNoTick && net->slot_tick[A] <= from) {
            slot = A;                // older than the ring: replay from the anchor
            from = net->slot_tick[A];
        }
        if (slot == kNoTick) {
            net->Log("n64net input: cannot roll back to tick %u from %u: state too old\n", from, tick);
            depth = -1;
        } else {
            net->driver_tick = from;
            cb->load(cb->user, slot);
            net->resimulating = 1;
            for (uint32_t t = from; t < tick; t++) {
                net->driver_tick = t;
                if (t != from) save(t);
                cb->advance(cb->user);
            }
            net->resimulating = 0;
            depth = (int)(tick - from);
            net->stats.rollbacks++;
        }
    }
    net->driver_tick = tick;
    save(tick);
    return depth;
}

uint32_t n64net_rollback_callback_tick(n64net_input *net) { return net->driver_tick; }

int n64net_rollback_resimulating(n64net_input *net) { return net->resimulating; }

} // extern "C"

int n64net_input_retire(n64net_input *net, int player, const void *action, uint32_t size, uint32_t *apply_tick) {
    if (!net || player < 0 || player >= net->cfg.num_players || net->IsLocal(player) ||
        (net->retired[player] && net->join_from[player] == kNoTick) || size > N64NET_INPUT_MAX_EVENT) {
        return 0;
    }
    // the last tick of this player everyone gets: what arrived here without a gap
    const uint32_t last = net->next_needed[player] > net->cfg.start_tick ? net->next_needed[player] - 1 : kNoTick;
    // the change happens after every tick this machine may already have simulated
    uint32_t apply = std::max(last == kNoTick ? net->cfg.start_tick : last + 1, net->last_sample_tick + 1);
    apply = std::max(apply, net->highest_got);
    net->ApplyRetire(player, last, apply, (const uint8_t *)action, size, 0, 0, nullptr);
    net->retire_repeats[player] = kRetireRepeats;
    net->SendRetire(player);
    if (apply_tick) *apply_tick = apply;
    return 1;
}

int n64net_input_retired(n64net_input *net, int player, uint32_t *last_tick, uint32_t *apply_tick, void *action,
                         uint32_t *size) {
    if (!net || player < 0 || player >= N64NET_INPUT_MAX_PLAYERS || !net->retired[player] || net->open_slot[player]) {
        return 0;
    }
    if (last_tick) *last_tick = net->retire_last[player];
    if (apply_tick) *apply_tick = net->retire_apply[player];
    if (action && size) {
        const uint32_t n = std::min<uint32_t>(*size, net->retire_action_len[player]);
        memcpy(action, net->retire_action[player], n);
        *size = net->retire_action_len[player];
    }
    return 1;
}

int n64net_input_remote_players(n64net_input *net) { return net ? (int)__builtin_popcount(net->remote_mask) : 0; }

int n64net_input_admit(n64net_input *net, uint32_t player_mask) {
    if (!net || net->cfg.joining) return 0;
    if (player_mask == 0) {   // cancel (the joiner went away before its live tick was announced)
        if (net->rejoin_tick == kNoTick || !net->admit_mask) net->admit_mask = 0;
        return 1;
    }
    if (net->admit_mask) return 0;
    for (int p = 0; p < net->cfg.num_players; p++) {
        if ((player_mask & (1u << p)) && (!net->retired[p] || net->join_from[p] != kNoTick || net->IsLocal(p))) return 0;
    }
    if (player_mask >> net->cfg.num_players) return 0;
    net->ExtendLog();
    net->admit_mask = player_mask;
    net->admit_have = net->admit_sent = net->cfg.start_tick;
    net->admit_progress_at = net->Now();
    net->admit_heard = false;
    net->rejoin_tick = kNoTick;
    net->Log("n64net input: a machine joins as players %x: sending it the log (%u ticks so far)\n", player_mask,
             net->log_end - net->cfg.start_tick);
    net->ServeJoiner();
    return 1;
}

int n64net_input_add(n64net_input *net, uint32_t player_mask) {
    if (!net || net->cfg.joining || player_mask == 0 || (player_mask >> net->cfg.num_players)) return 0;
    if (net->admit_mask) return 0;   // a join is already under way
    uint32_t marked = 0;
    for (int p = 0; p < net->cfg.num_players; p++) {
        if (!(player_mask & (1u << p))) continue;
        if (net->open_slot[p] && net->join_from[p] == kNoTick) continue;   // (asked again)
        if (net->IsLocal(p) || net->IsRemote(p) || net->retired[p]) return 0;   // someone plays it, or it left
        marked |= 1u << p;
    }
    for (int p = 0; p < net->cfg.num_players; p++) {
        if (marked & (1u << p)) net->MarkOpenSlot(p);
    }
    if (!n64net_input_admit(net, player_mask)) {
        for (int p = 0; p < net->cfg.num_players; p++) {
            if (marked & (1u << p)) net->retired[p] = net->open_slot[p] = false;
        }
        return 0;
    }
    net->Log("n64net input: players %x were nobody's so far (CPUs or empty): the joiner takes them\n", player_mask);
    return 1;
}

int n64net_input_catching_up(n64net_input *net) { return net && net->CatchingUp() ? 1 : 0; }

int n64net_input_rejoined(n64net_input *net, int player, uint32_t *tick) {
    if (!net || player < 0 || player >= N64NET_INPUT_MAX_PLAYERS || net->join_from[player] == kNoTick) return 0;
    if (tick) *tick = net->join_from[player];
    return 1;
}

uint32_t n64net_input_log_ticks(n64net_input *net, uint32_t *live_tick) {
    if (!net) return 0;
    if (live_tick) *live_tick = net->cfg.joining ? net->live_from : net->rejoin_tick;
    return net->log_end - net->cfg.start_tick;
}

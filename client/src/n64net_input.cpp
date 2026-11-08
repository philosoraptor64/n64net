/* n64net input exchange (see n64net_input.h).
 *
 * Wire format, all integers big endian:
 *   input:  u32 magic 'N64I' | u8 version | u8 type=0 | u8 player | u8 count |
 *           u32 session | u32 ack[4] (next tick this sender needs from each
 *           player) | u32 first_tick | count x (u16 buttons, s8 x, s8 y) |
 *           u32 stamp (sender clock, usecs) | u32 echo (newest stamp received
 *           from the destination) | u32 hold (usecs since that stamp arrived) |
 *           s32 advantage | u32 checksum
 *   sync:   magic | version | type=1 | player | count | session |
 *           count x (u32 tick, u8 kind, u32 hash) | checksum
 *   away:   magic | version | type=2 (away) or 3 (back) | player | 0 | session | checksum
 * Inputs are sent oldest-unacknowledged first, up to 16 per packet, so a
 * lost packet is covered by the next one and a long gap still drains. */
#include "n64net_input.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr uint8_t kVersion = 1;
constexpr uint8_t kTypeInput = 0, kTypeSync = 1, kTypeAway = 2, kTypeBack = 3;
constexpr uint32_t kRing = 1024;           // ticks of history kept per player
constexpr uint32_t kMaxFramesPerPacket = 16;
constexpr uint32_t kSyncRing = 64;
constexpr uint32_t kSyncPerPacket = 4;
constexpr uint32_t kNoTick = UINT32_MAX;

struct Slot {
    uint32_t tick = kNoTick;
    n64net_pad pad = {0, 0, 0};
};
struct Used {
    uint32_t tick = kNoTick;
    n64net_pad pad = {0, 0, 0};
    bool predicted = false;
};
struct SyncEntry {
    uint32_t tick = kNoTick;
    uint8_t kind = 0;
    uint32_t hash = 0;
};

bool PadEq(const n64net_pad &a, const n64net_pad &b) {
    return a.buttons == b.buttons && a.stick_x == b.stick_x && a.stick_y == b.stick_y;
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

    // Local player's scheduled inputs (tick = sample tick + delay).
    std::vector<Slot> local;
    bool local_started = false;
    uint32_t local_last = 0;        // newest scheduled tick
    uint32_t last_sample_tick = 0;  // newest tick passed to set_local
    n64net_pad last_pad = {0, 0, 0};
    uint32_t delay;

    // Remote players: confirmed inputs, what the simulation used, progress.
    std::vector<Slot> remote[N64NET_INPUT_MAX_PLAYERS];
    std::vector<Used> used[N64NET_INPUT_MAX_PLAYERS];
    uint32_t next_needed[N64NET_INPUT_MAX_PLAYERS] = {}; // all ticks below are confirmed
    uint32_t highest[N64NET_INPUT_MAX_PLAYERS] = {};     // newest tick received (+1; 0 = none)
    uint32_t peer_ack = 0;  // lowest "next tick needed" any peer reported for our player

    bool rollback_pending = false;
    uint32_t rollback_tick = 0;
    int resimulating = 0;
    std::vector<uint32_t> slot_tick;

    // Timing.
    uint64_t last_send_usecs = 0;
    uint64_t last_recv_usecs = 0;
    uint32_t peer_stamp = 0;          // newest stamp from the peer
    uint64_t peer_stamp_at = 0;       // when it arrived (our clock)
    bool have_peer_stamp = false;
    int32_t remote_advantage = 0;
    bool remote_advantage_valid = false;
    int32_t ahead16 = 0;
    uint32_t timesync_cooldown = 0;

    // Sync checks.
    SyncEntry local_sync[kSyncRing];
    SyncEntry remote_sync[kSyncRing];
    uint32_t local_sync_count = 0;
    bool desynced = false;
    uint32_t desync_tick = 0;
    uint8_t desync_kind = 0;
    uint32_t sync_matched = 0;

    bool peer_away = false;

    n64net_input_stats stats = {};
    uint32_t rtt_samples[32] = {};
    uint32_t rtt_count = 0;

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

    void StoreLocal(uint32_t tick, n64net_pad pad) {
        Slot &s = local[tick % kRing];
        s.tick = tick;
        s.pad = pad;
    }
    bool LocalAt(uint32_t tick, n64net_pad *out) const {
        const Slot &s = local[tick % kRing];
        if (s.tick != tick) return false;
        *out = s.pad;
        return true;
    }

    void SendInputs();
    void SendSync();
    void HandleInput(Reader &r, uint8_t player, uint8_t count);
    void HandleSync(Reader &r, uint8_t count);
    void CompareSync(const SyncEntry &a, const SyncEntry &b);
};

void n64net_input::SendInputs() {
    uint8_t buf[N64NET_INPUT_MAX_PACKET];
    Writer w{buf};
    // Oldest tick the peers still need from us, up to 16 frames.
    uint32_t first = std::max(peer_ack, cfg.start_tick);
    uint32_t count = 0;
    if (local_started && first <= local_last) {
        count = std::min(kMaxFramesPerPacket, local_last - first + 1);
    }
    w.U32(N64NET_INPUT_MAGIC);
    w.U8(kVersion);
    w.U8(kTypeInput);
    w.U8((uint8_t)cfg.local_player);
    w.U8((uint8_t)count);
    w.U32(cfg.session_id);
    for (int p = 0; p < N64NET_INPUT_MAX_PLAYERS; p++) {
        w.U32(IsRemote(p) ? next_needed[p] : 0);
    }
    w.U32(first);
    for (uint32_t i = 0; i < count; i++) {
        n64net_pad pad = {0, 0, 0};
        LocalAt(first + i, &pad);
        w.U16(pad.buttons);
        w.U8((uint8_t)pad.stick_x);
        w.U8((uint8_t)pad.stick_y);
    }
    const uint64_t now = Now();
    w.U32((uint32_t)now);
    w.U32(have_peer_stamp ? peer_stamp : 0);
    w.U32(have_peer_stamp ? (uint32_t)(now - peer_stamp_at) : 0);
    w.U32((uint32_t)LocalAdvantage());
    w.U32(Fnv(buf, (uint32_t)(w.p - buf)));
    Send(buf, (uint32_t)(w.p - buf));
    last_send_usecs = now;
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

void n64net_input::HandleInput(Reader &r, uint8_t player, uint8_t count) {
    uint32_t acks[N64NET_INPUT_MAX_PLAYERS];
    for (uint32_t &a : acks) a = r.U32();
    const uint32_t first = r.U32();
    n64net_pad pads[256];
    for (uint32_t i = 0; i < count; i++) {
        pads[i].buttons = r.U16();
        pads[i].stick_x = (int8_t)r.U8();
        pads[i].stick_y = (int8_t)r.U8();
    }
    const uint32_t stamp = r.U32();
    const uint32_t echo = r.U32();
    const uint32_t hold = r.U32();
    const int32_t advantage = (int32_t)r.U32();
    if (!r.ok || count > kMaxFramesPerPacket) {
        stats.packets_dropped++;
        return;
    }
    const uint64_t now = Now();
    stats.packets_received++;
    last_recv_usecs = now;
    peer_stamp = stamp;
    peer_stamp_at = now;
    have_peer_stamp = true;
    remote_advantage = advantage;
    remote_advantage_valid = true;
    if (echo != 0) {
        const uint32_t rtt = (uint32_t)now - echo - hold;
        if (rtt < 10000000u) {
            rtt_samples[rtt_count++ % 32] = rtt;
            stats.rtt_max_usecs = std::max(stats.rtt_max_usecs, rtt);
        }
    }
    // What this peer still needs from us (with 2 machines this is the ack).
    peer_ack = std::max(peer_ack, acks[cfg.local_player]);

    for (uint32_t i = 0; i < count; i++) {
        const uint32_t tick = first + i;
        if (tick < next_needed[player] || tick >= next_needed[player] + kRing) continue;
        Slot &s = remote[player][tick % kRing];
        if (s.tick == tick) continue;  // already have it
        s.tick = tick;
        s.pad = pads[i];
        highest[player] = std::max(highest[player], tick + 1);
        if (peer_away) {
            peer_away = false;  // its inputs flow again (even if BACK was lost)
        }
        const Used &u = used[player][tick % kRing];
        if (u.tick == tick && u.predicted && !PadEq(u.pad, pads[i])) {
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

void n64net_input::HandleSync(Reader &r, uint8_t count) {
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
        SyncEntry &slot = remote_sync[got[i].tick % kSyncRing];
        if (slot.tick == got[i].tick && slot.kind == got[i].kind) continue;  // seen
        slot = got[i];
        for (uint32_t j = 0; j < std::min(local_sync_count, kSyncRing); j++) {
            const SyncEntry &e = local_sync[j];
            if (e.tick == slot.tick && e.kind == slot.kind) {
                CompareSync(e, slot);
                break;
            }
        }
    }
}

extern "C" {

n64net_input *n64net_input_create(const n64net_input_config *config) {
    if (!config || config->num_players < 1 || config->num_players > N64NET_INPUT_MAX_PLAYERS ||
        config->local_player < 0 || config->local_player >= config->num_players) {
        return nullptr;
    }
    n64net_input *net = new n64net_input();
    net->cfg = *config;
    if (net->cfg.ticks_per_second == 0) net->cfg.ticks_per_second = 60;
    net->remote_mask = config->remote_mask ? config->remote_mask
                                           : (((1u << config->num_players) - 1) & ~(1u << config->local_player));
    net->remote_mask &= ~(1u << config->local_player);
    net->delay = config->input_delay;
    net->local.resize(kRing);
    for (int p = 0; p < N64NET_INPUT_MAX_PLAYERS; p++) {
        net->remote[p].resize(kRing);
        net->used[p].resize(kRing);
        net->next_needed[p] = config->start_tick;
    }
    net->peer_ack = config->start_tick;
    return net;
}

void n64net_input_destroy(n64net_input *net) { delete net; }

void n64net_input_set_local(n64net_input *net, uint32_t tick, n64net_pad pad) {
    const uint32_t target = tick + net->delay;
    if (!net->local_started) {
        // Ticks before the first sample lands are neutral on every machine.
        for (uint32_t t = net->cfg.start_tick; t < target; t++) net->StoreLocal(t, n64net_pad{0, 0, 0});
        net->local_started = true;
        net->local_last = target - 1;
    }
    if (target > net->local_last) {
        // A grown delay leaves a gap: repeat the last sample there.
        for (uint32_t t = net->local_last + 1; t < target; t++) net->StoreLocal(t, net->last_pad);
        net->StoreLocal(target, pad);
        net->local_last = target;
    }
    // else: a shrunk delay; that tick is already scheduled (and maybe sent): drop.
    net->last_pad = pad;
    net->last_sample_tick = tick;
    net->SendInputs();
}

int n64net_input_ready(n64net_input *net, uint32_t tick) {
    const uint32_t needed = net->MinNextNeeded();
    if (needed == kNoTick) return 1;  // no remote players
    // Confirmed up to needed - 1; ready if that is >= tick - window.
    return (uint64_t)needed + net->cfg.rollback_window >= (uint64_t)tick + 1 ? 1 : 0;
}

int n64net_input_get(n64net_input *net, int player, uint32_t tick, n64net_pad *out) {
    *out = n64net_pad{0, 0, 0};
    if (player < 0 || player >= net->cfg.num_players) return 0;
    if (player == net->cfg.local_player) {
        net->LocalAt(tick, out);  // neutral if not scheduled (before the first sample)
        return 0;
    }
    if (!net->IsRemote(player)) return 0;  // CPU / empty: neutral
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
    if (version != kVersion || session != net->cfg.session_id || sum != Fnv(data, size - 4) ||
        !net->IsRemote(player)) {
        net->stats.packets_dropped++;
        return 1;
    }
    r.end = data + size - 4;
    switch (type) {
    case kTypeInput:
        net->HandleInput(r, player, count);
        break;
    case kTypeSync:
        net->HandleSync(r, count);
        break;
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

int n64net_take_rollback(n64net_input *net, uint32_t *tick) {
    if (!net->rollback_pending) return 0;
    *tick = net->rollback_tick;
    net->rollback_pending = false;
    return 1;
}

uint32_t n64net_input_confirmed_tick(n64net_input *net) {
    uint32_t needed = net->MinNextNeeded();
    if (needed == kNoTick) needed = net->local_started ? net->local_last + 1 : net->cfg.start_tick;
    if (net->local_started) needed = std::min(needed, net->local_last + 1);
    else needed = std::min(needed, net->cfg.start_tick);
    return needed == 0 ? kNoTick : needed - 1;
}

void n64net_input_set_delay(n64net_input *net, uint32_t input_delay) {
    if (input_delay != net->delay) {
        net->Log("n64net input: input delay %u -> %u\n", net->delay, input_delay);
        net->delay = input_delay;
    }
}

int n64net_timesync_should_skip(n64net_input *net) {
    if (net->cfg.rollback_window == 0 || !net->remote_advantage_valid) return 0;
    // Twice the ticks we run ahead (our advantage minus the peer's), smoothed
    // over ~16 frames so jitter doesn't make both sides take turns.
    const int32_t ahead2 = net->LocalAdvantage() - net->remote_advantage;
    net->ahead16 += ahead2 - net->ahead16 / 16;
    if (net->timesync_cooldown != 0) {
        net->timesync_cooldown--;
        return 0;
    }
    if (net->ahead16 < 3 * 16) return 0;  // 1.5+ ticks ahead
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
    const SyncEntry &theirs = net->remote_sync[tick % kSyncRing];
    if (theirs.tick == tick && theirs.kind == kind) net->CompareSync(e, theirs);
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
    const uint32_t n = std::min(net->rtt_count, 32u);
    uint64_t sum = 0;
    for (uint32_t i = 0; i < n; i++) sum += net->rtt_samples[i];
    out->rtt_avg_usecs = n ? (uint32_t)(sum / n) : 0;
    out->local_advantage = net->LocalAdvantage();
    out->remote_advantage = net->remote_advantage;
}

int n64net_rollback_frame(n64net_input *net, uint32_t tick, const n64net_rollback_callbacks *cb) {
    const uint32_t n = cb->slot_count;
    if (net->slot_tick.size() != n) net->slot_tick.assign(n, kNoTick);
    int depth = 0;
    uint32_t from;
    if (n64net_take_rollback(net, &from) && from < tick) {
        if (net->slot_tick[from % n] != from) {
            net->Log("n64net input: cannot roll back to tick %u from %u: state too old\n", from, tick);
            depth = -1;
        } else {
            cb->load(cb->user, from % n);
            net->resimulating = 1;
            for (uint32_t t = from; t < tick; t++) {
                if (t != from) {
                    cb->save(cb->user, t % n);
                    net->slot_tick[t % n] = t;
                }
                cb->advance(cb->user);
            }
            net->resimulating = 0;
            depth = (int)(tick - from);
            net->stats.rollbacks++;
        }
    }
    cb->save(cb->user, tick % n);
    net->slot_tick[tick % n] = tick;
    return depth;
}

int n64net_rollback_resimulating(n64net_input *net) { return net->resimulating; }

} // extern "C"

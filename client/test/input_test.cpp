// n64net_input test: several machines (one with several local players), a lossy network with latency and
// jitter, a host event; every machine must simulate the same states and see the event at the same tick,
// in lockstep and with rollback.
//   clang++ -std=c++17 -O1 -Iclient/include client/src/n64net_input.cpp client/test/input_test.cpp -o input_test
//   ./input_test
#include "n64net_input.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>

namespace {

uint64_t g_now = 0;   // the simulated clock (usecs)
uint64_t Now() { return g_now; }

uint32_t g_rng = 12345;
uint32_t Rand() {   // xorshift: the network's randomness, the same every run
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

struct Packet {
    uint64_t at;
    int to;
    std::vector<uint8_t> data;
};
std::deque<Packet> g_wire;

struct Net {
    uint32_t latency_us, jitter_us, loss_pct;
} g_net;

constexpr int kMaxMachines = 4;
constexpr uint32_t kTicks = 1200;
constexpr uint32_t kSlots = 32;

struct Machine {
    int id = 0;
    uint32_t mask = 0;          // its players
    n64net_input *net = nullptr;
    uint32_t tick = 0;          // next tick to simulate
    bool sampled = false;       // set_local done for `tick`
    uint64_t state = 0;
    uint64_t hist[kTicks + 64] = {};
    uint64_t ring[kSlots] = {};
    int event_tick = -1;        // where it saw the host's event
    uint32_t rollbacks = 0;
};
Machine g_m[kMaxMachines];
int g_machines = 0;
int g_players = 0;

void SendFrom(void *user, const uint8_t *data, uint32_t size) {
    const int from = (int)(intptr_t)user;
    for (int to = 0; to < g_machines; to++) {
        if (to == from || Rand() % 100 < g_net.loss_pct) continue;
        const uint64_t at = g_now + g_net.latency_us + (g_net.jitter_us ? Rand() % g_net.jitter_us : 0);
        g_wire.push_back({ at, to, std::vector<uint8_t>(data, data + size) });
    }
}

// a player's controller: a function of (player, tick) only
n64net_pad PadOf(int player, uint32_t tick) {
    n64net_pad p = {};
    uint32_t h = (uint32_t)player * 2654435761u ^ (tick / 7) * 40503u;
    p.buttons = (uint16_t)(h & 0xF0F0);
    p.stick_x = (int8_t)(h >> 16);
    p.stick_y = (int8_t)(h >> 24);
    return p;
}

uint64_t Mix(uint64_t s, uint64_t v) { return (s ^ v) * 1099511628211ull + 0x9E37; }

// one game tick from the inputs in the ledger
void Step(Machine &m) {
    for (int p = 0; p < g_players; p++) {
        n64net_pad pad;
        n64net_input_get(m.net, p, m.tick, &pad);
        m.state = Mix(m.state, (uint64_t)pad.buttons << 16 | (uint8_t)pad.stick_x << 8 | (uint8_t)pad.stick_y);
        uint8_t ev[N64NET_INPUT_MAX_EVENT];
        const uint32_t n = n64net_input_get_event(m.net, p, m.tick, ev, sizeof(ev));
        if (n) {
            for (uint32_t k = 0; k < n; k++) m.state = Mix(m.state, ev[k] + 1000u * p);
        }
    }
    m.hist[m.tick] = m.state;
    m.tick++;
}

void Save(void *user, uint32_t slot) { Machine &m = *(Machine *)user; m.ring[slot] = m.state; }
void Load(void *user, uint32_t slot) {
    Machine &m = *(Machine *)user;
    m.state = m.ring[slot];
    m.tick = n64net_rollback_callback_tick(m.net);
    m.rollbacks++;
}
void Advance(void *user) { Step(*(Machine *)user); }

bool Run(const char *name, const uint32_t *masks, int machines, uint32_t window, uint32_t delay, Net net,
         uint32_t event_at, int bad_machine = -1) {
    g_now = 0;
    g_rng = 12345;
    g_wire.clear();
    g_net = net;
    g_machines = machines;
    g_players = 0;
    for (int i = 0; i < machines; i++) {
        for (int p = 0; p < 4; p++) if (masks[i] & (1u << p)) g_players = std::max(g_players, p + 1);
    }
    for (int i = 0; i < machines; i++) {
        Machine &m = g_m[i];
        m = Machine();
        m.id = i;
        m.mask = masks[i];
        n64net_input_config c = {};
        c.num_players = g_players;
        c.local_player = __builtin_ctz(masks[i]);
        c.local_mask = masks[i];
        c.session_id = 77;
        c.start_tick = 0;
        c.input_delay = delay;
        c.rollback_window = window;
        c.ticks_per_second = 60;
        c.send = SendFrom;
        c.user = (void *)(intptr_t)i;
        c.now_usecs = Now;
        m.net = n64net_input_create(&c);
        if (!m.net) { printf("%s: create failed\n", name); return false; }
    }
    const n64net_rollback_callbacks cb0 = { Save, Load, Advance, nullptr, kSlots };
    for (uint64_t step = 0; step < 200000; step++) {
        bool done = true;
        for (int i = 0; i < machines; i++) done &= g_m[i].tick >= kTicks;
        if (done) break;
        g_now += 1000;   // 1 ms per step; machines run a tick when 16.7 ms passed and they may
        // deliver
        for (auto it = g_wire.begin(); it != g_wire.end();) {
            if (it->at <= g_now) {
                n64net_input_handle_packet(g_m[it->to].net, it->data.data(), (uint32_t)it->data.size());
                it = g_wire.erase(it);
            } else {
                ++it;
            }
        }
        for (int i = 0; i < machines; i++) {
            Machine &m = g_m[i];
            if (m.tick >= kTicks || g_now < (uint64_t)m.tick * 16667 + (uint64_t)i * 3000) continue;
            if (!m.sampled) {
                n64net_rollback_callbacks cb = cb0;
                cb.user = &m;
                if (window > 0) n64net_rollback_frame(m.net, m.tick, &cb);
                n64net_pad pads[N64NET_INPUT_MAX_PLAYERS] = {};
                for (int p = 0; p < 4; p++) if (m.mask & (1u << p)) pads[p] = PadOf(p, m.tick);
                if (i == 0 && m.tick == event_at) {
                    const uint8_t ev[3] = { 'C', 'P', 3 };   // "player 3 becomes a CPU"
                    if (!n64net_input_set_event(m.net, ev, sizeof(ev))) { printf("%s: set_event refused\n", name); return false; }
                }
                n64net_input_set_local_pads(m.net, m.tick, pads);
                m.sampled = true;
            }
            if (!n64net_input_ready(m.net, m.tick)) {
                n64net_input_resend(m.net);
                continue;
            }
            Step(m);
            m.sampled = false;
        }
    }
    // a final rollback pass: every machine resimulates whatever late inputs arrived
    for (int round = 0; round < 50 && !g_wire.empty(); round++) {
        g_now += 100000;
        for (auto &pk : g_wire) n64net_input_handle_packet(g_m[pk.to].net, pk.data.data(), (uint32_t)pk.data.size());
        g_wire.clear();
        for (int i = 0; i < machines; i++) n64net_input_resend(g_m[i].net);
    }
    for (int i = 0; i < machines; i++) {
        Machine &m = g_m[i];
        if (window > 0) {
            n64net_rollback_callbacks cb = cb0;
            cb.user = &m;
            n64net_rollback_frame(m.net, m.tick, &cb);
        }
    }
    // compare: every tick confirmed everywhere must hold the same state on every machine
    uint32_t confirmed = kTicks;
    for (int i = 0; i < machines; i++) {
        uint32_t c = n64net_input_confirmed_tick(g_m[i].net);
        if (c == UINT32_MAX) c = 0;
        confirmed = std::min(confirmed, std::min(c, g_m[i].tick - 1));
    }
    bool ok = confirmed > kTicks / 2;
    uint32_t first_bad = UINT32_MAX;
    for (uint32_t t = 0; t <= confirmed && ok; t++) {
        for (int i = 1; i < machines; i++) {
            if (g_m[i].hist[t] != g_m[0].hist[t]) { first_bad = t; ok = false; break; }
        }
    }
    // the event: its state change happened at the same tick everywhere (states equal => yes); check it
    // happened at all: replay machine 0's inputs without the event would differ, so compare with a run
    // that has no event is overkill here; instead check event visibility on each machine at event tick + delay
    int seen = 0;
    for (int i = 0; i < machines; i++) {
        uint8_t ev[N64NET_INPUT_MAX_EVENT];
        if (n64net_input_get_event(g_m[i].net, 0, event_at + delay, ev, sizeof(ev)) == 3 && ev[2] == 3) seen++;
    }
    // sync checks every 60 confirmed ticks: every machine must see a wrong hash from any other machine
    // (bad_machine reports wrong ones), and none may see a desync when all agree
    for (uint32_t t = 60; t <= confirmed; t += 60) {
        for (int i = 0; i < machines; i++) {
            const uint32_t h = (uint32_t)g_m[i].hist[t] ^ (i == bad_machine ? 0x5A5Au : 0u);
            n64net_sync_record(g_m[i].net, t, 1, h);
        }
        for (auto &pk : g_wire) n64net_input_handle_packet(g_m[pk.to].net, pk.data.data(), (uint32_t)pk.data.size());
        g_wire.clear();
    }
    int detected = 0;
    for (int i = 0; i < machines; i++) detected += n64net_sync_desynced(g_m[i].net, nullptr, nullptr);
    const bool sync_ok = bad_machine < 0 ? detected == 0 : detected == machines;
    if (!sync_ok) printf("%s: sync check: %d of %d machines saw a desync (expected %d)\n", name, detected, machines,
                         bad_machine < 0 ? 0 : machines);
    ok = ok && sync_ok;
    uint32_t rb = 0;
    n64net_input_stats st;
    for (int i = 0; i < machines; i++) { n64net_input_get_stats(g_m[i].net, &st); rb += st.rollbacks; }
    printf("%-44s %s: %u ticks compared%s, event seen by %d/%d, rollbacks %u\n", name, ok && seen == machines ? "PASS" : "FAIL",
           confirmed + 1, first_bad != UINT32_MAX ? " (first difference at a tick)" : "", seen, machines, rb);
    for (int i = 0; i < machines; i++) n64net_input_destroy(g_m[i].net);
    return ok && seen == machines;
}

} // namespace

int main() {
    bool all = true;
    const uint32_t two[] = { 1u << 0, 1u << 1 };
    const uint32_t three_2local[] = { 1u << 0, (1u << 1) | (1u << 2), 1u << 3 };
    const uint32_t four[] = { 1u << 0, 1u << 1, 1u << 2, 1u << 3 };
    const uint32_t one_device_two_machines[] = { (1u << 0) | (1u << 1) | (1u << 2), 1u << 3 };
    const Net clean = { 20000, 0, 0 }, bad = { 60000, 30000, 5 }, cell = { 100000, 60000, 3 };
    all &= Run("2 machines, lockstep, clean", two, 2, 0, 2, clean, 300);
    all &= Run("2 machines, rollback, bad network", two, 2, 8, 2, bad, 300);
    all &= Run("3 machines (one with 2 players), lockstep", three_2local, 3, 0, 3, bad, 500);
    all &= Run("3 machines (one with 2 players), rollback", three_2local, 3, 8, 2, bad, 500);
    all &= Run("4 machines, rollback, cellular", four, 4, 8, 3, cell, 700);
    all &= Run("3 players on one machine + 1, rollback", one_device_two_machines, 2, 8, 2, bad, 400);
    all &= Run("4 machines, the last one desyncs (all must notice)", four, 4, 8, 2, bad, 300, 3);
    all &= Run("3 machines, the middle one desyncs", three_2local, 3, 8, 2, bad, 300, 1);
    printf("%s\n", all ? "ALL PASS" : "SOME FAILED");
    return all ? 0 : 1;
}

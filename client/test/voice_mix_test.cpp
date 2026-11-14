// Voice from several senders (n64net_voice_handle_packet_from): each sender gets its own jitter buffer and
// decoder, the mixer adds them up, and a sender's restart or silence doesn't disturb the others.
//   clang++ -std=c++17 -Iclient/include -Iclient/src -I<SDL2 include> -I<opus include> client/src/n64net_voice.cpp \
//       client/test/voice_mix_test.cpp <libopus.a> <libSDL2> -o voice_mix_test
//   SDL_AUDIODRIVER=dummy ./voice_mix_test
#include "n64net_voice.h"

#include <opus.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr int kRate = 48000, kFrame = 960;

struct Sender {
    OpusEncoder *enc = nullptr;
    uint16_t seq = 0;
    double phase = 0.0;
    double freq = 0.0;
};

// one 20 ms voice packet of a tone at `freq` (0 = silence), as n64net_voice sends them
std::vector<uint8_t> Packet(Sender &s) {
    int16_t pcm[kFrame];
    for (int i = 0; i < kFrame; i++) {
        pcm[i] = s.freq > 0 ? (int16_t)(8000.0 * std::sin(s.phase)) : 0;
        s.phase += 2.0 * M_PI * s.freq / kRate;
    }
    std::vector<uint8_t> p(N64NET_VOICE_MAX_PACKET);
    p[0] = 'N'; p[1] = '6'; p[2] = '4'; p[3] = 'V';
    p[4] = s.seq >> 8; p[5] = s.seq & 0xFF; p[6] = 0;
    s.seq++;
    const int n = opus_encode(s.enc, pcm, kFrame, p.data() + 7, N64NET_VOICE_MAX_PACKET - 7);
    p.resize(n > 0 ? 7 + n : 7);
    return p;
}

// energy of `out` at `freq` (Goertzel on the left channel)
double Power(const std::vector<int16_t> &stereo, double freq) {
    const int n = (int)stereo.size() / 2;
    const double w = 2.0 * M_PI * freq / kRate, c = 2.0 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (int i = 0; i < n; i++) {
        const double s0 = stereo[i * 2] / 32768.0 + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return (s1 * s1 + s2 * s2 - c * s1 * s2) / n;
}

}  // namespace

int main() {
    n64net_voice_config cfg = {};
    cfg.enabled = 1;
    cfg.test_tone = 1;   // no microphone
    n64net_voice_start(&cfg);
    if (!n64net_voice_is_running()) {
        printf("FAIL: voice didn't start\n");
        return 1;
    }
    Sender senders[3];
    const double freqs[3] = { 440.0, 0.0, 1250.0 };   // sender 1 is silent
    for (int i = 0; i < 3; i++) {
        int err = 0;
        senders[i].enc = opus_encoder_create(kRate, 1, OPUS_APPLICATION_VOIP, &err);
        senders[i].freq = freqs[i];
        senders[i].seq = (uint16_t)(1000 * i);   // unrelated sequence numbers per sender
    }
    senders[2].freq = 1250.0;
    std::vector<int16_t> out;
    bool ok = true;
    for (int frame = 0; frame < 150; frame++) {   // 3 s
        for (int i = 0; i < 3; i++) {
            if (i == 2 && frame == 0) senders[2].seq = 60000;   // a sender far away in sequence space
            std::vector<uint8_t> p = Packet(senders[i]);
            if (!n64net_voice_handle_packet_from(p.data(), (int32_t)p.size(), i)) ok = false;
        }
        std::vector<int16_t> block(kFrame * 2, 0);
        n64net_voice_mix(block.data(), kFrame, kRate);
        if (frame >= 50) out.insert(out.end(), block.begin(), block.end());   // after the start-up buffering
        if (frame == 100) {
            const int s0 = n64net_voice_remote_speaking_from(0), s1 = n64net_voice_remote_speaking_from(1),
                      s2 = n64net_voice_remote_speaking_from(2), s3 = n64net_voice_remote_speaking_from(3);
            printf("speaking: sender 0 %d, 1 %d, 2 %d, 3 %d (expected 1 0 1 0)\n", s0, s1, s2, s3);
            ok = ok && s0 == 1 && s1 == 0 && s2 == 1 && s3 == 0;
        }
    }
    const double p440 = Power(out, 440.0), p1250 = Power(out, 1250.0), p800 = Power(out, 800.0);
    printf("mix: 440 Hz %.5f, 1250 Hz %.5f, 800 Hz (neither) %.5f\n", p440, p1250, p800);
    ok = ok && p440 > 20 * p800 && p1250 > 20 * p800;
    // a packet from an out-of-range sender is consumed but ignored
    std::vector<uint8_t> p = Packet(senders[0]);
    ok = ok && n64net_voice_handle_packet_from(p.data(), (int32_t)p.size(), 7) == 1;
    n64net_voice_stop();
    for (Sender &s : senders) opus_encoder_destroy(s.enc);
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

// Runs a 48 kHz mono s16 WAV (e.g. an N64NET_VOICE_DUMP recording) through the
// send-side leveler and reports levels before/after:
//   n64net_voice_dsp_tool in.wav [out.wav] [pre-gain]
// pre-gain (default 1) scales the input first, e.g. 0.3 to simulate quieter speech.
#include "../client/src/n64net_voice_dsp.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static float Rms(const int16_t *p, int n) {
    double s = 0;
    for (int i = 0; i < n; i++) s += (p[i] / 32768.0) * (p[i] / 32768.0);
    return static_cast<float>(std::sqrt(s / n));
}

static float Pct(std::vector<float> v, float q) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[static_cast<size_t>(q * (v.size() - 1))];
}

int main(int argc, char **argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s in.wav [out.wav] [pre-gain]\n", argv[0]); return 2; }
    FILE *f = std::fopen(argv[1], "rb");
    if (!f) { std::perror(argv[1]); return 1; }
    std::vector<uint8_t> raw;
    uint8_t chunk[65536];
    size_t got;
    while ((got = std::fread(chunk, 1, sizeof chunk, f)) > 0) raw.insert(raw.end(), chunk, chunk + got);
    std::fclose(f);
    size_t pos = 12, data = 0, len = 0;
    while (pos + 8 <= raw.size()) {
        uint32_t sz;
        std::memcpy(&sz, &raw[pos + 4], 4);
        if (std::memcmp(&raw[pos], "data", 4) == 0) { data = pos + 8; len = std::min<size_t>(sz, raw.size() - data); break; }
        pos += 8 + sz;
    }
    std::vector<int16_t> pcm(len / 2);
    std::memcpy(pcm.data(), &raw[data], pcm.size() * 2);
    const float pre = argc > 3 ? std::strtof(argv[3], nullptr) : 1.0f;
    for (auto &s : pcm) s = static_cast<int16_t>(std::clamp(static_cast<int>(s * pre), -32768, 32767));
    while (!pcm.empty() && pcm.back() == 0) pcm.pop_back();  // unused tail of a dump

    n64net_dsp::VoiceLeveler lev;
    const int kFrame = 960;
    std::vector<float> inSpeech, outSpeech, inBg, outBg;
    int loudIn = 0, loudOut = 0, frames = 0, clipped = 0;
    std::vector<int16_t> out = pcm;
    for (size_t at = 0; at + kFrame <= out.size(); at += kFrame) {
        const float ri = Rms(&pcm[at], kFrame);
        const bool speech = lev.Process(&out[at], kFrame);
        const float ro = Rms(&out[at], kFrame);
        for (int i = 0; i < kFrame; i++) clipped += std::abs(out[at + i]) >= 32000;
        (speech ? inSpeech : inBg).push_back(ri);
        (speech ? outSpeech : outBg).push_back(ro);
        loudIn += ri > 0.015f;
        loudOut += ro > 0.015f;
        frames++;
    }
    std::printf("%s (pre-gain %.2f): %d frames, speech %.0f%%\n", argv[1], pre, frames, 100.0 * inSpeech.size() / std::max(frames, 1));
    std::printf("  speech RMS  in: median %.3f p90 %.3f | out: median %.3f p90 %.3f\n", Pct(inSpeech, .5f), Pct(inSpeech, .9f), Pct(outSpeech, .5f), Pct(outSpeech, .9f));
    std::printf("  background  in: median %.4f | out: median %.4f\n", Pct(inBg, .5f), Pct(outBg, .5f));
    std::printf("  frames above speaking level (0.015): in %.1f%%, out %.1f%%; samples near full scale: %d\n",
                100.0 * loudIn / std::max(frames, 1), 100.0 * loudOut / std::max(frames, 1), clipped);
    if (argc > 2) {
        FILE *o = std::fopen(argv[2], "wb");
        const uint32_t bytes = static_cast<uint32_t>(out.size() * 2), rate = 48000, br = 96000, sz = 36 + bytes, fmt = 16;
        const uint16_t pcmfmt = 1, ch = 1, align = 2, bits = 16;
        std::fwrite("RIFF", 1, 4, o); std::fwrite(&sz, 4, 1, o); std::fwrite("WAVEfmt ", 1, 8, o); std::fwrite(&fmt, 4, 1, o);
        std::fwrite(&pcmfmt, 2, 1, o); std::fwrite(&ch, 2, 1, o); std::fwrite(&rate, 4, 1, o); std::fwrite(&br, 4, 1, o);
        std::fwrite(&align, 2, 1, o); std::fwrite(&bits, 2, 1, o); std::fwrite("data", 1, 4, o); std::fwrite(&bytes, 4, 1, o);
        std::fwrite(out.data(), 2, out.size(), o);
        std::fclose(o);
    }
    return 0;
}

// n64net voice: send-side leveler (automatic gain + limiter) and receive-side
// ducking of the game's audio. Header-only so tests can run it on recordings.
//
// Leveler, per 20 ms mono frame at 48 kHz:
//  1. high-pass (~100 Hz) removes rumble and handling noise;
//  2. a noise-floor tracker tells speech frames from background;
//  3. automatic gain brings speech towards kTarget RMS (-20 dBFS): rises
//     slowly (~8 dB/s) and only on speech, falls faster, and is held in
//     pauses, so the start of the next phrase is already at the right level;
//     at most kMaxGain (+12 dB: more picked up keyboard and room noise);
//  3b. a noise gate: speech needs kSpeechMin RMS (about -44 dBFS before gain)
//     as well as 3x the noise floor; between phrases (after a 500 ms hold)
//     the signal fades to kGateFloor (-20 dB) over ~100 ms, and opens again
//     within one frame when speech starts;
//  4. a peak limiter (fast attack, ~80 ms release) plus a soft clip keep
//     shouts and plosives from distorting.
// Gain changes are interpolated across each frame (no zipper noise).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace n64net_dsp {

class VoiceLeveler {
public:
    static constexpr float kTarget = 0.10f;    // speech RMS after gain
    static constexpr float kMaxGain = 4.0f;    // +12 dB
    static constexpr float kMinGain = 0.5f;
    static constexpr float kNoiseGain = 1.0f;  // at most this much on background (before the gate)
    static constexpr float kSpeechMin = 0.006f; // -44 dBFS: quieter frames are never speech
    static constexpr float kGateFloor = 0.1f;   // -20 dB between phrases
    static constexpr float kLimit = 0.60f;     // limiter threshold (peak)

    void Reset() { *this = VoiceLeveler(); }

    // In place; returns true when the frame was classified as speech.
    bool Process(int16_t *pcm, int n) {
        // 1. high-pass, and the frame's RMS after it
        float buf[2048];
        n = std::min(n, 2048);
        double sum = 0.0;
        for (int i = 0; i < n; i++) {
            const float x = pcm[i] / 32768.0f;
            mHp = kHpA * (mHp + x - mHpPrev);
            mHpPrev = x;
            buf[i] = mHp;
            sum += static_cast<double>(mHp) * mHp;
        }
        const float rms = static_cast<float>(std::sqrt(sum / std::max(n, 1)));

        // 2. noise floor: follows quiet frames quickly, rises slowly (~3 dB/s)
        if (rms < mNoise) {
            mNoise = mNoise * 0.8f + rms * 0.2f;
        } else {
            mNoise *= 1.0072f;
        }
        mNoise = std::max(mNoise, 1e-4f);
        const bool speech = rms > std::max(mNoise * 3.0f, kSpeechMin);

        // 3. automatic gain
        if (speech) {
            const float want = std::clamp(kTarget / std::max(rms, 1e-5f), kMinGain, kMaxGain);
            if (want > mGain) {
                mGain = std::min(want, mGain * 1.02f);  // +0.17 dB per frame
            } else {
                mGain = std::max(want, mGain * 0.93f);  // -0.63 dB per frame
            }
            mHangover = 25;  // 500 ms
        } else if (mHangover > 0) {
            mHangover--;
        }
        const bool open = speech || mHangover > 0;
        // gate: open at once, close over ~100 ms (-4 dB per frame)
        mGate = open ? 1.0f : std::max(kGateFloor, mGate * 0.63f);
        const float frameGain = (open ? mGain : std::min(mGain, kNoiseGain)) * mGate;

        // 4. apply (gain ramped across the frame), limit, soft clip
        const float g0 = mLastGain, g1 = frameGain;
        for (int i = 0; i < n; i++) {
            const float g = g0 + (g1 - g0) * (static_cast<float>(i + 1) / n);
            float y = buf[i] * g;
            const float a = std::fabs(y);
            mEnv = a > mEnv ? a : mEnv * kRelease;
            if (mEnv > kLimit) y *= kLimit / mEnv;
            y = SoftClip(y);
            const int v = static_cast<int>(std::lrint(y * 32767.0f));
            pcm[i] = static_cast<int16_t>(std::clamp(v, -32768, 32767));
        }
        mLastGain = frameGain;
        return speech;
    }

    float Gain() const { return mGain; }
    float GateLevel() const { return mGate; }
    float NoiseFloor() const { return mNoise; }

private:
    static constexpr float kHpA = 0.98708f;     // one-pole high-pass, ~100 Hz at 48 kHz
    static constexpr float kRelease = 0.99974f; // limiter release, ~80 ms at 48 kHz
    static float SoftClip(float x) {
        // linear up to 0.8, then smoothly towards 1.0
        const float ax = std::fabs(x);
        if (ax <= 0.8f) return x;
        const float over = (ax - 0.8f) / 0.2f;
        const float y = 0.8f + 0.2f * (over / (1.0f + over));
        return x < 0 ? -y : y;
    }
    float mHp = 0.0f, mHpPrev = 0.0f;
    float mNoise = 0.01f;
    float mGain = 2.0f, mLastGain = 2.0f;
    float mGate = 1.0f;
    float mEnv = 0.0f;
    int mHangover = 0;
};

// Receive side: turn the game's audio down while the other player talks.
class Ducker {
public:
    static constexpr float kDucked = 0.5f;  // -6 dB
    // Per output sample: `talking` = the other player's voice is audible now.
    float Next(bool talking, int rate) {
        const float target = talking ? kDucked : 1.0f;
        // ~40 ms down, ~400 ms back up
        const float step = (target < mLevel ? 1.0f / (0.04f * rate) : 1.0f / (0.4f * rate)) * (1.0f - kDucked);
        if (mLevel > target) mLevel = std::max(target, mLevel - step);
        else if (mLevel < target) mLevel = std::min(target, mLevel + step);
        return mLevel;
    }
    void Reset() { mLevel = 1.0f; }

private:
    float mLevel = 1.0f;
};

} // namespace n64net_dsp

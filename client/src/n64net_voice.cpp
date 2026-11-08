/* n64net voice chat (see n64net_voice.h).
 *
 * Capture: an SDL capture device (48 kHz mono s16). Its callback cuts 20 ms
 * frames, encodes them with Opus (VOIP mode, DTX, in-band FEC) and queues the
 * packets; n64net_voice_pump() sends them from the game thread, so the game's
 * socket is only ever used from one thread.
 * Playback: mixed into the game's own audio output (n64net_voice_mix), since
 * some platforms (iOS) allow only one output device. Received packets go into
 * a small jitter buffer indexed by sequence number; the mixer starts once
 * ~60 ms is buffered, decodes in order, conceals a lost frame (FEC from the
 * next packet, else Opus PLC), skips ahead if the buffer grows past ~240 ms,
 * and resamples 48 kHz to the game's rate (linear). */
#include "n64net_voice.h"

#include <SDL2/SDL.h>
#include <opus.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

namespace {

constexpr int kRate = 48000;
constexpr int kFrame = 960;  // 20 ms
constexpr int kBitrate = 24000;
constexpr int kHeader = 7;
constexpr int kMaxPayload = N64NET_VOICE_MAX_PACKET - kHeader;
constexpr int kSlots = 64;
constexpr int kStartFrames = 3;    // buffer this much before playing
constexpr int kTrimBuffered = 6;   // more than this: drop one frame per frame played (catch up gently)
constexpr int kMaxBuffered = 25;   // more than this: jump ahead
constexpr int kMaxOutgoing = 16;
constexpr float kSpeakingRms = 0.015f;

struct Slot {
    bool full = false;
    uint16_t seq = 0;
    uint8_t len = 0;
    uint8_t data[kMaxPayload];
};

struct Packet {
    uint8_t len;
    uint8_t data[N64NET_VOICE_MAX_PACKET];
};

std::mutex sLock;  // guards sSlots/sHighest/sHaveAny and sOutgoing
bool sRunning = false;
n64net_voice_config sConfig = {};
SDL_AudioDeviceID sCaptureDev = 0;
OpusEncoder *sEncoder = nullptr;
OpusDecoder *sDecoder = nullptr;  // mixer only

// capture side (capture callback, or the pump in tone mode)
int16_t sCapBuf[kFrame];
int sCapFill = 0;
uint16_t sSendSeq = 0;
double sTonePhase = 0.0;
std::chrono::steady_clock::time_point sToneClock;
uint64_t sToneFrames = 0;
std::deque<Packet> sOutgoing;

// playback side
Slot sSlots[kSlots];
uint16_t sHighest = 0;
bool sHaveAny = false;
uint16_t sNextSeq = 0;  // mixer only
bool sPlaying = false;  // mixer only
int16_t sPcm[kFrame];
int sPcmPos = kFrame;
float sResPrev = 0.0f, sResCur = 0.0f;  // resampler: two 48 kHz samples
double sResPhase = 0.0;
constexpr float kVoiceGain = 1.4f;

std::atomic<int> sMuted{0};
std::atomic<int> sTalking{0};
std::atomic<int64_t> sRemoteSpeakUntilMs{0};
std::atomic<int64_t> sLocalSpeakUntilMs{0};
// stats (logged from the pump)
std::atomic<uint32_t> sSent{0}, sRecv{0}, sLost{0}, sFec{0}, sLate{0}, sUnderruns{0}, sSkips{0}, sDropped{0}, sTrims{0};
std::atomic<uint32_t> sAudibleFrames{0};
uint32_t sPumpCalls = 0;

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

template <typename... Args> void Log(const char *fmt, Args... args) {
    if (sConfig.log != nullptr) {
        sConfig.log(fmt, args...);
    }
}

bool Transmitting() {
    return sConfig.push_to_talk ? (sTalking.load() != 0) : (sMuted.load() == 0);
}

float Rms(const int16_t *pcm, int n) {
    double sum = 0.0;
    for (int i = 0; i < n; i++) {
        double v = pcm[i] / 32768.0;
        sum += v * v;
    }
    return static_cast<float>(std::sqrt(sum / n));
}

// One 20 ms frame of local audio -> a queued packet (when transmitting).
void EncodeFrame(const int16_t *pcm) {
    if (!Transmitting() || sEncoder == nullptr) {
        return;
    }
    Packet p;
    int n = opus_encode(sEncoder, pcm, kFrame, p.data + kHeader, kMaxPayload);
    if (n < 0) {
        return;
    }
    if (Rms(pcm, kFrame) > kSpeakingRms) {
        sLocalSpeakUntilMs = NowMs() + 250;
    }
    uint16_t seq = sSendSeq++;
    p.data[0] = (N64NET_VOICE_MAGIC >> 24) & 0xFF;
    p.data[1] = (N64NET_VOICE_MAGIC >> 16) & 0xFF;
    p.data[2] = (N64NET_VOICE_MAGIC >> 8) & 0xFF;
    p.data[3] = N64NET_VOICE_MAGIC & 0xFF;
    p.data[4] = seq >> 8;
    p.data[5] = seq & 0xFF;
    p.data[6] = 0;
    p.len = static_cast<uint8_t>(kHeader + n);
    std::lock_guard<std::mutex> lock(sLock);
    if (sOutgoing.size() >= kMaxOutgoing) {
        sOutgoing.pop_front();  // the game thread stalled: keep it recent
        sDropped++;
    }
    sOutgoing.push_back(p);
}

void SDLCALL CaptureCallback(void *, Uint8 *stream, int len) {
    const int16_t *in = reinterpret_cast<const int16_t *>(stream);
    int count = len / 2;
    while (count > 0) {
        int take = std::min(count, kFrame - sCapFill);
        std::memcpy(sCapBuf + sCapFill, in, take * 2);
        sCapFill += take;
        in += take;
        count -= take;
        if (sCapFill == kFrame) {
            EncodeFrame(sCapBuf);
            sCapFill = 0;
        }
    }
}

// Tests: a 440 Hz tone, one second on, one second off (exercises DTX).
void GenerateToneFrames() {
    auto elapsed = std::chrono::steady_clock::now() - sToneClock;
    uint64_t due = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() / 20;
    while (sToneFrames < due) {
        int16_t pcm[kFrame];
        bool on = ((sToneFrames / 50) % 2) == 0;
        for (int i = 0; i < kFrame; i++) {
            pcm[i] = on ? static_cast<int16_t>(8000.0 * std::sin(sTonePhase)) : 0;
            sTonePhase += 2.0 * M_PI * 440.0 / kRate;
        }
        sTonePhase = std::fmod(sTonePhase, 2.0 * M_PI);
        EncodeFrame(pcm);
        sToneFrames++;
    }
}

// Mixer: the next 20 ms of the other player's voice.
void DecodeNext() {
    Slot cur, next;
    bool have = false, haveNext = false;
    {
        std::lock_guard<std::mutex> lock(sLock);
        int buffered = sHaveAny ? static_cast<int16_t>(sHighest - sNextSeq) + 1 : 0;
        if (!sPlaying) {
            if (buffered < kStartFrames) {
                std::memset(sPcm, 0, sizeof(sPcm));
                return;
            }
            sPlaying = true;
        }
        if (buffered > kMaxBuffered) {
            sNextSeq = static_cast<uint16_t>(sHighest - kStartFrames + 1);  // far behind (a long stall)
            sSkips++;
        } else if (buffered > kTrimBuffered) {
            // A little behind (the game's audio paused for a moment): drop
            // the oldest frame, preferring silence (DTX) frames.
            Slot &old = sSlots[sNextSeq % kSlots];
            if (!old.full || old.seq != sNextSeq || old.len <= 3 || buffered > kTrimBuffered + 4) {
                old.full = false;
                sNextSeq++;
                sTrims++;
            }
        } else if (buffered <= 0) {
            sPlaying = false;  // ran dry: rebuffer
            sUnderruns++;
            std::memset(sPcm, 0, sizeof(sPcm));
            return;
        }
        Slot &s = sSlots[sNextSeq % kSlots];
        if (s.full && s.seq == sNextSeq) {
            cur = s;
            have = true;
            s.full = false;
        } else {
            Slot &n = sSlots[(sNextSeq + 1) % kSlots];
            if (n.full && n.seq == static_cast<uint16_t>(sNextSeq + 1)) {
                next = n;
                haveNext = true;
            }
        }
        sNextSeq++;
    }
    int n;
    if (have) {
        n = opus_decode(sDecoder, cur.data, cur.len, sPcm, kFrame, 0);
    } else {
        sLost++;
        if (haveNext) {
            sFec++;
            n = opus_decode(sDecoder, next.data, next.len, sPcm, kFrame, 1);  // in-band FEC
        } else {
            n = opus_decode(sDecoder, nullptr, 0, sPcm, kFrame, 0);  // concealment
        }
    }
    if (n != kFrame) {
        std::memset(sPcm, 0, sizeof(sPcm));
    }
    if (Rms(sPcm, kFrame) > kSpeakingRms) {
        sRemoteSpeakUntilMs = NowMs() + 250;
        sAudibleFrames++;
    }
}

float Pull48k() {
    if (sPcmPos == kFrame) {
        DecodeNext();
        sPcmPos = 0;
    }
    return static_cast<float>(sPcm[sPcmPos++]);
}

SDL_AudioDeviceID OpenMicrophone() {
    SDL_AudioSpec want = {}, have = {};
    want.freq = kRate;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = kFrame;
    want.callback = CaptureCallback;
    // No allowed changes: SDL converts from the hardware format for us.
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(nullptr, 1, &want, &have, 0);
    if (dev == 0) {
        Log("n64net voice: can't open the microphone (%s)\n", SDL_GetError());
    }
    return dev;
}

} // namespace

extern "C" void n64net_voice_start(const n64net_voice_config *config) {
    if (sRunning || config == nullptr || !config->enabled) {
        return;
    }
    sConfig = *config;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        Log("n64net voice: no audio (%s)\n", SDL_GetError());
        return;
    }
    int err = 0;
    sEncoder = opus_encoder_create(kRate, 1, OPUS_APPLICATION_VOIP, &err);
    sDecoder = opus_decoder_create(kRate, 1, &err);
    if (sEncoder == nullptr || sDecoder == nullptr) {
        Log("n64net voice: Opus setup failed\n");
        n64net_voice_stop();
        return;
    }
    opus_encoder_ctl(sEncoder, OPUS_SET_BITRATE(kBitrate));
    opus_encoder_ctl(sEncoder, OPUS_SET_DTX(1));
    opus_encoder_ctl(sEncoder, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(sEncoder, OPUS_SET_PACKET_LOSS_PERC(10));
    opus_encoder_ctl(sEncoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

    {
        std::lock_guard<std::mutex> lock(sLock);
        for (Slot &s : sSlots) {
            s.full = false;
        }
        sHaveAny = false;
        sOutgoing.clear();
    }
    sPlaying = false;
    sPcmPos = kFrame;
    sResPrev = sResCur = 0.0f;
    sResPhase = 0.0;
    sCapFill = 0;
    sSendSeq = 0;
    sToneFrames = 0;
    sToneClock = std::chrono::steady_clock::now();
    sSent = sRecv = sLost = sFec = sLate = sUnderruns = sSkips = sDropped = sTrims = sAudibleFrames = 0;
    sPumpCalls = 0;
    sRunning = true;  // before the devices start calling back

    if (!sConfig.test_tone) {
        sCaptureDev = OpenMicrophone();
    }
    if (sCaptureDev != 0) {
        SDL_PauseAudioDevice(sCaptureDev, 0);
    }
    Log("n64net voice: on (%s, %s)\n",
        sConfig.test_tone ? "test tone" : (sCaptureDev != 0 ? "microphone" : "NO microphone"),
        sConfig.push_to_talk ? "push to talk" : "open mic");
}

extern "C" void n64net_voice_stop(void) {
    if (sCaptureDev != 0) {
        SDL_CloseAudioDevice(sCaptureDev);  // waits for the callback to finish
        sCaptureDev = 0;
    }
    if (sEncoder != nullptr) {
        opus_encoder_destroy(sEncoder);
        sEncoder = nullptr;
    }
    if (sDecoder != nullptr) {
        opus_decoder_destroy(sDecoder);
        sDecoder = nullptr;
    }
    if (sRunning) {
        if (sConfig.log != nullptr) {
            sConfig.log("n64net voice: off (sent %u, received %u, lost %u)\n", sSent.load(), sRecv.load(),
                        sLost.load());
        }
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    sRunning = false;
}

extern "C" void n64net_voice_mix(int16_t *stereo, int frames, int rate) {
    if (!sRunning || sDecoder == nullptr || stereo == nullptr || rate <= 0) {
        return;
    }
    const double step = static_cast<double>(kRate) / rate;
    for (int i = 0; i < frames; i++) {
        float v = (sResPrev + (sResCur - sResPrev) * static_cast<float>(sResPhase)) * kVoiceGain;
        sResPhase += step;
        while (sResPhase >= 1.0) {
            sResPhase -= 1.0;
            sResPrev = sResCur;
            sResCur = Pull48k();
        }
        for (int ch = 0; ch < 2; ch++) {
            int s = stereo[i * 2 + ch] + static_cast<int>(v);
            stereo[i * 2 + ch] = static_cast<int16_t>(s > 32767 ? 32767 : (s < -32768 ? -32768 : s));
        }
    }
}

extern "C" int n64net_voice_is_running(void) {
    return sRunning ? 1 : 0;
}

extern "C" void n64net_voice_pump(n64net_voice_send_fn send) {
    if (!sRunning || send == nullptr) {
        return;
    }
    if (sConfig.test_tone) {
        GenerateToneFrames();
    }
    Packet batch[kMaxOutgoing];
    int count = 0;
    {
        std::lock_guard<std::mutex> lock(sLock);
        while (!sOutgoing.empty() && count < kMaxOutgoing) {
            batch[count++] = sOutgoing.front();
            sOutgoing.pop_front();
        }
    }
    for (int i = 0; i < count; i++) {
        send(batch[i].data, batch[i].len);
    }
    sSent += count;

    if ((++sPumpCalls % 600) == 0) {  // ~10 s at 60 calls/s
        Log("n64net voice: sent %u received %u lost %u (fec %u) late %u underruns %u trims %u skips %u "
                    "dropped %u audible %u\n",
                    sSent.load(), sRecv.load(), sLost.load(), sFec.load(), sLate.load(), sUnderruns.load(),
                    sTrims.load(), sSkips.load(), sDropped.load(), sAudibleFrames.load());
    }
}

extern "C" int n64net_voice_handle_packet(const uint8_t *data, int32_t size) {
    if (size < kHeader || data[0] != ((N64NET_VOICE_MAGIC >> 24) & 0xFF) ||
        data[1] != ((N64NET_VOICE_MAGIC >> 16) & 0xFF) || data[2] != ((N64NET_VOICE_MAGIC >> 8) & 0xFF) ||
        data[3] != (N64NET_VOICE_MAGIC & 0xFF)) {
        return 0;
    }
    if (!sRunning || size > N64NET_VOICE_MAX_PACKET) {
        return 1;  // voice, but we're not playing it
    }
    uint16_t seq = static_cast<uint16_t>((data[4] << 8) | data[5]);
    std::lock_guard<std::mutex> lock(sLock);
    if (sHaveAny && sPlaying && static_cast<int16_t>(seq - sNextSeq) < 0) {
        sLate++;  // its turn has passed
        return 1;
    }
    if (!sHaveAny || static_cast<int16_t>(seq - sHighest) > 0) {
        if (!sHaveAny || static_cast<int16_t>(seq - sHighest) > kSlots / 2) {
            sNextSeq = seq;  // first packet, or the sender restarted
            sPlaying = false;
        }
        sHighest = seq;
        sHaveAny = true;
    }
    Slot &s = sSlots[seq % kSlots];
    s.full = true;
    s.seq = seq;
    s.len = static_cast<uint8_t>(size - kHeader);
    std::memcpy(s.data, data + kHeader, size - kHeader);
    sRecv++;
    return 1;
}

extern "C" void n64net_voice_set_muted(int muted) {
    sMuted = muted ? 1 : 0;
}
extern "C" int n64net_voice_is_muted(void) {
    return sMuted.load();
}
extern "C" void n64net_voice_set_talking(int talking) {
    sTalking = talking ? 1 : 0;
}
extern "C" int n64net_voice_is_push_to_talk(void) {
    return sConfig.push_to_talk ? 1 : 0;
}
extern "C" int n64net_voice_remote_speaking(void) {
    return (sRunning && NowMs() < sRemoteSpeakUntilMs.load()) ? 1 : 0;
}
extern "C" int n64net_voice_local_speaking(void) {
    return (sRunning && Transmitting() && NowMs() < sLocalSpeakUntilMs.load()) ? 1 : 0;
}

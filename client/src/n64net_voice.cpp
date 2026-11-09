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
#include "n64net_voice_dsp.h"

#if __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#else
#include <SDL.h>
#endif
#include <opus.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

#if N64NET_VOICE_IOS
extern "C" int n64net_voice_ios_enable_voice_processing(int processing, char *error, unsigned error_size);  // n64net_voice_ios.mm
extern "C" void n64net_voice_ios_describe_route(char *out, unsigned out_size);
extern "C" void n64net_voice_ios_documents_path(const char *name, char *out, unsigned out_size);
// n64net_voice_vpio_ios.mm
extern "C" int n64net_vpio_start(void (*capture)(const int16_t *mono, int frames), char *error, unsigned error_size);
extern "C" void n64net_vpio_stop(void);
extern "C" int n64net_vpio_active(void);
extern "C" void n64net_vpio_write(const int16_t *stereo, int frames, int rate);
extern "C" int n64net_vpio_buffered(int rate);
extern "C" int n64net_vpio_check(char *note, unsigned note_size);
extern "C" void n64net_vpio_stats(char *out, unsigned out_size);
#endif

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
// Guards the mixer (decoder, jitter and resampler state): n64net_voice_mix
// may run on the game's audio thread (or two threads, e.g. a stall
// keep-alive), while start/stop run on the game thread. Lock order: sMixLock,
// then sLock.
std::mutex sMixLock;
std::atomic<bool> sRunning{false};
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
// Send side: speech is levelled before encoding (quiet talking at arm's
// length comes out like a raised voice). Receive side: the game's audio dips
// while the other player talks. Debug: N64NET_VOICE_LEVELER=0, N64NET_VOICE_DUCK=0.
n64net_dsp::VoiceLeveler sLeveler;
n64net_dsp::Ducker sDucker;
bool sLevelerOn = true, sDuckOn = true;
std::atomic<uint32_t> sSentLoud{0};

std::atomic<int> sMuted{0};
std::atomic<int> sTalking{0};
std::atomic<int64_t> sRemoteSpeakUntilMs{0};
std::atomic<int64_t> sLocalSpeakUntilMs{0};
// stats (logged from the pump)
std::atomic<uint32_t> sSent{0}, sRecv{0}, sLost{0}, sFec{0}, sLate{0}, sUnderruns{0}, sSkips{0}, sDropped{0}, sTrims{0};
std::atomic<uint32_t> sAudibleFrames{0};
// Microphone health: frames the capture callback delivered, how many were
// above the speaking level, and the loudest frame (RMS x 1000).
std::atomic<uint32_t> sMicFrames{0}, sMicLoud{0}, sMicPeak{0};
uint64_t sMicCheckMs = 0;   // when to check that the microphone delivers audio
int sMicReopens = 0;
uint32_t sPumpCalls = 0;
// Debug: N64NET_VOICE_DUMP=<file> records the first 90 s of microphone audio
// (before encoding) to a WAV file; relative names go to Documents on iOS.
constexpr int kDumpFrames = 90 * 50;
int16_t *sDumpBuf = nullptr;
std::atomic<int> sDumpCount{0};
int sDumpWritten = 0;
char sDumpPath[1024];

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

void WriteDump() {
    const int frames = sDumpCount.load();
    if (sDumpBuf == nullptr || frames == sDumpWritten) return;
    FILE *f = std::fopen(sDumpPath, "wb");
    if (f == nullptr) return;
    const uint32_t bytes = static_cast<uint32_t>(frames) * kFrame * 2;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); u32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(1); u16(1); u32(kRate); u32(kRate * 2); u16(2); u16(16);
    std::fwrite("data", 1, 4, f); u32(bytes);
    std::fwrite(sDumpBuf, 2, static_cast<size_t>(frames) * kFrame, f);
    std::fclose(f);
    sDumpWritten = frames;
}

// One 20 ms frame of local audio -> a queued packet (when transmitting).
void EncodeFrame(const int16_t *pcm, bool from_mic = true) {
    if (from_mic) {
        const uint32_t level = static_cast<uint32_t>(Rms(pcm, kFrame) * 1000.0f);
        sMicFrames++;
        if (level > static_cast<uint32_t>(kSpeakingRms * 1000.0f)) sMicLoud++;
        if (level > sMicPeak.load()) sMicPeak = level;
        const int d = sDumpCount.load();
        if (sDumpBuf != nullptr && d < kDumpFrames) {
            std::memcpy(sDumpBuf + static_cast<size_t>(d) * kFrame, pcm, kFrame * 2);
            sDumpCount = d + 1;
        }
        if (sConfig.test_tone) return;  // tests: the mic is only measured, the tone is sent
    }
    int16_t leveled[kFrame];
    if (from_mic) {
        std::memcpy(leveled, pcm, sizeof leveled);
        if (sLevelerOn) sLeveler.Process(leveled, kFrame);  // also while muted, so the gain stays current
        pcm = leveled;
    }
    if (!Transmitting() || sEncoder == nullptr) {
        return;
    }
    if (Rms(pcm, kFrame) > kSpeakingRms) sSentLoud++;
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

void FeedCapture(const int16_t *in, int count) {
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

void SDLCALL CaptureCallback(void *, Uint8 *stream, int len) {
    FeedCapture(reinterpret_cast<const int16_t *>(stream), len / 2);
}

#if N64NET_VOICE_IOS
bool sUsingUnit = false;  // the voice-processing unit records the mic and plays the game
char sPrevCategoryHint[96];
#endif
bool MicOpen() {
#if N64NET_VOICE_IOS
    if (sUsingUnit) return true;
#endif
    return sCaptureDev != 0;
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
        EncodeFrame(pcm, false);
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
#if N64NET_VOICE_IOS
    // Plain SDL mic (no voice-processing unit): loudspeaker, but no voice-chat
    // mode, which without the unit turns the player's own voice down ~20 dB.
    char error[160];
    if (dev != 0 && !n64net_voice_ios_enable_voice_processing(0, error, sizeof error)) {
        Log("n64net voice: audio session not set (%s)\n", error);
    }
#endif
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
    OpusDecoder *decoder = opus_decoder_create(kRate, 1, &err);
    if (sEncoder == nullptr || decoder == nullptr) {
        Log("n64net voice: Opus setup failed\n");
        if (decoder != nullptr) opus_decoder_destroy(decoder);
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
    {
        std::lock_guard<std::mutex> mix(sMixLock);
        sDecoder = decoder;
        sPlaying = false;
        sDucker.Reset();
        sPcmPos = kFrame;
        sResPrev = sResCur = 0.0f;
        sResPhase = 0.0;
    }
    sCapFill = 0;
    sSendSeq = 0;
    sToneFrames = 0;
    sToneClock = std::chrono::steady_clock::now();
    sSent = sRecv = sLost = sFec = sLate = sUnderruns = sSkips = sDropped = sTrims = sAudibleFrames = 0;
    sMicFrames = sMicLoud = sMicPeak = 0;
    sSentLoud = 0;
    sLeveler.Reset();
    {
        const char *l = getenv("N64NET_VOICE_LEVELER"), *d = getenv("N64NET_VOICE_DUCK");
        sLevelerOn = !(l != nullptr && l[0] == '0');
        sDuckOn = !(d != nullptr && d[0] == '0');
    }
    sMicReopens = 0;
    sMicCheckMs = NowMs() + 2000;
    if (const char *dump = getenv("N64NET_VOICE_DUMP"); dump != nullptr && dump[0] != 0 && sDumpBuf == nullptr) {
#if N64NET_VOICE_IOS
        if (dump[0] != '/') n64net_voice_ios_documents_path(dump, sDumpPath, sizeof sDumpPath);
        else
#endif
        snprintf(sDumpPath, sizeof sDumpPath, "%s", dump);
        sDumpBuf = static_cast<int16_t *>(std::calloc(static_cast<size_t>(kDumpFrames) * kFrame, 2));
        sDumpCount = 0;
        sDumpWritten = 0;
        Log("n64net voice: recording the microphone to %s\n", sDumpPath);
    }
    sPumpCalls = 0;
    sRunning = true;  // before the devices start calling back

#if N64NET_VOICE_IOS
    // Echo cancellation: the voice-processing unit records the mic and plays
    // the game's audio. N64NET_VOICE_PROCESSING=0 (debug) uses the plain SDL
    // mic instead; N64NET_VOICE_UNIT=1 also starts the unit in test-tone mode
    // (the mic is then only measured, e.g. to check what echo is left).
    const char *proc = getenv("N64NET_VOICE_PROCESSING");
    const char *unit = getenv("N64NET_VOICE_UNIT");
    const bool want_unit = !(proc != nullptr && proc[0] == '0') &&
                           (!sConfig.test_tone || (unit != nullptr && unit[0] == '1'));
    sUsingUnit = false;
    if (want_unit) {
        char error[160];
        const char *prev = SDL_GetHint(SDL_HINT_AUDIO_CATEGORY);
        snprintf(sPrevCategoryHint, sizeof sPrevCategoryHint, "%s", prev != nullptr ? prev : "");
        SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, "AVAudioSessionCategoryPlayAndRecord");  // SDL must not undo it
        if (n64net_vpio_start(FeedCapture, error, sizeof error)) {
            sUsingUnit = true;
        } else {
            Log("n64net voice: no echo cancellation (%s); using the plain microphone\n", error);
        }
    }
#endif
    if (!sConfig.test_tone && !MicOpen()) {
        sCaptureDev = OpenMicrophone();
    }
    if (sCaptureDev != 0) {
        SDL_PauseAudioDevice(sCaptureDev, 0);
    }
#if N64NET_VOICE_IOS
    {
        char route[512];
        n64net_voice_ios_describe_route(route, sizeof route);
        Log("n64net voice: audio route at start (%s): %s\n",
            sUsingUnit ? "echo-cancelling unit plays the game" : "no echo cancellation", route);
    }
#endif
    Log("n64net voice: on (%s, %s)\n",
        sConfig.test_tone ? "test tone" : (MicOpen() ? "microphone" : "NO microphone"),
        sConfig.push_to_talk ? "push to talk" : "open mic");
}

extern "C" void n64net_voice_stop(void) {
#if N64NET_VOICE_IOS
    if (sUsingUnit) {
        n64net_vpio_stop();  // the game's audio goes back to its own device, session restored
        sUsingUnit = false;
        SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, sPrevCategoryHint[0] != 0 ? sPrevCategoryHint : nullptr);
        char route[512];
        n64net_voice_ios_describe_route(route, sizeof route);
        Log("n64net voice: audio route after stopping: %s\n", route);
    }
#endif
    if (sCaptureDev != 0) {
        SDL_CloseAudioDevice(sCaptureDev);  // waits for the callback to finish
        sCaptureDev = 0;
    }
    if (sEncoder != nullptr) {
        opus_encoder_destroy(sEncoder);
        sEncoder = nullptr;
    }
    bool was_running;
    {
        std::lock_guard<std::mutex> mix(sMixLock);  // waits for a mix in progress (audio thread)
        was_running = sRunning.exchange(false);
        if (sDecoder != nullptr) {
            opus_decoder_destroy(sDecoder);
            sDecoder = nullptr;
        }
    }
    if (was_running) {
        if (sConfig.log != nullptr) {
            sConfig.log("n64net voice: off (sent %u, received %u, lost %u)\n", sSent.load(), sRecv.load(),
                        sLost.load());
        }
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
}

extern "C" void n64net_voice_mix(int16_t *stereo, int frames, int rate) {
    std::lock_guard<std::mutex> mix(sMixLock);
    if (!sRunning || sDecoder == nullptr || stereo == nullptr || rate <= 0) {
        return;
    }
    const double step = static_cast<double>(kRate) / rate;
    const bool talking = sDuckOn && NowMs() < sRemoteSpeakUntilMs.load();
    for (int i = 0; i < frames; i++) {
        const float duck = sDucker.Next(talking, rate);
        float v = (sResPrev + (sResCur - sResPrev) * static_cast<float>(sResPhase)) * kVoiceGain;
        sResPhase += step;
        while (sResPhase >= 1.0) {
            sResPhase -= 1.0;
            sResPrev = sResCur;
            sResCur = Pull48k();
        }
        for (int ch = 0; ch < 2; ch++) {
            int s = static_cast<int>(stereo[i * 2 + ch] * duck) + static_cast<int>(v);
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
#if N64NET_VOICE_IOS
    if (sUsingUnit) {
        char note[256];
        if (n64net_vpio_check(note, sizeof note)) Log("n64net voice: %s\n", note);
    }
#endif
    if (!sConfig.test_tone && sCaptureDev != 0 && sMicCheckMs != 0 && NowMs() >= sMicCheckMs) {
        // A microphone that opened but never calls back (e.g. its audio queue
        // was paused when the app's audio session changed): reopen it.
        if (sMicFrames.load() > 0) {
            sMicCheckMs = 0;
        } else if (sMicReopens < 3) {
            sMicReopens++;
            Log("n64net voice: the microphone delivers no audio; reopening it (try %d)\n", sMicReopens);
            SDL_CloseAudioDevice(sCaptureDev);
            sCaptureDev = OpenMicrophone();
            if (sCaptureDev != 0) SDL_PauseAudioDevice(sCaptureDev, 0);
            sMicCheckMs = NowMs() + 2000;
        } else {
            Log("n64net voice: the microphone still delivers no audio; giving up\n");
            sMicCheckMs = 0;
        }
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
                    "dropped %u audible %u mic %u (loud %u, peak %.3f) sent loud %u gain %.1f\n",
                    sSent.load(), sRecv.load(), sLost.load(), sFec.load(), sLate.load(), sUnderruns.load(),
                    sTrims.load(), sSkips.load(), sDropped.load(), sAudibleFrames.load(), sMicFrames.load(),
                    sMicLoud.load(), sMicPeak.load() / 1000.0, sSentLoud.load(), sLeveler.Gain());
#if N64NET_VOICE_IOS
        if (sUsingUnit) {
            char st[256];
            n64net_vpio_stats(st, sizeof st);
            Log("n64net voice: %s\n", st);
        }
#endif
        WriteDump();
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

extern "C" int n64net_voice_output_active(void) {
#if N64NET_VOICE_IOS
    return n64net_vpio_active();
#else
    return 0;
#endif
}

extern "C" void n64net_voice_output_write(const int16_t *stereo, int frames, int rate) {
#if N64NET_VOICE_IOS
    n64net_vpio_write(stereo, frames, rate);
#else
    (void)stereo; (void)frames; (void)rate;
#endif
}

extern "C" int n64net_voice_output_buffered(int rate) {
#if N64NET_VOICE_IOS
    return n64net_vpio_buffered(rate);
#else
    (void)rate;
    return 0;
#endif
}

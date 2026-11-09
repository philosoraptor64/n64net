// iOS only: voice chat through Apple's voice-processing audio unit
// (kAudioUnitSubType_VoiceProcessingIO).
//
// Echo cancellation subtracts what the speaker plays from what the microphone
// hears, so it must know everything the app plays. Setting the audio
// session's voice-chat mode alone is not enough: the game plays through its
// own SDL audio queue, which the echo canceller can't see, and it then
// suppressed the player's own voice by ~20 dB (measured 2026-11-08). So while
// voice chat runs, this unit records the microphone AND plays the game's
// final audio (game + the other player's voice): the game hands its blocks to
// n64net_vpio_write() instead of its normal audio device.
//
// Threads: write/buffered from the game's audio producer(s) (serialized by a
// mutex), the render/input callbacks on the audio I/O thread (lock-free ring),
// start/stop/check on the game thread.
#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <mutex>

namespace {

constexpr double kUnitRate = 48000.0;
constexpr uint32_t kRingFrames = 1u << 15;  // ~680 ms of stereo at 48 kHz
constexpr uint32_t kResumeFrames = 960;     // after running dry, wait for 20 ms before playing again
constexpr uint32_t kMaxSlice = 8192;
constexpr OSStatus kNoMoreInput = 'nmor';

typedef void (*CaptureFn)(const int16_t *mono, int frames);

AudioUnit sUnit = nullptr;
CaptureFn sCapture = nullptr;
std::atomic<bool> sActive{false};

// Output ring: interleaved stereo s16 at 48 kHz. One reader (render callback).
int16_t sRing[kRingFrames * 2];
std::atomic<uint64_t> sWritePos{0}, sReadPos{0};
bool sStarved = true;  // render thread only

// Microphone scratch (input callback only).
int16_t sInBuf[kMaxSlice];

// Writer side (game audio producers).
std::mutex sWriteLock;
AudioConverterRef sConverter = nullptr;
int sConverterRate = 0;
int16_t sConvOut[16384 * 2];

// Stats.
std::atomic<uint64_t> sPlayedFrames{0};  // incl. silence while starved
std::atomic<uint32_t> sRenderCalls{0}, sInputCalls{0}, sUnderruns{0}, sOverflowFrames{0};
uint32_t sLastRenderCalls = 0, sLastInputCalls = 0;
int sRestarts = 0;
double sLastCheck = 0, sLastRestart = 0;

double NowSec() { return CFAbsoluteTimeGetCurrent(); }

// The audio session as it was before voice chat started; put back on stop,
// so the game's normal output isn't left in voice-call mode (iOS treats
// output in that mode like call audio: other volume, processing).
NSString *sSavedCategory = nil, *sSavedMode = nil;
AVAudioSessionCategoryOptions sSavedOptions = 0;

OSStatus Render(void *, AudioUnitRenderActionFlags *, const AudioTimeStamp *, UInt32, UInt32 frames,
                AudioBufferList *io) {
    sRenderCalls++;
    sPlayedFrames += frames;
    int16_t *out = static_cast<int16_t *>(io->mBuffers[0].mData);
    const uint64_t r = sReadPos.load(std::memory_order_relaxed);
    const uint64_t avail = sWritePos.load(std::memory_order_acquire) - r;
    if (sStarved && avail < kResumeFrames) {
        std::memset(out, 0, frames * 4);
        return noErr;
    }
    const bool resuming = sStarved;
    sStarved = false;
    uint32_t n = avail < frames ? static_cast<uint32_t>(avail) : frames;
    for (uint32_t i = 0; i < n;) {
        const uint32_t at = static_cast<uint32_t>((r + i) % kRingFrames);
        uint32_t run = kRingFrames - at;
        if (run > n - i) run = n - i;
        std::memcpy(out + i * 2, sRing + at * 2, run * 4);
        i += run;
    }
    // The game sometimes makes a little less audio than the speaker plays
    // (e.g. online time sync skips a game step): fade around each gap (5 ms)
    // so it doesn't click.
    constexpr uint32_t kFade = 240;
    if (resuming) {
        const uint32_t f = n < kFade ? n : kFade;
        for (uint32_t i = 0; i < f; i++) {
            out[i * 2] = static_cast<int16_t>(out[i * 2] * static_cast<int>(i) / static_cast<int>(f));
            out[i * 2 + 1] = static_cast<int16_t>(out[i * 2 + 1] * static_cast<int>(i) / static_cast<int>(f));
        }
    }
    if (n < frames) {
        const uint32_t f = n < kFade ? n : kFade;
        for (uint32_t i = 0; i < f; i++) {
            const uint32_t at = n - f + i;
            const int gain = static_cast<int>(f - i);
            out[at * 2] = static_cast<int16_t>(out[at * 2] * gain / static_cast<int>(f));
            out[at * 2 + 1] = static_cast<int16_t>(out[at * 2 + 1] * gain / static_cast<int>(f));
        }
        std::memset(out + n * 2, 0, (frames - n) * 4);
        sUnderruns++;
        sStarved = true;
    }
    sReadPos.store(r + n, std::memory_order_release);
    return noErr;
}

OSStatus Input(void *, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus, UInt32 frames,
               AudioBufferList *) {
    sInputCalls++;
    if (frames > kMaxSlice || sUnit == nullptr) return noErr;
    AudioBufferList bl;
    bl.mNumberBuffers = 1;
    bl.mBuffers[0].mNumberChannels = 1;
    bl.mBuffers[0].mDataByteSize = frames * 2;
    bl.mBuffers[0].mData = sInBuf;
    if (AudioUnitRender(sUnit, flags, ts, bus, frames, &bl) == noErr && sCapture != nullptr) {
        sCapture(sInBuf, static_cast<int>(frames));
    }
    return noErr;
}

AudioStreamBasicDescription PcmFormat(double rate, UInt32 channels) {
    AudioStreamBasicDescription f = {};
    f.mSampleRate = rate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    f.mChannelsPerFrame = channels;
    f.mBitsPerChannel = 16;
    f.mBytesPerFrame = 2 * channels;
    f.mFramesPerPacket = 1;
    f.mBytesPerPacket = 2 * channels;
    return f;
}

bool ConfigureSession(char *error, unsigned error_size) {
    AVAudioSession *s = [AVAudioSession sharedInstance];
    NSError *err = nil;
    // Loudspeaker unless headphones are connected (the phone is in the
    // player's hands, not at their ear); keep the options SDL chose.
    const AVAudioSessionCategoryOptions options =
        s.categoryOptions | AVAudioSessionCategoryOptionDefaultToSpeaker;
    if (![s setCategory:AVAudioSessionCategoryPlayAndRecord mode:AVAudioSessionModeVoiceChat options:options
                  error:&err]) {
        snprintf(error, error_size, "audio session: %s", err.localizedDescription.UTF8String);
        return false;
    }
    [s setPreferredSampleRate:kUnitRate error:nil];
    [s setPreferredIOBufferDuration:0.01 error:nil];
    if (![s setActive:YES error:&err]) {
        snprintf(error, error_size, "audio session activate: %s", err.localizedDescription.UTF8String);
        return false;
    }
    for (AVAudioSessionPortDescription *o in s.currentRoute.outputs) {
        if ([o.portType isEqualToString:AVAudioSessionPortBuiltInReceiver]) {
            [s overrideOutputAudioPort:AVAudioSessionPortOverrideSpeaker error:nil];
            break;
        }
    }
    return true;
}

void DisposeUnit() {
    if (sUnit != nullptr) {
        AudioOutputUnitStop(sUnit);
        AudioUnitUninitialize(sUnit);
        AudioComponentInstanceDispose(sUnit);
        sUnit = nullptr;
    }
}

#define CHECK(call, what)                                                         \
    do {                                                                          \
        OSStatus st_ = (call);                                                    \
        if (st_ != noErr) {                                                       \
            snprintf(error, error_size, "%s failed (%d)", what, static_cast<int>(st_)); \
            DisposeUnit();                                                        \
            return 0;                                                             \
        }                                                                         \
    } while (0)

} // namespace

extern "C" int n64net_vpio_start(CaptureFn capture, char *error, unsigned error_size) {
    if (sActive) return 1;
    {
        AVAudioSession *s = [AVAudioSession sharedInstance];
        sSavedCategory = s.category;
        sSavedMode = s.mode;
        sSavedOptions = s.categoryOptions;
    }
    if (!ConfigureSession(error, error_size)) return 0;

    AudioComponentDescription desc = {};
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_VoiceProcessingIO;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    if (comp == nullptr) {
        snprintf(error, error_size, "no voice-processing audio unit");
        return 0;
    }
    CHECK(AudioComponentInstanceNew(comp, &sUnit), "AudioComponentInstanceNew");

    const UInt32 one = 1;
    CHECK(AudioUnitSetProperty(sUnit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &one, sizeof one),
          "enable input");
    CHECK(AudioUnitSetProperty(sUnit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &one, sizeof one),
          "enable output");
    const AudioStreamBasicDescription mic = PcmFormat(kUnitRate, 1);
    const AudioStreamBasicDescription spk = PcmFormat(kUnitRate, 2);
    CHECK(AudioUnitSetProperty(sUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &mic, sizeof mic),
          "mic format");
    CHECK(AudioUnitSetProperty(sUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &spk, sizeof spk),
          "speaker format");
    const UInt32 maxSlice = kMaxSlice;
    AudioUnitSetProperty(sUnit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxSlice,
                         sizeof maxSlice);
    AURenderCallbackStruct render = {Render, nullptr};
    CHECK(AudioUnitSetProperty(sUnit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &render,
                               sizeof render),
          "render callback");
    AURenderCallbackStruct input = {Input, nullptr};
    CHECK(AudioUnitSetProperty(sUnit, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 1, &input,
                               sizeof input),
          "input callback");
    if (const char *b = getenv("N64NET_VOICE_BYPASS"); b != nullptr && b[0] == '1') {  // debug A/B
        const UInt32 bypass = 1;
        AudioUnitSetProperty(sUnit, kAUVoiceIOProperty_BypassVoiceProcessing, kAudioUnitScope_Global, 0, &bypass,
                             sizeof bypass);
    }
    const UInt32 agc = 1;
    AudioUnitSetProperty(sUnit, kAUVoiceIOProperty_VoiceProcessingEnableAGC, kAudioUnitScope_Global, 1, &agc,
                         sizeof agc);
    if (@available(iOS 17.0, *)) {
        // Don't turn down other apps' audio (e.g. music) more than needed.
        AUVoiceIOOtherAudioDuckingConfiguration duck = {};
        duck.mEnableAdvancedDucking = false;
        duck.mDuckingLevel = kAUVoiceIOOtherAudioDuckingLevelMin;
        AudioUnitSetProperty(sUnit, kAUVoiceIOProperty_OtherAudioDuckingConfiguration, kAudioUnitScope_Global, 0,
                             &duck, sizeof duck);
    }

    sCapture = capture;
    sReadPos = 0;
    sWritePos = 0;
    sStarved = true;
    sRenderCalls = sInputCalls = sUnderruns = sOverflowFrames = 0;
    sPlayedFrames = 0;
    sLastRenderCalls = sLastInputCalls = 0;
    sRestarts = 0;
    sLastCheck = NowSec();
    CHECK(AudioUnitInitialize(sUnit), "AudioUnitInitialize");
    CHECK(AudioOutputUnitStart(sUnit), "AudioOutputUnitStart");
    sActive = true;
    return 1;
}

extern "C" void n64net_vpio_stop(void) {
    sActive = false;  // producers go back to the normal output first
    DisposeUnit();
    if (sSavedCategory != nil) {
        AVAudioSession *s = [AVAudioSession sharedInstance];
        [s overrideOutputAudioPort:AVAudioSessionPortOverrideNone error:nil];
        [s setCategory:sSavedCategory mode:sSavedMode options:sSavedOptions error:nil];
        [s setActive:YES error:nil];
        sSavedCategory = nil;
        sSavedMode = nil;
    }
    std::lock_guard<std::mutex> lock(sWriteLock);
    if (sConverter != nullptr) {
        AudioConverterDispose(sConverter);
        sConverter = nullptr;
        sConverterRate = 0;
    }
    sCapture = nullptr;
}

extern "C" int n64net_vpio_active(void) { return sActive ? 1 : 0; }

namespace {
struct ConvInput {
    const int16_t *data;
    UInt32 frames;
};

OSStatus ConvInputProc(AudioConverterRef, UInt32 *packets, AudioBufferList *io, AudioStreamPacketDescription **,
                       void *user) {
    ConvInput *in = static_cast<ConvInput *>(user);
    if (in->frames == 0) {
        *packets = 0;
        return kNoMoreInput;  // keep the converter's state for the next block
    }
    io->mBuffers[0].mData = const_cast<int16_t *>(in->data);
    io->mBuffers[0].mDataByteSize = in->frames * 4;
    io->mBuffers[0].mNumberChannels = 2;
    *packets = in->frames;
    in->frames = 0;
    return noErr;
}

void Push(const int16_t *stereo, uint32_t frames) {
    const uint64_t w = sWritePos.load(std::memory_order_relaxed);
    const uint64_t used = w - sReadPos.load(std::memory_order_acquire);
    const uint64_t room = kRingFrames - used;
    if (frames > room) {
        sOverflowFrames += frames - static_cast<uint32_t>(room);
        frames = static_cast<uint32_t>(room);
    }
    for (uint32_t i = 0; i < frames;) {
        const uint32_t at = static_cast<uint32_t>((w + i) % kRingFrames);
        uint32_t run = kRingFrames - at;
        if (run > frames - i) run = frames - i;
        std::memcpy(sRing + at * 2, stereo + i * 2, run * 4);
        i += run;
    }
    sWritePos.store(w + frames, std::memory_order_release);
}
} // namespace

extern "C" void n64net_vpio_write(const int16_t *stereo, int frames, int rate) {
    if (!sActive || stereo == nullptr || frames <= 0 || rate <= 0) return;
    std::lock_guard<std::mutex> lock(sWriteLock);
    if (rate == static_cast<int>(kUnitRate)) {
        Push(stereo, static_cast<uint32_t>(frames));
        return;
    }
    if (sConverter == nullptr || sConverterRate != rate) {
        if (sConverter != nullptr) AudioConverterDispose(sConverter);
        sConverter = nullptr;
        const AudioStreamBasicDescription from = PcmFormat(rate, 2), to = PcmFormat(kUnitRate, 2);
        if (AudioConverterNew(&from, &to, &sConverter) != noErr) {
            sConverter = nullptr;
            return;
        }
        sConverterRate = rate;
    }
    while (frames > 0) {
        const int chunk = frames > 8192 ? 8192 : frames;
        ConvInput in = {stereo, static_cast<UInt32>(chunk)};
        UInt32 outFrames = sizeof sConvOut / 4;
        AudioBufferList bl;
        bl.mNumberBuffers = 1;
        bl.mBuffers[0].mNumberChannels = 2;
        bl.mBuffers[0].mDataByteSize = sizeof sConvOut;
        bl.mBuffers[0].mData = sConvOut;
        const OSStatus st = AudioConverterFillComplexBuffer(sConverter, ConvInputProc, &in, &outFrames, &bl, nullptr);
        if ((st == noErr || st == kNoMoreInput) && outFrames > 0) {
            Push(sConvOut, outFrames);
        }
        stereo += chunk * 2;
        frames -= chunk;
    }
}

extern "C" int n64net_vpio_buffered(int rate) {
    if (!sActive || rate <= 0) return 0;
    const uint64_t used = sWritePos.load() - sReadPos.load();
    return static_cast<int>(used * static_cast<uint64_t>(rate) / static_cast<uint64_t>(kUnitRate));
}

// About once a second from the game thread: restarts the unit if iOS stopped
// it (interruption, app switch, route change) and reports health.
extern "C" int n64net_vpio_check(char *note, unsigned note_size) {
    note[0] = 0;
    if (!sActive || sUnit == nullptr) return 0;
    const double now = NowSec();
    if (now - sLastCheck < 1.0) return 0;
    sLastCheck = now;
    const uint32_t renders = sRenderCalls.load(), inputs = sInputCalls.load();
    UInt32 running = 0, size = sizeof running;
    AudioUnitGetProperty(sUnit, kAudioOutputUnitProperty_IsRunning, kAudioUnitScope_Global, 0, &running, &size);
    const bool stuck = renders == sLastRenderCalls || inputs == sLastInputCalls;
    sLastRenderCalls = renders;
    sLastInputCalls = inputs;
    if (running && !stuck) return 0;
    if (now - sLastRestart < 3.0) return 0;  // e.g. in the background: iOS keeps it stopped until the app is back
    sLastRestart = now;
    sRestarts++;
    AudioOutputUnitStop(sUnit);
    char error[160] = "";
    ConfigureSession(error, sizeof error);
    const OSStatus st = AudioOutputUnitStart(sUnit);
    snprintf(note, note_size, "voice audio unit %s; restarted it (try %d, %s%s)", running ? "stalled" : "stopped",
             sRestarts, st == noErr ? "ok" : "failed", error);
    return 1;
}

extern "C" void n64net_vpio_stats(char *out, unsigned out_size) {
    snprintf(out, out_size,
             "unit: renders %u mic %u underruns %u overflow %u queued %d ms restarts %d; game wrote %llu ms, "
             "speaker played %llu ms",
             sRenderCalls.load(), sInputCalls.load(), sUnderruns.load(), sOverflowFrames.load(),
             n64net_vpio_buffered(1000), sRestarts, (unsigned long long)(sWritePos.load() / 48),
             (unsigned long long)(sPlayedFrames.load() / 48));
}

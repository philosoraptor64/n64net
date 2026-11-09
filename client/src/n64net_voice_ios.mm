// iOS only: echo cancellation for voice chat. Apple's voice processing (echo
// cancellation, noise suppression, automatic gain) is on in the audio
// session's voice-chat mode; SDL leaves the session in the default mode.
// Set it once the microphone is open (before that it fails with -50).
#import <AVFoundation/AVFoundation.h>
#include <stdio.h>

extern "C" int n64net_voice_ios_enable_voice_processing(char *error, unsigned error_size) {
    NSError *err = nil;
    if ([[AVAudioSession sharedInstance] setMode:AVAudioSessionModeVoiceChat error:&err]) {
        return 1;
    }
    snprintf(error, error_size, "%s", err ? err.localizedDescription.UTF8String : "unknown error");
    return 0;
}

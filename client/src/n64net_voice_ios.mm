// iOS only: echo cancellation for voice chat. Apple's voice processing (echo
// cancellation, noise suppression, automatic gain) is on in the audio
// session's voice-chat mode; SDL leaves the session in the default mode.
// Set it once the microphone is open (before that it fails with -50).
#import <AVFoundation/AVFoundation.h>
#include <stdio.h>

extern "C" int n64net_voice_ios_enable_voice_processing(int processing, char *error, unsigned error_size) {
    AVAudioSession *s = [AVAudioSession sharedInstance];
    NSError *err = nil;
    // Voice-chat mode on its own routes like a phone call held to the face:
    // sound from the earpiece, the bottom mic tuned for a mouth a few cm
    // away. Players hold the phone in their hands, so ask for the
    // loudspeaker (headphones still win when connected) in the same call.
    AVAudioSessionCategoryOptions options = s.categoryOptions | AVAudioSessionCategoryOptionDefaultToSpeaker;
    NSString *category = s.category;
    if (![category isEqualToString:AVAudioSessionCategoryPlayAndRecord]) {
        category = AVAudioSessionCategoryPlayAndRecord;
    }
    NSString *mode = processing ? AVAudioSessionModeVoiceChat : AVAudioSessionModeDefault;
    if (![s setCategory:category mode:mode options:options error:&err]) {
        snprintf(error, error_size, "%s", err ? err.localizedDescription.UTF8String : "unknown error");
        return 0;
    }
    // If iOS still picked the earpiece, override the output to the speaker.
    for (AVAudioSessionPortDescription *o in s.currentRoute.outputs) {
        if ([o.portType isEqualToString:AVAudioSessionPortBuiltInReceiver]) {
            [s overrideOutputAudioPort:AVAudioSessionPortOverrideSpeaker error:nil];
            break;
        }
    }
    return 1;
}

// One line describing what iOS actually uses for voice: the session mode and
// options, the output (Speaker = loudspeaker, Receiver = the earpiece used
// when the phone is held to the face) and the input with its microphone
// (data source, e.g. Bottom/Front/Back) and polar pattern.
extern "C" void n64net_voice_ios_describe_route(char *out, unsigned out_size) {
    AVAudioSession *s = [AVAudioSession sharedInstance];
    NSMutableString *line = [NSMutableString stringWithFormat:@"category %@ mode %@ options 0x%lx defaultToSpeaker %d",
        s.category, s.mode, (unsigned long)s.categoryOptions,
        (s.categoryOptions & AVAudioSessionCategoryOptionDefaultToSpeaker) ? 1 : 0];
    for (AVAudioSessionPortDescription *o in s.currentRoute.outputs) {
        [line appendFormat:@"; output %@ (%@)", o.portType, o.portName];
    }
    for (AVAudioSessionPortDescription *i in s.currentRoute.inputs) {
        [line appendFormat:@"; input %@ (%@)", i.portType, i.portName];
        AVAudioSessionDataSourceDescription *d = i.selectedDataSource;
        if (d) {
            [line appendFormat:@" mic %@ pattern %@", d.dataSourceName, d.selectedPolarPattern ?: @"-"];
        }
    }
    [line appendFormat:@"; input gain %.2f", s.inputGain];
    snprintf(out, out_size, "%s", line.UTF8String);
}

extern "C" void n64net_voice_ios_documents_path(const char *name, char *out, unsigned out_size) {
    NSString *docs = [NSHomeDirectory() stringByAppendingPathComponent:@"Documents"];
    snprintf(out, out_size, "%s/%s", docs.UTF8String, name);
}

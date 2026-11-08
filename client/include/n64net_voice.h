/* n64net voice chat: game-neutral, C API (port/net/n64net_voice.cpp).
 *
 * Opus-coded mono speech over the game's own UDP connection (direct or
 * relayed). The game's netcode passes received datagrams to
 * n64net_voice_handle_packet() and sends what n64net_voice_pump() hands it.
 * Voice never touches the game simulation, so it can't cause a desync.
 *
 * Wire format (one 20 ms frame per datagram, at most N64NET_VOICE_MAX_PACKET):
 *   u32 magic 'N64V' (big endian) | u16 seq | u8 flags | Opus payload */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define N64NET_VOICE_MAGIC 0x4E363456u
#define N64NET_VOICE_MAX_PACKET 112

typedef struct {
    int enabled;         /* 0: never opens the microphone */
    int push_to_talk;    /* 1: sends only while talking is held; 0: open mic */
    int test_tone;       /* 1: a generated tone instead of the microphone (tests) */
    void (*log)(const char *fmt, ...);
} n64net_voice_config;

typedef void (*n64net_voice_send_fn)(const uint8_t *data, uint32_t size);

/* Connected: opens the microphone (when enabled). Idempotent. Call stop,
 * start and mix from the same thread (the game thread). */
void n64net_voice_start(const n64net_voice_config *config);
/* Disconnected: closes the microphone. Idempotent. */
void n64net_voice_stop(void);
int n64net_voice_is_running(void);

/* The game's audio output calls this for every block it plays: adds the
 * other player's voice to `stereo` (interleaved s16, `frames` frames at
 * `rate` Hz). The voice is paced by the game's own audio clock. */
void n64net_voice_mix(int16_t *stereo, int frames, int rate);

/* Call often (every game tick, and while waiting): sends captured frames. */
void n64net_voice_pump(n64net_voice_send_fn send);
/* Returns 1 if `data` was a voice packet (consumed), else 0. */
int n64net_voice_handle_packet(const uint8_t *data, int32_t size);

/* Open-mic mode: mute stops sending. Push-to-talk: talking sends. */
void n64net_voice_set_muted(int muted);
int n64net_voice_is_muted(void);
void n64net_voice_set_talking(int talking);
int n64net_voice_is_push_to_talk(void);
/* 1 while the other player's voice is audible (for a speaking indicator). */
int n64net_voice_remote_speaking(void);
/* 1 while this machine is sending speech. */
int n64net_voice_local_speaking(void);

#ifdef __cplusplus
}
#endif

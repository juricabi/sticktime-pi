/* Sound: the emulator's motor synth, beeps (playTone), generated music and the player's songs,
 * re-expressed as per-sample DSP (notes: audio-motor.md, audio-music.md, emulator-features.md §3),
 * played by an audio thread through ALSA (raw kernel PCM ioctls: the 3.5 mm jack's audio, or HDMI
 * with IEC958 packing). The frame loop only hands over parameters; it never blocks on audio. */
#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct audio audio_t;

typedef struct {
  const char *out;         /* "auto" | "jack" | "hdmi" | "off" | "wav:<path>" (tests) | "null" (render, discard) */
  bool video_hdmi;         /* auto: HDMI when the video goes to HDMI, else the jack */
  int rate;                /* 48000 */
  const char *music_dir;   /* the player's songs (mp3, ogg, wav, flac) */
  uint32_t seed;           /* music / noise randomness (0 = from the clock) */
} audio_config_t;

enum { AUDIO_MUSIC_GENERATED = 0, AUDIO_MUSIC_SONGS = 1 };

typedef struct {
  bool sound;              /* motor + beeps */
  int volume;              /* 0..100 (gain = (v/100)^2 like the browser's volGain) */
  bool music;
  int music_volume;        /* 0..100 */
  int music_source;        /* AUDIO_MUSIC_* */
  bool mono;               /* sum to mono (a VTX audio pad has one channel) */
} audio_levels_t;

/* Never NULL unless out of memory: when the device can't be opened the object stays silent and
 * retries now and then; err gets the reason. */
audio_t *audio_open(const audio_config_t *c, char *err, int errlen);
void audio_close(audio_t *a);
void audio_set_levels(audio_t *a, const audio_levels_t *lv);

/* Frame loop, after each run(): the values of STICKTIME_TEST.audio() (n of them, in order:
 * sT, S.quad, state, aTf, aVu, aE2, aW, sA, sE, sR, aS, aF, R.crashes, R.hv) and the frame time. */
void audio_game(audio_t *a, const double *vals, int n, double dt_ms);
/* The game's playTone(freq, length, pause, flags, freqIncr) and playHaptic(ms) calls this frame. */
void audio_tone(audio_t *a, int freq, int len_ms, int pause_ms, int flags, int freq_incr);
void audio_haptic(audio_t *a, int ms);
/* The game was reloaded or closed: motor sound off at once. */
void audio_game_reset(audio_t *a);
void audio_music_next(audio_t *a);
/* Rescan the songs folder (after an upload/delete). */
void audio_songs_changed(audio_t *a);

typedef struct {
  bool ok;                 /* device open and playing */
  char device[48];         /* "jack (Headphones)", "hdmi (vc4hdmi0)", "off", "wav" */
  char error[128];
  char now_playing[128];   /* "Generated: Song 3", "Songs: track.mp3", "" */
  uint32_t underruns;
} audio_status_t;
void audio_status(audio_t *a, audio_status_t *st);

#endif

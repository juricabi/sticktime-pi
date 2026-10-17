/* The parts behind audio.h, so they can be written and tested separately:
 *   audio.c   the output (ALSA raw PCM on the jack, HDMI with IEC958 packing, a WAV file, or nothing),
 *             the audio thread, levels, mono, clipping; mixes sfx and music
 *   sfx.c     the motor synth and the beeps (notes audio-motor.md): control in the frame thread,
 *             per-sample DSP in the audio thread
 *   music.c   the generated music (notes audio-music.md) and the player's songs (songs.c: decoders)
 *   dsp.c     shared WebAudio-like building blocks (dsp.h)
 * Threads: sfx_frame/sfx_tone/sfx_reset are called by the frame loop; *_render by the audio thread
 * only; music_set/next/songs_changed/now_playing from any thread. No allocation and no locks that
 * the audio thread could wait on for long in the render paths. */
#ifndef AUDIO_INTERNAL_H
#define AUDIO_INTERNAL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ---- sound effects: motor synth + beeps ---- */
typedef struct sfx sfx_t;
sfx_t *sfx_new(int rate, uint32_t seed);
void   sfx_free(sfx_t *s);
/* frame thread: STICKTIME_TEST.audio() values (see audio.h) and the frame time */
void   sfx_frame(sfx_t *s, const double *vals, int n, double dt_ms);
/* frame thread: playTone / playHaptic semantics of the emulator (queue, pause, PLAY_NOW, PLAY_BACKGROUND) */
void   sfx_tone(sfx_t *s, int freq, int len_ms, int pause_ms, int flags, int freq_incr);
void   sfx_reset(sfx_t *s);                 /* game reloaded: motors off now, tone queue cleared */
/* any thread: the sound volume 0..1 (the browser's vol; gain vol^2 smoothed; 0 = synth torn down) */
void   sfx_set_volume(sfx_t *s, float vol);
/* audio thread: ADD n stereo samples into L/R (float, nominal -1..1, already at the sound volume) */
void   sfx_render(sfx_t *s, float *L, float *R, int n);

/* ---- music: generated or the player's songs ---- */
typedef struct music music_t;
music_t *music_new(int rate, uint32_t seed, const char *songs_dir);
void     music_free(music_t *m);
/* any thread */
void     music_set(music_t *m, bool on, float vol, int source);   /* source: AUDIO_MUSIC_* (audio.h) */
void     music_next(music_t *m);
void     music_songs_changed(music_t *m);
void     music_now_playing(music_t *m, char *out, size_t n);
/* audio thread: ADD n stereo samples (at the music volume) */
void     music_render(music_t *m, float *L, float *R, int n);

/* ---- offline rendering for tests (audio.c) ---- */
/* Render the whole mix for a scripted sequence without a device: the caller feeds frames through
 * audio_game/audio_tone between calls; audio_render_offline() renders n samples into a WAV
 * writer. Only valid for an audio_t opened with out = "wav:<path>" or "null" and rate given;
 * with these outputs the audio thread is not started: rendering happens in this call. */
struct audio;
void audio_render_offline(struct audio *a, int nsamples);

#endif

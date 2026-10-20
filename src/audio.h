#ifndef TOWN_AUDIO_H
#define TOWN_AUDIO_H

#include "common.h"

enum { VOICE_SFX, VOICE_MUSIC, VOICE_CD, NUM_VOICES };

int audio_init(int enabled);
void audio_shutdown(void);
void audio_set_volume(float master);
void audio_set_trace(int on);

/* repeat: number of plays, 0 = forever */
int audio_play_sample(const char *path, int repeat);
int audio_play_voc(const char *path);
void audio_stop(int voice);

/* CD audio from a .cue sheet (BINARY or WAVE files). */
int cd_open(const char *cue_path);
int cd_available(void);
/* Play a range of a track, positions in 1/75 s frames relative to the track start;
 * end_frame < 0 plays to the end of the track. */
void cd_play(int track, long start_frame, long end_frame);
void cd_stop(void);

/* Decode helpers, exposed for the self-test. Return mono 16-bit PCM. */
int16_t *voc_decode_all(const char *path, int *out_len, int *out_rate);

#endif

/*
 * Audio: a small software mixer on top of SDL2.
 *
 * The DOS player drove a single SoundBlaster voice (8SVX/raw samples and the VOC
 * substitutes for the Amiga music) plus CD audio through MSCDEX. Here samples and music
 * get separate voices so a sound effect no longer cuts the music off, as on the Amiga.
 */
#include "audio.h"

#include <SDL.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OUT_RATE 44100

typedef struct {
    int16_t *pcm; /* interleaved when channels == 2 */
    long frames;
    int channels;
    int rate;
    int repeat; /* number of plays, 0 = forever */
} Part;

typedef struct {
    Part *parts;
    int nparts;
    int part;
    int plays;
    double pos;
    int active;
    float gain;
} Voice;

static SDL_AudioDeviceID dev;
static Voice voices[NUM_VOICES];
static float master = 1.0f;
static int trace;

void audio_set_trace(int on) { trace = on; }

static void free_parts(Part *parts, int n)
{
    for (int i = 0; i < n; i++)
        free(parts[i].pcm);
    free(parts);
}

static void mix_voice(Voice *v, float *out, int frames)
{
    for (int i = 0; i < frames && v->active; i++) {
        Part *p = &v->parts[v->part];
        if (p->frames <= 0) {
            v->pos = (double)p->frames;
        } else {
            long i0 = (long)v->pos;
            double frac = v->pos - (double)i0;
            long i1 = i0 + 1 < p->frames ? i0 + 1 : i0;
            float l, r;
            if (p->channels == 2) {
                l = (float)(p->pcm[i0 * 2] * (1.0 - frac) + p->pcm[i1 * 2] * frac);
                r = (float)(p->pcm[i0 * 2 + 1] * (1.0 - frac) + p->pcm[i1 * 2 + 1] * frac);
            } else {
                l = r = (float)(p->pcm[i0] * (1.0 - frac) + p->pcm[i1] * frac);
            }
            out[i * 2] += l * v->gain;
            out[i * 2 + 1] += r * v->gain;
            v->pos += (double)p->rate / OUT_RATE;
        }
        if (v->pos >= (double)p->frames) {
            v->plays++;
            if (p->repeat == 0 || v->plays < p->repeat) {
                v->pos = p->frames > 0 ? v->pos - (double)p->frames : 0;
            } else {
                v->part++;
                v->plays = 0;
                v->pos = 0;
                if (v->part >= v->nparts)
                    v->active = 0;
            }
        }
    }
}

#define MIX_CHUNK 1024

static void callback(void *userdata, Uint8 *stream, int len)
{
    (void)userdata;
    static float mix[MIX_CHUNK * 2];
    int16_t *out = (int16_t *)stream;
    int frames = len / 4;
    while (frames > 0) {
        int n = frames < MIX_CHUNK ? frames : MIX_CHUNK;
        memset(mix, 0, sizeof(float) * (size_t)n * 2);
        for (int v = 0; v < NUM_VOICES; v++)
            mix_voice(&voices[v], mix, n);
        for (int i = 0; i < n * 2; i++) {
            float s = mix[i] * master;
            if (s > 32767.f)
                s = 32767.f;
            if (s < -32768.f)
                s = -32768.f;
            *out++ = (int16_t)s;
        }
        frames -= n;
    }
}

int audio_init(int enabled)
{
    for (int v = 0; v < NUM_VOICES; v++)
        voices[v].gain = 1.0f;
    voices[VOICE_SFX].gain = 0.9f;
    voices[VOICE_MUSIC].gain = 0.8f;
    if (!enabled)
        return 0;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        log_msg("audio: %s", SDL_GetError());
        return -1;
    }
    SDL_AudioSpec want = {0}, have;
    want.freq = OUT_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = callback;
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!dev) {
        log_msg("audio: %s", SDL_GetError());
        return -1;
    }
    SDL_PauseAudioDevice(dev, 0);
    return 0;
}

void audio_shutdown(void)
{
    if (dev)
        SDL_CloseAudioDevice(dev);
    dev = 0;
    for (int v = 0; v < NUM_VOICES; v++) {
        free_parts(voices[v].parts, voices[v].nparts);
        memset(&voices[v], 0, sizeof voices[v]);
    }
}

void audio_set_volume(float m) { master = m; }

static void start_voice(int voice, Part *parts, int nparts)
{
    Part *old;
    int oldn;
    if (dev)
        SDL_LockAudioDevice(dev);
    Voice *v = &voices[voice];
    old = v->parts;
    oldn = v->nparts;
    v->parts = parts;
    v->nparts = nparts;
    v->part = 0;
    v->plays = 0;
    v->pos = 0;
    v->active = nparts > 0;
    if (dev)
        SDL_UnlockAudioDevice(dev);
    free_parts(old, oldn);
}

void audio_stop(int voice) { start_voice(voice, NULL, 0); }

/* ---- 8SVX and raw samples ---- */

static Part *load_sample(const char *path, int repeat)
{
    size_t size;
    uint8_t *d = read_file(path, &size);
    if (!d)
        return NULL;
    const uint8_t *body = NULL;
    uint32_t body_len = 0;
    int rate = 0;
    double vol = 1.0;
    if (size >= 12 && !memcmp(d, "FORM", 4) && !memcmp(d + 8, "8SVX", 4)) {
        size_t i = 12;
        while (i + 8 <= size) {
            uint32_t n = rd32be(d + i + 4);
            const uint8_t *c = d + i + 8;
            if (i + 8 + n > size)
                n = (uint32_t)(size - i - 8);
            if (!memcmp(d + i, "VHDR", 4) && n >= 20) {
                rate = rd16be(c + 12);
                uint32_t v = rd32be(c + 16);
                if (v && v <= 0x10000)
                    vol = v / 65536.0;
            } else if (!memcmp(d + i, "BODY", 4)) {
                body = c;
                body_len = n;
            }
            i += 8 + n + (n & 1);
        }
    } else if (size > 6) {
        /* headerless Amiga sample used by the PC version: length.l, rate.w, data */
        body_len = rd32be(d);
        rate = rd16be(d + 4);
        if (rate < 100)
            rate *= 1000;
        body = d + 6;
        if (body_len > size - 6)
            body_len = (uint32_t)(size - 6);
    }
    if (!body || !rate) {
        free(d);
        return NULL;
    }
    Part *p = xcalloc(1, sizeof *p);
    p->pcm = xmalloc(sizeof(int16_t) * (body_len ? body_len : 1));
    for (uint32_t k = 0; k < body_len; k++)
        p->pcm[k] = (int16_t)((int8_t)body[k] * 256 * vol);
    p->frames = body_len;
    p->channels = 1;
    p->rate = rate;
    p->repeat = repeat;
    free(d);
    return p;
}

int audio_play_sample(const char *path, int repeat)
{
    Part *p = load_sample(path, repeat);
    if (!p)
        return -1;
    start_voice(VOICE_SFX, p, 1);
    return 0;
}

/* ---- Creative Voice File, including the SoundBlaster ADPCM packings ---- */

static uint8_t adpcm4(int sample, uint8_t *ref, int *scale)
{
    static const int8_t scale_map[64] = {
        0, 1, 2, 3, 4, 5, 6, 7, 0, -1, -2, -3, -4, -5, -6, -7,
        1, 3, 5, 7, 9, 11, 13, 15, -1, -3, -5, -7, -9, -11, -13, -15,
        2, 6, 10, 14, 18, 22, 26, 30, -2, -6, -10, -14, -18, -22, -26, -30,
        4, 12, 20, 28, 36, 44, 52, 60, -4, -12, -20, -28, -36, -44, -52, -60};
    static const uint8_t adjust_map[64] = {
        0, 0, 0, 0, 0, 16, 16, 16, 0, 0, 0, 0, 0, 16, 16, 16,
        240, 0, 0, 0, 0, 16, 16, 16, 240, 0, 0, 0, 0, 16, 16, 16,
        240, 0, 0, 0, 0, 16, 16, 16, 240, 0, 0, 0, 0, 16, 16, 16,
        240, 0, 0, 0, 0, 0, 0, 0, 240, 0, 0, 0, 0, 0, 0, 0};
    int s = sample + *scale;
    if (s < 0)
        s = 0;
    if (s > 63)
        s = 63;
    int r = *ref + scale_map[s];
    *ref = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
    *scale = (*scale + adjust_map[s]) & 0xff;
    return *ref;
}

static uint8_t adpcm3(int sample, uint8_t *ref, int *scale)
{
    static const int8_t scale_map[40] = {
        0, 1, 2, 3, 0, -1, -2, -3, 1, 3, 5, 7, -1, -3, -5, -7,
        2, 6, 10, 14, -2, -6, -10, -14, 4, 12, 20, 28, -4, -12, -20, -28,
        5, 15, 25, 35, -5, -15, -25, -35};
    static const uint8_t adjust_map[40] = {
        0, 0, 0, 8, 0, 0, 0, 8, 248, 0, 0, 8, 248, 0, 0, 8,
        248, 0, 0, 8, 248, 0, 0, 8, 248, 0, 0, 8, 248, 0, 0, 8,
        248, 0, 0, 0, 248, 0, 0, 0};
    int s = sample + *scale;
    if (s < 0)
        s = 0;
    if (s > 39)
        s = 39;
    int r = *ref + scale_map[s];
    *ref = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
    *scale = (*scale + adjust_map[s]) & 0xff;
    return *ref;
}

static uint8_t adpcm2(int sample, uint8_t *ref, int *scale)
{
    static const int8_t scale_map[24] = {
        0, 1, 0, -1, 1, 3, -1, -3, 2, 6, -2, -6, 4, 12, -4, -12,
        8, 24, -8, -24, 6, 48, -16, -48};
    static const uint8_t adjust_map[24] = {
        0, 4, 0, 4, 252, 4, 252, 4, 252, 4, 252, 4,
        252, 4, 252, 4, 252, 4, 252, 4, 252, 0, 252, 0};
    int s = sample + *scale;
    if (s < 0)
        s = 0;
    if (s > 23)
        s = 23;
    int r = *ref + scale_map[s];
    *ref = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
    *scale = (*scale + adjust_map[s]) & 0xff;
    return *ref;
}

typedef struct {
    int16_t *pcm;
    long len, cap;
} Pcm;

static void pcm_push(Pcm *p, int16_t v)
{
    if (p->len == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 65536;
        int16_t *n = realloc(p->pcm, sizeof(int16_t) * (size_t)p->cap);
        if (!n)
            fatal("out of memory");
        p->pcm = n;
    }
    p->pcm[p->len++] = v;
}

static void decode_block(Pcm *out, const uint8_t *s, long n, int pack, int bits16)
{
    if (bits16) {
        for (long k = 0; k + 1 < n; k += 2)
            pcm_push(out, (int16_t)(s[k] | s[k + 1] << 8));
        return;
    }
    if (pack == 0 || n < 1) {
        for (long k = 0; k < n; k++)
            pcm_push(out, (int16_t)((s[k] - 128) * 256));
        return;
    }
    uint8_t ref = s[0];
    int scale = 0;
    pcm_push(out, (int16_t)((ref - 128) * 256));
    for (long k = 1; k < n; k++) {
        uint8_t b = s[k];
        if (pack == 1) {
            pcm_push(out, (int16_t)((adpcm4(b >> 4, &ref, &scale) - 128) * 256));
            pcm_push(out, (int16_t)((adpcm4(b & 15, &ref, &scale) - 128) * 256));
        } else if (pack == 2) {
            pcm_push(out, (int16_t)((adpcm3((b >> 5) & 7, &ref, &scale) - 128) * 256));
            pcm_push(out, (int16_t)((adpcm3((b >> 2) & 7, &ref, &scale) - 128) * 256));
            pcm_push(out, (int16_t)((adpcm3((b & 3) << 1, &ref, &scale) - 128) * 256));
        } else {
            for (int sh = 6; sh >= 0; sh -= 2)
                pcm_push(out, (int16_t)((adpcm2((b >> sh) & 3, &ref, &scale) - 128) * 256));
        }
    }
}

/* The 2.6-bit packing drifts (on real hardware too); a gentle DC blocker removes the
 * subsonic offset without touching the audible range. */
static void dc_block(int16_t *pcm, long n, int rate)
{
    double r = 1.0 - 2.0 * 3.14159265 * 8.0 / rate; /* ~8 Hz corner */
    double x1 = n ? pcm[0] : 0, y1 = 0;
    for (long i = 0; i < n; i++) {
        double x = pcm[i];
        double y = x - x1 + r * y1;
        x1 = x;
        y1 = y;
        pcm[i] = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
    }
}

/* Splits the file into parts at repeat boundaries; returns the number of parts. */
static int voc_parts(const char *path, Part **out)
{
    size_t size;
    uint8_t *d = read_file(path, &size);
    *out = NULL;
    if (!d)
        return 0;
    if (size < 26 || memcmp(d, "Creative Voice File\x1a", 20)) {
        free(d);
        return 0;
    }
    size_t i = d[20] | d[21] << 8;
    Part *parts = NULL;
    int nparts = 0;
    Pcm cur = {0};
    int cur_rate = 0, cur_repeat = 1, in_repeat = 0;
    int ext_rate = 0, ext_pack = -1;

#define FLUSH()                                                                 \
    do {                                                                        \
        if (cur.len) {                                                          \
            dc_block(cur.pcm, cur.len, cur_rate ? cur_rate : 8000);             \
            parts = realloc(parts, sizeof(Part) * (size_t)(nparts + 1));        \
            parts[nparts].pcm = cur.pcm;                                        \
            parts[nparts].frames = cur.len;                                     \
            parts[nparts].channels = 1;                                         \
            parts[nparts].rate = cur_rate ? cur_rate : 8000;                    \
            parts[nparts].repeat = cur_repeat;                                  \
            nparts++;                                                           \
        } else {                                                                \
            free(cur.pcm);                                                      \
        }                                                                       \
        memset(&cur, 0, sizeof cur);                                            \
    } while (0)

    while (i < size) {
        int type = d[i];
        if (type == 0 || i + 4 > size)
            break;
        long n = d[i + 1] | d[i + 2] << 8 | (long)d[i + 3] << 16;
        const uint8_t *b = d + i + 4;
        if (i + 4 + (size_t)n > size)
            n = (long)(size - i - 4);
        switch (type) {
        case 1: /* sound data */
            if (n >= 2) {
                int rate = 1000000 / (256 - b[0]);
                int pack = b[1];
                if (ext_pack >= 0) {
                    rate = ext_rate;
                    pack = ext_pack;
                    ext_pack = -1;
                }
                if (!in_repeat && cur.len && rate != cur_rate)
                    FLUSH();
                if (!cur.len)
                    cur_rate = rate;
                decode_block(&cur, b + 2, n - 2, pack, 0);
            }
            break;
        case 2: /* continuation */
            decode_block(&cur, b, n, 0, 0);
            break;
        case 3: /* silence */
            if (n >= 3) {
                long cnt = (b[0] | b[1] << 8) + 1;
                if (!cur.len)
                    cur_rate = 1000000 / (256 - b[2]);
                for (long k = 0; k < cnt; k++)
                    pcm_push(&cur, 0);
            }
            break;
        case 6: /* repeat start */
            FLUSH();
            if (n >= 2) {
                int cnt = b[0] | b[1] << 8;
                cur_repeat = cnt == 0xffff ? 0 : cnt + 1;
                in_repeat = 1;
            }
            break;
        case 7: /* repeat end */
            FLUSH();
            cur_repeat = 1;
            in_repeat = 0;
            break;
        case 8: /* extended: applies to the next sound data block */
            if (n >= 4) {
                int tc = b[0] | b[1] << 8;
                int stereo = b[3];
                ext_rate = (int)(256000000L / (65536 - tc)) / (stereo ? 2 : 1);
                ext_pack = b[2];
            }
            break;
        case 9: /* new format */
            if (n >= 12) {
                int rate = (int)(b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24);
                int bits = b[4];
                int codec = b[6] | b[7] << 8;
                if (!in_repeat && cur.len && rate != cur_rate)
                    FLUSH();
                if (!cur.len)
                    cur_rate = rate;
                if (codec == 0 || codec == 4)
                    decode_block(&cur, b + 12, n - 12, 0, bits == 16);
            }
            break;
        default:
            break;
        }
        i += 4 + (size_t)n;
    }
    FLUSH();
#undef FLUSH
    free(d);
    *out = parts;
    return nparts;
}

int audio_play_voc(const char *path)
{
    Part *parts;
    int n = voc_parts(path, &parts);
    if (!n)
        return -1;
    start_voice(VOICE_MUSIC, parts, n);
    return 0;
}

int16_t *voc_decode_all(const char *path, int *out_len, int *out_rate)
{
    Part *parts;
    int n = voc_parts(path, &parts);
    if (!n)
        return NULL;
    long total = 0;
    for (int k = 0; k < n; k++)
        total += parts[k].frames;
    int16_t *pcm = xmalloc(sizeof(int16_t) * (size_t)(total ? total : 1));
    long o = 0;
    for (int k = 0; k < n; k++) {
        memcpy(pcm + o, parts[k].pcm, sizeof(int16_t) * (size_t)parts[k].frames);
        o += parts[k].frames;
    }
    *out_len = (int)total;
    *out_rate = parts[0].rate;
    free_parts(parts, n);
    return pcm;
}

/* ---- CD audio from a cue sheet ---- */

#define MAX_TRACKS 100

typedef struct {
    char file[1024];
    int audio;
    long data_offset; /* byte offset of the audio data inside the file (WAVE header) */
    long start;       /* INDEX 01, sectors within the file */
    long end;         /* sectors within the file, -1 = end of file */
} CdTrack;

static CdTrack tracks[MAX_TRACKS];
static int cd_ok;

static long wav_data_offset(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    uint8_t h[12];
    long off = -1;
    if (fread(h, 1, 12, f) == 12 && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4)) {
        uint8_t c[8];
        long pos = 12;
        while (fread(c, 1, 8, f) == 8) {
            long n = c[4] | c[5] << 8 | c[6] << 16 | (long)c[7] << 24;
            pos += 8;
            if (!memcmp(c, "data", 4)) {
                off = pos;
                *len = n;
                break;
            }
            pos += n + (n & 1);
            fseek(f, pos, SEEK_SET);
        }
    }
    fclose(f);
    return off;
}

static long file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

int cd_open(const char *cue_path)
{
    size_t size;
    char *buf = (char *)read_file(cue_path, &size);
    if (!buf)
        return -1;
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", cue_path);
    char *slash = strrchr(dir, '/');
#ifdef _WIN32
    char *bslash = strrchr(dir, '\\');
    if (!slash || (bslash && bslash > slash))
        slash = bslash;
#endif
    if (slash)
        slash[1] = 0;
    else
        dir[0] = 0;

    char cur_file[1024] = "";
    int cur_wave = 0, track = 0;
    long wave_off = 0, wave_len = 0;
    memset(tracks, 0, sizeof tracks);
    for (char *line = strtok(buf, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        while (*line == ' ' || *line == '\t')
            line++;
        if (!strncmp(line, "FILE", 4)) {
            char *q1 = strchr(line, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
            char name[512];
            if (q1 && q2) {
                snprintf(name, sizeof name, "%.*s", (int)(q2 - q1 - 1), q1 + 1);
            } else {
                sscanf(line + 4, " %511s", name);
            }
            snprintf(cur_file, sizeof cur_file, "%s%s", dir, name);
            cur_wave = strstr(line, "WAVE") != NULL;
            wave_off = 0;
            if (cur_wave) {
                wave_off = wav_data_offset(cur_file, &wave_len);
                if (wave_off < 0)
                    wave_off = 44;
            }
        } else if (!strncmp(line, "TRACK", 5)) {
            track = atoi(line + 5);
            if (track > 0 && track < MAX_TRACKS) {
                snprintf(tracks[track].file, sizeof tracks[track].file, "%s", cur_file);
                tracks[track].audio = strstr(line, "AUDIO") != NULL;
                tracks[track].data_offset = wave_off;
                tracks[track].start = 0;
                tracks[track].end = -1;
            }
        } else if (!strncmp(line, "INDEX", 5) && track > 0 && track < MAX_TRACKS) {
            int idx, m, s, f;
            if (sscanf(line + 5, "%d %d:%d:%d", &idx, &m, &s, &f) == 4 && idx == 1) {
                tracks[track].start = (m * 60L + s) * 75 + f;
                /* the previous track in the same file ends here */
                if (track > 1 && !strcmp(tracks[track - 1].file, cur_file))
                    tracks[track - 1].end = tracks[track].start;
            }
        }
    }
    free(buf);
    cd_ok = 0;
    for (int t = 1; t < MAX_TRACKS; t++)
        if (tracks[t].audio && file_exists(tracks[t].file))
            cd_ok = 1;
    if (cd_ok)
        log_msg("CD audio: using %s", cue_path);
    return cd_ok ? 0 : -1;
}

int cd_available(void) { return cd_ok; }

void cd_play(int track, long start_frame, long end_frame)
{
    cd_stop();
    if (!cd_ok || track <= 0 || track >= MAX_TRACKS || !tracks[track].audio)
        return;
    CdTrack *t = &tracks[track];
    long fsize = file_size(t->file);
    if (fsize < 0)
        return;
    long track_end = t->end >= 0 ? t->end : (fsize - t->data_offset) / 2352;
    long s = t->start + start_frame;
    long e = end_frame < 0 ? track_end : t->start + end_frame;
    if (e > track_end)
        e = track_end;
    if (e <= s)
        return;
    long bytes = (e - s) * 2352;
    FILE *f = fopen(t->file, "rb");
    if (!f)
        return;
    fseek(f, t->data_offset + s * 2352, SEEK_SET);
    Part *p = xcalloc(1, sizeof *p);
    p->pcm = xmalloc((size_t)bytes);
    size_t got = fread(p->pcm, 1, (size_t)bytes, f);
    fclose(f);
    /* CD audio is little-endian 16-bit stereo */
    uint8_t *raw = (uint8_t *)p->pcm;
    for (size_t k = 0; k + 1 < got; k += 2)
        p->pcm[k / 2] = (int16_t)(raw[k] | raw[k + 1] << 8);
    p->frames = (long)(got / 4);
    if (trace)
        log_msg("CD: track %d, %ld frames from %ld", track, (long)(p->frames / 588), start_frame);
    p->channels = 2;
    p->rate = 44100;
    p->repeat = 1;
    start_voice(VOICE_CD, p, 1);
}

void cd_stop(void) { audio_stop(VOICE_CD); }

/*
 * The shot runtime, following DTVPLAY.EXE:
 *  - a shot shows a picture or ANIM, plays a sound/CD cue and arms its events;
 *  - events are hotspots (button A, polygon test), button B / key triggers, or timers;
 *  - an event picks one of its avenues at random, weighted, among those whose AFRM frame
 *    mask enables the current animation frame;
 *  - an avenue plays its own sound/CD cue and then goes to another shot, returns along
 *    the gosub stack, or quits.
 * All timing runs on the player's 72 Hz clock.
 */
#include "engine.h"

#include "audio.h"
#include "gfx.h"
#include "shot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TIMERS 64
#define STACK_DEPTH 51
#define FADE_HZ 70 /* one palette step per VGA retrace */

enum { FADE_NONE, FADE_OUT, FADE_IN };

typedef struct {
    uint64_t due;
    Event *ev;
} Timer;

static struct {
    EngineOptions opt;
    Shot *shot;
    Graphic *gfx;
    Graphic *ov[3];

    /* animation, see the player's routines at 6020h/61AFh */
    int animated;
    int cur_delta;  /* delta decoded ahead into the hidden buffer */
    int counter;    /* frame counter used by AFRM masks */
    int loops;
    uint64_t anim_due;
    int anim_armed;

    Timer timers[MAX_TIMERS];
    int ntimers;

    char stack[STACK_DEPTH][NAME_LEN * 2];
    int sp;

    int16_t seed;
    uint64_t tick;
    uint64_t ms;

    /* transition */
    int fade;
    uint64_t fade_start_ms;
    int fade_steps;
    int fade_level;
    Graphic *old_gfx;
    Graphic *old_ov[3];

    int quit;
} E;

/* ---- helpers ---- */

static int rnd(int n)
{
    /* seed = (seed * 13 + 19) mod 10001 in 16-bit signed arithmetic */
    int16_t ax = (int16_t)(uint16_t)((uint16_t)E.seed * 13u + 0x13u);
    E.seed = (int16_t)(ax % 10001);
    return n ? E.seed % n : 0;
}

static int frame_enabled(const Avenue *a, int counter)
{
    if (!a->afrm)
        return 1;
    int idx = counter / 8;
    if (idx >= a->afrm_len)
        return 1;
    int bits = (int8_t)a->afrm[idx];
    int sh = 7 - (counter - 1) % 8;
    return (bits >> sh) & 1;
}

/* Weighted random choice among the avenues enabled for the current frame. */
static Avenue *choose_avenue(Event *ev)
{
    int total = 0;
    for (Avenue *a = ev->avenues; a; a = a->next)
        if (frame_enabled(a, E.counter))
            total += a->weight;
    if (!total)
        return NULL;
    int r = rnd(total) + 1;
    int acc = 0;
    for (Avenue *a = ev->avenues; a; a = a->next) {
        if (!frame_enabled(a, E.counter))
            continue;
        acc += a->weight;
        if (acc >= r)
            return a;
    }
    return NULL;
}

static void play_sound(const SoundCmd *s)
{
    if (!s)
        return;
    const char *path;
    switch (s->type) {
    case SND_STOP:
        audio_stop(VOICE_SFX);
        audio_stop(VOICE_MUSIC);
        break;
    case SND_SAMPLE:
    case SND_SAMPLE2:
        if (cd_available())
            cd_stop();
        /* samples and music shared the single SoundBlaster voice: a sample cuts the music */
        audio_stop(VOICE_SFX);
        audio_stop(VOICE_MUSIC);
        path = resolve_path(s->name);
        if (!path || audio_play_sample(path, s->repeat))
            log_msg("sound: cannot play '%s'", s->name);
        break;
    case SND_MUSIC: {
        if (cd_available())
            cd_stop();
        audio_stop(VOICE_MUSIC);
        audio_stop(VOICE_SFX);
        const char *sub = music_substitute(s->name);
        if (!sub) {
            log_msg("music: no substitute for '%s'", s->name);
            break;
        }
        if (sub[0] == '!') {
            path = resolve_town_file(sub + 1);
            if (!path || audio_play_voc(path))
                log_msg("music: cannot play '%s'", sub + 1);
        } else {
            log_msg("music: unsupported substitute '%s'", sub);
        }
        break;
    }
    case SND_MUSIC_FILE:
        audio_stop(VOICE_MUSIC);
        log_msg("music: unsupported music file '%s'", s->name);
        break;
    default:
        break;
    }
}

static void play_cd(const CdCmd *c)
{
    if (!c || !cd_available())
        return;
    cd_stop();
    switch (c->cmd) {
    case 1:
    case 2:
        cd_play(c->track, 0, -1);
        break;
    case 3: {
        long s = (c->start_m * 60L + c->start_s) * 75 + c->start_f;
        long e = (c->end_m * 60L + c->end_s) * 75 + c->end_f;
        if (e > s)
            cd_play(c->track, s, e);
        break;
    }
    default:
        break;
    }
}

static void add_timer(uint64_t due, Event *ev)
{
    if (E.ntimers < MAX_TIMERS) {
        E.timers[E.ntimers].due = due;
        E.timers[E.ntimers].ev = ev;
        E.ntimers++;
    }
}

static void free_display(Graphic **g, Graphic **ov)
{
    gfx_free(*g);
    *g = NULL;
    for (int k = 0; k < 3; k++) {
        gfx_free(ov[k]);
        ov[k] = NULL;
    }
}

/* ---- animation ---- */

static int frame_delay(int first)
{
    if (E.shot->fps > 0)
        return TICK_HZ / E.shot->fps ? TICK_HZ / E.shot->fps : 1;
    uint32_t rel = E.gfx->reltime[E.cur_delta];
    int d = (int)(rel * TICK_HZ / 60);
    if (first) {
        if (d <= 1)
            d += 3;
    } else if (d == 1) {
        d += 6;
    }
    return d > 0 ? d : 1;
}

static void anim_start(void)
{
    anim_rewind(E.gfx);
    if (!E.animated)
        return;
    E.cur_delta = 0;
    E.loops = 0;
    E.counter = 1;
    anim_decode(E.gfx, E.cur_delta);
    E.anim_due = E.tick + (uint64_t)frame_delay(1);
    E.anim_armed = 1;
    if (++E.cur_delta >= E.gfx->ndeltas) {
        E.cur_delta = E.gfx->ndeltas > 1 ? 1 : 0;
        E.loops++;
    }
}

static void anim_tick(void)
{
    E.anim_armed = 0;
    if (!E.animated)
        return;
    anim_flip(E.gfx);
    if (++E.counter > E.shot->last_frame)
        E.counter = 1;
    int delay = frame_delay(0);
    if (E.counter >= E.shot->last_frame)
        E.loops++;
    if (E.shot->loops == 0 || E.loops < E.shot->loops) {
        E.anim_due = E.tick + (uint64_t)delay;
        E.anim_armed = 1;
    }
    anim_decode(E.gfx, E.cur_delta);
    if (++E.cur_delta >= E.gfx->ndeltas)
        E.cur_delta = E.gfx->ndeltas > 1 ? 1 : 0;
}

/* ---- shots ---- */

/* Second half of entering a shot: after the transition has finished. */
static void shot_begin(void)
{
    if (E.animated)
        anim_start();
    else
        E.counter = 1;
    for (Event *ev = E.shot->events; ev; ev = ev->next)
        if (ev->trigger == TRIG_TIMER)
            add_timer(E.tick + (uint64_t)(TICK_HZ * ev->secs) + (uint64_t)(TICK_HZ * ev->tenths / 10), ev);
}

static void render_picture(uint32_t *canvas, Graphic *g, Graphic **ov);

/* The fade out ends once every 6-bit palette component has reached zero. */
static int fade_out_steps(void)
{
    uint32_t *canvas = xcalloc((size_t)SCREEN_W * CANVAS_H, sizeof(uint32_t));
    render_picture(canvas, E.old_gfx, E.old_ov);
    int maxc = 0;
    for (size_t i = 0; i < (size_t)SCREEN_W * CANVAS_H; i++) {
        uint32_t c = canvas[i];
        int r = (c >> 18) & 63, g = (c >> 10) & 63, b = (c >> 2) & 63;
        if (r > maxc)
            maxc = r;
        if (g > maxc)
            maxc = g;
        if (b > maxc)
            maxc = b;
    }
    free(canvas);
    return maxc;
}

static int goto_shot(const char *name)
{
    if (cd_available())
        cd_stop();
    const char *path = resolve_path(name);
    Shot *s = path ? shot_load(path, name) : NULL;
    if (!s) {
        log_msg("shot: cannot load '%s'", name);
        return -1;
    }
    if (E.opt.trace)
        log_msg("shot: %s  [%s]", name, path);

    E.ntimers = 0;
    E.anim_armed = 0;

    /* keep the old picture on screen for the fade out */
    free_display(&E.old_gfx, E.old_ov);
    E.old_gfx = E.gfx;
    memcpy(E.old_ov, E.ov, sizeof E.ov);
    E.gfx = NULL;
    memset(E.ov, 0, sizeof E.ov);
    shot_free(E.shot);
    E.shot = s;

    path = resolve_path(s->graphic);
    E.gfx = path ? gfx_load(path) : NULL;
    if (!E.gfx)
        log_msg("graphic: cannot load '%s'", s->graphic);
    E.animated = E.gfx && E.gfx->ndeltas > 0;
    if (s->last_frame < 0)
        s->last_frame = E.gfx ? E.gfx->ndeltas - 1 : 0;
    for (int k = 0; k < 3; k++) {
        if (!s->overlay[k][0])
            continue;
        path = resolve_path(s->overlay[k]);
        if (path)
            E.ov[k] = gfx_load(path);
    }
    if (E.gfx)
        anim_rewind(E.gfx);

    play_sound(s->snd);
    play_cd(s->cd);

    if (s->transition && !E.opt.no_fades) {
        E.fade = FADE_OUT;
        E.fade_start_ms = E.ms;
        E.fade_level = 0;
        E.fade_steps = fade_out_steps();
    } else {
        free_display(&E.old_gfx, E.old_ov);
        shot_begin();
    }
    return 0;
}

static void exec_avenue(Avenue *a)
{
    if (!a)
        return;
    if (a->action != ACT_NONE && cd_available())
        cd_stop();
    play_sound(a->snd);
    play_cd(a->cd);
    if (a->flags & 1) {
        if (E.sp >= STACK_DEPTH)
            return;
        snprintf(E.stack[E.sp++], sizeof E.stack[0], "%s", E.shot->name);
    }
    switch (a->action) {
    case ACT_GOTO:
        goto_shot(a->target);
        break;
    case ACT_EXEC:
        log_msg("avenue: external program '%s' not supported", a->target);
        break;
    case ACT_RETURN:
        if (E.sp > 0) {
            E.sp--;
            char name[NAME_LEN * 2];
            snprintf(name, sizeof name, "%s", E.stack[E.sp]);
            goto_shot(name);
        }
        break;
    case ACT_QUIT:
    case ACT_RESET:
        E.quit = 1;
        break;
    default:
        break;
    }
}

/* ---- hit testing (the player's polygon test at 6BE9h/6C75h) ---- */

static int edge_cross(int px, int py, int ax, int ay, int bx, int by)
{
    if (ax == bx && ay == by)
        return 0;
    if (ay == by) {
        if (ax > bx) {
            if (ax < px || px < bx)
                return 0;
        } else if (px > bx || px < ax) {
            return 0;
        }
        return ay == py;
    }
    if (ay > by) {
        if (ay < py || py < by)
            return 0;
    } else if (py > by || py < ay) {
        return 0;
    }
    if (ax == bx)
        return ax <= px;
    long xi = ax + (long)(bx - ax) * (py - ay) / (by - ay);
    return xi <= px;
}

static int inside_polygon(const Event *ev, int x, int y)
{
    int n = ev->npts, cross = 0;
    if (n < 2 || !ev->pts)
        return 0;
    for (int i = 0; i < n - 1; i++)
        cross += edge_cross(x, y, ev->pts[i * 2], ev->pts[i * 2 + 1], ev->pts[i * 2 + 2], ev->pts[i * 2 + 3]);
    cross += edge_cross(x, y, ev->pts[(n - 1) * 2], ev->pts[(n - 1) * 2 + 1], ev->pts[0], ev->pts[1]);
    return cross & 1;
}

static Event *hit_event(int x, int y)
{
    for (Event *ev = E.shot->events; ev; ev = ev->next) {
        if (ev->x1 > x || x > ev->x2 || y < ev->y1 || y > ev->y2)
            continue;
        if (inside_polygon(ev, x, y))
            return ev;
    }
    return NULL;
}

/* ---- public API ---- */

int engine_start(const char *start_shot, const EngineOptions *opt)
{
    memset(&E, 0, sizeof E);
    if (opt)
        E.opt = *opt;
    E.seed = 0x1dc7;
    return goto_shot(start_shot);
}

void engine_shutdown(void)
{
    free_display(&E.gfx, E.ov);
    free_display(&E.old_gfx, E.old_ov);
    shot_free(E.shot);
    E.shot = NULL;
}

void engine_seed(int seed) { E.seed = (int16_t)seed; }

void engine_update(uint64_t ms)
{
    E.ms = ms;
    E.tick = ms * TICK_HZ / 1000;
    if (!E.shot)
        return;
    if (E.fade == FADE_OUT) {
        int step = (int)((ms - E.fade_start_ms) * FADE_HZ / 1000);
        E.fade_level = step;
        if (step > E.fade_steps) {
            free_display(&E.old_gfx, E.old_ov);
            E.fade = FADE_IN;
            E.fade_start_ms = ms;
            E.fade_level = 63;
        }
        return;
    }
    if (E.fade == FADE_IN) {
        int step = (int)((ms - E.fade_start_ms) * FADE_HZ / 1000);
        E.fade_level = 63 - step;
        if (E.fade_level <= 0) {
            E.fade_level = 0;
            E.fade = FADE_NONE;
            shot_begin();
        }
        return;
    }
    /* deliver due timers in order; a shot change discards the rest */
    for (int guard = 0; guard < 64 && !E.quit && E.fade == FADE_NONE; guard++) {
        int best = -1;
        uint64_t due = 0;
        for (int i = 0; i < E.ntimers; i++)
            if (E.timers[i].due <= E.tick && (best < 0 || E.timers[i].due < due)) {
                best = i;
                due = E.timers[i].due;
            }
        int anim = E.anim_armed && E.anim_due <= E.tick && (best < 0 || E.anim_due <= due);
        if (anim) {
            anim_tick();
            continue;
        }
        if (best < 0)
            break;
        Event *ev = E.timers[best].ev;
        E.timers[best] = E.timers[--E.ntimers];
        Avenue *a = choose_avenue(ev);
        exec_avenue(a ? a : ev->first);
    }
}

void engine_button(int button, int x, int y)
{
    if (!E.shot || E.fade != FADE_NONE || E.quit)
        return;
    if (button == 0) {
        Event *ev = hit_event(x, y);
        if (ev)
            exec_avenue(choose_avenue(ev));
    } else {
        engine_trigger(TRIG_BUTTON_B);
    }
}

void engine_trigger(int trigger)
{
    if (!E.shot || E.fade != FADE_NONE || E.quit)
        return;
    for (Event *ev = E.shot->events; ev; ev = ev->next)
        if (ev->trigger == trigger) {
            exec_avenue(choose_avenue(ev));
            return;
        }
}

/* The DOS player's mouse pointer (16x11, colour indices 1/3/12). */
static const uint8_t pointer_shape[11][16] = {
    {1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, 12, 12, 12, 12, 12, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, 3, 3, 3, 3, 12, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, 3, 3, 3, 12, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, 3, 3, 3, 3, 12, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, 3, 3, 1, 3, 3, 12, 1, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 1, 1, 0, 1, 3, 3, 12, 1, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 1, 3, 3, 12, 1, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 1, 3, 3, 12, 1, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 1, 3, 3, 1, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0},
};

static void draw_pointer(uint32_t *canvas, int px, int py)
{
    for (int y = 0; y < 11; y++)
        for (int x = 0; x < 16; x++) {
            int c = pointer_shape[y][x];
            int cx = px + x, cy = py + y;
            if (!c || cx < 0 || cx >= SCREEN_W || cy < 0 || cy >= SCREEN_H)
                continue;
            uint32_t rgb = c == 1 ? 0x000000 : c == 3 ? 0xffffff : 0xa0a0a0;
            canvas[(size_t)(cy * 2) * SCREEN_W + cx] = rgb;
            canvas[(size_t)(cy * 2 + 1) * SCREEN_W + cx] = rgb;
        }
}

static void render_picture(uint32_t *canvas, Graphic *g, Graphic **ov)
{
    if (g)
        gfx_render(g, canvas);
    for (int k = 0; k < 3; k++)
        if (ov[k])
            gfx_render_overlay(ov[k], canvas);
}

void engine_render(uint32_t *canvas, int px, int py)
{
    memset(canvas, 0, sizeof(uint32_t) * SCREEN_W * CANVAS_H);
    if (E.fade == FADE_OUT) {
        render_picture(canvas, E.old_gfx, E.old_ov);
    } else {
        render_picture(canvas, E.gfx, E.ov);
        if (E.fade == FADE_NONE && engine_pointer_visible())
            draw_pointer(canvas, px, py);
    }
    if (E.fade != FADE_NONE && E.fade_level > 0) {
        int lv = E.fade_level > 63 ? 63 : E.fade_level;
        for (size_t i = 0; i < (size_t)SCREEN_W * CANVAS_H; i++) {
            uint32_t c = canvas[i];
            int r = ((c >> 18) & 63) - lv, g = ((c >> 10) & 63) - lv, b = ((c >> 2) & 63) - lv;
            r = r < 0 ? 0 : r * 255 / 63;
            g = g < 0 ? 0 : g * 255 / 63;
            b = b < 0 ? 0 : b * 255 / 63;
            canvas[i] = (uint32_t)(r << 16 | g << 8 | b);
        }
    }
}

int engine_wants_quit(void) { return E.quit; }
int engine_busy(void) { return E.fade != FADE_NONE; }

int engine_pointer_visible(void)
{
    /* the player runs with -c: the pointer only shows on shots with input mode 0 */
    return E.shot && E.shot->input_mode == 0;
}

const char *engine_shot_name(void) { return E.shot ? E.shot->name : ""; }

int engine_has_timer(void) { return E.ntimers > 0; }

int engine_random_hotspot(int *x, int *y)
{
    if (!E.shot || E.fade != FADE_NONE)
        return 0;
    int n = 0;
    for (Event *ev = E.shot->events; ev; ev = ev->next)
        if (ev->npts >= 2)
            n++;
    if (!n)
        return 0;
    int pick = rand() % n;
    for (Event *ev = E.shot->events; ev; ev = ev->next) {
        if (ev->npts < 2 || pick--)
            continue;
        for (int tries = 0; tries < 400; tries++) {
            int tx = ev->x1 + (ev->x2 > ev->x1 ? rand() % (ev->x2 - ev->x1 + 1) : 0);
            int ty = ev->y1 + (ev->y2 > ev->y1 ? rand() % (ev->y2 - ev->y1 + 1) : 0);
            if (inside_polygon(ev, tx, ty) && hit_event(tx, ty) == ev) {
                *x = tx;
                *y = ty;
                return 1;
            }
        }
        return 0;
    }
    return 0;
}

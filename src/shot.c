/*
 * Shot file parsers. Two formats exist on the CD:
 *  - "FORM....DTV3": IFF chunks GRPH INPT TEXT CAUD CDTV, then EVNT blocks each followed by
 *    BPNT (hotspot polygon) and AVNE blocks (each followed by AFRM/CAUD/CDTV).
 *    Chunks are never padded.
 *  - "VER2SHOT": an older fixed layout (SCNF CDTV BACK ANMC STEX ...).
 */
#include "shot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SoundCmd *parse_caud(const uint8_t *b, uint32_t n)
{
    if (n < 0x28)
        return NULL;
    SoundCmd *s = xcalloc(1, sizeof *s);
    copy_name(s->name, b, 32);
    s->type = b[0x20];
    s->repeat = rd16be(b + 0x26);
    return s;
}

static CdCmd *parse_cdtv(const uint8_t *b, uint32_t n)
{
    if (n < 0x10)
        return NULL;
    CdCmd *c = xcalloc(1, sizeof *c);
    c->track = rd16be(b);
    c->start_m = b[5];
    c->start_s = b[6];
    c->start_f = b[7];
    c->end_m = b[9];
    c->end_s = b[10];
    c->end_f = b[11];
    c->cmd = b[0x0e];
    return c;
}

static void append_event(Shot *s, Event *e)
{
    Event **pp = &s->events;
    while (*pp)
        pp = &(*pp)->next;
    *pp = e;
}

static void append_avenue(Event *e, Avenue *a)
{
    Avenue **pp = &e->avenues;
    while (*pp)
        pp = &(*pp)->next;
    *pp = a;
    if (!e->first)
        e->first = a;
}

static Shot *parse_dtv3(const uint8_t *d, size_t size)
{
    Shot *s = xcalloc(1, sizeof *s);
    s->last_frame = -1;
    int have_grph = 0, have_inpt = 0;
    Event *ev = NULL;
    Avenue *av = NULL;
    size_t i = 12;
    while (i + 8 <= size) {
        const uint8_t *c = d + i;
        uint32_t n = rd32be(c + 4);
        const uint8_t *b = c + 8;
        if (i + 8 + n > size)
            n = (uint32_t)(size - i - 8);
        if (!memcmp(c, "EOF ", 4))
            break;
        if (!memcmp(c, "GRPH", 4) && n >= 0x8a && !ev) {
            copy_name(s->graphic, b, 32);
            for (int k = 0; k < 3; k++)
                copy_name(s->overlay[k], b + 32 * (k + 1), 32);
            s->transition = b[0x81];
            s->loops = (int16_t)rd16be(b + 0x84);
            s->last_frame = (int16_t)rd16be(b + 0x86);
            s->fps = (int16_t)rd16be(b + 0x88);
            have_grph = 1;
        } else if (!memcmp(c, "INPT", 4) && n >= 1 && !ev) {
            s->input_mode = b[0];
            have_inpt = 1;
        } else if (!memcmp(c, "EVNT", 4) && n >= 0x10) {
            ev = xcalloc(1, sizeof *ev);
            ev->trigger = b[0];
            ev->npts = (int16_t)rd16be(b + 2);
            ev->x1 = (int16_t)rd16be(b + 4);
            ev->y1 = (int16_t)rd16be(b + 6);
            ev->x2 = (int16_t)rd16be(b + 8);
            ev->y2 = (int16_t)rd16be(b + 10);
            ev->secs = (int16_t)rd16be(b + 12);
            ev->tenths = (int16_t)rd16be(b + 14);
            append_event(s, ev);
            av = NULL;
        } else if (!memcmp(c, "BPNT", 4) && ev && !av) {
            int np = ev->npts;
            if (np > 0 && (uint32_t)np * 4 <= n) {
                ev->pts = xcalloc((size_t)np * 2, sizeof(int16_t));
                for (int k = 0; k < np * 2; k++)
                    ev->pts[k] = (int16_t)rd16be(b + k * 2);
            } else {
                ev->npts = 0;
            }
        } else if (!memcmp(c, "AVNE", 4) && ev && n >= 0x24) {
            av = xcalloc(1, sizeof *av);
            copy_name(av->target, b, 32);
            av->action = b[0x20];
            av->flags = b[0x21];
            av->weight = (int16_t)rd16be(b + 0x22);
            append_avenue(ev, av);
        } else if (!memcmp(c, "AFRM", 4) && av) {
            av->afrm = xmalloc(n ? n : 1);
            memcpy(av->afrm, b, n);
            av->afrm_len = (int)n;
        } else if (!memcmp(c, "CAUD", 4)) {
            SoundCmd *snd = parse_caud(b, n);
            if (av) {
                free(av->snd);
                av->snd = snd;
            } else if (!ev) {
                free(s->snd);
                s->snd = snd;
            } else {
                free(snd);
            }
        } else if (!memcmp(c, "CDTV", 4)) {
            CdCmd *cd = parse_cdtv(b, n);
            if (av) {
                free(av->cd);
                av->cd = cd;
            } else if (!ev) {
                free(s->cd);
                s->cd = cd;
            } else {
                free(cd);
            }
        }
        i += 8 + n;
    }
    if (!have_grph || !have_inpt) {
        shot_free(s);
        return NULL;
    }
    return s;
}

/* VER2 avenue names: "^name" pushes a return, keywords map to actions, "!prog" runs a program. */
static Avenue *v2_avenue(const char *name)
{
    Avenue *a = xcalloc(1, sizeof *a);
    a->weight = 1;
    if (*name == '^') {
        name++;
        a->flags |= 1;
    }
    if (!strcmp(name, "None"))
        a->action = ACT_NONE;
    else if (!strcmp(name, "Return"))
        a->action = ACT_RETURN;
    else if (!strcmp(name, "Quit"))
        a->action = ACT_QUIT;
    else if (!strcmp(name, "Reset"))
        a->action = ACT_RESET;
    else if (*name == '!') {
        a->action = ACT_EXEC;
        snprintf(a->target, sizeof a->target, "%s", name + 1);
    } else {
        a->action = ACT_GOTO;
        snprintf(a->target, sizeof a->target, "%s", name);
    }
    return a;
}

static Shot *parse_ver2(const uint8_t *d, size_t size)
{
    if (size < 16 + 0x10a || memcmp(d + 8, "SCNF", 4))
        return NULL;
    Shot *s = xcalloc(1, sizeof *s);
    const uint8_t *b = d + 16;
    char tmp[NAME_LEN], snd1[NAME_LEN], snd2[NAME_LEN];

    copy_name(s->graphic, b + 0x20, 32);
    for (int k = 0; k < 3; k++) {
        copy_name(tmp, b + 0x40 + 32 * k, 32);
        if (strcmp(tmp, "None"))
            snprintf(s->overlay[k], NAME_LEN, "%s", tmp);
    }
    s->transition = b[0x109];
    s->last_frame = -1;

    copy_name(snd1, b + 0xa0, 32);
    copy_name(snd2, b + 0xc4, 32);
    if (!strcmp(snd1, "Stop") || !strcmp(snd2, "Stop")) {
        s->snd = xcalloc(1, sizeof *s->snd);
        s->snd->type = SND_STOP;
    } else {
        if (strcmp(snd1, "None")) {
            s->snd = xcalloc(1, sizeof *s->snd);
            snprintf(s->snd->name, NAME_LEN, "%s", snd1);
            s->snd->type = SND_SAMPLE2;
            s->snd->repeat = rd16be(b + 0xc0);
        }
        if (strcmp(snd2, "None")) {
            if (!s->snd)
                s->snd = xcalloc(1, sizeof *s->snd);
            snprintf(s->snd->name, NAME_LEN, "%s", snd2);
            s->snd->type = SND_MUSIC;
        }
    }

    copy_name(tmp, b + 0xe8, 32);
    int secs = (int16_t)rd16be(b + 0xe4), tenths = (int16_t)rd16be(b + 0xe6);
    if (strcmp(tmp, "None") && (secs || tenths)) {
        Event *ev = xcalloc(1, sizeof *ev);
        ev->trigger = TRIG_TIMER;
        ev->secs = secs;
        ev->tenths = tenths;
        append_avenue(ev, v2_avenue(tmp));
        append_event(s, ev);
    }

    size_t i = 16 + 0x10a;
    while (i + 8 <= size) {
        const uint8_t *c = d + i;
        uint32_t n = rd32be(c + 4);
        const uint8_t *p = c + 8;
        if (i + 8 + n > size)
            n = (uint32_t)(size - i - 8);
        if (!memcmp(c, "EOF ", 4))
            break;
        if (!memcmp(c, "CDTV", 4) && n >= 0x10) {
            uint8_t cd[0x1c] = {0};
            memcpy(cd, p, n < sizeof cd ? n : sizeof cd);
            if (n <= 0x1c && cd[0x0a] == '0')
                memset(cd + 4, 0, 8);
            CdCmd *cmd = parse_cdtv(cd, sizeof cd);
            int any_msf = 0;
            for (int k = 4; k < 12; k++)
                any_msf |= cd[k];
            if (!cmd->track)
                cmd->cmd = 0;
            else if (rd16be(cd + 2))
                cmd->cmd = 2;
            else
                cmd->cmd = any_msf ? 3 : 1;
            s->cd = cmd;
        } else if (!memcmp(c, "BACK", 4) && n >= 32) {
            copy_name(tmp, p, 32);
            if (strcmp(tmp, "None")) {
                Event *ev = xcalloc(1, sizeof *ev);
                ev->trigger = TRIG_BUTTON_B;
                append_avenue(ev, v2_avenue(tmp));
                append_event(s, ev);
            }
        } else if (!memcmp(c, "ANMC", 4) && n >= 4) {
            s->loops = (int16_t)rd16be(p);
            int iv = rd16be(p + 2);
            if (iv)
                s->fps = 10 / iv;
        }
        i += 8 + n;
    }
    return s;
}

Shot *shot_load(const char *path, const char *name)
{
    size_t size;
    uint8_t *d = read_file(path, &size);
    if (!d)
        return NULL;
    Shot *s = NULL;
    if (size >= 12 && !memcmp(d, "FORM", 4) && !memcmp(d + 8, "DTV3", 4)) {
        uint32_t formlen = rd32be(d + 4);
        if ((size_t)formlen + 8 < size)
            size = (size_t)formlen + 8;
        s = parse_dtv3(d, size);
    } else if (size >= 8 && !memcmp(d, "VER2SHOT", 8)) {
        s = parse_ver2(d, size);
    }
    free(d);
    if (s)
        snprintf(s->name, sizeof s->name, "%s", name);
    return s;
}

static void free_avenue(Avenue *a)
{
    while (a) {
        Avenue *n = a->next;
        free(a->afrm);
        free(a->snd);
        free(a->cd);
        free(a);
        a = n;
    }
}

void shot_free(Shot *s)
{
    if (!s)
        return;
    Event *e = s->events;
    while (e) {
        Event *n = e->next;
        free(e->pts);
        free_avenue(e->avenues);
        free(e);
        e = n;
    }
    free(s->snd);
    free(s->cd);
    free(s);
}

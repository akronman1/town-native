#ifndef TOWN_GFX_H
#define TOWN_GFX_H

#include "common.h"

/* An IFF ILBM picture or ANIM (op 5) animation, kept in planar form like the
 * original so deltas can be applied directly. */
typedef struct {
    int w, h, planes;
    int rowbytes;
    int ham, ehb, lace, hires;
    int transparent; /* BMHD transparentColor */
    uint32_t pal[256];

    /* animation */
    int ndeltas;
    const uint8_t **delta;   /* DLTA payloads (point into file) */
    uint32_t *delta_len;
    uint32_t *reltime;       /* ANHD reltime per delta, in 1/60 s */
    const uint8_t **dcmap;   /* optional CMAP per delta frame */
    uint32_t *dcmap_len;
    uint8_t *file;

    uint8_t *buf[2];  /* planar frame buffers; buf[shown] is visible */
    int shown;
    uint8_t *chunky;  /* cached chunky version of the visible buffer */
    int chunky_valid;
} Graphic;

Graphic *gfx_load(const char *path);
void gfx_free(Graphic *g);

/* Animation playback, mirroring the DOS player's decode-ahead double buffer. */
void anim_rewind(Graphic *g);           /* both buffers = frame 0, frame 0 visible */
void anim_decode(Graphic *g, int delta); /* apply delta into the hidden buffer */
void anim_flip(Graphic *g);             /* show the hidden buffer */

/* Chunky pixels of the visible buffer (w*h). */
const uint8_t *gfx_chunky(Graphic *g);

/* Render the visible picture onto a SCREEN_W x CANVAS_H RGB canvas. */
void gfx_render(Graphic *g, uint32_t *canvas);
/* Draw an overlay: pixels different from the transparent colour are drawn with the
 * overlay's own palette. */
void gfx_render_overlay(Graphic *ov, uint32_t *canvas);

#endif

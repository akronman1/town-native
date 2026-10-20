#include "gfx.h"

#include <stdlib.h>
#include <string.h>

#define CAMG_LACE 0x0004
#define CAMG_EHB 0x0080
#define CAMG_HAM 0x0800
#define CAMG_HIRES 0x8000

typedef struct {
    const uint8_t *bmhd, *cmap, *camg, *body;
    uint32_t cmap_len, body_len;
} IlbmChunks;

static void scan_ilbm(const uint8_t *p, const uint8_t *end, IlbmChunks *c,
                      const uint8_t **dlta, uint32_t *dlta_len, uint32_t *reltime)
{
    while (p + 8 <= end) {
        uint32_t n = rd32be(p + 4);
        const uint8_t *d = p + 8;
        if (d + n > end)
            n = (uint32_t)(end - d);
        if (!memcmp(p, "BMHD", 4) && n >= 20)
            c->bmhd = d;
        else if (!memcmp(p, "CMAP", 4)) {
            c->cmap = d;
            c->cmap_len = n;
        } else if (!memcmp(p, "CAMG", 4) && n >= 4)
            c->camg = d;
        else if (!memcmp(p, "BODY", 4)) {
            c->body = d;
            c->body_len = n;
        } else if (!memcmp(p, "ANHD", 4) && reltime && n >= 18) {
            *reltime = rd32be(d + 14);
        } else if (!memcmp(p, "DLTA", 4) && dlta) {
            *dlta = d;
            *dlta_len = n;
        }
        p = d + n + (n & 1);
    }
}

static void load_cmap(uint32_t *pal, const uint8_t *cmap, uint32_t len)
{
    int n = (int)(len / 3);
    if (n > 256)
        n = 256;
    /* Amiga 12-bit colours are stored as 0xX0; stretch them to the full range */
    int low_nibbles = 0;
    for (int i = 0; i < n * 3; i++)
        low_nibbles |= cmap[i] & 0x0f;
    for (int i = 0; i < n; i++) {
        uint32_t r = cmap[i * 3], g = cmap[i * 3 + 1], b = cmap[i * 3 + 2];
        if (!low_nibbles) {
            r |= r >> 4;
            g |= g >> 4;
            b |= b >> 4;
        }
        pal[i] = r << 16 | g << 8 | b;
    }
}

/* ByteRun1 decode of the BODY into planar layout buf[(plane*h + y)*rowbytes]. */
static void decode_body(Graphic *g, const uint8_t *src, uint32_t len, int compression, int has_mask)
{
    const uint8_t *end = src + len;
    int nplanes = g->planes + (has_mask ? 1 : 0);
    uint8_t *row = xmalloc((size_t)g->rowbytes);
    for (int y = 0; y < g->h; y++) {
        for (int p = 0; p < nplanes; p++) {
            if (compression == 1) {
                int o = 0;
                while (o < g->rowbytes && src < end) {
                    int8_t c = (int8_t)*src++;
                    if (c >= 0) {
                        int cnt = c + 1;
                        while (cnt-- && src < end) {
                            if (o < g->rowbytes)
                                row[o] = *src;
                            o++;
                            src++;
                        }
                    } else if (c != -128) {
                        int cnt = -c + 1;
                        uint8_t v = src < end ? *src++ : 0;
                        while (cnt--) {
                            if (o < g->rowbytes)
                                row[o] = v;
                            o++;
                        }
                    }
                }
            } else {
                for (int o = 0; o < g->rowbytes; o++)
                    row[o] = src < end ? *src++ : 0;
            }
            if (p < g->planes)
                memcpy(g->buf[0] + ((size_t)p * g->h + y) * g->rowbytes, row, (size_t)g->rowbytes);
        }
    }
    free(row);
}

static int setup_ilbm(Graphic *g, const IlbmChunks *c)
{
    if (!c->bmhd || !c->body)
        return -1;
    g->w = rd16be(c->bmhd);
    g->h = rd16be(c->bmhd + 2);
    g->planes = c->bmhd[8];
    int masking = c->bmhd[9];
    int compression = c->bmhd[10];
    g->transparent = rd16be(c->bmhd + 12);
    if (g->w <= 0 || g->h <= 0 || g->planes <= 0 || g->planes > 8 || g->w > 2048 || g->h > 2048)
        return -1;
    g->rowbytes = ((g->w + 15) / 16) * 2;
    uint32_t camg = c->camg ? rd32be(c->camg) : 0;
    g->ham = (camg & CAMG_HAM) && g->planes == 6;
    g->ehb = (camg & CAMG_EHB) && g->planes == 6 && !g->ham;
    g->lace = (camg & CAMG_LACE) || g->h >= 400;
    g->hires = (camg & CAMG_HIRES) || g->w >= 640;
    for (int i = 0; i < 256; i++)
        g->pal[i] = (i & 1) ? 0xffffff : 0;
    if (c->cmap)
        load_cmap(g->pal, c->cmap, c->cmap_len);
    if (g->ehb)
        for (int i = 0; i < 32; i++)
            g->pal[i + 32] = (g->pal[i] >> 1) & 0x7f7f7f;
    size_t bufsize = (size_t)g->rowbytes * g->h * g->planes;
    g->buf[0] = xcalloc(bufsize, 1);
    g->buf[1] = xcalloc(bufsize, 1);
    g->chunky = xcalloc((size_t)g->w * g->h, 1);
    decode_body(g, c->body, c->body_len, compression, masking == 1);
    memcpy(g->buf[1], g->buf[0], bufsize);
    return 0;
}

Graphic *gfx_load(const char *path)
{
    size_t size;
    uint8_t *data = read_file(path, &size);
    if (!data)
        return NULL;
    if (size < 12 || memcmp(data, "FORM", 4)) {
        free(data);
        return NULL;
    }
    const uint8_t *end = data + size;
    uint32_t formlen = rd32be(data + 4);
    if (8 + (size_t)formlen < size)
        end = data + 8 + formlen;
    Graphic *g = xcalloc(1, sizeof *g);
    g->file = data;
    if (!memcmp(data + 8, "ILBM", 4)) {
        IlbmChunks c = {0};
        scan_ilbm(data + 12, end, &c, NULL, NULL, NULL);
        if (setup_ilbm(g, &c)) {
            gfx_free(g);
            return NULL;
        }
        return g;
    }
    if (memcmp(data + 8, "ANIM", 4)) {
        gfx_free(g);
        return NULL;
    }
    /* count the frames first */
    int nforms = 0;
    for (const uint8_t *p = data + 12; p + 8 <= end;) {
        uint32_t n = rd32be(p + 4);
        if (!memcmp(p, "FORM", 4))
            nforms++;
        p += 8 + n + (n & 1);
    }
    if (nforms < 1) {
        gfx_free(g);
        return NULL;
    }
    g->delta = xcalloc((size_t)nforms, sizeof *g->delta);
    g->delta_len = xcalloc((size_t)nforms, sizeof *g->delta_len);
    g->reltime = xcalloc((size_t)nforms, sizeof *g->reltime);
    g->dcmap = xcalloc((size_t)nforms, sizeof *g->dcmap);
    g->dcmap_len = xcalloc((size_t)nforms, sizeof *g->dcmap_len);
    int frame = 0;
    for (const uint8_t *p = data + 12; p + 8 <= end;) {
        uint32_t n = rd32be(p + 4);
        const uint8_t *fend = p + 8 + n;
        if (fend > end)
            fend = end;
        if (!memcmp(p, "FORM", 4) && p + 12 <= end) {
            IlbmChunks c = {0};
            const uint8_t *dl = NULL;
            uint32_t dl_len = 0, rel = 1;
            scan_ilbm(p + 12, fend, &c, &dl, &dl_len, &rel);
            if (frame == 0) {
                if (setup_ilbm(g, &c)) {
                    gfx_free(g);
                    return NULL;
                }
            } else if (dl) {
                g->delta[g->ndeltas] = dl;
                g->delta_len[g->ndeltas] = dl_len;
                g->reltime[g->ndeltas] = rel;
                g->dcmap[g->ndeltas] = c.cmap;
                g->dcmap_len[g->ndeltas] = c.cmap_len;
                g->ndeltas++;
            }
            frame++;
        }
        p += 8 + n + (n & 1);
    }
    return g;
}

void gfx_free(Graphic *g)
{
    if (!g)
        return;
    free(g->buf[0]);
    free(g->buf[1]);
    free(g->chunky);
    free(g->delta);
    free(g->delta_len);
    free(g->reltime);
    free(g->dcmap);
    free(g->dcmap_len);
    free(g->file);
    free(g);
}

void anim_rewind(Graphic *g)
{
    size_t bufsize = (size_t)g->rowbytes * g->h * g->planes;
    memcpy(g->buf[1], g->buf[0], bufsize);
    g->shown = 0;
    g->chunky_valid = 0;
}

/* ANIM op 5: byte vertical delta, one column list per plane. */
static void apply_op5(Graphic *g, uint8_t *planes, const uint8_t *dl, uint32_t len)
{
    if (len < 64)
        return;
    const uint8_t *end = dl + len;
    for (int p = 0; p < g->planes && p < 8; p++) {
        uint32_t off = rd32be(dl + p * 4);
        if (!off || off >= len)
            continue;
        const uint8_t *s = dl + off;
        uint8_t *plane = planes + (size_t)p * g->h * g->rowbytes;
        for (int col = 0; col < g->rowbytes && s < end; col++) {
            int ops = *s++;
            int y = 0;
            while (ops-- && s < end) {
                int op = *s++;
                if (op == 0) {
                    if (s + 2 > end)
                        return;
                    int cnt = s[0];
                    uint8_t v = s[1];
                    s += 2;
                    while (cnt--) {
                        if (y < g->h)
                            plane[(size_t)y * g->rowbytes + col] = v;
                        y++;
                    }
                } else if (!(op & 0x80)) {
                    y += op; /* skip */
                } else {
                    op &= 0x7f; /* literal bytes */
                    while (op-- && s < end) {
                        if (y < g->h)
                            plane[(size_t)y * g->rowbytes + col] = *s;
                        s++;
                        y++;
                    }
                }
            }
        }
    }
}

void anim_decode(Graphic *g, int delta)
{
    if (delta < 0 || delta >= g->ndeltas)
        return;
    apply_op5(g, g->buf[g->shown ^ 1], g->delta[delta], g->delta_len[delta]);
    if (g->dcmap[delta])
        load_cmap(g->pal, g->dcmap[delta], g->dcmap_len[delta]);
}

void anim_flip(Graphic *g)
{
    g->shown ^= 1;
    g->chunky_valid = 0;
}

const uint8_t *gfx_chunky(Graphic *g)
{
    if (g->chunky_valid)
        return g->chunky;
    const uint8_t *planes = g->buf[g->shown];
    size_t plane_size = (size_t)g->h * g->rowbytes;
    for (int y = 0; y < g->h; y++) {
        uint8_t *out = g->chunky + (size_t)y * g->w;
        memset(out, 0, (size_t)g->w);
        for (int p = 0; p < g->planes; p++) {
            const uint8_t *row = planes + p * plane_size + (size_t)y * g->rowbytes;
            uint8_t bit = (uint8_t)(1 << p);
            for (int x = 0; x < g->w; x++)
                if (row[x >> 3] & (0x80 >> (x & 7)))
                    out[x] |= bit;
        }
    }
    g->chunky_valid = 1;
    return g->chunky;
}

/* Convert one source row to RGB, resolving HAM. */
static void row_rgb(const Graphic *g, const uint8_t *src, uint32_t *dst)
{
    if (!g->ham) {
        for (int x = 0; x < g->w; x++)
            dst[x] = g->pal[src[x]];
        return;
    }
    uint32_t c = g->pal[0];
    for (int x = 0; x < g->w; x++) {
        int v = src[x] & 0x0f;
        uint32_t v8 = (uint32_t)(v << 4 | v);
        switch (src[x] >> 4) {
        case 0: c = g->pal[v]; break;
        case 1: c = (c & 0xffff00) | v8; break;
        case 2: c = (c & 0x00ffff) | v8 << 16; break;
        case 3: c = (c & 0xff00ff) | v8 << 8; break;
        }
        dst[x] = c;
    }
}

static uint32_t avg2(uint32_t a, uint32_t b)
{
    return ((a & 0xfefefe) >> 1) + ((b & 0xfefefe) >> 1);
}

static void put_row(const Graphic *g, int y, const uint32_t *rgb, const uint8_t *mask, uint32_t *canvas)
{
    int y0, ny;
    if (g->lace) {
        y0 = y;
        ny = 1;
    } else {
        y0 = y * 2;
        ny = 2;
    }
    for (int r = 0; r < ny; r++) {
        int cy = y0 + r;
        if (cy < 0 || cy >= CANVAS_H)
            continue;
        uint32_t *out = canvas + (size_t)cy * SCREEN_W;
        for (int cx = 0; cx < SCREEN_W; cx++) {
            if (g->hires) {
                int sx = cx * 2;
                if (sx + 1 >= g->w)
                    break;
                if (mask && !mask[sx] && !mask[sx + 1])
                    continue;
                out[cx] = avg2(rgb[sx], rgb[sx + 1]);
            } else {
                if (cx >= g->w)
                    break;
                if (mask && !mask[cx])
                    continue;
                out[cx] = rgb[cx];
            }
        }
    }
}

void gfx_render(Graphic *g, uint32_t *canvas)
{
    const uint8_t *px = gfx_chunky(g);
    uint32_t *rgb = xmalloc(sizeof(uint32_t) * (size_t)g->w);
    for (int y = 0; y < g->h; y++) {
        row_rgb(g, px + (size_t)y * g->w, rgb);
        put_row(g, y, rgb, NULL, canvas);
    }
    free(rgb);
}

void gfx_render_overlay(Graphic *ov, uint32_t *canvas)
{
    const uint8_t *px = gfx_chunky(ov);
    uint32_t *rgb = xmalloc(sizeof(uint32_t) * (size_t)ov->w);
    uint8_t *mask = xmalloc((size_t)ov->w);
    for (int y = 0; y < ov->h; y++) {
        const uint8_t *row = px + (size_t)y * ov->w;
        row_rgb(ov, row, rgb);
        for (int x = 0; x < ov->w; x++)
            mask[x] = row[x] != 0;
        put_row(ov, y, rgb, mask, canvas);
    }
    free(mask);
    free(rgb);
}

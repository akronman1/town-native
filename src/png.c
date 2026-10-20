/* Minimal PNG writer (stored deflate blocks) for screenshots. */
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t crc_table[256];

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n)
{
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
    crc = ~crc;
    while (n--)
        crc = crc_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

static void put32(FILE *f, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    fwrite(b, 1, 4, f);
}

static void chunk(FILE *f, const char *type, const uint8_t *data, size_t n)
{
    put32(f, (uint32_t)n);
    fwrite(type, 1, 4, f);
    if (n)
        fwrite(data, 1, n, f);
    uint32_t crc = crc32_update(0, (const uint8_t *)type, 4);
    crc = crc32_update(crc, data, n);
    put32(f, crc);
}

int write_png_rgb(const char *path, const uint32_t *rgb, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13] = {0};
    ihdr[0] = (uint8_t)(w >> 24);
    ihdr[1] = (uint8_t)(w >> 16);
    ihdr[2] = (uint8_t)(w >> 8);
    ihdr[3] = (uint8_t)w;
    ihdr[4] = (uint8_t)(h >> 24);
    ihdr[5] = (uint8_t)(h >> 16);
    ihdr[6] = (uint8_t)(h >> 8);
    ihdr[7] = (uint8_t)h;
    ihdr[8] = 8; /* bit depth */
    ihdr[9] = 2; /* RGB */
    chunk(f, "IHDR", ihdr, 13);

    size_t raw_len = (size_t)h * (1 + (size_t)w * 3);
    uint8_t *raw = xmalloc(raw_len);
    size_t o = 0;
    for (int y = 0; y < h; y++) {
        raw[o++] = 0;
        for (int x = 0; x < w; x++) {
            uint32_t c = rgb[(size_t)y * w + x];
            raw[o++] = (uint8_t)(c >> 16);
            raw[o++] = (uint8_t)(c >> 8);
            raw[o++] = (uint8_t)c;
        }
    }
    size_t nblocks = (raw_len + 65534) / 65535;
    size_t z_len = 2 + raw_len + nblocks * 5 + 4;
    uint8_t *z = xmalloc(z_len);
    size_t zo = 0;
    z[zo++] = 0x78;
    z[zo++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_len; i++) {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
    for (size_t pos = 0; pos < raw_len; pos += 65535) {
        size_t n = raw_len - pos < 65535 ? raw_len - pos : 65535;
        z[zo++] = pos + n >= raw_len ? 1 : 0;
        z[zo++] = (uint8_t)n;
        z[zo++] = (uint8_t)(n >> 8);
        z[zo++] = (uint8_t)~n;
        z[zo++] = (uint8_t)(~n >> 8);
        memcpy(z + zo, raw + pos, n);
        zo += n;
    }
    uint32_t adler = b << 16 | a;
    z[zo++] = (uint8_t)(adler >> 24);
    z[zo++] = (uint8_t)(adler >> 16);
    z[zo++] = (uint8_t)(adler >> 8);
    z[zo++] = (uint8_t)adler;
    chunk(f, "IDAT", z, zo);
    chunk(f, "IEND", NULL, 0);
    free(raw);
    free(z);
    fclose(f);
    return 0;
}

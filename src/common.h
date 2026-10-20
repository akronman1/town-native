/*
 * Town With No Name - native port of the DTV shot player (DTVPLAY.EXE).
 * Shared declarations.
 */
#ifndef TOWN_COMMON_H
#define TOWN_COMMON_H

#include <stdint.h>
#include <stddef.h>

#define SCREEN_W 320
#define SCREEN_H 200
#define CANVAS_H 400 /* output canvas: every lores line doubled, interlaced images native */

#define TICK_HZ 72 /* the DOS player runs all its timers at 72 Hz */

/* ---- util.c ---- */
void log_msg(const char *fmt, ...);
void fatal(const char *fmt, ...);
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t m);
uint8_t *read_file(const char *path, size_t *size);
int file_exists(const char *path);
uint16_t rd16be(const uint8_t *p);
uint32_t rd32be(const uint8_t *p);
void str_lower(char *s);
int str_ieq(const char *a, const char *b);
void copy_name(char *dst, const uint8_t *src, size_t max); /* NUL-terminated copy of a fixed field */

/* ---- paths.c ---- */
int paths_init(const char *data_dir, const char *language);
/* Resolve an Amiga/DTV style name ("DUNE:OUT/Shot0.01", "LANGUAGE_DIR:OVout01")
 * to a host file path. Returns NULL if the file does not exist. Result is a static buffer. */
const char *resolve_path(const char *name);
/* MUSIC.SUB lookup: returns substitute entry ("!intotpk.voc") or NULL. */
const char *music_substitute(const char *name);
/* Resolve a file relative to the PC TOWN directory. */
const char *resolve_town_file(const char *name);
const char *data_root(void);

/* ---- png.c ---- */
int write_png_rgb(const char *path, const uint32_t *rgb, int w, int h);

#endif

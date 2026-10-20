/*
 * File name resolution.
 *
 * The shot scripts use AmigaDOS style names ("DUNE:OUT/Shot0.01", "DTV:JUSTIN/spacedoors",
 * "LANGUAGE_DIR:OVout01"). The DOS player strips the volume, substitutes LANGUAGE_DIR and
 * applies the FILES.TRN / FILES.MAP tables from its TOWN directory. The CD itself carries
 * both the PC renamed copies (under TOWN) and the original long Amiga names, so we try the
 * PC mapping first and fall back to the original names. All lookups are case-insensitive.
 */
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

typedef struct {
    char *key;   /* lower-case path relative to the data root, '/' separated */
    char *value; /* host path */
} Entry;

typedef struct {
    Entry *slots;
    size_t cap, count;
} Map;

static Map files_index;  /* every file of the CD */
static Map files_map;    /* FILES.MAP */
static Map files_trn;    /* FILES.TRN */
static Map music_sub;    /* MUSIC.SUB */
static Map basenames;    /* file name -> host path, or "" when ambiguous */
static char root_dir[1024];
static char language[64] = "english";

static uint32_t hash_str(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++)
        h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

static void map_insert(Map *m, const char *key, const char *value, int overwrite)
{
    if ((m->count + 1) * 2 > m->cap) {
        Map n = {0};
        n.cap = m->cap ? m->cap * 2 : 256;
        n.slots = xcalloc(n.cap, sizeof(Entry));
        for (size_t i = 0; i < m->cap; i++) {
            if (!m->slots[i].key)
                continue;
            size_t j = hash_str(m->slots[i].key) & (n.cap - 1);
            while (n.slots[j].key)
                j = (j + 1) & (n.cap - 1);
            n.slots[j] = m->slots[i];
            n.count++;
        }
        free(m->slots);
        *m = n;
    }
    char *k = xmalloc(strlen(key) + 1);
    strcpy(k, key);
    str_lower(k);
    size_t j = hash_str(k) & (m->cap - 1);
    while (m->slots[j].key) {
        if (!strcmp(m->slots[j].key, k)) {
            free(k);
            if (overwrite) {
                free(m->slots[j].value);
                m->slots[j].value = xmalloc(strlen(value) + 1);
                strcpy(m->slots[j].value, value);
            }
            return; /* otherwise the first entry wins, like a sequential table scan */
        }
        j = (j + 1) & (m->cap - 1);
    }
    m->slots[j].key = k;
    m->slots[j].value = xmalloc(strlen(value) + 1);
    strcpy(m->slots[j].value, value);
    m->count++;
}

static void map_put(Map *m, const char *key, const char *value) { map_insert(m, key, value, 0); }

static const char *map_get(const Map *m, const char *key)
{
    if (!m->cap)
        return NULL;
    char k[1024];
    snprintf(k, sizeof k, "%s", key);
    str_lower(k);
    size_t j = hash_str(k) & (m->cap - 1);
    while (m->slots[j].key) {
        if (!strcmp(m->slots[j].key, k))
            return m->slots[j].value;
        j = (j + 1) & (m->cap - 1);
    }
    return NULL;
}

static void add_file(const char *rel, const char *host, const char *name)
{
    map_put(&files_index, rel, host);
    /* names found in more than one directory are ambiguous and not used */
    map_insert(&basenames, name, map_get(&basenames, name) ? "" : host, 1);
}

static void index_dir(const char *host, const char *rel, int depth)
{
    if (depth > 8)
        return;
#ifdef _WIN32
    char pattern[1200];
    snprintf(pattern, sizeof pattern, "%s\\*", host);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        const char *name = fd.cFileName;
        if (!strcmp(name, ".") || !strcmp(name, ".."))
            continue;
        char hpath[1200], rpath[1200];
        snprintf(hpath, sizeof hpath, "%s\\%s", host, name);
        snprintf(rpath, sizeof rpath, "%s%s%s", rel, *rel ? "/" : "", name);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            index_dir(hpath, rpath, depth + 1);
        else
            add_file(rpath, hpath, name);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(host);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        const char *name = e->d_name;
        if (!strcmp(name, ".") || !strcmp(name, ".."))
            continue;
        char hpath[1200], rpath[1200];
        snprintf(hpath, sizeof hpath, "%s/%s", host, name);
        snprintf(rpath, sizeof rpath, "%s%s%s", rel, *rel ? "/" : "", name);
        struct stat st;
        if (stat(hpath, &st))
            continue;
        if (S_ISDIR(st.st_mode))
            index_dir(hpath, rpath, depth + 1);
        else
            add_file(rpath, hpath, name);
    }
    closedir(d);
#endif
}

static const char *lookup_rel(const char *rel)
{
    char buf[512];
    size_t o = 0;
    /* normalise separators and collapse duplicate slashes */
    for (const char *p = rel; *p && o < sizeof buf - 1; p++) {
        char c = *p == '\\' ? '/' : *p;
        if (c == '/' && (o == 0 || buf[o - 1] == '/'))
            continue;
        buf[o++] = c;
    }
    buf[o] = 0;
    return map_get(&files_index, buf);
}

/* Load "key<sep>value" lines; sep is '=' for the maps and ' ' for MUSIC.SUB. */
static void load_table(Map *m, const char *file, char sep)
{
    const char *path = lookup_rel(file);
    if (!path)
        return;
    size_t size;
    char *buf = (char *)read_file(path, &size);
    if (!buf)
        return;
    char *line = buf;
    while (line && *line) {
        char *next = strpbrk(line, "\r\n");
        if (next) {
            *next++ = 0;
            while (*next == '\r' || *next == '\n')
                next++;
        }
        char *s = strchr(line, sep);
        if (s) {
            *s++ = 0;
            while (*s == ' ' || *s == '\t')
                s++;
            char *e = line + strlen(line);
            while (e > line && (e[-1] == ' ' || e[-1] == '\t'))
                *--e = 0;
            if (*line && *s)
                map_put(m, line, s);
        }
        line = next;
    }
    free(buf);
}

int paths_init(const char *data_dir, const char *lang)
{
    snprintf(root_dir, sizeof root_dir, "%s", data_dir);
    if (lang && *lang) {
        snprintf(language, sizeof language, "%s", lang);
        str_lower(language);
    }
    index_dir(root_dir, "", 0);
    if (!files_index.count)
        return -1;
    load_table(&files_map, "town/files.map", '=');
    load_table(&files_trn, "town/files.trn", '=');
    load_table(&music_sub, "town/music.sub", ' ');
    return 0;
}

const char *data_root(void) { return root_dir; }

/* Turn "VOL:rest" into a path relative to the CD root. */
static void split_volume(const char *name, char *out, size_t outsz, int town)
{
    const char *colon = strchr(name, ':');
    const char *prefix = town ? "town/" : "";
    if (!colon) {
        snprintf(out, outsz, "%s%s", prefix, name);
        return;
    }
    char vol[64];
    size_t n = (size_t)(colon - name);
    if (n >= sizeof vol)
        n = sizeof vol - 1;
    memcpy(vol, name, n);
    vol[n] = 0;
    if (str_ieq(vol, "LANGUAGE_DIR"))
        snprintf(out, outsz, "%s%s/%s", prefix, language, colon + 1);
    else
        snprintf(out, outsz, "%s%s", prefix, colon + 1);
}

const char *resolve_path(const char *name)
{
    static char result[1200];
    if (!name || !*name)
        return NULL;
    const char *n = name;
    const char *t = map_get(&files_trn, n);
    if (t)
        n = t;
    char rel[600];
    const char *hit;
    const char *mapped = map_get(&files_map, n);
    if (mapped) {
        split_volume(mapped, rel, sizeof rel, 1);
        if ((hit = lookup_rel(rel)))
            goto found;
        split_volume(mapped, rel, sizeof rel, 0);
        if ((hit = lookup_rel(rel)))
            goto found;
    }
    split_volume(n, rel, sizeof rel, 0);
    if ((hit = lookup_rel(rel)))
        goto found;
    split_volume(n, rel, sizeof rel, 1);
    if ((hit = lookup_rel(rel)))
        goto found;
    /* a few names in the scripts point to the wrong directory (DUNE:TTWNN/gun2 is in OUT) */
    const char *base = strrchr(rel, '/');
    base = base ? base + 1 : rel;
    if ((hit = map_get(&basenames, base)) && *hit) {
        log_msg("note: '%s' not found, using %s", name, hit);
        goto found;
    }
    return NULL;
found:
    snprintf(result, sizeof result, "%s", hit);
    return result;
}

const char *music_substitute(const char *name) { return map_get(&music_sub, name); }

const char *resolve_town_file(const char *name)
{
    char rel[600];
    snprintf(rel, sizeof rel, "town/%s", name);
    const char *hit = lookup_rel(rel);
    return hit ? hit : lookup_rel(name);
}

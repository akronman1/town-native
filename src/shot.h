#ifndef TOWN_SHOT_H
#define TOWN_SHOT_H

#include "common.h"

#define NAME_LEN 33

/* CAUD: sound command */
enum {
    SND_NONE = 0,
    SND_STOP = 1,   /* stop sample and music */
    SND_SAMPLE = 2, /* 8SVX / raw sample */
    SND_SAMPLE2 = 3,
    SND_MUSIC = 4,  /* music, substituted through MUSIC.SUB */
    SND_MUSIC_FILE = 5
};

typedef struct {
    char name[NAME_LEN];
    int type;
    int repeat; /* times to play, 0 = forever */
} SoundCmd;

/* CDTV: CD audio command (track relative MSF positions) */
typedef struct {
    int track;
    int cmd; /* 0 none, 1/2 whole track, 3 range */
    int start_m, start_s, start_f;
    int end_m, end_s, end_f;
} CdCmd;

/* Avenue actions */
enum { ACT_NONE = 0, ACT_GOTO = 1, ACT_EXEC = 2, ACT_RETURN = 3, ACT_QUIT = 4, ACT_RESET = 5 };

typedef struct Avenue {
    char target[NAME_LEN];
    int action;
    int flags;  /* bit 0: push current shot for a later Return */
    int weight;
    uint8_t *afrm; /* frame mask */
    int afrm_len;
    SoundCmd *snd;
    CdCmd *cd;
    struct Avenue *next;
} Avenue;

/* Event triggers */
enum { TRIG_BUTTON_A = 0, TRIG_BUTTON_B = 1, TRIG_TIMER = 14 };

typedef struct Event {
    int trigger;
    int x1, y1, x2, y2;
    int npts;
    int16_t *pts; /* x,y pairs */
    int secs, tenths;
    Avenue *avenues;
    Avenue *first; /* fallback for timers */
    struct Event *next;
} Event;

typedef struct {
    char name[NAME_LEN * 2];
    char graphic[NAME_LEN];
    char overlay[3][NAME_LEN];
    int transition;  /* 0 cut, otherwise fade through black */
    int loops;       /* animation loops, 0 = forever */
    int last_frame;  /* frame count of one animation cycle, <0 = take from the file */
    int fps;         /* 0 = use the ANIM timing */
    int input_mode;  /* INPT byte 0: 0 shows the pointer */
    SoundCmd *snd;
    CdCmd *cd;
    Event *events;
} Shot;

Shot *shot_load(const char *path, const char *name);
void shot_free(Shot *s);

#endif

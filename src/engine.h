#ifndef TOWN_ENGINE_H
#define TOWN_ENGINE_H

#include "common.h"

typedef struct {
    int trace;      /* log shot changes */
    int no_fades;
} EngineOptions;

int engine_start(const char *start_shot, const EngineOptions *opt);
void engine_shutdown(void);
void engine_seed(int seed);

/* Advance timers, animation and fades; ms is a monotonic millisecond clock. */
void engine_update(uint64_t ms);

/* Input. Buttons: 0 = A (left), 1 = B (right). Coordinates in 320x200 space. */
void engine_button(int button, int x, int y);
void engine_trigger(int trigger);

void engine_render(uint32_t *canvas, int pointer_x, int pointer_y);

int engine_wants_quit(void);
int engine_busy(void);            /* a fade is running */
int engine_pointer_visible(void);
const char *engine_shot_name(void);

/* For the automatic walker: a random point inside a random clickable hotspot. */
int engine_random_hotspot(int *x, int *y);
int engine_has_timer(void);

#endif

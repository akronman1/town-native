/*
 * The Town With No Name - native player for Windows and Linux.
 * Replaces TOWN\DTVPLAY.EXE from the PC CD-ROM and plays the original data files.
 */
#include "audio.h"
#include "common.h"
#include "engine.h"
#include "shot.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* logical output size: 4:3 like the monitors of the time, or square pixels */
static int WIN_W = 640, WIN_H = 480;

typedef struct {
    const char *data_dir;
    const char *language;
    const char *cue;
    const char *start;
    const char *screenshot; /* headless test: save a PNG after run_ms and exit */
    long run_ms;
    int fullscreen;
    int square;
    int sound;
    int autoplay;
    int seed;
    int have_seed;
    EngineOptions eng;
} Options;

static void usage(void)
{
    puts("The Town With No Name - native player\n"
         "usage: townplay [options]\n"
         "  --data DIR        game CD directory (with STARTSHOT and TOWN\\)\n"
         "  --lang NAME       english (default), french, german, spanish, italian, danish\n"
         "  --cd FILE.cue     CD image for the speech/music audio tracks\n"
         "  --fullscreen      start in fullscreen (toggle with F11 or Alt+Enter)\n"
         "  --square          square pixels with integer scaling instead of 4:3\n"
         "  --nosound         disable audio\n"
         "  --nofade          skip palette fades\n"
         "  --seed N          random seed (default: time based)\n"
         "  --start SHOT      start at another shot (default: startshot)\n"
         "  --trace           log every shot change\n"
         "  --autoplay        click random hotspots by itself (testing)\n"
         "  --screenshot F    run for --run-ms milliseconds, save F (.png) and quit\n"
         "  --run-ms N\n"
         "\n"
         "keys: mouse left/A/F1/keypad+ = button A, right/B/F2/keypad- = button B,\n"
         "      arrows/keypad move the pointer, F12 screenshot, Ctrl+Esc quits");
}

static int parse_args(int argc, char **argv, Options *o)
{
    memset(o, 0, sizeof *o);
    o->sound = 1;
    o->start = "startshot";
    o->language = "english";
    o->run_ms = 3000;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *next = i + 1 < argc ? argv[i + 1] : NULL;
#define ARG(name) (!strcmp(a, name) && next && (i++, 1))
        if (ARG("--data"))
            o->data_dir = next;
        else if (ARG("--lang"))
            o->language = next;
        else if (ARG("--cd"))
            o->cue = next;
        else if (ARG("--start"))
            o->start = next;
        else if (ARG("--screenshot"))
            o->screenshot = next;
        else if (ARG("--run-ms"))
            o->run_ms = atol(next);
        else if (ARG("--seed")) {
            o->seed = atoi(next);
            o->have_seed = 1;
        } else if (!strcmp(a, "--fullscreen"))
            o->fullscreen = 1;
        else if (!strcmp(a, "--square"))
            o->square = 1;
        else if (!strcmp(a, "--nosound"))
            o->sound = 0;
        else if (!strcmp(a, "--nofade"))
            o->eng.no_fades = 1;
        else if (!strcmp(a, "--trace"))
            o->eng.trace = 1;
        else if (!strcmp(a, "--autoplay"))
            o->autoplay = 1;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage();
            exit(0);
        } else if (!strncmp(a, "LANGUAGE_DIR=", 13)) {
            /* accept the DOS command line form "LANGUAGE_DIR=french/" */
            static char lang[64];
            snprintf(lang, sizeof lang, "%s", a + 13);
            size_t n = strlen(lang);
            while (n && (lang[n - 1] == '/' || lang[n - 1] == '\\'))
                lang[--n] = 0;
            o->language = lang;
        } else if (a[0] == '-' && (a[1] == 'c' || a[1] == 'C')) {
            /* ignore the DOS player switches -c / -Cd */
        } else {
            fprintf(stderr, "unknown option: %s\n", a);
            usage();
            return -1;
        }
#undef ARG
    }
    return 0;
}

static int looks_like_game_dir(const char *dir)
{
    char p[2200];
    const char *probe[] = {"STARTSHOT", "startshot", "TOWN/FILES.MAP", "town/files.map"};
    for (size_t i = 0; i < sizeof probe / sizeof *probe; i++) {
        snprintf(p, sizeof p, "%s/%s", dir, probe[i]);
        if (file_exists(p))
            return 1;
    }
    return 0;
}

static const char *find_data_dir(void)
{
    static char buf[2048];
    char *base = SDL_GetBasePath();
    const char *rel[] = {"", "..", "../..", "../../..", "CDTOWN", "../CDTOWN", "data"};
    if (base) {
        for (size_t i = 0; i < sizeof rel / sizeof *rel; i++) {
            snprintf(buf, sizeof buf, "%s%s", base, rel[i]);
            if (looks_like_game_dir(buf)) {
                SDL_free(base);
                return buf;
            }
        }
        SDL_free(base);
    }
    for (size_t i = 0; i < sizeof rel / sizeof *rel; i++) {
        snprintf(buf, sizeof buf, "%s", *rel[i] ? rel[i] : ".");
        if (looks_like_game_dir(buf))
            return buf;
    }
    return NULL;
}

/* Look for a .cue sheet next to the data or the program when none was given. */
static const char *find_cue(const char *data_dir)
{
    static char buf[4096];
    const char *names[] = {"town.cue", "TOWN.cue", "cdtown.cue", "CDTOWN.cue", "game.cue"};
    const char *dirs[] = {data_dir, ".."};
    for (size_t d = 0; d < 2; d++)
        for (size_t n = 0; n < sizeof names / sizeof *names; n++) {
            if (d == 1)
                snprintf(buf, sizeof buf, "%s/../%s", data_dir, names[n]);
            else
                snprintf(buf, sizeof buf, "%s/%s", dirs[d], names[n]);
            if (file_exists(buf))
                return buf;
        }
    return NULL;
}

static int quiet_errors;

/* Startup errors also go to a message box: the Windows build has no console. */
static int startup_error(const char *fmt, const char *arg)
{
    char msg[1024];
    snprintf(msg, sizeof msg, fmt, arg ? arg : "");
    fprintf(stderr, "%s\n", msg);
    if (!quiet_errors)
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "The Town With No Name", msg, NULL);
    return 1;
}

static void save_screenshot(const uint32_t *canvas, const char *path)
{
    if (write_png_rgb(path, canvas, SCREEN_W, CANVAS_H))
        log_msg("screenshot: cannot write %s", path);
    else
        log_msg("screenshot: %s", path);
}

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "--decode-voc")) {
        /* developer aid: convert a VOC file to WAV */
        int len, rate;
        int16_t *pcm = voc_decode_all(argv[2], &len, &rate);
        FILE *f = pcm ? fopen(argv[3], "wb") : NULL;
        if (!f)
            return 1;
        uint32_t hdr[11] = {0x46464952, 36 + (uint32_t)len * 2, 0x45564157, 0x20746d66, 16, 0x00010001,
                            (uint32_t)rate, (uint32_t)rate * 2, 0x00100002, 0x61746164, (uint32_t)len * 2};
        fwrite(hdr, 4, 11, f);
        fwrite(pcm, 2, (size_t)len, f);
        fclose(f);
        free(pcm);
        return 0;
    }

    Options o;
    if (parse_args(argc, argv, &o))
        return 1;

    quiet_errors = o.screenshot != NULL;
    const char *data = o.data_dir ? o.data_dir : find_data_dir();
    if (!data) {
        return startup_error("Cannot find the game files.\n"
                             "Put townplay next to STARTSHOT (the root of the CD) or use --data DIR.%s", NULL);
    }
    if (paths_init(data, o.language) || !resolve_path("startshot")) {
        return startup_error("Cannot read the game files in %s", data);
    }

    int headless = o.screenshot != NULL;
    if (SDL_Init(headless ? 0 : SDL_INIT_VIDEO)) {
        return startup_error("SDL: %s", SDL_GetError());
    }
    audio_init(o.sound);
    audio_set_trace(o.eng.trace);
    const char *cue = o.cue ? o.cue : find_cue(data);
    if (cue && cd_open(cue))
        log_msg("CD audio: cannot use %s", cue);

    if (engine_start(o.start, &o.eng)) {
        return startup_error("Cannot start at shot '%s'", o.start);
    }
    engine_seed(o.have_seed ? o.seed : (int)(time(NULL) % 10001));
    srand(o.have_seed ? (unsigned)o.seed : (unsigned)time(NULL));

    uint32_t *canvas = xcalloc((size_t)SCREEN_W * CANVAS_H, sizeof(uint32_t));
    int px = SCREEN_W / 2, py = SCREEN_H / 2;

    if (headless) {
        /* deterministic offline run used for testing */
        uint64_t t;
        for (t = 0; t <= (uint64_t)o.run_ms && !engine_wants_quit(); t += 5) {
            engine_update(t);
            if (o.autoplay && t % 1500 == 0 && !engine_busy()) {
                int hx, hy;
                if (engine_random_hotspot(&hx, &hy))
                    engine_button(0, hx, hy);
            }
        }
        engine_render(canvas, px, py);
        save_screenshot(canvas, o.screenshot);
        engine_shutdown();
        audio_shutdown();
        SDL_Quit();
        return 0;
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0"); /* nearest neighbour, no smoothing */
    if (o.square) {
        WIN_W = 640;
        WIN_H = 400;
    }
    SDL_Window *win = SDL_CreateWindow("The Town With No Name", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       WIN_W * 2, WIN_H * 2,
                                       SDL_WINDOW_RESIZABLE | (o.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (!win) {
        return startup_error("SDL: %s", SDL_GetError());
    }
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren)
        ren = SDL_CreateRenderer(win, -1, 0);
    if (!ren) {
        return startup_error("SDL: %s", SDL_GetError());
    }
    SDL_RendererInfo info;
    int vsync = !SDL_GetRendererInfo(ren, &info) && (info.flags & SDL_RENDERER_PRESENTVSYNC);
    SDL_RenderSetLogicalSize(ren, WIN_W, WIN_H);
    if (o.square)
        SDL_RenderSetIntegerScale(ren, SDL_TRUE);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, SCREEN_W, CANVAS_H);
    SDL_SetTextureScaleMode(tex, SDL_ScaleModeNearest);
    SDL_ShowCursor(SDL_DISABLE);

    uint64_t start = SDL_GetTicks64();
    uint64_t next_auto = 0;
    int shot_counter = 0;
    int running = 1;
    while (running && !engine_wants_quit()) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT:
                running = 0;
                break;
            case SDL_MOUSEMOTION:
                px = ev.motion.x * SCREEN_W / WIN_W;
                py = ev.motion.y * SCREEN_H / WIN_H;
                break;
            case SDL_MOUSEBUTTONDOWN:
                px = ev.button.x * SCREEN_W / WIN_W;
                py = ev.button.y * SCREEN_H / WIN_H;
                if (ev.button.button == SDL_BUTTON_LEFT)
                    engine_button(0, px, py);
                else if (ev.button.button == SDL_BUTTON_RIGHT)
                    engine_button(1, px, py);
                break;
            case SDL_KEYDOWN: {
                SDL_Keycode k = ev.key.keysym.sym;
                Uint16 mod = ev.key.keysym.mod;
                if (k == SDLK_ESCAPE && (mod & KMOD_CTRL)) {
                    running = 0;
                } else if (k == SDLK_F11 || (k == SDLK_RETURN && (mod & KMOD_ALT))) {
                    Uint32 fs = SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP;
                    SDL_SetWindowFullscreen(win, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                } else if (k == SDLK_F12) {
                    char name[64];
                    snprintf(name, sizeof name, "town%03d.png", shot_counter++);
                    engine_render(canvas, px, py);
                    save_screenshot(canvas, name);
                } else if (k == SDLK_a || k == SDLK_F1 || k == SDLK_KP_PLUS) {
                    engine_button(0, px, py);
                } else if (k == SDLK_b || k == SDLK_F2 || k == SDLK_KP_MINUS) {
                    engine_button(1, px, py);
                } else if (k == SDLK_ESCAPE) {
                    engine_trigger(12);
                } else if (k >= SDLK_1 && k <= SDLK_9) {
                    engine_trigger(3 + (int)(k - SDLK_1));
                } else if (k == SDLK_0) {
                    engine_trigger(2);
                } else if (k == SDLK_RETURN) {
                    engine_trigger(13);
                } else {
                    int dx = 0, dy = 0;
                    if (k == SDLK_LEFT || k == SDLK_KP_4 || k == SDLK_KP_7 || k == SDLK_KP_1)
                        dx = -2;
                    if (k == SDLK_RIGHT || k == SDLK_KP_6 || k == SDLK_KP_9 || k == SDLK_KP_3)
                        dx = 2;
                    if (k == SDLK_UP || k == SDLK_KP_8 || k == SDLK_KP_7 || k == SDLK_KP_9)
                        dy = -2;
                    if (k == SDLK_DOWN || k == SDLK_KP_2 || k == SDLK_KP_1 || k == SDLK_KP_3)
                        dy = 2;
                    px += dx;
                    py += dy;
                    if (px < 0)
                        px = 0;
                    if (px > SCREEN_W - 2)
                        px = SCREEN_W - 2;
                    if (py < 0)
                        py = 0;
                    if (py > SCREEN_H - 2)
                        py = SCREEN_H - 2;
                }
                break;
            }
            default:
                break;
            }
        }
        uint64_t now = SDL_GetTicks64() - start;
        engine_update(now);
        if (o.autoplay && now >= next_auto && !engine_busy()) {
            int hx, hy;
            if (engine_random_hotspot(&hx, &hy)) {
                px = hx;
                py = hy;
                engine_button(0, hx, hy);
            }
            next_auto = now + 1500 + (uint64_t)(rand() % 2000);
        }
        if (px < 0)
            px = 0;
        if (py < 0)
            py = 0;
        if (px >= SCREEN_W)
            px = SCREEN_W - 1;
        if (py >= SCREEN_H)
            py = SCREEN_H - 1;

        engine_render(canvas, px, py);
        SDL_UpdateTexture(tex, NULL, canvas, SCREEN_W * 4);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
        if (!vsync)
            SDL_Delay(5);
    }

    engine_shutdown();
    audio_shutdown();
    free(canvas);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

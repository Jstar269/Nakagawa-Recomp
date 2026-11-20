/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The in-game overlay's runtime host (#1159, C9c) on its production path: SDL3_ttf
   and the bundled UI fonts, real SDL key and pad events through nk_ui's SDL input,
   and the pages drawn by the SDL_Renderer backend into a software surface laid over
   a composed base, as the pause presenter (src/rt/gui.c) draws them.

   Exit 77 means SKIP with the reason printed: a build without the SDL3 or SDL3_ttf
   headers, or a host where the SDL3_ttf library cannot be loaded. */

/* setenv/unsetenv are POSIX: without this, -std=c99 on glibc leaves them undeclared. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "overlay_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXIT_SKIP 77

#if !defined(NK_UI_WITH_SDL) || !defined(NK_UI_WITH_SDL_TTF) || !defined(NK_UI_WITH_SDL3)
int main(void) {
    printf("SKIP overlay host: SDL3 and SDL3_ttf headers were not available at build time\n");
    return EXIT_SKIP;
}
#else

#include "backend/nk_ui_sdlr.h"
#include "nk_ui_theme.h"

#include <SDL3/SDL.h>

#ifdef _WIN32
#include <direct.h>
#define test_chdir _chdir
#define test_getcwd _getcwd
#else
#include <unistd.h>
#define test_chdir chdir
#define test_getcwd getcwd
#endif

static int failures;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", message, __FILE__, __LINE__); \
        ++failures; \
    } \
} while (0)

#define BASE_RGBA 0x20304050u

typedef struct {
    SDL_Surface *surface;
    SDL_Renderer *renderer;
} Canvas;

static NkOverlayHost *g_host;
static Canvas g_canvas;
static NkOverlayModel g_model;
static NkOverlayAction g_last;

static void canvas_free(Canvas *canvas) {
    nk_overlay_host_unbind(g_host);
    if (canvas->renderer) SDL_DestroyRenderer(canvas->renderer);
    if (canvas->surface) SDL_DestroySurface(canvas->surface);
    canvas->renderer = NULL;
    canvas->surface = NULL;
}

static bool canvas_make(Canvas *canvas, int w, int h) {
    canvas_free(canvas);
    canvas->surface = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
    canvas->renderer = canvas->surface ? SDL_CreateSoftwareRenderer(canvas->surface) : NULL;
    return canvas->renderer != NULL;
}

static uint32_t pixel(const Canvas *canvas, int x, int y) {
    const uint8_t *row = (const uint8_t *)canvas->surface->pixels + (size_t)y * (size_t)canvas->surface->pitch;
    const uint8_t *p = row + (size_t)x * 4u;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* One presenter frame: the base, then the pages over it. */
static bool draw(void) {
    SDL_FillSurfaceRect(g_canvas.surface, NULL,
                        SDL_MapSurfaceRGBA(g_canvas.surface, (Uint8)(BASE_RGBA >> 24),
                                           (Uint8)(BASE_RGBA >> 16), (Uint8)(BASE_RGBA >> 8),
                                           (Uint8)BASE_RGBA));
    if (!nk_overlay_host_draw(g_host, g_canvas.renderer, 1.0f, 1.0f, &g_model, &g_last)) return false;
    SDL_RenderPresent(g_canvas.renderer);
    return true;
}

/* Draws frames until the host has nothing due, as the presenter's frame loop does. The
   eases run on the real clock, so the frames are paced like display frames; a host still
   due after two seconds never came to rest, and settle() says so. */
static bool settle(void) {
    const Uint64 deadline = SDL_GetTicksNS() + 2000000000u;
    do {
        if (!draw()) return false;
        if (g_last.kind != NK_OVERLAY_ACT_NONE) return true;
        if (!nk_overlay_host_due(g_host, (double)SDL_GetTicksNS() / 1e9)) return true;
        SDL_DelayNS(4000000u);
    } while (SDL_GetTicksNS() < deadline);
    return false;
}

static bool key(SDL_Keycode code) {
    SDL_Event e;
    bool changed;
    memset(&e, 0, sizeof(e));
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = code;
    e.key.down = true;
    changed = nk_overlay_host_event(g_host, (const union SDL_Event *)&e);
    e.type = SDL_EVENT_KEY_UP;
    e.key.down = false;
    (void)nk_overlay_host_event(g_host, (const union SDL_Event *)&e);
    return changed;
}

static void test_menu_to_settings_and_back(void) {
    size_t card_pixels = 0;
    int x, y;
    nk_overlay_host_open(g_host, NK_OVERLAY_OPEN_USER);
    CHECK(nk_overlay_host_is_open(g_host), "open: the host is open");
    CHECK(settle(), "open: the menu draws");
    CHECK(g_last.kind == NK_OVERLAY_ACT_NONE, "open: drawing chooses nothing");
    CHECK(g_model.logical_width == 1280.0f && g_model.logical_height == 800.0f,
          "open: the host fills in the logical size");
    for (y = 0; y < g_canvas.surface->h; y += 4) {
        for (x = 0; x < g_canvas.surface->w; x += 4) {
            if (pixel(&g_canvas, x, y) != BASE_RGBA) ++card_pixels;
        }
    }
    CHECK(card_pixels > 1000u, "open: the pages are drawn over the base");
    CHECK(pixel(&g_canvas, 2, 2) == BASE_RGBA, "open: the corner shows the base");

    /* Down to Settings, then Enter: the settings page. */
    CHECK(key(SDLK_DOWN), "menu: a key press asks for a redraw");
    CHECK(settle(), "menu: draw after down");
    CHECK(key(SDLK_RETURN), "menu: enter");
    CHECK(settle(), "menu: draw after enter");
    CHECK(g_last.kind == NK_OVERLAY_ACT_NONE, "menu: opening settings is a page change");
    /* Right on the focused scaling row picks the next mode. */
    g_model.scaling = NK_OVERLAY_SCALING_SHARP;
    CHECK(key(SDLK_RIGHT), "settings: right");
    CHECK(draw(), "settings: draw after right");
    CHECK(g_last.kind == NK_OVERLAY_ACT_SET_SCALING && g_last.value == NK_OVERLAY_SCALING_INTEGER,
          "settings: right picks Integer through the real key path");
    /* Esc goes back to the menu, then resumes from it. */
    CHECK(key(SDLK_ESCAPE), "settings: esc");
    CHECK(settle(), "settings: draw after esc");
    CHECK(g_last.kind == NK_OVERLAY_ACT_NONE, "settings: esc goes back a page, not into the game");
    CHECK(key(SDLK_ESCAPE), "menu: esc");
    CHECK(draw(), "menu: draw after esc");
    CHECK(g_last.kind == NK_OVERLAY_ACT_RESUME, "menu: esc resumes");
    nk_overlay_host_close(g_host);
    CHECK(!nk_overlay_host_is_open(g_host), "close: the host is closed");
    CHECK(!key(SDLK_RETURN), "close: a closed host takes no input");
}

static void test_f1_resumes_and_queue_is_dropped(void) {
    nk_overlay_host_open(g_host, NK_OVERLAY_OPEN_USER);
    CHECK(settle(), "f1: draw");
    CHECK(key(SDLK_F1), "f1: press");
    /* Closed before the frame that would have taken it: the next pause must not see it. */
    nk_overlay_host_close(g_host);
    nk_overlay_host_open(g_host, NK_OVERLAY_OPEN_USER);
    CHECK(settle(), "f1: the next pause draws");
    CHECK(g_last.kind == NK_OVERLAY_ACT_NONE, "f1: a press queued before close does not resume the next pause");
    CHECK(key(SDLK_F1), "f1: press again");
    CHECK(draw(), "f1: draw");
    CHECK(g_last.kind == NK_OVERLAY_ACT_RESUME, "f1: resumes from the menu");
    nk_overlay_host_close(g_host);
    /* Audit 2026-11-17: the pause closes right after a page's first frame, whose controls
       were not laid out yet, so a move and Esc that arrive then are held for the laid-out
       graph. The next pause must not see them: it opens on its menu, not resumed. */
    nk_overlay_host_open(g_host, NK_OVERLAY_OPEN_USER);
    CHECK(draw(), "held: the menu's first frame");
    CHECK(key(SDLK_DOWN) && key(SDLK_ESCAPE), "held: a move and Esc after the first frame");
    nk_overlay_host_close(g_host);
    nk_overlay_host_open(g_host, NK_OVERLAY_OPEN_USER);
    CHECK(settle(), "held: the next pause draws");
    CHECK(g_last.kind == NK_OVERLAY_ACT_NONE && nk_overlay_host_is_open(g_host),
          "held: input held at close does not resume the next pause");
    nk_overlay_host_close(g_host);
}

static bool render_reset(void) {
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type = SDL_EVENT_RENDER_DEVICE_RESET;
    return nk_overlay_host_event(g_host, (const union SDL_Event *)&e);
}

/* SDL_EVENT_RENDER_DEVICE_RESET (a D3D device lost to a driver reset) loses every
   texture: the host resets its backend, so the next frame uploads its glyphs again,
   and it does so while the pages are closed too, before they are next drawn. The event
   is built here as SDL delivers it; a software renderer never sends one itself. */
static void test_render_device_reset_drops_the_textures(void) {
    NkUiSdlrStats before;
    NkUiSdlrStats stats;
    memset(&before, 0, sizeof(before));
    memset(&stats, 0, sizeof(stats));
    nk_overlay_host_open(g_host, NK_OVERLAY_OPEN_USER);
    CHECK(settle(), "device reset: draws");
    nk_ui_sdlr_get_stats(nk_overlay_host_backend(g_host), &before);
    CHECK(before.atlas_pages > 0u && before.atlas_uploads > 0u && before.device_resets == 0u,
          "device reset: the frame drew from an atlas");
    CHECK(render_reset(), "device reset: the pages must be drawn again");
    nk_ui_sdlr_get_stats(nk_overlay_host_backend(g_host), &stats);
    CHECK(stats.device_resets == 1u && stats.atlas_pages == 0u && stats.atlas_glyphs == 0u,
          "device reset: the backend dropped its textures");
    CHECK(draw(), "device reset: draw after the reset");
    nk_ui_sdlr_get_stats(nk_overlay_host_backend(g_host), &stats);
    CHECK(stats.atlas_pages > 0u && stats.atlas_uploads > before.atlas_uploads,
          "device reset: the glyphs are uploaded again");
    nk_overlay_host_close(g_host);
    (void)render_reset();
    nk_ui_sdlr_get_stats(nk_overlay_host_backend(g_host), &stats);
    CHECK(stats.device_resets == 2u && stats.atlas_pages == 0u,
          "device reset: a closed host resets its backend too");
}

static void test_pad_lost_and_resize(void) {
    nk_overlay_host_open(g_host, NK_OVERLAY_OPEN_PAD_LOST);
    CHECK(settle(), "pad lost: draws");
    CHECK(!g_model.pad_connected, "pad lost: no pad is connected");
    /* Esc keeps the game paused: the menu, never the game. */
    CHECK(key(SDLK_ESCAPE), "pad lost: esc");
    CHECK(settle(), "pad lost: draw after esc");
    CHECK(g_last.kind == NK_OVERLAY_ACT_NONE, "pad lost: back goes to the menu");
    /* The window grows: a new renderer, and the host follows it. */
    CHECK(canvas_make(&g_canvas, 1920, 1200), "resize: a new canvas");
    CHECK(settle(), "resize: draws into the new renderer");
    CHECK(g_model.logical_width == 1920.0f, "resize: the logical size follows");
    CHECK(key(SDLK_ESCAPE), "resize: esc");
    CHECK(draw(), "resize: draw");
    CHECK(g_last.kind == NK_OVERLAY_ACT_RESUME, "resize: esc on the menu resumes");
    nk_overlay_host_close(g_host);
}

static void set_reduce_motion(const char *value) {
#ifdef _WIN32
    (void)_putenv_s("SR_REDUCE_MOTION", value != NULL ? value : "");
#else
    if (value != NULL) {
        (void)setenv("SR_REDUCE_MOTION", value, 1);
    } else {
        (void)unsetenv("SR_REDUCE_MOTION");
    }
#endif
}

/* The pages ease or cut as the player chose (SR_REDUCE_MOTION, which the launcher
   exports): with motion a focus move eases, and the host keeps drawing until it ends;
   under Reduce motion the move is a cut and nothing is left to draw. */
static void test_reduce_motion_follows_the_player(void) {
    CHECK(nk_overlay_reduce_motion("on", false), "reduce motion: on cuts");
    CHECK(!nk_overlay_reduce_motion("off", true), "reduce motion: off eases");
    CHECK(nk_overlay_reduce_motion("system", true) && !nk_overlay_reduce_motion("system", false),
          "reduce motion: system follows the system's setting");
    CHECK(!nk_overlay_reduce_motion(NULL, false) && nk_overlay_reduce_motion(NULL, true) &&
              nk_overlay_reduce_motion("", true) && !nk_overlay_reduce_motion("bogus", false),
          "reduce motion: a run started by hand, or an unknown value, follows the system");

    set_reduce_motion("off");
    nk_overlay_host_open(g_host, NK_OVERLAY_OPEN_USER);
    CHECK(settle(), "reduce motion off: draws");
    CHECK(settle(), "reduce motion off: settles");
    CHECK(key(SDLK_DOWN), "reduce motion off: a focus move");
    CHECK(draw(), "reduce motion off: draws the move");
    CHECK(nk_overlay_host_due(g_host, (double)SDL_GetTicksNS() / 1e9),
          "reduce motion off: the focus ring eases, so the host draws again");
    nk_overlay_host_close(g_host);

    set_reduce_motion("on");
    nk_overlay_host_open(g_host, NK_OVERLAY_OPEN_USER);
    CHECK(settle(), "reduce motion on: draws");
    CHECK(settle(), "reduce motion on: settles");
    CHECK(key(SDLK_DOWN), "reduce motion on: a focus move");
    CHECK(draw(), "reduce motion on: draws the move");
    CHECK(!nk_overlay_host_due(g_host, (double)SDL_GetTicksNS() / 1e9),
          "reduce motion on: the move is a cut, with nothing left to draw");
    nk_overlay_host_close(g_host);
    set_reduce_motion(NULL);
}

/* Copies `relative` from `root` into `folder`, making its folders on the way (SDL's file calls
   take UTF-8 paths on every host). */
static bool copy_font(const char *root, const char *folder, const char *relative) {
    char from[1024], to[1024];
    char *slash;
    void *data;
    size_t size = 0u;
    bool saved;
    if (snprintf(from, sizeof(from), "%s/%s", root, relative) >= (int)sizeof(from) ||
        snprintf(to, sizeof(to), "%s/%s", folder, relative) >= (int)sizeof(to)) {
        return false;
    }
    slash = strrchr(to, '/');
    if (slash == NULL) return false;
    *slash = '\0';
    if (!SDL_CreateDirectory(to)) return false;
    *slash = '/';
    data = SDL_LoadFile(from, &size);
    if (data == NULL) return false;
    saved = SDL_SaveFile(to, data, size);
    SDL_free(data);
    return saved;
}

/* After the folder it is given, the host looks for a face in SDL's base path (this executable's
   folder) and the working directory. The font fixtures run from the executable's own folder,
   which the make target keeps apart from the build folder, where `make player` stages the fonts
   beside the player: a copy in either fallback would let the host load the face and the check
   pass, or fail, for the wrong reason. False, after a failed CHECK naming `label`'s copy, when
   the folder holds a face or cannot become the working directory. */
static bool enter_fontless_exe_folder(const char *label) {
    const char *base = SDL_GetBasePath();
    char probe[1200];
    if (base == NULL || base[0] == '\0') {
        CHECK(false, "font fixture: SDL knows this executable's folder");
        fprintf(stderr, "  (%s)\n", label);
        return false;
    }
    for (int font = 0; font < NK_UI_FONT_COUNT; ++font) {
        if (snprintf(probe, sizeof(probe), "%s%s", base, nk_ui_font_path(font)) >=
                (int)sizeof(probe) ||
            SDL_GetPathInfo(probe, NULL)) {
            CHECK(false, "font fixture: the executable's folder holds no UI font to fall back to");
            fprintf(stderr, "  (%s: %s)\n", label, probe);
            return false;
        }
    }
    if (test_chdir(base) != 0) {
        CHECK(false, "font fixture: the executable's folder becomes the working directory");
        fprintf(stderr, "  (%s)\n", label);
        return false;
    }
    return true;
}

/* Audit 2026-11-17: the player's folder may sit under a name outside the ANSI code page (a
   user name in Japanese, say). SDL's base path and the launcher's SR_UI_ROOT are UTF-8, so the
   host finds the fonts there, from an executable folder and a working directory that have none. */
static void test_fonts_from_a_utf8_folder(const char *root) {
    static const char leaf[] = "overlay-host-fonts-\xE6\x97\xA5\xE6\x9C\xAC-\xC3\xBC";
    const char *build = SR_SELFTEST_BUILD_ROOT;
    bool absolute = build[0] == '/' || build[0] == '\\' || (build[0] != '\0' && build[1] == ':');
    char *cwd = SDL_GetCurrentDirectory();
    char saved[1024];
    char folder[1024];
    char build_abs[1024];
    char error[256];
    NkOverlayHost *host;
    bool copied = true;
    if (cwd == NULL || test_getcwd(saved, (int)sizeof(saved)) == NULL) {
        CHECK(false, "utf-8 fonts: the working directory is known");
        SDL_free(cwd);
        return;
    }
    if (snprintf(build_abs, sizeof(build_abs), "%s%s", absolute ? "" : cwd, build) >=
            (int)sizeof(build_abs) ||
        snprintf(folder, sizeof(folder), "%s/%s", build_abs, leaf) >= (int)sizeof(folder)) {
        CHECK(false, "utf-8 fonts: the fixture paths fit");
        SDL_free(cwd);
        return;
    }
    SDL_free(cwd);
    for (int font = 0; font < NK_UI_FONT_COUNT && copied; ++font) {
        copied = copy_font(root, folder, nk_ui_font_path(font));
    }
    CHECK(copied, "utf-8 fonts: the fonts are copied into a non-ASCII folder");
    if (!copied) return;
    /* Neither fallback has the fonts, so only the UTF-8 root can supply them. */
    if (!enter_fontless_exe_folder("utf-8 fonts")) return;
    host = nk_overlay_host_create(folder, error, sizeof(error));
    CHECK(host != NULL, "utf-8 fonts: the host loads the fonts from a non-ASCII folder");
    if (host == NULL) fprintf(stderr, "  (%s)\n", error);
    nk_overlay_host_destroy(host);
    CHECK(test_chdir(saved) == 0, "utf-8 fonts: the working directory comes back");
}

/* Audit 2026-11-17: a face that is there but cannot be read (empty, truncated, too large) is
   reported as unreadable, by its path, not as missing. The folder holds every face but an empty
   Geist; the executable's folder, the working directory here, has none to fall back to. */
static void test_unreadable_font_is_not_reported_missing(const char *root) {
    const char *build = SR_SELFTEST_BUILD_ROOT;
    bool absolute = build[0] == '/' || build[0] == '\\' || (build[0] != '\0' && build[1] == ':');
    const char *body = nk_ui_font_path(NK_UI_FONT_BODY);
    char *cwd = SDL_GetCurrentDirectory();
    char saved[1024];
    char folder[1024];
    char empty[1200];
    char build_abs[1024];
    char error[256];
    NkOverlayHost *host;
    bool copied = true;
    if (cwd == NULL || test_getcwd(saved, (int)sizeof(saved)) == NULL) {
        CHECK(false, "unreadable font: the working directory is known");
        SDL_free(cwd);
        return;
    }
    if (snprintf(build_abs, sizeof(build_abs), "%s%s", absolute ? "" : cwd, build) >=
            (int)sizeof(build_abs) ||
        snprintf(folder, sizeof(folder), "%s/overlay-host-empty-font", build_abs) >=
            (int)sizeof(folder) ||
        snprintf(empty, sizeof(empty), "%s/%s", folder, body) >= (int)sizeof(empty)) {
        CHECK(false, "unreadable font: the fixture paths fit");
        SDL_free(cwd);
        return;
    }
    SDL_free(cwd);
    for (int font = 0; font < NK_UI_FONT_COUNT && copied; ++font) {
        copied = copy_font(root, folder, nk_ui_font_path(font));
    }
    copied = copied && SDL_SaveFile(empty, "", 0u);
    CHECK(copied, "unreadable font: the fonts are copied and Geist is emptied");
    if (!copied) return;
    if (!enter_fontless_exe_folder("unreadable font")) return;
    error[0] = '\0';
    host = nk_overlay_host_create(folder, error, sizeof(error));
    CHECK(host == NULL, "unreadable font: an empty face fails the host");
    if (strstr(error, "could not be read") == NULL || strstr(error, body) == NULL) {
        CHECK(false, "unreadable font: the error names the unreadable face, not a missing one");
        fprintf(stderr, "  (%s)\n", error);
    }
    nk_overlay_host_destroy(host);
    CHECK(test_chdir(saved) == 0, "unreadable font: the working directory comes back");
}

int main(int argc, char **argv) {
    char error[256];
    const char *root = argc > 1 ? argv[1] : ".";
    NkUiTtf *probe;
    if (!SDL_Init(0)) {
        printf("SKIP overlay host: SDL_Init failed: %s\n", SDL_GetError());
        return EXIT_SKIP;
    }
    probe = nk_ui_ttf_create(NULL, 0u);
    if (probe == NULL) {
        printf("SKIP overlay host: SDL3_ttf is not usable here: %s\n", nk_ui_ttf_last_error());
        SDL_Quit();
        return EXIT_SKIP;
    }
    nk_ui_ttf_destroy(probe);
    /* A folder without the fonts falls back to the runtime's folder and the working
       directory; the test runs from the repository root, which has them. */
    g_host = nk_overlay_host_create("does-not-exist", error, sizeof(error));
    CHECK(g_host != NULL, "create: a folder without fonts falls back to the working directory");
    nk_overlay_host_destroy(g_host);
    test_fonts_from_a_utf8_folder(root);
    test_unreadable_font_is_not_reported_missing(root);
    g_host = nk_overlay_host_create(root, error, sizeof(error));
    if (g_host == NULL) {
        fprintf(stderr, "FAIL: create the host from %s: %s\n", root, error);
        SDL_Quit();
        return 1;
    }
    if (!canvas_make(&g_canvas, 1280, 800)) {
        /* Every page check reads this canvas's pixels: stop with the reason, not a crash. */
        fprintf(stderr, "FAIL: a 1280x800 canvas: %s\n", SDL_GetError());
        canvas_free(&g_canvas);
        nk_overlay_host_destroy(g_host);
        SDL_Quit();
        return 1;
    }
    memset(&g_model, 0, sizeof(g_model));
    g_model.title_name = "Overlay Host Fixture";
    g_model.disc_id = "TEST00001";
    test_menu_to_settings_and_back();
    test_f1_resumes_and_queue_is_dropped();
    test_render_device_reset_drops_the_textures();
    test_pad_lost_and_resize();
    test_reduce_motion_follows_the_player();
    canvas_free(&g_canvas);
    nk_overlay_host_destroy(g_host);
    SDL_Quit();
    if (failures != 0) {
        fprintf(stderr, "overlay host: %d failure(s)\n", failures);
        return 1;
    }
    printf("overlay host: PASS\n");
    return 0;
}
#endif

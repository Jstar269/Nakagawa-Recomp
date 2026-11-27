/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "nk_ui.h"
#include "nk_ui_assets.h"
#include "nk_ui_draw.h"
#include "nk_ui_image.h"
#include "nk_ui_input.h"
#include "nk_ui_text.h"
#include "nk_ui_theme.h"
#include "nk_ui_waves.h"
#include "nk_ui_widgets.h"
#include "backend/nk_ui_sdlr.h"
#include "backend/nk_ui_vk.h"
#include "clay.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

size_t nk_ui_dump_draw_list(const NkUiDrawList *list, char *out, size_t cap);
bool nk_ui_clay_error_kind(int type, const char **kind);

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s\n", message); \
        return 0; \
    } \
} while (0)

static const char *const g_ui_core_files[] = {
    "src/ui/nk_ui.h",
    "src/ui/nk_ui.c",
    "src/ui/nk_ui_clay.c",
    "src/ui/nk_ui_theme.h",
    "src/ui/nk_ui_theme.c",
    "src/ui/nk_ui_draw.h",
    "src/ui/nk_ui_draw.c",
    "src/ui/nk_ui_text.h",
    "src/ui/nk_ui_text.c",
    "src/ui/nk_ui_widgets.h",
    "src/ui/nk_ui_widgets.c",
    "src/ui/nk_ui_waves.h",
    "src/ui/nk_ui_waves.c",
    "src/ui/nk_ui_dump.c",
    "src/ui/nk_ui_focus.h",
    "src/ui/nk_ui_focus.c",
    "src/ui/nk_ui_input.h",
    "src/ui/nk_ui_input.c",
    "src/ui/nk_ui_prompts.c",
    "src/ui/nk_ui_image.h",
    "src/ui/nk_ui_image.c",
    "src/ui/nk_ui_assets.h",
    "src/ui/nk_ui_assets.c",
    "src/ui/nk_ui_sdl_input.c"
    /* nk_ui_ttf.c and backend/nk_ui_sdlr.* are the SDL edge since lane C3b: they
       include SDL headers behind NK_UI_WITH_SDL and are covered by test_nk_ui_sdlr.c.
       backend/nk_ui_vk.* is the Vulkan edge since lane C3c: it includes the Vulkan
       headers behind NK_UI_WITH_VULKAN and is covered by test_nk_ui_vk.c. */
};

static int contains_ascii_case_insensitive(const char *text, const char *needle) {
    size_t needle_length = strlen(needle);
    const char *start;
    for (start = text; *start != '\0'; ++start) {
        size_t i;
        for (i = 0; i < needle_length; ++i) {
            char a = start[i];
            char b = needle[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a == '\0' || a != b) break;
        }
        if (i == needle_length) return 1;
    }
    return 0;
}

static int test_no_platform_headers(void) {
    size_t i;
    for (i = 0; i < sizeof(g_ui_core_files) / sizeof(g_ui_core_files[0]); ++i) {
        FILE *file = fopen(g_ui_core_files[i], "rb");
        char line[1024];
        CHECK(file != NULL, "all reserved nk_ui headers and C files must exist");
        /* nk_ui_sdl_input.c is the SDL edge by design (it includes SDL3 behind
           NK_UI_WITH_SDL3); only its existence is checked here. The SDL-free
           headers it implements are still scanned. */
        if (strstr(g_ui_core_files[i], "nk_ui_sdl_input.c") != NULL) {
            fclose(file);
            continue;
        }
        while (fgets(line, sizeof(line), file) != NULL) {
            if (strstr(line, "#include") != NULL) {
                CHECK(strstr(line, "SDL") == NULL, "nk_ui core must not include SDL headers");
                CHECK(!contains_ascii_case_insensitive(line, "vulkan"),
                      "nk_ui core must not include Vulkan headers");
            }
        }
        fclose(file);
    }
    return 1;
}

static int test_theme_tokens(void) {
    const NkUiTheme *theme = nk_ui_theme_n1();
    CHECK(theme != NULL, "N1 theme must be available");
    CHECK(theme->accent_dark == 0x6B9FF0FFu, "dark N1 accent token: the site's dark accent");
    CHECK(theme->accent_light == 0x2F6FD0FFu, "light N1 accent token: the site's light accent");
    CHECK(theme->focus == theme->accent_dark, "the focus ring is the accent");
    CHECK(theme->info == 0xC29AF4FFu, "info is violet, apart from the blue accent");
    CHECK(theme->ready == 0x2DD4BFFFu, "Ready teal token");
    CHECK(theme->warning == 0xDBB024FFu, "warning gold token");
    CHECK(theme->ink == 0x10151CFFu && theme->paper == 0xF5F7FAFFu,
          "N1 ink and paper tokens");
    /* The type scale lists only tracking the measure and the draw apply: they
       drop a negative letter spacing, so TITLE_XL's design value is 0
       (docs/PLAYER_UI_DESIGN.md), not the -0.01 em that was never drawn. */
    for (int role = 0; role < (int)NK_UI_TYPE_COUNT; ++role) {
        CHECK(nk_ui_type((NkUiTypeRole)role, 0u).letter_spacing >= 0.0f,
              "no type role lists a negative letter spacing");
    }
    CHECK(nk_ui_type(NK_UI_TYPE_TITLE_XL, 0u).letter_spacing == 0.0f,
          "TITLE_XL has no tracking, as the design table says");
    return 1;
}

static int test_scale_model(void) {
    NkUiViewport viewport = {640, 400, 6.0f, 0, 150, 0.0f, 0.0f};
    NkUiScale scale = nk_ui_scale_for_viewport(&viewport);
    CHECK(scale.density == 4.0f, "automatic UI density clamps at four");
    CHECK(scale.logical_w == 160.0f && scale.logical_h == 100.0f,
          "logical layout uses the clamped density");
    viewport.display_scale = 0.5f;
    viewport.text_size_pct = 90;
    scale = nk_ui_scale_for_viewport(&viewport);
    CHECK(scale.density == 1.0f, "UI density clamps at one");
    viewport.display_scale = 1.0f;
    viewport.ui_scale_pct = 200;
    viewport.text_size_pct = 150;
    scale = nk_ui_scale_for_viewport(&viewport);
    CHECK(scale.density == 2.0f && scale.logical_w == 320.0f && scale.logical_h == 200.0f,
          "the text size is not a layout scale: density and logical size follow the UI scale");
    CHECK(nk_ui_text_scale_for_viewport(&viewport) == 1.5f,
          "a 150% text size scales type by 1.5");
    viewport.text_size_pct = 125;
    CHECK(nk_ui_text_scale_for_viewport(&viewport) == 1.25f,
          "a 125% text size scales type by 1.25");
    viewport.text_size_pct = 175;
    CHECK(nk_ui_text_scale_for_viewport(&viewport) == 1.0f,
          "an unsupported text size is treated as 100%");
    CHECK(nk_ui_text_scale_for_viewport(NULL) == 1.0f, "no viewport: 100% text");
    return 1;
}

/* One frame at a text size, laid out twice (as a screen is): a padded column
   with a label, a button, a status pill, a fixed 120 x 30 box and a paragraph
   wrapped in a 200 px column. */
static const NkUiDrawList *text_size_frame(NkUi *ui, int text_size_pct) {
    static const char paragraph[] = "Grows the type and lets the layout reflow around it.";
    NkUiViewport viewport;
    NkUiLayout layout;
    NkUiStyle fill;
    NkUiTextStyle body = nk_ui_type(NK_UI_TYPE_BODY, 0u);
    const NkUiDrawList *list = NULL;
    memset(&viewport, 0, sizeof(viewport));
    viewport.pixel_w = 640;
    viewport.pixel_h = 400;
    viewport.display_scale = 1.0f;
    viewport.ui_scale_pct = 100;
    viewport.text_size_pct = text_size_pct;
    nk_ui_set_viewport(ui, &viewport);
    for (int frame = 0; frame < 2; ++frame) {
        nk_ui_begin(ui, 37.0);
        memset(&layout, 0, sizeof(layout));
        layout.width = (NkUiSizing){NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f};
        layout.height = (NkUiSizing){NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f};
        layout.direction = NK_UI_LAYOUT_COLUMN;
        layout.gap = 10.0f;
        layout.padding_left = layout.padding_top = 16.0f;
        if (!nk_ui_begin_container(ui, 1u, &layout, NULL)) return NULL;
        nk_ui_label(ui, 2u, "Core layout", 0, 16);
        (void)nk_ui_button(ui, 3u, "Open library", 0u);
        nk_ui_status_pill(ui, 4u, NULL, "Ready", 0x2DD4BFFFu);
        memset(&layout, 0, sizeof(layout));
        layout.width = (NkUiSizing){NK_UI_SIZE_FIXED, 120.0f, 0.0f, 0.0f};
        layout.height = (NkUiSizing){NK_UI_SIZE_FIXED, 30.0f, 0.0f, 0.0f};
        memset(&fill, 0, sizeof(fill));
        fill.fill_rgba = 0x18202BFFu;
        if (nk_ui_begin_container(ui, 5u, &layout, &fill)) nk_ui_end_container(ui);
        memset(&layout, 0, sizeof(layout));
        layout.width = (NkUiSizing){NK_UI_SIZE_FIXED, 200.0f, 0.0f, 0.0f};
        layout.height = (NkUiSizing){NK_UI_SIZE_FIT, 0.0f, 0.0f, 0.0f};
        layout.direction = NK_UI_LAYOUT_COLUMN;
        if (nk_ui_begin_container(ui, 6u, &layout, NULL)) {
            nk_ui_text(ui, 7u, paragraph, &body);
            nk_ui_end_container(ui);
        }
        nk_ui_end_container(ui);
        list = nk_ui_end(ui);
    }
    return list;
}

/* The first command of `kind` element `id` drew, or NULL. A border is drawn
   in pieces (edges and corners), so for a border the command returned is the
   first piece in the edge colour with its bounds grown over all of them. */
static const NkUiDrawCmd *text_size_command(const NkUiDrawList *list, uint32_t id, int kind) {
    static NkUiDrawCmd border;
    const NkUiDrawCmd *first = NULL;
    for (uint32_t i = 0u; list != NULL && i < list->ncmd; ++i) {
        const NkUiDrawCmd *cmd = &list->cmd[i];
        if (cmd->element_id != id || cmd->kind != kind) continue;
        if (kind != NK_UI_DRAW_BORDER) return cmd;
        if (first == NULL) {
            first = cmd;
            border = *cmd;
        } else if (cmd->rgba_pm == first->rgba_pm) {
            if (cmd->bounds[0] < border.bounds[0]) border.bounds[0] = cmd->bounds[0];
            if (cmd->bounds[1] < border.bounds[1]) border.bounds[1] = cmd->bounds[1];
            if (cmd->bounds[2] > border.bounds[2]) border.bounds[2] = cmd->bounds[2];
            if (cmd->bounds[3] > border.bounds[3]) border.bounds[3] = cmd->bounds[3];
        }
    }
    return first != NULL ? &border : NULL;
}

static int text_size_height(const NkUiDrawCmd *cmd) {
    return cmd != NULL ? cmd->bounds[3] - cmd->bounds[1] : -1;
}

static int text_size_width(const NkUiDrawCmd *cmd) {
    return cmd != NULL ? cmd->bounds[2] - cmd->bounds[0] : -1;
}

static int text_size_runs(const NkUiDrawList *list, uint32_t id) {
    int runs = 0;
    for (uint32_t i = 0u; list != NULL && i < list->ncmd; ++i) {
        if (list->cmd[i].element_id == id && list->cmd[i].kind == NK_UI_DRAW_TEXT) ++runs;
    }
    return runs;
}

/* A text backend that measures as a font engine does: each character advances the pen by a
   fraction of a pixel (a space and a narrow letter about a quarter of the size, any other
   character about half), and a run's width is the whole pixels of the sum. */
static NkUiTextSize whole_pixel_measure(void *user, int font, int px, const char *text,
                                        size_t length) {
    NkUiTextSize measured = {0.0f, 0.0f, 0.0f};
    float pen = 0.0f;
    (void)user;
    (void)font;
    if (text == NULL || px <= 0) return measured;
    for (size_t i = 0u; i < length; ++i) {
        unsigned char c = (unsigned char)text[i];
        if ((c & 0xC0u) == 0x80u) continue;
        pen += (c == ' ' ? 0.25f : c != 0u && strchr("iljtf.,;'", (int)c) != NULL ? 0.24f : 0.52f) *
               (float)px;
    }
    measured.width = floorf(pen);
    measured.height = (float)px;
    measured.baseline = (float)px * 0.8f;
    return measured;
}

/* One frame, laid out twice: `text` as card text in a column `width` px wide (element 7),
   and as one line in a box that fits it (element 9). */
static const NkUiDrawList *wrapped_lines_frame(NkUi *ui, const char *text, float width) {
    NkUiViewport viewport;
    NkUiLayout layout;
    NkUiTextStyle body = nk_ui_type(NK_UI_TYPE_BODY_S, 0u);
    NkUiTextStyle line = nk_ui_type(NK_UI_TYPE_BODY_S, 0u);
    const NkUiDrawList *list = NULL;
    line.wrap = false;
    memset(&viewport, 0, sizeof(viewport));
    viewport.pixel_w = 1600;
    viewport.pixel_h = 400;
    viewport.display_scale = 1.0f;
    viewport.ui_scale_pct = 100;
    viewport.text_size_pct = 100;
    nk_ui_set_viewport(ui, &viewport);
    for (int frame = 0; frame < 2; ++frame) {
        nk_ui_begin(ui, 37.0);
        memset(&layout, 0, sizeof(layout));
        layout.width = (NkUiSizing){NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f};
        layout.height = (NkUiSizing){NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f};
        layout.direction = NK_UI_LAYOUT_COLUMN;
        layout.gap = 10.0f;
        if (!nk_ui_begin_container(ui, 1u, &layout, NULL)) return NULL;
        memset(&layout, 0, sizeof(layout));
        layout.width = (NkUiSizing){NK_UI_SIZE_FIXED, width, 0.0f, 0.0f};
        layout.height = (NkUiSizing){NK_UI_SIZE_FIT, 0.0f, 0.0f, 0.0f};
        layout.direction = NK_UI_LAYOUT_COLUMN;
        if (nk_ui_begin_container(ui, 6u, &layout, NULL)) {
            nk_ui_text(ui, 7u, text, &body);
            nk_ui_end_container(ui);
        }
        memset(&layout, 0, sizeof(layout));
        layout.width = (NkUiSizing){NK_UI_SIZE_FIT, 0.0f, 0.0f, 0.0f};
        layout.height = (NkUiSizing){NK_UI_SIZE_FIT, 0.0f, 0.0f, 0.0f};
        layout.direction = NK_UI_LAYOUT_ROW;
        if (nk_ui_begin_container(ui, 8u, &layout, NULL)) {
            nk_ui_text(ui, 9u, text, &line);
            nk_ui_end_container(ui);
        }
        nk_ui_end_container(ui);
        list = nk_ui_end(ui);
    }
    return list;
}

/* Wrapped text stays inside its box, and a one-line run's box is as wide as the run (review of
   #2644). The layout adds a run up word by word, and a backend that measures in whole pixels
   drops a fraction with every word and every space measured alone: the sum fell short of the
   line as drawn, by 8 px for a ten-word line of 14 px type, so a line was kept that ran past
   its box into the padding beside it, and a one-line run got a box narrower than its glyphs.
   Each paragraph is wrapped at every width from 150 to 420 px. No line may measure wider than
   the column, the lines together are the paragraph, and the paragraph takes as many lines as
   nk_ui_text_clamp, which measures each line whole, says it needs (one more is allowed where
   a word fits a line only without the space after it, which the layout counts). The last
   paragraph is longer than the layout measures a word in (256 bytes of its line): there each
   128-byte step of that context may shift the sum by a pixel either way, so its lines get
   2 px, its line count one either way, and its one-line box a pixel a step. */
static int test_wrapped_lines_fit_their_box(void) {
    static const char *const paragraphs[] = {
        "The game's speed and frame rate in a corner, over its HUD.",
        "Library-wide for Enhanced and Ace; Authentic ignores it, and each game's part choices win.",
        "More triangles per curved patch. Slower; alters GE timing; the game receives changed "
        "pictures.",
        "Ultra is used in Enhanced and Ace, with crisp text, FSR 1 and full 32-bit colour on. "
        "Authentic uses your settings. This build smooths only 30 FPS games. The choice here "
        "applies when this game's 60 FPS game logic is off. Shows more 3D space; HUD stays "
        "shaped. Some objects pop in. It is still the same game, if a little wider than it was.",
    };
    NkUiConfig config;
    NkUiTextBackend whole = nk_ui_text_fixed();
    NkUiTextStyle body = nk_ui_type(NK_UI_TYPE_BODY_S, 0u);
    NkUi *ui;
    whole.measure = whole_pixel_measure;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &whole;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create a context that measures in whole pixels");
    for (size_t p = 0u; p < sizeof(paragraphs) / sizeof(paragraphs[0]); ++p) {
        const size_t length = strlen(paragraphs[p]);
        const float line_width = whole_pixel_measure(NULL, 0, body.size_px, paragraphs[p],
                                                     length).width;
        const bool in_context = length <= 256u;
        const float line_slack = in_context ? 0.0f : 2.0f;
        const float box_slack = in_context ? 0.0f : (float)(length / 128u);
        for (int width = 150; width <= 420; ++width) {
            const NkUiDrawList *list = wrapped_lines_frame(ui, paragraphs[p], (float)width);
            char joined[512];
            size_t used = 0u;
            int lines = 0;
            int needed = 1;
            CHECK(list != NULL, "lay out the wrapped paragraph");
            for (uint32_t i = 0u; i < list->ncmd; ++i) {
                const NkUiDrawCmd *cmd = &list->cmd[i];
                const NkUiTextRun *run = cmd->kind == NK_UI_DRAW_TEXT
                                             ? (const NkUiTextRun *)cmd->custom : NULL;
                float measured;
                if (run == NULL || run->text == NULL) continue;
                measured = whole_pixel_measure(NULL, run->font, run->size_px, run->text,
                                               run->length).width;
                if (cmd->element_id == 9u) {
                    if ((float)(cmd->bounds[2] - cmd->bounds[0]) < line_width - box_slack ||
                        (float)(cmd->bounds[2] - cmd->bounds[0]) > line_width + 1.0f + box_slack) {
                        fprintf(stderr,
                                "FAIL: a one-line run %.0f px wide has a box %d px wide: "
                                "\"%s\"\n", (double)line_width,
                                cmd->bounds[2] - cmd->bounds[0], paragraphs[p]);
                        nk_ui_destroy(ui);
                        return 0;
                    }
                    continue;
                }
                if (cmd->element_id != 7u) continue;
                if (measured > (float)width + line_slack) {
                    fprintf(stderr,
                            "FAIL: in a %d px column the wrapped line \"%.*s\" is %.0f px "
                            "wide\n", width, (int)run->length, run->text, (double)measured);
                    nk_ui_destroy(ui);
                    return 0;
                }
                if (used + run->length + 2u < sizeof(joined)) {
                    if (lines > 0) joined[used++] = ' ';
                    memcpy(joined + used, run->text, run->length);
                    used += run->length;
                }
                ++lines;
            }
            joined[used] = '\0';
            while (used > 0u && joined[used - 1u] == ' ') joined[--used] = '\0';
            CHECK(strcmp(joined, paragraphs[p]) == 0,
                  "the wrapped lines together are the paragraph");
            while (needed < 40 &&
                   strcmp(nk_ui_text_clamp(ui, paragraphs[p], &body, (float)width, needed),
                          paragraphs[p]) != 0) {
                ++needed;
            }
            if (lines < needed - (in_context ? 0 : 1) || lines > needed + 1) {
                fprintf(stderr,
                        "FAIL: in a %d px column the paragraph wraps to %d lines where "
                        "nk_ui_text_clamp counts %d: \"%s\"\n", width, lines, needed,
                        paragraphs[p]);
                nk_ui_destroy(ui);
                return 0;
            }
        }
    }
    nk_ui_destroy(ui);
    return 1;
}

/* One frame, laid out twice: `text` through nk_ui_text_break as card text in a column `width`
   px wide (element 7). *broken receives a copy of what nk_ui_text_break gave the layout. */
static const NkUiDrawList *broken_lines_frame(NkUi *ui, const char *text, float width,
                                              char *broken, size_t cap) {
    NkUiViewport viewport;
    NkUiLayout layout;
    NkUiTextStyle body = nk_ui_type(NK_UI_TYPE_BODY_S, 0u);
    const NkUiDrawList *list = NULL;
    memset(&viewport, 0, sizeof(viewport));
    /* Tall enough that a column one letter wide keeps every line on screen. */
    viewport.pixel_w = 1600;
    viewport.pixel_h = 8000;
    viewport.display_scale = 1.0f;
    viewport.ui_scale_pct = 100;
    viewport.text_size_pct = 100;
    nk_ui_set_viewport(ui, &viewport);
    for (int frame = 0; frame < 2; ++frame) {
        const char *shown;
        nk_ui_begin(ui, 37.0);
        memset(&layout, 0, sizeof(layout));
        layout.width = (NkUiSizing){NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f};
        layout.height = (NkUiSizing){NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f};
        layout.direction = NK_UI_LAYOUT_COLUMN;
        if (!nk_ui_begin_container(ui, 1u, &layout, NULL)) return NULL;
        memset(&layout, 0, sizeof(layout));
        layout.width = (NkUiSizing){NK_UI_SIZE_FIXED, width, 0.0f, 0.0f};
        layout.height = (NkUiSizing){NK_UI_SIZE_FIT, 0.0f, 0.0f, 0.0f};
        layout.direction = NK_UI_LAYOUT_COLUMN;
        shown = nk_ui_text_break(ui, text, &body, width);
        (void)snprintf(broken, cap, "%s", shown);
        if (nk_ui_begin_container(ui, 6u, &layout, NULL)) {
            nk_ui_text(ui, 7u, shown, &body);
            nk_ui_end_container(ui);
        }
        nk_ui_end_container(ui);
        list = nk_ui_end(ui);
    }
    return list;
}

/* nk_ui_text_break (#2643, the follow-up to #2644): a word wider than its box is broken onto
   lines that each fit the box, so wrapped text never runs past it, and nothing is lost: the
   lines together are the text, spaces aside. A path breaks after a '/' or '-' where one keeps
   at least half the line, a word with a multi-byte letter never inside that letter, words
   that fit are left whole for the layout to wrap, and text with no word too wide is given
   back as it is. In columns from 4 px (narrower than any letter: one letter a line) to
   420 px. Failing before: the layout put the long word on one line wider than the box. */
static int test_text_break_keeps_every_line_in_its_box(void) {
    static const char *const texts[] = {
        "ULUS10000_Remastered_HD_Texture_Pack_v2.1_final_with_widescreen_HUD",
        "Folder: C:/Games/PSP/texture-packs/ULUS10000/replacements/high-resolution and more",
        "Texturfilterungsqualit\xC3\xA4tsstufenauswahlmen\xC3\xBC und "
        "Texturfilterungsqualit\xC3\xA4tsstufenauswahlmen\xC3\xBC",
        "Short words only, which the layout wraps by itself.",
    };
    static char broken[1024];
    NkUiConfig config;
    NkUiTextBackend whole = nk_ui_text_fixed();
    NkUiTextStyle body = nk_ui_type(NK_UI_TYPE_BODY_S, 0u);
    NkUi *ui;
    whole.measure = whole_pixel_measure;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &whole;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create a context that measures in whole pixels");
    for (size_t t = 0u; t < sizeof(texts) / sizeof(texts[0]); ++t) {
        for (int width = 4; width <= 420; width += (width < 40 ? 4 : 7)) {
            const NkUiDrawList *list = broken_lines_frame(ui, texts[t], (float)width, broken,
                                                          sizeof(broken));
            char joined[1024];
            char want[1024];
            size_t used = 0u;
            size_t wanted = 0u;
            char message[300];
            CHECK(list != NULL, "lay out the broken text");
            for (uint32_t i = 0u; i < list->ncmd; ++i) {
                const NkUiDrawCmd *cmd = &list->cmd[i];
                const NkUiTextRun *run = cmd->kind == NK_UI_DRAW_TEXT
                                             ? (const NkUiTextRun *)cmd->custom : NULL;
                float measured;
                size_t glyphs;
                if (run == NULL || run->text == NULL || cmd->element_id != 7u) continue;
                /* The glyphs: a line the layout ends at a word that fits only without its
                   space keeps that space, which draws nothing. */
                glyphs = run->length;
                while (glyphs > 0u && run->text[glyphs - 1u] == ' ') --glyphs;
                measured = whole_pixel_measure(NULL, run->font, run->size_px, run->text,
                                               glyphs).width;
                /* A box narrower than any letter still shows one letter a line. */
                (void)snprintf(message, sizeof(message),
                               "in a %d px column the line \"%.*s\" is %.0f px wide", width,
                               (int)run->length, run->text, (double)measured);
                CHECK(measured <= (float)width ||
                          (run->length <= 2u && ((unsigned char)run->text[0] & 0x80u) == 0u) ||
                          (run->length <= 3u && ((unsigned char)run->text[0] & 0xC0u) == 0xC0u),
                      message);
                (void)snprintf(message, sizeof(message),
                               "in a %d px column a line starts inside a letter: \"%.*s\"",
                               width, (int)run->length, run->text);
                CHECK(run->length == 0u || ((unsigned char)run->text[0] & 0xC0u) != 0x80u,
                      message);
                for (size_t c = 0u; c < run->length && used + 1u < sizeof(joined); ++c) {
                    if (run->text[c] != ' ' && run->text[c] != '\n') joined[used++] = run->text[c];
                }
            }
            joined[used] = '\0';
            for (const char *c = texts[t]; *c != '\0' && wanted + 1u < sizeof(want); ++c) {
                if (*c != ' ') want[wanted++] = *c;
            }
            want[wanted] = '\0';
            (void)snprintf(message, sizeof(message),
                           "in a %d px column the lines together are the text: \"%.200s\"", width,
                           joined);
            CHECK(strcmp(joined, want) == 0, message);
            if (t == 3u && width >= 60) {
                (void)snprintf(message, sizeof(message),
                               "in a %d px column text of short words is given back as it is",
                               width);
                CHECK(strcmp(broken, texts[t]) == 0, message);
            }
            if (t == 1u && width >= 200) {
                /* "Folder:" and "and more" fit: they are left whole, with their spaces. */
                const char *line = broken;
                CHECK(strncmp(broken, "Folder: ", 8u) == 0 &&
                          strstr(broken, " and more") != NULL,
                      "the words that fit around a path keep their spaces");
                /* Every line the break ends inside the path ends after a '/' or '-'. */
                while ((line = strchr(line, '\n')) != NULL) {
                    (void)snprintf(message, sizeof(message),
                                   "in a %d px column the path breaks after '%c': \"%.200s\"",
                                   width, line[-1], broken);
                    CHECK(line[-1] == '/' || line[-1] == '-', message);
                    ++line;
                }
            }
        }
    }
    {
        /* Without an open frame there is no storage for it: an empty string. */
        CHECK(nk_ui_text_break(ui, texts[0], &body, 100.0f)[0] == '\0',
              "nk_ui_text_break outside a frame gives \"\"");
    }
    nk_ui_destroy(ui);
    return 1;
}

/* Each measure the layout asks of the backend, in order (audit 2026-11-17). */
#define MEASURE_LOG_CAP 4096
static struct {
    const char *text;
    size_t length;
} g_measure_log[MEASURE_LOG_CAP];
static size_t g_measure_count;

static NkUiTextSize logging_measure(void *user, int font, int px, const char *text,
                                    size_t length) {
    if (g_measure_count < MEASURE_LOG_CAP) {
        g_measure_log[g_measure_count].text = text;
        g_measure_log[g_measure_count].length = length;
    }
    ++g_measure_count;
    return whole_pixel_measure(user, font, px, text, length);
}

/* The word measure of a long paragraph after a line break (audit 2026-11-17). F-5843d890: a
   word slice was measured twice in a row, once to test its width and once more as the word
   alone. F-a2c8c85a: to place the grid its measuring starts on, every word read back to the
   start of its paragraph, so a long paragraph cost its length for each of its words; the grid
   is now the run's, found within the measuring context. Here the run starts with a short line,
   so the paragraph's grid and the run's differ: a word starts at byte 129 of the run, just
   after the run's grid point 128 and before the paragraph's, 132. Every measure of text that
   holds a space starts at the start of a line, or at the first word at or after a point of
   the run's 128-byte grid. */
static int test_word_measure_reads_a_bounded_context(void) {
    char text[640];
    NkUiConfig config;
    NkUiTextBackend logging = nk_ui_text_fixed();
    NkUi *ui;
    const char *base = NULL;
    size_t used;
    size_t gridded = 0u;
    memcpy(text, "Hi.\n", 4u);
    used = 4u;
    /* Words of seven letters and a space start at 4 + 8k; one of four letters at byte 124
       puts the next word start at 129, and the rest at 129 + 8k. */
    while (used < 600u) {
        const char *word = used == 124u ? "abcd " : "abcdefg ";
        memcpy(text + used, word, strlen(word));
        used += strlen(word);
    }
    text[used - 1u] = '.';
    text[used] = '\0';
    CHECK(text[128] == ' ' && text[129] != ' ', "a word of the paragraph starts at byte 129");
    logging.measure = logging_measure;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &logging;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create a context that logs its measures");
    g_measure_count = 0u;
    CHECK(wrapped_lines_frame(ui, text, 300.0f) != NULL, "lay out the two-line run");
    CHECK(g_measure_count > 0u && g_measure_count <= MEASURE_LOG_CAP, "the measures fit the log");
    for (size_t i = 0u; i < g_measure_count; ++i) {
        if (g_measure_log[i].length >= 3u && memcmp(g_measure_log[i].text, "Hi.", 3u) == 0 &&
            (base == NULL || g_measure_log[i].text < base)) {
            base = g_measure_log[i].text;
        }
    }
    CHECK(base != NULL, "the run's first word is measured from its start");
    for (size_t i = 0u; i < g_measure_count; ++i) {
        const char *start = g_measure_log[i].text;
        size_t offset;
        size_t grid;
        bool first_word = true;
        if (i > 0u && start == g_measure_log[i - 1u].text &&
            g_measure_log[i].length == g_measure_log[i - 1u].length && start >= base &&
            start < base + used && g_measure_log[i].length > 1u) {
            fprintf(stderr, "FAIL: the slice at byte %zu is measured twice in a row\n",
                    (size_t)(start - base));
            nk_ui_destroy(ui);
            return 0;
        }
        if (start < base || start >= base + used ||
            memchr(start, ' ', g_measure_log[i].length) == NULL ||
            start == base || start[-1] == '\n') {
            continue;
        }
        offset = (size_t)(start - base);
        grid = offset / 128u * 128u;
        for (size_t t = grid; t < offset; ++t) {
            if (t > 0u && base[t - 1u] == ' ') first_word = false;
        }
        if (start[-1] != ' ' || !first_word) {
            fprintf(stderr, "FAIL: a measure starts at byte %zu, not at the first word after "
                    "the run's grid point %zu\n", offset, grid);
            nk_ui_destroy(ui);
            return 0;
        }
        ++gridded;
    }
    CHECK(gridded > 0u, "words past the context are measured from a grid point");
    nk_ui_destroy(ui);
    return 1;
}

/* The text size scales type only: the glyphs, their measured width, the line
   boxes and the wrapping grow, while the density, the padding, the gaps and a
   fixed box stay. A one-line component keeps its height while its text fits
   (a button) and grows when it would not (a chip). Before, the text size was a
   second whole-layout scale: a 150% text size laid everything out at 1.5. */
static int test_text_size_scales_type_only(void) {
    NkUiConfig config;
    NkUiTextBackend fixed = nk_ui_text_fixed();
    NkUiTextStyle body = nk_ui_type(NK_UI_TYPE_BODY, 0u);
    NkUiTextStyle label_s = nk_ui_type(NK_UI_TYPE_LABEL_S, 0u);
    NkUiTextStyle label = nk_ui_type(NK_UI_TYPE_LABEL, 0u);
    NkUiDrawCmd normal[8];
    NkUiDrawCmd large[8];
    int normal_text_px;
    int large_text_px;
    int normal_runs;
    int large_runs;
    const NkUiDrawList *list;
    NkUi *ui;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create a context for the text size");
    list = text_size_frame(ui, 100);
    CHECK(list != NULL, "lay out the frame at 100% text");
    CHECK(nk_ui_text_scale(ui) == 1.0f && nk_ui_line_height(ui, &body) == 22.0f &&
              nk_ui_line_box(ui, NK_UI_CHIP_HEIGHT, &label_s) == NK_UI_CHIP_HEIGHT,
          "at 100% a line is its style's line height and a chip keeps its height");
    {
        static const uint32_t ids[8] = {2u, 3u, 3u, 4u, 5u, 7u, 3u, 4u};
        static const int kinds[8] = {NK_UI_DRAW_TEXT, NK_UI_DRAW_BORDER, NK_UI_DRAW_TEXT,
                                     NK_UI_DRAW_BORDER, NK_UI_DRAW_RECTANGLE, NK_UI_DRAW_TEXT,
                                     NK_UI_DRAW_BORDER, NK_UI_DRAW_BORDER};
        for (int i = 0; i < 8; ++i) {
            const NkUiDrawCmd *found = text_size_command(list, ids[i], kinds[i]);
            CHECK(found != NULL, "every element draws at 100% text");
            normal[i] = *found;
        }
    }
    normal_text_px = ((const NkUiTextRun *)normal[0].custom)->size_px;
    normal_runs = text_size_runs(list, 7u);
    list = text_size_frame(ui, 150);
    CHECK(list != NULL, "lay out the frame at 150% text");
    CHECK(nk_ui_text_scale(ui) == 1.5f && nk_ui_line_height(ui, &body) == 33.0f &&
              nk_ui_line_box(ui, NK_UI_CHIP_HEIGHT, &label_s) == 32.0f &&
              nk_ui_line_box(ui, NK_UI_CONTROL_HEIGHT, &label) == NK_UI_CONTROL_HEIGHT,
          "at 150% a line is 1.5 times as tall, a chip grows to fit it, a control need not");
    {
        static const uint32_t ids[8] = {2u, 3u, 3u, 4u, 5u, 7u, 3u, 4u};
        static const int kinds[8] = {NK_UI_DRAW_TEXT, NK_UI_DRAW_BORDER, NK_UI_DRAW_TEXT,
                                     NK_UI_DRAW_BORDER, NK_UI_DRAW_RECTANGLE, NK_UI_DRAW_TEXT,
                                     NK_UI_DRAW_BORDER, NK_UI_DRAW_BORDER};
        for (int i = 0; i < 8; ++i) {
            const NkUiDrawCmd *found = text_size_command(list, ids[i], kinds[i]);
            CHECK(found != NULL, "every element draws at 150% text");
            large[i] = *found;
        }
    }
    large_text_px = ((const NkUiTextRun *)large[0].custom)->size_px;
    large_runs = text_size_runs(list, 7u);
    CHECK(normal_text_px == 16 && large_text_px == 24,
          "the glyphs are drawn at 1.5 times the size at density 1");
    CHECK(text_size_width(&large[0]) >= text_size_width(&normal[0]) * 3 / 2 - 1 &&
              text_size_height(&large[0]) == text_size_height(&normal[0]) * 3 / 2,
          "a run is measured 1.5 times as wide, in a line box 1.5 times as tall");
    CHECK(normal[0].bounds[0] == 16 && large[0].bounds[0] == 16 &&
              normal[0].bounds[1] == 16 && large[0].bounds[1] == 16,
          "the column's padding stays 16 px");
    CHECK(text_size_height(&normal[1]) == 44 && text_size_height(&large[1]) == 44 &&
              text_size_width(&large[1]) > text_size_width(&normal[1]),
          "the button keeps its 44 px height and widens around its larger label");
    CHECK(large[1].bounds[1] - (normal[0].bounds[1] + text_size_height(&large[0])) ==
              normal[1].bounds[1] - (normal[0].bounds[1] + text_size_height(&normal[0])),
          "the gap under the label stays 10 px");
    CHECK(text_size_height(&normal[3]) == 26 && text_size_height(&large[3]) == 32,
          "the status pill grows from 26 to 32 px to fit its larger word");
    CHECK(text_size_width(&normal[4]) == 120 && text_size_height(&normal[4]) == 30 &&
              text_size_width(&large[4]) == 120 && text_size_height(&large[4]) == 30,
          "a fixed box keeps its size");
    CHECK(normal_runs >= 2 && large_runs > normal_runs,
          "the paragraph wraps into more lines in its 200 px column");
    CHECK(normal[5].bounds[0] == large[5].bounds[0], "the paragraph starts at the same x");
    CHECK(strcmp(nk_ui_text_clamp(ui, "Open library settings", &body, 170.0f, 1),
                 "Open library settings") != 0,
          "a run that fits one 170 px line at 100% is clamped with an ellipsis at 150%");
    list = text_size_frame(ui, 100);
    {
        const NkUiDrawCmd *button = list != NULL ? text_size_command(list, 3u, NK_UI_DRAW_BORDER)
                                                 : NULL;
        const NkUiDrawCmd *label = list != NULL ? text_size_command(list, 2u, NK_UI_DRAW_TEXT)
                                                : NULL;
        CHECK(button != NULL && label != NULL &&
                  memcmp(button->bounds, normal[1].bounds, sizeof(normal[1].bounds)) == 0 &&
                  ((const NkUiTextRun *)label->custom)->size_px == 16,
              "back at 100% the frame is laid out as before");
    }
    nk_ui_destroy(ui);
    return 1;
}

static int test_explicit_ui_scale_values(void) {
    static const int supported[] = {100, 125, 150, 200};
    NkUiViewport viewport = {1600, 1000, 2.0f, 0, 100, 0.0f, 0.0f};
    NkUiScale scale;
    size_t i;
    scale = nk_ui_scale_for_viewport(&viewport);
    CHECK(scale.density == 2.0f, "ui_scale_pct 0 follows the display scale");
    viewport.ui_scale_pct = 100;
    scale = nk_ui_scale_for_viewport(&viewport);
    CHECK(scale.density == 1.0f, "ui_scale_pct 100 forces 1x on a high-DPI display");
    CHECK(scale.logical_w == 1600.0f && scale.logical_h == 1000.0f,
          "forced 1x keeps logical size equal to pixel size");
    for (i = 0; i < sizeof(supported) / sizeof(supported[0]); ++i) {
        viewport.ui_scale_pct = supported[i];
        scale = nk_ui_scale_for_viewport(&viewport);
        CHECK(scale.density == (float)supported[i] / 100.0f,
              "each supported explicit ui_scale_pct sets that density");
    }
    viewport.ui_scale_pct = 175;
    scale = nk_ui_scale_for_viewport(&viewport);
    CHECK(scale.density == 1.0f, "an unsupported ui_scale_pct falls back to 1x");
    return 1;
}

static int test_typed_prefs_and_events(void) {
    NkUiPrefs prefs;
    NkUiEvent event;
    NkUi *ui;
    memset(&prefs, 0, sizeof(prefs));
    memset(&event, 0, sizeof(event));
    CHECK(prefs.prompt_style == NK_UI_PROMPT_NEUTRAL,
          "zeroed prefs select the neutral prompt set");
    CHECK(prefs.repeat == NK_UI_REPEAT_NORMAL, "zeroed prefs select normal repeat");
    CHECK(event.dir == NK_UI_DIR_NONE && event.pad_family == NK_UI_PAD_NEUTRAL,
          "zeroed events carry no direction and a neutral pad family");
    CHECK(NK_UI_PROMPT_NEUTRAL != NK_UI_PROMPT_LETTERS_A_SOUTH &&
          NK_UI_PROMPT_LETTERS_A_SOUTH != NK_UI_PROMPT_LETTERS_A_EAST &&
          NK_UI_PROMPT_LETTERS_A_EAST != NK_UI_PROMPT_KEYCAPS &&
          NK_UI_PROMPT_NEUTRAL != NK_UI_PROMPT_KEYCAPS,
          "prompt styles are distinct");
    CHECK(NK_UI_REPEAT_SLOW != NK_UI_REPEAT_NORMAL &&
          NK_UI_REPEAT_NORMAL != NK_UI_REPEAT_FAST &&
          NK_UI_REPEAT_SLOW != NK_UI_REPEAT_FAST, "repeat rates are distinct");
    prefs.prompt_style = NK_UI_PROMPT_KEYCAPS;
    prefs.repeat = NK_UI_REPEAT_FAST;
    event.type = NK_UI_EV_TAB;
    event.dir = NK_UI_DIR_LEFT;
    event.pad_family = NK_UI_PAD_LETTERS_A_EAST;
    event.down = true; /* a press of a shoulder button: the screen takes it */
    ui = nk_ui_create(NULL);
    CHECK(ui != NULL, "create context for typed prefs");
    CHECK(nk_ui_prompt_style(ui) == NK_UI_PROMPT_NEUTRAL, "a new context starts with neutral prompts");
    nk_ui_set_prefs(ui, &prefs);
    CHECK(nk_ui_prompt_style(ui) == NK_UI_PROMPT_KEYCAPS, "the prefs set the prompt style");
    nk_ui_push_event(ui, &event);
    {
        NkUiNavDir dir = NK_UI_DIR_NONE;
        bool took;
        nk_ui_begin(ui, 0.0);
        took = nk_ui_take_event(ui, NK_UI_EV_TAB, &dir);
        (void)nk_ui_end(ui);
        CHECK(took && dir == NK_UI_DIR_LEFT, "the pushed event is queued with its direction");
        nk_ui_begin(ui, 0.1);
        took = nk_ui_take_event(ui, NK_UI_EV_TAB, &dir);
        (void)nk_ui_end(ui);
        CHECK(!took, "a taken event is gone");
    }
    nk_ui_destroy(ui);
    return 1;
}

static int theme_probe_dump(const NkUiTheme *theme, char *dump, size_t cap) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    NkUiConfig config;
    NkUiLayout layout;
    NkUiStyle style;
    NkUi *ui;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    config.theme = theme;
    memset(&layout, 0, sizeof(layout));
    layout.width = (NkUiSizing){NK_UI_SIZE_FIXED, 160.0f, 0.0f, 0.0f};
    layout.height = (NkUiSizing){NK_UI_SIZE_FIXED, 80.0f, 0.0f, 0.0f};
    layout.direction = NK_UI_LAYOUT_COLUMN;
    memset(&style, 0, sizeof(style));
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create context with NULL or explicit N1 theme");
    nk_ui_begin(ui, 37.0);
    CHECK(nk_ui_begin_container(ui, 1u, &layout, &style), "open theme probe container");
    nk_ui_label(ui, 2u, "Theme", 0, 16);
    (void)nk_ui_button(ui, 3u, "Go", NK_UI_BUTTON_PRIMARY);
    nk_ui_end_container(ui);
    CHECK(nk_ui_end(ui) != NULL, "complete theme probe frame");
    CHECK(nk_ui_dump(ui, dump, cap) < cap, "theme probe dump fits");
    nk_ui_destroy(ui);
    return 1;
}

static int test_theme_access(void) {
    char with_null[8192];
    char with_n1[8192];
    CHECK(nk_ui_theme_n1() != NULL, "public nk_ui_theme_n1 returns a theme");
    CHECK(nk_ui_theme_n1() == nk_ui_theme_n1(), "nk_ui_theme_n1 returns one stable pointer");
    CHECK(theme_probe_dump(NULL, with_null, sizeof(with_null)), "dump with a NULL theme");
    CHECK(theme_probe_dump(nk_ui_theme_n1(), with_n1, sizeof(with_n1)),
          "dump with the explicit N1 theme");
    CHECK(strcmp(with_null, with_n1) == 0, "a NULL theme selects the N1 theme");
    return 1;
}

static int test_overlay_rejects_waves(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    NkUiConfig config;
    NkUi *ui;
    double wake = 0.0;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_OVERLAY;
    config.text = &fixed;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create overlay context");
    nk_ui_begin(ui, 37.0);
    CHECK(!nk_ui_waves_element(ui, 9u, (NkUiRect){0.0f, 0.0f, 64.0f, 64.0f}, 0u),
          "overlay host returns false for waves instead of aborting");
    CHECK(nk_ui_end(ui) != NULL, "overlay frame still completes");
    CHECK(!nk_ui_next_wake(ui, &wake), "overlay frame schedules no wave wake");
    nk_ui_destroy(ui);
    return 1;
}

static int test_container_return_contract(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    NkUiConfig config;
    NkUiLayout layout;
    NkUi *ui;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    memset(&layout, 0, sizeof(layout));
    layout.width = (NkUiSizing){NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f};
    layout.height = (NkUiSizing){NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f};
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create container contract context");
    CHECK(!nk_ui_begin_container(ui, 1u, &layout, NULL),
          "begin_container returns false before nk_ui_begin");
    nk_ui_begin(ui, 37.0);
    CHECK(!nk_ui_begin_container(ui, 1u, NULL, NULL),
          "begin_container returns false for a NULL layout");
    CHECK(nk_ui_begin_container(ui, 1u, &layout, NULL),
          "begin_container returns true when opened");
    nk_ui_end_container(ui);
    CHECK(nk_ui_end(ui) != NULL, "balanced containers leave a good frame");
    nk_ui_destroy(ui);
    return 1;
}

static int test_backend_declarations(void) {
    /* Built without NK_UI_WITH_VULKAN here: the Vulkan entry points are stubs. */
    CHECK(!nk_ui_vk_available() && nk_ui_vk_create(NULL) == NULL,
          "a build without the Vulkan backend reports it and creates nothing");
    CHECK(!nk_ui_vk_prepare(NULL, NULL, NULL, 0u), "Vulkan prepare entry point refuses a NULL list");
    CHECK(!nk_ui_vk_render(NULL, NULL, NULL, 0u, 0u), "Vulkan render entry point refuses a NULL list");
    CHECK(!nk_ui_sdlr_render(NULL, NULL), "SDL renderer entry point refuses a NULL list");
    return 1;
}

/* Rect edges land on whole pixels; the quarter rings of rounded borders put
   their edges on pixel centres, so a vertex is on a whole or a half pixel. */
static int rect_vertices_are_integral(const NkUiDrawList *list) {
    uint32_t i;
    if (list == NULL) return 0;
    for (i = 0; i < list->nv; ++i) {
        if (!isfinite(list->v[i].x) || !isfinite(list->v[i].y) ||
            floorf(list->v[i].x * 2.0f) != list->v[i].x * 2.0f ||
            floorf(list->v[i].y * 2.0f) != list->v[i].y * 2.0f) {
            return 0;
        }
    }
    return 1;
}

static int append_text(char *out, size_t cap, size_t *used, const char *text) {
    int written;
    if (*used >= cap) return 0;
    written = snprintf(out + *used, cap - *used, "%s", text);
    if (written < 0 || (size_t)written >= cap - *used) return 0;
    *used += (size_t)written;
    return 1;
}

static int dump_synthetic_frame(const char *scale_name, int pixel_w, int pixel_h,
                                float display_scale, char *out, size_t cap,
                                size_t *used) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    NkUiConfig config;
    NkUiViewport viewport;
    NkUiLayout root_layout;
    NkUiStyle root_style;
    NkUi *ui;
    const NkUiDrawList *list;
    char dump[8192];
    size_t dump_size;

    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create Clay-backed nk_ui context");
    /* Every field, min_logical_w and min_logical_h included: a stale minimum
       layout lowers the density. */
    memset(&viewport, 0, sizeof(viewport));
    viewport.pixel_w = pixel_w;
    viewport.pixel_h = pixel_h;
    viewport.display_scale = display_scale;
    viewport.ui_scale_pct = 0;
    viewport.text_size_pct = 100;
    nk_ui_set_viewport(ui, &viewport);
    nk_ui_begin(ui, 37.0);

    memset(&root_layout, 0, sizeof(root_layout));
    memset(&root_style, 0, sizeof(root_style));
    root_layout.width = (NkUiSizing){NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f};
    root_layout.height = (NkUiSizing){NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f};
    root_layout.direction = NK_UI_LAYOUT_COLUMN;
    root_layout.gap = 10.0f;
    root_layout.padding_left = 16.0f;
    root_layout.padding_top = 16.0f;
    root_layout.padding_right = 16.0f;
    root_layout.padding_bottom = 16.0f;
    root_style.fill_rgba = 0x0B0F14FFu;
    root_style.border_rgba = 0x18202BFFu;
    root_style.border_width = 1.0f;
    root_style.corner_radius = 8.0f;
    CHECK(nk_ui_begin_container(ui, 1u, &root_layout, &root_style),
          "open synthetic root container");
    nk_ui_label(ui, 2u, "Core layout", 0, 16);
    (void)nk_ui_button(ui, 3u, "Open library", NK_UI_BUTTON_PRIMARY);
    (void)nk_ui_button(ui, 4u, "Settings", 0u);
    nk_ui_end_container(ui);
    list = nk_ui_end(ui);
    CHECK(list != NULL, "complete Clay frame");
    CHECK(rect_vertices_are_integral(list), "all rect-class vertices land on whole pixels");
    dump_size = nk_ui_dump(ui, dump, sizeof(dump));
    CHECK(dump_size < sizeof(dump), "synthetic dump fits test buffer");
    CHECK(append_text(out, cap, used, scale_name), "append scale marker");
    CHECK(append_text(out, cap, used, dump), "append synthetic core dump");
    nk_ui_destroy(ui);
    return 1;
}

static int build_golden_output(char *out, size_t cap) {
    size_t used = 0u;
    out[0] = '\0';
    CHECK(dump_synthetic_frame("scale=1.00\n", 640, 400, 1.0f,
                               out, cap, &used), "build 1x golden");
    CHECK(dump_synthetic_frame("scale=1.25\n", 800, 500, 1.25f,
                               out, cap, &used), "build 1.25x golden");
    CHECK(dump_synthetic_frame("scale=1.50\n", 960, 600, 1.5f,
                               out, cap, &used), "build 1.5x golden");
    CHECK(dump_synthetic_frame("scale=2.00\n", 1280, 800, 2.0f,
                               out, cap, &used), "build 2x golden");
    return 1;
}

static float srgb_linear(float value) {
    if (value <= 0.04045f) return value / 12.92f;
    return powf((value + 0.055f) / 1.055f, 2.4f);
}

static float luminance(float r, float g, float b) {
    return 0.2126f * srgb_linear(r) + 0.7152f * srgb_linear(g) +
           0.0722f * srgb_linear(b);
}

static int test_wave_caps(void) {
    const uint32_t surface = 0x0B0F14FFu;
    float surface_r = 11.0f / 255.0f;
    float surface_g = 15.0f / 255.0f;
    float surface_b = 20.0f / 255.0f;
    unsigned tint;
    uint8_t *pixels = (uint8_t *)malloc(160u * 96u * 4u);
    CHECK(pixels != NULL, "allocate wave reference buffer");
    for (tint = 0; tint <= NK_UI_WAVES_TINT_COUNT; ++tint) {
        NkUiWavePalette palette;
        uint32_t x;
        uint32_t y;
        CHECK(nk_ui_waves_palette(tint, &palette), "load base or monthly wave palette");
        CHECK(palette.cap > 0.0f && palette.cap <= NK_UI_WAVES_ALPHA_CAP_MAX + 1e-6f,
              "wave alpha cap remains at or under 0.24");
        CHECK(fabsf(palette.cap - nk_ui_waves_compute_cap(&palette, surface)) < 1e-6f,
              "wave cap is derived from its exact palette");
        CHECK(nk_ui_waves_contrast_ok(&palette, surface, NK_UI_WAVES_LUMINANCE_MAX),
              "wave palette stays below the contrast luminance limit");
        CHECK(nk_ui_waves_render_rgba(&palette, 37.0, 160u, 96u,
                                      pixels, 160u * 4u),
              "render project-authored CPU wave reference");
        for (y = 0; y < 96u; ++y) {
            for (x = 0; x < 160u; ++x) {
                const uint8_t *pixel = pixels + ((size_t)y * 160u + x) * 4u;
                float alpha = (float)pixel[3] / 255.0f;
                float r = (float)pixel[0] / 255.0f + surface_r * (1.0f - alpha);
                float g = (float)pixel[1] / 255.0f + surface_g * (1.0f - alpha);
                float b = (float)pixel[2] / 255.0f + surface_b * (1.0f - alpha);
                CHECK(alpha <= palette.cap + 1.0f / 255.0f,
                      "rendered alpha respects the palette cap");
                CHECK(luminance(r, g, b) <= NK_UI_WAVES_LUMINANCE_MAX + 0.0015f,
                      "rendered wave pixels preserve the contrast budget");
            }
        }
    }
    free(pixels);
    return 1;
}

static int test_rounding_edges(void) {
    NkUiPixelRect first = nk_ui_round_rect((NkUiRect){0.2f, 0.3f, 17.1f, 8.2f}, 1.25f);
    NkUiPixelRect neighbor = nk_ui_round_rect((NkUiRect){17.3f, 0.3f, 11.0f, 8.2f}, 1.25f);
    CHECK(first.x1 == neighbor.x0, "adjacent logical edges round to the same device pixel");
    CHECK(first.x0 == 0 && first.y0 == 0 && first.x1 == 22 && first.y1 == 11 &&
          neighbor.x0 == 22,
          "pixel rectangle edges use nearest whole device pixels");
    return 1;
}

static int test_progress_sweep(void) {
    const float travel = 300.0f;
    float previous = nk_ui_progress_sweep_offset(travel, 0.0);
    int step;
    CHECK(previous == 0.0f, "a zero phase holds the sweep at the start");
    CHECK(fabsf(nk_ui_progress_sweep_offset(travel, 1.2) - travel) < 0.5f,
          "the sweep reaches the far end half way through its cycle");
    CHECK(nk_ui_progress_sweep_offset(0.0f, 1.0) == 0.0f, "no travel, no movement");
    /* Three cycles at 120 Hz: the segment never jumps back to the start. */
    for (step = 1; step <= 3 * 288; ++step) {
        float offset = nk_ui_progress_sweep_offset(travel, step / 120.0);
        CHECK(offset >= 0.0f && offset <= travel, "the sweep stays on the track");
        CHECK(fabsf(offset - previous) < 0.02f * travel, "the sweep moves without a jump");
        previous = offset;
    }
    return 1;
}

static int test_focused_button_activation(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    NkUiConfig config;
    NkUiEvent event;
    NkUi *ui;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create button activation context");
    nk_ui_focus(ui, 42u);
    memset(&event, 0, sizeof(event));
    event.type = NK_UI_EV_ACTIVATE;
    event.down = true;
    nk_ui_push_event(ui, &event);
    nk_ui_begin(ui, 37.0);
    CHECK(nk_ui_button(ui, 42u, "Activate", 0u),
          "focused button reports a queued activation");
    CHECK(nk_ui_end(ui) != NULL, "complete focused button frame");
    nk_ui_destroy(ui);
    return 1;
}

/* One frame of a row of two buttons, base and base + 1. Returns the activated
   button, or 0. */
/* The clock draw_button_pair draws at; the tests of motion move it. */
static double g_button_pair_now = 37.0;

static NkUiId draw_button_pair(NkUi *ui, NkUiId base, NkUiId focus_request) {
    NkUiLayout layout;
    NkUiStyle style;
    NkUiId activated = 0u;
    memset(&layout, 0, sizeof(layout));
    layout.width = (NkUiSizing){NK_UI_SIZE_FIXED, 320.0f, 0.0f, 0.0f};
    layout.height = (NkUiSizing){NK_UI_SIZE_FIXED, 60.0f, 0.0f, 0.0f};
    layout.direction = NK_UI_LAYOUT_ROW;
    layout.gap = 16.0f;
    memset(&style, 0, sizeof(style));
    nk_ui_begin(ui, g_button_pair_now);
    if (focus_request != 0u) {
        nk_ui_focus(ui, focus_request);
    }
    if (nk_ui_begin_container(ui, base + 100u, &layout, &style)) {
        if (nk_ui_button(ui, base, "Keep", NK_UI_BUTTON_PRIMARY)) activated = base;
        if (nk_ui_button(ui, base + 1u, "Remove", 0u)) activated = base + 1u;
        nk_ui_end_container(ui);
    }
    (void)nk_ui_end(ui);
    return activated;
}

static int test_input_waits_for_a_new_screen_layout(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    NkUiConfig config;
    NkUiEvent nav;
    NkUiEvent activate;
    NkUi *ui;
    NkUiId activated = 0u;
    int frame;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    memset(&nav, 0, sizeof(nav));
    nav.type = NK_UI_EV_NAV;
    nav.dir = NK_UI_DIR_RIGHT;
    nav.down = true;
    memset(&activate, 0, sizeof(activate));
    activate.type = NK_UI_EV_ACTIVATE;
    activate.down = true;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create new screen context");
    for (frame = 0; frame < 3; ++frame) {
        (void)draw_button_pair(ui, 10u, frame == 0 ? 10u : 0u);
    }
    /* A new screen opens on its first button; Right then Enter arrive right after its
       first frame, before Clay has laid out its buttons. */
    (void)draw_button_pair(ui, 20u, 20u);
    CHECK(nk_ui_focused(ui) == 20u, "the new screen opens on its requested button");
    nk_ui_push_event(ui, &nav);
    nk_ui_push_event(ui, &activate);
    for (frame = 0; frame < 3 && activated == 0u; ++frame) {
        activated = draw_button_pair(ui, 20u, 0u);
    }
    CHECK(activated == 21u, "Right then Enter on a new screen's first frames picks the second button");
    CHECK(nk_ui_focused(ui) == 21u, "the move is kept, not lost to the unplaced graph");
    nk_ui_destroy(ui);
    return 1;
}

static void push_pointer(NkUi *ui, float x, float y, bool down) {
    NkUiEvent event;
    memset(&event, 0, sizeof(event));
    event.type = NK_UI_EV_POINTER;
    event.x = x;
    event.y = y;
    event.down = down;
    nk_ui_push_event(ui, &event);
}

/* The owner's first run: one click on COPY FILES AND CONTINUE swapped in the
   copy screen, whose CANCEL sat where the button was, and the same click
   cancelled the copy. A click activates a control only when its press and its
   release both land on it; a held pointer that moves is a drag, not a press. */
static int test_click_needs_press_and_release_on_one_control(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    NkUiConfig config;
    NkUi *ui;
    NkUiId activated = 0u;
    int frame;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create click context");
    for (frame = 0; frame < 3; ++frame) (void)draw_button_pair(ui, 10u, frame == 0 ? 10u : 0u);

    /* Press and release on one button: it fires on the release, once. */
    push_pointer(ui, 10.0f, 10.0f, true);
    CHECK(draw_button_pair(ui, 10u, 0u) == 0u, "a press alone activates nothing");
    CHECK(nk_ui_focused(ui) == 10u, "a press focuses the control under it");
    push_pointer(ui, 10.0f, 10.0f, false);
    CHECK(draw_button_pair(ui, 10u, 0u) == 10u, "the release on the pressed button fires it");
    CHECK(draw_button_pair(ui, 10u, 0u) == 0u, "it fires once");

    /* The copy button (10) and the copy screen's CANCEL (20), which takes its
       place once 10 fires. One click, one event a frame: the press, two small
       moves with the button held, the release. Before, the press fired 10 and
       a held move then pressed 20. */
    {
        bool copying = false;
        bool cancelled = false;
        int fired_on = -1;
        for (frame = 0; frame < 8; ++frame) {
            if (frame == 0) push_pointer(ui, 10.0f, 10.0f, true);
            if (frame == 1) push_pointer(ui, 11.0f, 10.0f, true);
            if (frame == 2) push_pointer(ui, 12.0f, 11.0f, true);
            if (frame == 3) push_pointer(ui, 12.0f, 11.0f, false);
            activated = draw_button_pair(ui, copying ? 20u : 10u, 0u);
            if (activated == 10u && !copying) {
                copying = true;
                fired_on = frame;
            } else if (activated == 20u) {
                cancelled = true;
            }
        }
        CHECK(copying && fired_on == 3, "the copy button fires once, on the release");
        CHECK(!cancelled, "the click does not press the CANCEL that took its place");
    }

    /* A host that changes screen drops the press, even for a control with the
       same id in the same place. */
    for (frame = 0; frame < 3; ++frame) (void)draw_button_pair(ui, 10u, 0u);
    push_pointer(ui, 10.0f, 10.0f, true);
    (void)draw_button_pair(ui, 10u, 0u);
    nk_ui_cancel_press(ui);
    push_pointer(ui, 10.0f, 10.0f, false);
    activated = 0u;
    for (frame = 0; frame < 3 && activated == 0u; ++frame) {
        activated = draw_button_pair(ui, 10u, 0u);
    }
    CHECK(activated == 0u, "a cancelled press's release activates nothing");

    /* A press on one button released on the other activates neither. */
    push_pointer(ui, 10.0f, 10.0f, true);
    (void)draw_button_pair(ui, 10u, 0u);
    push_pointer(ui, 300.0f, 10.0f, false);
    activated = 0u;
    for (frame = 0; frame < 3 && activated == 0u; ++frame) {
        activated = draw_button_pair(ui, 10u, 0u);
    }
    CHECK(activated == 0u, "a release away from the pressed button activates nothing");
    nk_ui_destroy(ui);
    return 1;
}

/* A row: a control that owns left and right (a segmented control), then a plain
   button to its right. */
static void horizontal_row_frame(NkUi *ui, NkUiNavDir *taken, bool *took) {
    NkUiLayout layout;
    memset(&layout, 0, sizeof(layout));
    layout.width = (NkUiSizing){NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f};
    layout.height = (NkUiSizing){NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f};
    layout.direction = NK_UI_LAYOUT_ROW;
    layout.gap = 12.0f;
    nk_ui_begin(ui, 37.0);
    *took = nk_ui_take_event(ui, NK_UI_EV_NAV, taken);
    if (nk_ui_begin_container(ui, 1u, &layout, NULL)) {
        (void)nk_ui_button(ui, 41u, "Segmented", NK_UI_BUTTON_OWNS_HORIZONTAL);
        (void)nk_ui_button(ui, 43u, "Plain", 0u);
        nk_ui_end_container(ui);
    }
    (void)nk_ui_end(ui);
}

static void push_nav(NkUi *ui, NkUiNavDir dir) {
    NkUiEvent event;
    memset(&event, 0, sizeof(event));
    event.type = NK_UI_EV_NAV;
    event.dir = dir;
    event.down = true;
    nk_ui_push_event(ui, &event);
}

/* True when the last frame's draw list fills a rect for `id` with `rgba`. */
static bool drew_fill(NkUi *ui, NkUiId id, uint32_t rgba) {
    char dump[8192];
    char want[48];
    (void)nk_ui_dump(ui, dump, sizeof(dump));
    (void)snprintf(want, sizeof(want), "%08X RECT", (unsigned)id);
    {
        const char *line = strstr(dump, want);
        char colour[16];
        (void)snprintf(colour, sizeof(colour), " %08X ", (unsigned)rgba);
        while (line != NULL) {
            const char *end = strchr(line, '\n');
            size_t length = end != NULL ? (size_t)(end - line) : strlen(line);
            const char *found = strstr(line, colour);
            if (found != NULL && (size_t)(found - line) < length) return true;
            line = strstr(line + 1, want);
        }
    }
    return false;
}

/* A 640 x 400 window at 1x, so nothing in these frames is culled. */
static void set_test_window(NkUi *ui) {
    NkUiViewport viewport;
    memset(&viewport, 0, sizeof(viewport));
    viewport.pixel_w = 640;
    viewport.pixel_h = 400;
    viewport.display_scale = 1.0f;
    viewport.text_size_pct = 100;
    nk_ui_set_viewport(ui, &viewport);
}

static void push_leave(NkUi *ui) {
    NkUiEvent event;
    memset(&event, 0, sizeof(event));
    event.type = NK_UI_EV_POINTER_LEAVE;
    nk_ui_push_event(ui, &event);
}

/* Hover and pressed looks come from Clay's pointer state over the last layout.
   Hover is for the pointer only: a key hides it until the pointer moves, it
   clears when the pointer leaves the window, and it never moves focus. The
   looks are checked at a clock that does not move, so motion is cut here;
   test_hover_fill_eases covers the easing. */
static int test_pointer_hover_and_press(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    const NkUiTheme *theme = nk_ui_theme_n1();
    NkUiConfig config;
    NkUiPrefs prefs;
    NkUi *ui;
    int frame;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create hover context");
    set_test_window(ui);
    memset(&prefs, 0, sizeof(prefs));
    prefs.reduce_motion = true;
    nk_ui_set_prefs(ui, &prefs);
    for (frame = 0; frame < 3; ++frame) (void)draw_button_pair(ui, 10u, frame == 0 ? 10u : 0u);
    CHECK(!nk_ui_hovered(ui, 10u) && !nk_ui_hovered(ui, 11u), "nothing is hovered before the pointer moves");

    push_pointer(ui, 100.0f, 10.0f, false);
    (void)draw_button_pair(ui, 10u, 0u);
    CHECK(nk_ui_hovered(ui, 11u) && !nk_ui_hovered(ui, 10u), "the pointer hovers the button under it");
    CHECK(nk_ui_focused(ui) == 10u, "hover does not move focus");
    CHECK(drew_fill(ui, 11u, theme->surface_high), "a hovered secondary button fills in");

    push_nav(ui, NK_UI_DIR_RIGHT);
    (void)draw_button_pair(ui, 10u, 0u);
    CHECK(!nk_ui_hovered(ui, 11u), "a key hides hover");
    push_pointer(ui, 101.0f, 10.0f, false);
    (void)draw_button_pair(ui, 10u, 0u);
    CHECK(nk_ui_hovered(ui, 11u), "moving the pointer shows it again");

    push_pointer(ui, 101.0f, 10.0f, true);
    (void)draw_button_pair(ui, 10u, 0u);
    CHECK(nk_ui_pressed(ui, 11u) && drew_fill(ui, 11u, theme->pressed),
          "a button held under the pointer is drawn pressed");
    push_pointer(ui, 10.0f, 10.0f, true);
    (void)draw_button_pair(ui, 10u, 0u);
    CHECK(!nk_ui_pressed(ui, 11u) && !nk_ui_pressed(ui, 10u),
          "a held pointer dragged off it presses nothing");
    push_pointer(ui, 10.0f, 10.0f, false);
    (void)draw_button_pair(ui, 10u, 0u);
    CHECK(nk_ui_hovered(ui, 10u) && drew_fill(ui, 10u, theme->accent_hover),
          "a hovered primary button lightens");

    push_leave(ui);
    (void)draw_button_pair(ui, 10u, 0u);
    CHECK(!nk_ui_hovered(ui, 10u) && drew_fill(ui, 10u, theme->accent_dark),
          "leaving the window clears hover");
    nk_ui_destroy(ui);
    return 1;
}

/* A button's hover fill eases in over NK_UI_MOTION_FAST: half way through it
   is neither the resting fill nor the hover fill, and nk_ui says it is still
   moving; then it is the hover fill and still. Reduce motion cuts it. */
static int test_hover_fill_eases(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    const NkUiTheme *theme = nk_ui_theme_n1();
    NkUiConfig config;
    NkUiPrefs prefs;
    NkUi *ui;
    int frame;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create hover easing context");
    set_test_window(ui);
    for (frame = 0; frame < 3; ++frame) {
        g_button_pair_now = 20.0 + frame * 0.01;
        (void)draw_button_pair(ui, 10u, frame == 0 ? 10u : 0u);
    }
    CHECK(!drew_fill(ui, 11u, theme->surface_high) && !nk_ui_in_motion(ui),
          "the secondary button rests unfilled");
    push_pointer(ui, 100.0f, 10.0f, false);
    g_button_pair_now = 20.05;
    (void)draw_button_pair(ui, 10u, 0u);
    g_button_pair_now = 20.05 + NK_UI_MOTION_FAST / 2.0;
    (void)draw_button_pair(ui, 10u, 0u);
    CHECK(nk_ui_hovered(ui, 11u) && !drew_fill(ui, 11u, theme->surface_high) &&
              nk_ui_in_motion(ui),
          "half way through, the hover fill is still easing in");
    for (frame = 1; frame <= 3; ++frame) {
        g_button_pair_now = 20.05 + NK_UI_MOTION_FAST * 2.0 * frame;
        (void)draw_button_pair(ui, 10u, 0u);
    }
    CHECK(drew_fill(ui, 11u, theme->surface_high) && !nk_ui_in_motion(ui),
          "then the button is filled and still");
    memset(&prefs, 0, sizeof(prefs));
    prefs.reduce_motion = true;
    nk_ui_set_prefs(ui, &prefs);
    push_pointer(ui, 10.0f, 10.0f, false);
    g_button_pair_now = 21.0;
    (void)draw_button_pair(ui, 10u, 0u);
    CHECK(drew_fill(ui, 10u, theme->accent_hover) && !nk_ui_in_motion(ui),
          "Reduce motion: the hover fill is there at once");
    g_button_pair_now = 37.0;
    nk_ui_destroy(ui);
    return 1;
}

/* Three 44 px buttons in a 50 px clip: the pointer below the clip is over no
   button, though the second and third run on under it. */
static NkUiId draw_clipped_column(NkUi *ui) {
    NkUiLayout layout;
    NkUiId activated = 0u;
    NkUiId id;
    memset(&layout, 0, sizeof(layout));
    layout.width = (NkUiSizing){NK_UI_SIZE_FIXED, 200.0f, 0.0f, 0.0f};
    layout.height = (NkUiSizing){NK_UI_SIZE_FIXED, 50.0f, 0.0f, 0.0f};
    layout.direction = NK_UI_LAYOUT_COLUMN;
    layout.scroll = true;
    nk_ui_begin(ui, 37.0);
    if (nk_ui_begin_container(ui, 1u, &layout, NULL)) {
        for (id = 20u; id < 23u; ++id) {
            if (nk_ui_button(ui, id, "Row", NK_UI_BUTTON_GROW)) activated = id;
        }
        nk_ui_end_container(ui);
    }
    (void)nk_ui_end(ui);
    return activated;
}

static int test_pointer_respects_clip(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    NkUiConfig config;
    NkUi *ui;
    NkUiId activated = 0u;
    int frame;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create clip context");
    set_test_window(ui);
    for (frame = 0; frame < 3; ++frame) (void)draw_clipped_column(ui);
    CHECK(nk_ui_focused(ui) == 20u, "the first row starts focused");
    push_pointer(ui, 50.0f, 70.0f, false);
    (void)draw_clipped_column(ui);
    CHECK(!nk_ui_hovered(ui, 21u), "a row clipped away is not hovered");
    push_pointer(ui, 50.0f, 70.0f, true);
    (void)draw_clipped_column(ui);
    push_pointer(ui, 50.0f, 70.0f, false);
    for (frame = 0; frame < 2 && activated == 0u; ++frame) activated = draw_clipped_column(ui);
    CHECK(activated == 0u && nk_ui_focused(ui) == 20u, "nor clicked");
    push_pointer(ui, 50.0f, 47.0f, false);
    (void)draw_clipped_column(ui);
    CHECK(nk_ui_hovered(ui, 21u), "the part inside the clip is");
    nk_ui_destroy(ui);
    return 1;
}

/* The top of text run `id` in the last frame's draw list, in device px, or
   -1 when the frame drew no such run. */
static int drawn_text_top(NkUi *ui, NkUiId id) {
    char dump[16384];
    char want[32];
    const char *line;
    int x0 = 0;
    int y0 = -1;
    (void)nk_ui_dump(ui, dump, sizeof(dump));
    (void)snprintf(want, sizeof(want), "%08X TEXT ", (unsigned)id);
    line = strstr(dump, want);
    if (line == NULL || sscanf(line + strlen(want), "%d %d", &x0, &y0) != 2) return -1;
    return y0;
}

/* The draw list of the last scroll-list frame. */
static const NkUiDrawList *g_scroll_list;

/* Ten 44 px rows (ids 40 to 49) in a 100 px scroll container (id 30) at the
   window's top left. Returns the row activated this frame, or 0. */
static NkUiId draw_scroll_list(NkUi *ui, bool *wheel_left) {
    NkUiLayout root;
    NkUiLayout list;
    NkUiNavDir dir;
    NkUiId activated = 0u;
    NkUiId id;
    memset(&root, 0, sizeof(root));
    root.width = (NkUiSizing){NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f};
    root.height = (NkUiSizing){NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f};
    root.direction = NK_UI_LAYOUT_COLUMN;
    memset(&list, 0, sizeof(list));
    list.width = (NkUiSizing){NK_UI_SIZE_FIXED, 200.0f, 0.0f, 0.0f};
    list.height = (NkUiSizing){NK_UI_SIZE_FIXED, 100.0f, 0.0f, 0.0f};
    list.direction = NK_UI_LAYOUT_COLUMN;
    list.scroll = true;
    nk_ui_begin(ui, 37.0);
    if (wheel_left != NULL) *wheel_left = nk_ui_take_event(ui, NK_UI_EV_WHEEL, &dir);
    if (nk_ui_begin_container(ui, 1u, &root, NULL)) {
        if (nk_ui_begin_container(ui, 30u, &list, NULL)) {
            for (id = 40u; id < 50u; ++id) {
                if (nk_ui_button(ui, id, "Row", NK_UI_BUTTON_GROW)) activated = id;
            }
            nk_ui_end_container(ui);
        }
        nk_ui_end_container(ui);
    }
    g_scroll_list = nk_ui_end(ui);
    return activated;
}

static void push_wheel(NkUi *ui, float x, float y, NkUiNavDir dir) {
    NkUiEvent event;
    memset(&event, 0, sizeof(event));
    event.type = NK_UI_EV_WHEEL;
    event.x = x;
    event.y = y;
    event.dir = dir;
    event.down = true;
    nk_ui_push_event(ui, &event);
}

static float scroll_offset(NkUi *ui) {
    NkUiScrollState state;
    return nk_ui_scroll_state(ui, 30u, &state) ? state.offset : -1.0f;
}

/* A scroll container scrolls through Clay: the wheel over it (a notch is 48
   px; a wheel anywhere else stays for the screen), a drag with the pointer
   held (and that drag is no click), focus moving to a row out of view, and
   nk_ui_scroll_to, which also sets where a container starts. */
static int test_scroll_container(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    NkUiConfig config;
    NkUiScrollState state;
    NkUi *ui;
    NkUiId activated = 0u;
    bool wheel_left = false;
    int frame;
    int top;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create scroll context");
    set_test_window(ui);
    for (frame = 0; frame < 3; ++frame) (void)draw_scroll_list(ui, NULL);
    CHECK(nk_ui_scroll_state(ui, 30u, &state) && state.offset == 0.0f &&
          state.max_offset == 340.0f && state.viewport == 100.0f,
          "the list starts at its top, with 340 px more than its 100 px of room");
    top = drawn_text_top(ui, 45u);

    push_wheel(ui, 50.0f, 50.0f, NK_UI_DIR_DOWN);
    (void)draw_scroll_list(ui, &wheel_left);
    CHECK(!wheel_left && scroll_offset(ui) == 48.0f && nk_ui_scroll_in_motion(ui),
          "a wheel notch over the list scrolls it 48 px and is not left for the screen");
    CHECK(drawn_text_top(ui, 45u) == top - 48, "the rows are drawn 48 px higher");
    (void)draw_scroll_list(ui, NULL);
    CHECK(scroll_offset(ui) == 48.0f && !nk_ui_scroll_in_motion(ui), "and stay there");
    push_wheel(ui, 400.0f, 300.0f, NK_UI_DIR_DOWN);
    (void)draw_scroll_list(ui, &wheel_left);
    CHECK(wheel_left && scroll_offset(ui) == 48.0f, "a wheel elsewhere stays for the screen");

    /* Keys: down from row 40 to row 47 (308 to 352 px down the content). */
    nk_ui_scroll_to(ui, 30u, 0.0f);
    (void)draw_scroll_list(ui, NULL);
    push_nav(ui, NK_UI_DIR_DOWN);
    (void)draw_scroll_list(ui, NULL);
    CHECK(nk_ui_focused(ui) == 41u && scroll_offset(ui) == 0.0f,
          "a move judges rows where the list was last drawn, so row 41 needs no scroll");
    for (frame = 0; frame < 6; ++frame) {
        push_nav(ui, NK_UI_DIR_DOWN);
        (void)draw_scroll_list(ui, NULL);
    }
    CHECK(NK_UI_FOCUS_RING_OUTSET >= NK_UI_FOCUS_RING_GAP + NK_UI_FOCUS_RING_WIDTH &&
          NK_UI_FOCUS_RING_OUTSET >= NK_UI_FOCUS_HC_KEYLINE * 2.0f + NK_UI_FOCUS_HC_RING_WIDTH,
          "the ring outset reaches as far as either focus ring");
    CHECK(nk_ui_focused(ui) == 47u, "focus walks down to row 47");
    CHECK(scroll_offset(ui) == 252.0f + NK_UI_FOCUS_RING_OUTSET,
          "the list scrolls just enough to show the focused row and its focus ring");
    push_nav(ui, NK_UI_DIR_UP);
    (void)draw_scroll_list(ui, NULL);
    CHECK(scroll_offset(ui) == 264.0f - NK_UI_FOCUS_RING_OUTSET,
          "row 46, in view but its ring cut at the top, scrolls just enough to show the ring");

    /* A drag: held on row 47 and moved up 70 px, the list follows the pointer
       and the release on the same row is no click. */
    push_pointer(ui, 50.0f, 80.0f, true);
    (void)draw_scroll_list(ui, NULL);
    push_pointer(ui, 50.0f, 10.0f, true);
    activated = draw_scroll_list(ui, NULL);
    CHECK(scroll_offset(ui) == 264.0f - NK_UI_FOCUS_RING_OUTSET + 70.0f,
          "a drag scrolls the list with the pointer");
    push_pointer(ui, 50.0f, 10.0f, false);
    for (frame = 0; frame < 2 && activated == 0u; ++frame) activated = draw_scroll_list(ui, NULL);
    CHECK(activated == 0u, "a drag that scrolled is no click");

    nk_ui_scroll_to(ui, 30u, 1000.0f);
    (void)draw_scroll_list(ui, NULL);
    CHECK(scroll_offset(ui) == 340.0f, "nk_ui_scroll_to stops at the end of the content");
    nk_ui_destroy(ui);

    /* A list asked to start 200 px down is drawn there from its first frame. */
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create second scroll context");
    set_test_window(ui);
    nk_ui_scroll_to(ui, 30u, 200.0f);
    (void)draw_scroll_list(ui, NULL);
    CHECK(drawn_text_top(ui, 45u) == top - 200 && scroll_offset(ui) == 200.0f,
          "a container starts where nk_ui_scroll_to asked");
    nk_ui_destroy(ui);
    return 1;
}

/* A 60 px band (id 1) holding one wide page button (60); with `modal`, a
   200 px dialog card (70) with its own button (71) floats centred over it. */
static NkUiId draw_modal_page(NkUi *ui, bool modal) {
    NkUiLayout band;
    NkUiId activated = 0u;
    memset(&band, 0, sizeof(band));
    band.width = (NkUiSizing){NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f};
    band.height = (NkUiSizing){NK_UI_SIZE_FIXED, 60.0f, 0.0f, 0.0f};
    band.direction = NK_UI_LAYOUT_COLUMN;
    nk_ui_begin(ui, 37.0);
    if (nk_ui_begin_container(ui, 1u, &band, NULL)) {
        if (nk_ui_button(ui, 60u, "Page", NK_UI_BUTTON_GROW)) activated = 60u;
        if (modal && nk_ui_dialog_begin(ui, 70u, 200.0f)) {
            if (nk_ui_button(ui, 71u, "Card", 0u)) activated = 71u;
            nk_ui_end_container(ui);
        }
        nk_ui_end_container(ui);
    }
    (void)nk_ui_end(ui);
    return activated;
}

/* A dialog card is a Clay floating element: it moves none of the page around
   it, draws over it, and takes the pointer, so the page under it is neither
   hovered nor clicked. */
static int test_modal_card_floats(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    NkUiConfig config;
    NkUi *ui;
    NkUiId activated = 0u;
    int frame;
    int page_top;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create modal context");
    set_test_window(ui);
    for (frame = 0; frame < 3; ++frame) (void)draw_modal_page(ui, false);
    page_top = drawn_text_top(ui, 60u);
    for (frame = 0; frame < 3; ++frame) (void)draw_modal_page(ui, true);
    CHECK(drawn_text_top(ui, 60u) == page_top, "the card moves nothing on the page");
    /* The card is 200 px wide, centred on the 640 px band, so its edge runs
       from 220 to 248 px before its button; the page button runs under it. */
    push_pointer(ui, 230.0f, 20.0f, false);
    (void)draw_modal_page(ui, true);
    CHECK(!nk_ui_hovered(ui, 60u) && !nk_ui_hovered(ui, 71u),
          "the pointer on the card's edge hovers nothing under it");
    push_pointer(ui, 230.0f, 20.0f, true);
    (void)draw_modal_page(ui, true);
    push_pointer(ui, 230.0f, 20.0f, false);
    for (frame = 0; frame < 2 && activated == 0u; ++frame) activated = draw_modal_page(ui, true);
    CHECK(activated == 0u, "nor clicks it");
    push_pointer(ui, 600.0f, 20.0f, false);
    (void)draw_modal_page(ui, true);
    CHECK(nk_ui_hovered(ui, 60u), "the page beside the card is still the pointer's");
    nk_ui_destroy(ui);
    return 1;
}

/* The focus ring is a floating element on its control: drawn after every
   other command, and clipped by the scroll container the control is in. */
static int test_focus_ring_floats(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    const NkUiTheme *theme = nk_ui_theme_n1();
    NkUiConfig config;
    const NkUiDrawList *list;
    NkUi *ui;
    uint32_t first_ring = UINT32_MAX;
    uint32_t last_other = 0u;
    uint32_t i;
    bool reaches_above = false;
    bool clipped = true;
    int frame;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create ring context");
    set_test_window(ui);
    for (frame = 0; frame < 3; ++frame) (void)draw_scroll_list(ui, NULL);
    nk_ui_focus(ui, 41u);
    (void)draw_scroll_list(ui, NULL);
    /* Row 41 runs from 44 to 88 px down the list; 60 px down, it starts 16 px
       above the list's top and its ring 22 px above. */
    nk_ui_scroll_to(ui, 30u, 60.0f);
    (void)draw_scroll_list(ui, NULL);
    CHECK(nk_ui_focused(ui) == 41u, "row 41 keeps focus");
    list = g_scroll_list;
    CHECK(list != NULL, "the frame was drawn");
    for (i = 0u; i < list->ncmd; ++i) {
        const NkUiDrawCmd *cmd = &list->cmd[i];
        if (cmd->kind == NK_UI_DRAW_BORDER && cmd->element_id == 41u &&
            cmd->rgba_pm == theme->focus) {
            if (first_ring == UINT32_MAX) first_ring = i;
            if (cmd->bounds[1] < 0) reaches_above = true;
            if (cmd->clip[3] > cmd->clip[1] && cmd->clip[1] < 0) clipped = false;
        } else if (cmd->kind != NK_UI_DRAW_SCISSOR_START && cmd->kind != NK_UI_DRAW_SCISSOR_END) {
            last_other = i;
        }
    }
    CHECK(first_ring != UINT32_MAX && first_ring > last_other, "the ring is drawn over everything");
    CHECK(reaches_above && clipped, "the ring is clipped where the list clips its row");
    nk_ui_destroy(ui);
    return 1;
}

/* Two 120 px buttons, 200 px apart, drawn at `now_s`. Returns the left edge of
   the focus ring's band, or INT_MIN when no ring is drawn. */
static int draw_ring_pair(NkUi *ui, double now_s) {
    const NkUiTheme *theme = nk_ui_theme_n1();
    NkUiLayout row;
    const NkUiDrawList *list;
    uint32_t i;
    memset(&row, 0, sizeof(row));
    row.width = (NkUiSizing){NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f};
    row.height = (NkUiSizing){NK_UI_SIZE_FIT, 0.0f, 0.0f, 0.0f};
    row.direction = NK_UI_LAYOUT_ROW;
    row.gap = 80.0f;
    row.padding_left = 40.0f;
    row.padding_top = 40.0f;
    nk_ui_begin(ui, now_s);
    if (nk_ui_begin_container(ui, 90u, &row, NULL)) {
        NkUiLayout cell = row;
        cell.width = (NkUiSizing){NK_UI_SIZE_FIXED, 120.0f, 0.0f, 0.0f};
        cell.gap = 0.0f;
        cell.padding_left = 0.0f;
        cell.padding_top = 0.0f;
        if (nk_ui_begin_container(ui, 91u, &cell, NULL)) {
            (void)nk_ui_button(ui, 81u, "One", 0u);
            nk_ui_end_container(ui);
        }
        if (nk_ui_begin_container(ui, 92u, &cell, NULL)) {
            (void)nk_ui_button(ui, 82u, "Two", 0u);
            nk_ui_end_container(ui);
        }
        nk_ui_end_container(ui);
    }
    list = nk_ui_end(ui);
    for (i = 0u; list != NULL && i < list->ncmd; ++i) {
        const NkUiDrawCmd *cmd = &list->cmd[i];
        if (cmd->kind == NK_UI_DRAW_BORDER && cmd->rgba_pm == theme->focus) return cmd->bounds[0];
    }
    return INT_MIN;
}

/* Touch moves no focus a person follows: in touch mode no ring is drawn, the high-contrast
   one included, and the first key or pad press (touch off) shows it again. */
static int test_focus_ring_waits_for_keys_after_touch(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    NkUiConfig config;
    NkUiPrefs prefs;
    NkUi *ui;
    int frame;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create touch ring context");
    set_test_window(ui);
    for (frame = 0; frame < 3; ++frame) (void)draw_ring_pair(ui, 0.0);
    nk_ui_focus(ui, 81u);
    CHECK(draw_ring_pair(ui, 0.0) != INT_MIN, "keys and pads: the focused control has a ring");
    memset(&prefs, 0, sizeof(prefs));
    prefs.touch = true;
    nk_ui_set_prefs(ui, &prefs);
    CHECK(draw_ring_pair(ui, 0.0) == INT_MIN && nk_ui_focused(ui) == 81u,
          "touch: no ring, and the focus stays where it was");
    prefs.high_contrast_focus = true;
    nk_ui_set_prefs(ui, &prefs);
    {
        const NkUiDrawList *list;
        uint32_t i;
        bool white = false;
        nk_ui_begin(ui, 0.0);
        (void)nk_ui_button(ui, 81u, "One", 0u);
        list = nk_ui_end(ui);
        for (i = 0u; list != NULL && i < list->ncmd; ++i) {
            if (list->cmd[i].kind == NK_UI_DRAW_BORDER && list->cmd[i].rgba_pm == 0xFFFFFFFFu) {
                white = true;
            }
        }
        CHECK(list != NULL && !white, "touch: no high-contrast ring either");
        prefs.touch = false;
        nk_ui_set_prefs(ui, &prefs);
        nk_ui_begin(ui, 0.0);
        (void)nk_ui_button(ui, 81u, "One", 0u);
        list = nk_ui_end(ui);
        for (i = 0u; list != NULL && i < list->ncmd; ++i) {
            if (list->cmd[i].kind == NK_UI_DRAW_BORDER && list->cmd[i].rgba_pm == 0xFFFFFFFFu) {
                white = true;
            }
        }
        CHECK(white, "a key after touch: the high-contrast ring is back");
    }
    prefs.high_contrast_focus = false;
    nk_ui_set_prefs(ui, &prefs);
    for (frame = 0; frame < 2; ++frame) (void)draw_ring_pair(ui, 1.0);
    CHECK(draw_ring_pair(ui, 1.0) != INT_MIN, "a key after touch: the ring is back");
    nk_ui_destroy(ui);
    return 1;
}

/* A prompt bar of Move, Confirm, Find (Options), Settings (Menu) and Back, at the bottom
   of a 640 x 400 window. */
static const NkUiPromptItem k_touch_prompts[] = {
    {NK_UI_PROMPT_ACT_MOVE, "Move"},
    {NK_UI_PROMPT_ACT_CONFIRM, "Open"},
    {NK_UI_PROMPT_ACT_OPTIONS, "Find"},
    {NK_UI_PROMPT_ACT_MENU, "Settings"},
    {NK_UI_PROMPT_ACT_BACK, "Back"}
};

static void draw_prompt_page(NkUi *ui) {
    NkUiLayout page;
    memset(&page, 0, sizeof(page));
    page.width = (NkUiSizing){NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f};
    page.height = (NkUiSizing){NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f};
    page.direction = NK_UI_LAYOUT_COLUMN;
    nk_ui_begin(ui, 0.0);
    if (nk_ui_begin_container(ui, 70u, &page, NULL)) {
        NkUiLayout body = page;
        if (nk_ui_begin_container(ui, 71u, &body, NULL)) {
            (void)nk_ui_button(ui, 72u, "Game", 0u);
            nk_ui_end_container(ui);
        }
        nk_ui_prompt_bar(ui, 73u, "", k_touch_prompts,
                         sizeof(k_touch_prompts) / sizeof(k_touch_prompts[0]));
        nk_ui_end_container(ui);
    }
    (void)nk_ui_end(ui);
}

/* The prompt item i's own container, and its glyph (nk_ui_prompt_bar's ids). */
static NkUiId prompt_item(size_t i) { return nk_ui_child_id(73u, 16u + (uint32_t)i * 4u); }

static void tap_at_middle(NkUi *ui, const NkUiRect *r) {
    push_pointer(ui, r->x + r->w * 0.5f, r->y + r->h * 0.5f, true);
    push_pointer(ui, r->x + r->w * 0.5f, r->y + r->h * 0.5f, false);
}

/* With touch only, nothing in the prompt bar could be tapped: every prompt was a key's
   glyph. In touch mode each action but Confirm and Move is a labelled button, and a tap
   does what its key does: Settings is the next frame's Menu event, and Back is the host's
   (nk_ui_take_prompt_back). */
static int test_prompt_bar_is_tappable_in_touch_mode(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    NkUiConfig config;
    NkUiPrefs prefs;
    NkUiRect menu;
    NkUiRect back;
    NkUiRect glyph;
    NkUi *ui;
    char dump[16384];
    int frame;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create prompt bar context");
    set_test_window(ui);
    memset(&prefs, 0, sizeof(prefs));
    prefs.prompt_style = NK_UI_PROMPT_KEYCAPS;
    nk_ui_set_prefs(ui, &prefs);
    for (frame = 0; frame < 3; ++frame) draw_prompt_page(ui);
    CHECK(nk_ui_element_rect(ui, nk_ui_child_id(prompt_item(3u), 1u), &glyph) && glyph.w > 0.0f,
          "keys: SETTINGS has its key's glyph");
    CHECK(nk_ui_element_rect(ui, prompt_item(3u), &menu), "keys: SETTINGS is laid out");
    tap_at_middle(ui, &menu);
    draw_prompt_page(ui);
    nk_ui_begin(ui, 0.0);
    CHECK(!nk_ui_take_event(ui, NK_UI_EV_MENU, NULL), "keys: a click on a glyph does nothing");
    (void)nk_ui_end(ui);

    prefs.touch = true;
    nk_ui_set_prefs(ui, &prefs);
    for (frame = 0; frame < 3; ++frame) draw_prompt_page(ui);
    (void)nk_ui_dump(ui, dump, sizeof(dump));
    CHECK(strstr(dump, "\"FIND\"") != NULL && strstr(dump, "\"SETTINGS\"") != NULL &&
              strstr(dump, "\"BACK\"") != NULL && strstr(dump, "\"MOVE\"") == NULL &&
              strstr(dump, "\"OPEN\"") == NULL,
          "touch: the actions are labelled, and Move and Open are left out");
    CHECK(!nk_ui_element_rect(ui, nk_ui_child_id(prompt_item(3u), 1u), &glyph),
          "touch: no key glyph is drawn");
    CHECK(nk_ui_element_rect(ui, prompt_item(3u), &menu) && menu.h >= NK_UI_CONTROL_HEIGHT &&
              menu.y + menu.h <= 400.0f,
          "touch: SETTINGS is a full-size button in the window");
    CHECK(nk_ui_element_rect(ui, prompt_item(4u), &back), "touch: BACK is laid out");
    nk_ui_focus(ui, 72u);
    draw_prompt_page(ui);
    tap_at_middle(ui, &menu);
    draw_prompt_page(ui);
    CHECK(nk_ui_wait_s(ui, 0.0, 0.05) == 0.0, "the tapped action is drawn at once");
    nk_ui_begin(ui, 0.0);
    CHECK(nk_ui_take_event(ui, NK_UI_EV_MENU, NULL), "a tap on SETTINGS is the next frame's Menu");
    (void)nk_ui_end(ui);
    nk_ui_begin(ui, 0.0);
    CHECK(!nk_ui_take_event(ui, NK_UI_EV_MENU, NULL), "once");
    (void)nk_ui_end(ui);
    for (frame = 0; frame < 2; ++frame) draw_prompt_page(ui);
    CHECK(!nk_ui_take_prompt_back(ui), "no Back was tapped");
    tap_at_middle(ui, &back);
    draw_prompt_page(ui);
    CHECK(nk_ui_take_prompt_back(ui) && !nk_ui_take_prompt_back(ui),
          "a tap on BACK is the host's Back, once");
    nk_ui_destroy(ui);
    return 1;
}

/* The ring eases from the control focus leaves to the one it reaches, over
   NK_UI_MOTION_FAST, and nk_ui says so while it moves, so the host keeps
   drawing. Reduce motion makes the move a cut. */
static int test_focus_ring_moves(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    NkUiConfig config;
    NkUiPrefs prefs;
    NkUi *ui;
    int start;
    int mid;
    int end;
    int frame;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create ring move context");
    set_test_window(ui);
    nk_ui_focus(ui, 81u);
    for (frame = 0; frame < 3; ++frame) start = draw_ring_pair(ui, 10.0 + frame * 0.01);
    CHECK(start != INT_MIN && !nk_ui_in_motion(ui), "the ring rests on the first button");
    nk_ui_focus(ui, 82u);
    (void)draw_ring_pair(ui, 10.03);
    mid = draw_ring_pair(ui, 10.03 + NK_UI_MOTION_FAST / 2.0);
    CHECK(mid > start && mid < start + 200 && nk_ui_in_motion(ui),
          "half way through, the ring is between the buttons and still moving");
    for (frame = 1; frame <= 3; ++frame) end = draw_ring_pair(ui, 10.03 + NK_UI_MOTION_FAST * 2.0 * frame);
    CHECK(end == start + 200 && !nk_ui_in_motion(ui), "then it rests on the second button");
    memset(&prefs, 0, sizeof(prefs));
    prefs.reduce_motion = true;
    nk_ui_set_prefs(ui, &prefs);
    nk_ui_focus(ui, 81u);
    (void)draw_ring_pair(ui, 11.0);
    CHECK(draw_ring_pair(ui, 11.0 + NK_UI_MOTION_FAST / 2.0) == start && !nk_ui_in_motion(ui),
          "Reduce motion: the ring is on the new control at once");
    nk_ui_destroy(ui);
    return 1;
}

/* A top bar whose second or third tab is active, drawn at `now_s`. Returns the
   left edge of the accent rule under the active tab, or INT_MIN. */
static int draw_tab_bar(NkUi *ui, double now_s, int active) {
    const NkUiTheme *theme = nk_ui_theme_n1();
    const NkUiSection sections[] = {
        {"Library", false, 0u, 0u}, {"Settings", active == 1, 0u, 0u},
        {"Controller", active == 2, 0u, 0u}
    };
    const NkUiDrawList *list;
    uint32_t i;
    nk_ui_begin(ui, now_s);
    (void)nk_ui_top_bar(ui, 95u, sections, 3u, NULL, false);
    list = nk_ui_end(ui);
    for (i = 0u; list != NULL && i < list->ncmd; ++i) {
        const NkUiDrawCmd *cmd = &list->cmd[i];
        if (cmd->kind == NK_UI_DRAW_RECTANGLE && cmd->rgba_pm == theme->accent_dark &&
            cmd->bounds[3] - cmd->bounds[1] == 3) {
            return cmd->bounds[0];
        }
    }
    return INT_MIN;
}

/* The accent rule slides to the newly active tab over NK_UI_MOTION_BASE
   instead of jumping; Reduce motion makes it a cut. */
static int test_tab_rule_slides(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    NkUiConfig config;
    NkUiPrefs prefs;
    NkUi *ui;
    int from = INT_MIN;
    int mid;
    int to = INT_MIN;
    int frame;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create tab rule context");
    set_test_window(ui);
    for (frame = 0; frame < 3; ++frame) to = draw_tab_bar(ui, 30.0 + frame * 0.01, 2);
    for (frame = 0; frame < 4; ++frame) from = draw_tab_bar(ui, 31.0 + frame * 0.5, 1);
    CHECK(from != INT_MIN && to != INT_MIN && to > from && !nk_ui_in_motion(ui),
          "the rule rests under the active tab");
    (void)draw_tab_bar(ui, 33.0, 2);
    mid = draw_tab_bar(ui, 33.0 + NK_UI_MOTION_BASE / 2.0, 2);
    CHECK(mid > from && mid < to && nk_ui_in_motion(ui),
          "half way through, the rule is between the two tabs");
    for (frame = 1; frame <= 3; ++frame) mid = draw_tab_bar(ui, 33.0 + NK_UI_MOTION_BASE * 2.0 * frame, 2);
    CHECK(mid == to && !nk_ui_in_motion(ui), "then it rests under the new tab");
    memset(&prefs, 0, sizeof(prefs));
    prefs.reduce_motion = true;
    nk_ui_set_prefs(ui, &prefs);
    (void)draw_tab_bar(ui, 35.0, 1);
    CHECK(draw_tab_bar(ui, 35.0 + NK_UI_MOTION_BASE / 2.0, 1) == from && !nk_ui_in_motion(ui),
          "Reduce motion: the rule is under the new tab at once");
    nk_ui_destroy(ui);
    return 1;
}

/* How long a host may wait before the next frame: until the waves' next 30 Hz
   step while they play (the launcher waited 50 ms, so they stepped at 20 Hz),
   one display frame while something moves, the idle wait otherwise and under
   Reduce motion, and never past the idle wait. */
static int test_wait_follows_motion(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    NkUiConfig config;
    NkUiPrefs prefs;
    NkUi *ui;
    int frame;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create wait context");
    set_test_window(ui);
    nk_ui_begin(ui, 40.0);
    CHECK(nk_ui_end(ui) != NULL, "draw a still frame");
    CHECK(nk_ui_wait_s(ui, 40.0, 0.05) == 0.05, "nothing moving: the idle wait");
    nk_ui_begin(ui, 40.01);
    CHECK(nk_ui_waves_element(ui, 9u, (NkUiRect){0.0f, 0.0f, 64.0f, 64.0f}, 0u), "queue the waves");
    CHECK(nk_ui_end(ui) != NULL, "draw a frame with the waves");
    CHECK(fabs(nk_ui_wait_s(ui, 40.01, 0.05) - (1201.0 / 30.0 - 40.01)) < 1e-9,
          "the waves: until their next 30 Hz step");
    CHECK(nk_ui_wait_s(ui, 40.01, 0.01) == 0.01, "never past the idle wait");
    CHECK(nk_ui_wait_s(ui, 40.2, 0.05) == 0.0, "a step already due: no wait");
    memset(&prefs, 0, sizeof(prefs));
    prefs.reduce_motion = true;
    nk_ui_set_prefs(ui, &prefs);
    CHECK(nk_ui_wait_s(ui, 40.01, 0.05) == 0.05, "Reduce motion holds the waves: the idle wait");
    prefs.reduce_motion = false;
    nk_ui_set_prefs(ui, &prefs);
    nk_ui_focus(ui, 81u);
    for (frame = 0; frame < 3; ++frame) (void)draw_ring_pair(ui, 41.0 + frame * 0.01);
    nk_ui_focus(ui, 82u);
    (void)draw_ring_pair(ui, 41.05);
    (void)draw_ring_pair(ui, 41.06);
    CHECK(nk_ui_in_motion(ui) && nk_ui_wait_s(ui, 41.06, 0.05) == NK_UI_MOTION_FRAME,
          "a ring still moving: one display frame");
    nk_ui_destroy(ui);
    return 1;
}

/* An art area, 160 x 90, holding `image` when there is one, drawn at `now_s`.
   Returns the alpha the picture is drawn at, -1 when no picture is drawn. */
static int draw_art_area(NkUi *ui, double now_s, const NkUiImage *image) {
    NkUiLayout area;
    const NkUiDrawList *list;
    uint32_t i;
    memset(&area, 0, sizeof(area));
    area.width = (NkUiSizing){NK_UI_SIZE_FIXED, 160.0f, 0.0f, 0.0f};
    area.height = (NkUiSizing){NK_UI_SIZE_FIXED, 90.0f, 0.0f, 0.0f};
    nk_ui_begin(ui, now_s);
    if (nk_ui_begin_container(ui, 97u, &area, NULL)) {
        if (image != NULL) {
            NkUiImageArt art;
            memset(&art, 0, sizeof(art));
            art.cover = image;
            (void)nk_ui_image_art(ui, 98u, (NkUiSizing){NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f},
                                  (NkUiSizing){NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f}, &art);
        } else {
            nk_ui_label(ui, 99u, "No picture", 0, 12);
        }
        nk_ui_end_container(ui);
    }
    list = nk_ui_end(ui);
    for (i = 0u; list != NULL && i < list->ncmd; ++i) {
        const NkUiDrawCmd *cmd = &list->cmd[i];
        if (cmd->kind == NK_UI_DRAW_CUSTOM && cmd->custom_kind == NK_UI_CUSTOM_IMAGE) {
            return (int)(cmd->rgba_pm & 0xFFu);
        }
    }
    return -1;
}

/* A disc's picture read after its card is on screen eases in over
   NK_UI_MOTION_SLOW; one that comes with its card is there at once, and so is
   any under Reduce motion. */
static int test_art_fades_in(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    static uint8_t pixels[2u * 2u * 4u] = {
        255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255
    };
    NkUiImage image = {2u, 2u, pixels, 1u};
    NkUiConfig config;
    NkUiPrefs prefs;
    NkUi *ui;
    int mid;
    int frame;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create art fade context");
    set_test_window(ui);
    CHECK(draw_art_area(ui, 50.0, &image) == 255 && !nk_ui_in_motion(ui),
          "a picture that comes with its card is there at once");
    for (frame = 0; frame < 2; ++frame) (void)draw_art_area(ui, 50.1 + frame * 0.01, NULL);
    CHECK(draw_art_area(ui, 50.2, &image) == 0 && nk_ui_in_motion(ui),
          "a picture read after its card was drawn starts transparent");
    mid = draw_art_area(ui, 50.2 + NK_UI_MOTION_SLOW / 2.0, &image);
    CHECK(mid > 0 && mid < 255 && nk_ui_in_motion(ui), "half way through, it is fading in");
    CHECK(draw_art_area(ui, 50.2 + NK_UI_MOTION_SLOW, &image) == 255, "then it is whole");
    (void)draw_art_area(ui, 50.8, &image);
    CHECK(!nk_ui_in_motion(ui), "and still");
    memset(&prefs, 0, sizeof(prefs));
    prefs.reduce_motion = true;
    nk_ui_set_prefs(ui, &prefs);
    (void)draw_art_area(ui, 51.0, NULL);
    CHECK(draw_art_area(ui, 51.1, &image) == 255 && !nk_ui_in_motion(ui),
          "Reduce motion: the picture is there at once");
    nk_ui_destroy(ui);
    return 1;
}

/* One frame with the launcher backdrop (nk_ui_backdrop) at `now_s`: `image` when not
   NULL, shown or fading out. Returns the alpha the picture is drawn at, -1 when it is
   not drawn; *scrim gets the scrim's alpha over it. */
static int draw_backdrop(NkUi *ui, double now_s, const NkUiImage *image, bool shown,
                         int *scrim) {
    const NkUiDrawList *list;
    int alpha = -1;
    uint32_t i;
    if (scrim != NULL) *scrim = -1;
    nk_ui_begin(ui, now_s);
    if (image != NULL) {
        (void)nk_ui_backdrop(ui, 96u, (NkUiRect){0.0f, 0.0f, 320.0f, 180.0f}, image, shown);
    }
    nk_ui_label(ui, 99u, "Library", 0, 12);
    list = nk_ui_end(ui);
    for (i = 0u; list != NULL && i < list->ncmd; ++i) {
        const NkUiDrawCmd *cmd = &list->cmd[i];
        if (cmd->element_id != 96u) continue;
        if (cmd->kind == NK_UI_DRAW_CUSTOM && cmd->custom_kind == NK_UI_CUSTOM_IMAGE) {
            alpha = (int)(cmd->rgba_pm & 0xFFu);
        } else if (cmd->kind == NK_UI_DRAW_RECTANGLE && scrim != NULL && cmd->count > 0u) {
            *scrim = (int)(list->v[list->idx[cmd->first]].rgba_pm & 0xFFu);
        }
    }
    return alpha;
}

/* The art rest's backdrop (#1299 launcher work): the focused card's picture behind the
   library fades in over NK_UI_MOTION_SLOW, fades out when no longer shown, and a new
   picture starts from nothing. Its scrim keeps a white picture, over waves at their
   limit, as dark as the waves' own limit at every point of the fade. Overlay hosts
   and Reduce motion leave it out. */
static int test_backdrop_rests_and_fades(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    static uint8_t pixels[2u * 2u * 4u] = {
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255
    };
    NkUiImage image = {2u, 2u, pixels, 7u};
    NkUiImage other = {2u, 2u, pixels, 8u};
    NkUiConfig config;
    NkUiPrefs prefs;
    NkUi *ui;
    int scrim = -1;
    int mid;
    float fade;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create backdrop context");
    set_test_window(ui);
    for (fade = 0.0f; fade <= 1.0f; fade += 0.01f) {
        float s = nk_ui_backdrop_scrim(fade);
        float worst = (0.22f * (1.0f - fade) + fade) * (1.0f - s) + (0x0B / 255.0f) * s;
        CHECK(s >= 0.0f && s <= 1.0f && worst <= 0.23f,
              "backdrop: the scrim keeps a white picture as dark as the waves' limit");
    }
    CHECK(nk_ui_backdrop_scrim(0.0f) == 0.0f && nk_ui_backdrop_scrim(1.0f) > 0.75f,
          "backdrop: no scrim before the picture shows, a strong one once it has");
    CHECK(draw_backdrop(ui, 60.0, NULL, false, NULL) == -1, "backdrop: none asked, none drawn");
    mid = draw_backdrop(ui, 60.1, &image, true, NULL);
    CHECK(mid >= 0 && mid < 64 && nk_ui_in_motion(ui),
          "backdrop: it starts faint, one display frame in, and the host keeps drawing");
    mid = draw_backdrop(ui, 60.1 + NK_UI_MOTION_SLOW / 2.0, &image, true, &scrim);
    CHECK(mid > 0 && mid < 255 && scrim > 0 && nk_ui_in_motion(ui),
          "backdrop: half way, the picture is fading in under its scrim");
    CHECK(draw_backdrop(ui, 60.1 + NK_UI_MOTION_SLOW, &image, true, &scrim) == 255 &&
              scrim == (int)(nk_ui_backdrop_scrim(1.0f) * 255.0f + 0.999f),
          "backdrop: then it is whole, under the full scrim");
    (void)draw_backdrop(ui, 60.7, &image, true, NULL);
    CHECK(!nk_ui_in_motion(ui), "backdrop: and still");
    CHECK(draw_backdrop(ui, 61.0, &other, true, NULL) < 64,
          "backdrop: a different picture starts from nothing");
    (void)draw_backdrop(ui, 61.0 + NK_UI_MOTION_SLOW, &other, true, NULL);
    CHECK(draw_backdrop(ui, 61.6, &other, false, NULL) > 0 && nk_ui_in_motion(ui),
          "backdrop: no longer shown, it starts to fade out");
    mid = draw_backdrop(ui, 61.6 + NK_UI_MOTION_SLOW / 2.0, &other, false, NULL);
    CHECK(mid > 0 && mid < 255 && nk_ui_in_motion(ui), "backdrop: half way out");
    CHECK(draw_backdrop(ui, 61.6 + NK_UI_MOTION_SLOW + 0.05, &other, false, NULL) == -1 &&
              !nk_ui_in_motion(ui),
          "backdrop: and then it is gone");
    (void)draw_backdrop(ui, 62.0, &image, true, NULL);
    (void)draw_backdrop(ui, 62.0 + NK_UI_MOTION_SLOW, &image, true, NULL);
    CHECK(draw_backdrop(ui, 62.6, NULL, false, NULL) == -1 &&
              draw_backdrop(ui, 62.7, &image, true, NULL) < 64,
          "backdrop: a frame without it ends it");
    memset(&prefs, 0, sizeof(prefs));
    prefs.reduce_motion = true;
    nk_ui_set_prefs(ui, &prefs);
    (void)draw_backdrop(ui, 63.0, &image, true, NULL);
    CHECK(draw_backdrop(ui, 64.0, &image, true, NULL) == -1 && !nk_ui_in_motion(ui),
          "backdrop: Reduce motion leaves it out");
    nk_ui_destroy(ui);
    config.host = NK_UI_HOST_OVERLAY;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create overlay context");
    set_test_window(ui);
    nk_ui_begin(ui, 1.0);
    CHECK(!nk_ui_backdrop(ui, 96u, (NkUiRect){0.0f, 0.0f, 10.0f, 10.0f}, &image, true),
          "backdrop: an overlay host refuses it");
    (void)nk_ui_end(ui);
    nk_ui_destroy(ui);
    return 1;
}

/* Clay's layout inspector draws its panel, listing the layout's elements by
   their id strings, while it is on, and the frame is still drawn. */
static int test_layout_debug(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    NkUiConfig config;
    NkUi *ui;
    char dump[65536];
    int frame;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create inspector context");
    set_test_window(ui);
    for (frame = 0; frame < 2; ++frame) (void)draw_button_pair(ui, 10u, 0u);
    (void)nk_ui_dump(ui, dump, sizeof(dump));
    CHECK(strstr(dump, "nk_ui.container") == NULL, "no inspector by default");
    nk_ui_set_layout_debug(ui, true);
    for (frame = 0; frame < 2; ++frame) (void)draw_button_pair(ui, 10u, 0u);
    CHECK(nk_ui_dump(ui, dump, sizeof(dump)) < sizeof(dump) &&
              strstr(dump, "nk_ui.container") != NULL,
          "the inspector lists the layout's elements");
    CHECK(nk_ui_layout_debug(ui), "the inspector reads on");
    /* Audit F-0ce70310: the panel's own close button (its x, 20 px square, 10 px in
       from the window's top right corner) turns it off, and the host can read that. */
    push_pointer(ui, 620.0f, 15.0f, false);
    (void)draw_button_pair(ui, 10u, 0u);
    push_pointer(ui, 620.0f, 15.0f, true);
    (void)draw_button_pair(ui, 10u, 0u);
    push_pointer(ui, 620.0f, 15.0f, false);
    (void)draw_button_pair(ui, 10u, 0u);
    CHECK(!nk_ui_layout_debug(ui), "the panel's close button turns the inspector off");
    nk_ui_set_layout_debug(ui, true);
    for (frame = 0; frame < 2; ++frame) (void)draw_button_pair(ui, 10u, 0u);
    nk_ui_set_layout_debug(ui, false);
    for (frame = 0; frame < 2; ++frame) (void)draw_button_pair(ui, 10u, 0u);
    (void)nk_ui_dump(ui, dump, sizeof(dump));
    CHECK(strstr(dump, "nk_ui.container") == NULL, "and is gone once turned off");
    nk_ui_destroy(ui);
    return 1;
}

/* Clay is told the pointer once a frame: a press reaches Clay as this frame's,
   which the inspector's clicks and Clay's own hover handlers rely on. */
static int test_pointer_state_once_a_frame(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    NkUiConfig config;
    NkUi *ui;
    Clay_PointerDataInteractionState state;
    int frame;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create pointer state context");
    set_test_window(ui);
    for (frame = 0; frame < 3; ++frame) (void)draw_button_pair(ui, 10u, 0u);
    push_pointer(ui, 10.0f, 10.0f, true);
    nk_ui_begin(ui, 37.0);
    state = Clay_GetPointerState().state;
    (void)nk_ui_end(ui);
    CHECK(state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME, "Clay sees the press as this frame's");
    nk_ui_begin(ui, 37.0);
    state = Clay_GetPointerState().state;
    (void)nk_ui_end(ui);
    CHECK(state == CLAY_POINTER_DATA_PRESSED, "and as held on the next");
    nk_ui_destroy(ui);
    return 1;
}

static int test_control_owns_horizontal(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    NkUiConfig config;
    NkUiViewport viewport = {1280, 800, 1.0f, 0, 100, 0.0f, 0.0f};
    NkUiNavDir taken = NK_UI_DIR_UP;
    bool took = false;
    NkUi *ui;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create horizontal-control context");
    nk_ui_set_viewport(ui, &viewport);
    nk_ui_focus(ui, 41u);
    horizontal_row_frame(ui, &taken, &took);
    horizontal_row_frame(ui, &taken, &took);
    CHECK(nk_ui_focused(ui) == 41u, "the segmented control has focus");

    push_nav(ui, NK_UI_DIR_RIGHT);
    horizontal_row_frame(ui, &taken, &took);
    CHECK(nk_ui_focused(ui) == 41u, "right on a control that owns it keeps the focus");
    CHECK(took && taken == NK_UI_DIR_RIGHT, "the screen takes the right press for the control");
    push_nav(ui, NK_UI_DIR_LEFT);
    horizontal_row_frame(ui, &taken, &took);
    CHECK(nk_ui_focused(ui) == 41u && took && taken == NK_UI_DIR_LEFT,
          "left stays with the control too");

    nk_ui_focus(ui, 43u);
    horizontal_row_frame(ui, &taken, &took);
    push_nav(ui, NK_UI_DIR_LEFT);
    horizontal_row_frame(ui, &taken, &took);
    CHECK(!took, "a plain button's left press is a focus move, not an event");
    CHECK(nk_ui_focused(ui) == 41u, "left from a plain button still moves focus");
    nk_ui_destroy(ui);
    return 1;
}

static int test_golden(const char *actual, size_t actual_size) {
    FILE *file = fopen("tests/fixtures/nk_ui/core_synthetic.golden", "rb");
    char expected[32768];
    size_t read_size;
    CHECK(file != NULL, "open checked-in core golden");
    read_size = fread(expected, 1u, sizeof(expected) - 1u, file);
    fclose(file);
    expected[read_size] = '\0';
    CHECK(read_size == actual_size, "core golden byte count matches all four scales");
    if (strcmp(actual, expected) != 0) {
        fprintf(stderr, "FAIL: core golden differs\n--- actual ---\n%s--- expected ---\n%s",
                actual, expected);
        return 0;
    }
    return 1;
}

/* A PNG signature and an IHDR chunk claiming w x h, 8-bit RGBA, and nothing
   else: enough for the header check, never enough to decode. */
static size_t png_header_only(uint8_t *out, uint32_t w, uint32_t h) {
    static const uint8_t head[16] = {0x89u, 'P', 'N', 'G', '\r', '\n', 0x1Au, '\n',
                                     0u, 0u, 0u, 13u, 'I', 'H', 'D', 'R'};
    memcpy(out, head, sizeof(head));
    out[16] = (uint8_t)(w >> 24); out[17] = (uint8_t)(w >> 16);
    out[18] = (uint8_t)(w >> 8); out[19] = (uint8_t)w;
    out[20] = (uint8_t)(h >> 24); out[21] = (uint8_t)(h >> 16);
    out[22] = (uint8_t)(h >> 8); out[23] = (uint8_t)h;
    out[24] = 8u; out[25] = 6u; out[26] = 0u; out[27] = 0u; out[28] = 0u;
    memset(out + 29, 0, 4u); /* CRC, not checked by the header read */
    return 33u;
}

/* PNG building for the decoder bounds: chunks with their CRC, and zlib streams
   (stored, or fixed-Huffman runs of one repeated byte) with their Adler-32. */
static uint32_t png_crc(const uint8_t *data, size_t length, uint32_t crc) {
    size_t i;
    int bit;
    for (i = 0; i < length; ++i) {
        crc ^= data[i];
        for (bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc;
}

static size_t png_put_be32(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)(value >> 24); out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8); out[3] = (uint8_t)value;
    return 4u;
}

static size_t png_chunk(uint8_t *out, const char *type, const uint8_t *data, uint32_t length) {
    uint32_t crc;
    png_put_be32(out, length);
    memcpy(out + 4, type, 4u);
    if (length != 0u) memcpy(out + 8, data, length);
    crc = png_crc(out + 4, (size_t)length + 4u, 0xFFFFFFFFu) ^ 0xFFFFFFFFu;
    png_put_be32(out + 8 + length, crc);
    return (size_t)length + 12u;
}

static size_t png_start(uint8_t *out, uint32_t w, uint32_t h, uint8_t depth, uint8_t color,
                        uint8_t interlace) {
    static const uint8_t signature[8] = {0x89u, 'P', 'N', 'G', '\r', '\n', 0x1Au, '\n'};
    uint8_t ihdr[13];
    memcpy(out, signature, sizeof(signature));
    png_put_be32(ihdr, w);
    png_put_be32(ihdr + 4, h);
    ihdr[8] = depth; ihdr[9] = color; ihdr[10] = 0u; ihdr[11] = 0u; ihdr[12] = interlace;
    return 8u + png_chunk(out + 8, "IHDR", ihdr, 13u);
}

static uint32_t png_adler(const uint8_t *data, size_t length) {
    uint32_t a = 1u;
    uint32_t b = 0u;
    size_t i;
    for (i = 0; i < length; ++i) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

/* A zlib stream holding raw in one stored block (raw under 64 KiB). */
static size_t png_zlib_stored(uint8_t *out, const uint8_t *raw, uint16_t length) {
    out[0] = 0x78u; out[1] = 0x01u;
    out[2] = 0x01u; /* final stored block */
    out[3] = (uint8_t)length; out[4] = (uint8_t)(length >> 8);
    out[5] = (uint8_t)~length; out[6] = (uint8_t)(~length >> 8);
    memcpy(out + 7, raw, length);
    return 7u + length + png_put_be32(out + 7 + length, png_adler(raw, length));
}

typedef struct {
    uint8_t *out;
    size_t bytes;
    unsigned bit;
} PngBits;

/* Huffman codes go most significant bit first into the LSB-first stream. */
static void png_bits_code(PngBits *bits, uint32_t code, unsigned length) {
    while (length-- > 0u) {
        if (bits->bit == 0u) bits->out[bits->bytes] = 0u;
        if ((code >> length) & 1u) bits->out[bits->bytes] |= (uint8_t)(1u << bits->bit);
        if (++bits->bit == 8u) {
            bits->bit = 0u;
            ++bits->bytes;
        }
    }
}

/* A zlib stream that inflates to 1 + 258 * runs zero bytes: a literal zero,
   then `runs` fixed-Huffman copies of length 258 at distance 1. Each copy is
   13 bits, so the stream grows about 1 byte for every 160 it inflates to. */
static size_t png_zlib_zero_runs(uint8_t *out, uint32_t runs) {
    PngBits bits;
    uint64_t inflated = 1u + 258u * (uint64_t)runs;
    uint32_t i;
    out[0] = 0x78u; out[1] = 0x01u;
    bits.out = out + 2;
    bits.bytes = 0u;
    bits.bit = 0u;
    png_bits_code(&bits, 1u, 1u);       /* BFINAL */
    png_bits_code(&bits, 1u, 1u);       /* BTYPE 01, fixed Huffman, LSB first */
    png_bits_code(&bits, 0u, 1u);
    png_bits_code(&bits, 0x30u, 8u);    /* literal 0 */
    for (i = 0; i < runs; ++i) {
        png_bits_code(&bits, 0xC5u, 8u); /* length code 285: 258 bytes */
        png_bits_code(&bits, 0u, 5u);    /* distance code 0: distance 1 */
    }
    png_bits_code(&bits, 0u, 7u);       /* end of block */
    if (bits.bit != 0u) ++bits.bytes;
    /* Adler-32 of `inflated` zero bytes: a stays 1, b adds it once a byte. */
    return 2u + bits.bytes +
           png_put_be32(out + 2 + bits.bytes, (uint32_t)((inflated % 65521u) << 16) | 1u);
}

/* The decoder's memory follows the PNG's own size, not its inflated data:
   image data that inflates far past what the header's image needs is refused
   before stb_image grows its buffer to match, a zero-length first IDAT never
   reaches stb_image (which copies into a NULL buffer for it), and genuine
   small and interlaced images still decode. */
static int test_image_decode_bounds(void) {
    enum { RUNS = 65536 };
    NkUiImageLimits limits = {1024u * 1024u, 256u, 256u};
    NkUiImage image;
    uint8_t *png = (uint8_t *)malloc(256u * 1024u);
    uint8_t *zlib = (uint8_t *)malloc(256u * 1024u);
    uint8_t raw[16];
    size_t zlib_size;
    size_t size;
    CHECK(png != NULL && zlib != NULL, "PNG build buffers");

    /* One opaque red RGBA pixel: filter byte 0, then the pixel. */
    raw[0] = 0u; raw[1] = 255u; raw[2] = 0u; raw[3] = 0u; raw[4] = 255u;
    zlib_size = png_zlib_stored(zlib, raw, 5u);
    size = png_start(png, 1u, 1u, 8u, 6u, 0u);
    size += png_chunk(png + size, "IDAT", zlib, (uint32_t)zlib_size);
    size += png_chunk(png + size, "IEND", NULL, 0u);
    CHECK(nk_ui_image_decode_png(png, size, &limits, &image) == NK_UI_IMAGE_OK &&
              image.width == 1u && image.height == 1u &&
              memcmp(image.rgba_pm, "\xFF\x00\x00\xFF", 4u) == 0,
          "a built 1 x 1 PNG decodes");
    nk_ui_image_free(&image);

    /* An 8 x 1 one-bit grey image, Adam7: four passes of one row, 8 bytes of
       image data where the plain image needs 2. */
    {
        static const uint8_t passes[8] = {0u, 0x80u, 0u, 0x80u, 0u, 0xC0u, 0u, 0xF0u};
        zlib_size = png_zlib_stored(zlib, passes, 8u);
        size = png_start(png, 8u, 1u, 1u, 0u, 1u);
        size += png_chunk(png + size, "IDAT", zlib, (uint32_t)zlib_size);
        size += png_chunk(png + size, "IEND", NULL, 0u);
        CHECK(nk_ui_image_decode_png(png, size, &limits, &image) == NK_UI_IMAGE_OK &&
                  image.width == 8u && image.height == 1u && image.rgba_pm[0] == 255u &&
                  image.rgba_pm[4] == 255u, "an interlaced one-bit image decodes");
        nk_ui_image_free(&image);
    }

    /* A 1 x 1 image whose data inflates to about 17 MB. */
    zlib_size = png_zlib_zero_runs(zlib, RUNS);
    CHECK(zlib_size < 200u * 1024u, "the inflate bomb stream is small");
    size = png_start(png, 1u, 1u, 8u, 6u, 0u);
    size += png_chunk(png + size, "IDAT", zlib, (uint32_t)zlib_size);
    size += png_chunk(png + size, "IEND", NULL, 0u);
    CHECK(nk_ui_image_decode_png(png, size, &limits, &image) == NK_UI_IMAGE_DECODE_FAILED &&
              image.rgba_pm == NULL,
          "image data inflating far past a 1 x 1 image is refused");

    /* A zero-length IDAT before the data. */
    zlib_size = png_zlib_stored(zlib, raw, 5u);
    size = png_start(png, 1u, 1u, 8u, 6u, 0u);
    size += png_chunk(png + size, "IDAT", NULL, 0u);
    size += png_chunk(png + size, "IDAT", zlib, (uint32_t)zlib_size);
    size += png_chunk(png + size, "IEND", NULL, 0u);
    CHECK(nk_ui_image_decode_png(png, size, &limits, &image) == NK_UI_IMAGE_DECODE_FAILED &&
              image.rgba_pm == NULL, "a zero-length first IDAT is refused before stb_image");
    free(zlib);
    free(png);
    return 1;
}

/* A destination pixel averaging more source pixels than a 32-bit channel sum
   holds (16.8 M of value 255): 4105 x 4105 opaque white drawn at 1 x 1 is
   still opaque white. A 32-bit sum wrapped and gave a near-black pixel. */
static int test_image_resample_wide_average(void) {
    enum { SIDE = 4105 };
    NkUiImage source;
    uint8_t one[4] = {0u, 0u, 0u, 0u};
    size_t bytes = (size_t)SIDE * SIDE * 4u;
    memset(&source, 0, sizeof(source));
    source.width = SIDE;
    source.height = SIDE;
    source.rgba_pm = (uint8_t *)malloc(bytes);
    source.serial = 1u;
    CHECK(source.rgba_pm != NULL, "allocate the 4105 x 4105 source");
    memset(source.rgba_pm, 0xFF, bytes);
    CHECK(nk_ui_image_resample(&source, 1u, 1u, one), "resample to one pixel");
    free(source.rgba_pm);
    CHECK(one[0] == 255u && one[1] == 255u && one[2] == 255u && one[3] == 255u,
          "16.85 M white pixels average to white");
    return 1;
}

/* The decoder: limits are applied before decoding, failures own nothing, and
   pixels come back premultiplied. */
static int test_image_decode(void) {
    const NkUiEmbeddedAsset *glyph = nk_ui_embedded_asset("input-prompts/xbox_button_a.png");
    NkUiImageLimits limits = {64u * 1024u, 256u, 256u};
    NkUiImage image;
    NkUiImage second;
    uint8_t buffer[64];
    uint64_t first_serial;
    size_t i;
    CHECK(glyph != NULL && glyph->size > 8u, "the embedded Xbox A glyph is present");
    CHECK(nk_ui_embedded_asset("input-prompts/missing.png") == NULL &&
          nk_ui_embedded_asset(NULL) == NULL, "an unknown asset name finds nothing");
    CHECK(nk_ui_image_decode_png(glyph->bytes, glyph->size, &limits, &image) == NK_UI_IMAGE_OK,
          "embedded glyph decodes");
    CHECK(image.width == 64u && image.height == 64u && image.rgba_pm != NULL && image.serial != 0u,
          "the Kenney Default glyphs are 64 x 64");
    for (i = 0; i < (size_t)image.width * image.height; ++i) {
        const uint8_t *p = image.rgba_pm + i * 4u;
        CHECK(p[0] <= p[3] && p[1] <= p[3] && p[2] <= p[3], "pixels are premultiplied");
    }
    first_serial = image.serial;
    CHECK(nk_ui_image_decode_png(glyph->bytes, glyph->size, &limits, &second) == NK_UI_IMAGE_OK &&
          second.serial != first_serial, "every decode gets its own serial");
    {
        /* Caller-filled pixels (the VRAM viewer, #314) draw from the same serial space. */
        uint64_t own = nk_ui_image_new_serial();
        uint64_t again = nk_ui_image_new_serial();
        CHECK(own != 0u && own != first_serial && own != second.serial && again != own,
              "a caller-owned image gets a serial no decode or earlier call has");
    }
    nk_ui_image_free(&second);
    nk_ui_image_free(&image);
    CHECK(image.rgba_pm == NULL && image.width == 0u && image.serial == 0u, "free zeroes the image");
    nk_ui_image_free(NULL);

    CHECK(nk_ui_image_decode_png(NULL, 10u, &limits, &image) == NK_UI_IMAGE_EMPTY &&
          nk_ui_image_decode_png(glyph->bytes, 0u, &limits, &image) == NK_UI_IMAGE_EMPTY &&
          nk_ui_image_decode_png(glyph->bytes, glyph->size, NULL, &image) == NK_UI_IMAGE_EMPTY &&
          nk_ui_image_decode_png(glyph->bytes, glyph->size, &limits, NULL) == NK_UI_IMAGE_EMPTY,
          "missing input is EMPTY");
    memcpy(buffer, "GIF89a and some more bytes", 27u);
    CHECK(nk_ui_image_decode_png(buffer, 27u, &limits, &image) == NK_UI_IMAGE_NOT_PNG &&
          image.rgba_pm == NULL, "only PNG is accepted");
    limits.max_bytes = glyph->size - 1u;
    CHECK(nk_ui_image_decode_png(glyph->bytes, glyph->size, &limits, &image) == NK_UI_IMAGE_TOO_LARGE,
          "encoded size over the limit");
    limits.max_bytes = 64u * 1024u;
    limits.max_width = 63u;
    CHECK(nk_ui_image_decode_png(glyph->bytes, glyph->size, &limits, &image) == NK_UI_IMAGE_TOO_LARGE &&
          image.rgba_pm == NULL, "width over the limit, refused before decoding");
    limits.max_width = 256u;
    limits.max_height = 63u;
    CHECK(nk_ui_image_decode_png(glyph->bytes, glyph->size, &limits, &image) == NK_UI_IMAGE_TOO_LARGE,
          "height over the limit");
    limits.max_height = 256u;
    /* A header claiming 5000 x 5000 never reaches the pixel decoder. */
    limits.max_width = 4096u;
    limits.max_height = 4096u;
    CHECK(nk_ui_image_decode_png(buffer, png_header_only(buffer, 5000u, 5000u), &limits, &image) ==
              NK_UI_IMAGE_TOO_LARGE, "a huge header is refused by the limits");
    CHECK(nk_ui_image_decode_png(buffer, png_header_only(buffer, 16u, 16u), &limits, &image) ==
              NK_UI_IMAGE_DECODE_FAILED && image.rgba_pm == NULL,
          "a header without image data fails to decode");
    CHECK(nk_ui_image_decode_png(glyph->bytes, glyph->size / 2u, &limits, &image) ==
              NK_UI_IMAGE_DECODE_FAILED, "a truncated PNG fails to decode");
    CHECK(strcmp(nk_ui_image_status_name(NK_UI_IMAGE_TOO_LARGE), "TOO_LARGE") == 0,
          "status names");
    {
        /* Resampling averages premultiplied pixels: a 2 x 1 image of opaque
           white and transparent black becomes one half-covered white pixel. */
        uint8_t two[8] = {255u, 255u, 255u, 255u, 0u, 0u, 0u, 0u};
        uint8_t one[4];
        uint8_t up[16];
        NkUiImage source;
        source.width = 2u;
        source.height = 1u;
        source.rgba_pm = two;
        source.serial = 1u;
        CHECK(nk_ui_image_resample(&source, 1u, 1u, one) &&
              one[0] == 128u && one[1] == 128u && one[2] == 128u && one[3] == 128u,
              "area average of premultiplied pixels");
        CHECK(nk_ui_image_resample(&source, 4u, 1u, up) && up[0] == 255u && up[4] == 255u &&
              up[8] == 0u && up[15] == 0u, "growing repeats source pixels");
        CHECK(!nk_ui_image_resample(&source, 0u, 1u, one) &&
              !nk_ui_image_resample(NULL, 1u, 1u, one) &&
              !nk_ui_image_resample(&source, 1u, 1u, NULL), "bad arguments are refused");
    }
    return 1;
}

/* An image command: tinted, dumped with the image's size, never drawn for an
   empty box, refused for a missing image. */
static int test_image_command(void) {
    const NkUiEmbeddedAsset *glyph = nk_ui_embedded_asset("input-prompts/kbd_enter.png");
    NkUiImageLimits limits = {64u * 1024u, 256u, 256u};
    NkUiImage image;
    NkUiImage empty;
    NkUiDrawBuffer buffer;
    NkUiPixelRect box = {10, 20, 34, 44};
    NkUiPixelRect flat = {10, 20, 10, 44};
    char dump[256];
    memset(&buffer, 0, sizeof(buffer));
    memset(&empty, 0, sizeof(empty));
    CHECK(glyph != NULL && nk_ui_image_decode_png(glyph->bytes, glyph->size, &limits, &image) ==
              NK_UI_IMAGE_OK, "decode the Enter keycap");
    CHECK(nk_ui_draw_add_image(&buffer, &image, box, 0x80808080u, 7u), "image command");
    CHECK(nk_ui_draw_add_image(&buffer, &image, flat, 0xFFFFFFFFu, 8u) && buffer.list.ncmd == 1u,
          "an empty box draws nothing");
    CHECK(!nk_ui_draw_add_image(&buffer, NULL, box, 0xFFFFFFFFu, 9u) &&
          !nk_ui_draw_add_image(&buffer, &empty, box, 0xFFFFFFFFu, 9u), "no image, no command");
    CHECK(buffer.list.cmd[0].kind == NK_UI_DRAW_CUSTOM &&
          buffer.list.cmd[0].custom_kind == NK_UI_CUSTOM_IMAGE &&
          buffer.list.cmd[0].custom == &image, "custom image command");
    (void)nk_ui_dump_draw_list(&buffer.list, dump, sizeof(dump));
    CHECK(strcmp(dump, "00000007 IMAGE 10 20 34 44 40404080 \"64x64\"\n") == 0,
          "dump: bounds, premultiplied tint and the image size");
    nk_ui_draw_buffer_destroy(&buffer);
    nk_ui_image_free(&image);
    return 1;
}

/* The app icon: a transparent rounded corner, the ink tile, a blue block and the
   paper bar, row padding left alone, and refusals for sizes it does not draw. */
static int test_app_icon(void) {
    enum { SIZE = 64, STRIDE = SIZE * 4 + 12 };
    static unsigned char rgba[SIZE * STRIDE];
    memset(rgba, 0xAB, sizeof(rgba));
    CHECK(nk_ui_app_icon_rgba(SIZE, rgba, STRIDE), "draw the 64 px icon");
#define ICON_PIXEL(x, y) (rgba + (size_t)(y) * STRIDE + (size_t)(x) * 4u)
    CHECK(ICON_PIXEL(0, 0)[3] == 0u, "the rounded corner stays transparent");
    CHECK(memcmp(ICON_PIXEL(32, 3), "\x10\x15\x1C\xFF", 4) == 0, "ink tile above the mark");
    CHECK(memcmp(ICON_PIXEL(15, 50), "\x6B\x9F\xF0\xFF", 4) == 0, "blue block, bottom left");
    CHECK(memcmp(ICON_PIXEL(47, 32), "\xF5\xF7\xFA\xFF", 4) == 0, "paper bar, right");
    CHECK(ICON_PIXEL(SIZE, 10)[0] == 0xABu && ICON_PIXEL(SIZE, 10)[11] == 0xABu,
          "the row padding is not written");
#undef ICON_PIXEL
    CHECK(!nk_ui_app_icon_rgba(NK_UI_APP_ICON_MIN - 1, rgba, STRIDE) &&
          !nk_ui_app_icon_rgba(NK_UI_APP_ICON_MAX + 1, rgba, sizeof(rgba)) &&
          !nk_ui_app_icon_rgba(SIZE, NULL, STRIDE) &&
          !nk_ui_app_icon_rgba(SIZE, rgba, SIZE * 4 - 1), "sizes and strides it cannot draw");
    return 1;
}

/* A host's display rule for the test: the synthetic profile folder becomes "~",
   which never grows the text. */
#define SYNTH_HOME "C:\\path\\SYNTHETIC-USER"
static size_t synth_display_text(const char *in, char *out, size_t out_size) {
    size_t home_len = strlen(SYNTH_HOME);
    size_t used = 0u;
    while (*in != '\0' && used + 1u < out_size) {
        if (strncmp(in, SYNTH_HOME, home_len) == 0) {
            out[used++] = '~';
            in += home_len;
        } else {
            out[used++] = *in++;
        }
    }
    out[used] = '\0';
    return used;
}

/* Every string reaches the frame through the host's display rules: copied
   text, labels, styled text, and a clamp, which measures the rewritten text
   so that its cut never keeps part of the profile folder. */
static int test_display_text_rules(void) {
    static const char path[] = SYNTH_HOME "\\Games\\PSP\\disc.iso";
    NkUiTextBackend fixed = nk_ui_text_fixed();
    NkUiTextStyle style = nk_ui_type(NK_UI_TYPE_DATA, 0xFFFFFFFFu);
    NkUiConfig config;
    NkUiLayout layout;
    NkUi *ui;
    char dump[4096];
    const char *shown;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    config.display_text = synth_display_text;
    memset(&layout, 0, sizeof(layout));
    layout.width = (NkUiSizing){NK_UI_SIZE_FIXED, 400.0f, 0.0f, 0.0f};
    layout.height = (NkUiSizing){NK_UI_SIZE_FIXED, 200.0f, 0.0f, 0.0f};
    layout.direction = NK_UI_LAYOUT_COLUMN;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create context with display rules");
    nk_ui_begin(ui, 0.0);
    CHECK(nk_ui_begin_container(ui, 1u, &layout, NULL), "open display rules container");
    shown = nk_ui_frame_text(ui, path);
    CHECK(strcmp(shown, "~\\Games\\PSP\\disc.iso") == 0, "frame text shows the profile as ~");
    shown = nk_ui_text_clamp(ui, path, &style, 2000.0f, 1);
    CHECK(strcmp(shown, "~\\Games\\PSP\\disc.iso") == 0, "a clamp that fits returns the rewritten text");
    shown = nk_ui_text_clamp(ui, "Copied from " SYNTH_HOME "\\Games\\PSP\\a-long-file-name.iso",
                             &style, 60.0f, 1);
    CHECK(strstr(shown, "SYNTHETIC") == NULL && strstr(shown, "C:\\path") == NULL,
          "a clamp cuts the rewritten text, never a part of the profile folder");
    nk_ui_label(ui, 2u, path, 0, 16);
    nk_ui_text(ui, 3u, "Log file: " SYNTH_HOME "\\logs\\player.txt", &style);
    nk_ui_end_container(ui);
    CHECK(nk_ui_end(ui) != NULL, "complete display rules frame");
    CHECK(nk_ui_dump(ui, dump, sizeof(dump)) < sizeof(dump), "display rules dump fits");
    CHECK(strstr(dump, "SYNTHETIC-USER") == NULL, "no drawn run carries the profile folder");
    CHECK(strstr(dump, "~\\\\Games") != NULL || strstr(dump, "~\\Games") != NULL,
          "the label run shows the rewritten path");
    nk_ui_destroy(ui);
    return 1;
}

/* A list joined by middle dots, line by line (nk_ui_list_line): whole items on
   each line with the dot kept at its end, every space a no-break space, the
   items a last line leaves out counted and never cut, and one item too wide for
   the line shortened. The fixed backend measures 6.5 px a character at 13 px. */
static int test_list_lines(void) {
#define DOT "\xC2\xB7"
#define NB "\xC2\xA0"
    static const char list[] = "aa " DOT " bb cc " DOT " dd " DOT " ee";
    NkUiTextBackend fixed = nk_ui_text_fixed();
    NkUiTextStyle style = nk_ui_type(NK_UI_TYPE_CAPTION, 0xFFFFFFFFu);
    NkUiConfig config;
    NkUi *ui;
    const char *rest;
    const char *line;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_OVERLAY;
    config.text = &fixed;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create context for list lines");
    nk_ui_begin(ui, 0.0);
    CHECK(style.size_px == 13 && nk_ui_text_width(ui, "aaaa", &style) == 26.0f,
          "the fixed backend measures a caption 6.5 px a character");
    /* 20 characters, 130 px: one line, the run it is drawn as. */
    rest = list;
    line = nk_ui_list_line(ui, &rest, &style, 130.0f, false);
    CHECK(strcmp(line, "aa" NB DOT NB "bb" NB "cc" NB DOT NB "dd" NB DOT NB "ee") == 0 && rest[0] == '\0',
          "a list that fits is one run with no-break spaces");
    CHECK(nk_ui_list_line_count(ui, list, &style, 130.0f) == 1, "a list that fits counts one line");
    rest = list;
    line = nk_ui_list_line(ui, &rest, &style, 130.0f, true);
    CHECK(strcmp(line, "aa" NB DOT NB "bb" NB "cc" NB DOT NB "dd" NB DOT NB "ee") == 0 && rest[0] == '\0',
          "a last line that holds the whole list counts nothing");
    /* At 80 px: "aa . bb cc ." is 78 px, and a third item would make 110.5. */
    rest = list;
    line = nk_ui_list_line(ui, &rest, &style, 80.0f, false);
    CHECK(strcmp(line, "aa" NB DOT NB "bb" NB "cc" NB DOT) == 0 && strcmp(rest, "dd " DOT " ee") == 0,
          "a line takes the whole items that fit and keeps the dot after the last");
    line = nk_ui_list_line(ui, &rest, &style, 80.0f, false);
    CHECK(strcmp(line, "dd" NB DOT NB "ee") == 0 && rest[0] == '\0', "the next line takes the rest");
    CHECK(nk_ui_list_line_count(ui, list, &style, 80.0f) == 2, "the list counts two lines at 80 px");
    /* A last line gives up its own items until the count fits: "aa . and 3 more" is 97.5 px. */
    rest = list;
    line = nk_ui_list_line(ui, &rest, &style, 100.0f, true);
    CHECK(strcmp(line, "aa" NB DOT NB "and" NB "3" NB "more") == 0 && rest[0] == '\0',
          "a last line with items left over counts every item it leaves out");
    rest = list;
    line = nk_ui_list_line(ui, &rest, &style, 80.0f, true);
    CHECK(strcmp(line, "and" NB "4" NB "more") == 0 && rest[0] == '\0',
          "a last line too narrow for an item and the count keeps the count");
    /* One item wider than the line ("aa ." is 26 px) is shortened; the list goes on after it. */
    rest = list;
    line = nk_ui_list_line(ui, &rest, &style, 20.0f, false);
    CHECK(strstr(line, "\xE2\x80\xA6") != NULL && strstr(line, "bb") == NULL &&
              strcmp(rest, "bb cc " DOT " dd " DOT " ee") == 0,
          "an item wider than the line ends in an ellipsis and the next line starts at the next item");
    CHECK(nk_ui_list_line_count(ui, list, &style, 20.0f) == 4, "four items too wide count four lines");
    rest = "";
    CHECK(nk_ui_list_line(ui, &rest, &style, 80.0f, true)[0] == '\0' && rest[0] == '\0' &&
              nk_ui_list_line_count(ui, "", &style, 80.0f) == 0,
          "an empty list has no line");
    CHECK(nk_ui_end(ui) != NULL, "complete the list lines frame");
    nk_ui_destroy(ui);
#undef DOT
#undef NB
    return 1;
}

static bool clip_is(const NkUiDrawList *list, uint32_t index, int x0, int y0, int x1, int y1) {
    const int *clip = list->cmd[index].clip;
    return clip[0] == x0 && clip[1] == y0 && clip[2] == x1 && clip[3] == y1;
}

/* Each command carries where it may draw: its bounds inside every scissor open
   around it, nested, so a backend clips by the command alone (the Vulkan
   backend's contract) and agrees with the SDL_Renderer backend's stack. */
static int test_draw_clips_nest(void) {
    NkUiDrawBuffer buffer;
    const NkUiDrawList *list;
    /* A convex shape's bounds hold every pixel it touches, its 0.75 px fringe too, so
       a backend clipping each command to them trims none of its ink. */
    {
        static const float triangle[6] = {10.5f, 10.5f, 20.5f, 10.5f, 15.5f, 20.5f};
        const NkUiDrawCmd *shape;
        memset(&buffer, 0, sizeof(buffer));
        CHECK(nk_ui_draw_add_convex(&buffer, triangle, 3u, 0xFFFFFFFFu, 9u), "add a triangle");
        shape = &buffer.list.cmd[buffer.list.ncmd - 1u];
        CHECK(shape->bounds[0] <= 9 && shape->bounds[1] <= 10 && shape->bounds[2] >= 22 &&
                  shape->bounds[3] >= 22 && shape->bounds[0] >= 8 && shape->bounds[1] >= 9 &&
                  shape->bounds[2] <= 23 && shape->bounds[3] <= 23,
              "a convex shape's bounds cover its fringe, and no more than a pixel past it");
        nk_ui_draw_buffer_destroy(&buffer);
    }
    memset(&buffer, 0, sizeof(buffer));
    CHECK(nk_ui_draw_add_rect(&buffer, (NkUiRect){0.0f, 0.0f, 300.0f, 300.0f}, 1.0f, 0.0f,
                              0xFFFFFFFFu, 1u), "add an unclipped rect");
    CHECK(nk_ui_draw_add_scissor(&buffer, true, (NkUiPixelRect){0, 0, 100, 100}, 2u) &&
              nk_ui_draw_add_scissor(&buffer, true, (NkUiPixelRect){50, 50, 200, 200}, 3u),
          "open two nested scissors");
    CHECK(nk_ui_draw_add_rect(&buffer, (NkUiRect){80.0f, 80.0f, 70.0f, 70.0f}, 1.0f, 0.0f,
                              0xFFFFFFFFu, 4u) &&
              nk_ui_draw_add_rect(&buffer, (NkUiRect){150.0f, 150.0f, 20.0f, 20.0f}, 1.0f, 0.0f,
                                  0xFFFFFFFFu, 5u) &&
              nk_ui_draw_add_scissor(&buffer, false, (NkUiPixelRect){50, 50, 200, 200}, 3u) &&
              nk_ui_draw_add_rect(&buffer, (NkUiRect){80.0f, 80.0f, 70.0f, 70.0f}, 1.0f, 0.0f,
                                  0xFFFFFFFFu, 6u) &&
              nk_ui_draw_add_scissor(&buffer, false, (NkUiPixelRect){0, 0, 100, 100}, 2u) &&
              nk_ui_draw_add_rect(&buffer, (NkUiRect){80.0f, 80.0f, 70.0f, 70.0f}, 1.0f, 0.0f,
                                  0xFFFFFFFFu, 7u),
          "draw inside, across and after the scissors");
    list = &buffer.list;
    CHECK(list->ncmd == 9u, "nine commands");
    CHECK(clip_is(list, 0u, 0, 0, 300, 300), "a command outside every scissor keeps its bounds");
    CHECK(clip_is(list, 1u, 0, 0, 100, 100) && clip_is(list, 2u, 50, 50, 100, 100),
          "a nested scissor is narrowed by the one around it");
    CHECK(clip_is(list, 3u, 80, 80, 100, 100), "a rect is clipped by both");
    CHECK(list->cmd[4].clip[2] <= list->cmd[4].clip[0], "a rect they hide is empty");
    CHECK(clip_is(list, 5u, 50, 50, 100, 100) && clip_is(list, 6u, 80, 80, 100, 100),
          "closing the inner scissor leaves the outer one");
    CHECK(clip_is(list, 8u, 80, 80, 150, 150), "closing both leaves the bounds");
    CHECK(!nk_ui_draw_add_scissor(&buffer, false, (NkUiPixelRect){0, 0, 1, 1}, 9u),
          "a scissor end with none open is refused");
    nk_ui_draw_buffer_reset(&buffer);
    CHECK(nk_ui_draw_add_rect(&buffer, (NkUiRect){0.0f, 0.0f, 10.0f, 10.0f}, 1.0f, 0.0f,
                              0xFFFFFFFFu, 1u) && clip_is(&buffer.list, 0u, 0, 0, 10, 10),
          "a reset buffer has no scissor open");
    nk_ui_draw_buffer_destroy(&buffer);
    return 1;
}

/* Text runs live one frame: a reset keeps their storage for the next frame, and a
   reused run carries only what its new command says (its text, a longer one
   included, and no centring left from the run before). */
static int test_draw_text_runs_reused(void) {
    static const NkUiPixelRect box = {0, 0, 100, 20};
    static const char longer[] = "a much longer label than the one before it";
    NkUiDrawBuffer buffer;
    const NkUiTextRun *first;
    const NkUiTextRun *second;
    memset(&buffer, 0, sizeof(buffer));
    CHECK(nk_ui_draw_add_text_centered(&buffer, box, "ok", 2u, 0, 12, 0xFFFFFFFFu, 1u) &&
              nk_ui_draw_add_text(&buffer, box, "two", 3u, 1, 14, 0xFFFFFFFFu, 2u),
          "two text runs");
    first = buffer.text_runs[0];
    CHECK(first->center && strcmp(first->text, "ok") == 0, "the first run is centred");
    nk_ui_draw_buffer_reset(&buffer);
    CHECK(buffer.text_run_count == 0u && buffer.list.ncmd == 0u, "a reset empties the frame");
    CHECK(nk_ui_draw_add_text_spaced(&buffer, box, longer, sizeof(longer) - 1u, 2, 16, 1.5f,
                                     0xFFFFFFFFu, 3u),
          "a longer text after the reset");
    second = (const NkUiTextRun *)buffer.list.cmd[0].custom;
    CHECK(second == buffer.text_runs[0] && buffer.text_run_count == 1u,
          "the command points at the frame's run");
    CHECK(second->length == sizeof(longer) - 1u && strcmp(second->text, longer) == 0 &&
              second->text_capacity >= sizeof(longer),
          "the reused run holds the whole longer text");
    CHECK(!second->center && second->font == 2 && second->size_px == 16 &&
              second->letter_spacing == 1.5f,
          "the reused run keeps nothing from the run before");
    CHECK(nk_ui_draw_add_text(&buffer, box, NULL, 0u, 0, 12, 0xFFFFFFFFu, 4u) &&
              buffer.text_runs[1]->length == 0u && buffer.text_runs[1]->text[0] == '\0',
          "an empty text reuses the second run");
    nk_ui_draw_buffer_destroy(&buffer);
    CHECK(buffer.text_runs == NULL && buffer.text_run_pool == 0u, "destroy frees every run");
    return 1;
}

/* Every Clay error type has a name, and only the ones that leave a whole
   layout keep their frame. A type Clay adds later is fatal until it is named. */
static int test_layout_error_kinds(void) {
    static const struct {
        int type;
        const char *kind;
        bool fatal;
    } expected[] = {
        {CLAY_ERROR_TYPE_TEXT_MEASUREMENT_FUNCTION_NOT_PROVIDED,
         "TEXT_MEASUREMENT_FUNCTION_NOT_PROVIDED", true},
        {CLAY_ERROR_TYPE_ARENA_CAPACITY_EXCEEDED, "ARENA_CAPACITY_EXCEEDED", true},
        {CLAY_ERROR_TYPE_ELEMENTS_CAPACITY_EXCEEDED, "ELEMENTS_CAPACITY_EXCEEDED", true},
        {CLAY_ERROR_TYPE_TEXT_MEASUREMENT_CAPACITY_EXCEEDED,
         "TEXT_MEASUREMENT_CAPACITY_EXCEEDED", true},
        {CLAY_ERROR_TYPE_DUPLICATE_ID, "DUPLICATE_ID", false},
        {CLAY_ERROR_TYPE_FLOATING_CONTAINER_PARENT_NOT_FOUND,
         "FLOATING_CONTAINER_PARENT_NOT_FOUND", false},
        {CLAY_ERROR_TYPE_PERCENTAGE_OVER_1, "PERCENTAGE_OVER_1", false},
        {CLAY_ERROR_TYPE_INTERNAL_ERROR, "INTERNAL_ERROR", true},
        {CLAY_ERROR_TYPE_UNBALANCED_OPEN_CLOSE, "UNBALANCED_OPEN_CLOSE", true},
        {CLAY_ERROR_TYPE_HASH_MAP_CAPACITY_EXCEEDED, "HASH_MAP_CAPACITY_EXCEEDED", true},
        {CLAY_ERROR_TYPE_HASH_MAP_CAPACITY_EXCEEDED + 1, "UNKNOWN", true}
    };
    size_t i;
    for (i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        const char *kind = NULL;
        bool fatal = nk_ui_clay_error_kind(expected[i].type, &kind);
        if (kind == NULL || strcmp(kind, expected[i].kind) != 0 || fatal != expected[i].fatal) {
            fprintf(stderr, "FAIL: Clay error type %d is %s (%s), expected %s (%s)\n",
                    expected[i].type, kind != NULL ? kind : "(none)",
                    fatal ? "fatal" : "drawn", expected[i].kind,
                    expected[i].fatal ? "fatal" : "drawn");
            return 0;
        }
    }
    return 1;
}

typedef struct {
    int count;
    int fatal;
    char kinds[256];   /* every kind reported, space-separated */
    bool all_have_text;
} LayoutErrorLog;

static void record_layout_error(void *user, const NkUiLayoutError *error) {
    LayoutErrorLog *log = (LayoutErrorLog *)user;
    size_t used = strlen(log->kinds);
    ++log->count;
    if (error->fatal) ++log->fatal;
    (void)snprintf(log->kinds + used, sizeof(log->kinds) - used, "%s ", error->kind);
    if (error->text == NULL || error->text_length == 0u) log->all_have_text = false;
}

/* One frame of `spacers` spacers under a root, the first `duplicates` of them
   sharing one id. Returns whether nk_ui_end kept the frame. */
static bool layout_error_frame(NkUi *ui, double now_s, size_t spacers, size_t duplicates) {
    NkUiLayout layout;
    size_t i;
    memset(&layout, 0, sizeof(layout));
    layout.width = (NkUiSizing){NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f};
    layout.height = (NkUiSizing){NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f};
    layout.direction = NK_UI_LAYOUT_COLUMN;
    nk_ui_begin(ui, now_s);
    if (nk_ui_begin_container(ui, 1u, &layout, NULL)) {
        for (i = 0; i < spacers; ++i) {
            NkUiId id = i < duplicates ? 2u : (NkUiId)(100u + i);
            nk_ui_spacer(ui, id, (NkUiSizing){NK_UI_SIZE_FIXED, 4.0f, 0.0f, 0.0f},
                         (NkUiSizing){NK_UI_SIZE_FIXED, 1.0f, 0.0f, 0.0f});
        }
        nk_ui_end_container(ui);
    }
    return nk_ui_end(ui) != NULL;
}

/* A duplicate id still lays out a whole frame: it is drawn and reported once
   when it starts, not every frame it repeats. Past Clay's element limit the
   layout is incomplete, so that frame is dropped, and the next whole frame is
   drawn again. */
static int test_layout_errors_reported(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    LayoutErrorLog log;
    NkUiConfig config;
    NkUi *ui;
    memset(&log, 0, sizeof(log));
    log.all_have_text = true;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    config.report_error = record_layout_error;
    config.report_user = &log;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create context with an error reporter");
    CHECK(layout_error_frame(ui, 1.0, 3u, 2u), "a frame with a duplicate id is drawn");
    CHECK(log.count == 1 && strcmp(log.kinds, "DUPLICATE_ID ") == 0 && log.fatal == 0 &&
          log.all_have_text, "the duplicate id is reported as drawn, with Clay's text");
    CHECK(layout_error_frame(ui, 2.0, 3u, 2u) && log.count == 1,
          "a duplicate id that repeats the next frame is not reported again");
    CHECK(layout_error_frame(ui, 3.0, 3u, 0u) && log.count == 1, "a clean frame reports nothing");
    CHECK(layout_error_frame(ui, 4.0, 3u, 2u) && log.count == 2,
          "a duplicate id that comes back is reported again");
    /* Clay raises no error of its own past the limit; it also stops closing
       elements, so the frame is unbalanced too. */
    CHECK(!layout_error_frame(ui, 5.0, 9000u, 0u), "a frame past the element limit is dropped");
    CHECK(strstr(log.kinds, "ELEMENTS_CAPACITY_EXCEEDED ") != NULL &&
          log.fatal == log.count - 2 && log.all_have_text,
          "the element limit is reported as fatal");
    {
        int reported = log.count;
        CHECK(layout_error_frame(ui, 6.0, 3u, 0u) && log.count == reported,
              "the next whole frame is drawn again");
    }
    nk_ui_destroy(ui);
    return 1;
}

/* nk_ui draws pictures as its own custom elements. A Clay image element (here
   declared straight through Clay, as nothing in nk_ui does) is a command the
   draw list cannot carry: the frame is dropped and reported, never drawn
   with the picture silently missing. */
static int test_undrawn_command_is_reported(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    LayoutErrorLog log;
    NkUiConfig config;
    NkUiLayout layout;
    NkUi *ui;
    int picture = 0;
    memset(&log, 0, sizeof(log));
    log.all_have_text = true;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    config.report_error = record_layout_error;
    config.report_user = &log;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create undrawn-command context");
    set_test_window(ui);
    memset(&layout, 0, sizeof(layout));
    layout.width = (NkUiSizing){NK_UI_SIZE_FIXED, 100.0f, 0.0f, 0.0f};
    layout.height = (NkUiSizing){NK_UI_SIZE_FIXED, 100.0f, 0.0f, 0.0f};
    nk_ui_begin(ui, 37.0);
    if (nk_ui_begin_container(ui, 1u, &layout, NULL)) {
        Clay_ElementDeclaration image;
        memset(&image, 0, sizeof(image));
        image.layout.sizing.width = CLAY_SIZING_FIXED(10.0f);
        image.layout.sizing.height = CLAY_SIZING_FIXED(10.0f);
        image.image.imageData = &picture;
        Clay__OpenElementWithId(Clay_GetElementId(CLAY_STRING("test.image")));
        Clay__ConfigureOpenElement(image);
        Clay__CloseElement();
        nk_ui_end_container(ui);
    }
    CHECK(nk_ui_end(ui) == NULL, "a frame with a Clay image element is dropped");
    CHECK(log.count == 1 && strcmp(log.kinds, "UNDRAWN_RENDER_COMMAND ") == 0 && log.fatal == 1 &&
          log.all_have_text, "and the command it could not draw is reported");
    nk_ui_destroy(ui);
    return 1;
}

/* The developer log on a tall window: a hundred long lines, each clamped and
   then drawn (nk_ui_text of nk_ui_text_clamp), past 16 KB of frame text. The
   frame is drawn, nothing is reported, the first line's storage holds while
   more text is added, and a clamp's result is drawn where it is, not copied
   again. */
static int test_frame_text_grows(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    NkUiTextStyle style = nk_ui_type(NK_UI_TYPE_DATA, 0xFFFFFFFFu);
    LayoutErrorLog log;
    NkUiConfig config;
    NkUiViewport viewport;
    NkUiLayout layout;
    NkUi *ui;
    int frame;
    memset(&log, 0, sizeof(log));
    log.all_have_text = true;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    config.display_text = synth_display_text;
    config.report_error = record_layout_error;
    config.report_user = &log;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create frame text context");
    memset(&viewport, 0, sizeof(viewport));
    viewport.pixel_w = 2560;
    viewport.pixel_h = 1440;
    viewport.display_scale = 1.0f;
    viewport.ui_scale_pct = 100;
    viewport.text_size_pct = 100;
    nk_ui_set_viewport(ui, &viewport);
    memset(&layout, 0, sizeof(layout));
    layout.width = (NkUiSizing){NK_UI_SIZE_FIXED, 2400.0f, 0.0f, 0.0f};
    layout.height = (NkUiSizing){NK_UI_SIZE_FIT, 0.0f, 0.0f, 0.0f};
    layout.direction = NK_UI_LAYOUT_COLUMN;
    for (frame = 0; frame < 2; ++frame) {
        const char *first = NULL;
        unsigned i;
        nk_ui_begin(ui, 1.0 + frame);
        CHECK(nk_ui_begin_container(ui, 1u, &layout, NULL), "open the log column");
        for (i = 0u; i < 100u; ++i) {
            char line[241];
            const char *shown;
            size_t used = (size_t)snprintf(line, sizeof(line), "%03u ", i);
            while (used + 5u < sizeof(line)) {
                memcpy(line + used, "word ", 5u);
                used += 5u;
            }
            line[used] = '\0';
            shown = nk_ui_text_clamp(ui, line, &style, 2400.0f, 1);
            if (i == 0u) {
                first = shown;
                CHECK(nk_ui_frame_text(ui, shown) == shown,
                      "text already in the frame is not copied again");
            }
            nk_ui_text(ui, (NkUiId)(1000u + i), shown, &style);
        }
        nk_ui_end_container(ui);
        CHECK(first != NULL && strncmp(first, "000 word ", 9u) == 0,
              "the first line keeps its storage while more text is added");
        CHECK(nk_ui_end(ui) != NULL, "a frame with more than 16 KB of text is drawn");
        CHECK(log.count == 0, "and nothing is reported");
    }
    nk_ui_destroy(ui);
    return 1;
}

/* An Enter pressed just before a held arrow repeats, in one frame's queue:
   it fires the control that was focused when it was pressed, and the repeat
   still moves focus. A click released just before an arrow fires the control
   clicked. */
static int test_activation_belongs_to_its_focus(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    NkUiConfig config;
    NkUiEvent activate;
    NkUiEvent repeat;
    NkUi *ui;
    int frame;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create activation order context");
    for (frame = 0; frame < 3; ++frame) (void)draw_button_pair(ui, 10u, frame == 0 ? 10u : 0u);
    CHECK(nk_ui_focused(ui) == 10u, "the pair opens on its first button");
    memset(&activate, 0, sizeof(activate));
    activate.type = NK_UI_EV_ACTIVATE;
    activate.down = true;
    memset(&repeat, 0, sizeof(repeat));
    repeat.type = NK_UI_EV_NAV;
    repeat.dir = NK_UI_DIR_RIGHT;
    repeat.down = true;
    repeat.repeat = true;
    nk_ui_push_event(ui, &activate);
    nk_ui_push_event(ui, &repeat);
    CHECK(draw_button_pair(ui, 10u, 0u) == 10u,
          "Enter fires the button focused when it was pressed");
    CHECK(nk_ui_focused(ui) == 11u, "and the arrow after it still moves focus");
    nk_ui_focus(ui, 10u);
    (void)draw_button_pair(ui, 10u, 0u);
    CHECK(nk_ui_focused(ui) == 10u, "focus is back on the first button");
    push_pointer(ui, 10.0f, 10.0f, true);
    (void)draw_button_pair(ui, 10u, 0u);
    push_pointer(ui, 10.0f, 10.0f, false);
    repeat.repeat = false;
    nk_ui_push_event(ui, &repeat);
    CHECK(draw_button_pair(ui, 10u, 0u) == 10u,
          "a click released before an arrow fires the button clicked");
    CHECK(nk_ui_focused(ui) == 11u, "and the arrow moves focus on");
    nk_ui_destroy(ui);
    return 1;
}

/* nk_ui_focus(ui, 0) drops the focus for good: the frame ends on the screen's
   default control, not on the control that had it. */
static int test_focus_zero_returns_to_default(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    NkUiConfig config;
    NkUi *ui;
    int frame;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create focus reset context");
    for (frame = 0; frame < 3; ++frame) (void)draw_button_pair(ui, 10u, 0u);
    CHECK(nk_ui_focused(ui) == 10u, "the pair's default is its first button");
    (void)draw_button_pair(ui, 10u, 11u);
    (void)draw_button_pair(ui, 10u, 0u);
    CHECK(nk_ui_focused(ui) == 11u, "focus moves to the second button");
    nk_ui_focus(ui, 0u);
    (void)draw_button_pair(ui, 10u, 0u);
    CHECK(nk_ui_focused(ui) == 10u, "focus 0 returns to the default button");
    (void)draw_button_pair(ui, 10u, 0u);
    CHECK(nk_ui_focused(ui) == 10u, "and stays there");
    nk_ui_destroy(ui);
    return 1;
}

/* A stalled frame under a fast mouse: a press on one button, more held moves
   onto the other than the queue holds, and the release there. The queue drops
   moves, never the press, so this drag clicks neither button. */
static int test_full_queue_keeps_pointer_edges(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    NkUiConfig config;
    NkUi *ui;
    NkUiId activated = 0u;
    int frame;
    int move;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create full queue context");
    for (frame = 0; frame < 3; ++frame) (void)draw_button_pair(ui, 10u, frame == 0 ? 10u : 0u);
    push_pointer(ui, 100.0f, 10.0f, true);
    push_pointer(ui, 100.0f, 10.0f, false);
    CHECK(draw_button_pair(ui, 10u, 0u) == 11u, "a click at x 100 fires the second button");
    push_pointer(ui, 10.0f, 10.0f, true);
    for (move = 0; move < 80; ++move) push_pointer(ui, 100.0f, 10.0f, true);
    push_pointer(ui, 100.0f, 10.0f, false);
    for (frame = 0; frame < 3 && activated == 0u; ++frame) {
        activated = draw_button_pair(ui, 10u, 0u);
    }
    CHECK(activated == 0u, "a drag from one button to the other clicks neither");
    CHECK(nk_ui_focused(ui) == 10u, "the press kept its place in the queue");
    nk_ui_destroy(ui);
    return 1;
}

/* nk_ui's own limits drop a frame and say why, through report_error, once
   when they start: a container left open, too many scroll containers. */
static int test_own_errors_reported(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    LayoutErrorLog log;
    NkUiConfig config;
    NkUiLayout layout;
    NkUi *ui;
    int frame;
    memset(&log, 0, sizeof(log));
    log.all_have_text = true;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    config.report_error = record_layout_error;
    config.report_user = &log;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create own error context");
    memset(&layout, 0, sizeof(layout));
    layout.width = (NkUiSizing){NK_UI_SIZE_FIXED, 100.0f, 0.0f, 0.0f};
    layout.height = (NkUiSizing){NK_UI_SIZE_FIXED, 100.0f, 0.0f, 0.0f};
    for (frame = 0; frame < 2; ++frame) {
        nk_ui_begin(ui, 1.0 + frame);
        CHECK(nk_ui_begin_container(ui, 1u, &layout, NULL), "open a container");
        CHECK(nk_ui_end(ui) == NULL, "a frame left with a container open is dropped");
    }
    CHECK(log.count == 1 && strcmp(log.kinds, "CONTAINER_LEFT_OPEN ") == 0 &&
          log.fatal == 1 && log.all_have_text,
          "and reported once, as fatal, with its text");
    layout.scroll = true;
    nk_ui_begin(ui, 3.0);
    {
        unsigned i;
        for (i = 0u; i <= NK_UI_SCROLL_MAX; ++i) {
            if (nk_ui_begin_container(ui, (NkUiId)(10u + i), &layout, NULL)) {
                nk_ui_end_container(ui);
            }
        }
    }
    CHECK(nk_ui_end(ui) == NULL, "a frame past the scroll containers it can hold is dropped");
    CHECK(log.count == 2 && strstr(log.kinds, "SCROLL_CONTAINERS_EXCEEDED ") != NULL,
          "and the limit is named");
    /* Each line height is its own text style in the measure cache's key; a
       frame with more styles than the table holds is dropped by name, and the
       next frame starts the table over. */
    layout.scroll = false;
    layout.direction = NK_UI_LAYOUT_COLUMN;
    {
        NkUiTextStyle style = nk_ui_type(NK_UI_TYPE_BODY, 0u);
        unsigned i;
        style.size_px = 16;
        style.wrap = false;
        nk_ui_begin(ui, 4.0);
        CHECK(nk_ui_begin_container(ui, 1u, &layout, NULL), "open the text-style column");
        for (i = 0u; i < 300u; ++i) {
            style.line_height = 16.0f + (float)i;
            nk_ui_text(ui, (NkUiId)(100u + i), "x", &style);
        }
        nk_ui_end_container(ui);
        CHECK(nk_ui_end(ui) == NULL, "a frame past the text styles it can tell apart is dropped");
        CHECK(log.count == 3 && strstr(log.kinds, "TEXT_STYLES_EXCEEDED ") != NULL,
              "and the limit is named");
        nk_ui_begin(ui, 5.0);
        CHECK(nk_ui_begin_container(ui, 1u, &layout, NULL), "open the next text-style column");
        nk_ui_text(ui, 100u, "x", &style);
        nk_ui_end_container(ui);
        CHECK(nk_ui_end(ui) != NULL, "the next frame starts the text styles over and draws");
    }
    nk_ui_destroy(ui);
    return 1;
}

/* A label formatted into a buffer the caller reuses before the frame ends
   draws what it was given: the text is copied into the frame. */
static int test_label_copies_text(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    NkUiConfig config;
    NkUiLayout layout;
    NkUi *ui;
    char buffer[32];
    char dump[4096];
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    memset(&layout, 0, sizeof(layout));
    layout.width = (NkUiSizing){NK_UI_SIZE_FIXED, 400.0f, 0.0f, 0.0f};
    layout.height = (NkUiSizing){NK_UI_SIZE_FIXED, 100.0f, 0.0f, 0.0f};
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create label copy context");
    nk_ui_begin(ui, 0.0);
    CHECK(nk_ui_begin_container(ui, 1u, &layout, NULL), "open label container");
    strcpy(buffer, "First label");
    nk_ui_label(ui, 2u, buffer, 0, 16);
    strcpy(buffer, "Wrong label");
    nk_ui_end_container(ui);
    CHECK(nk_ui_end(ui) != NULL, "complete label frame");
    CHECK(nk_ui_dump(ui, dump, sizeof(dump)) < sizeof(dump), "label dump fits");
    CHECK(strstr(dump, "First label") != NULL && strstr(dump, "Wrong label") == NULL,
          "the label draws the text it was given");
    nk_ui_destroy(ui);
    return 1;
}

/* A clamp's input longer than the text it measures (2043 bytes) is cut there
   and ends with the ellipsis even when the part measured fits, and the cut
   never splits a character. */
static int test_clamp_cuts_long_text(void) {
    static const char ellipsis[] = "\xE2\x80\xA6";
    NkUiTextBackend fixed = nk_ui_text_fixed();
    NkUiTextStyle style = nk_ui_type(NK_UI_TYPE_BODY, 0xFFFFFFFFu);
    NkUiConfig config;
    NkUi *ui;
    char text[2201];
    const char *shown;
    size_t length;
    size_t i;
    for (i = 0u; i < 1100u; ++i) {
        text[2u * i] = (char)0xC3;
        text[2u * i + 1u] = (char)0xA9;
    }
    text[2200] = '\0';
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create long clamp context");
    nk_ui_begin(ui, 0.0);
    shown = nk_ui_text_clamp(ui, text, &style, 1.0e9f, 1);
    length = strlen(shown);
    CHECK(length > sizeof(ellipsis) - 1u && length < sizeof(text) - 1u &&
          memcmp(shown + length - (sizeof(ellipsis) - 1u), ellipsis, sizeof(ellipsis) - 1u) == 0,
          "a text past what the clamp measures is cut with an ellipsis");
    CHECK((length - (sizeof(ellipsis) - 1u)) % 2u == 0u,
          "the cut ends on a whole character");
    (void)nk_ui_end(ui);
    nk_ui_destroy(ui);
    return 1;
}

/* Two runs of the same words, face, size and spacing that differ only in
   their line height each draw in their own line box. The measured height
   includes the line height, so Clay's text-measure cache must key on it: when
   it did not, the second run reused the first run's 40 px height and was drawn
   10 px above its 20 px box. */
static int test_line_height_keys_the_measure_cache(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    NkUiConfig config;
    NkUiViewport viewport;
    NkUiLayout layout;
    NkUiTextStyle tall = nk_ui_type(NK_UI_TYPE_BODY, 0u);
    NkUiTextStyle short_line;
    const NkUiDrawList *list;
    const NkUiDrawCmd *first;
    const NkUiDrawCmd *second;
    NkUi *ui;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    memset(&layout, 0, sizeof(layout));
    layout.width = (NkUiSizing){NK_UI_SIZE_FIXED, 400.0f, 0.0f, 0.0f};
    layout.height = (NkUiSizing){NK_UI_SIZE_FIXED, 200.0f, 0.0f, 0.0f};
    layout.direction = NK_UI_LAYOUT_COLUMN;
    tall.size_px = 16;
    tall.letter_spacing = 0.0f;
    tall.wrap = false;
    tall.line_height = 40.0f;
    short_line = tall;
    short_line.line_height = 20.0f;
    memset(&viewport, 0, sizeof(viewport));
    viewport.pixel_w = 640;
    viewport.pixel_h = 400;
    viewport.display_scale = 1.0f;
    viewport.ui_scale_pct = 100;
    viewport.text_size_pct = 100;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create the line-height context");
    nk_ui_set_viewport(ui, &viewport);
    for (int frame = 0; frame < 2; ++frame) {
        nk_ui_begin(ui, 1.0 + frame);
        CHECK(nk_ui_begin_container(ui, 1u, &layout, NULL), "open the line-height column");
        nk_ui_text(ui, 2u, "Same words", &tall);
        nk_ui_text(ui, 3u, "Same words", &short_line);
        nk_ui_end_container(ui);
        list = nk_ui_end(ui);
        CHECK(list != NULL, "lay out the line-height frame");
    }
    first = text_size_command(list, 2u, NK_UI_DRAW_TEXT);
    second = text_size_command(list, 3u, NK_UI_DRAW_TEXT);
    CHECK(first != NULL && second != NULL, "both runs draw");
    CHECK(first->bounds[1] == 0 && text_size_height(first) == 40,
          "the 40 px run fills its own line box");
    CHECK(second->bounds[1] == 40 && text_size_height(second) == 20,
          "the 20 px run draws in its own box, not offset by the 40 px run's height");
    nk_ui_destroy(ui);
    return 1;
}

/* A prompt glyph whose image rounds to no width draws nothing; the frame stays. */
static int test_empty_prompt_glyph_keeps_the_frame(void) {
    static NkUiPromptImages images;
    NkUiTextBackend fixed = nk_ui_text_fixed();
    NkUiConfig config;
    NkUiLayout layout;
    NkUi *ui;
    CHECK(nk_ui_prompt_images_load(&images) > 0u, "prompt images decode");
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create prompt-glyph context");
    set_test_window(ui);
    nk_ui_set_prompt_images(ui, &images);
    memset(&layout, 0, sizeof(layout));
    layout.width = (NkUiSizing){NK_UI_SIZE_FIXED, 100.0f, 0.0f, 0.0f};
    layout.height = (NkUiSizing){NK_UI_SIZE_FIXED, 40.0f, 0.0f, 0.0f};
    nk_ui_begin(ui, 37.0);
    CHECK(nk_ui_begin_container(ui, 1u, &layout, NULL), "open prompt-glyph container");
    /* 64 x 64 glyph at 0.4 px tall: its width rounds to 0. */
    CHECK(nk_ui_prompt_glyph_in(ui, 2u, NK_UI_PROMPT_KEYCAPS, NK_UI_PROMPT_ACT_CONFIRM, NULL,
                                0.4f), "the narrow glyph is laid out");
    CHECK(!nk_ui_prompt_glyph_in(ui, 3u, NK_UI_PROMPT_KEYCAPS, NK_UI_PROMPT_ACT_COUNT, NULL,
                                 20.0f), "an action out of range is refused when it is laid out");
    nk_ui_label(ui, 4u, "Still here", 0, 16);
    nk_ui_end_container(ui);
    CHECK(nk_ui_end(ui) != NULL, "a prompt glyph with no width does not drop the frame");
    nk_ui_destroy(ui);
    nk_ui_prompt_images_free(&images);
    return 1;
}

/* nk_ui_caps cuts a long label on a whole UTF-8 sequence. */
static int test_caps_cuts_on_a_sequence(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    NkUiConfig config;
    NkUi *ui;
    char text[300];
    const char *caps;
    size_t i;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create caps context");
    /* 254 ASCII letters, then a three-byte kanji the 255-byte cut would split. */
    memset(text, 'a', 254u);
    memcpy(text + 254u, "\xe6\x97\xa5\xe6\x97\xa5", 6u);
    text[260] = '\0';
    nk_ui_begin(ui, 37.0);
    caps = nk_ui_caps(ui, text);
    CHECK(caps != NULL && strlen(caps) == 254u, "the cut drops the split kanji whole");
    for (i = 0; i < 254u; ++i) {
        CHECK(caps[i] == 'A', "the letters before the cut are upper-cased");
    }
    /* A whole kanji before the cut is kept. */
    memset(text, 'b', 252u);
    memcpy(text + 252u, "\xe6\x97\xa5\xe6\x97\xa5", 6u);
    text[258] = '\0';
    caps = nk_ui_caps(ui, text);
    CHECK(caps != NULL && strlen(caps) == 255u &&
          memcmp(caps + 252u, "\xe6\x97\xa5", 3u) == 0, "a sequence that fits stays");
    (void)nk_ui_end(ui);
    nk_ui_destroy(ui);
    return 1;
}

int main(int argc, char **argv) {
    char actual[32768];
    size_t actual_size;
    if (!test_no_platform_headers() || !test_theme_tokens() || !test_scale_model() ||
        !test_text_size_scales_type_only() ||
        !test_wrapped_lines_fit_their_box() || !test_text_break_keeps_every_line_in_its_box() ||
        !test_word_measure_reads_a_bounded_context() ||
        !test_explicit_ui_scale_values() || !test_typed_prefs_and_events() ||
        !test_theme_access() || !test_overlay_rejects_waves() ||
        !test_container_return_contract() || !test_backend_declarations() ||
        !test_rounding_edges() || !test_progress_sweep() || !test_focused_button_activation() ||
        !test_input_waits_for_a_new_screen_layout() ||
        !test_click_needs_press_and_release_on_one_control() ||
        !test_control_owns_horizontal() || !test_wave_caps() ||
        !test_image_decode() || !test_image_decode_bounds() ||
        !test_image_resample_wide_average() || !test_image_command() || !test_app_icon() ||
        !test_display_text_rules() || !test_list_lines() || !test_layout_error_kinds() ||
        !test_layout_errors_reported() || !test_pointer_hover_and_press() ||
        !test_pointer_respects_clip() || !test_scroll_container() ||
        !test_draw_clips_nest() || !test_draw_text_runs_reused() ||
        !test_modal_card_floats() || !test_focus_ring_floats() ||
        !test_focus_ring_moves() || !test_focus_ring_waits_for_keys_after_touch() ||
        !test_prompt_bar_is_tappable_in_touch_mode() || !test_hover_fill_eases() || !test_tab_rule_slides() ||
        !test_wait_follows_motion() || !test_art_fades_in() ||
        !test_backdrop_rests_and_fades() ||
        !test_layout_debug() || !test_pointer_state_once_a_frame() ||
        !test_undrawn_command_is_reported() || !test_frame_text_grows() ||
        !test_activation_belongs_to_its_focus() || !test_focus_zero_returns_to_default() ||
        !test_full_queue_keeps_pointer_edges() || !test_own_errors_reported() ||
        !test_label_copies_text() || !test_clamp_cuts_long_text() ||
        !test_line_height_keys_the_measure_cache() ||
        !test_empty_prompt_glyph_keeps_the_frame() || !test_caps_cuts_on_a_sequence()) {
        return 1;
    }
    if (!build_golden_output(actual, sizeof(actual))) {
        return 1;
    }
    actual_size = strlen(actual);
    if (argc > 1 && strcmp(argv[1], "--print") == 0) {
        fputs(actual, stdout);
        return 0;
    }
    if (!test_golden(actual, actual_size)) {
        return 1;
    }
    puts("nk_ui core goldens, pixel rounding, SDL/Vulkan boundary, wave caps and image decoding passed");
    return 0;
}

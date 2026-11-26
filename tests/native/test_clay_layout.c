// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Upstream's own warnings, silenced as src/ui/nk_ui_clay.c does. */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#define CLAY_IMPLEMENTATION
#include "../../third_party/clay/clay.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

static int clay_error_count;
static Clay_ErrorType clay_last_error;
/* One bit per Clay_ErrorType seen since a test last cleared it. */
static unsigned clay_errors_seen;

static void record_clay_error(Clay_ErrorData error)
{
    clay_error_count++;
    clay_last_error = error.errorType;
    if ((unsigned)error.errorType < 32u) clay_errors_seen |= 1u << (unsigned)error.errorType;
    fprintf(stderr, "Clay error %d\n", (int)error.errorType);
}

/* Every character is 10 x 10, so a word's width is ten times its length. */
static Clay_Dimensions measure_text(Clay_StringSlice text,
                                    Clay_TextElementConfig *config,
                                    void *user_data)
{
    (void)config;
    (void)user_data;
    if (text.length <= 0) {
        return (Clay_Dimensions){ 0.0f, 0.0f };
    }
    return (Clay_Dimensions){ 10.0f * (float)text.length, 10.0f };
}

/* A 50 x 50 scroll container over 500 px of content, at the top left. */
static void scroll_frame(float pointer_y, bool pointer_down)
{
    Clay_SetPointerState((Clay_Vector2){ 10.0f, pointer_y }, pointer_down);
    Clay_UpdateScrollContainers(true, (Clay_Vector2){ 0.0f, 0.0f }, 0.0f);
    Clay_BeginLayout();
    CLAY(CLAY_ID("scroller"), {
        .layout = { .sizing = { .width = CLAY_SIZING_FIXED(50.0f),
                                .height = CLAY_SIZING_FIXED(50.0f) } },
        .clip = { .vertical = true, .childOffset = Clay_GetScrollOffset() },
    }) {
        CLAY(CLAY_ID("scrolled"), {
            .layout = { .sizing = { .width = CLAY_SIZING_FIXED(50.0f),
                                    .height = CLAY_SIZING_FIXED(500.0f) } },
        }) {}
    }
    (void)Clay_EndLayout(0.0f);
}

static int expect_rectangle(const char *name, Clay_ElementId id,
                            float x, float y, float width, float height)
{
    Clay_ElementData element = Clay_GetElementData(id);

    if (!element.found) {
        fprintf(stderr, "FAIL: Clay element %s was not found\n", name);
        return 0;
    }

    if (element.boundingBox.x != x || element.boundingBox.y != y ||
        element.boundingBox.width != width ||
        element.boundingBox.height != height) {
        fprintf(stderr,
                "FAIL: %s rectangle expected (%g, %g, %g, %g), got (%g, %g, %g, %g)\n",
                name, (double)x, (double)y, (double)width, (double)height,
                (double)element.boundingBox.x, (double)element.boundingBox.y,
                (double)element.boundingBox.width,
                (double)element.boundingBox.height);
        return 0;
    }
    return 1;
}

/* Bytes past the end of a guarded arena, checked after a layout. */
#define GUARD_BYTES 16384u
#define GUARD_FILL 0xA5u

/* A fresh context in an arena of exactly Clay_MinMemorySize() bytes, with
   GUARD_BYTES of known bytes behind it. Returns the block, or NULL. */
static unsigned char *guarded_context(uint32_t *arena_bytes)
{
    Clay_ErrorHandler error_handler = { record_clay_error, NULL };
    uint32_t size = Clay_MinMemorySize();
    unsigned char *block = malloc((size_t)size + GUARD_BYTES);

    if (!block) {
        fprintf(stderr, "FAIL: unable to allocate a guarded Clay arena\n");
        return NULL;
    }
    memset(block + size, (int)GUARD_FILL, GUARD_BYTES);
    if (!Clay_Initialize(Clay_CreateArenaWithCapacityAndMemory(size, block),
                         (Clay_Dimensions){ 800.0f, 600.0f }, error_handler)) {
        fprintf(stderr, "FAIL: guarded Clay initialization failed\n");
        free(block);
        return NULL;
    }
    Clay_SetMeasureTextFunction(measure_text, NULL);
    *arena_bytes = size;
    return block;
}

static int guard_intact(const unsigned char *block, uint32_t arena_bytes)
{
    for (uint32_t i = 0; i < GUARD_BYTES; ++i) {
        if (block[arena_bytes + i] != GUARD_FILL) {
            return 0;
        }
    }
    return 1;
}

/* The debug view lists every element with an id and prints each id's offset.
   Those digits go to a buffer of one char per element at the end of the
   arena: 1000 offsets of 11 characters (a negative 10-digit number) are more
   than it holds, and none of them may land past the arena. */
static int debug_view_id_digits_stay_in_arena(void)
{
    uint32_t arena_bytes = 0;
    unsigned char *block = guarded_context(&arena_bytes);
    int ok;

    if (!block) {
        return 0;
    }
    clay_error_count = 0;
    clay_errors_seen = 0;
    Clay_SetDebugModeEnabled(true);
    Clay_BeginLayout();
    CLAY(CLAY_ID("listed"), {
        .layout = { .layoutDirection = CLAY_TOP_TO_BOTTOM },
    }) {
        for (uint32_t i = 0; i < 1000u; ++i) {
            CLAY(CLAY_IDI("row", 3000000000u + i), {
                .layout = { .sizing = { .width = CLAY_SIZING_FIXED(1.0f),
                                        .height = CLAY_SIZING_FIXED(1.0f) } },
            }) {}
        }
    }
    (void)Clay_EndLayout(0.0f);
    Clay_SetDebugModeEnabled(false);

    ok = guard_intact(block, arena_bytes);
    if (!ok) {
        fprintf(stderr, "FAIL: the debug view wrote id digits past the Clay arena\n");
    } else if (!(clay_errors_seen & (1u << CLAY_ERROR_TYPE_ARENA_CAPACITY_EXCEEDED))) {
        fprintf(stderr, "FAIL: the debug view ran out of digit space without an error\n");
        ok = 0;
    }
    clay_error_count = 0;
    free(block);
    return ok;
}

/* A frame with a transition lays out twice. Each pass wraps every text again,
   so 5000 one-line texts need 5000 wrapped lines, well inside the pool of one
   per element; both passes together need more than that. The layout must have
   no error, every text one line, and every TEXT command (those on screen) the
   text it came from. */
static int wrapped_lines_fit_a_two_pass_layout(void)
{
    int text_commands = 0;
    int bad_commands = 0;
    Clay_RenderCommandArray commands;
    Clay_ElementData eased;

    clay_error_count = 0;
    clay_errors_seen = 0;
    for (int frame = 0; frame < 2; ++frame) {
        Clay_BeginLayout();
        CLAY(CLAY_ID("eased"), {
            .layout = { .layoutDirection = CLAY_TOP_TO_BOTTOM },
            .backgroundColor = { 10, 20, 30, 255 },
            .transition = { .handler = Clay_EaseOut, .duration = 1.0f,
                            .properties = CLAY_TRANSITION_PROPERTY_BACKGROUND_COLOR },
        }) {
            for (int i = 0; i < 5000; ++i) {
                CLAY_TEXT(CLAY_STRING("ab"), CLAY_TEXT_CONFIG({ .fontSize = 10, .lineHeight = 10 }));
            }
        }
        commands = Clay_EndLayout(0.0f);
    }
    for (int32_t i = 0; i < commands.length; ++i) {
        Clay_RenderCommand *command = Clay_RenderCommandArray_Get(&commands, i);
        if (command->commandType != CLAY_RENDER_COMMAND_TYPE_TEXT) {
            continue;
        }
        text_commands++;
        if (command->renderData.text.stringContents.length != 2) {
            bad_commands++;
        }
    }
    eased = Clay_GetElementData(CLAY_ID("eased"));
    if (clay_error_count != 0 || text_commands == 0 || bad_commands != 0 ||
        !eased.found || eased.boundingBox.height != 50000.0f) {
        fprintf(stderr, "FAIL: a two-pass layout of 5000 texts gave %d errors, %d of %d TEXT "
                        "commands wrong, and a column %g px high, not 50000\n",
                clay_error_count, bad_commands, text_commands,
                eased.found ? (double)eased.boundingBox.height : 0.0);
        return 0;
    }
    return 1;
}

/* A word too wide for its line gets a line of its own, and a newline right
   after it ends that line; it adds no second one. "abcdef " is 70 px in a
   60 px paragraph, so the text is two lines, "abcdef " and "next", whether
   or not the long word ends in a space. */
static int newline_after_overlong_word(const char *name, Clay_String text,
                                       int32_t first_length)
{
    Clay_RenderCommandArray commands;
    Clay_ElementData paragraph;
    int32_t lengths[4];
    int text_commands = 0;

    clay_error_count = 0;
    Clay_BeginLayout();
    CLAY(CLAY_ID("narrow"), {
        .layout = { .sizing = { .width = CLAY_SIZING_FIXED(60.0f),
                                .height = CLAY_SIZING_FIT(0) } },
    }) {
        CLAY_TEXT(text, CLAY_TEXT_CONFIG({ .fontSize = 10, .lineHeight = 10,
                                           .wrapMode = CLAY_TEXT_WRAP_WORDS }));
    }
    commands = Clay_EndLayout(0.0f);
    for (int32_t i = 0; i < commands.length; ++i) {
        Clay_RenderCommand *command = Clay_RenderCommandArray_Get(&commands, i);
        if (command->commandType == CLAY_RENDER_COMMAND_TYPE_TEXT) {
            if (text_commands < 4) {
                lengths[text_commands] = command->renderData.text.stringContents.length;
            }
            text_commands++;
        }
    }
    paragraph = Clay_GetElementData(CLAY_ID("narrow"));
    if (clay_error_count != 0 || text_commands != 2 || lengths[0] != first_length ||
        lengths[1] != 4 || !paragraph.found || paragraph.boundingBox.height != 20.0f) {
        fprintf(stderr, "FAIL: %s: %d TEXT commands (the first %d chars long) in a "
                        "paragraph %g px high, not two lines in 20 px\n",
                name, text_commands, text_commands > 0 ? (int)lengths[0] : 0,
                paragraph.found ? (double)paragraph.boundingBox.height : 0.0);
        return 0;
    }
    return 1;
}

/* Two 50 x 50 scroll containers over 500 px of content, side by side; either
   can be left out. */
static void two_scrollers_frame(bool with_left)
{
    Clay_BeginLayout();
    CLAY(CLAY_ID("pair"), { .layout = { .layoutDirection = CLAY_LEFT_TO_RIGHT } }) {
        if (with_left) {
            CLAY(CLAY_ID("left"), {
                .layout = { .sizing = { .width = CLAY_SIZING_FIXED(50.0f),
                                        .height = CLAY_SIZING_FIXED(50.0f) } },
                .clip = { .vertical = true, .childOffset = Clay_GetScrollOffset() },
            }) {
                CLAY(CLAY_ID("left-content"), {
                    .layout = { .sizing = { .width = CLAY_SIZING_FIXED(50.0f),
                                            .height = CLAY_SIZING_FIXED(500.0f) } },
                }) {}
            }
        }
        CLAY(CLAY_ID("right"), {
            .layout = { .sizing = { .width = CLAY_SIZING_FIXED(50.0f),
                                    .height = CLAY_SIZING_FIXED(50.0f) } },
            .clip = { .vertical = true, .childOffset = Clay_GetScrollOffset() },
        }) {
            CLAY(CLAY_ID("right-content"), {
                .layout = { .sizing = { .width = CLAY_SIZING_FIXED(50.0f),
                                        .height = CLAY_SIZING_FIXED(500.0f) } },
            }) {}
        }
    }
    (void)Clay_EndLayout(0.0f);
}

/* When one scroll container stops being declared, the one listed after it
   still takes the wheel in that same update. */
static int scroll_container_after_a_removed_one_takes_the_wheel(void)
{
    Clay_ScrollContainerData right;

    clay_error_count = 0;
    /* Two updates around an empty layout retire every earlier scroll
       container, so "left" and "right" are listed in that order. */
    Clay_SetPointerState((Clay_Vector2){ 75.0f, 10.0f }, false);
    Clay_UpdateScrollContainers(false, (Clay_Vector2){ 0.0f, 0.0f }, 0.016f);
    Clay_BeginLayout();
    (void)Clay_EndLayout(0.0f);
    Clay_UpdateScrollContainers(false, (Clay_Vector2){ 0.0f, 0.0f }, 0.016f);
    two_scrollers_frame(true);
    Clay_SetPointerState((Clay_Vector2){ 75.0f, 10.0f }, false);
    Clay_UpdateScrollContainers(false, (Clay_Vector2){ 0.0f, 0.0f }, 0.016f);
    two_scrollers_frame(false);
    Clay_SetPointerState((Clay_Vector2){ 25.0f, 10.0f }, false);
    Clay_UpdateScrollContainers(false, (Clay_Vector2){ 0.0f, -1.0f }, 0.016f);
    right = Clay_GetScrollContainerData(CLAY_ID("right"));
    if (clay_error_count != 0 || !right.found || right.scrollPosition->y != -10.0f) {
        fprintf(stderr, "FAIL: a wheel notch over the remaining scroll container moved it "
                        "to %g, not -10\n",
                right.found ? (double)right.scrollPosition->y : 0.0);
        return 0;
    }
    return 1;
}

/* One text measured into a container around it, as nk_ui passes text: not
   statically allocated, so the measure cache keys it by its contents. */
static float measured_text_width(const char *id, const char *chars, int32_t length)
{
    Clay_String text = { .isStaticallyAllocated = false, .length = length, .chars = chars };
    Clay_ElementData element;
    Clay_ElementId element_id = Clay_GetElementId((Clay_String){ .length = (int32_t)strlen(id), .chars = id });

    Clay_BeginLayout();
    CLAY(element_id, { .layout = { .sizing = { .width = CLAY_SIZING_FIT(0),
                                               .height = CLAY_SIZING_FIT(0) } } }) {
        CLAY_TEXT(text, CLAY_TEXT_CONFIG({ .fontSize = 10, .lineHeight = 10 }));
    }
    (void)Clay_EndLayout(0.0f);
    element = Clay_GetElementData(element_id);
    return element.found ? element.boundingBox.width : -1.0f;
}

/* The measure cache's 32-bit id for a text nk_ui would pass. */
static uint32_t measure_cache_id(const char *chars, int length, Clay_TextElementConfig *config)
{
    Clay_String text = { .isStaticallyAllocated = false, .length = length, .chars = chars };
    uint64_t content_key;
    return Clay__HashStringContentsWithConfig(&text, config, &content_key);
}

/* A text with more words than the word cache holds fails to measure. The
   words it took must come back: the next, short text measures without an
   error, though the cache's slots were all used once. */
static int word_cache_recovers_after_an_overfull_text(void)
{
    enum { WORDS = 20000 };
    static char crowd[WORDS * 2];
    float width;

    for (int i = 0; i < WORDS; ++i) {
        crowd[2 * i] = 'a';
        crowd[2 * i + 1] = ' ';
    }
    clay_error_count = 0;
    clay_errors_seen = 0;
    (void)measured_text_width("overfull", crowd, (int32_t)sizeof(crowd));
    if (!(clay_errors_seen & (1u << CLAY_ERROR_TYPE_TEXT_MEASUREMENT_CAPACITY_EXCEEDED))) {
        fprintf(stderr, "FAIL: %d words measured without a word cache error\n", (int)WORDS);
        return 0;
    }
    clay_error_count = 0;
    width = measured_text_width("after-overfull", "hello world", 11);
    if (clay_error_count != 0 || width != 110.0f) {
        fprintf(stderr, "FAIL: after an overfull text, \"hello world\" measured %g px "
                        "with %d errors, not 110 px\n", (double)width, clay_error_count);
        return 0;
    }
    return 1;
}

/* A text that fills the word cache to its last slot measures, and is evicted
   once it has been gone a few layouts and a lookup walks its bucket. Its words
   are then free: a text in that bucket measures without an error, though the
   cache's slot array stays at its peak. */
static int word_cache_reuses_evicted_words(void)
{
    static char full[2 * 16384];
    static char probe[16];
    Clay_Context *context = Clay_GetCurrentContext();
    Clay_TextElementConfig config = { .fontSize = 10, .lineHeight = 10 };
    /* Text scanning needs two slots before each character. The last word
       uses one of those two slots after the loop. */
    int32_t words = context->measuredWords.capacity - 1;
    int32_t full_length;
    uint32_t buckets = (uint32_t)context->measureTextHashMap.capacity;
    uint32_t full_bucket;
    int32_t probe_length = 0;
    float width;

    /* Looking up the full text can evict older texts from its bucket. Start
       with an empty cache so that earlier layouts cannot change the count. */
    Clay_ResetMeasureTextCache();
    if (words < 2 || (size_t)words > sizeof(full) / 2) {
        fprintf(stderr, "FAIL: word cache capacity %d is outside the fixture's range\n",
                (int)context->measuredWords.capacity);
        return 0;
    }
    for (int32_t i = 0; i < words; ++i) {
        full[2 * i] = 'a';
        full[2 * i + 1] = ' ';
    }
    full_length = 2 * words - 1;
    /* A short text whose cache entry lands in the full text's bucket. */
    full_bucket = measure_cache_id(full, full_length, &config) % buckets;
    for (int n = 0; n < 100000 && probe_length == 0; ++n) {
        int length = snprintf(probe, sizeof(probe), "w%d", n);
        if (measure_cache_id(probe, length, &config) % buckets == full_bucket) {
            probe_length = length;
        }
    }
    if (probe_length == 0) {
        fprintf(stderr, "FAIL: no probe text shares the full text's cache bucket\n");
        return 0;
    }

    clay_error_count = 0;
    (void)measured_text_width("full", full, full_length);
    if (clay_error_count != 0 ||
        context->measuredWords.length != words || context->measuredWordsFreeList.length != 0) {
        fprintf(stderr, "FAIL: %d words filled %d slots with %d still free and %d errors\n",
                (int)words, (int)context->measuredWords.length,
                (int)context->measuredWordsFreeList.length, clay_error_count);
        return 0;
    }
    for (int frame = 0; frame < 3; ++frame) {
        Clay_BeginLayout();
        (void)Clay_EndLayout(0.0f);
    }
    width = measured_text_width("probe", probe, probe_length);
    if (clay_error_count != 0 || width != 10.0f * (float)probe_length) {
        fprintf(stderr, "FAIL: after the full text was evicted, \"%s\" measured %g px "
                        "with %d errors, expected %g px\n", probe, (double)width,
                clay_error_count, 10.0 * probe_length);
        return 0;
    }
    if (context->measuredWords.length != words || context->measuredWordsFreeList.length != words - 1) {
        fprintf(stderr, "FAIL: after reusing one word, the cache has %d slots and %d free "
                        "(expected %d and %d)\n",
                (int)context->measuredWords.length, (int)context->measuredWordsFreeList.length,
                (int)words, (int)(words - 1));
        return 0;
    }
    return 1;
}

/* Candidate texts for a measure-cache id collision: a number and a run of
   'x', so the candidates differ in length as well as content. */
#define COLLISION_CANDIDATES 1000000u
#define COLLISION_TEXT_BYTES 24

typedef struct {
    uint32_t id;
    uint32_t index;
} cache_id_entry;

static int compare_cache_ids(const void *left, const void *right)
{
    const cache_id_entry *a = left;
    const cache_id_entry *b = right;
    return a->id < b->id ? -1 : a->id > b->id ? 1 : 0;
}

static int collision_text(uint32_t index, char *out)
{
    int length = snprintf(out, COLLISION_TEXT_BYTES, "%u", index);
    for (uint32_t pad = 0; pad < index % 7u; ++pad) {
        out[length++] = 'x';
    }
    out[length] = '\0';
    return length;
}

/* The measure cache keys an entry by a 32-bit id. Two texts of different
   lengths whose ids collide, found by a birthday search, must each be measured
   as themselves: a hit on the other's entry hands one text the other's
   width. */
static int measure_cache_tells_colliding_texts_apart(void)
{
    static char first[COLLISION_TEXT_BYTES];
    static char second[COLLISION_TEXT_BYTES];
    Clay_TextElementConfig config = { .fontSize = 10, .lineHeight = 10 };
    cache_id_entry *entries = malloc(sizeof(*entries) * COLLISION_CANDIDATES);
    int first_length = 0;
    int second_length = 0;
    float first_width;
    float second_width;

    if (!entries) {
        fprintf(stderr, "FAIL: unable to allocate the collision search\n");
        return 0;
    }
    for (uint32_t i = 0; i < COLLISION_CANDIDATES; ++i) {
        char text[COLLISION_TEXT_BYTES];
        int length = collision_text(i, text);
        entries[i].id = measure_cache_id(text, length, &config);
        entries[i].index = i;
    }
    qsort(entries, COLLISION_CANDIDATES, sizeof(*entries), compare_cache_ids);
    for (uint32_t i = 1; i < COLLISION_CANDIDATES && first_length == 0; ++i) {
        if (entries[i].id != entries[i - 1].id) {
            continue;
        }
        first_length = collision_text(entries[i - 1].index, first);
        second_length = collision_text(entries[i].index, second);
        if (first_length == second_length) {
            first_length = 0;
        }
    }
    free(entries);
    if (first_length == 0) {
        fprintf(stderr, "FAIL: no measure-cache id collision between texts of different lengths "
                        "among %u candidates\n", COLLISION_CANDIDATES);
        return 0;
    }

    clay_error_count = 0;
    first_width = measured_text_width("collision-first", first, first_length);
    second_width = measured_text_width("collision-second", second, second_length);
    if (clay_error_count != 0 || first_width != 10.0f * (float)first_length ||
        second_width != 10.0f * (float)second_length) {
        fprintf(stderr, "FAIL: colliding texts \"%s\" and \"%s\" measured %g and %g px, "
                        "not %g and %g px\n", first, second, (double)first_width,
                (double)second_width, 10.0 * first_length, 10.0 * second_length);
        return 0;
    }
    return 1;
}

/* A word cache smaller than 32 words still measures text. The measure-text
   buckets are counted from the bucket array, not from the word count: a word
   count below 32 made upstream take each bucket modulo zero. */
static int small_word_cache_measures_text(void)
{
    Clay_Context *previous = Clay_GetCurrentContext();
    int32_t previous_words = Clay_GetMaxMeasureTextCacheWordCount();
    uint32_t arena_bytes = 0;
    unsigned char *block;
    Clay_Context *small_context;
    float width;
    int ok;

    /* A new context takes its counts from the current one. */
    Clay_SetMaxMeasureTextCacheWordCount(16);
    block = guarded_context(&arena_bytes);
    small_context = Clay_GetCurrentContext();
    Clay_SetCurrentContext(previous);
    Clay_SetMaxMeasureTextCacheWordCount(previous_words);
    if (!block) {
        return 0;
    }
    Clay_SetCurrentContext(small_context);
    clay_error_count = 0;
    width = measured_text_width("small-cache", "hello world", 11);
    ok = clay_error_count == 0 && width == 110.0f &&
         Clay_GetMaxMeasureTextCacheWordCount() == 16 && guard_intact(block, arena_bytes);
    if (!ok) {
        fprintf(stderr, "FAIL: with a 16-word cache, \"hello world\" measured %g px "
                        "with %d errors, not 110 px\n", (double)width, clay_error_count);
    }
    Clay_SetCurrentContext(previous);
    free(block);
    return ok;
}

/* A box sized as a percentage of its row's height sizes nothing in that row: the row is as
   tall as its other children, and the box as tall as the row, however much wrapped text the
   scroll column inside it holds (Settings > Enhanced's detail panel beside its list box, in
   src/player/ui/scr_settings.c). The column is clipped there and scrolls over its whole
   text: ten lines of 10 px, 100 px of content in 40 px. */
static int percent_height_box_follows_its_row(void)
{
    Clay_ElementData row;
    Clay_ElementData box;
    Clay_ElementData column;
    Clay_ScrollContainerData scroll;
    int ok = 1;

    clay_error_count = 0;
    for (int frame = 0; frame < 2; ++frame) {
        Clay_BeginLayout();
        CLAY(CLAY_ID("pct-row"), {
            .layout = { .layoutDirection = CLAY_LEFT_TO_RIGHT },
        }) {
            CLAY(CLAY_ID("pct-sibling"), {
                .layout = { .sizing = { .width = CLAY_SIZING_FIXED(20.0f),
                                        .height = CLAY_SIZING_FIXED(60.0f) } },
            }) {}
            CLAY(CLAY_ID("pct-box"), {
                .layout = { .sizing = { .width = CLAY_SIZING_FIXED(80.0f),
                                        .height = CLAY_SIZING_PERCENT(1.0f) },
                            .padding = { .left = 10, .right = 10, .top = 10, .bottom = 10 } },
            }) {
                CLAY(CLAY_ID("pct-column"), {
                    .layout = { .sizing = { .width = CLAY_SIZING_GROW(0),
                                            .height = CLAY_SIZING_GROW(0) },
                                .layoutDirection = CLAY_TOP_TO_BOTTOM },
                    .clip = { .vertical = true, .childOffset = Clay_GetScrollOffset() },
                }) {
                    CLAY_TEXT(CLAY_STRING("aaaa bbbb cccc dddd eeee ffff gggg hhhh iiii jjjj"),
                              CLAY_TEXT_CONFIG({ .fontSize = 10, .lineHeight = 10 }));
                }
            }
        }
        (void)Clay_EndLayout(0.0f);
    }
    row = Clay_GetElementData(CLAY_ID("pct-row"));
    box = Clay_GetElementData(CLAY_ID("pct-box"));
    column = Clay_GetElementData(CLAY_ID("pct-column"));
    scroll = Clay_GetScrollContainerData(CLAY_ID("pct-column"));
    if (clay_error_count != 0 || !row.found || !box.found || !column.found || !scroll.found ||
        row.boundingBox.height != 60.0f || box.boundingBox.height != 60.0f ||
        column.boundingBox.height != 40.0f || scroll.contentDimensions.height != 100.0f) {
        fprintf(stderr, "FAIL: a percent-height box beside a 60 px sibling: row %g px, box %g "
                        "px, its column %g px over %g px of text (want 60, 60, 40, 100), %d "
                        "errors\n",
                row.found ? (double)row.boundingBox.height : -1.0,
                box.found ? (double)box.boundingBox.height : -1.0,
                column.found ? (double)column.boundingBox.height : -1.0,
                scroll.found ? (double)scroll.contentDimensions.height : -1.0,
                clay_error_count);
        ok = 0;
    }
    return ok;
}

int main(void)
{
    uint32_t memory_size = Clay_MinMemorySize();
    void *memory = malloc(memory_size);
    Clay_Arena arena;
    Clay_Context *main_context;
    Clay_ErrorHandler error_handler = { record_clay_error, NULL };

    if (!memory) {
        fprintf(stderr, "FAIL: unable to allocate Clay arena (%u bytes)\n",
                (unsigned)memory_size);
        return EXIT_FAILURE;
    }

    arena = Clay_CreateArenaWithCapacityAndMemory(memory_size, memory);
    main_context = Clay_Initialize(arena, (Clay_Dimensions){ 120.0f, 80.0f },
                                   error_handler);
    if (!main_context) {
        fprintf(stderr, "FAIL: Clay initialization failed\n");
        free(memory);
        return EXIT_FAILURE;
    }
    Clay_SetMeasureTextFunction(measure_text, NULL);

    Clay_BeginLayout();
    CLAY(CLAY_ID("root"), {
        .layout = {
            .sizing = {
                .width = CLAY_SIZING_FIXED(120.0f),
                .height = CLAY_SIZING_FIXED(80.0f),
            },
            .padding = { .left = 10, .right = 10, .top = 10, .bottom = 10 },
            .childGap = 5,
            .layoutDirection = CLAY_LEFT_TO_RIGHT,
        },
    }) {
        CLAY(CLAY_ID("fixed-a"), {
            .layout = {
                .sizing = {
                    .width = CLAY_SIZING_FIXED(20.0f),
                    .height = CLAY_SIZING_FIXED(20.0f),
                },
            },
        }) {}
        CLAY(CLAY_ID("fixed-b"), {
            .layout = {
                .sizing = {
                    .width = CLAY_SIZING_FIXED(30.0f),
                    .height = CLAY_SIZING_FIXED(20.0f),
                },
            },
        }) {}
        CLAY(CLAY_ID("grow"), {
            .layout = {
                .sizing = {
                    .width = CLAY_SIZING_GROW(0),
                    .height = CLAY_SIZING_FIXED(20.0f),
                },
            },
        }) {}
    }
    (void)Clay_EndLayout(0.0f);

    if (clay_error_count != 0 ||
        !expect_rectangle("root", CLAY_ID("root"), 0.0f, 0.0f, 120.0f, 80.0f) ||
        !expect_rectangle("fixed-a", CLAY_ID("fixed-a"), 10.0f, 10.0f, 20.0f, 20.0f) ||
        !expect_rectangle("fixed-b", CLAY_ID("fixed-b"), 35.0f, 10.0f, 30.0f, 20.0f) ||
        !expect_rectangle("grow", CLAY_ID("grow"), 70.0f, 10.0f, 40.0f, 20.0f)) {
        free(memory);
        return EXIT_FAILURE;
    }

    /* An overfull column keeps a wrapped paragraph at its wrapped height. The
       four words wrap to four 10 px lines in the 50 px column; with the fixed
       box under them the content is 120 px in a 100 px column. Compressing the
       column must not squeeze the paragraph back toward one line, or the box
       paints over its later lines. */
    Clay_BeginLayout();
    CLAY(CLAY_ID("column"), {
        .layout = {
            .sizing = {
                .width = CLAY_SIZING_FIXED(50.0f),
                .height = CLAY_SIZING_FIXED(100.0f),
            },
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
        },
    }) {
        CLAY(CLAY_ID("paragraph"), {
            .layout = {
                .sizing = {
                    .width = CLAY_SIZING_GROW(0),
                    .height = CLAY_SIZING_FIT(0),
                },
            },
        }) {
            CLAY_TEXT(CLAY_STRING("aaaa bbbb cccc dddd"),
                      CLAY_TEXT_CONFIG({ .fontSize = 10, .lineHeight = 10,
                                         .wrapMode = CLAY_TEXT_WRAP_WORDS }));
        }
        CLAY(CLAY_ID("after"), {
            .layout = {
                .sizing = {
                    .width = CLAY_SIZING_FIXED(50.0f),
                    .height = CLAY_SIZING_FIXED(80.0f),
                },
            },
        }) {}
    }
    (void)Clay_EndLayout(0.0f);

    if (clay_error_count != 0 ||
        !expect_rectangle("paragraph", CLAY_ID("paragraph"), 0.0f, 0.0f, 50.0f, 40.0f) ||
        !expect_rectangle("after", CLAY_ID("after"), 0.0f, 40.0f, 50.0f, 80.0f)) {
        free(memory);
        return EXIT_FAILURE;
    }

    /* An arena request whose byte count passes 32 bits is refused. Formed in
       32 bits, 65536 items of 65536 bytes wrap to 0 and the request
       "succeeds" with no room behind it. */
    {
        char small[256];
        Clay_Arena tiny = Clay_CreateArenaWithCapacityAndMemory(sizeof(small), small);
        void *block = Clay__Array_Allocate_Arena(65536, 65536u, &tiny);

        if (block != NULL || clay_error_count != 1 ||
            clay_last_error != CLAY_ERROR_TYPE_ARENA_CAPACITY_EXCEEDED) {
            fprintf(stderr, "FAIL: a 4 GiB arena request was not refused\n");
            free(memory);
            return EXIT_FAILURE;
        }
    }

    /* An arena request for a negative count of items is refused and leaves
       the arena as it was. Formed in size_t, -1 items of 64 bytes are just
       under SIZE_MAX bytes, so adding them to the offset wraps below the
       capacity: the request would get memory already handed out and move the
       arena's next allocation backwards. */
    {
        char small[256];
        Clay_Arena tiny = Clay_CreateArenaWithCapacityAndMemory(sizeof(small), small);
        void *first = Clay__Array_Allocate_Arena(1, 128u, &tiny);
        void *block;

        clay_error_count = 0;
        block = Clay__Array_Allocate_Arena(-1, 64u, &tiny);
        if (first == NULL || block != NULL || tiny.nextAllocation != 128u ||
            clay_error_count != 1 || clay_last_error != CLAY_ERROR_TYPE_ARENA_CAPACITY_EXCEEDED) {
            fprintf(stderr, "FAIL: a request for -1 arena items was not refused "
                            "(next allocation at %u)\n", (unsigned)tiny.nextAllocation);
            free(memory);
            return EXIT_FAILURE;
        }
        clay_error_count = 1; /* as the 4 GiB request left it */
    }

    /* A layout past the element limit fills the element-id map. The next
       layout, back under the limit, reuses the slots pruning freed and lays
       out without an error. The full map is reported, not dropped in silence. */
    clay_error_count = 0;
    clay_errors_seen = 0u;
    Clay_BeginLayout();
    for (int i = 0; i < Clay_GetMaxElementCount() + 16; ++i) {
        CLAY(CLAY_IDI("crowd", (uint32_t)i), {
            .layout = { .sizing = { .width = CLAY_SIZING_FIXED(1.0f),
                                    .height = CLAY_SIZING_FIXED(1.0f) } },
        }) {}
    }
    (void)Clay_EndLayout(0.0f);
    if ((clay_errors_seen & (1u << (unsigned)CLAY_ERROR_TYPE_HASH_MAP_CAPACITY_EXCEEDED)) == 0u) {
        fprintf(stderr, "FAIL: a layout past the element limit did not report a full element-id "
                        "map (errors=%d, last=%d)\n", clay_error_count, (int)clay_last_error);
        free(memory);
        return EXIT_FAILURE;
    }
    clay_error_count = 0;
    Clay_BeginLayout();
    CLAY(CLAY_ID("after-crowd"), {
        .layout = { .sizing = { .width = CLAY_SIZING_FIXED(30.0f),
                                .height = CLAY_SIZING_FIXED(20.0f) } },
    }) {}
    (void)Clay_EndLayout(0.0f);
    if (clay_error_count != 0 ||
        !expect_rectangle("after-crowd", CLAY_ID("after-crowd"), 0.0f, 0.0f, 30.0f, 20.0f)) {
        fprintf(stderr, "FAIL: the layout after a full element-id map did not recover\n");
        free(memory);
        return EXIT_FAILURE;
    }

    /* A drag whose frames passed no time scrolls with the pointer and keeps no
       momentum when it is released. */
    scroll_frame(40.0f, false);
    scroll_frame(40.0f, true);
    scroll_frame(10.0f, true);
    scroll_frame(10.0f, false);
    scroll_frame(10.0f, false);
    {
        Clay_ScrollContainerData scroll = Clay_GetScrollContainerData(CLAY_ID("scroller"));
        if (clay_error_count != 0 || !scroll.found || scroll.scrollPosition->y != -30.0f) {
            fprintf(stderr, "FAIL: a timeless drag ended at %g, not at -30\n",
                    scroll.found ? (double)scroll.scrollPosition->y : 0.0);
            free(memory);
            return EXIT_FAILURE;
        }
    }

    if (!scroll_container_after_a_removed_one_takes_the_wheel()) {
        free(memory);
        return EXIT_FAILURE;
    }

    if (!newline_after_overlong_word("a long word and its space, then a newline",
                                     CLAY_STRING("abcdef \nnext"), 7) ||
        !newline_after_overlong_word("a long word, then a newline",
                                     CLAY_STRING("abcdefg\nnext"), 7)) {
        free(memory);
        return EXIT_FAILURE;
    }

    if (!wrapped_lines_fit_a_two_pass_layout() || !percent_height_box_follows_its_row()) {
        free(memory);
        return EXIT_FAILURE;
    }

    if (!measure_cache_tells_colliding_texts_apart() ||
        !word_cache_reuses_evicted_words() ||
        !word_cache_recovers_after_an_overfull_text()) {
        free(memory);
        return EXIT_FAILURE;
    }

    if (!debug_view_id_digits_stay_in_arena()) {
        free(memory);
        return EXIT_FAILURE;
    }
    Clay_SetCurrentContext(main_context);

    if (!small_word_cache_measures_text()) {
        free(memory);
        return EXIT_FAILURE;
    }

    free(memory);
    puts("Clay layout selftest passed: root, padding, child gap, fixed children, grow child, "
         "wrapped text kept whole in an overfull column, oversized and negative arena requests refused, "
         "a layout after a full element-id map, a timeless drag without momentum, "
         "the wheel on the scroll container after a removed one, a newline after an "
         "overlong word, wrapped lines for both passes of a layout with a transition, "
         "a percent-height box that follows its row and scrolls its text, "
         "colliding measure-cache ids told apart, evicted and overfull texts' words "
         "reused, the debug view's id digits kept inside the arena, and text measured "
         "with a word cache under 32 words");
    return EXIT_SUCCESS;
}

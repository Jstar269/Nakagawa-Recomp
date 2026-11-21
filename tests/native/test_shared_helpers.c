/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#define _POSIX_C_SOURCE 200809L

#include "nk_platform.h"
#include "nk_shared_helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SR_SELFTEST_BUILD_ROOT
#define SR_SELFTEST_BUILD_ROOT "build"
#endif

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#include <fcntl.h> /* _O_BINARY: the test pipe */
#include <io.h>    /* _pipe, _write, _close */
#else
#include <unistd.h>
#endif

static int failures;

static unsigned long test_process_id(void) {
#if defined(_WIN32) || defined(_WIN64)
    return (unsigned long)GetCurrentProcessId();
#else
    return (unsigned long)getpid();
#endif
}

static int remove_test_file(const char *path) {
#if defined(_WIN32) || defined(_WIN64)
    WCHAR wide_path[32768];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
                            wide_path, (int)(sizeof(wide_path) / sizeof(wide_path[0]))) <= 0) {
        return -1;
    }
    return DeleteFileW(wide_path) ? 0 : -1;
#else
    return remove(path);
#endif
}

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__); \
        failures++; \
    } \
} while (0)

/* With NK_TEST_WRAP_MALLOC the link wraps malloc (-Wl,--wrap=malloc: the shared-helpers-selftest
   recipe, and CMake where the linker has --wrap) so the bounded reader's allocation can fail:
   s_malloc_fail_at counts down the calls still to pass. */
#if defined(NK_TEST_WRAP_MALLOC)
void *__real_malloc(size_t size);
void *__wrap_malloc(size_t size);
static long s_malloc_fail_at = -1;
void *__wrap_malloc(size_t size) {
    if (s_malloc_fail_at >= 0 && s_malloc_fail_at-- == 0) return NULL;
    return __real_malloc(size);
}
#endif

static void emit_boot_event(FILE *stderr_stream, uint64_t timestamp_ns,
                            const char *format, ...) {
    va_list args;
    va_start(args, format);
    nk_shared_boot_event_vwrite(stderr_stream, timestamp_ns, format, args);
    va_end(args);
}

static int set_boot_event_path(const char *path) {
#if defined(_WIN32) || defined(_WIN64)
    return _putenv_s("SR_BOOT_EVENT_FILE", path);
#else
    return setenv("SR_BOOT_EVENT_FILE", path, 1);
#endif
}

static void clear_boot_event_path(void) {
#if defined(_WIN32) || defined(_WIN64)
    (void)_putenv_s("SR_BOOT_EVENT_FILE", "");
#else
    (void)unsetenv("SR_BOOT_EVENT_FILE");
#endif
}

static void test_path_joins(void) {
    char out[64];
    char small[6] = "xxxxx";

    CHECK(nk_shared_path_join(out, sizeof(out), "C:\\root\\", "leaf/sub", '\\',
                              NK_SHARED_PATH_JOIN_ALWAYS, 0u),
          "always-separator join fits");
    CHECK(strcmp(out, "C:\\root\\\\leaf/sub") == 0,
          "always-separator join preserves duplicate separator and suffix bytes");

    CHECK(nk_shared_path_join(out, sizeof(out), "C:/root/", "leaf", '\\',
                              NK_SHARED_PATH_JOIN_IF_LEFT_NEEDS_SEPARATOR, 0u),
          "if-needed join fits");
    CHECK(strcmp(out, "C:/root/leaf") == 0,
          "if-needed join preserves either existing separator");

    CHECK(nk_shared_path_join(out, sizeof(out), "left/", "right", '/',
                              NK_SHARED_PATH_JOIN_IF_BOTH_NONEMPTY, 0u),
          "allocated-join mode fits in bounded output");
    CHECK(strcmp(out, "left/right") == 0, "if-both mode avoids duplicate separator");
    CHECK(nk_shared_path_join(out, sizeof(out), "", "leaf", '/',
                              NK_SHARED_PATH_JOIN_IF_BOTH_NONEMPTY, 0u),
          "empty left side is accepted by if-both mode");
    CHECK(strcmp(out, "leaf") == 0, "if-both mode adds no leading separator");

    CHECK(nk_shared_path_join(out, sizeof(out), "root", "a/b", '\\',
                              NK_SHARED_PATH_JOIN_ALWAYS,
                              NK_SHARED_PATH_NORMALIZE_FORWARD_SLASHES),
          "forward-slash normalization fits");
    CHECK(strcmp(out, "root\\a\\b") == 0, "forward slash suffix is normalized");
    CHECK(nk_shared_path_join(out, sizeof(out), "C:/root//", "folder/name", '\\',
                              NK_SHARED_PATH_JOIN_ALWAYS,
                              NK_SHARED_PATH_TRIM_LEFT_SEPARATORS |
                                  NK_SHARED_PATH_NORMALIZE_BOTH_SLASHES),
          "trim and normalize join fits");
    CHECK(strcmp(out, "C:/root\\folder\\name") == 0,
          "trim mode removes all root separators and normalizes both suffix separators");

    CHECK(!nk_shared_path_join(small, sizeof(small), "root", "part", '/',
                               NK_SHARED_PATH_JOIN_ALWAYS, 0u),
          "ordinary bounded join reports truncation");
    CHECK(strcmp(small, "root/") == 0, "ordinary overflow keeps snprintf-style prefix");
    memset(small, 'z', sizeof(small));
    CHECK(!nk_shared_path_join(small, sizeof(small), "root", "part", '/',
                               NK_SHARED_PATH_JOIN_ALWAYS, NK_SHARED_PATH_REQUIRE_FIT),
          "strict bounded join reports overlong input");
    CHECK(small[0] == 'z' && small[sizeof(small) - 1u] == 'z',
          "strict bounded overflow leaves destination untouched");

    memset(out, 'q', sizeof(out));
    CHECK(!nk_shared_path_join(out, sizeof(out), "root", "C:\\absolute", '/',
                               NK_SHARED_PATH_JOIN_ALWAYS,
                               NK_SHARED_PATH_REJECT_ABSOLUTE_RIGHT),
          "absolute right side is rejected when requested");
    CHECK(out[0] == 'q', "rejected right side leaves destination untouched");

    char *allocated = nk_shared_path_join_alloc("folder/", "child", '/',
                                                 NK_SHARED_PATH_JOIN_IF_BOTH_NONEMPTY, 0u);
    CHECK(allocated != NULL && strcmp(allocated, "folder/child") == 0,
          "allocated join matches if-both bounded semantics");
    free(allocated);
    allocated = nk_shared_path_join_alloc("", "child", '/',
                                           NK_SHARED_PATH_JOIN_ALWAYS, 0u);
    CHECK(allocated != NULL && strcmp(allocated, "/child") == 0,
          "allocated always mode retains leading separator for empty left side");
    free(allocated);
}

/* Pin the distinct legacy contracts of the callers routed through these join modes. */
static void test_legacy_path_contracts(void) {
    char out[64];
    char *allocated;

    CHECK(nk_shared_path_join(out, sizeof(out), "", "leaf", '\\',
                              NK_SHARED_PATH_JOIN_IF_LEFT_NEEDS_SEPARATOR,
                              NK_SHARED_PATH_REQUIRE_POSITIVE_LENGTH),
          "UI join accepts an empty root when the relative path is nonempty");
    CHECK(strcmp(out, "\\leaf") == 0, "UI join keeps the separator after an empty root");
    CHECK(nk_shared_path_join(out, sizeof(out), "", "", '\\',
                              NK_SHARED_PATH_JOIN_IF_LEFT_NEEDS_SEPARATOR,
                              NK_SHARED_PATH_REQUIRE_POSITIVE_LENGTH) &&
              strcmp(out, "\\") == 0,
          "UI join of two empty parts is the lone separator, as the old snprintf wrote");
    CHECK(nk_shared_path_join(out, sizeof(out), "root/", "", '\\',
                              NK_SHARED_PATH_JOIN_IF_LEFT_NEEDS_SEPARATOR,
                              NK_SHARED_PATH_REQUIRE_POSITIVE_LENGTH) &&
              strcmp(out, "root/") == 0,
          "UI join permits an empty relative path when root makes the result nonempty");

    CHECK(nk_shared_path_join(out, sizeof(out), "C:\\root///", "folder/name", '\\',
                              NK_SHARED_PATH_JOIN_ALWAYS,
                              NK_SHARED_PATH_TRIM_LEFT_SEPARATORS),
          "staging path trims root separators without normalizing the suffix");
    CHECK(strcmp(out, "C:\\root\\folder/name") == 0,
          "staging path preserves relative forward slashes");

    allocated = nk_shared_path_join_alloc("left\\", "right", '/',
                                           NK_SHARED_PATH_JOIN_IF_BOTH_NONEMPTY, 0u);
    CHECK(allocated != NULL && strcmp(allocated, "left\\right") == 0,
          "runtime join preserves either trailing separator form");
    free(allocated);
    allocated = nk_shared_path_join_alloc("left", "", '/',
                                           NK_SHARED_PATH_JOIN_IF_BOTH_NONEMPTY, 0u);
    CHECK(allocated != NULL && strcmp(allocated, "left") == 0,
          "runtime join adds a separator only when both parts are nonempty");
    free(allocated);
    allocated = nk_shared_path_join_alloc("folder/", "", '/', NK_SHARED_PATH_JOIN_ALWAYS, 0u);
    CHECK(allocated != NULL && strcmp(allocated, "folder//") == 0,
          "package source join always adds a separator, including for an empty right side");
    free(allocated);

    memset(out, 'q', sizeof(out));
    CHECK(!nk_shared_path_join(out, sizeof(out), "root", "/absolute", '/',
                               NK_SHARED_PATH_JOIN_ALWAYS,
                               NK_SHARED_PATH_REJECT_ABSOLUTE_RIGHT) && out[0] == 'q',
          "launch join rejects slash-rooted paths without writing output");
    CHECK(!nk_shared_path_join(out, sizeof(out), "root", "\\absolute", '/',
                               NK_SHARED_PATH_JOIN_ALWAYS,
                               NK_SHARED_PATH_REJECT_ABSOLUTE_RIGHT) && out[0] == 'q',
          "launch join rejects backslash-rooted paths without writing output");
    CHECK(!nk_shared_path_join(out, sizeof(out), "root", "C:relative", '/',
                               NK_SHARED_PATH_JOIN_ALWAYS,
                               NK_SHARED_PATH_REJECT_ABSOLUTE_RIGHT) && out[0] == 'q',
          "launch join rejects drive-prefixed paths without writing output");
}

static void test_text_copy(void) {
    char truncated[4] = { 'x', 'x', 'x', 'x' };
    CHECK(nk_shared_text_copy(truncated, sizeof(truncated), "long text",
                              NK_SHARED_TEXT_COPY_TRUNCATE),
          "truncating text copy accepts overlong input");
    CHECK(strcmp(truncated, "lon") == 0, "truncating text copy terminates at capacity");

    char padded[6];
    memset(padded, 'x', sizeof(padded));
    CHECK(nk_shared_text_copy(padded, sizeof(padded), "ab",
                              NK_SHARED_TEXT_COPY_TRUNCATE),
          "truncating copy accepts short text");
    CHECK(padded[0] == 'a' && padded[1] == 'b' && padded[2] == '\0' &&
              padded[3] == '\0' && padded[4] == '\0' && padded[5] == '\0',
          "truncating copy preserves strncpy NUL padding");
    CHECK(nk_shared_text_copy(padded, sizeof(padded), NULL,
                              NK_SHARED_TEXT_COPY_TRUNCATE) && padded[0] == '\0',
          "truncating copy maps NULL source to empty text");

    char strict[4] = { 's', 'a', 'f', 'e' };
    CHECK(!nk_shared_text_copy(strict, sizeof(strict), "long", NK_SHARED_TEXT_COPY_REQUIRE_FIT),
          "strict copy rejects overlong input");
    CHECK(memcmp(strict, "safe", sizeof(strict)) == 0,
          "strict copy leaves output unchanged when overlong");
    CHECK(nk_shared_text_copy(strict, sizeof(strict), "ok", NK_SHARED_TEXT_COPY_REQUIRE_FIT) &&
              strcmp(strict, "ok") == 0,
          "strict copy writes a fitting string");
}

static void test_utf8_open(void) {
    char path[4096];
    int written = snprintf(path, sizeof(path), "%s/shared-helper-%lu-\xC3\xA9.tmp",
                           SR_SELFTEST_BUILD_ROOT, test_process_id());
    CHECK(written > 0 && (size_t)written < sizeof(path), "UTF-8 test path formats");
    if (written <= 0 || (size_t)written >= sizeof(path)) return;

    FILE *file = nk_fopen_utf8(path, "wb");
    CHECK(file != NULL, "UTF-8 open creates a non-ASCII path");
    if (file) {
        CHECK(fputc('U', file) == 'U', "UTF-8 open writes test byte");
        fclose(file);
    }
    file = nk_fopen_utf8(path, "rb");
    CHECK(file != NULL, "UTF-8 open reads the non-ASCII path");
    if (file) {
        CHECK(fgetc(file) == 'U', "UTF-8 open reads written test byte");
        fclose(file);
    }
    CHECK(remove_test_file(path) == 0, "UTF-8 path cleanup succeeds");
    CHECK(nk_fopen_utf8(NULL, "rb") == NULL && nk_fopen_utf8(path, NULL) == NULL,
          "UTF-8 open rejects NULL arguments");
    CHECK(nk_fopen_utf8("", "rb") == NULL,
          "UTF-8 open preserves empty-path failure");

    char overlong[32770];
    memset(overlong, 'x', sizeof(overlong) - 1u);
    overlong[sizeof(overlong) - 1u] = '\0';
    CHECK(nk_fopen_utf8(overlong, "rb") == NULL,
          "UTF-8 open rejects a path longer than the conversion/input limit");
}

static void test_size_format(void) {
    char out[32];
    nk_shared_format_size(out, sizeof(out), 0u);
    CHECK(strcmp(out, "0 bytes") == 0, "sub-MiB size uses byte label");
    nk_shared_format_size(out, sizeof(out), 1024ULL * 1024ULL);
    CHECK(strcmp(out, "1.00 MB") == 0, "MiB threshold formats two decimals");
    nk_shared_format_size(out, sizeof(out), 1024ULL * 1024ULL * 1024ULL);
    CHECK(strcmp(out, "1.00 GB") == 0, "GiB threshold formats two decimals");
    char short_out[5];
    nk_shared_format_size(short_out, sizeof(short_out), 1024ULL * 1024ULL);
    CHECK(strcmp(short_out, "1.00") == 0, "size formatting preserves snprintf truncation");
}

static void test_boot_events(void) {
    char path[4096];
    int written = snprintf(path, sizeof(path), "%s/shared-boot-%lu.events",
                           SR_SELFTEST_BUILD_ROOT, test_process_id());
    CHECK(written > 0 && (size_t)written < sizeof(path), "boot event test path formats");
    if (written <= 0 || (size_t)written >= sizeof(path)) return;
    (void)remove_test_file(path);

    FILE *console = tmpfile();
    CHECK(console != NULL, "temporary stderr capture opens");
    if (!console) return;

    clear_boot_event_path();
    emit_boot_event(console, 0u, "event=stderr-only");
    CHECK(set_boot_event_path(path) == 0, "boot event output path is set");
    emit_boot_event(console, 17u, "event=%s", "ready");
    emit_boot_event(console, 0u, "event=untimed");
    char long_message[2049];
    memset(long_message, 'L', sizeof(long_message) - 1u);
    long_message[sizeof(long_message) - 1u] = '\0';
    emit_boot_event(console, 0u, "%s", long_message);
    rewind(console);
    char console_contents[2300];
    size_t console_count = fread(console_contents, 1u, sizeof(console_contents) - 1u, console);
    console_contents[console_count] = '\0';
    CHECK(strstr(console_contents,
                 "event=stderr-only\nevent=ready t_ns=17\nevent=untimed\n") != NULL,
          "boot event writer preserves stderr records and timestamps");
    CHECK(strlen(console_contents) ==
              strlen("event=stderr-only\nevent=ready t_ns=17\nevent=untimed\n") +
                  strlen(long_message) + 1u,
          "boot event writer does not truncate stderr records");
    fclose(console);

    char contents[2300];
    FILE *file = nk_fopen_utf8(path, "rb");
    CHECK(file != NULL, "boot event writer creates append file");
    if (file) {
        size_t count = fread(contents, 1u, sizeof(contents) - 1u, file);
        contents[count] = '\0';
        fclose(file);
        CHECK(strstr(contents, "event=ready t_ns=17\nevent=untimed\n") != NULL,
              "boot event writer preserves timestamp and untimed record formats");
        CHECK(strlen(contents) == strlen("event=ready t_ns=17\nevent=untimed\n") +
                                  strlen(long_message) + 1u,
              "boot event writer does not truncate an overlong record");
    }
    CHECK(remove_test_file(path) == 0, "boot event test file cleanup succeeds");
    clear_boot_event_path();

#if defined(_WIN32) || defined(_WIN64)
    /* The player passes the path in a Unicode environment. A character outside the ANSI code
       page (a Cyrillic letter here) must survive into the opened file name. */
    char unicode_path[4096];
    WCHAR wide_unicode_path[4096];
    written = snprintf(unicode_path, sizeof(unicode_path), "%s/shared-boot-%lu-\xD0\x96.events",
                       SR_SELFTEST_BUILD_ROOT, test_process_id());
    CHECK(written > 0 && (size_t)written < sizeof(unicode_path),
          "non-ANSI boot event test path formats");
    CHECK(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, unicode_path, -1,
                              wide_unicode_path,
                              (int)(sizeof(wide_unicode_path) / sizeof(wide_unicode_path[0]))) > 0,
          "non-ANSI boot event test path converts to UTF-16");
    (void)remove_test_file(unicode_path);
    CHECK(_wputenv_s(L"SR_BOOT_EVENT_FILE", wide_unicode_path) == 0,
          "non-ANSI boot event output path is set");
    FILE *unicode_console = tmpfile();
    CHECK(unicode_console != NULL, "temporary stderr capture opens for the non-ANSI path");
    if (unicode_console) {
        emit_boot_event(unicode_console, 0u, "event=unicode-path");
        fclose(unicode_console);
    }
    file = nk_fopen_utf8(unicode_path, "rb");
    CHECK(file != NULL, "boot event writer creates a file at a non-ANSI path");
    if (file) {
        size_t count = fread(contents, 1u, sizeof(contents) - 1u, file);
        contents[count] = '\0';
        fclose(file);
        CHECK(strcmp(contents, "event=unicode-path\n") == 0,
              "boot event writer appends the record at a non-ANSI path");
        CHECK(remove_test_file(unicode_path) == 0, "non-ANSI boot event file cleanup succeeds");
    }
    clear_boot_event_path();
#endif

    char formatted[128];
    int path_length = nk_shared_boot_event_format_path(formatted, sizeof(formatted),
                                                       "cache/", '/', 42ul, 7u);
    CHECK(path_length > 0 && strcmp(formatted, "cache//player-boot-42-7.events") == 0,
          "boot path formatter appends separator without trimming");
    char short_path[8] = "oldpath";
    CHECK(nk_shared_boot_event_format_path(short_path, sizeof(short_path), "long cache", '/',
                                           42ul, 7u) == 0 && short_path[0] == '\0',
          "boot path formatter clears an overlong output");
    CHECK(nk_shared_boot_event_format_path(short_path, sizeof(short_path), NULL, '/',
                                           42ul, 7u) == 0 && short_path[0] == '\0',
          "boot path formatter clears output when the cache directory is unavailable");
}

/* #2151: each start-up outcome gives its path, and the event line the runtime writes reads
   back as that path in the player. */
static void test_render_path(void) {
    static const struct {
        bool vulkan_asked, vulkan_started, gpu_ge_asked, gpu_ge_started;
        bool vulkan, gpu_ge;
        NkRenderFallback fallback;
        const char *line;
    } cases[] = {
        /* What the player launches with: Vulkan and the GPU GE renderer, both started. */
        {true, true, true, true, true, true, NK_RENDER_FALLBACK_NONE,
         "BOOT_EVENT phase=render_path presenter=vulkan ge=gpu fallback=none"},
        /* Vulkan did not start: the GDI window, and so the software GE. */
        {true, false, true, false, false, false, NK_RENDER_FALLBACK_GDI_PRESENTER,
         "BOOT_EVENT phase=render_path presenter=gdi ge=software fallback=gdi_presenter"},
        {true, false, false, false, false, false, NK_RENDER_FALLBACK_GDI_PRESENTER,
         "BOOT_EVENT phase=render_path presenter=gdi ge=software fallback=gdi_presenter"},
        /* The GPU GE renderer did not start in the Vulkan window. */
        {true, true, true, false, true, false, NK_RENDER_FALLBACK_SOFTWARE_GE,
         "BOOT_EVENT phase=render_path presenter=vulkan ge=software fallback=software_ge"},
        /* Not asked for, so no fallback: SR_GPU_GE unset, or SR_VIDEO=gdi. */
        {true, true, false, false, true, false, NK_RENDER_FALLBACK_NONE,
         "BOOT_EVENT phase=render_path presenter=vulkan ge=software fallback=none"},
        {false, false, true, false, false, false, NK_RENDER_FALLBACK_NONE,
         "BOOT_EVENT phase=render_path presenter=gdi ge=software fallback=none"},
    };
    static const char *const refused[] = {
        "BOOT_EVENT phase=window_ready backend=gdi",
        "BOOT_EVENT phase=render_path presenter=metal ge=gpu fallback=none",
        "BOOT_EVENT phase=render_path presenter=vulkan ge=gpu fallback=slow",
        "BOOT_EVENT phase=render_path presenter=vulkan ge=gpu",
        "BOOT_EVENT phase=render_path presenter=vulkan ge=gpux fallback=none",
        "stderr: BOOT_EVENT phase=render_path presenter=vulkan ge=gpu fallback=none",
    };
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        NkRenderPath path = nk_render_path_from_init(cases[i].vulkan_asked,
                                                     cases[i].vulkan_started,
                                                     cases[i].gpu_ge_asked,
                                                     cases[i].gpu_ge_started);
        NkRenderPath read = {false, false, NK_RENDER_FALLBACK_COUNT};
        char line[128];
        char timed[160];
        CHECK(path.vulkan == cases[i].vulkan && path.gpu_ge == cases[i].gpu_ge &&
                  path.fallback == cases[i].fallback,
              "render path follows what was asked for and what started");
        CHECK(nk_render_path_event_format(&path, line, sizeof(line)) ==
                      (int)strlen(cases[i].line) &&
                  strcmp(line, cases[i].line) == 0,
              "render path event names the presenter, the GE path and the fallback");
        (void)snprintf(timed, sizeof(timed), "%s t_ns=123\r\n", line);
        CHECK(nk_render_path_event_parse(timed, &read) && read.vulkan == path.vulkan &&
                  read.gpu_ge == path.gpu_ge && read.fallback == path.fallback,
              "render path event reads back as the path, timestamp and line end included");
    }
    for (size_t i = 0u; i < sizeof(refused) / sizeof(refused[0]); ++i) {
        NkRenderPath read = {true, true, NK_RENDER_FALLBACK_SOFTWARE_GE};
        CHECK(!nk_render_path_event_parse(refused[i], &read) && read.vulkan && read.gpu_ge &&
                  read.fallback == NK_RENDER_FALLBACK_SOFTWARE_GE,
              "another line or an unknown value is not a render path");
    }
    {
        NkRenderPath path = {true, true, NK_RENDER_FALLBACK_NONE};
        char small[16] = "old";
        CHECK(nk_render_path_event_format(&path, small, sizeof(small)) == 0 && small[0] == '\0',
              "render path event formatter clears an output too small for it");
        CHECK(nk_render_fallback_name(NK_RENDER_FALLBACK_COUNT) == NULL,
              "an out-of-range fallback has no name");
    }
}

/* The render_fallback event (#2151): every reason's name, a round trip through the line the
 * runtime writes, and the lines the player must not take for one. */
static void test_render_fallback_event(void) {
    static const char *const names[NK_RENDER_EFFECT_COUNT] = {
        "stencil", "color_test", "logic_op", "write_mask", "ram_fb", "wide_stride", "mips",
        "line", "point"
    };
    static const char *const refused[] = {
        "BOOT_EVENT phase=render_path presenter=vulkan ge=gpu fallback=none",
        "BOOT_EVENT phase=render_fallback reason=fog frames=360 sw_prims=1 gpu_prims=2 "
        "readbacks=0 share_pm=5",
        "BOOT_EVENT phase=render_fallback reason=stencil frames=360 sw_prims=1 gpu_prims=2 "
        "readbacks=0",
        "BOOT_EVENT phase=render_fallback reason=stencil frames=-1 sw_prims=1 gpu_prims=2 "
        "readbacks=0 share_pm=5",
        "BOOT_EVENT phase=render_fallback reason=stencil frames=4294967296 sw_prims=1 "
        "gpu_prims=2 readbacks=0 share_pm=5",
        "BOOT_EVENT phase=render_fallback reason=stencil frames=360 sw_prims=1 gpu_prims=2 "
        "readbacks=0 share_pm=1001",
        "BOOT_EVENT phase=render_fallback reason=stencil frames=360 sw_prims= gpu_prims=2 "
        "readbacks=0 share_pm=5",
        "BOOT_EVENT phase=render_fallback reason=stencil frames=360 sw_prims=18446744073709551616 "
        "gpu_prims=2 readbacks=0 share_pm=5",
        "stderr: BOOT_EVENT phase=render_fallback reason=stencil frames=360 sw_prims=1 "
        "gpu_prims=2 readbacks=0 share_pm=5",
    };
    for (int i = 0; i < NK_RENDER_EFFECT_COUNT; ++i) {
        NkRenderFallbackEvent event = {(NkRenderEffect)i, 360u, 18446744073709551615ull,
                                       123456u, (uint64_t)i, 1000u};
        NkRenderFallbackEvent read;
        char line[256];
        char want[256];
        char timed[300];
        CHECK(nk_render_effect_name((NkRenderEffect)i) != NULL &&
                  strcmp(nk_render_effect_name((NkRenderEffect)i), names[i]) == 0,
              "each fallback reason has its event name");
        (void)snprintf(want, sizeof(want),
                       "BOOT_EVENT phase=render_fallback reason=%s frames=360 "
                       "sw_prims=18446744073709551615 gpu_prims=123456 readbacks=%d "
                       "share_pm=1000",
                       names[i], i);
        CHECK(nk_render_fallback_event_format(&event, line, sizeof(line)) ==
                      (int)strlen(want) &&
                  strcmp(line, want) == 0,
              "render fallback event names the reason and its counts");
        (void)snprintf(timed, sizeof(timed), "%s t_ns=99\r\n", line);
        memset(&read, 0, sizeof(read));
        CHECK(nk_render_fallback_event_parse(timed, &read) && read.reason == event.reason &&
                  read.frames == event.frames && read.sw_prims == event.sw_prims &&
                  read.gpu_prims == event.gpu_prims && read.readbacks == event.readbacks &&
                  read.share_pm == event.share_pm,
              "render fallback event reads back, timestamp and line end included");
    }
    for (size_t i = 0u; i < sizeof(refused) / sizeof(refused[0]); ++i) {
        NkRenderFallbackEvent read = {NK_RENDER_EFFECT_POINT, 7u, 7u, 7u, 7u, 7u};
        CHECK(!nk_render_fallback_event_parse(refused[i], &read) &&
                  read.reason == NK_RENDER_EFFECT_POINT && read.frames == 7u &&
                  read.share_pm == 7u,
              "another line, an unknown reason or a bad number is not a render fallback");
    }
    {
        NkRenderFallbackEvent event = {NK_RENDER_EFFECT_STENCIL, 360u, 1u, 2u, 0u, 5u};
        NkRenderFallbackEvent bad = {NK_RENDER_EFFECT_COUNT, 360u, 1u, 2u, 0u, 5u};
        char small[24] = "old";
        char line[256] = "old";
        CHECK(nk_render_fallback_event_format(&event, small, sizeof(small)) == 0 &&
                  small[0] == '\0',
              "render fallback formatter clears an output too small for it");
        CHECK(nk_render_fallback_event_format(&bad, line, sizeof(line)) == 0 && line[0] == '\0',
              "render fallback formatter refuses an unknown reason");
        CHECK(nk_render_effect_name(NK_RENDER_EFFECT_COUNT) == NULL,
              "an out-of-range reason has no name");
    }
}

/* #2151: the rule nk_render_fallback_window_step keeps, on its own counts. */
static uint16_t window_frames(NkRenderFallbackWindow *w, NkRenderFallbackCounts *c, int r,
                              uint32_t gpu, uint64_t sw, uint64_t rb, uint32_t presents) {
    uint16_t crossed = 0u;
    for (uint32_t f = 0u; f < presents; ++f) {
        if (f == 0u) {
            c->gpu_prims += gpu;
            c->sw_prims[r] += sw;
            c->readbacks[r] += rb;
        }
        crossed |= nk_render_fallback_window_step(w, c, NULL);
    }
    return crossed;
}

static void test_render_fallback_window(void) {
    const uint32_t W = NK_RENDER_FALLBACK_WINDOW_FRAMES, K = NK_RENDER_FALLBACK_WINDOWS;
    const uint16_t ram_fb = (uint16_t)(1u << NK_RENDER_EFFECT_RAM_FB);
    NkRenderFallbackWindow w;
    NkRenderFallbackCounts c;
    NkRenderFallbackDelta d;
    memset(&w, 0, sizeof(w));
    memset(&c, 0, sizeof(c));
    c.ready = 1;
    c.gpu_prims = 0xFFFFFFF0u;
    CHECK(nk_render_fallback_window_step(&w, &c, &d) == 0u && w.primed,
          "the first sample only primes the window");
    /* A present's share, the GPU count wrapping. */
    c.gpu_prims += 0x20u;
    c.sw_prims[NK_RENDER_EFFECT_LINE] += 3u;
    c.readbacks[NK_RENDER_EFFECT_LINE] += 1u;
    CHECK(nk_render_fallback_window_step(&w, &c, &d) == 0u && d.gpu_prims == 0x20u &&
              d.sw_prims[NK_RENDER_EFFECT_LINE] == 3u && d.readbacks[NK_RENDER_EFFECT_LINE] == 1u,
          "a present's delta carries its counts across the GPU count's wrap");
    memset(&w, 0, sizeof(w));
    (void)nk_render_fallback_window_step(&w, &c, NULL);
    /* 90 software primitives a present in 10,000 GPU ones, no readback: under 1 % of the scene,
       over the primitive limit, reported on the K-th window with its share. */
    uint16_t crossed = 0u;
    for (uint32_t i = 0u; i + 1u < K; ++i)
        crossed |= window_frames(&w, &c, NK_RENDER_EFFECT_RAM_FB, 10000u * W, 90u * W, 0u, W);
    CHECK(crossed == 0u, "nothing is reported before the K-th window");
    crossed = window_frames(&w, &c, NK_RENDER_EFFECT_RAM_FB, 10000u * W, 90u * W, 0u, W);
    NkRenderFallbackEvent e = nk_render_fallback_window_event(&w, NK_RENDER_EFFECT_RAM_FB);
    CHECK(crossed == ram_fb && e.frames == K * W && e.sw_prims == 90ull * W * K &&
              e.readbacks == 0u && e.share_pm == 8u,
          "the primitive limit decides on its own, whatever the share");
    CHECK(window_frames(&w, &c, NK_RENDER_EFFECT_RAM_FB, 10000u * W, 90u * W, 0u, K * W) == 0u,
          "a reason is reported once per run");
    /* Counts going back start the window over; the run's reports are kept. */
    memset(&c, 0, sizeof(c));
    c.ready = 1;
    CHECK(nk_render_fallback_window_step(&w, &c, &d) == 0u && w.frames == 0u &&
              w.reported == ram_fb && d.gpu_prims == 0u,
          "counts that started over restart the window, not a negative one");
    /* Readbacks alone, one short and then at the limit. */
    crossed = 0u;
    for (uint32_t i = 0u; i < 2u * K; ++i)
        crossed |= window_frames(&w, &c, NK_RENDER_EFFECT_STENCIL, 1000000u, 0u,
                                 NK_RENDER_FALLBACK_READBACKS - 1u, W);
    CHECK(crossed == 0u, "one readback under the limit is never reported");
    for (uint32_t i = 0u; i < K; ++i)
        crossed |= window_frames(&w, &c, NK_RENDER_EFFECT_STENCIL, 1000000u, 0u,
                                 NK_RENDER_FALLBACK_READBACKS, W);
    CHECK(crossed == (uint16_t)(1u << NK_RENDER_EFFECT_STENCIL),
          "readbacks at the limit are reported");
    c.ready = 0;
    CHECK(nk_render_fallback_window_step(&w, &c, &d) == 0u && !w.primed,
          "a renderer that is not running unprimes the window");

    /* One cost budget per effect: readbacks and primitives add up; each kind alone still trips
       it at its old limit, and one short of it does not. */
    const uint64_t P = NK_RENDER_FALLBACK_MIN_PRIMS, B = NK_RENDER_FALLBACK_READBACKS;
    CHECK(nk_render_fallback_cost_us(P, 0u) == NK_RENDER_FALLBACK_BUDGET_US &&
              nk_render_fallback_cost_us(0u, B) == NK_RENDER_FALLBACK_BUDGET_US &&
              nk_render_fallback_cost_us(P - 1u, 0u) < NK_RENDER_FALLBACK_BUDGET_US &&
              nk_render_fallback_cost_us(0u, B - 1u) < NK_RENDER_FALLBACK_BUDGET_US &&
              nk_render_fallback_cost_us(P - 1u, B - 1u) == 95976u + 92800u,
          "the cost adds readbacks and primitives at their per-unit costs");
    CHECK(nk_render_fallback_cost_us(0u, UINT64_MAX) == UINT64_MAX &&
              nk_render_fallback_cost_us(UINT64_MAX, 0u) == UINT64_MAX &&
              nk_render_fallback_cost_us(UINT64_MAX / NK_RENDER_FALLBACK_PRIM_US, 1u) == UINT64_MAX,
          "a cost too large for 64 bits is the largest one, never a wrapped small one");
    const NkRenderEffect each[3] = {NK_RENDER_EFFECT_STENCIL, NK_RENDER_EFFECT_LINE,
                                    NK_RENDER_EFFECT_COLOR_TEST};
    const uint64_t sw_of[3] = {P - 1u, P, 0u}, rb_of[3] = {B - 1u, 0u, B};
    for (int k = 0; k < 3; ++k) {
        memset(&w, 0, sizeof(w));
        c.ready = 1;
        (void)nk_render_fallback_window_step(&w, &c, NULL);
        crossed = 0u;
        for (uint32_t i = 0u; i < K; ++i)
            crossed |= window_frames(&w, &c, each[k], 50000u, sw_of[k], rb_of[k], W);
        CHECK(crossed == (uint16_t)(1u << each[k]),
              "one short of each limit together, or either limit alone, is reported");
    }
    /* Two effects each under the budget are not reported, whatever their sum: the budget is per
       effect, so a report names an effect that costs it alone. */
    memset(&w, 0, sizeof(w));
    (void)nk_render_fallback_window_step(&w, &c, NULL);
    crossed = 0u;
    for (uint32_t i = 0u; i < 2u * K; ++i) {
        c.sw_prims[NK_RENDER_EFFECT_STENCIL] += 3000u;
        crossed |= window_frames(&w, &c, NK_RENDER_EFFECT_COLOR_TEST, 50000u, 3000u, 0u, W);
    }
    CHECK(crossed == 0u, "costs of different effects do not add up");
    /* One cheap primitive a present for ever, with no readback (a frame buffer in main memory):
       120 x 24 us a window, never reported. */
    memset(&w, 0, sizeof(w));
    (void)nk_render_fallback_window_step(&w, &c, NULL);
    crossed = 0u;
    for (uint32_t f = 0u; f < 20u * K * W; ++f) {
        c.gpu_prims += 3000u;
        c.sw_prims[NK_RENDER_EFFECT_RAM_FB] += 1u;
        crossed |= nk_render_fallback_window_step(&w, &c, NULL);
    }
    CHECK(crossed == 0u, "one cheap software primitive a present is never reported");
}

/* The one SHA-256 the player and the runtime share (the disc, package and font identities),
   against the FIPS 180-2 examples, the last fed in uneven pieces across block boundaries. */
static void sha256_hex(const uint8_t *data, size_t length, size_t piece, char out[65]) {
    NkSha256 sha;
    uint8_t digest[32];
    nk_sha256_init(&sha);
    while (length > 0) {
        size_t take = length < piece ? length : piece;
        nk_sha256_update(&sha, data, take);
        data += take;
        length -= take;
    }
    nk_sha256_finish(&sha, digest);
    for (size_t i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", digest[i]);
    out[64] = '\0';
}

static void test_sha256(void) {
    static const char two_blocks[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    char hex[65];
    sha256_hex((const uint8_t *)"", 0, 1, hex);
    CHECK(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0,
          "SHA-256 of the empty message");
    sha256_hex((const uint8_t *)"abc", 3, 3, hex);
    CHECK(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0,
          "SHA-256 of abc");
    sha256_hex((const uint8_t *)two_blocks, sizeof(two_blocks) - 1, 7, hex);
    CHECK(strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0,
          "SHA-256 of the 448-bit message (padding spills into a second block)");
    static const char long_two_blocks[] =
        "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
        "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
    sha256_hex((const uint8_t *)long_two_blocks, sizeof(long_two_blocks) - 1, 13, hex);
    CHECK(strcmp(hex, "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1") == 0,
          "SHA-256 of the 896-bit message");
    /* Messages that end at and around the block boundary and the length field (55, 56, 63,
       64, 65 bytes, and the same a block later), byte i = i * 7 + 1. The digests come from
       Python's hashlib, not from this code. Each is fed whole and split in two at every
       position, so every update size crosses the 64-byte block and the 56-byte padding
       limit; tests/native/vfpu_tables_selftest.c runs the same table on the runtime side. */
    static const struct { size_t length; const char *want; } edges[] = {
        { 55u, "16fa57a0a3423a715d594516339f36189d6b5f93754a9714fef202616a9fabfe" },
        { 56u, "c37b44e5f1b18554b36966f4f8e08bfbf3164c4b6c10374d12d89850892073c5" },
        { 63u, "bbba992d2c85af960fb2987a1fd05e0aa82a3db3c740dd8982a9e273b75e36a3" },
        { 64u, "66bd4633ed6f71c4ecfa4763bf7ba1c8ec7612de9aa6c0578a7b675207c71e0b" },
        { 65u, "9f7dc47107b750a1f3d35db5d9547f24ef40da5b731b9540d4f43710a154f6c9" },
        { 119u, "a3ed307b730fa77c07531300c6e4a282330011d4d4caf6bb7b63ae05950f4b66" },
        { 120u, "8e3b15d9fea7472655aa069620b7f8c2e55ee1499f763200a7515fe826e99d20" },
        { 127u, "44480fb9672845177f5368a08b69ea263275f2a5ec42e06a933370fe0d2968a4" },
        { 128u, "e462c130fef8c97e34f7dc3ff3ad2f8b3533ab849af21c10531552a2852387a4" },
        { 129u, "aa7ea4e8bf89146aeb67ff195fd8182a0e504c9d580aa7af8d2c862e14b98405" },
    };
    uint8_t message[129];
    for (size_t i = 0; i < sizeof(message); i++) message[i] = (uint8_t)(i * 7u + 1u);
    for (size_t e = 0; e < sizeof(edges) / sizeof(edges[0]); e++) {
        size_t length = edges[e].length;
        sha256_hex(message, length, length, hex);
        if (strcmp(hex, edges[e].want) != 0) {
            fprintf(stderr, "FAIL: SHA-256 of a %zu-byte message: got %s\n", length, hex);
            failures++;
        }
        for (size_t split = 0; split <= length; split++) {
            NkSha256 sha;
            uint8_t digest[32];
            nk_sha256_init(&sha);
            nk_sha256_update(&sha, message, split);
            nk_sha256_update(&sha, message + split, length - split);
            nk_sha256_finish(&sha, digest);
            for (size_t i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
            if (strcmp(hex, edges[e].want) != 0) {
                fprintf(stderr, "FAIL: SHA-256 of a %zu-byte message split after %zu bytes: got %s\n",
                        length, split, hex);
                failures++;
            }
        }
    }
    uint8_t *million = (uint8_t *)malloc(1000000u);
    CHECK(million != NULL, "SHA-256 million-byte buffer");
    if (million) {
        memset(million, 'a', 1000000u);
        sha256_hex(million, 1000000u, 997, hex);
        CHECK(strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0,
              "SHA-256 of one million 'a' bytes fed in 997-byte pieces");
        free(million);
    }
}

/* The reference nk_utf8_char_len is checked against: classify the lead byte by its bit pattern,
 * decode the code point, then check its range. That is a different method from the helper's
 * (Unicode table 3-7 second-byte ranges); it is how nk_cheats.c's and player_state.c's own copies
 * worked before they moved to the helper. */
static size_t utf8_reference_len(const unsigned char *s, size_t len, unsigned int flags) {
    unsigned long cp, min;
    size_t extra;
    if (len == 0u) return 0u;
    if (s[0] < 0x80u) return 1u;
    if ((s[0] & 0xE0u) == 0xC0u) { extra = 1u; min = 0x80u; cp = s[0] & 0x1Fu; }
    else if ((s[0] & 0xF0u) == 0xE0u) { extra = 2u; min = 0x800u; cp = s[0] & 0x0Fu; }
    else if ((s[0] & 0xF8u) == 0xF0u) { extra = 3u; min = 0x10000u; cp = s[0] & 0x07u; }
    else return 0u;
    if (len <= extra) return 0u;
    for (size_t k = 1u; k <= extra; k++) {
        if ((s[k] & 0xC0u) != 0x80u) return 0u;
        cp = (cp << 6) | (s[k] & 0x3Fu);
    }
    if (cp < min || cp > 0x10FFFFul || (cp >= 0xD800ul && cp <= 0xDFFFul)) return 0u;
    if ((flags & NK_UTF8_REJECT_C1) != 0u && cp <= 0x9Ful) return 0u;
    return extra + 1u;
}

/* One table for every copy nk_utf8_char_len replaced. Rule set "strict" (flags 0) is
 * nk_zip.c's zip_utf8_valid (ZIP entry names), player_state.c's texpack_utf8_valid (texture-pack
 * paths and names) and texrepl.c's text_clean (pack text printed to a terminal; it turns C1 into
 * "??" itself). Rule set "no C1" (NK_UTF8_REJECT_C1) is nk_cheats.c's clean_name (cheat names in
 * logs). nk_iso.c's SFO reader keeps its own decoder: it needs the length of an ill-formed
 * sequence's maximal subpart, the unit one U+FFFD replaces, which a 0 cannot carry. */
static void test_utf8_char_len(void) {
    static const struct {
        const char *name;
        unsigned char bytes[5];
        size_t len;
        size_t strict;
        size_t no_c1;
    } cases[] = {
        { "no bytes", { 0x41 }, 0u, 0u, 0u },
        { "NUL", { 0x00 }, 1u, 1u, 1u },
        { "ASCII letter", { 0x41 }, 1u, 1u, 1u },
        { "C0 control", { 0x1B }, 1u, 1u, 1u },
        { "DEL", { 0x7F }, 1u, 1u, 1u },
        { "lone continuation byte 80", { 0x80 }, 1u, 0u, 0u },
        { "lone continuation byte 9B (a raw C1 byte)", { 0x9B, 0x41 }, 2u, 0u, 0u },
        { "lone continuation byte BF", { 0xBF, 0x41 }, 2u, 0u, 0u },
        { "overlong C0 80", { 0xC0, 0x80 }, 2u, 0u, 0u },
        { "overlong C1 BF", { 0xC1, 0xBF }, 2u, 0u, 0u },
        { "U+0080, the first C1 control", { 0xC2, 0x80 }, 2u, 2u, 0u },
        { "U+009B, CSI", { 0xC2, 0x9B }, 2u, 2u, 0u },
        { "U+009F, the last C1 control", { 0xC2, 0x9F }, 2u, 2u, 0u },
        { "U+00A0", { 0xC2, 0xA0 }, 2u, 2u, 2u },
        { "U+00E9", { 0xC3, 0xA9 }, 2u, 2u, 2u },
        { "U+07FF", { 0xDF, 0xBF }, 2u, 2u, 2u },
        { "two bytes cut by len", { 0xC3, 0xA9 }, 1u, 0u, 0u },
        { "two bytes, second not a continuation", { 0xC3, 0x41 }, 2u, 0u, 0u },
        { "two bytes, second a lead byte", { 0xC3, 0xC3 }, 2u, 0u, 0u },
        { "overlong E0 9F BF", { 0xE0, 0x9F, 0xBF }, 3u, 0u, 0u },
        { "U+0800", { 0xE0, 0xA0, 0x80 }, 3u, 3u, 3u },
        { "U+3042", { 0xE3, 0x81, 0x82 }, 3u, 3u, 3u },
        { "U+D7FF", { 0xED, 0x9F, 0xBF }, 3u, 3u, 3u },
        { "surrogate U+D800", { 0xED, 0xA0, 0x80 }, 3u, 0u, 0u },
        { "surrogate U+DFFF", { 0xED, 0xBF, 0xBF }, 3u, 0u, 0u },
        { "U+E000", { 0xEE, 0x80, 0x80 }, 3u, 3u, 3u },
        { "U+FFFD", { 0xEF, 0xBF, 0xBD }, 3u, 3u, 3u },
        { "U+FFFF", { 0xEF, 0xBF, 0xBF }, 3u, 3u, 3u },
        { "three bytes cut by len after two", { 0xE3, 0x81, 0x82 }, 2u, 0u, 0u },
        { "three bytes cut by len after one", { 0xE3, 0x81, 0x82 }, 1u, 0u, 0u },
        { "three bytes, third not a continuation", { 0xE3, 0x81, 0x41 }, 3u, 0u, 0u },
        { "three bytes, NUL second", { 0xE3, 0x00, 0x82 }, 3u, 0u, 0u },
        { "overlong F0 8F BF BF", { 0xF0, 0x8F, 0xBF, 0xBF }, 4u, 0u, 0u },
        { "U+10000", { 0xF0, 0x90, 0x80, 0x80 }, 4u, 4u, 4u },
        { "U+1F600", { 0xF0, 0x9F, 0x98, 0x80 }, 4u, 4u, 4u },
        { "U+10FFFF", { 0xF4, 0x8F, 0xBF, 0xBF }, 4u, 4u, 4u },
        { "past U+10FFFF, F4 90", { 0xF4, 0x90, 0x80, 0x80 }, 4u, 0u, 0u },
        { "lead F5", { 0xF5, 0x80, 0x80, 0x80 }, 4u, 0u, 0u },
        { "lead F7", { 0xF7, 0xBF, 0xBF, 0xBF }, 4u, 0u, 0u },
        { "lead F8", { 0xF8, 0x88, 0x80, 0x80, 0x80 }, 5u, 0u, 0u },
        { "lead FE", { 0xFE }, 1u, 0u, 0u },
        { "lead FF", { 0xFF }, 1u, 0u, 0u },
        { "four bytes cut by len after three", { 0xF0, 0x9F, 0x98, 0x80 }, 3u, 0u, 0u },
        { "four bytes, fourth not a continuation", { 0xF0, 0x9F, 0x98, 0x41 }, 4u, 0u, 0u },
        { "only the first character counts", { 0x41, 0xFF }, 2u, 1u, 1u },
        { "bytes after the character are not read into it", { 0xC3, 0xA9, 0xFF }, 3u, 2u, 2u },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        size_t strict = nk_utf8_char_len(cases[i].bytes, cases[i].len, 0u);
        size_t no_c1 = nk_utf8_char_len(cases[i].bytes, cases[i].len, NK_UTF8_REJECT_C1);
        if (strict != cases[i].strict || no_c1 != cases[i].no_c1) {
            fprintf(stderr, "FAIL: UTF-8 length of %s: strict %zu (want %zu), no C1 %zu (want %zu)\n",
                    cases[i].name, strict, cases[i].strict, no_c1, cases[i].no_c1);
            failures++;
        }
    }
    CHECK(nk_utf8_char_len(NULL, 4u, 0u) == 0u, "UTF-8 length of no pointer is 0");

    /* Every first and second byte, with the third and fourth bytes over each class boundary,
     * at every length from 0 to 4, under both rule sets, against the reference. */
    static const unsigned char edges[] = { 0x00, 0x41, 0x7F, 0x80, 0x8F, 0x90, 0x9F, 0xA0, 0xBF,
                                           0xC0, 0xFF };
    unsigned long mismatches = 0;
    for (unsigned first = 0; first < 256u; first++) {
        for (unsigned second = 0; second < 256u; second++) {
            for (size_t t = 0; t < sizeof(edges); t++) {
                for (size_t f = 0; f < sizeof(edges); f++) {
                    const unsigned char bytes[4] = { (unsigned char)first, (unsigned char)second,
                                                     edges[t], edges[f] };
                    for (size_t len = 0; len <= 4u; len++) {
                        for (unsigned flags = 0; flags <= NK_UTF8_REJECT_C1; flags++) {
                            if (nk_utf8_char_len(bytes, len, flags) !=
                                utf8_reference_len(bytes, len, flags) && mismatches++ < 8u) {
                                fprintf(stderr, "FAIL: UTF-8 length of %02X %02X %02X %02X, len %zu, "
                                        "flags %u differs from the reference\n", bytes[0], bytes[1],
                                        bytes[2], bytes[3], len, flags);
                            }
                        }
                    }
                }
            }
        }
    }
    CHECK(mismatches == 0, "UTF-8 length agrees with the decoding reference on every sequence");
}

static bool write_test_bytes(const char *path, const unsigned char *bytes, size_t size) {
    FILE *file = nk_fopen_utf8(path, "wb");
    bool ok;
    if (!file) return false;
    ok = size == 0u || fwrite(bytes, 1u, size, file) == size;
    return fclose(file) == 0 && ok;
}

/* Reads path with max and checks the status, and on success the length and bytes, and that a
 * NUL follows them; on failure, that no buffer and no length are left. */
static void check_read(const char *what, const char *path, size_t max, NkReadFileStatus want,
                       const unsigned char *bytes, size_t size) {
    unsigned char *data = (unsigned char *)"not touched";
    size_t len = 12345u;
    NkReadFileStatus got = nk_read_file_bounded(path, max, &data, &len);
    bool ok = got == want;
    if (ok && want == NK_READ_FILE_OK) {
        ok = data != NULL && len == size && (size == 0u || memcmp(data, bytes, size) == 0) &&
             data[len] == '\0';
    } else if (ok) {
        ok = data == NULL && len == 0u;
    }
    if (!ok) {
        fprintf(stderr, "FAIL: bounded read, %s: status %d (want %d), length %zu\n", what, (int)got,
                (int)want, len);
        failures++;
    }
    if (got == NK_READ_FILE_OK) free(data);
}

static void test_read_file_bounded(void) {
    char path[4096];
    char missing[4096];
    unsigned char *data = NULL;
    size_t len = 0;
    int written = snprintf(path, sizeof(path), "%s/shared-helper-read-%lu.tmp",
                           SR_SELFTEST_BUILD_ROOT, test_process_id());
    int written_missing = snprintf(missing, sizeof(missing), "%s/shared-helper-read-%lu-missing.tmp",
                                   SR_SELFTEST_BUILD_ROOT, test_process_id());
    CHECK(written > 0 && (size_t)written < sizeof(path) && written_missing > 0 &&
          (size_t)written_missing < sizeof(missing), "bounded-read test paths format");
    if (written <= 0 || (size_t)written >= sizeof(path) || written_missing <= 0 ||
        (size_t)written_missing >= sizeof(missing)) {
        return;
    }

    /* Open errors. */
    check_read("a missing file", missing, 16u, NK_READ_FILE_OPEN_FAILED, NULL, 0u);
    check_read("an empty path", "", 16u, NK_READ_FILE_OPEN_FAILED, NULL, 0u);
    check_read("no path", NULL, 16u, NK_READ_FILE_OPEN_FAILED, NULL, 0u);
    CHECK(nk_read_file_bounded(missing, 16u, NULL, &len) == NK_READ_FILE_OPEN_FAILED &&
          nk_read_file_bounded(missing, 16u, &data, NULL) == NK_READ_FILE_OPEN_FAILED,
          "bounded read without a place for the result opens nothing");
    CHECK(nk_read_stream_bounded(NULL, 16u, &data, &len) == NK_READ_FILE_OPEN_FAILED &&
          data == NULL && len == 0u, "bounded read of no stream is an open error");

    /* Sizes around the limit. */
    static const unsigned char abc[] = { 'a', 'b', 'c' };
    CHECK(write_test_bytes(path, abc, sizeof(abc)), "bounded-read test file is written");
    check_read("three bytes, limit 16", path, 16u, NK_READ_FILE_OK, abc, sizeof(abc));
    check_read("three bytes, limit 3", path, 3u, NK_READ_FILE_OK, abc, sizeof(abc));
    check_read("three bytes, limit 2", path, 2u, NK_READ_FILE_TOO_LARGE, NULL, 0u);
    check_read("three bytes, limit 0", path, 0u, NK_READ_FILE_TOO_LARGE, NULL, 0u);
    check_read("three bytes, no limit", path, (size_t)-1, NK_READ_FILE_OK, abc, sizeof(abc));

    /* Out of memory: the buffer's allocation fails. */
#if defined(NK_TEST_WRAP_MALLOC)
    s_malloc_fail_at = 0;
    check_read("the buffer cannot be allocated", path, 16u, NK_READ_FILE_OUT_OF_MEMORY, NULL, 0u);
    CHECK(s_malloc_fail_at == -1, "bounded read made the allocation that failed");
    s_malloc_fail_at = -1;
#else
    puts("SKIP: bounded read out-of-memory case (this linker has no --wrap=malloc)");
#endif

    CHECK(write_test_bytes(path, NULL, 0u), "empty bounded-read test file is written");
    check_read("an empty file", path, 16u, NK_READ_FILE_OK, NULL, 0u);
    check_read("an empty file, limit 0", path, 0u, NK_READ_FILE_OK, NULL, 0u);

    enum { BIG = 200000 };
    unsigned char *big = (unsigned char *)malloc(BIG);
    CHECK(big != NULL, "bounded-read large pattern buffer");
    if (big) {
        for (size_t i = 0; i < BIG; i++) big[i] = (unsigned char)(i * 131u + (i >> 9));
        CHECK(write_test_bytes(path, big, BIG), "large bounded-read test file is written");
        check_read("200000 bytes, limit 200000", path, BIG, NK_READ_FILE_OK, big, BIG);
        check_read("200000 bytes, limit 199999", path, BIG - 1u, NK_READ_FILE_TOO_LARGE, NULL, 0u);
    }

    /* A read error: a stream open only for writing cannot be read. */
    FILE *write_only = nk_fopen_utf8(path, "wb");
    CHECK(write_only != NULL, "write-only stream opens");
    if (write_only) {
        data = (unsigned char *)"not touched";
        len = 7u;
        CHECK(nk_read_stream_bounded(write_only, 16u, &data, &len) == NK_READ_FILE_READ_FAILED &&
              data == NULL && len == 0u, "bounded read of a write-only stream is a read error");
        fclose(write_only);
    }
    CHECK(remove_test_file(path) == 0, "bounded-read test file cleanup succeeds");

    /* A stream that reports no size (a pipe): the buffer grows as it reads. */
    if (big) {
        static const struct { size_t bytes; size_t max; NkReadFileStatus want; } pipes[] = {
            { 10000u, 1u << 20, NK_READ_FILE_OK },
            { 10000u, 10000u, NK_READ_FILE_OK },
            { 10000u, 9999u, NK_READ_FILE_TOO_LARGE },
            { 0u, 16u, NK_READ_FILE_OK },
        };
        for (size_t p = 0; p < sizeof(pipes) / sizeof(pipes[0]); p++) {
            int fds[2];
            FILE *reader;
#if defined(_WIN32) || defined(_WIN64)
            CHECK(_pipe(fds, 65536u, _O_BINARY) == 0, "test pipe opens");
            if (fds[0] < 0) continue;
            CHECK(_write(fds[1], big, (unsigned)pipes[p].bytes) == (int)pipes[p].bytes,
                  "test pipe takes the bytes");
            _close(fds[1]);
            reader = _fdopen(fds[0], "rb");
#else
            if (pipe(fds) != 0) {
                CHECK(0, "test pipe opens");
                continue;
            }
            CHECK(write(fds[1], big, pipes[p].bytes) == (ssize_t)pipes[p].bytes,
                  "test pipe takes the bytes");
            close(fds[1]);
            reader = fdopen(fds[0], "rb");
#endif
            CHECK(reader != NULL, "test pipe reads as a stream");
            if (!reader) continue;
            data = NULL;
            len = 0u;
            NkReadFileStatus got = nk_read_stream_bounded(reader, pipes[p].max, &data, &len);
            bool ok = got == pipes[p].want;
            if (ok && got == NK_READ_FILE_OK) {
                ok = data != NULL && len == pipes[p].bytes &&
                     (len == 0u || memcmp(data, big, len) == 0) && data[len] == '\0';
            } else if (ok) {
                ok = data == NULL && len == 0u;
            }
            if (!ok) {
                fprintf(stderr, "FAIL: bounded read of a %zu-byte pipe, limit %zu: status %d (want %d)\n",
                        pipes[p].bytes, pipes[p].max, (int)got, (int)pipes[p].want);
                failures++;
            }
            free(got == NK_READ_FILE_OK ? data : NULL);
            fclose(reader);
        }
        free(big);
    }
}

int main(void) {
    test_path_joins();
    test_legacy_path_contracts();
    test_text_copy();
    test_utf8_open();
    test_utf8_char_len();
    test_read_file_bounded();
    test_size_format();
    test_boot_events();
    test_render_path();
    test_render_fallback_event();
    test_render_fallback_window();
    test_sha256();
    if (failures != 0) {
        fprintf(stderr, "shared helper selftest: %d failure(s)\n", failures);
        return 1;
    }
    puts("shared helper selftest passed");
    return 0;
}

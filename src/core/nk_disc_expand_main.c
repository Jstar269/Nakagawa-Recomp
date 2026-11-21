/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/*
 * nk_disc_expand: the command-line face of nk_disc_expand.c, so the Python
 * tools (tools/nk_core/disc_expand_tool.py, used by `nk_cli.py bringup` and
 * iso_inspect) expand a compressed disc image with the same C code the player
 * runs. There is no second decoder in Python.
 *
 *   nk_disc_expand probe  SOURCE
 *   nk_disc_expand import [--progress] --data-root DIR SOURCE
 *   nk_disc_expand expand [--progress] SOURCE DEST.iso
 *
 * Each prints one JSON object on stdout: {"status":"OK",...} with exit 0, or
 * {"status":"REFUSED","code":"<NK_DISC_* name>","message":"..."} with exit 2.
 * --progress writes "progress <done> <total>" lines to stderr. Exit 64 is a
 * usage error. import reads DIR/library.json, as the player reads its library,
 * to choose the folder of the key the disc's entry holds.
 */

#include "nk_disc_chd.h"
#include "nk_disc_expand.h"
#include "nk_library.h"
#include "nk_platform.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#include <shellapi.h>
#endif

static void json_string(FILE *out, const char *text) {
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; p++) {
        if (*p == '"' || *p == '\\') {
            fputc('\\', out);
            fputc(*p, out);
        } else if (*p < 0x20u) {
            fprintf(out, "\\u%04x", (unsigned)*p);
        } else {
            fputc(*p, out);
        }
    }
    fputc('"', out);
}

static int refused(const NkDiscExpandStatus *status) {
    fputs("{\"status\":\"REFUSED\",\"code\":", stdout);
    json_string(stdout, status->code[0] ? status->code : NK_DISC_INTERNAL);
    fputs(",\"message\":", stdout);
    json_string(stdout, status->message);
    fputs("}\n", stdout);
    return 2;
}

static void print_progress(uint64_t done, uint64_t total, void *user) {
    (void)user;
    fprintf(stderr, "progress %" PRIu64 " %" PRIu64 "\n", done, total);
}

static int usage(void) {
    fputs("usage: nk_disc_expand probe SOURCE\n"
          "       nk_disc_expand import [--progress] --data-root DIR SOURCE\n"
          "       nk_disc_expand expand [--progress] SOURCE DEST.iso\n",
          stderr);
    return 64;
}

static int run(int argc, char **argv) {
    NkDiscExpandCallbacks callbacks = { NULL, NULL, NULL };
    NkDiscExpandStatus status;
    const char *data_root = NULL;
    const char *positional[2] = { NULL, NULL };
    int positional_count = 0;
    const char *command;
    memset(&status, 0, sizeof(status));
    if (argc < 2) return usage();
    command = argv[1];
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--progress") == 0) {
            callbacks.on_progress = print_progress;
        } else if (strcmp(argv[i], "--data-root") == 0 && i + 1 < argc) {
            data_root = argv[++i];
        } else if (argv[i][0] == '-' && argv[i][1] == '-') {
            return usage();
        } else if (positional_count < 2) {
            positional[positional_count++] = argv[i];
        } else {
            return usage();
        }
    }
    nk_disc_chd_register();

    if (strcmp(command, "probe") == 0 && positional_count == 1 && !data_root) {
        NkDiscFormat format;
        uint64_t bytes;
        if (!nk_disc_expanded_size(positional[0], &format, &bytes, &status)) return refused(&status);
        printf("{\"status\":\"OK\",\"format\":\"%s\",\"expanded_bytes\":%" PRIu64 "}\n",
               nk_disc_format_extension(format), bytes);
        return 0;
    }
    if (strcmp(command, "import") == 0 && positional_count == 1 && data_root) {
        /* The library in the data folder keys the import as the player's does
           (player_disc_image_expand): a prototype disc whose entry holds its
           v1 key goes into that key's folder. No library is an empty one; a
           library that cannot be read, or one a newer Nakagawa wrote, refuses
           the import by name. */
        NkDiscImport result;
        char library_path[NK_MAX_PATH];
        NkLibrary *library = (NkLibrary *)calloc(1, sizeof(NkLibrary));
        int written = snprintf(library_path, sizeof(library_path), "%s%clibrary.json",
                               data_root, nk_platform_path_separator());
        bool imported;
        if (!library || written < 0 || (size_t)written >= sizeof(library_path) ||
            nk_library_load(library, library_path) != NK_OK) {
            free(library);
            snprintf(status.code, sizeof(status.code), "%s", NK_DISC_LIBRARY_UNREADABLE);
            snprintf(status.message, sizeof(status.message),
                     "Nakagawa's library could not be read, so the folder this disc image "
                     "belongs in can't be chosen. Nothing was changed.");
            return refused(&status);
        }
        imported = nk_disc_import_for_library(positional[0], data_root, library->entries,
                                              library->count, &callbacks, &result, &status);
        free(library);
        if (!imported) return refused(&status);
        printf("{\"status\":\"OK\",\"format\":\"%s\",\"disc_id\":",
               nk_disc_format_extension(result.format));
        json_string(stdout, result.disc_id);
        fputs(",\"iso_path\":", stdout);
        json_string(stdout, result.iso_path);
        printf(",\"identity\":\"%s\",\"expanded_bytes\":%" PRIu64 ",\"reused\":%s}\n",
               result.identity, result.expanded_bytes, result.reused ? "true" : "false");
        return 0;
    }
    if (strcmp(command, "expand") == 0 && positional_count == 2 && !data_root) {
        uint64_t bytes;
        if (!nk_disc_expand_file(positional[0], positional[1], &callbacks, &bytes, &status)) {
            return refused(&status);
        }
        printf("{\"status\":\"OK\",\"expanded_bytes\":%" PRIu64 "}\n", bytes);
        return 0;
    }
    return usage();
}

int main(int argc, char **argv) {
#if defined(_WIN32) || defined(_WIN64)
    /* Paths arrive as UTF-16 on Windows; the core takes UTF-8. */
    int wargc = 0;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    char **utf8 = wargv ? (char **)calloc((size_t)wargc + 1u, sizeof(char *)) : NULL;
    int rc;
    if (!utf8) return usage();
    for (int i = 0; i < wargc; i++) {
        int len = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
        utf8[i] = len > 0 ? (char *)malloc((size_t)len) : NULL;
        if (!utf8[i] || WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, utf8[i], len, NULL, NULL) <= 0) {
            return usage();
        }
    }
    LocalFree(wargv);
    (void)argc;
    (void)argv;
    rc = run(wargc, utf8);
    for (int i = 0; i < wargc; i++) free(utf8[i]);
    free(utf8);
    return rc;
#else
    return run(argc, argv);
#endif
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/* What Nakagawa Font Copy (src/psp_companion/fontcopy.c) copies and where it writes. These
 * choices live in a header so the host harness (tests/fixtures/psp_oracle/io_cases_host.c, mode
 * fontcopy) runs exactly what the app runs; tools/test_psp_oracle.py checks them.
 *
 *  - Source: flash0:/font, listed and opened read-only through font_files.h. Only names ending
 *    in .pgf are copied: the console's system fonts in the PlayStation Glyph Format, plain
 *    unencrypted files that the player imports (docs/SETUP.md, "System fonts"). Nothing is
 *    decrypted, and no other flash0 folder is touched.
 *  - If the console refuses to list flash0:/font to the app, or the listing opens but names no
 *    .pgf file (its first read fails, or it reads as empty), the eighteen firmware font names
 *    below (the files the player's Japanese, Korean and sixteen Latin font slots stand for) are
 *    opened read-only one by one instead.
 *  - A font whose copy in the folder already matches a fresh flash0 read (size and SHA-256) is
 *    kept as it is (keep_verified), so "try again" never truncates a copy that was good.
 *  - Destination: the app's own folder, NAKAGAWA/fonts, on the storage the app was started
 *    from: ef0: (the PSP Go's internal storage) when argv[0] names it, otherwise ms0: (the
 *    Memory Stick). Only that folder and its NAKAGAWA parent are created; nothing else is
 *    written.
 *  - Outcome: fontcopy_view_record reads the copy's records and fontcopy_outcome decides what
 *    the app reports. A listing that stopped early is never reported as a complete copy, and a
 *    failed copy says whether reading the PSP's font or writing the folder failed.
 *  - Progress: fontcopy_step_font names the font a copy step starts, so the app shows the font
 *    being copied while it is copied.
 *
 * The includer includes font_files.h first. */

#ifndef NAKAGAWA_FONTCOPY_CONFIG_H
#define NAKAGAWA_FONTCOPY_CONFIG_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FONTCOPY_PARENT "/NAKAGAWA"
#define FONTCOPY_FOLDER "/NAKAGAWA/fonts"
#define FONTCOPY_KNOWN_COUNT 18u

static const char *const s_fontcopy_known[FONTCOPY_KNOWN_COUNT] = {
    "jpn0.pgf", "kr0.pgf",
    "ltn0.pgf", "ltn1.pgf", "ltn2.pgf", "ltn3.pgf", "ltn4.pgf", "ltn5.pgf",
    "ltn6.pgf", "ltn7.pgf", "ltn8.pgf", "ltn9.pgf", "ltn10.pgf", "ltn11.pgf",
    "ltn12.pgf", "ltn13.pgf", "ltn14.pgf", "ltn15.pgf",
};

/* The folder leaves room for every safe font name, so font_files.h never refuses it (font-dest). */
_Static_assert(sizeof("ms0:" FONTCOPY_FOLDER) - 1u + 1u + FONTFILES_NAME_MAX < FONTFILES_PATH_MAX,
               "<device>/NAKAGAWA/fonts/<name> must fit FONTFILES_PATH_MAX");
_Static_assert(sizeof("ms0:") == sizeof("ef0:"), "both devices name folders of one length");

typedef struct FontCopyTarget {
    char device[8];   /* "ms0:" or "ef0:" */
    char parent[24];  /* <device>/NAKAGAWA */
    char folder[24];  /* <device>/NAKAGAWA/fonts */
} FontCopyTarget;

/* The storage the app started from: ef0: when argv[0] begins with it (in any case), else ms0:. */
static void fontcopy_target(FontCopyTarget *target, const char *argv0) {
    const char *device = "ms0:";
    if (argv0 != NULL && (argv0[0] | 0x20) == 'e' && (argv0[1] | 0x20) == 'f' &&
        argv0[2] == '0' && argv0[3] == ':') {
        device = "ef0:";
    }
    (void)snprintf(target->device, sizeof(target->device), "%s", device);
    (void)snprintf(target->parent, sizeof(target->parent), "%s" FONTCOPY_PARENT, device);
    (void)snprintf(target->folder, sizeof(target->folder), "%s" FONTCOPY_FOLDER, device);
}

/* The copy settings for a target fontcopy_target chose (once, from argv[0]). */
static void fontcopy_use_target(FontFiles *files, const FontCopyTarget *target) {
    files->dest_parent = target->parent;
    files->dest_dir = target->folder;
    files->pgf_only = 1u;
    files->known_names = s_fontcopy_known;
    files->known_count = FONTCOPY_KNOWN_COUNT;
    files->keep_verified = 1u;
}

/* Which side a failed copy failed on, from its font-copy record. */
typedef enum FontCopyFault {
    FONTCOPY_FAULT_NONE,  /* no copy failed */
    FONTCOPY_FAULT_READ,  /* the PSP's font: the flash0 open or read failed, or a second read or
                           * the listed size disagreed with the first read */
    FONTCOPY_FAULT_WRITE, /* the folder: the destination open or a write failed, or the copy
                           * read back differs from what was written */
} FontCopyFault;

/* What the copy reported, read from its records as they arrive. */
typedef struct FontCopyView {
    uint32_t finished;     /* copies finished (font-copy and font-kept records), failed or not */
    uint32_t first_error;  /* the first failing copy's result; 0 when none */
    uint32_t first_fault;  /* FontCopyFault of that copy */
    uint32_t known_tried;  /* 1 when the known names were opened (font-known records) */
    uint32_t list_error;   /* the font folder listing's open result when it failed; 0 otherwise */
    uint32_t listing_end;  /* the sceIoDread result that stopped the listing early (0xffffffff
                            * when FONTFILES_ENTRIES_MAX stopped it); 0 when it read to the end */
} FontCopyView;

/* The word after " <key>=" in a record line; 0 when the key is absent. */
static uint32_t fontcopy_field(const char *line, const char *key) {
    const char *at = strstr(line, key);
    return at != NULL ? (uint32_t)strtoul(at + strlen(key), NULL, 0) : 0u;
}

/* The side a failing font-copy record failed on (font_files.h fontfiles_copy: out0 flash0 open,
 * out1 pass A bytes or read error, out2 destination open, out8 FONTFILES_MATCH_* bits; result
 * 0x80000106 is a digest or size mismatch, any other result after both opens a write error). */
static FontCopyFault fontcopy_copy_fault(const char *line) {
    if ((int32_t)fontcopy_field(line, " out0=") < 0) return FONTCOPY_FAULT_READ;
    if ((int32_t)fontcopy_field(line, " out2=") < 0) return FONTCOPY_FAULT_WRITE;
    if ((int32_t)fontcopy_field(line, " out1=") < 0) return FONTCOPY_FAULT_READ;
    if (fontcopy_field(line, " result=") != 0x80000106u) return FONTCOPY_FAULT_WRITE;
    return (fontcopy_field(line, " out8=") & FONTFILES_MATCH_READBACK) == 0u
        ? FONTCOPY_FAULT_WRITE : FONTCOPY_FAULT_READ;
}

/* Fold one font_files.h record into view. Returns 1 for a finished copy (font-copy or
 * font-kept record). */
static int fontcopy_view_record(FontCopyView *view, const char *line) {
    if (strstr(line, " case_id=font-kept-") != NULL) {
        view->finished++;
        return 1;
    }
    if (strstr(line, " case_id=font-copy-") != NULL) {
        view->finished++;
        if (strstr(line, " status=PASS ") == NULL && view->first_error == 0u) {
            view->first_error = fontcopy_field(line, " result=");
            view->first_fault = (uint32_t)fontcopy_copy_fault(line);
        }
        return 1;
    }
    if (strstr(line, " case_id=font-known-") != NULL) {
        view->known_tried = 1u;
    } else if (strstr(line, " case_id=font-dir-open ") != NULL &&
        strstr(line, " status=FAIL ") != NULL) {
        view->list_error = fontcopy_field(line, " result=");
    } else if (strstr(line, " case_id=font-dir-close ") != NULL) {
        /* out1 is the read that ended the listing: nonzero means entries after it were never
         * seen, so the copies made cannot be the whole font folder. */
        view->listing_end = fontcopy_field(line, " out1=");
    }
    return 0;
}

/* The font a font_files.h step starts copying, or NULL: with keep_verified every copy starts
 * with "check:<folder>/<name>", before the font is read. */
static const char *fontcopy_step_font(const char *step) {
    if (strncmp(step, "check:", 6) != 0) return NULL;
    const char *slash = strrchr(step, '/');
    return slash != NULL ? slash + 1 : step + 6;
}

typedef enum FontCopyOutcome {
    FONTCOPY_ALL_COPIED,   /* the whole folder (or every known name found) was copied and checked */
    FONTCOPY_NONE_FOUND,   /* no font could be read, so nothing was copied */
    FONTCOPY_LISTING_CUT,  /* the listing stopped early and the known names were not tried, so
                            * some fonts were never seen */
    FONTCOPY_NONE_WRITTEN, /* fonts were found but none was copied */
    FONTCOPY_SOME_COPIED,  /* some of the fonts found were copied */
} FontCopyOutcome;

/* How a finished run ended, and the error code the app shows with it (0 for none). */
static FontCopyOutcome fontcopy_outcome(const FontFiles *files, const FontCopyView *view,
                                        uint32_t *code) {
    if (files->candidates == 0u) {
        *code = view->list_error != 0u ? view->list_error : view->listing_end;
        return FONTCOPY_NONE_FOUND;
    }
    if (view->listing_end != 0u && !view->known_tried) {
        *code = view->listing_end;
        return FONTCOPY_LISTING_CUT;
    }
    *code = view->first_error;
    if (files->copied == files->candidates) return FONTCOPY_ALL_COPIED;
    return files->copied == 0u ? FONTCOPY_NONE_WRITTEN : FONTCOPY_SOME_COPIED;
}

#endif /* NAKAGAWA_FONTCOPY_CONFIG_H */

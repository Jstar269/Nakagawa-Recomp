/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 the Nakagawa Recomp authors
 *
 * Homebrew in an EBOOT.PBP (issue #2143): the player turns a package whose
 * DATA.PSP is a plain, unencrypted user-mode PSP program into a small private
 * disc image and imports that exactly as a picked disc. The image holds
 * PSP_GAME/PARAM.SFO (the package's title and a placeholder DISC_ID),
 * PSP_GAME/SYSDIR/EBOOT.BIN (the DATA.PSP bytes) and, when the package has
 * one, PSP_GAME/ICON0.PNG. The placeholder is not a retail disc ID, so the
 * existing rule that keys a non-retail disc by its contents names the game:
 * the same program with the same title is the same game on every machine
 * (nk_iso_inspect and nk_non_retail_disc_key in nk_iso.c; the Python disc route
 * in tools/nk_core/iso_inspect.py applies the same rule to the image).
 *
 * Refused by name, never converted: an encrypted DATA.PSP, a DATA.PSP that is
 * not a PSP program, kernel-mode homebrew and custom-firmware plugins, and a
 * program larger than the disc route reads. No package section other than
 * these is executed or extracted.
 */

#ifndef NK_PBP_HOMEBREW_H
#define NK_PBP_HOMEBREW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nk_psp_npumdimg.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum NkPbpHomebrewKind {
    NK_PBP_HB_PROGRAM = 0,   /* a plain user-mode program this route can import */
    NK_PBP_HB_ENCRYPTED,     /* DATA.PSP is a ~PSP container */
    NK_PBP_HB_NOT_A_PROGRAM, /* DATA.PSP is empty, not an ELF32 MIPS PSP program, or its
                                module information lies outside it */
    NK_PBP_HB_KERNEL_MODE,   /* the module declares kernel mode (attribute 0x1000) */
    NK_PBP_HB_TOO_LARGE      /* DATA.PSP is larger than the disc route reads */
} NkPbpHomebrewKind;

typedef struct NkPbpHomebrew {
    NkPbpHomebrewKind kind;
    /* The library key the converted disc will have: X, three letters, five digits. */
    char key[10];
    /* The full digest the key is cut from (NkIsoMetadata.identity_sha256 of the converted
       disc), 64 lower-case hex digits. The key keeps about 31 bits of it, so two programs
       can share a key; this tells them apart. */
    char identity_sha256[65];
    char title[128];
    uint32_t program_size;
    uint32_t icon_size;
} NkPbpHomebrew;

/* The largest DATA.PSP imported. Homebrew is small, and the program is read and hashed on every
   pick, so the bound is far below the disc route's executable bound (512 MiB). */
#define NK_PBP_HOMEBREW_MAX_PROGRAM_BYTES (64u * 1024u * 1024u)

/* Classify a package nk_pbp_identify_file stopped at with
 * NK_PBP_PAYLOAD_OTHER (info is what it filled in). NK_PSP_OK and kind
 * NK_PBP_HB_PROGRAM: the package is importable, and out->key is the key its
 * converted disc will have. Otherwise NK_PSP_ERR_UNSUPPORTED with out->kind
 * naming the refusal, or NK_PSP_ERR_INPUT / NK_PSP_ERR_FORMAT when the file
 * cannot be read as a package. Reads the package; writes nothing. */
int nk_pbp_homebrew_inspect_file(NkPspCtx *ctx, const char *pbp_path,
                                 const NkPbpInfo *info, NkPbpHomebrew *out);

/* Write the private disc image for an importable package to iso_path: to
 * "<iso_path>.converting" first, checked by reading it back as a disc
 * (nk_iso_inspect must name out->key), then renamed into place. A failure or
 * a cancel removes the partial file. callbacks may be NULL. */
int nk_pbp_homebrew_convert_file(NkPspCtx *ctx, const char *pbp_path,
                                 const NkPbpInfo *info, const char *iso_path,
                                 const NkNpumdimgCallbacks *callbacks, NkPbpHomebrew *out);

/* The program's Memory Stick folder. A PSP runs homebrew from
 * ms0:/PSP/GAME/<folder>/EBOOT.PBP and passes that path as the program's first
 * argument; PSPSDK's start-up code makes its folder the program's working folder,
 * so the program opens its own files there. The import keeps a private copy of
 * the folder the package was picked from beside the disc image, laid out as on a
 * Memory Stick: "<image without .iso>.ms0/PSP/GAME/<folder>/" (the view), with a
 * manifest of every entry (its path and, for a file, its size and SHA-256) in
 * "<image without .iso>.ms0/view.manifest". Beside the image, "<image without
 * .iso>.source" records the path the picked folder really has and the name it was picked
 * by, written before the copy starts. A launch puts the view on the title's
 * Memory Stick (nk_pbp_homebrew_seed_memstick), then boots
 * ms0:/PSP/GAME/<folder>/EBOOT.PBP. Nothing is aliased: a path the folder does not
 * hold is missing, as on a PSP, and what the program writes stays on the title's
 * Memory Stick. The user's own folder is only read. */

/* The bounds of a folder imported with its program. */
#define NK_PBP_HOMEBREW_FOLDER_MAX_BYTES (1024ull * 1024ull * 1024ull)
#define NK_PBP_HOMEBREW_FOLDER_MAX_ENTRIES 4096u
#define NK_PBP_HOMEBREW_FOLDER_MAX_DEPTH 16u
/* The longest folder name, in bytes, without its terminator. */
#define NK_PBP_HOMEBREW_FOLDER_NAME_MAX 64u

typedef enum NkPbpHomebrewView {
    NK_PBP_HB_VIEW_NONE = 0, /* the image is not one this import wrote */
    NK_PBP_HB_VIEW_FOUND,    /* one folder, holding its EBOOT.PBP, and its manifest */
    NK_PBP_HB_VIEW_MISSING,  /* an imported program's image with no view beside it */
    NK_PBP_HB_VIEW_BROKEN    /* a view that is not one such folder */
} NkPbpHomebrewView;

/* The name the program's folder has on the Memory Stick: the name of the folder
 * pbp_path is in. False, with a sentence in problem, when that folder has no name
 * a PSP's Memory Stick can hold (a drive root, a name longer than
 * NK_PBP_HOMEBREW_FOLDER_NAME_MAX, a character outside printable ASCII or one of
 * \ / : * ? " < > |, or a name ending in a space or a dot). */
bool nk_pbp_homebrew_folder_name(const char *pbp_path, char *out, size_t out_size,
                                 char *problem, size_t problem_size);

/* Whether the package at pbp_path is named EBOOT.PBP, in capitals, as the file has it on
 * the disk: the one name a PSP starts a program from, and the name the view and the
 * launch use. False, with a sentence in problem, for any other name (eboot.pbp,
 * MyGame.PBP). */
bool nk_pbp_homebrew_program_named(const char *pbp_path, char *problem, size_t problem_size);

/* The view's root for the disc image at iso_path: iso_path with ".iso" replaced
 * by ".ms0". False when iso_path does not end in ".iso" or the result does not fit. */
bool nk_pbp_homebrew_view_root(const char *iso_path, char *out, size_t out_size);

/* Check that the folder pbp_path is in can be imported with it: it has a name
 * (nk_pbp_homebrew_folder_name), stays within the bounds above, holds nothing a
 * copy would leave out (a link, a junction, a file kept in the cloud) and no name a
 * Memory Stick cannot hold, neither holds view_root nor lies inside it, and neither
 * is nor holds one of the user's personal folders (the home folder, the Desktop,
 * Documents, Downloads, Music, Pictures and Videos where the system says they are,
 * and a cloud-sync root), whose files are the user's, not the program's. Folders
 * are compared by the paths they really have, every link resolved. A hard link is a
 * file like any other: its bytes are copied, as the user put them in the folder.
 * Reads the folder; writes nothing. */
bool nk_pbp_homebrew_check_folder(const char *pbp_path, const char *view_root,
                                  char *problem, size_t problem_size);

/* Write the view for the package at pbp_path to view_root: the folder it is in,
 * copied to "<view_root>.importing/PSP/GAME/<folder>/" with each file on the disk
 * (read without following a link, exactly the bytes counted), and its manifest,
 * then put in place of any earlier view. callbacks may be NULL; its is_cancelled is
 * asked between files. On failure nothing new is kept and problem says why. */
bool nk_pbp_homebrew_write_view(const char *pbp_path, const char *view_root,
                                const NkNpumdimgCallbacks *callbacks,
                                char *problem, size_t problem_size);

/* Whether anything beside the image at iso_path came from a folder other than the one
 * pbp_path is in (review of #2426, M1, S2). The program's key comes from its EBOOT.PBP
 * alone, so the same file in two folders is one title: importing it from the second
 * folder would replace the first folder's copy, and both would run on one Memory Stick.
 * With a record (".source"), the folder must be the recorded one, by the path it really
 * has, every link resolved, and by the name it is picked by; whatever state the view or
 * its staging copy is in. Without one, the view must hold only PSP/GAME/<this folder's
 * name>, and no staging copy may be there. A record or view that cannot be read counts as
 * another folder's. True puts what names that folder in other: the recorded path, the
 * view's other folder name, or "" when nothing does. False when nothing is there or all
 * of it is this folder's. Reads only. */
bool nk_pbp_homebrew_other_source(const char *pbp_path, const char *iso_path, char *other,
                                  size_t other_size);

/* Delete what the import of the program whose image is iso_path keeps beside it: the image,
 * the view, its staging copy, the seed records and the source record, the record last. The
 * title's Memory Stick, with the program's own files, is not touched. Refused, deleting
 * nothing, unless the folder holding iso_path is in the data folder's discs/ and the image,
 * when there, is a program's. True when everything is gone (also when nothing was there);
 * false, with a sentence in problem, when a part could not be deleted. */
bool nk_pbp_homebrew_delete_import(const char *iso_path, char *problem, size_t problem_size);

/* Whether the view at view_root is the folder pbp_path is in as it is now: the
 * folder passes nk_pbp_homebrew_check_folder and its manifest, every file read
 * again, is the view's. False when anything differs or cannot be read. */
bool nk_pbp_homebrew_view_current(const char *pbp_path, const char *view_root);

/* Look for the view beside the disc image at iso_path. Only an image this import
 * wrote has one: an image with the import's volume identifier, in the data folder's
 * discs/ folder (nk_platform_get_app_data_dir), where only the import writes images.
 * Any other image is NK_PBP_HB_VIEW_NONE, whatever lies beside it.
 * NK_PBP_HB_VIEW_FOUND puts the folder's name in folder. Reads only. */
NkPbpHomebrewView nk_pbp_homebrew_find_view(const char *iso_path, char *folder,
                                            size_t folder_size);

/* Put the view beside iso_path on the Memory Stick at memstick_root (an absolute
 * path), in PSP/GAME/<folder>/, as a PSP owner copying a newer release over the
 * folder would, without losing what the program wrote. A file or folder the stick
 * lacks is copied. A file the stick has is replaced only when it still holds the
 * bytes an earlier launch put there (or the view's own bytes) and the view now has
 * others; anything else there (the program's own writes, a file the program
 * changed) stays as it is. What was put there is recorded per Memory Stick in
 * "<image without .iso>.seeded/", beside the image, also when the seed stops part
 * way. Every file copied is checked against the manifest; a view file that is
 * missing or differs from it removes the manifest, so adding the program again
 * writes the view afresh. Nothing is written through a link or junction already on
 * the stick: a folder whose real path leads outside the program's folder stops the
 * seed. False, with a sentence in problem, when a part could not be read or copied. */
bool nk_pbp_homebrew_seed_memstick(const char *iso_path, const char *folder,
                                   const char *memstick_root,
                                   char *problem, size_t problem_size);

#ifdef __cplusplus
}
#endif

#endif /* NK_PBP_HOMEBREW_H */

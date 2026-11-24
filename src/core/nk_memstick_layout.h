/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The shared Memory Stick (#2744): where it is and the record that says what state it is in.
 *
 * Every package launch uses one Memory Stick for all games, as a PSP does. It lives in the
 * per-user saves folder (NK_PATH_SAVES), so it follows that folder on every platform and in
 * portable mode and under --user-data-root, with no rule of its own:
 *
 *   <saves>/
 *     Memory Stick/               the shared stick, ms0:/ of every package launch
 *     Memory Stick.moving/        the stick while the first move builds it (renamed at commit)
 *     nakagawa-saves.json         the layout record below; outside the stick, so no game sees it
 *     .nakagawa-migration/        the move's plan, progress log and one record per old folder
 *     Kept copies/                the losing copy of each collision
 *     .nakagawa-stick.lock        held while a game runs or the move runs (one at a time)
 *     .nakagawa-claims/           which stick paths each game used
 *     <DISC_ID>/, <title ID>/, X???#####/, unidentified/
 *                                 the per-game sticks of v0.0.1 to v0.0.3, left in place
 *
 * nk_memstick_layout_paths names every one of these, so the move, the launcher and the player
 * all build them here; nk_memstick_layout_is_reserved_name tells the move which entries of
 * <saves> are this layout's own and never an old per-game folder.
 *
 * The layout record, <saves>/nakagawa-saves.json:
 *
 *   {
 *     "schema_version": 1,
 *     "layout": "shared-memory-stick",
 *     "stick": "Memory Stick",
 *     "state": "moving",
 *     "written_by": "0.1.0",
 *     "sources": [
 *       {"slot": "ULUS99999", "status": "moved", "record": ".nakagawa-migration/ULUS99999.json"},
 *       {"slot": "XABC12345", "status": "pending", "reason": "MIGRATION_SOURCE_UNREADABLE"}
 *     ]
 *   }
 *
 * - "state" is "moving" until the stick is in place and complete, then "ready".
 * - Each source is one folder directly in <saves>. Its "status" is "moved" (its units are on the
 *   stick; "record" names the per-source record under .nakagawa-migration/), "pending" (not moved
 *   yet; "reason" is the named code), "left-by-choice" (the user chose to start without it),
 *   "changed-after-move" (an older player wrote to it after the move; "record" names the record
 *   of the move) or "not-a-slot" (not a game's save folder, left as it is). Slot names are unique
 *   without ASCII case ("ulus99999" and "ULUS99999" are one folder on NTFS and APFS), and each is
 *   a name every platform can make: none of < > : " / \ | ? * or a control byte, no trailing dot
 *   or space, and no Windows device name (CON, PRN, AUX, NUL, COM1-9, LPT1-9, with or without an
 *   extension).
 * - Any change to what a version-1 reader must understand bumps "schema_version". A version-1
 *   reader ignores members it does not know, so a later version-1 writer may only add members a
 *   version-1 reader can safely skip.
 *
 * The record follows the rule of every versioned per-user file (nk_user_data_schema.h), with one
 * difference: a damaged record is never replaced, because the stick's state cannot be known
 * without it.
 * - A newer schema_version is refused by name, SAVES_LAYOUT_NEWER: no game starts and nothing in
 *   <saves> is changed, so returning to the newer player finds everything as it was.
 * - A record that cannot be opened or read (another program holds it, a read error) is
 *   SAVES_LAYOUT_IO and is never replaced: its version is unknown.
 * - A record that is read whole but is not a valid version-1 record (empty, not JSON, a member
 *   out of range, larger than NK_MEMSTICK_LAYOUT_MAX_BYTES) is SAVES_LAYOUT_DAMAGED: it is kept
 *   and never replaced, and no game starts. Nothing rebuilds it on its own.
 * - nk_memstick_layout_write checks the record on the disk first and replaces only a valid
 *   version-1 record or none. It writes a temporary sibling, flushes it to the disk, renames it
 *   into place and syncs <saves>. The caller holds the stick lock, so there is one writer.
 *
 * The layout API names paths and reads and writes the record. The collision-copy API below
 * prepares one save folder for first migration. Neither API starts a game. */

#ifndef NK_MEMSTICK_LAYOUT_H
#define NK_MEMSTICK_LAYOUT_H

#include "nk_types.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NK_MEMSTICK_LAYOUT_SCHEMA_VERSION 1
#define NK_MEMSTICK_LAYOUT_KIND "shared-memory-stick"

/* The names in <saves> that belong to the layout. */
#define NK_MEMSTICK_STICK_NAME "Memory Stick"
#define NK_MEMSTICK_STAGING_NAME "Memory Stick.moving"
#define NK_MEMSTICK_RECORD_NAME "nakagawa-saves.json"
#define NK_MEMSTICK_RECORD_TEMP_NAME "nakagawa-saves.json.tmp"
#define NK_MEMSTICK_MIGRATION_NAME ".nakagawa-migration"
#define NK_MEMSTICK_KEPT_COPIES_NAME "Kept copies"
#define NK_MEMSTICK_LOCK_NAME ".nakagawa-stick.lock"
#define NK_MEMSTICK_CLAIMS_NAME ".nakagawa-claims"

/* The bounds a record is held to, far above any library's: a larger file, or one with more
   sources, is damaged. */
#define NK_MEMSTICK_LAYOUT_MAX_BYTES (16u * 1024u * 1024u)
#define NK_MEMSTICK_LAYOUT_MAX_SOURCES 16384u
/* Bytes, the terminating NUL not counted. A slot is a folder name, which on NTFS and APFS can be
   255 UTF-16 units, up to 765 bytes of UTF-8. */
#define NK_MEMSTICK_SLOT_MAX_BYTES 1020u
#define NK_MEMSTICK_RECORD_PATH_MAX_BYTES 1100u
#define NK_MEMSTICK_REASON_MAX_BYTES 63u
#define NK_MEMSTICK_WRITTEN_BY_MAX_BYTES 63u

typedef enum {
    NK_MEMSTICK_LAYOUT_OK = 0,
    /* There is no record: the move has never run on this saves folder. */
    NK_MEMSTICK_LAYOUT_ABSENT,
    /* SAVES_LAYOUT_NEWER: a later player wrote the record. */
    NK_MEMSTICK_LAYOUT_NEWER,
    /* SAVES_LAYOUT_DAMAGED: read whole, but not a valid record. */
    NK_MEMSTICK_LAYOUT_DAMAGED,
    /* SAVES_LAYOUT_IO: the record could not be read or written, or memory ran out. */
    NK_MEMSTICK_LAYOUT_IO,
    /* SAVES_LAYOUT_INVALID: the caller passed a layout or name this module refuses. */
    NK_MEMSTICK_LAYOUT_INVALID
} NkMemstickLayoutResult;

typedef enum {
    NK_MEMSTICK_COLLISION_COPIED = 0,      /* distinct whole units, both preserved */
    NK_MEMSTICK_COLLISION_IDENTICAL,      /* one equivalent whole unit is enough */
    NK_MEMSTICK_COLLISION_INVALID,        /* incompatible names or the same legacy slot */
    NK_MEMSTICK_COLLISION_LAYOUT_BLOCKED, /* no readable moving record, or active stick exists */
    NK_MEMSTICK_COLLISION_SOURCE_UNREADABLE,
    NK_MEMSTICK_COLLISION_SOURCE_CHANGING,
    NK_MEMSTICK_COLLISION_DESTINATION_CONFLICT,
    NK_MEMSTICK_COLLISION_COPY_MISMATCH,
    NK_MEMSTICK_COLLISION_IO
} NkMemstickCollisionResult;

/* Copy one colliding PSP/SAVEDATA folder from two legacy slots during first migration.
 * The caller holds the stick lock and passes the already selected winner first. A valid
 * moving layout record is required and the shared stick must not exist yet. The slot
 * and save names are single components, with distinct slots and equivalent save names
 * under ASCII case folding. Legacy units are read-only. The winner goes to the staged
 * stick and a differing loser to Kept copies/<loser_slot>/PSP/SAVEDATA using its own
 * spelling. An existing complete, equivalent output can be reused on retry. A different
 * output is never replaced. Callers still own election, the journal and stick promotion. */
NkMemstickCollisionResult nk_memstick_copy_save_collision(const char *saves_root,
                                                          const char *winner_slot,
                                                          const char *winner_name,
                                                          const char *loser_slot,
                                                          const char *loser_name);

typedef enum {
    NK_MEMSTICK_STATE_MOVING = 0,
    NK_MEMSTICK_STATE_READY
} NkMemstickState;

typedef enum {
    NK_MEMSTICK_SOURCE_MOVED = 0,
    NK_MEMSTICK_SOURCE_PENDING,
    NK_MEMSTICK_SOURCE_LEFT_BY_CHOICE,
    NK_MEMSTICK_SOURCE_CHANGED_AFTER_MOVE,
    NK_MEMSTICK_SOURCE_NOT_A_SLOT
} NkMemstickSourceStatus;

/* One folder directly in <saves>. The strings are owned by the layout; record and reason are ""
   when absent. */
typedef struct {
    char *slot;
    NkMemstickSourceStatus status;
    char *record;
    char *reason;
} NkMemstickSource;

typedef struct {
    int schema_version;
    NkMemstickState state;
    char written_by[NK_MEMSTICK_WRITTEN_BY_MAX_BYTES + 1u];
    NkMemstickSource *sources;
    size_t source_count;
    size_t source_capacity;
} NkMemstickLayout;

/* Every path of the layout for one saves folder, absolute when saves is. */
typedef struct {
    char saves[NK_MAX_PATH];
    char stick[NK_MAX_PATH];
    char staging[NK_MAX_PATH];
    char record[NK_MAX_PATH];
    char record_temp[NK_MAX_PATH];
    char migration[NK_MAX_PATH];
    char kept_copies[NK_MAX_PATH];
    char lock[NK_MAX_PATH];
    char claims[NK_MAX_PATH];
} NkMemstickPaths;

/* The named code of a result ("SAVES_LAYOUT_NEWER", ...), never NULL. */
const char *nk_memstick_layout_result_code(NkMemstickLayoutResult result);
/* The record's spelling of a state or status ("moving", "changed-after-move", ...), never NULL. */
const char *nk_memstick_state_name(NkMemstickState state);
const char *nk_memstick_source_status_name(NkMemstickSourceStatus status);

/* Fill *out for the saves folder `saves_root`. False, with *out cleared, when saves_root is empty
   or a path does not fit. */
bool nk_memstick_layout_paths(const char *saves_root, NkMemstickPaths *out);
/* The same for this process's saves folder (NK_PATH_SAVES: the platform's, the portable folder's
   or --user-data-root's), resolved without creating anything. */
bool nk_memstick_layout_resolve_paths(NkMemstickPaths *out);
/* Whether `name`, an entry directly in <saves>, is one of the layout's own names above, compared
   without ASCII case as a case-insensitive file system would. Such an entry is never an old
   per-game folder. */
bool nk_memstick_layout_is_reserved_name(const char *name);

/* An empty version-1 layout in `state`, written by `written_by` (the player's version). False,
   leaving *layout empty, when written_by is not 1 to 63 printable ASCII characters. */
bool nk_memstick_layout_init(NkMemstickLayout *layout, NkMemstickState state,
                             const char *written_by);
/* Release the sources and clear *layout. NULL and a cleared layout are left alone. */
void nk_memstick_layout_free(NkMemstickLayout *layout);
/* The source named `slot` (exact bytes), or NULL. */
const NkMemstickSource *nk_memstick_layout_find_source(const NkMemstickLayout *layout,
                                                       const char *slot);
/* Add the source `slot`, or replace it when the layout has it, keeping its place. `record` and
   `reason` may be NULL or "". Refused with NK_MEMSTICK_LAYOUT_INVALID when a name is out of range
   or the status lacks what it needs ("moved" and "changed-after-move" need a record, "pending"
   needs a reason, "not-a-slot" takes no record), or when `slot` is an existing slot in another
   ASCII case; NK_MEMSTICK_LAYOUT_IO when memory runs out. A refusal leaves the layout as it
   was. */
NkMemstickLayoutResult nk_memstick_layout_put_source(NkMemstickLayout *layout, const char *slot,
                                                     NkMemstickSourceStatus status,
                                                     const char *record, const char *reason,
                                                     char *err, size_t err_size);

/* Parse a record's text into *out (cleared first; left empty unless NK_MEMSTICK_LAYOUT_OK).
   NK_MEMSTICK_LAYOUT_NEWER for an integer schema_version above this build's, whatever else the
   text holds; NK_MEMSTICK_LAYOUT_DAMAGED for anything else that is not a valid version-1 record.
   `err` names the defect. */
NkMemstickLayoutResult nk_memstick_layout_parse(const char *text, size_t length,
                                                NkMemstickLayout *out,
                                                char *err, size_t err_size);
/* The record's text for *layout, in a buffer the caller frees. NK_MEMSTICK_LAYOUT_INVALID for a
   layout parse would refuse, NK_MEMSTICK_LAYOUT_IO when memory runs out. */
NkMemstickLayoutResult nk_memstick_layout_format(const NkMemstickLayout *layout,
                                                 char **out_text, size_t *out_length,
                                                 char *err, size_t err_size);

/* Read <saves_root>/nakagawa-saves.json into *out, as the rules above say. `err` names the code
   and the file. *out is left empty unless NK_MEMSTICK_LAYOUT_OK. */
NkMemstickLayoutResult nk_memstick_layout_read(const char *saves_root, NkMemstickLayout *out,
                                               char *err, size_t err_size);
/* Replace <saves_root>/nakagawa-saves.json with *layout, as the rules above say. The record on
   the disk is left exactly as it was unless NK_MEMSTICK_LAYOUT_OK: NEWER, DAMAGED and IO name the
   record that stopped the write. The one exception is an IO result whose message says the record
   was replaced but <saves> could not be synced afterwards: the new record is in place, and a power
   loss before the system writes the folder may bring back the old one. */
NkMemstickLayoutResult nk_memstick_layout_write(const char *saves_root,
                                                const NkMemstickLayout *layout,
                                                char *err, size_t err_size);

#ifdef __cplusplus
}
#endif

#endif /* NK_MEMSTICK_LAYOUT_H */

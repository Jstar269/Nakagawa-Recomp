/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Add to Steam (#2152): one non-Steam shortcut in a user's local Steam library.
 *
 * Steam keeps a user's non-Steam games in `<Steam>/userdata/<account id>/config/shortcuts.vdf`, a
 * binary KeyValues file (Valve's public binary VDF layout): a top-level map "shortcuts" whose
 * children "0", "1", ... are maps, one per game. Every entry is a type byte, a NUL-terminated
 * key and a value. The types this file uses are 0x00 (a nested map, closed by 0x08), 0x01 (a
 * NUL-terminated string), 0x02 (a little-endian int32) and 0x08 (the end of a map). The file
 * ends with 0x08 0x08: the end of "shortcuts" and the end of the root.
 *
 * Everything here works on the user's own machine and makes no network call. Nothing is
 * stored locally: `ICON0.PNG` and `PIC1.PNG` from the game go into `config/grid` for its
 * shortcut app ID (no network calls, nothing uploaded; portrait capsule `<appid>p.png` and logo
 * `<appid>_logo.png` have no matching widescreen PSP art and are left empty).
 *
 * Rules (each has a test in tests/native/test_steam_shortcuts.c):
 *   - A shortcut is its executable and its launch options together: the player is one
 *     executable, and each game's shortcut starts it with that game's launch options.
 *   - A new entry is inserted before the end of "shortcuts" and every other byte stays byte
 *     for byte. The one entry ever changed is this shortcut's own from an earlier Nakagawa
 *     folder: the same non-empty launch options and the same executable name in another
 *     folder (each update unpacks into a new folder). Only its "exe" and "StartDir" values
 *     change; its app ID, name, play time and tags stay, so nothing is duplicated.
 *   - A file that does not parse as the layout above is NK_STEAM_ERR_CORRUPT and is left alone,
 *     with no backup and no temporary file. The parser reads untrusted bytes: every read is
 *     bounds-checked, nesting is capped and sizes are capped.
 *   - An entry with this executable and these launch options is NK_STEAM_ERR_EXISTS: nothing
 *     is written.
 *   - Steam rewrites this file when it exits, so while Steam runs nothing is written
 *     (NK_STEAM_ERR_STEAM_RUNNING). Nakagawa never ends Steam.
 *   - Before a write the file is copied to `shortcuts.vdf.nakagawa-backup-<stamp>` and the copy
 *     is read back and compared. If that fails nothing is written (NK_STEAM_ERR_BACKUP). The
 *     new file goes to a temporary name, is flushed, and is renamed over the original; it is read
 *     back and compared with what was meant to be written.
 *   - Only Windows finds Steam itself (the registry). Elsewhere a caller names the Steam folder,
 *     or gets NK_STEAM_ERR_UNSUPPORTED; nothing pretends to succeed. */

#ifndef NK_STEAM_SHORTCUTS_H
#define NK_STEAM_SHORTCUTS_H

#include "nk_types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NK_STEAM_OK = 0,
    NK_STEAM_ERR_ARGUMENT,       /* a missing, empty, too long or control-character field */
    NK_STEAM_ERR_CORRUPT,        /* shortcuts.vdf or a supplied artwork PNG is not valid */
    NK_STEAM_ERR_EXISTS,         /* an entry or existing artwork file is already there */
    NK_STEAM_ERR_TOO_LARGE,      /* the file or artwork PNG is over its size cap */
    NK_STEAM_ERR_STEAM_RUNNING,  /* close Steam first */
    NK_STEAM_ERR_NOT_FOUND,      /* no Steam, no such account, or no account at all */
    NK_STEAM_ERR_BACKUP,         /* the backup could not be made and verified */
    NK_STEAM_ERR_WRITE,          /* the new file could not be written or did not verify */
    NK_STEAM_ERR_UNSUPPORTED     /* this host cannot find Steam by itself */
} NkSteamStatus;

/* A stable name for a status, for logs and cards (never NULL). */
const char *nk_steam_status_name(NkSteamStatus status);

/* What a successful add did. */
typedef enum {
    NK_STEAM_CHANGE_NONE = 0,
    NK_STEAM_CHANGE_ADDED,    /* a new entry */
    NK_STEAM_CHANGE_UPDATED   /* this shortcut's entry from another folder now points here */
} NkSteamChange;

#define NK_STEAM_VDF_MAX_BYTES (16u * 1024u * 1024u)
#define NK_STEAM_FIELD_MAX 4096u
#define NK_STEAM_ACCOUNT_CAP 16u

typedef struct {
    const char *app_name;        /* shown in the library; required */
    const char *exe;             /* the executable's full path, unquoted (a leading quote is refused) */
    const char *start_dir;       /* its start-in folder; required */
    const char *launch_options;  /* NULL or "" for none (the default) */
} NkSteamShortcut;

/* The app ID Steam derives for a non-Steam shortcut: the CRC-32 of the quoted executable path
 * followed by the name, with the top bit set. Steam shows it in the entry's "appid". */
uint32_t nk_steam_shortcut_app_id(const char *exe, const char *app_name);

/* The pure edit: `in` is the current shortcuts.vdf (NULL and 0 for no file), `*out` is a new
 * buffer holding it with the entry added or updated, released with nk_steam_free, and `*change`
 * (optional) says which. On any status other than NK_STEAM_OK, *out is NULL and *change NONE.
 * The result is parsed again, and must hold the shortcut, before it is returned. */
NkSteamStatus nk_steam_vdf_add(const uint8_t *in, size_t in_len, const NkSteamShortcut *shortcut,
                               uint8_t **out, size_t *out_len, NkSteamChange *change);
void nk_steam_free(void *buffer);

/* Whether `in` lists an entry with this executable (compared without case, quotes ignored).
 * CORRUPT when it does not parse. */
NkSteamStatus nk_steam_vdf_has_exe(const uint8_t *in, size_t in_len, const char *exe, bool *found);

/* The file-level add: `vdf_path` is the full path of shortcuts.vdf (its folder is made when
 * missing and the file is created when absent). `steam_running` is the caller's answer to
 * "is Steam running"; true refuses. `stamp` is the backup suffix, 1 to 32 of [0-9A-Za-z-]
 * (the caller passes YYYYMMDD-HHMMSS). `backup_path` (optional) receives the backup's path, or
 * "" when there was no file to back up. `change` (optional) says what was written. */
NkSteamStatus nk_steam_add_to_file(const char *vdf_path, const NkSteamShortcut *shortcut,
                                   bool steam_running, const char *stamp,
                                   char *backup_path, size_t backup_cap, NkSteamChange *change);

/* ---- Finding Steam ---- */

typedef struct {
    char id[24];                       /* the account folder name: digits, the 32-bit account ID */
    char shortcuts_path[NK_MAX_PATH];  /* <Steam>/userdata/<id>/config/shortcuts.vdf */
} NkSteamAccount;

/* Steam's folder from this user's settings (Windows: HKCU\Software\Valve\Steam
 * SteamPath). NK_STEAM_ERR_UNSUPPORTED on systems other than Windows and Linux,
 * NK_STEAM_ERR_NOT_FOUND when Steam is not installed for this user or its folder is gone. Linux
 * checks `$HOME/.local/share/Steam` and `$HOME/.steam/steam`, preferring a root with `userdata/`. */
NkSteamStatus nk_steam_find_root(char *root, size_t root_cap);

/* The accounts that have signed in for this user: the all-digit folders of <root>/userdata other than
 * "0", in ascending order, at most `cap`. `*count` is set. NOT_FOUND when there are none. */
NkSteamStatus nk_steam_list_accounts(const char *root, NkSteamAccount *accounts, size_t cap,
                                     size_t *count);

/* The name an account shows in Steam, so a user with several accounts can tell them apart: the
 * "PersonaName" of the account's entry in <root>/config/loginusers.vdf (a text KeyValues file,
 * keyed by the 64-bit Steam ID, 76561197960265728 + the account ID). Control characters are
 * dropped and a long name is cut at a whole UTF-8 character. NOT_FOUND when the file or the
 * account is missing or the file does not parse; the name is then "". Shown only on this machine,
 * never logged. */
NkSteamStatus nk_steam_account_name(const char *root, const char *account_id, char *name,
                                    size_t cap);
/* The same from the file's text (at most 1 MiB); true when a name was found. */
bool nk_steam_login_users_name(const char *text, size_t length, const char *account_id,
                               char *name, size_t cap);

/* Whether the Steam client runs now (Windows: its active-process registry PID; Linux: the PID in
 * `$HOME/.steam/steam.pid`). True when it cannot tell (fail closed: the file is only written when
 * Steam is surely closed), false when Steam is not running. Other platforms return false and
 * nk_steam_add_for_account requires a caller-supplied probe there. */
bool nk_steam_client_running(void);

/* The fail-closed rules of nk_steam_client_running as functions of a Win32 error number, available
 * on every host so a test can check them: only "that process does not exist" (87) and "that
 * registry value does not exist" (2, 3) mean Steam is not running; every other error, access denied
 * above all, means it may be. */
bool nk_steam_open_error_means_running(unsigned long error);
bool nk_steam_registry_error_means_running(unsigned long error);

/* The whole step for one account: `account_id` must be one of the digit-only folder names of
 * nk_steam_list_accounts. `running` is nk_steam_client_running unless a caller (a test) names
 * another probe. */
typedef bool (*NkSteamRunningFn)(void);
NkSteamStatus nk_steam_add_for_account(const char *root, const char *account_id,
                                       const NkSteamShortcut *shortcut, NkSteamRunningFn running,
                                       const char *stamp, char *backup_path, size_t backup_cap,
                                       NkSteamChange *change);

/* ---- Steam shortcut grid artwork (#2571) -------------------------------------------------- */

/* Maximum accepted byte length for one local ICON0.PNG / PIC1.PNG artwork picture. */
#define NK_STEAM_GRID_MAX_BYTES (1u * 1024u * 1024u)

/* Maximum byte cap accepted when backing up an existing user-customized grid file before an
 * explicit overwrite. */
#define NK_STEAM_GRID_EXISTING_BACKUP_MAX_BYTES (16u * 1024u * 1024u)

/* Slots populated in `<Steam>/userdata/<account_id>/config/grid/`:
 *   - ICON (<appid>_icon.png): game's ICON0.PNG (144x80 PSP icon); Steam reads a shortcut's
 *     icon only from its "icon" field, which a new entry sets to this file's full path
 *   - WIDE (<appid>.png):      game's ICON0.PNG (or PIC1.PNG if ICON0.PNG is absent)
 *   - HERO (<appid>_hero.png): game's PIC1.PNG (480x272 PSP background picture)
 * Steam's vertical portrait capsule (`<appid>p.png`, 2:3) and logo (`<appid>_logo.png`) have
 * no fitting PSP source image and are left empty rather than cropping, stretching or inventing
 * artwork. */
typedef enum {
    NK_STEAM_GRID_SLOT_ICON = 1u << 0,
    NK_STEAM_GRID_SLOT_WIDE = 1u << 1,
    NK_STEAM_GRID_SLOT_HERO = 1u << 2
} NkSteamGridSlot;

typedef enum {
    NK_STEAM_GRID_KEEP_EXISTING = 0,  /* write missing slots only; keep existing files untouched */
    NK_STEAM_GRID_FAIL_IF_EXISTS = 1, /* refuse before writing any file if a target exists */
    NK_STEAM_GRID_OVERWRITE = 2       /* replace existing slot files (rollback on failure) */
} NkSteamGridOverwrite;

typedef struct {
    const uint8_t *icon0_png;
    size_t icon0_size;
    const uint8_t *pic1_png;
    size_t pic1_size;
} NkSteamGridArt;

typedef struct {
    uint32_t app_id;                  /* 32-bit shortcut appid used for grid filenames */
    unsigned written_mask;            /* NkSteamGridSlot bits written and read-back verified */
    unsigned kept_mask;               /* NkSteamGridSlot bits left untouched (already existed) */
    unsigned absent_mask;             /* NkSteamGridSlot bits omitted (no source picture) */
    char grid_dir[NK_MAX_PATH];       /* `<Steam>/userdata/<id>/config/grid`, or "" */
    char failed_path[NK_MAX_PATH];    /* full path of the failing file, if any */
    char detail[256];                 /* plain-English kept-file list or failure reason */
    char skipped[256];                /* the game's pictures left out and why, or "" */
    bool restore_failed;              /* the artwork failed after shortcuts.vdf changed and the
                                         list could not be put back: the backup is kept */
} NkSteamGridResult;

/* Formats `<grid_dir>/<app_id>_icon.png`, `<grid_dir>/<app_id>.png`, or
 * `<grid_dir>/<app_id>_hero.png`. Returns false on invalid arguments or overflow. */
bool nk_steam_grid_slot_path(const char *grid_dir, uint32_t app_id, NkSteamGridSlot slot,
                             char *out, size_t out_cap);

/* Returns the filename suffix for `slot` (`"_icon.png"`, `".png"`, `"_hero.png"`), or NULL. */
const char *nk_steam_grid_slot_suffix(NkSteamGridSlot slot);

/* Looks up the `"appid"` of this shortcut's own entry in `shortcuts.vdf`: the same executable
 * and the same launch options, as Add to Steam matches it (every Nakagawa shortcut runs the same
 * player, so the executable alone names another game's entry). Returns true and writes
 * `*out_app_id` if found with a non-zero id. */
bool nk_steam_vdf_find_app_id(const uint8_t *in, size_t in_len, const NkSteamShortcut *shortcut,
                              uint32_t *out_app_id);

/* Writes `art` into `grid_dir` for `app_id`. Validates PNG signatures and chunks before writing,
 * refuses while `steam_running` is true, honours `overwrite` without silently overwriting an
 * existing picture, verifies every write by reading it back, and on failure names the failing
 * path while rolling back any writes performed during this call. */
NkSteamStatus nk_steam_write_grid_art(const char *grid_dir, uint32_t app_id,
                                      const NkSteamGridArt *art, bool steam_running,
                                      NkSteamGridOverwrite overwrite,
                                      NkSteamGridResult *result);

/* Combines `nk_steam_add_for_account` with writing `art` into the account's `config/grid`
 * folder (using the shortcut's persisted `"appid"` when updated, or `nk_steam_shortcut_app_id`
 * when added). The pictures are optional: one that is not a PNG this code accepts (malformed,
 * over NK_STEAM_GRID_MAX_BYTES, bytes after IEND) is left out, named in `skipped`, and the
 * shortcut is still added. A new entry's `"icon"` field names the copied icon (empty without
 * one); an entry already there, or moved, keeps its own. If writing artwork fails after
 * `shortcuts.vdf` was modified, `shortcuts.vdf` is put back; the backup is deleted only once the
 * list reads back as it was. Otherwise the backup stays in `backup_path`, `*change` still says
 * what changed, and `restore_failed` is set. `art` may be NULL to skip grid artwork. */
NkSteamStatus nk_steam_add_for_account_with_art(const char *root, const char *account_id,
                                                const NkSteamShortcut *shortcut,
                                                NkSteamRunningFn running, const char *stamp,
                                                char *backup_path, size_t backup_cap,
                                                NkSteamChange *change,
                                                const NkSteamGridArt *art,
                                                NkSteamGridOverwrite overwrite,
                                                NkSteamGridResult *grid_result);

#ifdef __cplusplus
}
#endif

#endif /* NK_STEAM_SHORTCUTS_H */

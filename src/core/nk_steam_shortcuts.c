/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Add to Steam (#2152). The layout and rules are in nk_steam_shortcuts.h. */

#include "nk_steam_shortcuts.h"

#include "nk_platform.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#define VDF_MAP 0x00
#define VDF_STRING 0x01
#define VDF_INT32 0x02
#define VDF_FLOAT 0x03
#define VDF_PTR 0x04
#define VDF_COLOR 0x06
#define VDF_UINT64 0x07
#define VDF_END 0x08
#define VDF_INT64 0x0A

#define VDF_MAX_DEPTH 8
#define VDF_MAX_ENTRIES 200000u

const char *nk_steam_status_name(NkSteamStatus status) {
    switch (status) {
        case NK_STEAM_OK: return "OK";
        case NK_STEAM_ERR_ARGUMENT: return "STEAM_ARGUMENT";
        case NK_STEAM_ERR_CORRUPT: return "STEAM_SHORTCUTS_CORRUPT";
        case NK_STEAM_ERR_EXISTS: return "STEAM_SHORTCUT_EXISTS";
        case NK_STEAM_ERR_TOO_LARGE: return "STEAM_SHORTCUTS_TOO_LARGE";
        case NK_STEAM_ERR_STEAM_RUNNING: return "STEAM_RUNNING";
        case NK_STEAM_ERR_NOT_FOUND: return "STEAM_NOT_FOUND";
        case NK_STEAM_ERR_BACKUP: return "STEAM_BACKUP_FAILED";
        case NK_STEAM_ERR_WRITE: return "STEAM_WRITE_FAILED";
        case NK_STEAM_ERR_UNSUPPORTED: return "STEAM_UNSUPPORTED";
    }
    return "STEAM_UNKNOWN";
}

void nk_steam_free(void *buffer) { free(buffer); }

/* ---- CRC-32 (IEEE 802.3), for the app ID Steam derives from a shortcut ---- */

static uint32_t crc32_update(uint32_t crc, const char *data, size_t length) {
    for (size_t i = 0; i < length; i++) {
        crc ^= (uint8_t)data[i];
        for (int bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc;
}

uint32_t nk_steam_shortcut_app_id(const char *exe, const char *app_name) {
    uint32_t crc = 0xFFFFFFFFu;
    crc = crc32_update(crc, "\"", 1u);
    crc = crc32_update(crc, exe, strlen(exe));
    crc = crc32_update(crc, "\"", 1u);
    crc = crc32_update(crc, app_name, strlen(app_name));
    return (crc ^ 0xFFFFFFFFu) | 0x80000000u;
}

/* ---- Reading: a bounds-checked walk ---- */

typedef struct {
    const uint8_t *data;
    size_t size;
    const char *want_exe;     /* the executable to look for, or NULL */
    bool exe_found;
    /* The shortcut being added, or NULL: its own entry (the same executable and launch
     * options) and an entry this one replaces (the same launch options and executable name,
     * another folder). */
    const NkSteamShortcut *want;
    bool same_found;
    bool moved_found;
    size_t moved_exe_at, moved_exe_end;   /* the stored exe value, its NUL included */
    size_t moved_dir_at, moved_dir_end;   /* the stored StartDir value, its NUL included */
    /* The entry being walked (level 2). */
    const char *cur_exe;
    size_t cur_exe_at, cur_exe_end;
    const char *cur_options;
    bool cur_has_dir;
    size_t cur_dir_at, cur_dir_end;
    bool have_shortcuts;
    size_t shortcuts_end;     /* offset of the 0x08 that closes "shortcuts" */
    size_t root_end;          /* offset of the 0x08 that closes the root */
    uint32_t next_index;      /* one past the largest numeric child key */
    uint32_t entries;
} Scan;

static bool ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    }
    return *a == *b;
}

/* The NUL-terminated string at *pos, inside the buffer; advances past the NUL. */
static const char *read_cstr(const Scan *s, size_t *pos) {
    size_t at = *pos;
    if (at >= s->size) return NULL;
    const void *nul = memchr(s->data + at, 0, s->size - at);
    if (!nul) return NULL;
    *pos = (size_t)((const uint8_t *)nul - s->data) + 1u;
    return (const char *)(s->data + at);
}

/* A stored executable is quoted; compare without the quotes. */
static bool exe_matches(const char *stored, const char *want) {
    size_t length = strlen(stored);
    if (length >= 2u && stored[0] == '"' && stored[length - 1u] == '"') {
        stored++;
        length -= 2u;
    }
    size_t want_length = strlen(want);
    if (want_length >= 2u && want[0] == '"' && want[want_length - 1u] == '"') {
        want++;
        want_length -= 2u;
    }
    if (length != want_length) return false;
    for (size_t i = 0; i < length; i++) {
        if (tolower((unsigned char)stored[i]) != tolower((unsigned char)want[i])) return false;
    }
    return true;
}

/* The file name of a path, after its last separator (quotes at either end ignored). */
static void exe_base(const char *path, const char **base, size_t *length) {
    size_t n = strlen(path);
    if (n >= 2u && path[0] == '"' && path[n - 1u] == '"') {
        path++;
        n -= 2u;
    }
    size_t start = n;
    while (start > 0u && path[start - 1u] != '/' && path[start - 1u] != '\\') start--;
    *base = path + start;
    *length = n - start;
}

static bool same_exe_name(const char *stored, const char *want) {
    const char *a, *b;
    size_t la, lb;
    exe_base(stored, &a, &la);
    exe_base(want, &b, &lb);
    if (la != lb || la == 0u) return false;
    for (size_t i = 0; i < la; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    }
    return true;
}

/* After one entry was walked: is it this shortcut, or the one it replaces? */
static void judge_entry(Scan *s) {
    if (!s->want || !s->cur_exe) return;
    const char *options = s->cur_options ? s->cur_options : "";
    const char *want_options = s->want->launch_options ? s->want->launch_options : "";
    if (strcmp(options, want_options) != 0) return;
    if (exe_matches(s->cur_exe, s->want->exe)) {
        s->same_found = true;
        return;
    }
    /* Only an entry with Nakagawa's own launch options names one game; a bare executable
     * could be anything the user added. The first such entry is the one updated. */
    if (want_options[0] && s->cur_has_dir && !s->moved_found &&
        same_exe_name(s->cur_exe, s->want->exe)) {
        s->moved_found = true;
        s->moved_exe_at = s->cur_exe_at;
        s->moved_exe_end = s->cur_exe_end;
        s->moved_dir_at = s->cur_dir_at;
        s->moved_dir_end = s->cur_dir_end;
    }
}

static bool all_digits(const char *text) {
    if (!*text) return false;
    for (; *text; text++) {
        if (*text < '0' || *text > '9') return false;
    }
    return true;
}

/* Walks the entries of one map from *pos to its closing 0x08, which it consumes. `level`:
 * 0 the root, 1 inside "shortcuts", 2 inside one entry, 3 anywhere else. */
static bool scan_map(Scan *s, size_t *pos, int depth, int level) {
    if (depth > VDF_MAX_DEPTH) return false;
    for (;;) {
        if (*pos >= s->size) return false;
        uint8_t type = s->data[(*pos)++];
        if (type == VDF_END) {
            if (level == 0) s->root_end = *pos - 1u;
            return true;
        }
        if (++s->entries > VDF_MAX_ENTRIES) return false;
        const char *key = read_cstr(s, pos);
        if (!key) return false;
        switch (type) {
            case VDF_MAP: {
                int child_level = 3;
                if (level == 0 && !s->have_shortcuts && ieq(key, "shortcuts")) {
                    child_level = 1;
                } else if (level == 1) {
                    child_level = 2;
                    if (all_digits(key) && strlen(key) <= 9u) {
                        uint32_t index = (uint32_t)strtoul(key, NULL, 10) + 1u;
                        if (index > s->next_index) s->next_index = index;
                    }
                    s->cur_exe = NULL;
                    s->cur_options = NULL;
                    s->cur_has_dir = false;
                }
                if (!scan_map(s, pos, depth + 1, child_level)) return false;
                if (child_level == 2) judge_entry(s);
                if (child_level == 1) {
                    s->have_shortcuts = true;
                    s->shortcuts_end = *pos - 1u;
                }
                break;
            }
            case VDF_STRING: {
                size_t value_at = *pos;
                const char *value = read_cstr(s, pos);
                if (!value) return false;
                if (level == 2 && s->want_exe && ieq(key, "exe") &&
                    exe_matches(value, s->want_exe)) {
                    s->exe_found = true;
                }
                if (level == 2 && ieq(key, "exe")) {
                    s->cur_exe = value;
                    s->cur_exe_at = value_at;
                    s->cur_exe_end = *pos;
                } else if (level == 2 && ieq(key, "StartDir")) {
                    s->cur_has_dir = true;
                    s->cur_dir_at = value_at;
                    s->cur_dir_end = *pos;
                } else if (level == 2 && ieq(key, "LaunchOptions")) {
                    s->cur_options = value;
                }
                break;
            }
            case VDF_INT32:
            case VDF_FLOAT:
            case VDF_PTR:
            case VDF_COLOR:
                if (s->size - *pos < 4u) return false;
                *pos += 4u;
                break;
            case VDF_UINT64:
            case VDF_INT64:
                if (s->size - *pos < 8u) return false;
                *pos += 8u;
                break;
            default:
                return false;
        }
    }
}

/* Parses a whole file: the root map, then nothing. */
static NkSteamStatus scan_file(Scan *s, const uint8_t *data, size_t size, const char *want_exe,
                               const NkSteamShortcut *want) {
    memset(s, 0, sizeof(*s));
    s->data = data;
    s->size = size;
    s->want_exe = want_exe;
    s->want = want;
    if (size > NK_STEAM_VDF_MAX_BYTES) return NK_STEAM_ERR_TOO_LARGE;
    size_t pos = 0;
    if (!scan_map(s, &pos, 0, 0) || pos != size) return NK_STEAM_ERR_CORRUPT;
    return NK_STEAM_OK;
}

NkSteamStatus nk_steam_vdf_has_exe(const uint8_t *in, size_t in_len, const char *exe, bool *found) {
    if (!exe || !exe[0] || !found || (!in && in_len)) return NK_STEAM_ERR_ARGUMENT;
    *found = false;
    if (!in_len) return NK_STEAM_OK;
    Scan scan;
    NkSteamStatus status = scan_file(&scan, in, in_len, exe, NULL);
    if (status != NK_STEAM_OK) return status;
    *found = scan.exe_found;
    return NK_STEAM_OK;
}

/* ---- Writing ---- */

typedef struct {
    uint8_t *data;
    size_t size;
    size_t cap;
    bool failed;
} Buf;

static void buf_put(Buf *b, const void *bytes, size_t count) {
    if (b->failed) return;
    if (count > NK_STEAM_VDF_MAX_BYTES || b->size > NK_STEAM_VDF_MAX_BYTES - count) {
        b->failed = true;
        return;
    }
    if (b->size + count > b->cap) {
        size_t cap = b->cap ? b->cap : 512u;
        while (cap < b->size + count) cap *= 2u;
        uint8_t *grown = (uint8_t *)realloc(b->data, cap);
        if (!grown) {
            b->failed = true;
            return;
        }
        b->data = grown;
        b->cap = cap;
    }
    if (count) memcpy(b->data + b->size, bytes, count);
    b->size += count;
}

static void buf_byte(Buf *b, uint8_t value) { buf_put(b, &value, 1u); }
static void buf_cstr(Buf *b, const char *text) { buf_put(b, text, strlen(text) + 1u); }

static void buf_string_entry(Buf *b, const char *key, const char *value) {
    buf_byte(b, VDF_STRING);
    buf_cstr(b, key);
    buf_cstr(b, value);
}

static void buf_int_entry(Buf *b, const char *key, uint32_t value) {
    uint8_t le[4] = {(uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16),
                     (uint8_t)(value >> 24)};
    buf_byte(b, VDF_INT32);
    buf_cstr(b, key);
    buf_put(b, le, 4u);
}

/* A field is a non-empty (unless allowed) UTF-8 text of printable characters. The bytes of a
 * multi-byte character are all >= 0x80, so only ASCII controls and DEL are refused. */
static bool field_ok(const char *text, bool allow_empty) {
    if (!text) return false;
    size_t length = strlen(text);
    if ((!length && !allow_empty) || length > NK_STEAM_FIELD_MAX) return false;
    for (size_t i = 0; i < length; i++) {
        uint8_t c = (uint8_t)text[i];
        if (c < 0x20 || c == 0x7F) return false;
    }
    return true;
}

static void put_quoted(Buf *b, const char *key, const char *path) {
    buf_byte(b, VDF_STRING);
    buf_cstr(b, key);
    buf_put(b, "\"", 1u);
    buf_put(b, path, strlen(path));
    buf_put(b, "\"\0", 2u);
}

static void put_entry(Buf *b, uint32_t index, const NkSteamShortcut *sc) {
    char key[16];
    snprintf(key, sizeof(key), "%u", (unsigned)index);
    buf_byte(b, VDF_MAP);
    buf_cstr(b, key);
    buf_int_entry(b, "appid", nk_steam_shortcut_app_id(sc->exe, sc->app_name));
    buf_string_entry(b, "appname", sc->app_name);
    put_quoted(b, "exe", sc->exe);
    put_quoted(b, "StartDir", sc->start_dir);
    buf_string_entry(b, "icon", "");
    buf_string_entry(b, "ShortcutPath", "");
    buf_string_entry(b, "LaunchOptions", sc->launch_options ? sc->launch_options : "");
    buf_int_entry(b, "IsHidden", 0u);
    buf_int_entry(b, "AllowDesktopConfig", 1u);
    buf_int_entry(b, "AllowOverlay", 1u);
    buf_int_entry(b, "OpenVR", 0u);
    buf_int_entry(b, "Devkit", 0u);
    buf_string_entry(b, "DevkitGameID", "");
    buf_int_entry(b, "DevkitOverrideAppID", 0u);
    buf_int_entry(b, "LastPlayTime", 0u);
    buf_string_entry(b, "FlatpakAppID", "");
    buf_string_entry(b, "sortas", "");
    buf_byte(b, VDF_MAP);
    buf_cstr(b, "tags");
    buf_byte(b, VDF_END);
    buf_byte(b, VDF_END);
}

/* The bytes of `in` with the entry at `at` rewritten to point at this shortcut's executable
 * and folder; every other byte, the entry's app ID, name, play time and tags included, stays. */
static void put_moved(Buf *b, const uint8_t *in, size_t in_len, const Scan *scan,
                      const NkSteamShortcut *sc) {
    bool exe_first = scan->moved_exe_at < scan->moved_dir_at;
    size_t first_at = exe_first ? scan->moved_exe_at : scan->moved_dir_at;
    size_t first_end = exe_first ? scan->moved_exe_end : scan->moved_dir_end;
    size_t second_at = exe_first ? scan->moved_dir_at : scan->moved_exe_at;
    size_t second_end = exe_first ? scan->moved_dir_end : scan->moved_exe_end;
    const char *first = exe_first ? sc->exe : sc->start_dir;
    const char *second = exe_first ? sc->start_dir : sc->exe;
    buf_put(b, in, first_at);
    buf_put(b, "\"", 1u);
    buf_put(b, first, strlen(first));
    buf_put(b, "\"\0", 2u);
    buf_put(b, in + first_end, second_at - first_end);
    buf_put(b, "\"", 1u);
    buf_put(b, second, strlen(second));
    buf_put(b, "\"\0", 2u);
    buf_put(b, in + second_end, in_len - second_end);
}

NkSteamStatus nk_steam_vdf_add(const uint8_t *in, size_t in_len, const NkSteamShortcut *shortcut,
                               uint8_t **out, size_t *out_len, NkSteamChange *change) {
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (change) *change = NK_STEAM_CHANGE_NONE;
    if (!shortcut || !out || !out_len || (!in && in_len)) return NK_STEAM_ERR_ARGUMENT;
    if (!field_ok(shortcut->app_name, false) || !field_ok(shortcut->exe, false) ||
        shortcut->exe[0] == '"' || !field_ok(shortcut->start_dir, false) || shortcut->start_dir[0] == '"' ||
        (shortcut->launch_options && !field_ok(shortcut->launch_options, true))) {
        return NK_STEAM_ERR_ARGUMENT;
    }
    Scan scan;
    Buf b = {0};
    uint32_t index = 0;
    NkSteamChange made = NK_STEAM_CHANGE_ADDED;
    if (in_len) {
        NkSteamStatus status = scan_file(&scan, in, in_len, NULL, shortcut);
        if (status != NK_STEAM_OK) return status;
        if (scan.same_found) return NK_STEAM_ERR_EXISTS;
        index = scan.next_index;
        if (scan.moved_found) {
            put_moved(&b, in, in_len, &scan, shortcut);
            made = NK_STEAM_CHANGE_UPDATED;
        } else if (scan.have_shortcuts) {
            buf_put(&b, in, scan.shortcuts_end);
            put_entry(&b, index, shortcut);
            buf_put(&b, in + scan.shortcuts_end, in_len - scan.shortcuts_end);
        } else {
            buf_put(&b, in, scan.root_end);
            buf_byte(&b, VDF_MAP);
            buf_cstr(&b, "shortcuts");
            put_entry(&b, index, shortcut);
            buf_byte(&b, VDF_END);
            buf_put(&b, in + scan.root_end, in_len - scan.root_end);
        }
    } else {
        buf_byte(&b, VDF_MAP);
        buf_cstr(&b, "shortcuts");
        put_entry(&b, 0u, shortcut);
        buf_byte(&b, VDF_END);
        buf_byte(&b, VDF_END);
    }
    if (b.failed) {
        free(b.data);
        return NK_STEAM_ERR_TOO_LARGE;
    }
    /* The result must parse and now hold this shortcut, or it is not returned. */
    NkSteamStatus verify = scan_file(&scan, b.data, b.size, NULL, shortcut);
    if (verify != NK_STEAM_OK || !scan.same_found || !scan.have_shortcuts) {
        free(b.data);
        return NK_STEAM_ERR_WRITE;
    }
    *out = b.data;
    *out_len = b.size;
    if (change) *change = made;
    return NK_STEAM_OK;
}

/* ---- Files ---- */

/* The whole file into a new buffer. *exists tells a missing file (which is fine) from a
 * failed read. */
static NkSteamStatus read_file(const char *path, uint8_t **data, size_t *size, bool *exists) {
    *data = NULL;
    *size = 0;
    *exists = false;
    FILE *f = nk_fopen_utf8(path, "rb");
    if (!f) return nk_platform_path_missing(path) ? NK_STEAM_OK : NK_STEAM_ERR_WRITE;
    *exists = true;
    uint8_t *buffer = NULL;
    size_t length = 0, cap = 0;
    for (;;) {
        if (length == cap) {
            cap = cap ? cap * 2u : 4096u;
            uint8_t *grown = (uint8_t *)realloc(buffer, cap);
            if (!grown) {
                free(buffer);
                fclose(f);
                return NK_STEAM_ERR_WRITE;
            }
            buffer = grown;
        }
        size_t got = fread(buffer + length, 1u, cap - length, f);
        length += got;
        if (length > NK_STEAM_VDF_MAX_BYTES) {
            free(buffer);
            fclose(f);
            return NK_STEAM_ERR_TOO_LARGE;
        }
        if (got == 0u) break;
    }
    bool bad = ferror(f) != 0;
    fclose(f);
    if (bad) {
        free(buffer);
        return NK_STEAM_ERR_WRITE;
    }
    *data = buffer;
    *size = length;
    return NK_STEAM_OK;
}

static bool write_file_synced(const char *path, const uint8_t *data, size_t size) {
    FILE *f = nk_fopen_utf8(path, "wb");
    if (!f) return false;
    bool ok = (size == 0u || fwrite(data, 1u, size, f) == size) && fflush(f) == 0 &&
              nk_platform_sync_file(f);
    ok = (fclose(f) == 0) && ok;
    return ok;
}

/* True when the file at `path` holds exactly these bytes. */
static bool file_equals(const char *path, const uint8_t *data, size_t size) {
    uint8_t *back = NULL;
    size_t back_size = 0;
    bool exists = false;
    if (read_file(path, &back, &back_size, &exists) != NK_STEAM_OK || !exists) {
        free(back);
        return false;
    }
    bool same = back_size == size && (size == 0u || memcmp(back, data, size) == 0);
    free(back);
    return same;
}

static bool stamp_ok(const char *stamp) {
    if (!stamp || !stamp[0]) return false;
    size_t length = strlen(stamp);
    if (length > 32u) return false;
    for (size_t i = 0; i < length; i++) {
        if (!isalnum((unsigned char)stamp[i]) && stamp[i] != '-') return false;
    }
    return true;
}

static bool parent_dir(const char *path, char *dir, size_t cap) {
    size_t length = strlen(path);
    while (length > 0u && !nk_platform_is_path_separator(path[length - 1u])) length--;
    if (length <= 1u || length >= cap) return false;
    memcpy(dir, path, length - 1u);
    dir[length - 1u] = '\0';
    return true;
}

NkSteamStatus nk_steam_add_to_file(const char *vdf_path, const NkSteamShortcut *shortcut,
                                   bool steam_running, const char *stamp,
                                   char *backup_path, size_t backup_cap, NkSteamChange *change) {
    if (backup_path && backup_cap) backup_path[0] = '\0';
    if (change) *change = NK_STEAM_CHANGE_NONE;
    if (!vdf_path || !vdf_path[0] || !shortcut || !stamp_ok(stamp)) return NK_STEAM_ERR_ARGUMENT;
    if (steam_running) return NK_STEAM_ERR_STEAM_RUNNING;

    uint8_t *current = NULL;
    size_t current_size = 0;
    bool existed = false;
    NkSteamStatus status = read_file(vdf_path, &current, &current_size, &existed);
    if (status != NK_STEAM_OK) return status;

    uint8_t *next = NULL;
    size_t next_size = 0;
    NkSteamChange made = NK_STEAM_CHANGE_NONE;
    status = nk_steam_vdf_add(current, current_size, shortcut, &next, &next_size, &made);
    if (status != NK_STEAM_OK) {
        free(current);
        return status;
    }

    char dir[NK_MAX_PATH];
    char backup[NK_MAX_PATH];
    char temp[NK_MAX_PATH];
    backup[0] = '\0';
    status = NK_STEAM_ERR_WRITE;
    if (!parent_dir(vdf_path, dir, sizeof(dir)) || !nk_platform_mkdir_p(dir) ||
        snprintf(temp, sizeof(temp), "%s.nakagawa-tmp", vdf_path) >= (int)sizeof(temp)) {
        goto done;
    }
    if (existed) {
        /* A dated backup beside the file, never over an earlier one; if it cannot be made and
         * read back, nothing is written. */
        status = NK_STEAM_ERR_BACKUP;
        bool backup_ready = false;
        for (unsigned attempt = 0; attempt < 100u && !backup_ready; attempt++) {
            int n = attempt == 0u
                ? snprintf(backup, sizeof(backup), "%s.nakagawa-backup-%s", vdf_path, stamp)
                : snprintf(backup, sizeof(backup), "%s.nakagawa-backup-%s-%u", vdf_path, stamp,
                           attempt + 1u);
            if (n <= 0 || (size_t)n >= sizeof(backup)) goto done;
            if (!nk_platform_path_missing(backup)) continue;
            backup_ready = true;
        }
        if (!backup_ready || !write_file_synced(backup, current, current_size) ||
            !file_equals(backup, current, current_size)) {
            (void)nk_remove_utf8(backup);
            backup[0] = '\0';
            goto done;
        }
    }
    /* Reported as soon as it is verified: a write that fails after this still leaves the backup. */
    if (backup_path && backup_cap && backup[0] && strlen(backup) < backup_cap) {
        memcpy(backup_path, backup, strlen(backup) + 1u);
    }
    status = NK_STEAM_ERR_WRITE;
    if (!write_file_synced(temp, next, next_size) || !file_equals(temp, next, next_size) ||
        nk_rename_utf8(temp, vdf_path) != 0) {
        (void)nk_remove_utf8(temp);
        goto done;
    }
    if (!file_equals(vdf_path, next, next_size)) goto done;
    status = NK_STEAM_OK;
    if (change) *change = made;
done:
    free(current);
    free(next);
    return status;
}

/* ---- Finding Steam ---- */

typedef struct {
    NkSteamAccount *accounts;
    size_t cap;
    size_t count;
    const char *userdata;
    bool overflow;
} AccountList;

static int account_compare(const void *a, const void *b) {
    const NkSteamAccount *x = (const NkSteamAccount *)a;
    const NkSteamAccount *y = (const NkSteamAccount *)b;
    size_t lx = strlen(x->id), ly = strlen(y->id);
    if (lx != ly) return lx < ly ? -1 : 1;
    return strcmp(x->id, y->id);
}

static bool collect_account(const char *name, bool is_dir, void *context) {
    AccountList *list = (AccountList *)context;
    /* An account folder is all digits and not "0" (the anonymous user). */
    if (!is_dir || !all_digits(name) || strlen(name) >= sizeof(list->accounts[0].id) ||
        strcmp(name, "0") == 0) {
        return true;
    }
    if (list->count >= list->cap) {
        list->overflow = true;
        return true;
    }
    NkSteamAccount *account = &list->accounts[list->count];
    memcpy(account->id, name, strlen(name) + 1u);
    if (snprintf(account->shortcuts_path, sizeof(account->shortcuts_path),
                 "%s%c%s%cconfig%cshortcuts.vdf", list->userdata, nk_platform_path_separator(),
                 name, nk_platform_path_separator(), nk_platform_path_separator()) >=
        (int)sizeof(account->shortcuts_path)) {
        return true;
    }
    list->count++;
    return true;
}

NkSteamStatus nk_steam_list_accounts(const char *root, NkSteamAccount *accounts, size_t cap,
                                     size_t *count) {
    if (count) *count = 0;
    if (!root || !root[0] || !accounts || !count || cap == 0u) return NK_STEAM_ERR_ARGUMENT;
    char userdata[NK_MAX_PATH];
    if (snprintf(userdata, sizeof(userdata), "%s%cuserdata", root,
                 nk_platform_path_separator()) >= (int)sizeof(userdata) ||
        !nk_platform_dir_exists(userdata)) {
        return NK_STEAM_ERR_NOT_FOUND;
    }
    AccountList list = {accounts, cap, 0u, userdata, false};
    if (!nk_platform_list_dir(userdata, collect_account, &list)) return NK_STEAM_ERR_NOT_FOUND;
    qsort(accounts, list.count, sizeof(accounts[0]), account_compare);
    *count = list.count;
    return list.count ? NK_STEAM_OK : NK_STEAM_ERR_NOT_FOUND;
}

/* ---- Account names: <Steam>/config/loginusers.vdf, a text KeyValues file ---- */

#define LOGIN_USERS_MAX_BYTES (1024u * 1024u)
#define LOGIN_TOKEN_MAX 256u
#define STEAM_ID64_BASE 76561197960265728ull

typedef struct {
    const char *p;
    const char *end;
} Text;

enum { TOKEN_ERROR = -1, TOKEN_END = 0, TOKEN_STRING = 1, TOKEN_OPEN = '{', TOKEN_CLOSE = '}' };

/* The next token: a quoted string (\\, \", \n and \t escapes; a longer one is cut at the
 * buffer), an opening or closing brace, or the end. A // comment runs to the end of its line. */
static int text_token(Text *t, char *buffer, size_t cap) {
    for (;;) {
        while (t->p < t->end && (unsigned char)*t->p <= ' ') t->p++;
        if (t->end - t->p >= 2 && t->p[0] == '/' && t->p[1] == '/') {
            while (t->p < t->end && *t->p != '\n') t->p++;
            continue;
        }
        break;
    }
    if (t->p >= t->end) return TOKEN_END;
    char c = *t->p++;
    if (c == '{') return TOKEN_OPEN;
    if (c == '}') return TOKEN_CLOSE;
    if (c != '"') return TOKEN_ERROR;
    size_t n = 0;
    while (t->p < t->end) {
        char ch = *t->p++;
        if (ch == '"') {
            buffer[n] = '\0';
            return TOKEN_STRING;
        }
        if (ch == '\\' && t->p < t->end) {
            char escaped = *t->p++;
            ch = escaped == 'n' ? '\n' : escaped == 't' ? '\t' : escaped;
        }
        if (n + 1u < cap) buffer[n++] = ch;
    }
    return TOKEN_ERROR;
}

/* Copies a display name: control characters dropped, cut at a whole UTF-8 character. */
static bool copy_name(const char *value, char *name, size_t cap) {
    size_t n = 0;
    for (const unsigned char *c = (const unsigned char *)value; *c; c++) {
        if (*c < 0x20u || *c == 0x7Fu) continue;
        if (n + 1u >= cap) break;
        name[n++] = (char)*c;
    }
    /* Do not end inside a multi-byte character. */
    size_t keep = n;
    while (keep > 0u && ((unsigned char)name[keep - 1u] & 0xC0u) == 0x80u) keep--;
    if (keep > 0u && ((unsigned char)name[keep - 1u] & 0x80u) != 0u) {
        unsigned char lead = (unsigned char)name[keep - 1u];
        size_t want = lead >= 0xF0u ? 4u : lead >= 0xE0u ? 3u : lead >= 0xC0u ? 2u : 1u;
        if (n - (keep - 1u) == want) keep = n;
        else keep--;
    } else {
        keep = n;
    }
    name[keep] = '\0';
    return keep > 0u;
}

bool nk_steam_login_users_name(const char *text, size_t length, const char *account_id,
                               char *name, size_t cap) {
    if (name && cap) name[0] = '\0';
    if (!text || !account_id || !all_digits(account_id) || strlen(account_id) > 10u || !name ||
        cap == 0u || length > LOGIN_USERS_MAX_BYTES) {
        return false;
    }
    unsigned long long id = strtoull(account_id, NULL, 10);
    if (id == 0ull || id > 0xFFFFFFFFull) return false;
    char want[24];
    snprintf(want, sizeof(want), "%llu", id + STEAM_ID64_BASE);
    Text t = {text, text + length};
    char key[LOGIN_TOKEN_MAX];
    char value[LOGIN_TOKEN_MAX];
    int depth = 0;
    bool in_users = false;
    bool in_want = false;
    bool found = false;
    for (;;) {
        int token = text_token(&t, key, sizeof(key));
        if (token == TOKEN_END) return depth == 0 && found;
        if (token == TOKEN_CLOSE) {
            if (depth == 0) return false;
            if (depth == 2) in_want = false;
            if (depth == 1) in_users = false;
            depth--;
            continue;
        }
        if (token != TOKEN_STRING) return false;
        token = text_token(&t, value, sizeof(value));
        if (token == TOKEN_OPEN) {
            if (++depth > VDF_MAX_DEPTH) return false;
            if (depth == 1) in_users = ieq(key, "users");
            if (depth == 2) in_want = in_users && strcmp(key, want) == 0;
            continue;
        }
        if (token != TOKEN_STRING) return false;
        if (depth == 2 && in_want && !found && ieq(key, "PersonaName")) {
            found = copy_name(value, name, cap);
        }
    }
}

NkSteamStatus nk_steam_account_name(const char *root, const char *account_id, char *name,
                                    size_t cap) {
    if (name && cap) name[0] = '\0';
    if (!root || !root[0] || !account_id || !name || cap == 0u) return NK_STEAM_ERR_ARGUMENT;
    char path[NK_MAX_PATH];
    if (snprintf(path, sizeof(path), "%s%cconfig%cloginusers.vdf", root,
                 nk_platform_path_separator(), nk_platform_path_separator()) >= (int)sizeof(path)) {
        return NK_STEAM_ERR_NOT_FOUND;
    }
    uint8_t *data = NULL;
    size_t size = 0;
    bool exists = false;
    NkSteamStatus status = read_file(path, &data, &size, &exists);
    if (status != NK_STEAM_OK || !exists) {
        free(data);
        return NK_STEAM_ERR_NOT_FOUND;
    }
    bool named = size <= LOGIN_USERS_MAX_BYTES &&
                 nk_steam_login_users_name((const char *)data, size, account_id, name, cap);
    free(data);
    return named ? NK_STEAM_OK : NK_STEAM_ERR_NOT_FOUND;
}

/* Fail closed (the file is only written when Steam is surely closed): a Win32 error other than "no
 * such process" and "no such registry value" means Steam may be running where this process cannot
 * see it, for example elevated (ERROR_ACCESS_DENIED = 5). Host-independent so a test can call it. */
bool nk_steam_open_error_means_running(unsigned long error) {
    return error != 87ul; /* ERROR_INVALID_PARAMETER: OpenProcess names a pid that is gone */
}

bool nk_steam_registry_error_means_running(unsigned long error) {
    return error != 2ul && error != 3ul; /* ERROR_FILE_NOT_FOUND, ERROR_PATH_NOT_FOUND: never ran */
}

#ifdef _WIN32

static bool registry_string(HKEY root, const wchar_t *subkey, const wchar_t *value, char *out,
                            size_t cap) {
    wchar_t wide[1024];
    DWORD bytes = (DWORD)sizeof(wide) - (DWORD)sizeof(wchar_t);
    memset(wide, 0, sizeof(wide));
    if (RegGetValueW(root, subkey, value, RRF_RT_REG_SZ, NULL, wide, &bytes) != ERROR_SUCCESS) {
        return false;
    }
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, out, (int)cap, NULL, NULL);
    return n > 1;
}

NkSteamStatus nk_steam_find_root(char *root, size_t root_cap) {
    if (!root || root_cap == 0u) return NK_STEAM_ERR_ARGUMENT;
    root[0] = '\0';
    if (!registry_string(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", root,
                         root_cap)) {
        root[0] = '\0';
        return NK_STEAM_ERR_NOT_FOUND;
    }
    for (char *c = root; *c; c++) {
        if (*c == '/') *c = '\\';
    }
    size_t length = strlen(root);
    while (length > 3u && root[length - 1u] == '\\') root[--length] = '\0';
    if (!nk_platform_dir_exists(root)) {
        root[0] = '\0';
        return NK_STEAM_ERR_NOT_FOUND;
    }
    return NK_STEAM_OK;
}

bool nk_steam_client_running(void) {
    DWORD pid = 0;
    DWORD size = (DWORD)sizeof(pid);
    LONG read = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\ActiveProcess", L"pid",
                             RRF_RT_REG_DWORD, NULL, &pid, &size);
    if (read != ERROR_SUCCESS) return nk_steam_registry_error_means_running((unsigned long)read);
    /* Steam writes 0 when it exits cleanly. A pid it left behind after a crash is caught below. */
    if (pid == 0u) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return nk_steam_open_error_means_running((unsigned long)GetLastError());
    DWORD code = 0;
    /* A handle that cannot report its state is a process that exists. */
    bool alive = !GetExitCodeProcess(process, &code) || code == STILL_ACTIVE;
    CloseHandle(process);
    return alive;
}

#else

NkSteamStatus nk_steam_find_root(char *root, size_t root_cap) {
    if (root && root_cap) root[0] = '\0';
    return NK_STEAM_ERR_UNSUPPORTED;
}

bool nk_steam_client_running(void) { return false; }

#endif

NkSteamStatus nk_steam_add_for_account(const char *root, const char *account_id,
                                       const NkSteamShortcut *shortcut, NkSteamRunningFn running,
                                       const char *stamp, char *backup_path, size_t backup_cap,
                                       NkSteamChange *change) {
    if (backup_path && backup_cap) backup_path[0] = '\0';
    if (change) *change = NK_STEAM_CHANGE_NONE;
    if (!root || !root[0] || !account_id || !all_digits(account_id) ||
        strcmp(account_id, "0") == 0 || strlen(account_id) >= sizeof(((NkSteamAccount *)0)->id)) {
        return NK_STEAM_ERR_ARGUMENT;
    }
#ifndef _WIN32
    /* Without a probe of its own, a host that cannot see Steam's process cannot promise that
     * Steam is closed, so it writes nothing. */
    if (!running) return NK_STEAM_ERR_UNSUPPORTED;
#endif
    NkSteamAccount accounts[NK_STEAM_ACCOUNT_CAP];
    size_t count = 0;
    NkSteamStatus status = nk_steam_list_accounts(root, accounts, NK_STEAM_ACCOUNT_CAP, &count);
    if (status != NK_STEAM_OK) return status;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(accounts[i].id, account_id) != 0) continue;
        bool is_running = (running ? running : nk_steam_client_running)();
        return nk_steam_add_to_file(accounts[i].shortcuts_path, shortcut, is_running, stamp,
                                    backup_path, backup_cap, change);
    }
    return NK_STEAM_ERR_NOT_FOUND;
}

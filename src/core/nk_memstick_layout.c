/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The shared Memory Stick's paths and its layout record (#2744); nk_memstick_layout.h has the
   layout and the rules. */

#include "nk_memstick_layout.h"

#include "nk_json.h"
#include "nk_platform.h"
#include "nk_shared_helpers.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIGRATION_PREFIX NK_MEMSTICK_MIGRATION_NAME "/"

static void layout_error(char *err, size_t err_size, const char *format, ...) {
    if (!err || err_size == 0u) return;
    va_list args;
    va_start(args, format);
    int written = vsnprintf(err, err_size, format, args);
    va_end(args);
    if (written < 0) err[0] = '\0';
}

const char *nk_memstick_layout_result_code(NkMemstickLayoutResult result) {
    switch (result) {
        case NK_MEMSTICK_LAYOUT_OK: return "SAVES_LAYOUT_OK";
        case NK_MEMSTICK_LAYOUT_ABSENT: return "SAVES_LAYOUT_ABSENT";
        case NK_MEMSTICK_LAYOUT_NEWER: return "SAVES_LAYOUT_NEWER";
        case NK_MEMSTICK_LAYOUT_DAMAGED: return "SAVES_LAYOUT_DAMAGED";
        case NK_MEMSTICK_LAYOUT_IO: return "SAVES_LAYOUT_IO";
        case NK_MEMSTICK_LAYOUT_INVALID: return "SAVES_LAYOUT_INVALID";
    }
    return "SAVES_LAYOUT_INVALID";
}

const char *nk_memstick_state_name(NkMemstickState state) {
    switch (state) {
        case NK_MEMSTICK_STATE_MOVING: return "moving";
        case NK_MEMSTICK_STATE_READY: return "ready";
    }
    return "";
}

const char *nk_memstick_source_status_name(NkMemstickSourceStatus status) {
    switch (status) {
        case NK_MEMSTICK_SOURCE_MOVED: return "moved";
        case NK_MEMSTICK_SOURCE_PENDING: return "pending";
        case NK_MEMSTICK_SOURCE_LEFT_BY_CHOICE: return "left-by-choice";
        case NK_MEMSTICK_SOURCE_CHANGED_AFTER_MOVE: return "changed-after-move";
        case NK_MEMSTICK_SOURCE_NOT_A_SLOT: return "not-a-slot";
    }
    return "";
}

static bool state_from_name(const char *name, NkMemstickState *out) {
    if (strcmp(name, "moving") == 0) *out = NK_MEMSTICK_STATE_MOVING;
    else if (strcmp(name, "ready") == 0) *out = NK_MEMSTICK_STATE_READY;
    else return false;
    return true;
}

static bool status_from_name(const char *name, NkMemstickSourceStatus *out) {
    static const NkMemstickSourceStatus all[] = {
        NK_MEMSTICK_SOURCE_MOVED, NK_MEMSTICK_SOURCE_PENDING, NK_MEMSTICK_SOURCE_LEFT_BY_CHOICE,
        NK_MEMSTICK_SOURCE_CHANGED_AFTER_MOVE, NK_MEMSTICK_SOURCE_NOT_A_SLOT};
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
        if (strcmp(name, nk_memstick_source_status_name(all[i])) == 0) {
            *out = all[i];
            return true;
        }
    }
    return false;
}

static bool state_known(NkMemstickState state) {
    return state == NK_MEMSTICK_STATE_MOVING || state == NK_MEMSTICK_STATE_READY;
}

static bool status_known(NkMemstickSourceStatus status) {
    return nk_memstick_source_status_name(status)[0] != '\0';
}

/* ---- Paths ---- */

static bool layout_join(const char *saves, const char *name, char out[NK_MAX_PATH]) {
    out[0] = '\0';
    return nk_shared_path_join(out, NK_MAX_PATH, saves, name, nk_platform_path_separator(),
                               NK_SHARED_PATH_JOIN_ALWAYS,
                               NK_SHARED_PATH_REJECT_ABSOLUTE_RIGHT |
                                   NK_SHARED_PATH_REQUIRE_FIT |
                                   NK_SHARED_PATH_REQUIRE_POSITIVE_LENGTH);
}

bool nk_memstick_layout_paths(const char *saves_root, NkMemstickPaths *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!saves_root || !saves_root[0]) return false;
    size_t length = strlen(saves_root);
    if (length >= sizeof(out->saves)) return false;
    memcpy(out->saves, saves_root, length + 1u);
    bool ok = layout_join(out->saves, NK_MEMSTICK_STICK_NAME, out->stick) &&
              layout_join(out->saves, NK_MEMSTICK_STAGING_NAME, out->staging) &&
              layout_join(out->saves, NK_MEMSTICK_RECORD_NAME, out->record) &&
              layout_join(out->saves, NK_MEMSTICK_RECORD_TEMP_NAME, out->record_temp) &&
              layout_join(out->saves, NK_MEMSTICK_MIGRATION_NAME, out->migration) &&
              layout_join(out->saves, NK_MEMSTICK_KEPT_COPIES_NAME, out->kept_copies) &&
              layout_join(out->saves, NK_MEMSTICK_LOCK_NAME, out->lock) &&
              layout_join(out->saves, NK_MEMSTICK_CLAIMS_NAME, out->claims);
    if (!ok) memset(out, 0, sizeof(*out));
    return ok;
}

bool nk_memstick_layout_resolve_paths(NkMemstickPaths *out) {
    if (!out) return false;
    char saves[NK_MAX_PATH];
    if (!nk_platform_resolve_path(NK_PATH_SAVES, saves, sizeof(saves))) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    return nk_memstick_layout_paths(saves, out);
}

static unsigned char ascii_fold(unsigned char byte) {
    return byte >= 'A' && byte <= 'Z' ? (unsigned char)(byte + ('a' - 'A')) : byte;
}

static int ascii_compare_ignoring_case(const char *a, const char *b) {
    while (*a && ascii_fold((unsigned char)*a) == ascii_fold((unsigned char)*b)) {
        ++a;
        ++b;
    }
    return (int)ascii_fold((unsigned char)*a) - (int)ascii_fold((unsigned char)*b);
}

static bool ascii_equal_ignoring_case(const char *a, const char *b) {
    return ascii_compare_ignoring_case(a, b) == 0;
}

bool nk_memstick_layout_is_reserved_name(const char *name) {
    static const char *const reserved[] = {
        NK_MEMSTICK_STICK_NAME, NK_MEMSTICK_STAGING_NAME, NK_MEMSTICK_RECORD_NAME,
        NK_MEMSTICK_RECORD_TEMP_NAME, NK_MEMSTICK_MIGRATION_NAME, NK_MEMSTICK_KEPT_COPIES_NAME,
        NK_MEMSTICK_LOCK_NAME, NK_MEMSTICK_CLAIMS_NAME};
    if (!name) return false;
    for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); ++i) {
        if (ascii_equal_ignoring_case(name, reserved[i])) return true;
    }
    return false;
}

/* ---- Names ---- */

/* No C0 control byte and no DEL: a name the record holds is shown on screen and in the log. */
static bool text_printable(const char *text, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        unsigned char ch = (unsigned char)text[i];
        if (ch < 0x20u || ch == 0x7fu) return false;
    }
    return nk_json_validate_utf8((const uint8_t *)text, length);
}

static bool written_by_valid(const char *text) {
    if (!text) return false;
    size_t length = strlen(text);
    if (length == 0u || length > NK_MEMSTICK_WRITTEN_BY_MAX_BYTES) return false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char ch = (unsigned char)text[i];
        if (ch < 0x20u || ch > 0x7eu) return false;
    }
    return true;
}

/* A Windows device name (CON, PRN, AUX, NUL, COM1-9, LPT1-9), in any ASCII case, alone or before
   an extension: Windows opens the device, not a folder, for "nul" or "COM1.txt". The name before
   the first dot is compared with its trailing spaces dropped, as Windows drops them. */
static bool windows_device_name(const char *name) {
    static const char *const devices[] = {"CON", "PRN", "AUX", "NUL"};
    size_t stem = strcspn(name, ".");
    while (stem > 0u && name[stem - 1u] == ' ') --stem;
    if (stem < 3u || stem > 4u) return false;
    char upper[5];
    for (size_t i = 0; i < stem; ++i) {
        char ch = name[i];
        upper[i] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 'a' + 'A') : ch;
    }
    upper[stem] = '\0';
    if (stem == 3u) {
        for (size_t i = 0; i < sizeof(devices) / sizeof(devices[0]); ++i) {
            if (strcmp(upper, devices[i]) == 0) return true;
        }
        return false;
    }
    return (strncmp(upper, "COM", 3u) == 0 || strncmp(upper, "LPT", 3u) == 0) &&
           upper[3] >= '1' && upper[3] <= '9';
}

/* A single portable path component with no platform separators, special names or aliases. */
static bool component_valid(const char *name) {
    if (!name) return false;
    size_t length = strlen(name);
    if (length == 0u || length > NK_MEMSTICK_SLOT_MAX_BYTES) return false;
    if (strpbrk(name, "<>:\"/\\|?*")) return false;
    if (name[length - 1u] == '.' || name[length - 1u] == ' ') return false;
    if (!text_printable(name, length)) return false;
    if (windows_device_name(name)) return false;
    return true;
}

static bool slot_valid(const char *slot) {
    return component_valid(slot) && !nk_memstick_layout_is_reserved_name(slot);
}

/* A named code: a capital letter, then capitals, digits and underscores. */
static bool reason_valid(const char *reason) {
    size_t length = strlen(reason);
    if (length == 0u || length > NK_MEMSTICK_REASON_MAX_BYTES) return false;
    if (reason[0] < 'A' || reason[0] > 'Z') return false;
    for (size_t i = 1; i < length; ++i) {
        char ch = reason[i];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_')) return false;
    }
    return true;
}

/* A file under .nakagawa-migration/, named with '/' whatever the host: no empty, "." or ".."
   component, no backslash, so it never reaches outside that folder. */
static bool record_path_valid(const char *record) {
    size_t length = strlen(record);
    size_t prefix = sizeof(MIGRATION_PREFIX) - 1u;
    if (length <= prefix || length > NK_MEMSTICK_RECORD_PATH_MAX_BYTES) return false;
    if (strncmp(record, MIGRATION_PREFIX, prefix) != 0) return false;
    if (strchr(record, '\\') || !text_printable(record, length)) return false;
    const char *part = record + prefix;
    for (;;) {
        const char *end = strchr(part, '/');
        size_t part_length = end ? (size_t)(end - part) : strlen(part);
        if (part_length == 0u) return false;
        if (part_length == 1u && part[0] == '.') return false;
        if (part_length == 2u && part[0] == '.' && part[1] == '.') return false;
        if (!end) return true;
        part = end + 1;
    }
}

/* Every check a source passes, in the layout or on its way in. */
static bool source_check(const char *slot, NkMemstickSourceStatus status, const char *record,
                         const char *reason, char *err, size_t err_size) {
    if (!slot_valid(slot)) {
        layout_error(err, err_size, "a source's slot is not a folder name the record can hold");
        return false;
    }
    if (!status_known(status)) {
        layout_error(err, err_size, "source %s has an unknown status", slot);
        return false;
    }
    if (record[0] && !record_path_valid(record)) {
        layout_error(err, err_size, "source %s names a record outside %s", slot,
                     NK_MEMSTICK_MIGRATION_NAME);
        return false;
    }
    if (reason[0] && !reason_valid(reason)) {
        layout_error(err, err_size, "source %s has a reason that is not a named code", slot);
        return false;
    }
    bool needs_record = status == NK_MEMSTICK_SOURCE_MOVED ||
                        status == NK_MEMSTICK_SOURCE_CHANGED_AFTER_MOVE;
    if (needs_record && !record[0]) {
        layout_error(err, err_size, "source %s is %s but names no record", slot,
                     nk_memstick_source_status_name(status));
        return false;
    }
    if (status == NK_MEMSTICK_SOURCE_PENDING && !reason[0]) {
        layout_error(err, err_size, "source %s is pending but gives no reason", slot);
        return false;
    }
    if (status == NK_MEMSTICK_SOURCE_NOT_A_SLOT && record[0]) {
        layout_error(err, err_size, "source %s is not a slot but names a record", slot);
        return false;
    }
    return true;
}

/* ---- The layout in memory ---- */

static char *copy_text(const char *text) {
    size_t length = strlen(text);
    char *copy = (char *)malloc(length + 1u);
    if (copy) memcpy(copy, text, length + 1u);
    return copy;
}

static void source_release(NkMemstickSource *source) {
    free(source->slot);
    free(source->record);
    free(source->reason);
    memset(source, 0, sizeof(*source));
}

bool nk_memstick_layout_init(NkMemstickLayout *layout, NkMemstickState state,
                             const char *written_by) {
    if (!layout) return false;
    memset(layout, 0, sizeof(*layout));
    if (!state_known(state) || !written_by_valid(written_by)) return false;
    layout->schema_version = NK_MEMSTICK_LAYOUT_SCHEMA_VERSION;
    layout->state = state;
    memcpy(layout->written_by, written_by, strlen(written_by) + 1u);
    return true;
}

void nk_memstick_layout_free(NkMemstickLayout *layout) {
    if (!layout) return;
    for (size_t i = 0; i < layout->source_count; ++i) source_release(&layout->sources[i]);
    free(layout->sources);
    memset(layout, 0, sizeof(*layout));
}

const NkMemstickSource *nk_memstick_layout_find_source(const NkMemstickLayout *layout,
                                                       const char *slot) {
    if (!layout || !slot) return NULL;
    for (size_t i = 0; i < layout->source_count; ++i) {
        if (strcmp(layout->sources[i].slot, slot) == 0) return &layout->sources[i];
    }
    return NULL;
}

/* The first of the first `count` sources whose slot is `slot` without ASCII case, or NULL: on
   NTFS and APFS "ulus99999" and "ULUS99999" are one folder, so they are one slot. */
static const NkMemstickSource *find_folded_source(const NkMemstickLayout *layout, size_t count,
                                                  const char *slot) {
    for (size_t i = 0; i < count; ++i) {
        if (ascii_equal_ignoring_case(layout->sources[i].slot, slot)) return &layout->sources[i];
    }
    return NULL;
}

NkMemstickLayoutResult nk_memstick_layout_put_source(NkMemstickLayout *layout, const char *slot,
                                                     NkMemstickSourceStatus status,
                                                     const char *record, const char *reason,
                                                     char *err, size_t err_size) {
    if (!record) record = "";
    if (!reason) reason = "";
    if (!layout || layout->schema_version != NK_MEMSTICK_LAYOUT_SCHEMA_VERSION) {
        layout_error(err, err_size, "the layout is not an initialised version-1 layout");
        return NK_MEMSTICK_LAYOUT_INVALID;
    }
    if (!source_check(slot, status, record, reason, err, err_size)) {
        return NK_MEMSTICK_LAYOUT_INVALID;
    }
    NkMemstickSource *existing = (NkMemstickSource *)nk_memstick_layout_find_source(layout, slot);
    const NkMemstickSource *folded = existing ? NULL
        : find_folded_source(layout, layout->source_count, slot);
    if (folded) {
        layout_error(err, err_size, "source %s is source %s in another case", slot, folded->slot);
        return NK_MEMSTICK_LAYOUT_INVALID;
    }
    if (!existing && layout->source_count >= NK_MEMSTICK_LAYOUT_MAX_SOURCES) {
        layout_error(err, err_size, "the layout already holds %u sources",
                     (unsigned)NK_MEMSTICK_LAYOUT_MAX_SOURCES);
        return NK_MEMSTICK_LAYOUT_INVALID;
    }
    NkMemstickSource fresh = {0};
    fresh.status = status;
    fresh.slot = copy_text(slot);
    fresh.record = copy_text(record);
    fresh.reason = copy_text(reason);
    if (!fresh.slot || !fresh.record || !fresh.reason) {
        source_release(&fresh);
        layout_error(err, err_size, "out of memory");
        return NK_MEMSTICK_LAYOUT_IO;
    }
    if (existing) {
        source_release(existing);
        *existing = fresh;
        return NK_MEMSTICK_LAYOUT_OK;
    }
    if (layout->source_count == layout->source_capacity) {
        size_t capacity = layout->source_capacity ? layout->source_capacity * 2u : 16u;
        if (capacity > NK_MEMSTICK_LAYOUT_MAX_SOURCES) capacity = NK_MEMSTICK_LAYOUT_MAX_SOURCES;
        NkMemstickSource *grown = (NkMemstickSource *)realloc(
            layout->sources, capacity * sizeof(*grown));
        if (!grown) {
            source_release(&fresh);
            layout_error(err, err_size, "out of memory");
            return NK_MEMSTICK_LAYOUT_IO;
        }
        layout->sources = grown;
        layout->source_capacity = capacity;
    }
    layout->sources[layout->source_count++] = fresh;
    return NK_MEMSTICK_LAYOUT_OK;
}

/* A layout format and write accept: what parse would read back as the same layout. */
static bool layout_check(const NkMemstickLayout *layout, char *err, size_t err_size) {
    if (!layout || layout->schema_version != NK_MEMSTICK_LAYOUT_SCHEMA_VERSION) {
        layout_error(err, err_size, "the layout is not version %d",
                     NK_MEMSTICK_LAYOUT_SCHEMA_VERSION);
        return false;
    }
    if (!memchr(layout->written_by, '\0', sizeof(layout->written_by)) ||
        !written_by_valid(layout->written_by)) {
        layout_error(err, err_size, "written_by is not 1 to %u printable ASCII characters",
                     (unsigned)NK_MEMSTICK_WRITTEN_BY_MAX_BYTES);
        return false;
    }
    if (!state_known(layout->state)) {
        layout_error(err, err_size, "the layout's state is unknown");
        return false;
    }
    if (layout->source_count > NK_MEMSTICK_LAYOUT_MAX_SOURCES ||
        (layout->source_count && !layout->sources)) {
        layout_error(err, err_size, "the layout's sources are out of range");
        return false;
    }
    for (size_t i = 0; i < layout->source_count; ++i) {
        const NkMemstickSource *source = &layout->sources[i];
        if (!source->slot || !source->record || !source->reason) {
            layout_error(err, err_size, "source %u is incomplete", (unsigned)i);
            return false;
        }
        if (!source_check(source->slot, source->status, source->record, source->reason,
                          err, err_size)) {
            return false;
        }
        if (find_folded_source(layout, i, source->slot)) {
            layout_error(err, err_size, "source %s is listed twice (ASCII case aside)",
                         source->slot);
            return false;
        }
    }
    return true;
}

/* ---- Parse ---- */

/* The member `key` of `object`; *count is how many members have that key. */
static const NkJsonNode *object_member(const NkJsonNode *object, const char *key,
                                       size_t *count) {
    const NkJsonNode *found = NULL;
    *count = 0u;
    for (size_t i = 0; i < object->u.obj.count; ++i) {
        if (strcmp(object->u.obj.members[i].key, key) == 0) {
            if (!found) found = object->u.obj.members[i].val;
            ++*count;
        }
    }
    return found;
}

/* The string member `key`: NULL with *ok true when absent and optional; *ok false when it is
   repeated, not a string, or absent and required. */
static const char *string_member(const NkJsonNode *object, const char *key, bool required,
                                 bool *ok) {
    size_t count = 0u;
    const NkJsonNode *node = object_member(object, key, &count);
    *ok = count <= 1u && (count == 1u ? node->type == NK_JSON_STRING : !required);
    return *ok && node ? node->u.str_val : NULL;
}

static NkMemstickLayoutResult parse_source(const NkJsonNode *node, size_t index,
                                           NkMemstickLayout *out, char *err, size_t err_size) {
    if (node->type != NK_JSON_OBJECT) {
        layout_error(err, err_size, "source %u is not an object", (unsigned)index);
        return NK_MEMSTICK_LAYOUT_DAMAGED;
    }
    bool slot_ok = false, status_ok = false, record_ok = false, reason_ok = false;
    const char *slot = string_member(node, "slot", true, &slot_ok);
    const char *status_name = string_member(node, "status", true, &status_ok);
    const char *record = string_member(node, "record", false, &record_ok);
    const char *reason = string_member(node, "reason", false, &reason_ok);
    if (!slot_ok || !status_ok || !record_ok || !reason_ok) {
        layout_error(err, err_size, "source %u needs one string slot and status, and at most "
                     "one string record and reason", (unsigned)index);
        return NK_MEMSTICK_LAYOUT_DAMAGED;
    }
    NkMemstickSourceStatus status;
    if (!status_from_name(status_name, &status)) {
        layout_error(err, err_size, "source %u has an unknown status", (unsigned)index);
        return NK_MEMSTICK_LAYOUT_DAMAGED;
    }
    if (find_folded_source(out, out->source_count, slot)) {
        layout_error(err, err_size, "source %s is listed twice (ASCII case aside)", slot);
        return NK_MEMSTICK_LAYOUT_DAMAGED;
    }
    NkMemstickLayoutResult put = nk_memstick_layout_put_source(out, slot, status, record, reason,
                                                               err, err_size);
    return put == NK_MEMSTICK_LAYOUT_INVALID ? NK_MEMSTICK_LAYOUT_DAMAGED : put;
}

/* The members after schema_version, for a version-1 record. */
static NkMemstickLayoutResult parse_body(const NkJsonNode *root, NkMemstickLayout *out,
                                         char *err, size_t err_size) {
    bool kind_ok = false, stick_ok = false, state_ok = false, by_ok = false;
    const char *kind = string_member(root, "layout", true, &kind_ok);
    const char *stick = string_member(root, "stick", true, &stick_ok);
    const char *state_name = string_member(root, "state", true, &state_ok);
    const char *written_by = string_member(root, "written_by", true, &by_ok);
    if (!kind_ok || !stick_ok || !state_ok || !by_ok) {
        layout_error(err, err_size, "it needs exactly one string layout, stick, state and "
                     "written_by");
        return NK_MEMSTICK_LAYOUT_DAMAGED;
    }
    if (strcmp(kind, NK_MEMSTICK_LAYOUT_KIND) != 0 ||
        strcmp(stick, NK_MEMSTICK_STICK_NAME) != 0) {
        layout_error(err, err_size, "a version-1 record is layout \"%s\" with stick \"%s\"",
                     NK_MEMSTICK_LAYOUT_KIND, NK_MEMSTICK_STICK_NAME);
        return NK_MEMSTICK_LAYOUT_DAMAGED;
    }
    NkMemstickState state;
    if (!state_from_name(state_name, &state)) {
        layout_error(err, err_size, "its state is neither moving nor ready");
        return NK_MEMSTICK_LAYOUT_DAMAGED;
    }
    if (!nk_memstick_layout_init(out, state, written_by)) {
        layout_error(err, err_size, "written_by is not 1 to %u printable ASCII characters",
                     (unsigned)NK_MEMSTICK_WRITTEN_BY_MAX_BYTES);
        return NK_MEMSTICK_LAYOUT_DAMAGED;
    }
    size_t count = 0u;
    const NkJsonNode *sources = object_member(root, "sources", &count);
    if (count != 1u || sources->type != NK_JSON_ARRAY) {
        layout_error(err, err_size, "it needs exactly one sources array");
        return NK_MEMSTICK_LAYOUT_DAMAGED;
    }
    size_t source_count = nk_json_array_count(sources);
    if (source_count > NK_MEMSTICK_LAYOUT_MAX_SOURCES) {
        layout_error(err, err_size, "it lists more than %u sources",
                     (unsigned)NK_MEMSTICK_LAYOUT_MAX_SOURCES);
        return NK_MEMSTICK_LAYOUT_DAMAGED;
    }
    for (size_t i = 0; i < source_count; ++i) {
        NkMemstickLayoutResult result = parse_source(nk_json_array_get(sources, i), i, out,
                                                     err, err_size);
        if (result != NK_MEMSTICK_LAYOUT_OK) return result;
    }
    return NK_MEMSTICK_LAYOUT_OK;
}

NkMemstickLayoutResult nk_memstick_layout_parse(const char *text, size_t length,
                                                NkMemstickLayout *out,
                                                char *err, size_t err_size) {
    if (!out) {
        layout_error(err, err_size, "no layout to parse into");
        return NK_MEMSTICK_LAYOUT_INVALID;
    }
    memset(out, 0, sizeof(*out));
    if (!text || length == 0u) {
        layout_error(err, err_size, "it is empty");
        return NK_MEMSTICK_LAYOUT_DAMAGED;
    }
    if (length > NK_MEMSTICK_LAYOUT_MAX_BYTES) {
        layout_error(err, err_size, "it is larger than %u bytes",
                     (unsigned)NK_MEMSTICK_LAYOUT_MAX_BYTES);
        return NK_MEMSTICK_LAYOUT_DAMAGED;
    }
    char json_error[160] = "";
    NkJsonNode *root = nk_json_parse(text, length, json_error, sizeof(json_error));
    if (!root) {
        layout_error(err, err_size, "it is not JSON (%s)", json_error[0] ? json_error : "?");
        return NK_MEMSTICK_LAYOUT_DAMAGED;
    }
    NkMemstickLayoutResult result = NK_MEMSTICK_LAYOUT_DAMAGED;
    size_t count = 0u;
    const NkJsonNode *version = root->type == NK_JSON_OBJECT
        ? object_member(root, "schema_version", &count) : NULL;
    int64_t schema_version = 0;
    if (root->type != NK_JSON_OBJECT) {
        layout_error(err, err_size, "it is not a JSON object");
    } else if (count != 1u || !nk_json_get_int64(version, &schema_version)) {
        layout_error(err, err_size, "it has no single integer schema_version");
    } else if (schema_version > NK_MEMSTICK_LAYOUT_SCHEMA_VERSION) {
        layout_error(err, err_size, "it is schema_version %lld, and this Nakagawa reads %d; "
                     "a later Nakagawa wrote it", (long long)schema_version,
                     NK_MEMSTICK_LAYOUT_SCHEMA_VERSION);
        result = NK_MEMSTICK_LAYOUT_NEWER;
    } else if (schema_version < NK_MEMSTICK_LAYOUT_SCHEMA_VERSION) {
        layout_error(err, err_size, "schema_version %lld is not a version",
                     (long long)schema_version);
    } else {
        result = parse_body(root, out, err, err_size);
    }
    nk_json_free(root);
    if (result != NK_MEMSTICK_LAYOUT_OK) nk_memstick_layout_free(out);
    return result;
}

/* ---- Format ---- */

typedef struct {
    char *bytes;
    size_t length;
    size_t capacity;
    bool failed;
} LayoutText;

static void text_append(LayoutText *text, const char *bytes, size_t length) {
    if (text->failed) return;
    if (length > SIZE_MAX - text->length - 1u) {
        text->failed = true;
        return;
    }
    size_t needed = text->length + length + 1u;
    if (needed > text->capacity) {
        size_t capacity = text->capacity ? text->capacity : 1024u;
        while (capacity < needed) {
            if (capacity > SIZE_MAX / 2u) {
                capacity = needed;
                break;
            }
            capacity *= 2u;
        }
        char *grown = (char *)realloc(text->bytes, capacity);
        if (!grown) {
            text->failed = true;
            return;
        }
        text->bytes = grown;
        text->capacity = capacity;
    }
    memcpy(text->bytes + text->length, bytes, length);
    text->length += length;
    text->bytes[text->length] = '\0';
}

static void text_literal(LayoutText *text, const char *literal) {
    text_append(text, literal, strlen(literal));
}

/* A JSON string. The layout's text is valid UTF-8 with no control byte (layout_check), so only
   the quote and the backslash need an escape. */
static void text_string(LayoutText *text, const char *value) {
    text_literal(text, "\"");
    for (const char *run = value; *run;) {
        size_t plain = strcspn(run, "\"\\");
        text_append(text, run, plain);
        run += plain;
        if (*run) {
            char escaped[2] = {'\\', *run};
            text_append(text, escaped, sizeof(escaped));
            ++run;
        }
    }
    text_literal(text, "\"");
}

static void text_member(LayoutText *text, const char *key, const char *value) {
    text_string(text, key);
    text_literal(text, ": ");
    text_string(text, value);
}

NkMemstickLayoutResult nk_memstick_layout_format(const NkMemstickLayout *layout,
                                                 char **out_text, size_t *out_length,
                                                 char *err, size_t err_size) {
    if (out_text) *out_text = NULL;
    if (out_length) *out_length = 0u;
    if (!out_text || !layout_check(layout, err, err_size)) {
        if (!out_text) layout_error(err, err_size, "no buffer to format into");
        return NK_MEMSTICK_LAYOUT_INVALID;
    }
    LayoutText text = {0};
    char version[32];
    snprintf(version, sizeof(version), "{\n  \"schema_version\": %d,\n  ",
             NK_MEMSTICK_LAYOUT_SCHEMA_VERSION);
    text_literal(&text, version);
    text_member(&text, "layout", NK_MEMSTICK_LAYOUT_KIND);
    text_literal(&text, ",\n  ");
    text_member(&text, "stick", NK_MEMSTICK_STICK_NAME);
    text_literal(&text, ",\n  ");
    text_member(&text, "state", nk_memstick_state_name(layout->state));
    text_literal(&text, ",\n  ");
    text_member(&text, "written_by", layout->written_by);
    text_literal(&text, ",\n  \"sources\": [");
    for (size_t i = 0; i < layout->source_count; ++i) {
        const NkMemstickSource *source = &layout->sources[i];
        text_literal(&text, i ? ",\n    {" : "\n    {");
        text_member(&text, "slot", source->slot);
        text_literal(&text, ", ");
        text_member(&text, "status", nk_memstick_source_status_name(source->status));
        if (source->record[0]) {
            text_literal(&text, ", ");
            text_member(&text, "record", source->record);
        }
        if (source->reason[0]) {
            text_literal(&text, ", ");
            text_member(&text, "reason", source->reason);
        }
        text_literal(&text, "}");
    }
    text_literal(&text, layout->source_count ? "\n  ]\n}\n" : "]\n}\n");
    if (text.failed || !text.bytes) {
        free(text.bytes);
        layout_error(err, err_size, "out of memory");
        return NK_MEMSTICK_LAYOUT_IO;
    }
    *out_text = text.bytes;
    if (out_length) *out_length = text.length;
    return NK_MEMSTICK_LAYOUT_OK;
}

/* ---- Read and write ---- */

/* The whole file into a buffer the caller frees, held to the record's bound: *oversize when it
   is larger. False when it cannot be opened or read, or memory runs out. */
static bool read_record_file(const char *path, char **out_bytes, size_t *out_length,
                             bool *oversize) {
    *out_bytes = NULL;
    *out_length = 0u;
    *oversize = false;
    FILE *file = nk_fopen_utf8(path, "rb");
    if (!file) return false;
    LayoutText text = {0};
    char chunk[16384];
    size_t got;
    while (!text.failed && (got = fread(chunk, 1u, sizeof(chunk), file)) > 0u) {
        if (text.length + got > NK_MEMSTICK_LAYOUT_MAX_BYTES) {
            *oversize = true;
            break;
        }
        text_append(&text, chunk, got);
    }
    bool read_failed = ferror(file) != 0;
    fclose(file);
    if (text.failed || read_failed) {
        free(text.bytes);
        return false;
    }
    *out_bytes = text.bytes;
    *out_length = text.length;
    return true;
}

NkMemstickLayoutResult nk_memstick_layout_read(const char *saves_root, NkMemstickLayout *out,
                                               char *err, size_t err_size) {
    if (out) memset(out, 0, sizeof(*out));
    NkMemstickPaths paths;
    if (!out || !nk_memstick_layout_paths(saves_root, &paths)) {
        layout_error(err, err_size, "SAVES_LAYOUT_INVALID: no saves folder to read from");
        return NK_MEMSTICK_LAYOUT_INVALID;
    }
    if (nk_platform_dir_exists(paths.record)) {
        layout_error(err, err_size, "SAVES_LAYOUT_DAMAGED: %s is a folder, not the layout record",
                     paths.record);
        return NK_MEMSTICK_LAYOUT_DAMAGED;
    }
    if (!nk_platform_file_exists(paths.record)) {
        layout_error(err, err_size, "SAVES_LAYOUT_ABSENT: %s does not exist", paths.record);
        return NK_MEMSTICK_LAYOUT_ABSENT;
    }
    char *bytes = NULL;
    size_t length = 0u;
    bool oversize = false;
    if (!read_record_file(paths.record, &bytes, &length, &oversize)) {
        layout_error(err, err_size, "SAVES_LAYOUT_IO: %s cannot be read; it is left as it is",
                     paths.record);
        return NK_MEMSTICK_LAYOUT_IO;
    }
    char detail[384] = "";
    NkMemstickLayoutResult result;
    if (oversize) {
        snprintf(detail, sizeof(detail), "it is larger than %u bytes",
                 (unsigned)NK_MEMSTICK_LAYOUT_MAX_BYTES);
        result = NK_MEMSTICK_LAYOUT_DAMAGED;
    } else {
        result = nk_memstick_layout_parse(bytes, length, out, detail, sizeof(detail));
    }
    free(bytes);
    if (result != NK_MEMSTICK_LAYOUT_OK) {
        layout_error(err, err_size, "%s: %s: %s", nk_memstick_layout_result_code(result),
                     paths.record, detail);
    }
    return result;
}

NkMemstickLayoutResult nk_memstick_layout_write(const char *saves_root,
                                                const NkMemstickLayout *layout,
                                                char *err, size_t err_size) {
    char detail[384] = "";
    if (!layout_check(layout, detail, sizeof(detail))) {
        layout_error(err, err_size, "SAVES_LAYOUT_INVALID: %s", detail);
        return NK_MEMSTICK_LAYOUT_INVALID;
    }
    NkMemstickPaths paths;
    if (!nk_memstick_layout_paths(saves_root, &paths)) {
        layout_error(err, err_size, "SAVES_LAYOUT_INVALID: no saves folder to write to");
        return NK_MEMSTICK_LAYOUT_INVALID;
    }
    /* The record on the disk decides whether it may be replaced. */
    NkMemstickLayout current;
    NkMemstickLayoutResult found = nk_memstick_layout_read(saves_root, &current, err, err_size);
    nk_memstick_layout_free(&current);
    if (found != NK_MEMSTICK_LAYOUT_OK && found != NK_MEMSTICK_LAYOUT_ABSENT) {
        size_t used = err ? strlen(err) : 0u;
        if (err && used < err_size) {
            layout_error(err + used, err_size - used, "; it is kept and not replaced");
        }
        return found;
    }
    char *text = NULL;
    size_t length = 0u;
    NkMemstickLayoutResult formatted = nk_memstick_layout_format(layout, &text, &length,
                                                                 detail, sizeof(detail));
    if (formatted != NK_MEMSTICK_LAYOUT_OK) {
        layout_error(err, err_size, "%s: %s", nk_memstick_layout_result_code(formatted), detail);
        return formatted;
    }
    FILE *file = nk_fopen_utf8(paths.record_temp, "wb");
    bool written = file != NULL;
    if (file) {
        written = fwrite(text, 1u, length, file) == length;
        written = nk_platform_sync_file(file) && written;
        written = fclose(file) == 0 && written;
    }
    free(text);
    if (!written) {
        if (file) (void)nk_remove_utf8(paths.record_temp);
        layout_error(err, err_size, "SAVES_LAYOUT_IO: %s cannot be written; the record is left "
                     "as it was", paths.record_temp);
        return NK_MEMSTICK_LAYOUT_IO;
    }
    if (nk_rename_utf8(paths.record_temp, paths.record) != 0) {
        (void)nk_remove_utf8(paths.record_temp);
        layout_error(err, err_size, "SAVES_LAYOUT_IO: %s cannot be renamed to %s; the record is "
                     "left as it was", paths.record_temp, paths.record);
        return NK_MEMSTICK_LAYOUT_IO;
    }
    if (!nk_platform_sync_dir(paths.saves)) {
        layout_error(err, err_size, "SAVES_LAYOUT_IO: %s is replaced, but %s could not be put on "
                     "the disk", paths.record, paths.saves);
        return NK_MEMSTICK_LAYOUT_IO;
    }
    if (err && err_size) err[0] = '\0';
    return NK_MEMSTICK_LAYOUT_OK;
}

/* ---- First migration: one colliding PSP/SAVEDATA unit ---- */

static bool unit_plain_directory(const char *path) {
    char resolved[NK_MAX_PATH];
    return nk_platform_dir_exists(path) &&
           nk_platform_real_path(path, resolved, sizeof(resolved)) &&
           (nk_platform_paths_fold_case() ? ascii_equal_ignoring_case(resolved, path)
                                          : strcmp(resolved, path) == 0);
}

typedef struct {
    const char *name;
    char *spelling;
    bool found;
    bool ambiguous;
} UnitLookup;

static bool unit_lookup_entry(const char *name, bool is_dir, void *opaque) {
    UnitLookup *lookup = opaque;
    if (!ascii_equal_ignoring_case(name, lookup->name)) return true;
    if (lookup->found || !is_dir || !nk_shared_text_copy(lookup->spelling, NK_MAX_PATH, name,
                                                       NK_SHARED_TEXT_COPY_REQUIRE_FIT)) {
        lookup->ambiguous = true;
        return false;
    }
    lookup->found = true;
    return true;
}

/* Resolve a directory by PSP spelling. A missing result returns the requested new path.
 * Otherwise require an ordinary, unambiguous directory. */
static bool unit_directory(const char *parent, const char *name, bool create,
                           char out[NK_MAX_PATH]) {
    char spelling[NK_MAX_PATH];
    UnitLookup lookup = {name, spelling, false, false};
    size_t skipped = 0u;
    if (!nk_platform_list_dir_counted(parent, unit_lookup_entry, &lookup, &skipped) ||
        lookup.ambiguous || (!lookup.found && skipped != 0u) ||
        !layout_join(parent, lookup.found ? spelling : name, out)) return false;
    if (lookup.found) return unit_plain_directory(out) && (!create || nk_platform_sync_dir(parent));
    if (!nk_platform_path_missing(out)) return false;
    return !create || (nk_platform_mkdir_p_private(out) && nk_platform_sync_dir(parent));
}

static bool unit_savedata_directory(const char *slot, bool create, char out[NK_MAX_PATH]) {
    char psp[NK_MAX_PATH];
    return unit_directory(slot, "PSP", create, psp) &&
           unit_directory(psp, "SAVEDATA", create, out);
}

/* These folders use the exact names of the layout API. Other entries directly in
 * saves, including a refused legacy slot, do not prevent creating them. */
static bool unit_ensure_layout_directory(const char *saves, const char *path) {
    if (unit_plain_directory(path)) return nk_platform_sync_dir(saves);
    return nk_platform_path_missing(path) && nk_platform_mkdir_p_private(path) &&
           nk_platform_sync_dir(saves);
}

typedef struct {
    char *name;
    bool is_dir;
} UnitEntry;

typedef struct {
    UnitEntry *items;
    size_t count;
    size_t capacity;
    bool failed;
} UnitEntries;

static bool unit_collect_entry(const char *name, bool is_dir, void *opaque) {
    UnitEntries *entries = opaque;
    if (!component_valid(name)) {
        entries->failed = true;
        return false;
    }
    if (entries->count == entries->capacity) {
        size_t capacity = entries->capacity ? entries->capacity * 2u : 16u;
        UnitEntry *items = capacity <= SIZE_MAX / sizeof(*items) && capacity > entries->capacity
            ? realloc(entries->items, capacity * sizeof(*items)) : NULL;
        if (!items) {
            entries->failed = true;
            return false;
        }
        entries->items = items;
        entries->capacity = capacity;
    }
    size_t bytes = strlen(name) + 1u;
    char *copy = malloc(bytes);
    if (!copy) {
        entries->failed = true;
        return false;
    }
    memcpy(copy, name, bytes);
    entries->items[entries->count++] = (UnitEntry){copy, is_dir};
    return true;
}

static int unit_compare_entries(const void *left, const void *right) {
    const UnitEntry *a = left, *b = right;
    return ascii_compare_ignoring_case(a->name, b->name);
}

static bool unit_read_entries(const char *path, UnitEntries *entries) {
    size_t skipped = 0u;
    if (!unit_plain_directory(path) ||
        !nk_platform_list_dir_counted(path, unit_collect_entry, entries, &skipped) ||
        entries->failed || skipped != 0u) return false;
    if (entries->count > 1u)
        qsort(entries->items, entries->count, sizeof(*entries->items), unit_compare_entries);
    for (size_t i = 1; i < entries->count; ++i) {
        if (unit_compare_entries(&entries->items[i - 1u], &entries->items[i]) == 0) return false;
    }
    return true;
}

static void unit_free_entries(UnitEntries *entries) {
    for (size_t i = 0; i < entries->count; ++i) free(entries->items[i].name);
    free(entries->items);
}

typedef struct {
    NkFileWriteTime value;
    bool present;
} UnitWriteTime;

static int unit_compare_times(NkFileWriteTime a, NkFileWriteTime b) {
    if (a.seconds != b.seconds) return a.seconds < b.seconds ? -1 : 1;
    if (a.nanoseconds != b.nanoseconds) return a.nanoseconds < b.nanoseconds ? -1 : 1;
    return 0;
}

/* Hash the bytes actually read, also writing them when we're building an unpublished copy. */
static bool unit_file_digest(const char *path, const char *copy, uint8_t digest[32],
                             UnitWriteTime *latest) {
    FILE *source = nk_platform_open_regular_file(path);
    if (!source) return false;
    FILE *target = copy ? nk_fopen_utf8(copy, "wb") : NULL;
    bool ok = !copy || target;
    NkFileWriteTime modified;
    if (ok && latest) ok = nk_platform_file_write_time(source, &modified);
    NkSha256 hash;
    nk_sha256_init(&hash);
    unsigned char buffer[8192];
    while (ok) {
        size_t count = fread(buffer, 1u, sizeof(buffer), source);
        nk_sha256_update(&hash, buffer, count);
        if (target && fwrite(buffer, 1u, count, target) != count) ok = false;
        if (count < sizeof(buffer)) break;
    }
    ok = !ferror(source) && ok;
    if (ok && latest) {
        NkFileWriteTime after;
        ok = nk_platform_file_write_time(source, &after) && !unit_compare_times(after, modified);
        if (ok && (!latest->present || unit_compare_times(modified, latest->value) > 0))
            *latest = (UnitWriteTime){modified, true};
    }
    ok = fclose(source) == 0 && ok;
    if (target) {
        ok = nk_platform_sync_file(target) && ok;
        ok = fclose(target) == 0 && ok;
    }
    nk_sha256_finish(&hash, digest);
    return ok;
}

/* Each sorted directory hashes typed, NUL-terminated folded names and fixed-size
 * child digests. Empty directories and per-entry types are part of the fingerprint. */
static bool unit_tree_fingerprint(const char *path, const char *copy, uint8_t digest[32],
                                  UnitWriteTime *latest) {
    UnitEntries entries = {0};
    bool ok = unit_read_entries(path, &entries);
    NkSha256 hash;
    nk_sha256_init(&hash);
    for (size_t i = 0; ok && i < entries.count; ++i) {
        const UnitEntry *entry = &entries.items[i];
        char source[NK_MAX_PATH], target[NK_MAX_PATH];
        if (!layout_join(path, entry->name, source) ||
            (copy && !layout_join(copy, entry->name, target))) {
            ok = false;
            break;
        }
        uint8_t child_digest[32];
        if (entry->is_dir) {
            ok = (!copy || nk_platform_mkdir_p_private(target)) &&
                 unit_tree_fingerprint(source, copy ? target : NULL, child_digest, latest);
        } else {
            ok = unit_file_digest(source, copy ? target : NULL, child_digest, latest);
        }
        if (!ok) break;
        const uint8_t type = entry->is_dir ? 'd' : 'f';
        nk_sha256_update(&hash, &type, 1u);
        const unsigned char *name = (const unsigned char *)entry->name;
        for (;;) {
            const uint8_t folded = ascii_fold(*name++);
            nk_sha256_update(&hash, &folded, 1u);
            if (folded == 0u) break;
        }
        nk_sha256_update(&hash, child_digest, sizeof(child_digest));
    }
    unit_free_entries(&entries);
    nk_sha256_finish(&hash, digest);
    return ok && (!copy || nk_platform_sync_dir(copy));
}

static bool unit_tree_digest(const char *path, const char *copy, uint8_t digest[32]) {
    return unit_tree_fingerprint(path, copy, digest, NULL);
}

static bool unit_matches(const char *path, const uint8_t expected[32]) {
    uint8_t actual[32];
    return unit_tree_digest(path, NULL, actual) && memcmp(expected, actual, sizeof(actual)) == 0;
}

static bool move_scratch_directory(const char *migration, char scratch[NK_MAX_PATH]) {
    uint8_t nonce[8];
    if (!nk_platform_random_bytes(nonce, sizeof(nonce))) return false;
    char scratch_name[6u + 2u * sizeof(nonce)];
    snprintf(scratch_name, sizeof(scratch_name), "unit-%02x%02x%02x%02x%02x%02x%02x%02x",
             (unsigned)nonce[0], (unsigned)nonce[1], (unsigned)nonce[2], (unsigned)nonce[3],
             (unsigned)nonce[4], (unsigned)nonce[5], (unsigned)nonce[6], (unsigned)nonce[7]);
    return layout_join(migration, scratch_name, scratch) && nk_platform_path_missing(scratch) &&
           nk_platform_mkdir_p_private(scratch);
}

/* Publish the complete, independently read-back directory with an atomic no-clobber rename.
 * A crash can leave an unused temporary folder in the migration area or a whole output. */
static NkMemstickCollisionResult unit_publish(const char *source, const uint8_t expected[32],
                                               const char *parent, const char *name,
                                               const char *migration) {
    char target[NK_MAX_PATH];
    if (!unit_directory(parent, name, false, target))
        return NK_MEMSTICK_COLLISION_DESTINATION_CONFLICT;
    if (!nk_platform_path_missing(target)) {
        if (!unit_matches(target, expected)) return NK_MEMSTICK_COLLISION_DESTINATION_CONFLICT;
        return nk_platform_sync_dir(parent) ? NK_MEMSTICK_COLLISION_COPIED
                                            : NK_MEMSTICK_COLLISION_IO;
    }
    char scratch[NK_MAX_PATH];
    if (!move_scratch_directory(migration, scratch)) return NK_MEMSTICK_COLLISION_IO;
    uint8_t copied[32];
    NkMemstickCollisionResult result;
    if (!unit_tree_digest(source, scratch, copied)) {
        result = NK_MEMSTICK_COLLISION_IO;
    } else if (memcmp(expected, copied, sizeof(copied)) != 0 || !unit_matches(source, expected)) {
        result = NK_MEMSTICK_COLLISION_SOURCE_CHANGING;
    } else if (!unit_matches(scratch, expected)) {
        result = NK_MEMSTICK_COLLISION_COPY_MISMATCH;
    } else {
        int renamed = nk_rename_noreplace_utf8(scratch, target);
        if (renamed == 0) {
            bool synced = nk_platform_sync_dir(parent) && nk_platform_sync_dir(migration);
            return synced ? NK_MEMSTICK_COLLISION_COPIED : NK_MEMSTICK_COLLISION_IO;
        }
        result = renamed == 1 || !nk_platform_path_missing(target)
            ? NK_MEMSTICK_COLLISION_DESTINATION_CONFLICT : NK_MEMSTICK_COLLISION_IO;
    }
    (void)nk_platform_remove_tree(scratch, NULL);
    return result;
}

static bool unit_source_path(const char *saves, const char *slot, const char *name,
                             char out[NK_MAX_PATH]) {
    char slot_path[NK_MAX_PATH], parent[NK_MAX_PATH];
    return layout_join(saves, slot, slot_path) && unit_plain_directory(slot_path) &&
           unit_savedata_directory(slot_path, false, parent) &&
           unit_directory(parent, name, false, out);
}

static bool unit_source(const char *saves, const char *slot, const char *name,
                        char out[NK_MAX_PATH], uint8_t digest[32]) {
    return unit_source_path(saves, slot, name, out) && unit_tree_digest(out, NULL, digest);
}

static bool unit_open_move(const char *saves_root, NkMemstickPaths *paths) {
    char saves[NK_MAX_PATH];
    if (!nk_platform_real_path(saves_root, saves, sizeof(saves)) ||
        !nk_memstick_layout_paths(saves, paths) || !nk_platform_path_missing(paths->stick))
        return false;
    NkMemstickLayout layout;
    if (nk_memstick_layout_read(saves, &layout, NULL, 0u) != NK_MEMSTICK_LAYOUT_OK) return false;
    const bool moving = layout.state == NK_MEMSTICK_STATE_MOVING;
    nk_memstick_layout_free(&layout);
    return moving;
}

static NkMemstickCollisionResult unit_publish_to_layout(const NkMemstickPaths *paths,
                                                         const char *source,
                                                         const uint8_t expected[32],
                                                         const char *slot,
                                                         const char *name,
                                                         bool keep) {
    const char *store = keep ? paths->kept_copies : paths->staging;
    char kept_slot[NK_MAX_PATH], parent[NK_MAX_PATH];
    if (!unit_ensure_layout_directory(paths->saves, paths->migration) ||
        !unit_ensure_layout_directory(paths->saves, store)) return NK_MEMSTICK_COLLISION_IO;
    if (keep) {
        if (!unit_directory(store, slot, true, kept_slot)) return NK_MEMSTICK_COLLISION_IO;
        store = kept_slot;
    }
    if (!unit_savedata_directory(store, true, parent)) return NK_MEMSTICK_COLLISION_IO;
    return unit_publish(source, expected, parent, name, paths->migration);
}

NkMemstickCollisionResult nk_memstick_copy_save_collision(const char *saves_root,
                                                          const char *winner_slot,
                                                          const char *winner_name,
                                                          const char *loser_slot,
                                                          const char *loser_name) {
    if (!slot_valid(winner_slot) || !slot_valid(loser_slot) ||
        ascii_equal_ignoring_case(winner_slot, loser_slot) ||
        !component_valid(winner_name) || !component_valid(loser_name) ||
        !ascii_equal_ignoring_case(winner_name, loser_name)) return NK_MEMSTICK_COLLISION_INVALID;

    char winner[NK_MAX_PATH], loser[NK_MAX_PATH];
    uint8_t win_digest[32], lose_digest[32];
    NkMemstickPaths paths;
    if (!unit_open_move(saves_root, &paths)) return NK_MEMSTICK_COLLISION_LAYOUT_BLOCKED;

    if (!unit_source(paths.saves, winner_slot, winner_name, winner, win_digest) ||
        !unit_source(paths.saves, loser_slot, loser_name, loser, lose_digest))
        return NK_MEMSTICK_COLLISION_SOURCE_UNREADABLE;

    const bool identical = memcmp(win_digest, lose_digest, sizeof(win_digest)) == 0;
    if (!identical) {
        NkMemstickCollisionResult kept_result = unit_publish_to_layout(&paths, loser, lose_digest,
                                                                       loser_slot, loser_name, true);
        if (kept_result != NK_MEMSTICK_COLLISION_COPIED) return kept_result;
    }
    NkMemstickCollisionResult result = unit_publish_to_layout(&paths, winner, win_digest,
                                                              winner_slot, winner_name, false);
    if (result != NK_MEMSTICK_COLLISION_COPIED) return result;
    if (!unit_matches(winner, win_digest) || !unit_matches(loser, lose_digest))
        return NK_MEMSTICK_COLLISION_SOURCE_CHANGING;
    return identical ? NK_MEMSTICK_COLLISION_IDENTICAL : NK_MEMSTICK_COLLISION_COPIED;
}

/* ---- Whole-save planning and replay ---- */

typedef struct {
    NkMemstickPlanUnit unit;
    UnitWriteTime latest;
    bool prefix_owner;
} PlanCandidate;

typedef struct {
    PlanCandidate *items;
    size_t count;
    size_t capacity;
} PlanCandidates;

static bool plan_disc_prefix(const char *name, const char *disc_id) {
    if (!disc_id || !*disc_id) return false;
    for (; *disc_id; ++name, ++disc_id) {
        if (ascii_fold((unsigned char)*name) != ascii_fold((unsigned char)*disc_id))
            return false;
    }
    return true;
}

static int plan_input_order(const void *left, const void *right) {
    const NkMemstickPlanInput *a = left, *b = right;
    return ascii_compare_ignoring_case(a->slot, b->slot);
}

typedef struct {
    const NkMemstickPlanInput *inputs;
    NkMemstickPlanSource *sources;
    size_t count;
} PlanSlots;

static bool plan_find_slot(const char *name, bool is_dir, void *opaque) {
    PlanSlots *slots = opaque;
    size_t low = 0u, high = slots->count;
    while (low < high) {
        size_t i = low + (high - low) / 2u;
        int order = ascii_compare_ignoring_case(name, slots->inputs[i].slot);
        if (order < 0) high = i;
        else if (order > 0) low = i + 1u;
        else {
            NkMemstickPlanSource *source = &slots->sources[i];
            if (!is_dir || source->status != NK_MEMSTICK_PLAN_SOURCE_ABSENT) {
                memcpy(source->slot, slots->inputs[i].slot, strlen(slots->inputs[i].slot) + 1u);
                source->status = NK_MEMSTICK_PLAN_SOURCE_UNREADABLE;
            } else {
                memcpy(source->slot, name, strlen(name) + 1u);
                source->status = NK_MEMSTICK_PLAN_SOURCE_READABLE;
            }
            break;
        }
    }
    return true;
}

static int plan_candidate_order(const void *left, const void *right) {
    const PlanCandidate *a = left, *b = right;
    int by_key = ascii_compare_ignoring_case(a->unit.name, b->unit.name);
    if (by_key) return by_key;
    if (a->prefix_owner != b->prefix_owner) return a->prefix_owner ? -1 : 1;
    if (a->latest.present != b->latest.present) return a->latest.present ? -1 : 1;
    int by_age = unit_compare_times(b->latest.value, a->latest.value);
    if (by_age) return by_age;
    return ascii_compare_ignoring_case(a->unit.slot, b->unit.slot);
}

static bool plan_append(PlanCandidates *candidates, const PlanCandidate *candidate) {
    if (candidates->count == candidates->capacity) {
        size_t capacity = candidates->capacity ? candidates->capacity * 2u : 16u;
        if (capacity <= candidates->capacity || capacity > SIZE_MAX / sizeof(*candidates->items))
            return false;
        PlanCandidate *items = realloc(candidates->items, capacity * sizeof(*items));
        if (!items) return false;
        candidates->items = items;
        candidates->capacity = capacity;
    }
    candidates->items[candidates->count++] = *candidate;
    return true;
}

static bool plan_read_source(const char *saves, const NkMemstickPlanInput *input,
                             NkMemstickPlanSource *source, PlanCandidates *candidates) {
    char slot[NK_MAX_PATH], psp[NK_MAX_PATH], parent[NK_MAX_PATH];
    if (!layout_join(saves, source->slot, slot)) {
        source->status = NK_MEMSTICK_PLAN_SOURCE_UNREADABLE;
        return true;
    }
    if (source->status == NK_MEMSTICK_PLAN_SOURCE_UNREADABLE) return true;
    if (source->status == NK_MEMSTICK_PLAN_SOURCE_ABSENT) {
        /* Unsupported entries are omitted from the enumeration. If its exact
           name exists anyway, don't treat that legacy source as absent. */
        if (!nk_platform_path_missing(slot)) source->status = NK_MEMSTICK_PLAN_SOURCE_UNREADABLE;
        return true;
    }
    source->status = NK_MEMSTICK_PLAN_SOURCE_UNREADABLE;
    if (!unit_plain_directory(slot) || !unit_directory(slot, "PSP", false, psp)) return true;
    if (nk_platform_path_missing(psp)) {
        source->status = NK_MEMSTICK_PLAN_SOURCE_READABLE;
        return true;
    }
    if (!unit_directory(psp, "SAVEDATA", false, parent)) return true;
    if (nk_platform_path_missing(parent)) {
        source->status = NK_MEMSTICK_PLAN_SOURCE_READABLE;
        return true;
    }

    UnitEntries entries = {0};
    bool readable = unit_read_entries(parent, &entries);
    const size_t before = candidates->count;
    bool allocated = true;
    for (size_t i = 0; readable && allocated && i < entries.count; ++i) {
        const UnitEntry *entry = &entries.items[i];
        if (!entry->is_dir) continue;
        PlanCandidate candidate = {0};
        char from[NK_MAX_PATH];
        readable = layout_join(parent, entry->name, from) &&
                   nk_shared_text_copy(candidate.unit.name, sizeof(candidate.unit.name),
                                        entry->name, NK_SHARED_TEXT_COPY_REQUIRE_FIT) &&
                   unit_tree_fingerprint(from, NULL, candidate.unit.digest, &candidate.latest);
        if (readable) {
            memcpy(candidate.unit.slot, source->slot, strlen(source->slot) + 1u);
            candidate.prefix_owner = plan_disc_prefix(entry->name, input->disc_id);
            allocated = plan_append(candidates, &candidate);
        }
    }
    unit_free_entries(&entries);
    if (readable && allocated) source->status = NK_MEMSTICK_PLAN_SOURCE_READABLE;
    else candidates->count = before;
    return allocated;
}

void nk_memstick_save_plan_free(NkMemstickSavePlan *plan) {
    if (!plan) return;
    free(plan->units);
    free(plan->sources);
    memset(plan, 0, sizeof(*plan));
}

static bool plan_elect(PlanCandidates *candidates, NkMemstickSavePlan *out) {
    if (!candidates->count) return true;
    qsort(candidates->items, candidates->count, sizeof(*candidates->items), plan_candidate_order);
    out->units = malloc(candidates->count * sizeof(*out->units));
    if (!out->units) return false;
    out->unit_count = candidates->count;
    size_t group = 0u;
    for (size_t i = 0; i < out->unit_count; ++i) {
        NkMemstickPlanUnit *row = &out->units[i];
        *row = candidates->items[i].unit;
        if (!ascii_equal_ignoring_case(out->units[group].name, row->name)) group = i;
        row->decision = group == i ? NK_MEMSTICK_PLAN_STAGE : NK_MEMSTICK_PLAN_KEEP;
        row->copy_of = i;
        for (size_t prior = group; prior < i; ++prior) {
            if (memcmp(out->units[prior].digest, row->digest, sizeof(row->digest)) == 0) {
                row->copy_of = out->units[prior].copy_of;
                row->decision = NK_MEMSTICK_PLAN_DEDUPLICATE;
                break;
            }
        }
    }
    return true;
}

bool nk_memstick_plan_savedata(const char *saves_root, const NkMemstickPlanInput *inputs,
                               size_t input_count, NkMemstickSavePlan *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (input_count > NK_MEMSTICK_LAYOUT_MAX_SOURCES || (input_count && !inputs)) return false;
    char saves[NK_MAX_PATH];
    NkMemstickPaths paths;
    if (!nk_platform_real_path(saves_root, saves, sizeof(saves)) ||
        !nk_memstick_layout_paths(saves, &paths) || !nk_platform_dir_exists(saves)) return false;
    for (size_t i = 0; i < input_count; ++i) {
        if (!slot_valid(inputs[i].slot) || strlen(inputs[i].slot) >= NK_MAX_PATH ||
            (inputs[i].disc_id && inputs[i].disc_id[0] &&
             (!component_valid(inputs[i].disc_id) ||
              strlen(inputs[i].disc_id) >= NK_MAX_DISC_ID_LEN))) return false;
    }
    NkMemstickPlanInput *sorted = NULL;
    if (input_count) {
        sorted = malloc(input_count * sizeof(*sorted));
        out->sources = calloc(input_count, sizeof(*out->sources));
        if (!sorted || !out->sources) {
            free(sorted);
            nk_memstick_save_plan_free(out);
            return false;
        }
        memcpy(sorted, inputs, input_count * sizeof(*sorted));
        qsort(sorted, input_count, sizeof(*sorted), plan_input_order);
    }
    out->active_stick_present = !nk_platform_path_missing(paths.stick);
    out->source_count = input_count;
    PlanCandidates candidates = {0};
    bool ok = true;
    for (size_t i = 0; ok && i < input_count; ++i) {
        ok = !i || plan_input_order(&sorted[i - 1u], &sorted[i]) != 0;
        memcpy(out->sources[i].slot, sorted[i].slot, strlen(sorted[i].slot) + 1u);
        out->sources[i].status = NK_MEMSTICK_PLAN_SOURCE_ABSENT;
    }
    if (ok && input_count) {
        PlanSlots slots = {sorted, out->sources, input_count};
        size_t skipped = 0u;
        ok = nk_platform_list_dir_counted(saves, plan_find_slot, &slots, &skipped);
    }
    for (size_t i = 0; ok && i < input_count; ++i) {
        ok = plan_read_source(saves, &sorted[i], &out->sources[i], &candidates);
    }
    if (ok) ok = plan_elect(&candidates, out);
    free(candidates.items);
    free(sorted);
    if (!ok) nk_memstick_save_plan_free(out);
    return ok;
}

static bool plan_source_matches(const NkMemstickPaths *paths, const NkMemstickPlanUnit *row,
                                char path[NK_MAX_PATH]) {
    return unit_source_path(paths->saves, row->slot, row->name, path) &&
           unit_matches(path, row->digest);
}

NkMemstickCollisionResult nk_memstick_replay_savedata(const char *saves_root,
                                                      const NkMemstickSavePlan *plan,
                                                      size_t first_unit) {
    if (!plan || !plan->units || first_unit >= plan->unit_count ||
        plan->units[first_unit].decision != NK_MEMSTICK_PLAN_STAGE ||
        (plan->source_count && !plan->sources)) return NK_MEMSTICK_COLLISION_INVALID;
    NkMemstickPaths paths;
    if (!unit_open_move(saves_root, &paths)) return NK_MEMSTICK_COLLISION_LAYOUT_BLOCKED;
    for (size_t i = 0; i < plan->source_count; ++i) {
        if (plan->sources[i].status == NK_MEMSTICK_PLAN_SOURCE_UNREADABLE)
            return NK_MEMSTICK_COLLISION_SOURCE_UNREADABLE;
    }

    const NkMemstickPlanUnit *winner = &plan->units[first_unit];
    char source[NK_MAX_PATH];
    size_t end = first_unit;
    for (; end < plan->unit_count; ++end) {
        const NkMemstickPlanUnit *row = &plan->units[end];
        if (!slot_valid(row->slot) || !component_valid(row->name))
            return NK_MEMSTICK_COLLISION_INVALID;
        if (!ascii_equal_ignoring_case(row->name, winner->name)) break;
        if (row->decision == NK_MEMSTICK_PLAN_DEDUPLICATE) {
            if (row->copy_of < first_unit || row->copy_of >= end ||
                plan->units[row->copy_of].decision == NK_MEMSTICK_PLAN_DEDUPLICATE ||
                memcmp(row->digest, plan->units[row->copy_of].digest, sizeof(row->digest)) != 0)
                return NK_MEMSTICK_COLLISION_INVALID;
        } else if (row->copy_of != end || (end != first_unit && row->decision != NK_MEMSTICK_PLAN_KEEP)) {
            return NK_MEMSTICK_COLLISION_INVALID;
        }
        if (!plan_source_matches(&paths, row, source)) return NK_MEMSTICK_COLLISION_SOURCE_CHANGING;
    }

    /* Publish recoverable alternates before the staged winner. Repetition uses the
       same destinations, fingerprints and no-clobber checks as the two-source copy. */
    for (size_t i = first_unit + 1u; i < end; ++i) {
        const NkMemstickPlanUnit *row = &plan->units[i];
        if (row->decision == NK_MEMSTICK_PLAN_DEDUPLICATE) continue;
        if (!unit_source_path(paths.saves, row->slot, row->name, source))
            return NK_MEMSTICK_COLLISION_SOURCE_CHANGING;
        NkMemstickCollisionResult result = unit_publish_to_layout(&paths, source, row->digest,
                                                                  row->slot, row->name, true);
        if (result != NK_MEMSTICK_COLLISION_COPIED) return result;
    }
    if (!unit_source_path(paths.saves, winner->slot, winner->name, source))
        return NK_MEMSTICK_COLLISION_SOURCE_CHANGING;
    NkMemstickCollisionResult result = unit_publish_to_layout(&paths, source, winner->digest,
                                                              winner->slot, winner->name, false);
    if (result != NK_MEMSTICK_COLLISION_COPIED) return result;
    for (size_t i = first_unit; i < end; ++i) {
        if (!plan_source_matches(&paths, &plan->units[i], source))
            return NK_MEMSTICK_COLLISION_SOURCE_CHANGING;
    }
    return result;
}

/* ---- Durable first-move plan and checkpoint ---- */

#define SAVEDATA_MOVE_JOURNAL "psp-savedata"
#define SAVEDATA_MOVE_PLAN "plan.json"
#define SAVEDATA_MOVE_PROGRESS "progress.json"

/* Find the first row after this complete group. The caller has validated the
   names and group ordering. */
static size_t move_group_end(const NkMemstickSavePlan *plan, size_t first) {
    size_t end = first + 1u;
    while (end < plan->unit_count &&
           ascii_equal_ignoring_case(plan->units[first].name, plan->units[end].name)) ++end;
    return end;
}

static bool move_known_source(const NkMemstickSavePlan *plan, const char *slot) {
    size_t low = 0u, high = plan->source_count;
    while (low < high) {
        size_t i = low + (high - low) / 2u;
        const NkMemstickPlanSource *source = &plan->sources[i];
        int order = ascii_compare_ignoring_case(slot, source->slot);
        if (order < 0) high = i;
        else if (order > 0) low = i + 1u;
        else return !strcmp(slot, source->slot) && source->status == NK_MEMSTICK_PLAN_SOURCE_READABLE;
    }
    return false;
}

static bool move_plan_valid(const NkMemstickSavePlan *plan) {
    if (!plan || plan->source_count > NK_MEMSTICK_LAYOUT_MAX_SOURCES ||
        plan->unit_count > NK_MEMSTICK_LAYOUT_MAX_BYTES / 70u ||
        (plan->source_count && !plan->sources) || (plan->unit_count && !plan->units)) return false;
    for (size_t i = 0; i < plan->source_count; ++i) {
        const NkMemstickPlanSource *source = &plan->sources[i];
        if (!memchr(source->slot, '\0', sizeof(source->slot)) || !slot_valid(source->slot) ||
            (source->status != NK_MEMSTICK_PLAN_SOURCE_READABLE &&
             source->status != NK_MEMSTICK_PLAN_SOURCE_ABSENT) ||
            (i && ascii_compare_ignoring_case(plan->sources[i - 1u].slot, source->slot) >= 0))
            return false;
    }
    size_t first = 0u;
    for (size_t i = 0; i < plan->unit_count; ++i) {
        const NkMemstickPlanUnit *row = &plan->units[i];
        if (!memchr(row->slot, '\0', sizeof(row->slot)) || !move_known_source(plan, row->slot) ||
            !memchr(row->name, '\0', sizeof(row->name)) || !component_valid(row->name))
            return false;
        int order = i ? ascii_compare_ignoring_case(plan->units[i - 1u].name, row->name) : -1;
        if (order > 0) return false;
        if (order < 0) {
            if (row->decision != NK_MEMSTICK_PLAN_STAGE || row->copy_of != i) return false;
            first = i;
        } else if (row->decision == NK_MEMSTICK_PLAN_KEEP) {
            if (row->copy_of != i) return false;
        } else if (row->decision == NK_MEMSTICK_PLAN_DEDUPLICATE) {
            if (row->copy_of < first || row->copy_of >= i ||
                plan->units[row->copy_of].decision == NK_MEMSTICK_PLAN_DEDUPLICATE ||
                memcmp(plan->units[row->copy_of].digest, row->digest, sizeof(row->digest)))
                return false;
        } else {
            return false;
        }
    }
    return true;
}

static void move_digest_hex(const uint8_t bytes[32], char hex[65]) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < 32u; ++i) {
        hex[i * 2u] = digits[bytes[i] >> 4u];
        hex[i * 2u + 1u] = digits[bytes[i] & 15u];
    }
    hex[64] = '\0';
}

static bool move_parse_digest(const char *hex, uint8_t bytes[32]) {
    static const char digits[] = "0123456789abcdef";
    if (!hex || strlen(hex) != 64u) return false;
    for (size_t i = 0; i < 32u; ++i) {
        const char *high = strchr(digits, hex[i * 2u]);
        const char *low = strchr(digits, hex[i * 2u + 1u]);
        if (!high || !low) return false;
        bytes[i] = (uint8_t)(((high - digits) << 4) | (low - digits));
    }
    return true;
}

static void move_text_size(LayoutText *text, size_t size) {
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%zu", size);
    text_literal(text, buffer);
}

/* The format is canonical. Re-encoding catches unknown members, duplicate
   keys, noncanonical numbers and any embedded NUL in a JSON string value. */
static LayoutText move_format_plan(const NkMemstickSavePlan *plan) {
    LayoutText text = {0};
    text_literal(&text, "{\"schema_version\":1,\"sources\":[");
    for (size_t i = 0; i < plan->source_count; ++i) {
        const NkMemstickPlanSource *source = &plan->sources[i];
        text_literal(&text, i ? ",[" : "[");
        text_string(&text, source->slot);
        text_literal(&text, ",");
        move_text_size(&text, (size_t)source->status);
        text_literal(&text, "]");
    }
    text_literal(&text, "],\"units\":[");
    for (size_t i = 0; i < plan->unit_count; ++i) {
        const NkMemstickPlanUnit *unit = &plan->units[i];
        char hex[65];
        move_digest_hex(unit->digest, hex);
        text_literal(&text, i ? ",[" : "[");
        text_string(&text, unit->slot);
        text_literal(&text, ",");
        text_string(&text, unit->name);
        text_literal(&text, ",");
        text_string(&text, hex);
        text_literal(&text, ",");
        move_text_size(&text, (size_t)unit->decision);
        text_literal(&text, ",");
        move_text_size(&text, unit->copy_of);
        text_literal(&text, "]");
    }
    text_literal(&text, "]}\n");
    return text;
}

static bool move_same_text(const LayoutText *expected, const unsigned char *bytes, size_t length) {
    return !expected->failed && expected->length == length &&
           memcmp(expected->bytes, bytes, length) == 0;
}

static bool move_read_index(const NkJsonNode *node, size_t limit, size_t *index) {
    int64_t parsed = 0;
    if (!nk_json_get_int64(node, &parsed) || parsed < 0 || (uint64_t)parsed > limit) return false;
    *index = (size_t)parsed;
    return true;
}

static bool move_read_string(const NkJsonNode *node, char out[NK_MAX_PATH]) {
    const char *value = nk_json_get_string(node);
    return value && nk_shared_text_copy(out, NK_MAX_PATH, value, NK_SHARED_TEXT_COPY_REQUIRE_FIT);
}

static bool move_parse_plan_arrays(const NkJsonNode *root, NkMemstickSavePlan *plan) {
    const NkJsonNode *sources = nk_json_object_get(root, "sources");
    const NkJsonNode *units = nk_json_object_get(root, "units");
    if (!nk_json_is_array(sources) || !nk_json_is_array(units)) return false;
    plan->source_count = nk_json_array_count(sources);
    plan->unit_count = nk_json_array_count(units);
    if (plan->source_count > NK_MEMSTICK_LAYOUT_MAX_SOURCES ||
        plan->unit_count > NK_MEMSTICK_LAYOUT_MAX_BYTES / 70u) return false;
    if (plan->source_count) {
        plan->sources = calloc(plan->source_count, sizeof(*plan->sources));
        if (!plan->sources) return false;
    }
    if (plan->unit_count) {
        plan->units = calloc(plan->unit_count, sizeof(*plan->units));
        if (!plan->units) return false;
    }
    for (size_t i = 0; i < plan->source_count; ++i) {
        const NkJsonNode *row = nk_json_array_get(sources, i);
        NkMemstickPlanSource *source = &plan->sources[i];
        size_t status;
        if (nk_json_array_count(row) != 2u ||
            !move_read_string(nk_json_array_get(row, 0u), source->slot) ||
            !move_read_index(nk_json_array_get(row, 1u), NK_MEMSTICK_PLAN_SOURCE_ABSENT, &status))
            return false;
        source->status = (NkMemstickPlanSourceStatus)status;
    }
    for (size_t i = 0; i < plan->unit_count; ++i) {
        const NkJsonNode *row = nk_json_array_get(units, i);
        NkMemstickPlanUnit *unit = &plan->units[i];
        size_t decision;
        if (nk_json_array_count(row) != 5u ||
            !move_read_string(nk_json_array_get(row, 0u), unit->slot) ||
            !move_read_string(nk_json_array_get(row, 1u), unit->name) ||
            !move_parse_digest(nk_json_get_string(nk_json_array_get(row, 2u)), unit->digest) ||
            !move_read_index(nk_json_array_get(row, 3u), NK_MEMSTICK_PLAN_DEDUPLICATE, &decision) ||
            !move_read_index(nk_json_array_get(row, 4u), i, &unit->copy_of)) return false;
        unit->decision = (NkMemstickPlanDecision)decision;
    }
    return true;
}

static bool move_parse_plan(const unsigned char *bytes, size_t length, NkMemstickSavePlan *plan) {
    NkJsonNode *root = nk_json_parse((const char *)bytes, length, NULL, 0u);
    size_t version;
    bool ok = root && move_read_index(nk_json_object_get(root, "schema_version"), 1u, &version) &&
              version == 1u && move_parse_plan_arrays(root, plan) && move_plan_valid(plan);
    nk_json_free(root);
    if (ok) {
        LayoutText canonical = move_format_plan(plan);
        ok = move_same_text(&canonical, bytes, length);
        free(canonical.bytes);
    }
    return ok;
}

static NkMemstickCollisionResult move_read(const char *path, size_t bound,
                                            unsigned char **bytes, size_t *length) {
    *bytes = NULL;
    *length = 0u;
    if (nk_platform_path_missing(path)) return NK_MEMSTICK_COLLISION_JOURNAL_MISSING;
    FILE *file = nk_platform_open_regular_file(path);
    if (!file) return NK_MEMSTICK_COLLISION_IO;
    NkReadFileStatus got = nk_read_stream_bounded(file, bound, bytes, length);
    if (fclose(file) != 0) return NK_MEMSTICK_COLLISION_IO;
    if (got == NK_READ_FILE_OK) return NK_MEMSTICK_COLLISION_COPIED;
    return got == NK_READ_FILE_TOO_LARGE ? NK_MEMSTICK_COLLISION_JOURNAL_DAMAGED
                                         : NK_MEMSTICK_COLLISION_IO;
}

static NkMemstickCollisionResult move_write(const char *directory, const char *target,
                                             const LayoutText *text, bool replace) {
    char scratch[NK_MAX_PATH], temporary[NK_MAX_PATH];
    if (text->failed || !text->length || !move_scratch_directory(directory, scratch))
        return NK_MEMSTICK_COLLISION_IO;
    bool named = layout_join(scratch, "record", temporary);
    FILE *file = named ? nk_platform_fopen_private(temporary, "wb") : NULL;
    bool ok = false;
    if (file) {
        ok = fwrite(text->bytes, 1u, text->length, file) == text->length;
        ok = nk_platform_sync_file(file) && ok;
        ok = fclose(file) == 0 && ok;
    }
    if (ok) {
        int renamed = replace ? nk_rename_utf8(temporary, target)
                              : nk_rename_noreplace_utf8(temporary, target);
        ok = renamed == 0 && nk_platform_sync_dir(directory);
    }
    if (named) (void)nk_remove_utf8(temporary);
    (void)nk_platform_remove_empty_dir(scratch);
    return ok ? NK_MEMSTICK_COLLISION_COPIED : NK_MEMSTICK_COLLISION_IO;
}

static int move_inventory_order(const void *left, const void *right) {
    const NkMemstickPlanUnit *a = *(const NkMemstickPlanUnit *const *)left;
    const NkMemstickPlanUnit *b = *(const NkMemstickPlanUnit *const *)right;
    int slot = strcmp(a->slot, b->slot);
    return slot ? slot : strcmp(a->name, b->name);
}

static NkMemstickCollisionResult move_match_units(const NkMemstickSavePlan *plan,
                                                   const NkMemstickSavePlan *current) {
    if (plan->unit_count != current->unit_count) return NK_MEMSTICK_COLLISION_SOURCE_CHANGING;
    if (!plan->unit_count) return NK_MEMSTICK_COLLISION_COPIED;
    size_t count = plan->unit_count;
    const NkMemstickPlanUnit **snapshot = malloc(2u * count * sizeof(*snapshot));
    if (!snapshot) return NK_MEMSTICK_COLLISION_IO;
    const NkMemstickPlanUnit **now = snapshot + count;
    for (size_t i = 0; i < count; ++i) {
        snapshot[i] = &plan->units[i];
        now[i] = &current->units[i];
    }
    qsort(snapshot, count, sizeof(*snapshot), move_inventory_order);
    qsort(now, count, sizeof(*now), move_inventory_order);
    bool match = true;
    for (size_t i = 0; match && i < count; ++i) {
        match = move_inventory_order(&snapshot[i], &now[i]) == 0 &&
                !memcmp(snapshot[i]->digest, now[i]->digest, sizeof(snapshot[i]->digest));
    }
    free(snapshot);
    return match ? NK_MEMSTICK_COLLISION_COPIED : NK_MEMSTICK_COLLISION_SOURCE_CHANGING;
}

/* Rescan this frozen set of legacy slots. Their exact inventory and bytes must
   still match, including slots that were absent. Recency or enumeration changes
   do not replace the originally persisted election. */
static NkMemstickCollisionResult move_match_inventory(const NkMemstickPaths *paths,
                                                       const NkMemstickSavePlan *plan) {
    NkMemstickPlanInput *inputs = NULL;
    if (plan->source_count) {
        inputs = calloc(plan->source_count, sizeof(*inputs));
        if (!inputs) return NK_MEMSTICK_COLLISION_IO;
        for (size_t i = 0; i < plan->source_count; ++i) inputs[i].slot = plan->sources[i].slot;
    }
    NkMemstickSavePlan current;
    bool scanned = nk_memstick_plan_savedata(paths->saves, inputs, plan->source_count, &current);
    free(inputs);
    if (!scanned) return NK_MEMSTICK_COLLISION_IO;
    NkMemstickCollisionResult result = NK_MEMSTICK_COLLISION_COPIED;
    if (current.active_stick_present) {
        result = NK_MEMSTICK_COLLISION_LAYOUT_BLOCKED;
    } else {
        for (size_t i = 0; i < plan->source_count; ++i) {
            if (plan->sources[i].status != current.sources[i].status ||
                strcmp(plan->sources[i].slot, current.sources[i].slot)) {
                result = NK_MEMSTICK_COLLISION_SOURCE_CHANGING;
                break;
            }
        }
    }
    if (result == NK_MEMSTICK_COLLISION_COPIED) result = move_match_units(plan, &current);
    nk_memstick_save_plan_free(&current);
    return result;
}

static LayoutText move_format_progress(const char *plan_hash, size_t next) {
    LayoutText text = {0};
    text_literal(&text, "{\"schema_version\":1,\"plan_sha256\":");
    text_string(&text, plan_hash);
    text_literal(&text, ",\"next_unit\":");
    move_text_size(&text, next);
    text_literal(&text, "}\n");
    return text;
}

static NkMemstickCollisionResult move_read_progress(const char *path, const char *plan_hash,
                                                     const NkMemstickSavePlan *plan, size_t *next) {
    unsigned char *bytes;
    size_t length;
    *next = 0u;
    NkMemstickCollisionResult read = move_read(path, 256u, &bytes, &length);
    if (read == NK_MEMSTICK_COLLISION_COPIED) {
        NkJsonNode *json = nk_json_parse((const char *)bytes, length, NULL, 0u);
        bool valid = json && move_read_index(nk_json_object_get(json, "next_unit"),
                                             plan->unit_count, next) &&
                     (*next == plan->unit_count ||
                      plan->units[*next].decision == NK_MEMSTICK_PLAN_STAGE);
        nk_json_free(json);
        LayoutText canonical = move_format_progress(plan_hash, *next);
        valid = valid && move_same_text(&canonical, bytes, length);
        free(canonical.bytes);
        if (!valid) read = NK_MEMSTICK_COLLISION_JOURNAL_DAMAGED;
    }
    free(bytes);
    return read == NK_MEMSTICK_COLLISION_JOURNAL_MISSING ? NK_MEMSTICK_COLLISION_COPIED : read;
}

static bool move_matches_output(const NkMemstickPaths *paths, const NkMemstickPlanUnit *unit) {
    const char *store = unit->decision == NK_MEMSTICK_PLAN_KEEP ? paths->kept_copies : paths->staging;
    char kept_slot[NK_MAX_PATH], parent[NK_MAX_PATH], output[NK_MAX_PATH];
    if (!unit_plain_directory(store)) return false;
    if (unit->decision == NK_MEMSTICK_PLAN_KEEP) {
        if (!unit_directory(store, unit->slot, false, kept_slot)) return false;
        store = kept_slot;
    }
    return unit_savedata_directory(store, false, parent) &&
           unit_directory(parent, unit->name, false, output) && unit_matches(output, unit->digest);
}

NkMemstickCollisionResult nk_memstick_savedata_move_begin(const char *saves_root,
                                                          const NkMemstickSavePlan *plan) {
    if (!move_plan_valid(plan)) return NK_MEMSTICK_COLLISION_INVALID;
    NkMemstickPaths paths;
    if (!unit_open_move(saves_root, &paths)) return NK_MEMSTICK_COLLISION_LAYOUT_BLOCKED;
    char directory[NK_MAX_PATH], immutable[NK_MAX_PATH], progress[NK_MAX_PATH];
    if (!layout_join(paths.migration, SAVEDATA_MOVE_JOURNAL, directory) ||
        !layout_join(directory, SAVEDATA_MOVE_PLAN, immutable) ||
        !layout_join(directory, SAVEDATA_MOVE_PROGRESS, progress))
        return NK_MEMSTICK_COLLISION_IO;
    if ((!nk_platform_path_missing(paths.migration) && !unit_plain_directory(paths.migration)) ||
        (!nk_platform_path_missing(directory) && !unit_plain_directory(directory)))
        return NK_MEMSTICK_COLLISION_JOURNAL_DAMAGED;
    LayoutText text = move_format_plan(plan);
    NkMemstickCollisionResult result = NK_MEMSTICK_COLLISION_IO;
    if (!text.failed && text.length <= NK_MEMSTICK_LAYOUT_MAX_BYTES) {
        unsigned char *present;
        size_t length;
        result = move_read(immutable, NK_MEMSTICK_LAYOUT_MAX_BYTES, &present, &length);
        if (result == NK_MEMSTICK_COLLISION_COPIED) {
            if (!move_same_text(&text, present, length)) {
                result = NK_MEMSTICK_COLLISION_DESTINATION_CONFLICT;
            } else if (!nk_platform_sync_dir(paths.saves) ||
                       !nk_platform_sync_dir(paths.migration) ||
                       !nk_platform_sync_dir(directory)) {
                result = NK_MEMSTICK_COLLISION_IO;
            }
        } else if (result == NK_MEMSTICK_COLLISION_JOURNAL_MISSING) {
            if (!nk_platform_path_missing(progress) || !nk_platform_path_missing(paths.staging)) {
                result = NK_MEMSTICK_COLLISION_JOURNAL_DAMAGED;
            } else {
                result = move_match_inventory(&paths, plan);
                if (result == NK_MEMSTICK_COLLISION_COPIED) {
                    result = unit_ensure_layout_directory(paths.saves, paths.migration) &&
                             unit_ensure_layout_directory(paths.migration, directory)
                        ? move_write(directory, immutable, &text, false) : NK_MEMSTICK_COLLISION_IO;
                }
            }
        }
        free(present);
    }
    free(text.bytes);
    return result;
}

static NkMemstickCollisionResult move_resume_plan(const NkMemstickPaths *paths,
                                                    const NkMemstickSavePlan *plan,
                                                    const char *directory,
                                                    const char *hash,
                                                    size_t max_groups, bool *complete) {
    char checkpoint[NK_MAX_PATH];
    if (!layout_join(directory, SAVEDATA_MOVE_PROGRESS, checkpoint))
        return NK_MEMSTICK_COLLISION_IO;
    size_t next;
    NkMemstickCollisionResult result = move_read_progress(checkpoint, hash, plan, &next);
    if (result != NK_MEMSTICK_COLLISION_COPIED) return result;
    result = move_match_inventory(paths, plan);
    if (result != NK_MEMSTICK_COLLISION_COPIED) return result;
    for (size_t i = 0; i < next; ++i) {
        const NkMemstickPlanUnit *unit = &plan->units[i];
        if (unit->decision != NK_MEMSTICK_PLAN_DEDUPLICATE && !move_matches_output(paths, unit))
            return NK_MEMSTICK_COLLISION_DESTINATION_CONFLICT;
    }
    /* A previous process may have exited just after renaming the plan. Make
       that directory entry durable before there can be any staged output. */
    if (!nk_platform_sync_dir(paths->saves) || !nk_platform_sync_dir(paths->migration) ||
        !nk_platform_sync_dir(directory))
        return NK_MEMSTICK_COLLISION_IO;
    size_t groups = 0u;
    while (next < plan->unit_count && (!max_groups || groups < max_groups)) {
        result = nk_memstick_replay_savedata(paths->saves, plan, next);
        if (result != NK_MEMSTICK_COLLISION_COPIED) return result;
        next = move_group_end(plan, next);
        ++groups;
        LayoutText text = move_format_progress(hash, next);
        result = move_write(directory, checkpoint, &text, true);
        free(text.bytes);
        if (result != NK_MEMSTICK_COLLISION_COPIED) return result;
    }
    if (next == plan->unit_count) {
        result = move_match_inventory(paths, plan);
        if (result == NK_MEMSTICK_COLLISION_COPIED) *complete = true;
    }
    return result;
}

NkMemstickCollisionResult nk_memstick_savedata_move_resume(const char *saves_root,
                                                           size_t max_groups, bool *complete) {
    if (!complete) return NK_MEMSTICK_COLLISION_INVALID;
    *complete = false;
    NkMemstickPaths paths;
    if (!unit_open_move(saves_root, &paths)) return NK_MEMSTICK_COLLISION_LAYOUT_BLOCKED;
    char directory[NK_MAX_PATH], immutable[NK_MAX_PATH];
    if (!layout_join(paths.migration, SAVEDATA_MOVE_JOURNAL, directory) ||
        !layout_join(directory, SAVEDATA_MOVE_PLAN, immutable))
        return NK_MEMSTICK_COLLISION_IO;
    if (nk_platform_path_missing(directory)) return NK_MEMSTICK_COLLISION_JOURNAL_MISSING;
    if (!unit_plain_directory(paths.migration) || !unit_plain_directory(directory))
        return NK_MEMSTICK_COLLISION_JOURNAL_DAMAGED;
    unsigned char *bytes;
    size_t length;
    NkMemstickCollisionResult result = move_read(immutable, NK_MEMSTICK_LAYOUT_MAX_BYTES,
                                                &bytes, &length);
    NkMemstickSavePlan plan = {0};
    if (result == NK_MEMSTICK_COLLISION_COPIED) {
        if (move_parse_plan(bytes, length, &plan)) {
            NkSha256 sha;
            uint8_t digest[32];
            char hex[65];
            nk_sha256_init(&sha);
            nk_sha256_update(&sha, bytes, length);
            nk_sha256_finish(&sha, digest);
            move_digest_hex(digest, hex);
            result = move_resume_plan(&paths, &plan, directory, hex, max_groups, complete);
        } else {
            result = NK_MEMSTICK_COLLISION_JOURNAL_DAMAGED;
        }
    }
    nk_memstick_save_plan_free(&plan);
    free(bytes);
    return result;
}

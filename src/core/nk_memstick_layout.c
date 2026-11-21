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

static bool ascii_equal_ignoring_case(const char *a, const char *b) {
    for (;; ++a, ++b) {
        unsigned char ca = (unsigned char)*a;
        unsigned char cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb) return false;
        if (ca == '\0') return true;
    }
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

/* One folder name directly in <saves>: a single path component that is not the layout's own,
   and one every platform can make, so a record stays portable: no character Windows forbids in
   a name (< > : " / \ | ? * or a control byte), no trailing dot or space ("." and ".." among
   them), and no Windows device name. */
static bool slot_valid(const char *slot) {
    if (!slot) return false;
    size_t length = strlen(slot);
    if (length == 0u || length > NK_MEMSTICK_SLOT_MAX_BYTES) return false;
    if (strpbrk(slot, "<>:\"/\\|?*")) return false;
    if (slot[length - 1u] == '.' || slot[length - 1u] == ' ') return false;
    if (!text_printable(slot, length)) return false;
    if (windows_device_name(slot)) return false;
    return !nk_memstick_layout_is_reserved_name(slot);
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

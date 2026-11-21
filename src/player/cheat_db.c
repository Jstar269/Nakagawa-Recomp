/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The cheat database download and import (cheat_db.h, #2639). */

#include "cheat_db.h"
#include "nk_cheats.h"
#include "nk_json.h"
#include "nk_platform.h"
#include "nk_shared_helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHEAT_DB_READ_CHUNK 65536u

const char *nk_cheat_db_status_name(NkCheatDbStatus status) {
    switch (status) {
        case NK_CHEAT_DB_OK: return "ok";
        case NK_CHEAT_DB_BAD_ARGUMENT: return "CHEAT_DB_BAD_ARGUMENT";
        case NK_CHEAT_DB_NOT_REACHED: return "CHEAT_DB_NOT_REACHED";
        case NK_CHEAT_DB_HTTP_STATUS: return "CHEAT_DB_HTTP_STATUS";
        case NK_CHEAT_DB_REDIRECT_REFUSED: return "CHEAT_DB_REDIRECT_REFUSED";
        case NK_CHEAT_DB_TOO_LARGE: return "CHEAT_DB_TOO_LARGE";
        case NK_CHEAT_DB_TRUNCATED: return "CHEAT_DB_TRUNCATED";
        case NK_CHEAT_DB_SIZE_MISMATCH: return "CHEAT_DB_SIZE_MISMATCH";
        case NK_CHEAT_DB_HASH_MISMATCH: return "CHEAT_DB_HASH_MISMATCH";
        case NK_CHEAT_DB_LIST_INVALID: return "CHEAT_DB_LIST_INVALID";
        case NK_CHEAT_DB_LIST_VERSION: return "CHEAT_DB_LIST_VERSION";
        case NK_CHEAT_DB_NOT_A_DATABASE: return "CHEAT_DB_NOT_A_DATABASE";
        case NK_CHEAT_DB_EXISTS: return "CHEAT_DB_EXISTS";
        case NK_CHEAT_DB_IO_ERROR: return "CHEAT_DB_IO_ERROR";
        case NK_CHEAT_DB_CANCELLED: return "CHEAT_DB_CANCELLED";
        case NK_CHEAT_DB_OUT_OF_MEMORY: return "CHEAT_DB_OUT_OF_MEMORY";
    }
    return "unknown";
}

static NkCheatDbStatus fail(NkCheatDbStatus status, char *error, size_t error_size,
                            const char *what) {
    if (error && error_size > 0u) {
        snprintf(error, error_size, "%s: %s", nk_cheat_db_status_name(status), what);
    }
    return status;
}

bool nk_cheat_db_path(const char *data_root, char *out, size_t out_size) {
    int n;
    char sep = nk_platform_path_separator();
    if (!out || out_size == 0u) return false;
    out[0] = '\0';
    if (!data_root || !data_root[0]) return false;
    n = snprintf(out, out_size, "%s%ccheats%c%s", data_root, sep, sep, NK_CHEAT_DB_FILE_NAME);
    if (n < 0 || (size_t)n >= out_size) {
        out[0] = '\0';
        return false;
    }
    return true;
}

/* ---- The list of sources ---- */

/* Shown text: printable ASCII or UTF-8 continuation bytes, no control characters. */
static bool copy_text(const NkJsonNode *obj, const char *key, char *out, size_t out_size) {
    const char *value = nk_json_get_string(nk_json_obj_get(obj, key));
    size_t len;
    if (!value) return false;
    len = strlen(value);
    if (len == 0u || len >= out_size) return false;
    for (size_t i = 0u; i < len; i++) {
        unsigned char c = (unsigned char)value[i];
        if (c < 0x20u || c == 0x7Fu) return false;
    }
    memcpy(out, value, len + 1u);
    return true;
}

static bool is_lower_hex(const char *s, size_t len) {
    if (strlen(s) != len) return false;
    for (size_t i = 0u; i < len; i++) {
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
    }
    return true;
}

static int hex_value(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c - 'a' + 10;
}

/* One path segment of a pinned URL: letters, digits and "-_.", never "." or "..". */
static bool url_segment_ok(const char *begin, size_t len) {
    if (len == 0u || (len == 1u && begin[0] == '.') || (len == 2u && begin[0] == '.' && begin[1] == '.')) {
        return false;
    }
    for (size_t i = 0u; i < len; i++) {
        char c = begin[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_' || c == '.')) {
            return false;
        }
    }
    return true;
}

/* NK_CHEAT_DB_URL_PREFIX <owner>/<repo>/<commit>/<path...>, the commit the source names. */
static bool pinned_url_ok(const char *url, const char *commit) {
    const size_t prefix = strlen(NK_CHEAT_DB_URL_PREFIX);
    const char *p;
    size_t segment = 0u;
    if (strncmp(url, NK_CHEAT_DB_URL_PREFIX, prefix) != 0) return false;
    p = url + prefix;
    for (;;) {
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        if (!url_segment_ok(p, len)) return false;
        if (segment == 2u && (len != 40u || strncmp(p, commit, 40u) != 0)) return false;
        segment++;
        if (!slash) break;
        p = slash + 1;
    }
    return segment >= 4u;
}

static bool parse_source(const NkJsonNode *obj, NkCheatDbSource *out) {
    const char *sha;
    double size;
    memset(out, 0, sizeof(*out));
    if (!nk_json_is_object(obj)) return false;
    if (!copy_text(obj, "id", out->id, sizeof(out->id)) ||
        !copy_text(obj, "name", out->name, sizeof(out->name)) ||
        !copy_text(obj, "maintainer", out->maintainer, sizeof(out->maintainer)) ||
        !copy_text(obj, "licence", out->licence, sizeof(out->licence)) ||
        !copy_text(obj, "homepage", out->homepage, sizeof(out->homepage)) ||
        !copy_text(obj, "commit", out->commit, sizeof(out->commit)) ||
        !copy_text(obj, "url", out->url, sizeof(out->url))) {
        return false;
    }
    for (const char *c = out->id; *c; c++) {
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-')) return false;
    }
    if (strncmp(out->homepage, "https://", 8u) != 0) return false;
    if (!is_lower_hex(out->commit, 40u) || !pinned_url_ok(out->url, out->commit)) return false;
    if (!nk_json_get_number(nk_json_obj_get(obj, "size"), &size) || !(size >= 1.0) ||
        size > (double)NK_CHEATS_MAX_FILE_BYTES || size != (double)(uint64_t)size) {
        return false;
    }
    out->size = (uint64_t)size;
    sha = nk_json_get_string(nk_json_obj_get(obj, "sha256"));
    if (!sha || !is_lower_hex(sha, 64u)) return false;
    for (size_t i = 0u; i < 32u; i++) {
        out->sha256[i] = (uint8_t)((hex_value(sha[2u * i]) << 4) | hex_value(sha[2u * i + 1u]));
    }
    return true;
}

NkCheatDbStatus nk_cheat_db_parse_sources(const char *text, size_t size, NkCheatDbSources *out,
                                          char *error, size_t error_size) {
    char json_error[128];
    NkJsonNode *root;
    const NkJsonNode *list;
    double schema;
    const char *format;
    NkCheatDbStatus status = NK_CHEAT_DB_OK;
    if (!out || (!text && size != 0u)) {
        return fail(NK_CHEAT_DB_BAD_ARGUMENT, error, error_size, "no list");
    }
    memset(out, 0, sizeof(*out));
    if (size > NK_CHEAT_DB_SOURCES_MAX_BYTES) {
        return fail(NK_CHEAT_DB_TOO_LARGE, error, error_size, "the list of sources is too large");
    }
    json_error[0] = '\0';
    root = nk_json_parse(text, size, json_error, sizeof(json_error));
    if (!root) {
        return fail(NK_CHEAT_DB_LIST_INVALID, error, error_size, "the list of sources is not valid JSON");
    }
    format = nk_json_get_string(nk_json_obj_get(root, "format"));
    if (!nk_json_is_object(root) || !format || strcmp(format, NK_CHEAT_DB_SOURCES_FORMAT) != 0 ||
        !nk_json_get_number(nk_json_obj_get(root, "schema_version"), &schema)) {
        status = fail(NK_CHEAT_DB_LIST_INVALID, error, error_size,
                      "the list of sources has no format or schema_version");
        goto done;
    }
    if (schema != (double)NK_CHEAT_DB_SOURCES_SCHEMA) {
        status = fail(NK_CHEAT_DB_LIST_VERSION, error, error_size,
                      "the list of sources is a version this player does not read; a newer "
                      "Nakagawa reads it");
        goto done;
    }
    list = nk_json_obj_get(root, "sources");
    if (!nk_json_is_array(list) || nk_json_array_count(list) == 0u ||
        nk_json_array_count(list) > NK_CHEAT_DB_MAX_SOURCES) {
        status = fail(NK_CHEAT_DB_LIST_INVALID, error, error_size,
                      "the list of sources names no source, or too many");
        goto done;
    }
    for (size_t i = 0u; i < nk_json_array_count(list); i++) {
        if (!parse_source(nk_json_array_get(list, i), &out->sources[i])) {
            char what[96];
            snprintf(what, sizeof(what), "source %u of the list is not a pinned source",
                     (unsigned)(i + 1u));
            status = fail(NK_CHEAT_DB_LIST_INVALID, error, error_size, what);
            memset(out, 0, sizeof(*out));
            goto done;
        }
        out->count = i + 1u;
    }
done:
    nk_json_free(root);
    return status;
}

/* ---- Fetching ---- */

/* The whole body of url, at most max_bytes (exact_size: exactly that many), in *out. */
static NkCheatDbStatus fetch_body(PackageHttpOpen transport, void *transport_context,
                                  const char *url, uint64_t max_bytes, bool exact_size,
                                  NkCheatDbCancelFn should_cancel, void *cancel_context,
                                  char **out, size_t *out_size, char *error, size_t error_size) {
    PackageHttpResponse response;
    char *buffer = NULL;
    size_t used = 0u;
    size_t capacity;
    NkCheatDbStatus status = NK_CHEAT_DB_OK;
    *out = NULL;
    *out_size = 0u;
    if (!transport) return fail(NK_CHEAT_DB_BAD_ARGUMENT, error, error_size, "no transport");
    if (should_cancel && should_cancel(cancel_context)) {
        return fail(NK_CHEAT_DB_CANCELLED, error, error_size, "cancelled");
    }
    memset(&response, 0, sizeof(response));
    if (!transport(transport_context, url, NK_CHEAT_DB_HOST, &response)) {
        return fail(NK_CHEAT_DB_NOT_REACHED, error, error_size,
                    response.refused ? response.refused : "no answer from " NK_CHEAT_DB_HOST);
    }
    if (!response.final_url || !package_builder_url_allowed(response.final_url, NK_CHEAT_DB_HOST)) {
        status = fail(NK_CHEAT_DB_REDIRECT_REFUSED, error, error_size,
                      "the answer came from a host other than " NK_CHEAT_DB_HOST);
        goto cleanup;
    }
    if (response.http_status != 0 && response.http_status != 200) {
        char what[64];
        snprintf(what, sizeof(what), "the host answered HTTP %d", response.http_status);
        status = fail(NK_CHEAT_DB_HTTP_STATUS, error, error_size, what);
        goto cleanup;
    }
    if (response.has_content_length && response.content_length > max_bytes) {
        status = fail(exact_size ? NK_CHEAT_DB_SIZE_MISMATCH : NK_CHEAT_DB_TOO_LARGE, error,
                      error_size, "the answer is larger than allowed");
        goto cleanup;
    }
    capacity = (size_t)max_bytes + 1u;   /* one byte more shows a body past the cap */
    buffer = (char *)malloc(capacity);
    if (!buffer) {
        status = fail(NK_CHEAT_DB_OUT_OF_MEMORY, error, error_size, "out of memory");
        goto cleanup;
    }
    for (;;) {
        size_t got = 0u;
        size_t room = capacity - used;
        if (should_cancel && should_cancel(cancel_context)) {
            status = fail(NK_CHEAT_DB_CANCELLED, error, error_size, "cancelled");
            goto cleanup;
        }
        if (room == 0u) {
            status = fail(exact_size ? NK_CHEAT_DB_SIZE_MISMATCH : NK_CHEAT_DB_TOO_LARGE, error,
                          error_size, "the answer is larger than allowed");
            goto cleanup;
        }
        if (!response.read ||
            !response.read(response.context, (unsigned char *)buffer + used,
                           room < CHEAT_DB_READ_CHUNK ? room : CHEAT_DB_READ_CHUNK, &got)) {
            status = fail(NK_CHEAT_DB_TRUNCATED, error, error_size, "the download broke off");
            goto cleanup;
        }
        if (got == 0u) break;
        if (got > room) {
            status = fail(NK_CHEAT_DB_IO_ERROR, error, error_size, "the transport overran its buffer");
            goto cleanup;
        }
        used += got;
    }
    if (used > max_bytes) {
        status = fail(exact_size ? NK_CHEAT_DB_SIZE_MISMATCH : NK_CHEAT_DB_TOO_LARGE, error,
                      error_size, "the answer is larger than allowed");
        goto cleanup;
    }
    if (response.has_content_length && (uint64_t)used != response.content_length) {
        status = fail(NK_CHEAT_DB_TRUNCATED, error, error_size,
                      "the download ended before its declared length");
        goto cleanup;
    }
    if (exact_size && (uint64_t)used != max_bytes) {
        status = fail(NK_CHEAT_DB_SIZE_MISMATCH, error, error_size,
                      "the download is not the pinned size");
        goto cleanup;
    }
    *out = buffer;
    *out_size = used;
    buffer = NULL;
cleanup:
    if (response.close) response.close(response.context);
    free(buffer);
    return status;
}

NkCheatDbStatus nk_cheat_db_fetch_sources(PackageHttpOpen transport, void *transport_context,
                                          NkCheatDbSources *out, char *error, size_t error_size) {
    char *body = NULL;
    size_t size = 0u;
    NkCheatDbStatus status;
    if (!out) return fail(NK_CHEAT_DB_BAD_ARGUMENT, error, error_size, "no list");
    memset(out, 0, sizeof(*out));
    status = fetch_body(transport, transport_context, NK_CHEAT_DB_SOURCES_URL,
                        NK_CHEAT_DB_SOURCES_MAX_BYTES, false, NULL, NULL, &body, &size, error,
                        error_size);
    if (status != NK_CHEAT_DB_OK) return status;
    status = nk_cheat_db_parse_sources(body, size, out, error, error_size);
    free(body);
    return status;
}

/* ---- Checking and writing ---- */

/* _S lines, counted as the reader sees a line start: at the start or after a newline, after
 * blanks. */
static uint32_t count_games(const char *text, size_t size) {
    uint32_t games = 0u;
    size_t i = 0u;
    while (i < size) {
        size_t j = i;
        while (j < size && (text[j] == ' ' || text[j] == '\t')) j++;
        if (j + 1u < size && text[j] == '_' && text[j + 1u] == 'S' && games < UINT32_MAX) games++;
        while (i < size && text[i] != '\n') i++;
        i++;
    }
    return games;
}

NkCheatDbStatus nk_cheat_db_check_text(const char *text, size_t size, NkCheatDbCounts *counts,
                                       char *error, size_t error_size) {
    /* No disc ID: the reader keeps only cheats outside every _S section (there must be none)
     * and counts the others. A layout is needed for the load-time checks; this one is PSP user
     * memory with a game image at its usual base, and nothing is applied. */
    NkCheatConfig config;
    NkCheatSummary summary;
    NkCheatSet *set;
    char reason[NK_CHEATS_REASON_MAX];
    uint32_t games;
    if (counts) memset(counts, 0, sizeof(*counts));
    if (!text && size != 0u) return fail(NK_CHEAT_DB_BAD_ARGUMENT, error, error_size, "no text");
    if (size > NK_CHEATS_MAX_FILE_BYTES) {
        return fail(NK_CHEAT_DB_TOO_LARGE, error, error_size, "the file is larger than a cheat file may be");
    }
    memset(&config, 0, sizeof(config));
    config.disc_id = NULL;
    config.user_base = NK_CHEATS_ADDRESS_BASE;
    config.user_end = 0x0A000000u;
    config.psp_image_base = 0x08804000u;
    config.image_base = 0x08804000u;
    reason[0] = '\0';
    set = nk_cheats_load(text ? text : "", size, &config, NULL, reason, sizeof(reason));
    if (!set) {
        char what[NK_CHEATS_REASON_MAX + 48];
        snprintf(what, sizeof(what), "the CWCheat reader refused it (%s)", reason);
        return fail(NK_CHEAT_DB_NOT_A_DATABASE, error, error_size, what);
    }
    nk_cheats_summary(set, &summary);
    if (nk_cheats_count(set) != 0u || summary.not_kept != 0u) {
        nk_cheats_free(set);
        return fail(NK_CHEAT_DB_NOT_A_DATABASE, error, error_size,
                    "it has cheats outside any game's _S line, which would apply to every game");
    }
    nk_cheats_free(set);
    games = count_games(text, size);
    if (games == 0u || summary.other_game == 0u) {
        return fail(NK_CHEAT_DB_NOT_A_DATABASE, error, error_size, "it names no game and no cheat");
    }
    if (counts) {
        counts->games = games;
        counts->cheats = summary.other_game;
    }
    return NK_CHEAT_DB_OK;
}

static bool ensure_parent_dir(const char *file_path) {
    char dir[NK_MAX_PATH];
    size_t len = strlen(file_path);
    if (len == 0u || len >= sizeof(dir)) return false;
    memcpy(dir, file_path, len + 1u);
    while (len > 0u && dir[len - 1u] != '/' && dir[len - 1u] != '\\') --len;
    if (len <= 1u) return false;
    dir[len - 1u] = '\0';
    return nk_platform_mkdir_p_private(dir);
}

static NkCheatDbStatus write_atomic(const char *dest_path, const char *bytes, size_t size,
                                    char *error, size_t error_size) {
    char tmp[NK_MAX_PATH + 16];
    FILE *f;
    bool ok;
    int n;
    if (!ensure_parent_dir(dest_path)) {
        return fail(NK_CHEAT_DB_IO_ERROR, error, error_size, "the cheats folder could not be made");
    }
    n = snprintf(tmp, sizeof(tmp), "%s.part", dest_path);
    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        return fail(NK_CHEAT_DB_IO_ERROR, error, error_size, "the path is too long");
    }
    f = nk_platform_fopen_private(tmp, "wb");
    if (!f) return fail(NK_CHEAT_DB_IO_ERROR, error, error_size, "the file could not be written");
    ok = fwrite(bytes, 1u, size, f) == size && nk_platform_sync_file(f);
    if (fclose(f) != 0) ok = false;
    if (ok) ok = nk_rename_utf8(tmp, dest_path) == 0;
    if (!ok) {
        (void)nk_remove_utf8(tmp);
        return fail(NK_CHEAT_DB_IO_ERROR, error, error_size, "the file could not be written");
    }
    return NK_CHEAT_DB_OK;
}

NkCheatDbStatus nk_cheat_db_download(PackageHttpOpen transport, void *transport_context,
                                     const NkCheatDbSource *source, const char *dest_path,
                                     bool overwrite, NkCheatDbCancelFn should_cancel,
                                     void *cancel_context, NkCheatDbCounts *counts,
                                     char *error, size_t error_size) {
    char *body = NULL;
    size_t size = 0u;
    uint8_t digest[32];
    NkSha256 sha;
    NkCheatDbStatus status;
    if (counts) memset(counts, 0, sizeof(*counts));
    if (!source || !dest_path || !dest_path[0] || source->size == 0u ||
        source->size > NK_CHEATS_MAX_FILE_BYTES || !pinned_url_ok(source->url, source->commit)) {
        return fail(NK_CHEAT_DB_BAD_ARGUMENT, error, error_size, "not a pinned source");
    }
    if (!overwrite && nk_platform_file_exists(dest_path)) {
        return fail(NK_CHEAT_DB_EXISTS, error, error_size,
                    "a cheat database is already there; confirm to replace it");
    }
    status = fetch_body(transport, transport_context, source->url, source->size, true,
                        should_cancel, cancel_context, &body, &size, error, error_size);
    if (status != NK_CHEAT_DB_OK) return status;
    nk_sha256_init(&sha);
    nk_sha256_update(&sha, (const uint8_t *)body, size);
    nk_sha256_finish(&sha, digest);
    if (memcmp(digest, source->sha256, sizeof(digest)) != 0) {
        free(body);
        return fail(NK_CHEAT_DB_HASH_MISMATCH, error, error_size,
                    "the download is not the file the list pins; nothing was written");
    }
    status = nk_cheat_db_check_text(body, size, counts, error, error_size);
    if (status == NK_CHEAT_DB_OK && should_cancel && should_cancel(cancel_context)) {
        status = fail(NK_CHEAT_DB_CANCELLED, error, error_size, "cancelled");
    }
    if (status == NK_CHEAT_DB_OK) status = write_atomic(dest_path, body, size, error, error_size);
    free(body);
    return status;
}

NkCheatDbStatus nk_cheat_db_import_file(const char *source_path, const char *dest_path,
                                        bool overwrite, NkCheatDbCounts *counts,
                                        char *error, size_t error_size) {
    FILE *f;
    char *body;
    size_t used = 0u;
    NkCheatDbStatus status;
    if (counts) memset(counts, 0, sizeof(*counts));
    if (!source_path || !source_path[0] || !dest_path || !dest_path[0]) {
        return fail(NK_CHEAT_DB_BAD_ARGUMENT, error, error_size, "no file");
    }
    if (!overwrite && nk_platform_file_exists(dest_path)) {
        return fail(NK_CHEAT_DB_EXISTS, error, error_size,
                    "a cheat database is already there; confirm to replace it");
    }
    f = nk_fopen_utf8(source_path, "rb");
    if (!f) return fail(NK_CHEAT_DB_IO_ERROR, error, error_size, "the file could not be opened");
    body = NULL;
    for (size_t capacity = 0u;;) {
        size_t got;
        /* One byte past the cap shows a file that is too large. */
        if (used == capacity) {
            size_t grown = capacity == 0u ? CHEAT_DB_READ_CHUNK : capacity * 2u;
            char *resized;
            if (capacity > (size_t)NK_CHEATS_MAX_FILE_BYTES) break;
            if (grown > (size_t)NK_CHEATS_MAX_FILE_BYTES + 1u) grown = (size_t)NK_CHEATS_MAX_FILE_BYTES + 1u;
            resized = (char *)realloc(body, grown);
            if (!resized) {
                fclose(f);
                free(body);
                return fail(NK_CHEAT_DB_OUT_OF_MEMORY, error, error_size, "out of memory");
            }
            body = resized;
            capacity = grown;
        }
        got = fread(body + used, 1u, capacity - used, f);
        used += got;
        if (got == 0u) break;
    }
    if (ferror(f)) {
        fclose(f);
        free(body);
        return fail(NK_CHEAT_DB_IO_ERROR, error, error_size, "the file could not be read");
    }
    fclose(f);
    status = nk_cheat_db_check_text(body, used, counts, error, error_size);
    if (status == NK_CHEAT_DB_OK) status = write_atomic(dest_path, body, used, error, error_size);
    free(body);
    return status;
}

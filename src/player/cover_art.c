/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Opt-in libretro box art client & cache (cover_art.h). */

#include "cover_art.h"
#include "nk_json.h"
#include "nk_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NK_COVER_ART_USER_AGENT "NakagawaRecomp/CoverArt"
#define NK_COVER_ART_READ_CHUNK 16384u

static PackageHttpOpen resolve_transport(PackageHttpOpen transport, void **context) {
    static const PackageHttpOptions options = { NK_COVER_ART_USER_AGENT, NULL };
    if (transport) return transport;
    if (context) *context = (void *)&options;
    return package_builder_native_transport();
}

static bool is_libretro_replaced_char(char c) {
    return c == '&' || c == '*' || c == '/' || c == ':' || c == '<' ||
           c == '>' || c == '?' || c == '\\' || c == '|' || c == '"';
}

static bool is_unreserved_uri_byte(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool normalize_sha1_lower(const char *sha1_hex, char out[41]) {
    if (!sha1_hex || strlen(sha1_hex) != 40u) return false;
    for (size_t i = 0u; i < 40u; i++) {
        int nib = hex_nibble(sha1_hex[i]);
        if (nib < 0) return false;
        out[i] = (char)(nib < 10 ? '0' + nib : 'a' + (nib - 10));
    }
    out[40] = '\0';
    return true;
}

static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

const char *nk_cover_art_status_name(NkCoverArtStatus status) {
    switch (status) {
        case NK_COVER_ART_OK: return "OK";
        case NK_COVER_ART_NO_MATCH: return "NO_MATCH";
        case NK_COVER_ART_NOT_FOUND: return "NOT_FOUND";
        case NK_COVER_ART_INVALID_PNG: return "INVALID_PNG";
        case NK_COVER_ART_TOO_LARGE: return "TOO_LARGE";
        case NK_COVER_ART_CANCELLED: return "CANCELLED";
        case NK_COVER_ART_UNSUPPORTED: return "UNSUPPORTED";
        case NK_COVER_ART_IO_ERROR: return "IO_ERROR";
        case NK_COVER_ART_REDIRECT_REFUSED: return "REDIRECT_REFUSED";
        default: return "UNKNOWN";
    }
}

void nk_cover_art_misses_clear(NkCoverArtMisses *misses) {
    if (!misses) return;
    misses->count = 0u;
    misses->next = 0u;
}

bool nk_cover_art_misses_contains(const NkCoverArtMisses *misses, const char *sha1_hex) {
    char lower[41];
    if (!misses || !normalize_sha1_lower(sha1_hex, lower)) return false;
    for (size_t i = 0u; i < misses->count; i++) {
        if (memcmp(misses->sha1[i], lower, sizeof(lower)) == 0) return true;
    }
    return false;
}

bool nk_cover_art_misses_add(NkCoverArtMisses *misses, const char *sha1_hex) {
    char lower[41];
    if (!misses || !normalize_sha1_lower(sha1_hex, lower)) return false;
    if (nk_cover_art_misses_contains(misses, lower)) return true;
    if (misses->count < NK_COVER_ART_MISS_SLOTS) {
        memcpy(misses->sha1[misses->count++], lower, sizeof(lower));
        return true;
    }
    memcpy(misses->sha1[misses->next], lower, sizeof(lower));
    misses->next = (misses->next + 1u) % NK_COVER_ART_MISS_SLOTS;
    return true;
}

bool nk_cover_art_sanitize_name(const char *redump_name, char *out, size_t out_size) {
    if (out && out_size > 0u) out[0] = '\0';
    if (!redump_name || !redump_name[0] || !out || out_size == 0u) return false;
    size_t len = strlen(redump_name);
    if (len >= out_size) return false;
    if (!nk_json_validate_utf8((const uint8_t *)redump_name, len)) return false;
    for (size_t i = 0u; i < len; i++) {
        unsigned char c = (unsigned char)redump_name[i];
        if (c < 0x20u || c == 0x7Fu) {
            out[0] = '\0';
            return false;
        }
        out[i] = is_libretro_replaced_char((char)c) ? '_' : (char)c;
    }
    out[len] = '\0';
    return true;
}

bool nk_cover_art_boxart_url(const char *redump_name, char *out_url, size_t out_url_size) {
    static const char hex[] = "0123456789ABCDEF";
    static const char prefix[] = NK_COVER_ART_BOXARTS_URL_PREFIX;
    char sanitized[NK_DISC_DAT_MAX_GAME_NAME + 1];
    if (out_url && out_url_size > 0u) out_url[0] = '\0';
    if (!out_url || out_url_size == 0u) return false;
    if (!nk_cover_art_sanitize_name(redump_name, sanitized, sizeof(sanitized))) return false;
    size_t cap = out_url_size < NK_COVER_ART_URL_MAX ? out_url_size : NK_COVER_ART_URL_MAX;
    size_t used = sizeof(prefix) - 1u;
    if (used + 5u > cap) return false;
    memcpy(out_url, prefix, used);
    for (size_t i = 0u; sanitized[i] != '\0'; i++) {
        unsigned char c = (unsigned char)sanitized[i];
        if (is_unreserved_uri_byte(c)) {
            if (used + 1u >= cap) {
                out_url[0] = '\0';
                return false;
            }
            out_url[used++] = (char)c;
        } else {
            if (used + 3u >= cap) {
                out_url[0] = '\0';
                return false;
            }
            out_url[used++] = '%';
            out_url[used++] = hex[c >> 4];
            out_url[used++] = hex[c & 0x0Fu];
        }
    }
    if (used + 5u > cap) {
        out_url[0] = '\0';
        return false;
    }
    memcpy(out_url + used, ".png", 5u);
    return true;
}

bool nk_cover_art_cache_dir(const char *data_root, char *out_dir, size_t out_size) {
    char sep = nk_platform_path_separator();
    int n;
    if (out_dir && out_size > 0u) out_dir[0] = '\0';
    if (!data_root || !data_root[0] || !out_dir || out_size == 0u) return false;
    n = snprintf(out_dir, out_size, "%s%c%s%c%s", data_root, sep,
                 NK_COVER_ART_CACHE_DIR, sep, NK_COVER_ART_CACHE_SUBDIR);
    if (n < 0 || (size_t)n >= out_size) {
        out_dir[0] = '\0';
        return false;
    }
    return true;
}

bool nk_cover_art_cache_path(const char *data_root, const char *sha1_hex,
                             char *out_path, size_t out_size) {
    char dir[NK_MAX_PATH];
    char lower[41];
    char sep = nk_platform_path_separator();
    int n;
    if (out_path && out_size > 0u) out_path[0] = '\0';
    if (!out_path || out_size == 0u) return false;
    if (!nk_cover_art_cache_dir(data_root, dir, sizeof(dir)) ||
        !normalize_sha1_lower(sha1_hex, lower)) {
        return false;
    }
    n = snprintf(out_path, out_size, "%s%c%s.png", dir, sep, lower);
    if (n < 0 || (size_t)n >= out_size) {
        out_path[0] = '\0';
        return false;
    }
    return true;
}

bool nk_cover_art_cache_size(const char *data_root, uint64_t *out_bytes) {
    char dir[NK_MAX_PATH];
    if (out_bytes) *out_bytes = 0u;
    if (!out_bytes || !nk_cover_art_cache_dir(data_root, dir, sizeof(dir))) return false;
    return nk_platform_tree_size(dir, NULL, out_bytes);
}

void nk_cover_art_format_size(uint64_t bytes, char *out, size_t out_size) {
    if (!out || out_size == 0u) return;
    if (bytes == 0u) {
        snprintf(out, out_size, "0 KB");
    } else if (bytes < 1024u * 1024u) {
        uint64_t kb = (bytes + 1023u) / 1024u;
        snprintf(out, out_size, "%llu KB", (unsigned long long)kb);
    } else if (bytes < 1024ull * 1024ull * 1024ull) {
        double mb = (double)bytes / (1024.0 * 1024.0);
        snprintf(out, out_size, "%.1f MB", mb);
    } else {
        double gb = (double)bytes / (1024.0 * 1024.0 * 1024.0);
        snprintf(out, out_size, "%.1f GB", gb);
    }
}

bool nk_cover_art_clear_cache(const char *data_root,
                              char *error_code, size_t error_code_size,
                              char *error_message, size_t error_message_size) {
    char dir[NK_MAX_PATH];
    if (error_code && error_code_size > 0u) error_code[0] = '\0';
    if (error_message && error_message_size > 0u) error_message[0] = '\0';
    if (!data_root || !data_root[0]) {
        if (error_code && error_code_size > 0u) snprintf(error_code, error_code_size, "DATA_DIR_UNAVAILABLE");
        if (error_message && error_message_size > 0u) {
            snprintf(error_message, error_message_size,
                     "The app-data folder is unavailable. No cover art was removed.");
        }
        return false;
    }
    if (!nk_cover_art_cache_dir(data_root, dir, sizeof(dir))) {
        if (error_code && error_code_size > 0u) snprintf(error_code, error_code_size, "PATH_TOO_LONG");
        if (error_message && error_message_size > 0u) {
            snprintf(error_message, error_message_size,
                     "The cover art cache path is too long. No files were removed.");
        }
        return false;
    }
    if (!nk_platform_remove_tree(dir, NULL)) {
        if (error_code && error_code_size > 0u) snprintf(error_code, error_code_size, "COVER_ART_CLEAR_FAILED");
        if (error_message && error_message_size > 0u) {
            snprintf(error_message, error_message_size,
                     "Couldn't delete every file in the cover art cache at %s.", dir);
        }
        return false;
    }
    return true;
}

bool nk_cover_art_validate_png_header(const uint8_t *bytes, size_t size,
                                      uint32_t max_side,
                                      uint32_t *out_width, uint32_t *out_height) {
    static const uint8_t kPngSig[8] = {0x89u, 'P', 'N', 'G', 0x0Du, 0x0Au, 0x1Au, 0x0Au};
    if (out_width) *out_width = 0u;
    if (out_height) *out_height = 0u;
    if (!bytes || size < 33u) return false;
    if (memcmp(bytes, kPngSig, 8u) != 0) return false;
    if (read_be32(bytes + 8u) != 13u || memcmp(bytes + 12u, "IHDR", 4u) != 0) return false;
    uint32_t width = read_be32(bytes + 16u);
    uint32_t height = read_be32(bytes + 20u);
    if (width == 0u || height == 0u || (max_side > 0u && (width > max_side || height > max_side))) {
        return false;
    }
    if (out_width) *out_width = width;
    if (out_height) *out_height = height;
    return true;
}

static bool ensure_parent_dir_for_file(const char *file_path) {
    char dir[NK_MAX_PATH];
    size_t len;
    if (!file_path || !file_path[0]) return false;
    len = strlen(file_path);
    if (len >= sizeof(dir)) return false;
    memcpy(dir, file_path, len + 1u);
    while (len > 0u && dir[len - 1u] != '/' && dir[len - 1u] != '\\') {
        --len;
    }
    if (len == 0u) return false;
    dir[len - 1u] = '\0';
    return nk_platform_mkdir_p_private(dir);
}

static bool write_file_atomic(const char *dest_path, const uint8_t *bytes, size_t size) {
    char tmp[NK_MAX_PATH + 16];
    int n;
    if (!dest_path || !bytes || size == 0u) return false;
    if (!ensure_parent_dir_for_file(dest_path)) return false;
    n = snprintf(tmp, sizeof(tmp), "%s.tmp", dest_path);
    if (n < 0 || (size_t)n >= sizeof(tmp)) return false;
    FILE *f = nk_platform_fopen_private(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(bytes, 1u, size, f) == size && nk_platform_sync_file(f);
    if (fclose(f) != 0) ok = false;
    if (ok) ok = nk_rename_utf8(tmp, dest_path) == 0;
    if (!ok) (void)nk_remove_utf8(tmp);
    return ok;
}

NkCoverArtStatus nk_cover_art_fetch_url(PackageHttpOpen transport, void *transport_context,
                                        const char *url, const char *allowed_hosts,
                                        const char *dest_path,
                                        NkCoverArtCancelFn should_cancel, void *cancel_ctx,
                                        const NkCoverArtCacheLock *cache_lock) {
    PackageHttpResponse response;
    uint8_t *buffer = NULL;
    size_t used = 0u;
    size_t capacity = 0u;
    NkCoverArtStatus status = NK_COVER_ART_NOT_FOUND;

    if (!url || strncmp(url, "https://", 8u) != 0 || !allowed_hosts || !allowed_hosts[0] ||
        !dest_path || !dest_path[0]) {
        return NK_COVER_ART_NO_MATCH;
    }
    if (should_cancel && should_cancel(cancel_ctx)) return NK_COVER_ART_CANCELLED;

    transport = resolve_transport(transport, &transport_context);
    if (!transport) return NK_COVER_ART_UNSUPPORTED;

    memset(&response, 0, sizeof(response));
    if (!transport(transport_context, url, allowed_hosts, &response)) {
        return NK_COVER_ART_NOT_FOUND;
    }
    /* A transport that followed redirects reports where the bytes come from; a
       host outside the allowlist is refused here, whatever the transport did. */
    if (!response.final_url || !package_builder_url_allowed(response.final_url, allowed_hosts)) {
        if (response.close) response.close(response.context);
        return NK_COVER_ART_REDIRECT_REFUSED;
    }
    if (response.has_content_length && response.content_length > NK_COVER_ART_MAX_BYTES) {
        if (response.close) response.close(response.context);
        return NK_COVER_ART_TOO_LARGE;
    }

    capacity = response.has_content_length && response.content_length > 0u &&
                       response.content_length <= NK_COVER_ART_MAX_BYTES
                   ? (size_t)response.content_length
                   : 65536u;
    buffer = (uint8_t *)malloc(capacity);
    if (!buffer) {
        if (response.close) response.close(response.context);
        return NK_COVER_ART_IO_ERROR;
    }

    for (;;) {
        uint8_t chunk[NK_COVER_ART_READ_CHUNK];
        size_t bytes_read = 0u;
        if (should_cancel && should_cancel(cancel_ctx)) {
            status = NK_COVER_ART_CANCELLED;
            goto cleanup;
        }
        if (!response.read || !response.read(response.context, chunk, sizeof(chunk), &bytes_read)) {
            status = NK_COVER_ART_NOT_FOUND;
            goto cleanup;
        }
        if (bytes_read == 0u) break;
        if (bytes_read > NK_COVER_ART_MAX_BYTES - used) {
            status = NK_COVER_ART_TOO_LARGE;
            goto cleanup;
        }
        if (used + bytes_read > capacity) {
            size_t grown = capacity;
            while (grown < used + bytes_read) {
                grown = grown > NK_COVER_ART_MAX_BYTES / 2u ? NK_COVER_ART_MAX_BYTES : grown * 2u;
            }
            uint8_t *resized = (uint8_t *)realloc(buffer, grown);
            if (!resized) {
                status = NK_COVER_ART_IO_ERROR;
                goto cleanup;
            }
            buffer = resized;
            capacity = grown;
        }
        memcpy(buffer + used, chunk, bytes_read);
        used += bytes_read;
    }

    if (should_cancel && should_cancel(cancel_ctx)) {
        status = NK_COVER_ART_CANCELLED;
        goto cleanup;
    }
    /* A body that ended before its declared length is a broken download, not a picture:
       the header check reads only its first 33 bytes, and the cache keeps what it is given. */
    if (response.has_content_length && (uint64_t)used != response.content_length) {
        status = NK_COVER_ART_NOT_FOUND;
        goto cleanup;
    }
    if (!nk_cover_art_validate_png_header(buffer, used, NK_COVER_ART_MAX_SIDE, NULL, NULL)) {
        status = NK_COVER_ART_INVALID_PNG;
        goto cleanup;
    }
    {
        const bool locked = cache_lock && cache_lock->lock && cache_lock->unlock;
        if (locked) cache_lock->lock(cache_lock->context);
        /* Checked again under the lock: a deleter cancels before it takes the lock. */
        if (should_cancel && should_cancel(cancel_ctx)) {
            status = NK_COVER_ART_CANCELLED;
        } else if (!write_file_atomic(dest_path, buffer, used)) {
            status = NK_COVER_ART_IO_ERROR;
        } else {
            status = NK_COVER_ART_OK;
        }
        if (locked) cache_lock->unlock(cache_lock->context);
    }

cleanup:
    if (response.close) response.close(response.context);
    free(buffer);
    return status;
}

NkCoverArtStatus nk_cover_art_fetch_for_disc(PackageHttpOpen transport, void *transport_context,
                                             const NkDiscDat *dat,
                                             const char *data_root,
                                             const char *disc_id,
                                             const char *sha1_hex,
                                             const char *crc32_hex,
                                             const NkCoverArtProviders *providers,
                                             NkCoverArtCancelFn should_cancel, void *cancel_ctx,
                                             const NkCoverArtCacheLock *cache_lock,
                                             NkCoverArtSource *out_source) {
    char dest_path[NK_MAX_PATH];
    char url[NK_COVER_ART_URL_MAX];
    const char *redump_name = NULL;
    NkCoverArtStatus primary_status = NK_COVER_ART_NO_MATCH;

    if (out_source) *out_source = NK_COVER_ART_SOURCE_NONE;
    if (should_cancel && should_cancel(cancel_ctx)) return NK_COVER_ART_CANCELLED;
    if (!nk_cover_art_cache_path(data_root, sha1_hex, dest_path, sizeof(dest_path))) {
        return NK_COVER_ART_NO_MATCH;
    }

    if (dat) {
        redump_name = nk_disc_dat_find_game_name(dat, sha1_hex, crc32_hex, 0u);
    }
    if (redump_name && nk_cover_art_boxart_url(redump_name, url, sizeof(url))) {
        primary_status = nk_cover_art_fetch_url(transport, transport_context, url,
                                                NK_COVER_ART_HOST, dest_path,
                                                should_cancel, cancel_ctx, cache_lock);
        if (primary_status == NK_COVER_ART_OK) {
            if (out_source) *out_source = NK_COVER_ART_SOURCE_LIBRETRO;
            return NK_COVER_ART_OK;
        }
        if (primary_status == NK_COVER_ART_CANCELLED ||
            primary_status == NK_COVER_ART_UNSUPPORTED) {
            return primary_status;
        }
    }

    if (should_cancel && should_cancel(cancel_ctx)) return NK_COVER_ART_CANCELLED;

    /* RetroAchievements (#1316) fallback seam: only queried when primary lookup
       found no Redump/libretro box art and a provider is installed. */
    if (providers && providers->fallback) {
        char fb_url[NK_COVER_ART_URL_MAX];
        char fb_hosts[256];
        memset(fb_url, 0, sizeof(fb_url));
        memset(fb_hosts, 0, sizeof(fb_hosts));
        if (providers->fallback(providers->fallback_context,
                                disc_id ? disc_id : "",
                                sha1_hex ? sha1_hex : "",
                                crc32_hex ? crc32_hex : "",
                                fb_url, sizeof(fb_url),
                                fb_hosts, sizeof(fb_hosts)) &&
            strncmp(fb_url, "https://", 8u) == 0 && fb_hosts[0] != '\0') {
            NkCoverArtStatus fb_status = nk_cover_art_fetch_url(
                transport, transport_context, fb_url, fb_hosts, dest_path,
                should_cancel, cancel_ctx, cache_lock);
            if (fb_status == NK_COVER_ART_OK) {
                if (out_source) *out_source = NK_COVER_ART_SOURCE_RETROACHIEVEMENTS;
                return NK_COVER_ART_OK;
            }
            if (fb_status == NK_COVER_ART_CANCELLED || fb_status == NK_COVER_ART_UNSUPPORTED) {
                return fb_status;
            }
        }
    }

    return primary_status;
}

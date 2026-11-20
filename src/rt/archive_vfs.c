/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "archive_vfs.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

int sr_archive_variant_from_name(const char *name, int *variant_out) {
    if (!name || !variant_out) return 0;
    const char *base = strrchr(name, '.');
    if (!base || !(base[1] == 'x' || base[1] == 'X') ||
        !(base[2] == 'b' || base[2] == 'B')) return 0;
    const char *suffix = base + 3;
    if (*suffix == '\0') {
        *variant_out = -1;
        return 1;
    }
    uint64_t value = 0;
    while (*suffix) {
        if (*suffix < '0' || *suffix > '9') return 0;
        uint64_t digit = (uint64_t)(*suffix - '0');
        if (value > ((uint64_t)INT_MAX - digit) / 10u) return 0;
        value = value * 10u + digit;
        suffix++;
    }
    *variant_out = (int)value;
    return 1;
}

static int archive_component_valid(const char *out, size_t start, size_t end) {
    if (end == start) return 0;
    if (end - start == 1u && out[start] == '.') return 0;
    if (end - start == 2u && out[start] == '.' && out[start + 1u] == '.') return 0;
    return 1;
}

/* The archive key of a member path or of a guest key: separators become one '/', ASCII
 * letters fold to lower case, and a Shift-JIS pair (sr_vfs_sjis_pair) stays byte for
 * byte, as the XB reader keeps it and as the guest key (data_normalize_guest_key) keeps
 * it: its trail byte is not folded, not a separator and not checked against the
 * forbidden ASCII characters, so 0x83 0x41 and 0x83 0x61 are two keys and a trail 0x5c
 * is part of the name. A directory key may be empty and may end in a separator, which
 * is dropped. */
static int archive_normalize_key(const char *input, char *out, size_t capacity,
                                 int directory) {
    if (!input || !out || capacity == 0u) return 0;
    const unsigned char *p = (const unsigned char *)input;
    while (*p == '/' || *p == '\\') p++;
    size_t out_pos = 0;
    size_t component_start = 0;
    while (*p) {
        unsigned char value = *p;
        if (sr_vfs_sjis_pair((const char *)p)) {
            if (out_pos + 2u >= capacity) return 0;
            out[out_pos++] = (char)p[0];
            out[out_pos++] = (char)p[1];
            p += 2;
            continue;
        }
        if (value == '/' || value == '\\') {
            if (!archive_component_valid(out, component_start, out_pos)) return 0;
            if (out_pos + 1u >= capacity) return 0;
            out[out_pos++] = '/';
            component_start = out_pos;
            while (*p == '/' || *p == '\\') p++;
            continue;
        }
        if (value < 0x20u || value == 0x7fu || value == ':' || value == '*' ||
            value == '?' || value == '"' || value == '<' || value == '>' ||
            value == '|') return 0;
        if (out_pos + 1u >= capacity) return 0;
        out[out_pos++] = (char)sr_vfs_ascii_tolower(value);
        p++;
    }
    if (out_pos == component_start) {
        /* Empty, or ending in a separator. */
        if (!directory) return 0;
        if (out_pos > 0u) out_pos--;
    } else if (!archive_component_valid(out, component_start, out_pos)) {
        return 0;
    }
    out[out_pos] = '\0';
    return 1;
}

static int archive_normalize_directory(const char *input, char *out, size_t capacity) {
    if (!input || !out || capacity == 0u) return 0;
    return archive_normalize_key(input, out, capacity, 1);
}

static int archive_index_cmp(const void *left, const void *right) {
    const SrArchiveIndexEntry *a = (const SrArchiveIndexEntry *)left;
    const SrArchiveIndexEntry *b = (const SrArchiveIndexEntry *)right;
    int result = strcmp(a->key, b->key);
    if (result) return result;
    if (a->variant != b->variant) return a->variant < b->variant ? -1 : 1;
    if (a->order != b->order) return a->order < b->order ? -1 : 1;
    return 0;
}

static size_t archive_index_lower_bound(const SrArchiveVfs *vfs, const char *key) {
    if (!vfs || !key) return 0;
    size_t low = 0;
    size_t high = vfs->index_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        if (strcmp(vfs->index[middle].key, key) < 0) low = middle + 1u;
        else high = middle;
    }
    return low;
}

static int archive_index_reserve(SrArchiveVfs *vfs, size_t wanted) {
    if (!vfs || wanted <= vfs->index_capacity) return 1;
    size_t next = vfs->index_capacity ? vfs->index_capacity : 256u;
    while (next < wanted) {
        if (next > SIZE_MAX / 2u) return 0;
        next *= 2u;
    }
    if (next > SIZE_MAX / sizeof(*vfs->index)) return 0;
    SrArchiveIndexEntry *grown = (SrArchiveIndexEntry *)realloc(
        vfs->index, next * sizeof(*grown));
    if (!grown) return 0;
    memset(grown + vfs->index_capacity, 0,
           (next - vfs->index_capacity) * sizeof(*grown));
    vfs->index = grown;
    vfs->index_capacity = next;
    return 1;
}

static int archive_mount_reserve(SrArchiveVfs *vfs, size_t wanted) {
    if (!vfs || wanted <= vfs->mount_capacity) return 1;
    size_t next = vfs->mount_capacity ? vfs->mount_capacity : 4u;
    while (next < wanted) {
        if (next > SIZE_MAX / 2u) return 0;
        next *= 2u;
    }
    if (next > SIZE_MAX / sizeof(*vfs->mounts)) return 0;
    NkXbArchive *grown = (NkXbArchive *)realloc(vfs->mounts, next * sizeof(*grown));
    if (!grown) return 0;
    memset(grown + vfs->mount_capacity, 0,
           (next - vfs->mount_capacity) * sizeof(*grown));
    vfs->mounts = grown;
    vfs->mount_capacity = next;
    return 1;
}

static void archive_index_rollback(SrArchiveVfs *vfs, size_t first) {
    if (!vfs) return;
    while (vfs->index_count > first) {
        SrArchiveIndexEntry *entry = &vfs->index[--vfs->index_count];
        free(entry->key);
        free(entry->path);
        memset(entry, 0, sizeof(*entry));
    }
}

static int archive_index_finalize(SrArchiveVfs *vfs) {
    if (!vfs) return 0;
    if (!vfs->index_dirty) return 1;
    if (vfs->index_count > 1u) {
        qsort(vfs->index, vfs->index_count, sizeof(*vfs->index), archive_index_cmp);
    }
    vfs->index_dirty = 0;
    vfs->index_sort_count++;
    return 1;
}

int sr_archive_vfs_finalize(SrArchiveVfs *vfs) {
    return archive_index_finalize(vfs);
}

void sr_archive_vfs_init(SrArchiveVfs *vfs) {
    if (!vfs) return;
    memset(vfs, 0, sizeof(*vfs));
    vfs->max_cache_bytes = SR_ARCHIVE_VFS_DEFAULT_CACHE_BYTES;
    vfs->max_cache_entries = SR_ARCHIVE_VFS_DEFAULT_CACHE_ENTRIES;
}

void sr_archive_vfs_destroy(SrArchiveVfs *vfs) {
    if (!vfs) return;
    for (size_t i = 0; i < vfs->mount_count; i++) nk_xb_close(&vfs->mounts[i]);
    free(vfs->mounts);
    for (size_t i = 0; i < vfs->index_count; i++) {
        free(vfs->index[i].key);
        free(vfs->index[i].path);
    }
    free(vfs->index);
    for (size_t i = 0; i < vfs->cache_count; i++) free(vfs->cache[i].data);
    free(vfs->cache);
    free(vfs->oversize_data);
    sr_archive_vfs_init(vfs);
}

int sr_archive_vfs_configure(SrArchiveVfs *vfs, size_t max_cache_bytes,
                             size_t max_cache_entries) {
    if (!vfs || vfs->mount_count != 0u || vfs->cache_count != 0u) return 0;
    vfs->max_cache_bytes = max_cache_bytes;
    vfs->max_cache_entries = max_cache_entries;
    return 1;
}

static NkResult archive_adopt(SrArchiveVfs *vfs, NkXbArchive *archive, int variant) {
    if (!vfs || !archive || archive->entry_count == 0u || variant < -1) {
        return NK_ERROR_INVALID_XB;
    }
    if (archive->entry_count > SIZE_MAX - vfs->index_count ||
        !archive_mount_reserve(vfs, vfs->mount_count + 1u) ||
        !archive_index_reserve(vfs, vfs->index_count + archive->entry_count)) {
        return NK_ERROR_OUT_OF_MEMORY;
    }
    size_t first = vfs->index_count;
    for (size_t i = 0; i < archive->entry_count; i++) {
        const NkXbEntry *source = &archive->entries[i];
        if (source->expanded_size > UINT32_MAX) {
            archive_index_rollback(vfs, first);
            return NK_ERROR_INVALID_XB;
        }
        /* A member name with no key is a malformed archive, not a short allocation. The
         * key rule keeps a Shift-JIS pair whole, as the XB reader does, so a name that
         * reader accepted has a key; this refusal is the boundary for any other name. */
        char key[NK_XB_MAX_NAME_BYTES + 1u];
        if (!archive_normalize_key(source->path, key, sizeof(key), 0)) {
            archive_index_rollback(vfs, first);
            return NK_ERROR_INVALID_XB;
        }
        SrArchiveIndexEntry *target = &vfs->index[vfs->index_count];
        target->key = sr_asset_index_strdup(key);
        target->path = sr_asset_index_strdup(source->path);
        if (!target->key || !target->path) {
            free(target->key);
            free(target->path);
            target->key = NULL;
            target->path = NULL;
            archive_index_rollback(vfs, first);
            return NK_ERROR_OUT_OF_MEMORY;
        }
        target->mount_index = vfs->mount_count;
        target->entry_index = source->index;
        target->size = source->expanded_size;
        target->variant = variant;
        target->order = vfs->index_count;
        vfs->index_count++;
    }
    vfs->mounts[vfs->mount_count++] = *archive;
    memset(archive, 0, sizeof(*archive));
    vfs->index_dirty = 1;
    return NK_OK;
}

NkResult sr_archive_vfs_mount_file(SrArchiveVfs *vfs, const char *path,
                                   bool big_endian, int variant,
                                   const NkXbLimits *limits) {
    if (!vfs || !path || !path[0]) return NK_ERROR_INVALID_XB;
    int actual_variant = variant;
    if (actual_variant == SR_ARCHIVE_VARIANT_AUTO) {
        if (!sr_archive_variant_from_name(path, &actual_variant)) return NK_ERROR_INVALID_XB;
    }
    if (actual_variant < -1) return NK_ERROR_INVALID_XB;
    NkXbArchive archive;
    NkResult result = nk_xb_open_file_lazy(path, big_endian, limits, &archive,
                                           NULL, 0);
    if (result != NK_OK) return result;
    result = archive_adopt(vfs, &archive, actual_variant);
    if (result != NK_OK) nk_xb_close(&archive);
    return result;
}

NkResult sr_archive_vfs_mount_memory(SrArchiveVfs *vfs, const void *data,
                                     size_t data_size, const char *source_name,
                                     bool big_endian, int variant,
                                     const NkXbLimits *limits) {
    if (!vfs || !data || data_size == 0u || variant < -1) return NK_ERROR_INVALID_XB;
    NkXbArchive archive;
    NkResult result = nk_xb_open_memory(data, data_size, source_name, big_endian,
                                        limits, &archive, NULL, 0);
    if (result != NK_OK) return result;
    result = archive_adopt(vfs, &archive, variant);
    if (result != NK_OK) nk_xb_close(&archive);
    return result;
}

static int archive_file_from_index(const SrArchiveVfs *vfs,
                                   const SrArchiveIndexEntry *index,
                                   SrArchiveFile *file_out) {
    if (!vfs || !index || !file_out || index->mount_index >= vfs->mount_count) return 0;
    const NkXbArchive *mount = &vfs->mounts[index->mount_index];
    if (index->entry_index >= mount->entry_count) return 0;
    file_out->mount_index = index->mount_index;
    file_out->entry_index = index->entry_index;
    file_out->size = index->size;
    file_out->variant = index->variant;
    return 1;
}

int sr_archive_vfs_lookup(const SrArchiveVfs *vfs, const char *key,
                          int wanted_variant, SrArchiveFile *file_out) {
    if (!vfs || !key || !file_out) return 0;
    if (!archive_index_finalize((SrArchiveVfs *)vfs)) return 0;
    memset(file_out, 0, sizeof(*file_out));
    char normalized[NK_XB_MAX_NAME_BYTES + 1u];
    if (!archive_normalize_key(key, normalized, sizeof(normalized), 0)) return 0;
    int wanted = wanted_variant == SR_ARCHIVE_VARIANT_AUTO ? -2 : wanted_variant;
    if (wanted < -2) return 0;
    /* The index is sorted by (key, variant, order), so the first selected entry is the
     * one to open: for an unqualified request an unnumbered (-1) member before any
     * numbered one and the lowest number otherwise, and among equal variants the first
     * mount. sr_archive_vfs_list_dir merges entries in the same order and keeps the
     * first, so the listing and the open describe the same member. */
    size_t first = archive_index_lower_bound(vfs, normalized);
    for (size_t i = first; i < vfs->index_count; i++) {
        const SrArchiveIndexEntry *candidate = &vfs->index[i];
        if (strcmp(candidate->key, normalized) != 0) break;
        if (!sr_asset_index_variant_selected(candidate->variant, wanted)) continue;
        return archive_file_from_index(vfs, candidate, file_out);
    }
    return 0;
}

static uint64_t archive_next_use(SrArchiveVfs *vfs) {
    if (vfs->use_clock == UINT64_MAX) {
        for (size_t i = 0; i < vfs->cache_count; i++) vfs->cache[i].last_used = 0;
        vfs->use_clock = 0;
    }
    return ++vfs->use_clock;
}

static SrArchiveCacheEntry *archive_cache_find(SrArchiveVfs *vfs,
                                               size_t mount_index,
                                               size_t entry_index) {
    for (size_t i = 0; i < vfs->cache_count; i++) {
        SrArchiveCacheEntry *entry = &vfs->cache[i];
        if (entry->mount_index == mount_index && entry->entry_index == entry_index) {
            entry->last_used = archive_next_use(vfs);
            return entry;
        }
    }
    return NULL;
}

static void archive_cache_remove(SrArchiveVfs *vfs, size_t index) {
    if (!vfs || index >= vfs->cache_count) return;
    free(vfs->cache[index].data);
    vfs->cache_bytes -= vfs->cache[index].size;
    if (index + 1u < vfs->cache_count) {
        memmove(&vfs->cache[index], &vfs->cache[index + 1u],
                (vfs->cache_count - index - 1u) * sizeof(vfs->cache[0]));
    }
    vfs->cache_count--;
    memset(&vfs->cache[vfs->cache_count], 0, sizeof(vfs->cache[0]));
}

static int archive_cache_reserve(SrArchiveVfs *vfs, size_t wanted) {
    if (wanted <= vfs->cache_capacity) return 1;
    size_t next = vfs->cache_capacity ? vfs->cache_capacity : 8u;
    while (next < wanted) {
        if (next > SIZE_MAX / 2u) return 0;
        next *= 2u;
    }
    if (next > vfs->max_cache_entries) next = vfs->max_cache_entries;
    if (next < wanted || next > SIZE_MAX / sizeof(*vfs->cache)) return 0;
    SrArchiveCacheEntry *grown = (SrArchiveCacheEntry *)realloc(
        vfs->cache, next * sizeof(*grown));
    if (!grown) return 0;
    memset(grown + vfs->cache_capacity, 0,
           (next - vfs->cache_capacity) * sizeof(*grown));
    vfs->cache = grown;
    vfs->cache_capacity = next;
    return 1;
}

static void archive_cache_insert(SrArchiveVfs *vfs, size_t mount_index,
                                 size_t entry_index, uint8_t *data, size_t size) {
    if (!vfs || !data || size == 0u) {
        free(data);
        return;
    }
    if (size > vfs->max_cache_bytes) {
        /* Too large for the cache: keep it as the one oversize member, in place of the
         * previous one, so the next pieces of a sequential read reuse it. */
        free(vfs->oversize_data);
        vfs->oversize_data = data;
        vfs->oversize_size = size;
        vfs->oversize_mount_index = mount_index;
        vfs->oversize_entry_index = entry_index;
        return;
    }
    if (vfs->max_cache_entries == 0u) {
        free(data);
        return;
    }
    while (vfs->cache_count > 0u &&
           (vfs->cache_count >= vfs->max_cache_entries ||
            size > vfs->max_cache_bytes - vfs->cache_bytes)) {
        size_t oldest = 0;
        for (size_t i = 1; i < vfs->cache_count; i++) {
            if (vfs->cache[i].last_used < vfs->cache[oldest].last_used) oldest = i;
        }
        archive_cache_remove(vfs, oldest);
    }
    if (!archive_cache_reserve(vfs, vfs->cache_count + 1u)) {
        free(data);
        return;
    }
    SrArchiveCacheEntry *entry = &vfs->cache[vfs->cache_count++];
    entry->mount_index = mount_index;
    entry->entry_index = entry_index;
    entry->data = data;
    entry->size = size;
    entry->last_used = archive_next_use(vfs);
    vfs->cache_bytes += size;
}

/* Each file-backed mount caches its member-read handle. A title can mount hundreds of
 * archives, more than the C runtime allows open streams, so reading from one mount
 * releases the handle of the mount read before it. */
static void archive_handle_switch(SrArchiveVfs *vfs, size_t mount_index) {
    size_t slot = mount_index + 1u;
    if (vfs->handle_mount_slot != 0u && vfs->handle_mount_slot != slot &&
        vfs->handle_mount_slot <= vfs->mount_count) {
        nk_xb_release_file_handle(&vfs->mounts[vfs->handle_mount_slot - 1u]);
    }
    vfs->handle_mount_slot = slot;
}

NkResult sr_archive_vfs_read(const SrArchiveVfs *vfs, const SrArchiveFile *file,
                             uint64_t offset, void *output, size_t output_capacity,
                             size_t *output_size) {
    if (output_size) *output_size = 0;
    if (!vfs || !file || file->mount_index >= vfs->mount_count) return NK_ERROR_INVALID_XB;
    const NkXbArchive *mount = &vfs->mounts[file->mount_index];
    if (file->entry_index >= mount->entry_count) return NK_ERROR_INVALID_XB;
    const NkXbEntry *entry = &mount->entries[file->entry_index];
    if (entry->expanded_size != file->size || file->size > SIZE_MAX) return NK_ERROR_INVALID_XB;
    uint64_t available = file->size > offset ? file->size - offset : 0;
    size_t amount = output_capacity;
    if ((uint64_t)amount > available) amount = (size_t)available;
    if (amount == 0u) return NK_OK;
    if (!output) return NK_ERROR_INVALID_XB;

    SrArchiveVfs *mutable_vfs = (SrArchiveVfs *)vfs;
    SrArchiveCacheEntry *cached = archive_cache_find(mutable_vfs, file->mount_index,
                                                     file->entry_index);
    if (cached) {
        if (offset > cached->size || amount > cached->size - (size_t)offset)
            return NK_ERROR_INVALID_XB;
        memcpy(output, cached->data + (size_t)offset, amount);
        if (output_size) *output_size = amount;
        return NK_OK;
    }
    if (vfs->oversize_data && vfs->oversize_mount_index == file->mount_index &&
        vfs->oversize_entry_index == file->entry_index) {
        if (offset > vfs->oversize_size || amount > vfs->oversize_size - (size_t)offset)
            return NK_ERROR_INVALID_XB;
        memcpy(output, vfs->oversize_data + (size_t)offset, amount);
        if (output_size) *output_size = amount;
        return NK_OK;
    }
    if (mount->file_backed) archive_handle_switch(mutable_vfs, file->mount_index);

    if (entry->compression == NK_XB_COMPRESSION_NONE) {
        if (entry->offset > mount->source_size ||
            entry->expanded_size > mount->source_size - entry->offset ||
            offset > entry->expanded_size ||
            amount > entry->expanded_size - (size_t)offset) return NK_ERROR_INVALID_XB;
        if (mount->file_backed) {
            NkResult result = nk_xb_read_entry_range(mount, file->entry_index, offset,
                                                     output, output_capacity,
                                                     output_size, NULL, 0);
            return result;
        }
        memcpy(output, mount->data + (size_t)entry->offset + (size_t)offset, amount);
        if (output_size) *output_size = amount;
        return NK_OK;
    }

    size_t expanded = (size_t)entry->expanded_size;
    uint8_t *decoded = (uint8_t *)malloc(expanded);
    if (!decoded) return NK_ERROR_OUT_OF_MEMORY;
    mutable_vfs->decode_count++;
    NkResult result = nk_xb_read_entry(mount, file->entry_index, decoded, expanded,
                                       NULL, NULL, 0);
    if (result != NK_OK) {
        free(decoded);
        return result;
    }
    if (offset > expanded || amount > expanded - (size_t)offset) {
        free(decoded);
        return NK_ERROR_INVALID_XB;
    }
    memcpy(output, decoded + (size_t)offset, amount);
    if (output_size) *output_size = amount;
    archive_cache_insert(mutable_vfs, file->mount_index, file->entry_index,
                         decoded, expanded);
    return NK_OK;
}

static size_t archive_directory_depth(const char *key) {
    if (!key || !key[0]) return 0;
    size_t depth = 1;
    for (const char *p = key; *p; p++) {
        if (*p == '/') depth++;
    }
    return depth;
}

static void archive_original_child(const char *path, size_t depth,
                                   const char *folded, size_t folded_length,
                                   char *out, size_t capacity) {
    out[0] = '\0';
    const char *p = path;
    for (size_t i = 0; i <= depth && p; i++) {
        const char *start = p;
        while (*p && *p != '/') p++;
        size_t length = (size_t)(p - start);
        if (i == depth) {
            if (length > 0u && length < capacity) {
                memcpy(out, start, length);
                out[length] = '\0';
                return;
            }
            break;
        }
        if (*p == '/') p++;
    }
    if (folded_length > 0u && folded_length < capacity) {
        memcpy(out, folded, folded_length);
        out[folded_length] = '\0';
    }
}

int sr_archive_vfs_list_dir(const SrArchiveVfs *vfs, const char *dir_key,
                            int wanted_variant, SrVfsDirList *list) {
    if (!vfs || !list) return -1;
    if (!archive_index_finalize((SrArchiveVfs *)vfs)) return -1;
    char normalized[NK_XB_MAX_NAME_BYTES + 1u];
    if (!archive_normalize_directory(dir_key, normalized, sizeof(normalized))) return -1;
    int wanted = wanted_variant == SR_ARCHIVE_VARIANT_AUTO ? -2 : wanted_variant;
    if (wanted < -2) return -1;
    char prefix[NK_XB_MAX_NAME_BYTES + 2u];
    size_t prefix_length = 0;
    if (normalized[0]) {
        size_t length = strlen(normalized);
        if (length + 2u > sizeof(prefix)) return -1;
        memcpy(prefix, normalized, length);
        prefix[length] = '/';
        prefix[length + 1u] = '\0';
        prefix_length = length + 1u;
    } else {
        prefix[0] = '\0';
    }
    size_t first = archive_index_lower_bound(vfs, prefix);
    for (size_t i = first; i < vfs->index_count; i++) {
        const SrArchiveIndexEntry *entry = &vfs->index[i];
        if (prefix_length && strncmp(entry->key, prefix, prefix_length) != 0) break;
        const char *tail = entry->key + prefix_length;
        if (!tail[0]) continue;
        list->exists = 1;
        if (!sr_asset_index_variant_selected(entry->variant, wanted)) continue;
        const char *slash = strchr(tail, '/');
        int is_dir = slash != NULL;
        size_t folded_length = is_dir ? (size_t)(slash - tail) : strlen(tail);
        if (!is_dir && entry->size > UINT32_MAX) {
            list->skipped++;
            continue;
        }
        char child[SR_VFS_NAME_MAX];
        archive_original_child(entry->path, archive_directory_depth(normalized),
                               tail, folded_length, child, sizeof(child));
        if (!child[0]) {
            list->skipped++;
            continue;
        }
        if (!sr_vfs_dirlist_merge(list, child, is_dir, is_dir ? 0u : entry->size))
            return -1;
    }
    return 1;
}

size_t sr_archive_vfs_mount_count(const SrArchiveVfs *vfs) {
    return vfs ? vfs->mount_count : 0u;
}

size_t sr_archive_vfs_entry_count(const SrArchiveVfs *vfs) {
    return vfs ? vfs->index_count : 0u;
}

size_t sr_archive_vfs_index_sort_count(const SrArchiveVfs *vfs) {
    return vfs ? vfs->index_sort_count : 0u;
}

size_t sr_archive_vfs_cache_bytes(const SrArchiveVfs *vfs) {
    return vfs ? vfs->cache_bytes : 0u;
}

size_t sr_archive_vfs_cache_entry_count(const SrArchiveVfs *vfs) {
    return vfs ? vfs->cache_count : 0u;
}

size_t sr_archive_vfs_decode_count(const SrArchiveVfs *vfs) {
    return vfs ? vfs->decode_count : 0u;
}

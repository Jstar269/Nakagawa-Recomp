/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * texrepl.h - Texture replacement packs in PPSSPP's layout (#1880), phase 1: the host half.
 *
 * A pack is a local folder or ZIP file the user supplies (the project ships none and never
 * downloads one): a textures.ini (optional in a folder) and images, each named for the texture it
 * replaces. This module reads a pack and answers "what replaces this guest texture?". It never
 * touches the GPU, guest memory or the network: the renderer (ge_gpu.c) hashes a texture it has
 * just decoded, asks here, and uploads whatever comes back through its scaled-copy path
 * (tex_scale.h).
 *
 * Where a pack is read from (docs/ENHANCEMENT_CONTRACT.md section 10.12 has the full rules):
 *   - a folder: its textures.ini and files; a textures.zip in it is read instead, as in PPSSPP;
 *   - a ZIP file, read in place (nothing is extracted): its root is the archive's top when
 *     textures.ini is there, else its one top-level folder holding textures.ini. A ZIP with no
 *     textures.ini is refused, as PPSSPP refuses one. Names in a ZIP are matched with case ignored.
 * What is read, from PPSSPP's layout (the behaviour is PPSSPP's; the code is this project's):
 *   [options]          hash = quick | xxh32 | xxh64 (required when the ini exists), ignoreAddress,
 *                      reduceHash, skipLastDXT1Blocks128x64, skipLastDXT1Blocks128x128, version;
 *                      the first line of each counts
 *   [hashes]           <key>[_<level>] = <file under the pack's root>, the key read as PPSSPP's
 *                      sscanf("%16llx%8x_%d") reads it (a short key is a wildcard); an empty file
 *                      marks a texture to leave alone; "_<level>" lines are mip levels
 *   [hashranges]       addr,w,h = newW,newH   (the area a texture's hash covers)
 *   [reducehashranges] w,h = fraction          (with reduceHash)
 *   and hash-named files in the root (24 characters, or 26-27 with "_<level>"; .png, .dds, .ktx2,
 *   .zim), which need no ini line. Only the first section of each name is read, as in PPSSPP.
 * What is not applied is reported, never silently ignored: [filtering], [games], saveIndexed,
 * DDS/KTX2/ZIM images (listed, so they match, but not decoded), a texture [indexed] marks (its
 * image holds palette indices, so it is not replaced), and any key or section this module does not
 * know each add a line to nk_texrepl_diag.
 *
 * A pack is refused whole, with a message and nothing loaded, when its folder, textures.ini or ZIP
 * cannot be read or is malformed: a line that is no comment, section or key = value, an unfinished
 * or indented section header, a line before any section, no hash type or an unknown one, a
 * textures.ini over 4 MiB, a damaged ZIP or one with an unsafe name. One image that cannot be used
 * (missing, not a regular file, outside the pack folder, over 64 MiB, a bad PNG, the wrong size for
 * its level, over the budget) is skipped: its texture is drawn as the game draws it, a line goes to
 * nk_texrepl_diag and stats.skipped counts it.
 *
 * Files are checked on the handle opened, not the name: a folder file is read only when it is a
 * regular disk file whose final path (links and junctions resolved) is inside the folder's final
 * path, and only up to its cap; a name the ZIP reader would refuse (CON.png, "a.png.", "a:b") is
 * not opened. A ZIP pack's file is checked the same way, a regular file, the folder's textures.zip
 * inside it; the ZIP reader then opens it again by name to read it.
 * Messages name the pack by its last path component only.
 */
#ifndef NK_TEXREPL_H
#define NK_TEXREPL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NK_TEXREPL_DEFAULT_BUDGET_MB 256u
/* A replacement image is at most this many texels on a side. */
#define NK_TEXREPL_MAX_DIM 2048
/* The diagnostics kept; the rest are counted in the last line. */
#define NK_TEXREPL_MAX_DIAG 40
/* A ZIP pack's entries may declare at most this many unpacked bytes in all (more is refused whole);
 * each image read is capped at 64 MiB and textures.ini at 4 MiB, in a ZIP or a folder. */
#define NK_TEXREPL_MAX_PACK_BYTES ((uint64_t)8u << 30)
/* Mip levels a texture's chain holds at most (PPSSPP's MAX_REPLACEMENT_MIP_LEVELS). */
#define NK_TEXREPL_MAX_LEVELS 12

typedef enum {
    NK_TEXREPL_HASH_QUICK = 0,
    NK_TEXREPL_HASH_XXH32,
    NK_TEXREPL_HASH_XXH64
} NkTexReplHash;

/* ---- hashes ----------------------------------------------------------------------------- */
/* PPSSPP's quick texture hash over `size` bytes. `guest_addr` is the texture's guest address:
 * its 16-byte alignment picks the vectorised variant, as the host pointer's does there. */
uint32_t nk_texrepl_quick_hash(const void *data, uint32_t size, uint32_t guest_addr);
uint32_t nk_texrepl_xxh32(const void *data, size_t len, uint32_t seed);
uint64_t nk_texrepl_xxh64(const void *data, size_t len, uint64_t seed);
/* The palette's hash in a texture's identity (XXH32 with the seed packs use). */
uint32_t nk_texrepl_clut_hash(const void *clut, uint32_t bytes);
/* The palette part of a palette texture's identity, as PPSSPP's TextureCacheCommon computes it
 * (UpdateCurrentClut, then "cluthash = clutHash_ ^ gstate.clutformat"): XXH32 of the loaded palette
 * bytes, then xor the CLUTFORMAT command word with its opcode (0xC5) in the top byte. The bytes
 * hashed are min(last_load_bytes + start_bytes, max_load_bytes), where `last_load_bytes` is the size
 * of the last CLUT load, `max_load_bytes` the largest load seen, and `start_bytes` the format's index
 * start position (in 16-colour units) times the entry size. A game that has loaded no palette yet
 * (`max_load_bytes` 0, as in a replayed capture) hashes the 16 entries a CLUT4 texture can index,
 * else 256, instead. `texture_format` is the PSP texture format word (4..7 are the palette ones). */
uint32_t nk_texrepl_clut_identity(const void *clutram, uint32_t clutram_bytes, uint32_t clut_format,
                                  uint32_t texture_format, uint32_t last_load_bytes,
                                  uint32_t max_load_bytes);
/* The 64-bit cache key of a texture: address (30 bits) and size code, xor the palette hash for a
 * palette format. `dim` is the PSP's size register (log2 width, log2 height << 8). */
uint64_t nk_texrepl_cache_key(uint32_t addr, uint32_t format, uint32_t dim, uint32_t clut_hash);
/* Bits per texel of a PSP texture format (0 for an invalid one). */
uint32_t nk_texrepl_bits_per_texel(uint32_t format);

/* ---- pack ------------------------------------------------------------------------------- */
typedef struct NkTexRepl NkTexRepl;

/* Read the pack at `path`, a folder or a ZIP file. `budget_bytes` bounds the decoded images kept in
 * memory (0 takes NK_TEXREPL_DEFAULT_BUDGET_MB). Returns NULL and fills `err` when the pack cannot be
 * read, its ZIP is damaged or holds an unsafe name, or its textures.ini is malformed; nothing is
 * loaded then. A ZIP's structure is checked whole here; each file's CRC-32 when it is first read. */
NkTexRepl *nk_texrepl_open(const char *path, uint64_t budget_bytes, char *err, size_t err_cap);
void nk_texrepl_close(NkTexRepl *pack);

/* 1 when the pack is read from a ZIP (a .zip path, or a folder's textures.zip). */
int nk_texrepl_is_zip(const NkTexRepl *pack);
NkTexReplHash nk_texrepl_hash_kind(const NkTexRepl *pack);
/* How many textures the pack lists (replacements and "leave alone" lines alike). */
size_t nk_texrepl_entry_count(const NkTexRepl *pack);
/* What was read but not applied, one line each; the last line says how many more were dropped. */
size_t nk_texrepl_diag_count(const NkTexRepl *pack);
const char *nk_texrepl_diag(const NkTexRepl *pack, size_t index);
/* The games a pack says it is made for: the disc IDs its textures.ini [games] section lists
 * (PPSSPP's per-game list), each once, in file order, in one normal form (upper case, '-' and '_'
 * dropped, so ULUS-10515 and ulus10515 are one game). A key that is not a disc ID is not kept, and
 * a pack that lists more than NK_TEXREPL_GAMES_MAX is refused. nk_texrepl_names_game returns 1
 * when disc_id (in any of those forms) is one of them, 0 when the pack names other games only, and
 * -1 when it names none, so it does not say. */
#define NK_TEXREPL_GAME_ID_MAX 16
#define NK_TEXREPL_GAMES_MAX 64
size_t nk_texrepl_game_count(const NkTexRepl *pack);
const char *nk_texrepl_game(const NkTexRepl *pack, size_t index);
int nk_texrepl_names_game(const NkTexRepl *pack, const char *disc_id);
/* Take the oldest note not yet taken, so a caller can print notes as they appear (a file that
 * fails to load shows up at its first use, long after the pack opened). Copies it to `out` and
 * returns 1, or returns 0 when there are none. Taken notes leave the list and free its room, so
 * the 40-line cap bounds the notes waiting, not the notes a run can report. When notes were dropped
 * for lack of room, the last one taken says how many. */
int nk_texrepl_diag_take(NkTexRepl *pack, char *out, size_t cap);

/* The content hash PPSSPP's replacer takes of a guest texture, with this pack's hash kind and
 * ranges. `data` points at the texture's first byte and `avail` is how many bytes are readable
 * from it (guest memory up to its end); a texture that reaches past it hashes to 0, like there.
 * `bufw` is the row stride in texels, `max_seen_v` the tallest row the game has drawn (0 when not
 * tracked: a 512-high texture is then hashed whole). */
uint32_t nk_texrepl_hash_texture(const NkTexRepl *pack, const uint8_t *data, size_t avail,
                                 uint32_t guest_addr, int bufw, int w, int h, uint32_t format,
                                 int max_seen_v);

typedef struct NkTexReplId {
    uint32_t addr;       /* guest address of level 0 */
    uint32_t format;     /* PSP texture format 0..10 */
    uint32_t dim;        /* the PSP's size register: log2 width | log2 height << 8 */
    uint32_t clut_hash;  /* nk_texrepl_clut_hash of the palette; ignored for a non-palette format */
    uint32_t hash;       /* nk_texrepl_hash_texture */
} NkTexReplId;

typedef struct NkTexReplHit {
    const uint32_t *rgba;  /* R in the low byte, as ge.c's decoder writes; owned by the pack */
    int w, h;
    void *pin;             /* internal; give the hit back with nk_texrepl_unpin */
} NkTexReplHit;

/* The replacement for a texture, or none. Returns 1 and fills `out` (the image stays in memory
 * until nk_texrepl_unpin), or 0: no entry, a "leave alone" entry, an unsupported or unreadable file,
 * or an image the budget cannot hold. The first lookup of an entry decodes its PNG on the calling
 * thread. The same as nk_texrepl_lookup_level with level 0. */
int nk_texrepl_lookup(NkTexRepl *pack, const NkTexReplId *id, NkTexReplHit *out);
/* How many mip levels the pack lists for the texture (0 to NK_TEXREPL_MAX_LEVELS; 0 when it lists
 * none or leaves the texture alone). Listed is not usable: see nk_texrepl_lookup_level. */
int nk_texrepl_level_count(const NkTexRepl *pack, const NkTexReplId *id);
/* Mip level `level` of the texture's replacement, as PPSSPP loads a chain: it counts only when
 * every level from 0 up to it is a readable PNG and level n is level 0's size halved n times (at
 * least 1), so a chain stops at its first unusable level. Returns 1 and fills `out` as
 * nk_texrepl_lookup does, or 0. The renderer does not draw levels above 0 yet. */
int nk_texrepl_lookup_level(NkTexRepl *pack, const NkTexReplId *id, int level, NkTexReplHit *out);
/* The cache key a file of this pack names texture `id` by first: nk_texrepl_cache_key, with the
 * address dropped (the high 32 bits zero) when the pack's textures.ini sets ignoreAddress. The
 * texture dump names its files with it, so they are the ones nk_texrepl_lookup reads. */
uint64_t nk_texrepl_file_key(const NkTexRepl *pack, const NkTexReplId *id);
void nk_texrepl_unpin(NkTexRepl *pack, NkTexReplHit *hit);
/* Decodes PNG bytes as a pack image is decoded: refused, with a reason in `why`, unless the bytes
 * are a whole PNG (signature, IHDR first, every chunk's CRC right, an IDAT, IEND), 1 to
 * NK_TEXREPL_MAX_DIM texels a side and, when `max_bytes` is not 0, at most that many bytes of
 * R, G, B, A pixels, and image data no longer than R + R / 8 + 70 bytes, R being the raw size
 * IHDR implies (more is work no honest encoder makes the reader do). All is checked before any
 * pixel memory is allocated. The image data is then unpacked by nk_psp_inflate_raw_into into a
 * buffer of exactly R bytes, and refused if it would unpack to more or less, if its Adler-32 is
 * wrong or not right after the stream's last block. On success `*rgba` is malloc'd (free it with
 * free) and 1 is returned. */
int nk_texrepl_decode_png(const void *data, size_t size, uint64_t max_bytes, uint32_t **rgba, int *w, int *h,
                          char *why, size_t why_cap);
/* The whole-number factor a hit is larger than a w x h texture by, the same on both axes, or 0
 * when it is not one (the renderer draws such a texture unreplaced and counts it). */
int nk_texrepl_factor(const NkTexReplHit *hit, int w, int h);

typedef struct NkTexReplStats {
    uint64_t lookups, hits, misses;
    uint64_t left_alone;      /* entries that mark a texture to keep */
    uint64_t loaded;          /* images decoded */
    uint64_t load_failed;     /* files missing, unsupported or not decodable */
    uint64_t over_budget;     /* images the budget could not hold */
    uint64_t evicted;
    uint64_t bad_factor;      /* counted by the renderer through nk_texrepl_note_bad_factor */
    uint64_t cache_bytes, cache_peak;
    uint64_t decode_ns;
    uint64_t open_ns;         /* nk_texrepl_open, by the C clock */
    uint64_t index_bytes;     /* what the open pack's own index holds (not the ZIP reader's directory) */
    uint64_t skipped;         /* texture levels found unusable and skipped from then on, each counted once
                                 (a file several levels name is read, noted and counted in load_failed once) */
    uint64_t list_ns;         /* the part of open_ns spent listing the folder, or opening and indexing the ZIP */
    uint64_t decode_peak;     /* the most memory one image's read and decode held at once: the file with a
                                 ZIP entry's packed bytes while it is read, then the file with the joined
                                 image data and the rows, then the rows and the result */
    uint64_t reads;           /* image files read from the pack: once for all the levels that name one file,
                                 again only after its decoded image was evicted */
    uint64_t deferred;        /* lookups of an evicted image left unreplaced while its next decode waits
                                 (an image evicted and needed again is decoded less often each time) */
} NkTexReplStats;
void nk_texrepl_stats(const NkTexRepl *pack, NkTexReplStats *out);
void nk_texrepl_note_bad_factor(NkTexRepl *pack);

#ifdef __cplusplus
}
#endif

#endif

/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "player_ui_art.h"

#include <SDL3/SDL.h>

#include <stdlib.h>
#include <string.h>

/* Long enough for any path the library stores (NK_MAX_PATH). */
#define PLAYER_UI_ART_PATH_MAX 1024u

typedef enum {
    PLAYER_UI_ART_NOT_WANTED = 0,
    PLAYER_UI_ART_WANTED,
    PLAYER_UI_ART_READING,
    PLAYER_UI_ART_DONE        /* decoded, or settled as absent */
} PlayerUiArtState;

typedef struct {
    char iso_path[PLAYER_UI_ART_PATH_MAX];  /* "" : free slot */
    char cover_path[PLAYER_UI_ART_PATH_MAX];
    NkUiImage icon;
    NkUiImage picture;
    NkUiImage cover;
    PlayerUiArtState icon_state;
    PlayerUiArtState picture_state;
    PlayerUiArtState cover_state;
    uint64_t last_used;                     /* frame of the last get */
} PlayerUiArtEntry;

typedef struct {
    PlayerUiArtReader reader;
    char iso_path[PLAYER_UI_ART_PATH_MAX];
    char cover_path[PLAYER_UI_ART_PATH_MAX];
    unsigned wanted;
    uint64_t cover_generation;
    NkUiImage icon;
    NkUiImage picture;
    NkUiImage cover;
    SDL_AtomicInt done;
    SDL_Thread *thread;
} PlayerUiArtJob;

struct PlayerUiArt {
    PlayerUiArtReader reader;
    PlayerUiArtEntry entries[PLAYER_UI_ART_SLOTS];
    PlayerUiArtJob *job;
    size_t job_entry;
    uint64_t frame;
    uint64_t cover_generation;
};

static void player_ui_art_decode(const uint8_t *bytes, size_t size, size_t max_bytes,
                                 uint32_t max_side, NkUiImage *out) {
    NkUiImageLimits limits;
    limits.max_bytes = max_bytes;
    limits.max_width = max_side;
    limits.max_height = max_side;
    if (bytes != NULL && size > 0u) {
        (void)nk_ui_image_decode_png(bytes, size, &limits, out);
    } else {
        memset(out, 0, sizeof(*out));
    }
}

static void player_ui_art_decode_cover(const char *path, NkUiImage *out) {
    SDL_PathInfo info;
    size_t size = 0u;
    void *bytes;
    memset(out, 0, sizeof(*out));
    if (path == NULL || path[0] == '\0') return;
    if (!SDL_GetPathInfo(path, &info) || info.type != SDL_PATHTYPE_FILE ||
        info.size == 0u || info.size > (Uint64)PLAYER_UI_ART_COVER_MAX_BYTES) {
        return;
    }
    bytes = SDL_LoadFile(path, &size);
    if (bytes != NULL) {
        player_ui_art_decode((const uint8_t *)bytes, size,
                             PLAYER_UI_ART_COVER_MAX_BYTES,
                             PLAYER_UI_ART_COVER_MAX_SIDE, out);
        SDL_free(bytes);
    }
}

static int SDLCALL player_ui_art_worker(void *data) {
    PlayerUiArtJob *job = (PlayerUiArtJob *)data;
    if ((job->wanted & (PLAYER_UI_ART_ICON | PLAYER_UI_ART_PICTURE)) != 0u &&
        job->reader != NULL) {
        uint8_t *icon = NULL;
        uint8_t *picture = NULL;
        size_t icon_size = 0u;
        size_t picture_size = 0u;
        job->reader(job->iso_path, job->wanted & (PLAYER_UI_ART_ICON | PLAYER_UI_ART_PICTURE),
                    &icon, &icon_size, &picture, &picture_size);
        if ((job->wanted & PLAYER_UI_ART_ICON) != 0u) {
            player_ui_art_decode(icon, icon_size, PLAYER_UI_ART_MAX_BYTES,
                                 PLAYER_UI_ART_ICON_MAX_SIDE, &job->icon);
        }
        if ((job->wanted & PLAYER_UI_ART_PICTURE) != 0u) {
            player_ui_art_decode(picture, picture_size, PLAYER_UI_ART_MAX_BYTES,
                                 PLAYER_UI_ART_PICTURE_MAX_SIDE, &job->picture);
        }
        free(icon);
        free(picture);
    }
    if ((job->wanted & PLAYER_UI_ART_COVER) != 0u) {
        player_ui_art_decode_cover(job->cover_path, &job->cover);
    }
    SDL_SetAtomicInt(&job->done, 1);
    return 0;
}

PlayerUiArt *player_ui_art_create(PlayerUiArtReader reader) {
    PlayerUiArt *art;
    if (reader == NULL) {
        return NULL;
    }
    art = (PlayerUiArt *)calloc(1u, sizeof(*art));
    if (art != NULL) {
        art->reader = reader;
    }
    return art;
}

static void player_ui_art_entry_clear(PlayerUiArtEntry *entry) {
    nk_ui_image_free(&entry->icon);
    nk_ui_image_free(&entry->picture);
    nk_ui_image_free(&entry->cover);
    memset(entry, 0, sizeof(*entry));
}

/* Waits for the running job and hands its images to its entry. */
static void player_ui_art_finish(PlayerUiArt *art) {
    PlayerUiArtJob *job = art->job;
    PlayerUiArtEntry *entry;
    if (job == NULL) {
        return;
    }
    SDL_WaitThread(job->thread, NULL);
    entry = &art->entries[art->job_entry];
    if (strcmp(entry->iso_path, job->iso_path) == 0) {
        if ((job->wanted & PLAYER_UI_ART_ICON) != 0u) {
            entry->icon = job->icon;
            entry->icon_state = PLAYER_UI_ART_DONE;
            memset(&job->icon, 0, sizeof(job->icon));
        }
        if ((job->wanted & PLAYER_UI_ART_PICTURE) != 0u) {
            entry->picture = job->picture;
            entry->picture_state = PLAYER_UI_ART_DONE;
            memset(&job->picture, 0, sizeof(job->picture));
        }
        if ((job->wanted & PLAYER_UI_ART_COVER) != 0u &&
            job->cover_generation == art->cover_generation &&
            strcmp(entry->cover_path, job->cover_path) == 0 &&
            entry->cover_state == PLAYER_UI_ART_READING) {
            entry->cover = job->cover;
            entry->cover_state = PLAYER_UI_ART_DONE;
            memset(&job->cover, 0, sizeof(job->cover));
        }
    }
    nk_ui_image_free(&job->icon);
    nk_ui_image_free(&job->picture);
    nk_ui_image_free(&job->cover);
    free(job);
    art->job = NULL;
}

void player_ui_art_destroy(PlayerUiArt *art) {
    size_t i;
    if (art == NULL) {
        return;
    }
    player_ui_art_finish(art);
    for (i = 0; i < PLAYER_UI_ART_SLOTS; ++i) {
        player_ui_art_entry_clear(&art->entries[i]);
    }
    free(art);
}

/* Starts a read for the most recently shown entry that wants an image. */
static void player_ui_art_start(PlayerUiArt *art) {
    PlayerUiArtEntry *best = NULL;
    PlayerUiArtJob *job;
    size_t best_index = 0u;
    size_t i;
    for (i = 0; i < PLAYER_UI_ART_SLOTS; ++i) {
        PlayerUiArtEntry *entry = &art->entries[i];
        if (entry->iso_path[0] == '\0' ||
            (entry->icon_state != PLAYER_UI_ART_WANTED &&
             entry->picture_state != PLAYER_UI_ART_WANTED &&
             entry->cover_state != PLAYER_UI_ART_WANTED)) {
            continue;
        }
        if (best == NULL || entry->last_used > best->last_used) {
            best = entry;
            best_index = i;
        }
    }
    if (best == NULL) {
        return;
    }
    job = (PlayerUiArtJob *)calloc(1u, sizeof(*job));
    if (job == NULL) {
        return; /* try again next frame */
    }
    job->reader = art->reader;
    job->cover_generation = art->cover_generation;
    memcpy(job->iso_path, best->iso_path, sizeof(job->iso_path));
    memcpy(job->cover_path, best->cover_path, sizeof(job->cover_path));
    if (best->icon_state == PLAYER_UI_ART_WANTED) job->wanted |= PLAYER_UI_ART_ICON;
    if (best->picture_state == PLAYER_UI_ART_WANTED) job->wanted |= PLAYER_UI_ART_PICTURE;
    if (best->cover_state == PLAYER_UI_ART_WANTED) job->wanted |= PLAYER_UI_ART_COVER;
    job->thread = SDL_CreateThread(player_ui_art_worker, "nakagawa-disc-art", job);
    if (job->thread == NULL) {
        /* No thread: settle this disc as having no art rather than retrying
           every frame. */
        if ((job->wanted & PLAYER_UI_ART_ICON) != 0u) best->icon_state = PLAYER_UI_ART_DONE;
        if ((job->wanted & PLAYER_UI_ART_PICTURE) != 0u) best->picture_state = PLAYER_UI_ART_DONE;
        if ((job->wanted & PLAYER_UI_ART_COVER) != 0u) best->cover_state = PLAYER_UI_ART_DONE;
        free(job);
        return;
    }
    if ((job->wanted & PLAYER_UI_ART_ICON) != 0u) best->icon_state = PLAYER_UI_ART_READING;
    if ((job->wanted & PLAYER_UI_ART_PICTURE) != 0u) best->picture_state = PLAYER_UI_ART_READING;
    if ((job->wanted & PLAYER_UI_ART_COVER) != 0u) best->cover_state = PLAYER_UI_ART_READING;
    art->job = job;
    art->job_entry = best_index;
}

void player_ui_art_pump(PlayerUiArt *art) {
    if (art == NULL) {
        return;
    }
    ++art->frame;
    if (art->job != NULL && SDL_GetAtomicInt(&art->job->done) != 0) {
        player_ui_art_finish(art);
    }
    if (art->job == NULL) {
        player_ui_art_start(art);
    }
}

/* The entry for a path, claiming a free slot or the least recently shown one
   that is neither shown this frame nor being read. NULL when none can go. */
static PlayerUiArtEntry *player_ui_art_entry(PlayerUiArt *art, const char *iso_path) {
    PlayerUiArtEntry *victim = NULL;
    size_t i;
    for (i = 0; i < PLAYER_UI_ART_SLOTS; ++i) {
        if (strcmp(art->entries[i].iso_path, iso_path) == 0) {
            return &art->entries[i];
        }
    }
    for (i = 0; i < PLAYER_UI_ART_SLOTS; ++i) {
        PlayerUiArtEntry *entry = &art->entries[i];
        if (entry->iso_path[0] == '\0') {
            victim = entry;
            break;
        }
        if (entry->last_used == art->frame || (art->job != NULL && i == art->job_entry)) {
            continue;
        }
        if (victim == NULL || entry->last_used < victim->last_used) {
            victim = entry;
        }
    }
    if (victim != NULL) {
        player_ui_art_entry_clear(victim);
        memcpy(victim->iso_path, iso_path, strlen(iso_path) + 1u);
    }
    return victim;
}

void player_ui_art_get_title(PlayerUiArt *art, const char *iso_path, const char *cover_path,
                             bool want_picture, const NkUiImage **icon,
                             const NkUiImage **picture, const NkUiImage **cover) {
    PlayerUiArtEntry *entry;
    bool has_cover;
    if (icon != NULL) *icon = NULL;
    if (picture != NULL) *picture = NULL;
    if (cover != NULL) *cover = NULL;
    if (art == NULL || iso_path == NULL || iso_path[0] == '\0' ||
        strlen(iso_path) >= PLAYER_UI_ART_PATH_MAX) {
        return;
    }
    entry = player_ui_art_entry(art, iso_path);
    if (entry == NULL) {
        return;
    }
    entry->last_used = art->frame;
    if (entry->icon_state == PLAYER_UI_ART_NOT_WANTED) {
        entry->icon_state = PLAYER_UI_ART_WANTED;
    }
    if (want_picture && entry->picture_state == PLAYER_UI_ART_NOT_WANTED) {
        entry->picture_state = PLAYER_UI_ART_WANTED;
    }
    has_cover = cover_path != NULL && cover_path[0] != '\0' &&
                strlen(cover_path) < PLAYER_UI_ART_PATH_MAX;
    if (has_cover) {
        if (strcmp(entry->cover_path, cover_path) != 0) {
            memcpy(entry->cover_path, cover_path, strlen(cover_path) + 1u);
            nk_ui_image_free(&entry->cover);
            entry->cover_state = PLAYER_UI_ART_WANTED;
        } else if (entry->cover_state == PLAYER_UI_ART_NOT_WANTED) {
            entry->cover_state = PLAYER_UI_ART_WANTED;
        }
    } else if (entry->cover_path[0] != '\0' || entry->cover.rgba_pm != NULL ||
               entry->cover_state != PLAYER_UI_ART_NOT_WANTED) {
        entry->cover_path[0] = '\0';
        nk_ui_image_free(&entry->cover);
        entry->cover_state = PLAYER_UI_ART_NOT_WANTED;
    }
    if (icon != NULL && entry->icon.rgba_pm != NULL) *icon = &entry->icon;
    if (picture != NULL && entry->picture.rgba_pm != NULL) *picture = &entry->picture;
    if (cover != NULL && has_cover && entry->cover.rgba_pm != NULL) *cover = &entry->cover;
}

void player_ui_art_get(PlayerUiArt *art, const char *iso_path, bool want_picture,
                       const NkUiImage **icon, const NkUiImage **picture) {
    player_ui_art_get_title(art, iso_path, NULL, want_picture, icon, picture, NULL);
}

void player_ui_art_invalidate_covers(PlayerUiArt *art) {
    size_t i;
    if (art == NULL) return;
    ++art->cover_generation;
    for (i = 0; i < PLAYER_UI_ART_SLOTS; ++i) {
        PlayerUiArtEntry *entry = &art->entries[i];
        entry->cover_path[0] = '\0';
        nk_ui_image_free(&entry->cover);
        entry->cover_state = PLAYER_UI_ART_NOT_WANTED;
    }
}

bool player_ui_art_reading_cover(const PlayerUiArt *art) {
    return art != NULL && art->job != NULL &&
           (art->job->wanted & PLAYER_UI_ART_COVER) != 0u &&
           SDL_GetAtomicInt(&art->job->done) == 0;
}

uint32_t player_ui_art_wait_ms(const PlayerUiArt *art, uint32_t wait_ms) {
    return player_ui_art_busy(art) && wait_ms > PLAYER_UI_ART_POLL_MS ? PLAYER_UI_ART_POLL_MS
                                                                      : wait_ms;
}

bool player_ui_art_busy(const PlayerUiArt *art) {
    size_t i;
    if (art == NULL) {
        return false;
    }
    if (art->job != NULL) {
        return true;
    }
    for (i = 0; i < PLAYER_UI_ART_SLOTS; ++i) {
        if (art->entries[i].icon_state == PLAYER_UI_ART_WANTED ||
            art->entries[i].picture_state == PLAYER_UI_ART_WANTED ||
            art->entries[i].cover_state == PLAYER_UI_ART_WANTED) {
            return true;
        }
    }
    return false;
}

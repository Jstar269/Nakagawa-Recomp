/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The RetroAchievements bridge (#1316): see nk_achievements.h. */

#include "nk_achievements.h"

#include "nk_iso.h"
#include "nk_platform.h"
#include "rc_client.h"
#include "rc_consoles.h"
#include "rc_api_request.h"
#include "rc_api_runtime.h"
#include "rhash/md5.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The transport's sentinels pass straight through as rc_client's status codes. */
typedef char nk_ach_client_error_matches_rcheevos
    [(NK_ACH_HTTP_CLIENT_ERROR == RC_API_SERVER_RESPONSE_CLIENT_ERROR) ? 1 : -1];
typedef char nk_ach_retry_matches_rcheevos
    [(NK_ACH_HTTP_RETRY == RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR) ? 1 : -1];

typedef struct NkAchPendingEntry {
    NkAchPendingUnlock unlock;
    bool restored;
} NkAchPendingEntry;

typedef struct AchPendingReplay {
    NkAch *ach;
    uint32_t achievement_id;
    char game_hash[33];
    rc_api_request_t request;
    NkAchRequest transport_request;
} AchPendingReplay;

static bool ach_hash_ok(const char *hash);

struct NkAch {
    rc_client_t *client;
    NkAchMemory mem;
    char game_hash[33];
    NkAchTransport transport;
    void *transport_ctx;
    NkAchEvent events[NK_ACH_EVENT_QUEUE];
    size_t event_head; /* oldest */
    size_t event_count;
    uint32_t events_dropped;
    NkAchPendingEntry pending_unlocks[NK_ACH_PENDING_UNLOCKS_MAX];
    size_t pending_unlock_count;
    bool pending_replay_inflight;
    uint64_t pending_replay_now_ms;
    uint64_t pending_replay_after_ms;
    bool pending_replay_blocked; /* the server refused the account: wait for the next sign-in */
    bool confirmed_unlock_unseen;
    uint32_t confirmed_unlock_id;
    char confirmed_unlock_hash[33];
    NkAchLock lock;
    /* The account every pending record belongs to; meaningful while any record waits. */
    char pending_owner[NK_ACH_ACCOUNT_TAG_SIZE];
    bool queue_save_blocked;   /* a queue file neither read nor set aside is still on disk */
    bool response_unreachable; /* the response being handled is no answer from the server */
    bool token_retry_armed;
    uint64_t token_retry_wait_ms;
    uint64_t token_retry_after_ms; /* 0: the next nk_ach_token_login_due starts the wait */
};

static void ach_enter(const NkAch *ach) {
    if (ach && ach->lock.enter) ach->lock.enter(ach->lock.context);
}

static void ach_leave(const NkAch *ach) {
    if (ach && ach->lock.leave) ach->lock.leave(ach->lock.context);
}

static const char k_account_tag_label[] = "Nakagawa Recomp unlock queue owner\n";

bool nk_ach_account_tag(const char *username, char out[NK_ACH_ACCOUNT_TAG_SIZE]) {
    static const char hex[] = "0123456789abcdef";
    if (out) out[0] = '\0';
    if (!username || !username[0] || !out) return false;
    md5_state_t md5;
    md5_byte_t digest[16];
    md5_init(&md5);
    md5_append(&md5, (const md5_byte_t *)k_account_tag_label,
               (int)(sizeof k_account_tag_label - 1u));
    for (const char *p = username; *p; p++) {
        /* RetroAchievements account names do not differ by case. */
        md5_byte_t c = (md5_byte_t)*p;
        if (c >= 'A' && c <= 'Z') c = (md5_byte_t)(c - 'A' + 'a');
        md5_append(&md5, &c, 1);
    }
    md5_finish(&md5, digest);
    for (size_t i = 0u; i < sizeof digest; i++) {
        out[i * 2u] = hex[digest[i] >> 4];
        out[i * 2u + 1u] = hex[digest[i] & 15u];
    }
    out[32] = '\0';
    return true;
}

static bool ach_user_tag(const NkAch *ach, char out[NK_ACH_ACCOUNT_TAG_SIZE]) {
    const rc_client_user_t *user = rc_client_get_user_info(ach->client);
    return user && user->username && nk_ach_account_tag(user->username, out);
}

bool nk_ach_url_allowed(const char *url) {
    static const char prefix[] = NK_ACH_URL_PREFIX;
    /* The prefix ends in the '/' after the host, so "retroachievements.org.example/",
     * "retroachievements.org:8080/" and "user@..." forms cannot match. */
    return url != NULL && strncmp(url, prefix, sizeof prefix - 1u) == 0;
}

bool nk_ach_psp_hash_iso(const char *iso_path, char out_hash[33]) {
    static const char *const paths[] = {
        "PSP_GAME/PARAM.SFO",
        "PSP_GAME/SYSDIR/EBOOT.BIN",
    };
    static const char hex[] = "0123456789abcdef";
    enum { CHUNK_BYTES = 64 * 1024 };
    if (out_hash) out_hash[0] = '\0';
    if (!iso_path || !iso_path[0] || !out_hash) return false;

    NkIsoReader *reader = nk_iso_reader_open(iso_path);
    if (!reader) return false;
    md5_state_t md5;
    md5_byte_t digest[16];
    uint8_t buffer[CHUNK_BYTES];
    bool ok = true;
    md5_init(&md5);
    for (size_t file_index = 0u; file_index < sizeof paths / sizeof paths[0] && ok;
         file_index++) {
        uint32_t lba = 0u;
        uint32_t size = 0u;
        bool is_directory = false;
        if (nk_iso_reader_lookup(reader, paths[file_index], &lba, &size, &is_directory) != 0 ||
            is_directory || size == 0u) {
            ok = false;
            break;
        }
        uint64_t offset = 0u;
        while (offset < (uint64_t)size) {
            const uint64_t remaining = (uint64_t)size - offset;
            const size_t count = remaining < sizeof buffer ? (size_t)remaining : sizeof buffer;
            const int received = nk_iso_reader_read(reader, lba, offset, buffer, (uint32_t)count);
            if (received < 0 || (size_t)received != count) {
                ok = false;
                break;
            }
            md5_append(&md5, buffer, (int)count);
            offset += (uint64_t)count;
        }
    }
    nk_iso_reader_close(reader);
    if (!ok) return false;

    md5_finish(&md5, digest);
    for (size_t i = 0u; i < sizeof digest; i++) {
        out_hash[i * 2u] = hex[digest[i] >> 4];
        out_hash[i * 2u + 1u] = hex[digest[i] & 15u];
    }
    out_hash[32] = '\0';
    return true;
}

uint32_t nk_ach_read_memory(const NkAchMemory *mem, uint32_t ra_address, uint8_t *out,
                            uint32_t n) {
    if (!mem || !mem->base || !out || n == 0u) return 0u;
    const rc_memory_regions_t *regions = rc_console_memory_regions(NK_ACH_CONSOLE_PSP);
    if (!regions) return 0u;
    for (uint32_t i = 0; i < regions->num_regions; i++) {
        const rc_memory_region_t *r = &regions->region[i];
        if (ra_address < r->start_address || ra_address > r->end_address) continue;
        /* 64-bit arithmetic: no guest address or span end can wrap. */
        const uint64_t guest = (uint64_t)r->real_address + (ra_address - r->start_address);
        const uint64_t span_end = (uint64_t)mem->guest_start + mem->size;
        if (guest < mem->guest_start || guest >= span_end) return 0u;
        /* The run of regions that follow on in both RA and guest addresses (all three, for the
         * PSP) is one piece of memory: a read stops only at its end and at the end of the span. */
        uint32_t run_end = r->end_address;
        for (uint32_t k = i + 1u; k < regions->num_regions; k++) {
            const rc_memory_region_t *next = &regions->region[k];
            const rc_memory_region_t *prev = &regions->region[k - 1u];
            if ((uint64_t)next->start_address != (uint64_t)prev->end_address + 1u ||
                (uint64_t)next->real_address !=
                    (uint64_t)prev->real_address + (prev->end_address - prev->start_address) + 1u)
                break;
            run_end = next->end_address;
        }
        uint64_t avail = span_end - guest;
        const uint64_t run_left = (uint64_t)run_end - ra_address + 1u;
        if (run_left < avail) avail = run_left;
        const uint32_t count = (uint32_t)(avail < n ? avail : n);
        memcpy(out, mem->base + (size_t)(guest - mem->guest_start), count);
        return count;
    }
    return 0u;
}

static void ach_push(NkAch *ach, NkAchEventKind kind, uint32_t id, uint32_t points,
                     uint32_t count, const char *title, const char *message) {
    if (ach->event_count == NK_ACH_EVENT_QUEUE) {
        ach->event_head = (ach->event_head + 1u) % NK_ACH_EVENT_QUEUE;
        ach->event_count--;
        ach->events_dropped++;
    }
    NkAchEvent *e = &ach->events[(ach->event_head + ach->event_count) % NK_ACH_EVENT_QUEUE];
    memset(e, 0, sizeof *e);
    e->kind = kind;
    e->id = id;
    e->points = points;
    e->count = count;
    if (title) snprintf(e->title, sizeof e->title, "%s", title);
    if (message) snprintf(e->message, sizeof e->message, "%s", message);
    ach->event_count++;
}

static size_t ach_pending_find(const NkAch *ach, const char *game_hash, uint32_t id) {
    if (!ach || !game_hash || id == 0u) return SIZE_MAX;
    for (size_t i = 0u; i < ach->pending_unlock_count; i++) {
        const NkAchPendingUnlock *entry = &ach->pending_unlocks[i].unlock;
        if (entry->achievement_id == id && strcmp(entry->game_hash, game_hash) == 0) return i;
    }
    return SIZE_MAX;
}

/* The queue belongs to one account. With an account signed in, records another account left
 * are discarded and reported: they are never awarded to this one, and never mixed with its own. */
static bool ach_queue_claim(NkAch *ach) {
    char tag[NK_ACH_ACCOUNT_TAG_SIZE];
    if (!ach_user_tag(ach, tag)) return false;
    if (ach->pending_unlock_count != 0u && strcmp(ach->pending_owner, tag) != 0) {
        const size_t discarded = ach->pending_unlock_count;
        memset(ach->pending_unlocks, 0, sizeof ach->pending_unlocks);
        ach->pending_unlock_count = 0u;
        ach->pending_replay_after_ms = 0u;
        ach->confirmed_unlock_unseen = false;
        ach_push(ach, NK_ACH_EVENT_UNLOCKS_DISCARDED, 0u, 0u, (uint32_t)discarded, NULL,
                 "Saved unlocks belonged to another account and were discarded.");
        ach_push(ach, NK_ACH_EVENT_QUEUE_CHANGED, 0u, 0u, 0u, NULL, NULL);
    }
    memcpy(ach->pending_owner, tag, sizeof tag);
    return true;
}

static bool ach_pending_add(NkAch *ach, const char *game_hash, uint32_t id) {
    if (!ach || !ach_hash_ok(game_hash) || id == 0u) return false;
    /* A record is made for the signed-in account, in a queue that is its own. */
    if (!ach_queue_claim(ach)) return false;
    if (ach_pending_find(ach, game_hash, id) != SIZE_MAX) return true;
    if (ach->pending_unlock_count == NK_ACH_PENDING_UNLOCKS_MAX) {
        ach_push(ach, NK_ACH_EVENT_QUEUE_FULL, id, 0u, 0u, NULL,
                 "The offline unlock queue is full; this unlock could not be queued.");
        return false;
    }
    NkAchPendingEntry *entry = &ach->pending_unlocks[ach->pending_unlock_count++];
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->unlock.game_hash, sizeof entry->unlock.game_hash, "%s", game_hash);
    entry->unlock.achievement_id = id;
    time_t now = time(NULL);
    entry->unlock.unlocked_at_utc = now > 0 ? (uint64_t)now : 0u;
    return true;
}

static bool ach_pending_remove(NkAch *ach, const char *game_hash, uint32_t id,
                               bool *was_restored) {
    const size_t index = ach_pending_find(ach, game_hash, id);
    if (index == SIZE_MAX) return false;
    if (was_restored) *was_restored = ach->pending_unlocks[index].restored;
    memmove(&ach->pending_unlocks[index], &ach->pending_unlocks[index + 1u],
            (ach->pending_unlock_count - index - 1u) * sizeof(ach->pending_unlocks[0]));
    ach->pending_unlock_count--;
    memset(&ach->pending_unlocks[ach->pending_unlock_count], 0,
           sizeof ach->pending_unlocks[0]);
    return true;
}

static uint32_t ach_request_award_id(const char *post_data) {
    if (!post_data || strncmp(post_data, "r=awardachievement&", 19u) != 0) return 0u;
    const char *field = strstr(post_data, "&a=");
    if (!field) return 0u;
    field += 3;
    if (*field < '0' || *field > '9') return 0u;
    char *end = NULL;
    unsigned long value = strtoul(field, &end, 10);
    if (end == field || (*end != '\0' && *end != '&') || value == 0u || value > UINT32_MAX)
        return 0u;
    return (uint32_t)value;
}

static bool ach_award_response_success(const NkAchResponse *response, uint32_t id) {
    if (!response || response->http_status < 200 || response->http_status >= 300 ||
        !response->body) return false;
    rc_api_server_response_t server;
    memset(&server, 0, sizeof server);
    server.body = response->body;
    server.body_length = response->body_length;
    server.http_status_code = response->http_status;
    rc_api_award_achievement_response_t result;
    int rc = rc_api_process_award_achievement_server_response(&result, &server);
    bool succeeded = rc == RC_OK && result.response.succeeded &&
        (result.awarded_achievement_id == id || result.response.error_message != NULL);
    rc_api_destroy_award_achievement_response(&result);
    return succeeded;
}

static uint32_t RC_CCONV ach_read(uint32_t address, uint8_t *buffer, uint32_t num_bytes,
                                  rc_client_t *client) {
    const NkAch *ach = (const NkAch *)rc_client_get_userdata(client);
    return ach ? nk_ach_read_memory(&ach->mem, address, buffer, num_bytes) : 0u;
}

/* One request in flight: rc_client's callback, carried through the caller's transport. */
typedef struct AchCall {
    NkAch *ach;
    rc_client_server_callback_t callback;
    void *callback_data;
    uint32_t award_id;
    char game_hash[33];
} AchCall;

/* rcheevos 12.5.0 marks the set complete when an award answer says no achievement remains, and
 * tells of it from the next rc_client_do_frame. Until then that state is not safe: unloading
 * the game (the feature turned off, a sign-out, another game, the player closing) raises it
 * with no game and reads through a null pointer (rc_client.c, rc_client_unload_game then
 * rc_client_raise_mastery_event), and an answer that arrives once the game is unloaded does the
 * same in rc_client_award_achievement_callback. Both happen when the last achievement of a set
 * is earned just before the game ends. This bridge shows no "set complete" notice, so rc_client
 * is given the same answer saying one remains and never enters that state. True when out holds
 * that answer. */
static bool ach_award_answer_without_completion(const NkAchResponse *response, char *out,
                                                size_t out_size) {
    if (!response || !response->body || response->http_status < 200 ||
        response->http_status >= 300) return false;
    rc_api_server_response_t server;
    memset(&server, 0, sizeof server);
    server.body = response->body;
    server.body_length = response->body_length;
    server.http_status_code = response->http_status;
    rc_api_award_achievement_response_t parsed;
    const int result = rc_api_process_award_achievement_server_response(&parsed, &server);
    const bool completes = result == RC_OK && parsed.response.succeeded &&
                           parsed.achievements_remaining == 0u;
    int length = 0;
    if (completes)
        length = snprintf(out, out_size,
                          "{\"Success\":true,\"Score\":%u,\"SoftcoreScore\":%u,"
                          "\"AchievementID\":%u,\"AchievementsRemaining\":1}",
                          parsed.new_player_score, parsed.new_player_score_softcore,
                          parsed.awarded_achievement_id);
    rc_api_destroy_award_achievement_response(&parsed);
    return completes && length > 0 && (size_t)length < out_size;
}

/* No answer from the server itself: no connection, a busy or failing server. */
static bool ach_response_unreachable(const NkAchResponse *response) {
    if (!response) return false;
    const int status = response->http_status;
    return status == NK_ACH_HTTP_RETRY || status == 0 || status >= 500 || status == 408 ||
           status == 429;
}

static void ach_done(const NkAchResponse *response, void *done_ctx) {
    AchCall *call = (AchCall *)done_ctx;
    NkAch *ach = call->ach;
    ach_enter(ach);
    rc_api_server_response_t server;
    memset(&server, 0, sizeof server);
    if (response) {
        server.body = response->body;
        server.body_length = response->body ? response->body_length : 0u;
        server.http_status_code = response->http_status;
    } else {
        server.http_status_code = RC_API_SERVER_RESPONSE_CLIENT_ERROR;
    }
    char without_completion[192];
    if (call->award_id &&
        ach_award_answer_without_completion(response, without_completion,
                                            sizeof without_completion)) {
        server.body = without_completion;
        server.body_length = strlen(without_completion);
    }
    const bool outer_unreachable = ach->response_unreachable;
    ach->response_unreachable = ach_response_unreachable(response);
    call->callback(&server, call->callback_data);
    ach->response_unreachable = outer_unreachable;
    if (call->award_id && ach_award_response_success(response, call->award_id)) {
        bool restored = false;
        if (!ach_pending_remove(call->ach, call->game_hash, call->award_id, &restored)) {
            /* A transport may finish synchronously before rc_client raises the trigger event. */
            call->ach->confirmed_unlock_unseen = true;
            call->ach->confirmed_unlock_id = call->award_id;
            snprintf(call->ach->confirmed_unlock_hash,
                     sizeof call->ach->confirmed_unlock_hash, "%s", call->game_hash);
        } else {
            ach_push(call->ach, NK_ACH_EVENT_QUEUE_CHANGED, call->award_id, 0u, 0u, NULL, NULL);
            if (restored && call->ach->pending_unlock_count == 0u)
                ach_push(call->ach, NK_ACH_EVENT_RECONNECTED, 0u, 0u, 0u, NULL, NULL);
        }
    }
    free(call);
    ach_leave(ach);
}

static void ach_refuse(rc_client_server_callback_t callback, void *callback_data,
                       const char *why) {
    rc_api_server_response_t server;
    memset(&server, 0, sizeof server);
    server.body = why;
    server.body_length = strlen(why);
    server.http_status_code = RC_API_SERVER_RESPONSE_CLIENT_ERROR;
    callback(&server, callback_data);
}

static void RC_CCONV ach_server_call(const rc_api_request_t *request,
                                     rc_client_server_callback_t callback, void *callback_data,
                                     rc_client_t *client) {
    NkAch *ach = (NkAch *)rc_client_get_userdata(client);
    if (!ach || !request || !nk_ach_url_allowed(request->url)) {
        ach_refuse(callback, callback_data, "refused: not a RetroAchievements server");
        return;
    }
    if (!ach->transport) {
        ach_refuse(callback, callback_data, "refused: online achievements are off");
        return;
    }
    AchCall *call = (AchCall *)malloc(sizeof *call);
    if (!call) {
        ach_refuse(callback, callback_data, "refused: out of memory");
        return;
    }
    call->callback = callback;
    call->callback_data = callback_data;
    call->ach = ach;
    call->award_id = ach_request_award_id(request->post_data);
    snprintf(call->game_hash, sizeof call->game_hash, "%s", ach->game_hash);
    NkAchRequest out = {request->url, request->post_data, request->content_type};
    ach->transport(ach->transport_ctx, &out, ach_done, call);
}

static void RC_CCONV ach_event(const rc_client_event_t *event, rc_client_t *client) {
    NkAch *ach = (NkAch *)rc_client_get_userdata(client);
    if (!ach || !event) return;
    switch (event->type) {
    case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
        if (event->achievement) {
            if (ach->confirmed_unlock_unseen &&
                ach->confirmed_unlock_id == event->achievement->id &&
                strcmp(ach->confirmed_unlock_hash, ach->game_hash) == 0) {
                ach->confirmed_unlock_unseen = false;
            } else {
                (void)ach_pending_add(ach, ach->game_hash, event->achievement->id);
            }
            ach_push(ach, NK_ACH_EVENT_UNLOCKED, event->achievement->id,
                     event->achievement->points, 0u, event->achievement->title, NULL);
        }
        break;
    case RC_CLIENT_EVENT_SERVER_ERROR:
        ach_push(ach, NK_ACH_EVENT_SERVER_ERROR,
                 event->server_error ? event->server_error->related_id : 0u, 0u, 0u, NULL,
                 event->server_error ? event->server_error->error_message : NULL);
        break;
    case RC_CLIENT_EVENT_DISCONNECTED:
        ach_push(ach, NK_ACH_EVENT_DISCONNECTED, 0u, 0u, 0u, NULL, NULL);
        break;
    case RC_CLIENT_EVENT_RECONNECTED:
        ach_push(ach, NK_ACH_EVENT_RECONNECTED, 0u, 0u, 0u, NULL, NULL);
        break;
    default:
        break;
    }
}

/* rc_client raises no event when a short read turns an achievement off; it only logs
 * "Disabled achievement <id>. Invalid address <addr>" (rc_client.c,
 * rc_client_invalidate_memref_achievements). That warning becomes a DISABLED event, so the user is
 * told. achievements-selftest checks the text, so an rcheevos update that changes it fails there. */
static const char k_disabled_prefix[] = "Disabled achievement ";

static void RC_CCONV ach_log(const char *message, const rc_client_t *client) {
    NkAch *ach = (NkAch *)rc_client_get_userdata(client);
    if (!ach || !message || strncmp(message, k_disabled_prefix, sizeof k_disabled_prefix - 1u))
        return;
    const char *p = message + sizeof k_disabled_prefix - 1u;
    uint32_t id = 0u;
    while (*p >= '0' && *p <= '9' && id <= 0x0fffffffu) id = id * 10u + (uint32_t)(*p++ - '0');
    ach_push(ach, NK_ACH_EVENT_DISABLED, id, 0u, 0u, NULL, message);
}

NkAch *nk_ach_create(const NkAchMemory *mem, NkAchTransport transport, void *transport_ctx) {
    NkAch *ach = (NkAch *)calloc(1u, sizeof *ach);
    if (!ach) return NULL;
    if (mem) ach->mem = *mem;
    ach->transport = transport;
    ach->transport_ctx = transport_ctx;
    ach->client = rc_client_create(ach_read, ach_server_call);
    if (!ach->client) {
        free(ach);
        return NULL;
    }
    rc_client_set_userdata(ach->client, ach);
    rc_client_set_event_handler(ach->client, ach_event);
    rc_client_enable_logging(ach->client, RC_CLIENT_LOG_LEVEL_WARN, ach_log);
    rc_client_set_hardcore_enabled(ach->client, 0);
    rc_client_set_encore_mode_enabled(ach->client, 0);
    rc_client_set_unofficial_enabled(ach->client, 0);
    return ach;
}

void nk_ach_set_lock(NkAch *ach, const NkAchLock *lock) {
    if (!ach) return;
    if (lock) ach->lock = *lock;
    else memset(&ach->lock, 0, sizeof ach->lock);
}

void nk_ach_destroy(NkAch *ach) {
    if (!ach) return;
    const NkAchLock lock = ach->lock;
    if (lock.enter) lock.enter(lock.context);
    rc_client_destroy(ach->client);
    free(ach);
    if (lock.leave) lock.leave(lock.context);
}

void nk_ach_set_memory(NkAch *ach, const NkAchMemory *mem) {
    if (!ach) return;
    ach_enter(ach);
    if (mem) ach->mem = *mem;
    else memset(&ach->mem, 0, sizeof ach->mem);
    ach_leave(ach);
}

#define ACH_TOKEN_RETRY_FIRST_MS 30000u
#define ACH_TOKEN_RETRY_MAX_MS 600000u
/* rc_client hands this back to ach_login_done for a sign-in made with the stored token. */
static const char k_token_login = 't';

static void ach_token_retry_end(NkAch *ach) {
    ach->token_retry_armed = false;
    ach->token_retry_wait_ms = 0u;
    ach->token_retry_after_ms = 0u;
}

static void RC_CCONV ach_login_done(int result, const char *error_message,
                                    rc_client_t *client, void *userdata) {
    NkAch *ach = (NkAch *)rc_client_get_userdata(client);
    if (!ach) return;
    if (result == RC_OK) {
        const rc_client_user_t *user = rc_client_get_user_info(client);
        ach->pending_replay_blocked = false;
        ach->pending_replay_after_ms = 0u;
        ach_token_retry_end(ach);
        ach_push(ach, NK_ACH_EVENT_LOGIN_OK, 0u, user ? user->score_softcore : 0u, 0u,
                 user ? user->display_name : NULL, NULL);
        /* Records another account left end here, before anything can send them. */
        (void)ach_queue_claim(ach);
    } else if (userdata == &k_token_login && ach->response_unreachable) {
        /* The server gave no answer, so the stored token was not refused: it is tried again
         * (nk_ach_token_login_due), each wait twice the last. */
        const uint64_t wait = ach->token_retry_wait_ms == 0u
                                  ? ACH_TOKEN_RETRY_FIRST_MS : ach->token_retry_wait_ms * 2u;
        ach->token_retry_wait_ms = wait < ACH_TOKEN_RETRY_MAX_MS ? wait : ACH_TOKEN_RETRY_MAX_MS;
        ach->token_retry_after_ms = 0u;
        ach->token_retry_armed = true;
        ach_push(ach, NK_ACH_EVENT_LOGIN_UNREACHABLE, 0u, 0u, 0u, NULL,
                 error_message ? error_message : rc_error_str(result));
    } else {
        ach_token_retry_end(ach);
        ach_push(ach, NK_ACH_EVENT_LOGIN_FAILED, 0u, 0u, 0u, NULL,
                 error_message ? error_message : rc_error_str(result));
    }
}

bool nk_ach_begin_login(NkAch *ach, const char *username, const char *token) {
    if (!ach || !username || !*username || !token || !*token) return false;
    ach_enter(ach);
    /* rc_client returns no handle for a call it finished at once; either way the outcome
     * arrives as an event. */
    (void)rc_client_begin_login_with_token(ach->client, username, token, ach_login_done,
                                           (void *)&k_token_login);
    ach_leave(ach);
    return true;
}

bool nk_ach_token_login_due(NkAch *ach, uint64_t now_ms) {
    if (!ach) return false;
    bool due = false;
    ach_enter(ach);
    if (ach->token_retry_armed) {
        if (ach->token_retry_after_ms == 0u) {
            ach->token_retry_after_ms = now_ms <= UINT64_MAX - ach->token_retry_wait_ms
                                            ? now_ms + ach->token_retry_wait_ms : UINT64_MAX;
        } else if (now_ms >= ach->token_retry_after_ms) {
            /* The wait length stays: the next unreachable answer doubles it. */
            ach->token_retry_armed = false;
            ach->token_retry_after_ms = 0u;
            due = true;
        }
    }
    ach_leave(ach);
    return due;
}

bool nk_ach_begin_login_with_password(NkAch *ach, const char *username, const char *password) {
    if (!ach || !username || !*username || !password || !*password) return false;
    ach_enter(ach);
    ach_token_retry_end(ach);
    (void)rc_client_begin_login_with_password(ach->client, username, password,
                                              ach_login_done, NULL);
    ach_leave(ach);
    return true;
}

bool nk_ach_copy_user_token(const NkAch *ach, char *out, size_t out_size) {
    if (!out || out_size == 0u) return false;
    out[0] = '\0';
    if (!ach) return false;
    bool copied = false;
    ach_enter(ach);
    const rc_client_user_t *user = rc_client_get_user_info(ach->client);
    if (user && user->token) {
        const size_t length = strlen(user->token);
        if (length != 0u && length < out_size) {
            memcpy(out, user->token, length + 1u);
            copied = true;
        }
    }
    ach_leave(ach);
    return copied;
}

bool nk_ach_copy_user_name(const NkAch *ach, char *out, size_t out_size) {
    if (!out || out_size == 0u) return false;
    out[0] = '\0';
    if (!ach) return false;
    bool copied = false;
    ach_enter(ach);
    const rc_client_user_t *user = rc_client_get_user_info(ach->client);
    if (user && user->username) {
        const size_t length = strlen(user->username);
        if (length != 0u && length < out_size) {
            memcpy(out, user->username, length + 1u);
            copied = true;
        }
    }
    ach_leave(ach);
    return copied;
}

void nk_ach_logout(NkAch *ach) {
    if (!ach) return;
    ach_enter(ach);
    ach_token_retry_end(ach);
    rc_client_logout(ach->client);
    ach_leave(ach);
}

size_t nk_ach_sign_out(NkAch *ach) {
    if (!ach) return 0u;
    ach_enter(ach);
    ach_token_retry_end(ach);
    rc_client_logout(ach->client);
    /* The queue is the signed-out account's: nothing of that account stays behind, and the
     * next account to sign in starts with an empty queue of its own. */
    const size_t discarded = ach->pending_unlock_count;
    memset(ach->pending_unlocks, 0, sizeof ach->pending_unlocks);
    memset(ach->pending_owner, 0, sizeof ach->pending_owner);
    ach->pending_unlock_count = 0u;
    ach->pending_replay_after_ms = 0u;
    ach->pending_replay_blocked = false;
    ach->confirmed_unlock_unseen = false;
    if (discarded) ach_push(ach, NK_ACH_EVENT_QUEUE_CHANGED, 0u, 0u, 0u, NULL, NULL);
    ach_leave(ach);
    return discarded;
}

static void RC_CCONV ach_load_done(int result, const char *error_message,
                                   rc_client_t *client, void *userdata) {
    (void)userdata;
    NkAch *ach = (NkAch *)rc_client_get_userdata(client);
    if (!ach) return;
    const rc_client_game_t *game = rc_client_get_game_info(client);
    if (result == RC_OK && game) {
        rc_client_user_game_summary_t summary;
        memset(&summary, 0, sizeof summary);
        rc_client_get_user_game_summary(client, &summary);
        ach_push(ach, NK_ACH_EVENT_GAME_LOADED, game->id, 0u, summary.num_core_achievements,
                 game->title, NULL);
    } else {
        ach_push(ach, NK_ACH_EVENT_GAME_FAILED, 0u, 0u, 0u, NULL,
                 error_message ? error_message : rc_error_str(result));
    }
}

static bool ach_hash_ok(const char *hash) {
    if (!hash) return false;
    for (int i = 0; i < 32; i++) {
        const char c = hash[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return hash[32] == '\0';
}

bool nk_ach_begin_load_game(NkAch *ach, const char *hash) {
    if (!ach || !ach_hash_ok(hash)) return false;
    ach_enter(ach);
    snprintf(ach->game_hash, sizeof ach->game_hash, "%s", hash);
    (void)rc_client_begin_load_game(ach->client, hash, ach_load_done, NULL);
    ach_leave(ach);
    return true;
}

void nk_ach_frame(NkAch *ach) {
    if (!ach) return;
    ach_enter(ach);
    rc_client_do_frame(ach->client);
    ach_leave(ach);
}

void nk_ach_idle(NkAch *ach) {
    if (!ach) return;
    ach_enter(ach);
    rc_client_idle(ach->client);
    ach_leave(ach);
}

bool nk_ach_logged_in(const NkAch *ach) {
    if (!ach) return false;
    ach_enter(ach);
    const bool logged_in = rc_client_get_user_info(ach->client) != NULL;
    ach_leave(ach);
    return logged_in;
}

bool nk_ach_game_loaded(const NkAch *ach) {
    if (!ach) return false;
    ach_enter(ach);
    const bool loaded = rc_client_is_game_loaded(ach->client) != 0;
    ach_leave(ach);
    return loaded;
}

bool nk_ach_hardcore(const NkAch *ach) {
    if (!ach) return false;
    ach_enter(ach);
    const bool hardcore = rc_client_get_hardcore_enabled(ach->client) != 0;
    ach_leave(ach);
    return hardcore;
}

size_t nk_ach_copy_rich_presence(const NkAch *ach, char *out, size_t out_size) {
    if (!out || out_size == 0u) return 0u;
    out[0] = '\0';
    if (!ach) return 0u;
    ach_enter(ach);
    const size_t length = rc_client_get_rich_presence_message(ach->client, out, out_size);
    ach_leave(ach);
    return length;
}

size_t nk_ach_pending_unlock_count(const NkAch *ach) {
    if (!ach) return 0u;
    ach_enter(ach);
    const size_t count = ach->pending_unlock_count;
    ach_leave(ach);
    return count;
}

size_t nk_ach_copy_pending_unlocks(const NkAch *ach, NkAchPendingUnlock *out, size_t max) {
    if (!ach || !out) return 0u;
    ach_enter(ach);
    const size_t count = ach->pending_unlock_count < max ? ach->pending_unlock_count : max;
    for (size_t i = 0u; i < count; i++) out[i] = ach->pending_unlocks[i].unlock;
    ach_leave(ach);
    return count;
}

bool nk_ach_copy_pending_owner(const NkAch *ach, char out[NK_ACH_ACCOUNT_TAG_SIZE]) {
    if (out) out[0] = '\0';
    if (!ach || !out) return false;
    ach_enter(ach);
    const bool owned = ach->pending_unlock_count != 0u && ach_hash_ok(ach->pending_owner);
    if (owned) memcpy(out, ach->pending_owner, NK_ACH_ACCOUNT_TAG_SIZE);
    ach_leave(ach);
    return owned;
}

/* An account tag has the form of a game hash: 32 lowercase hex digits. */
static bool ach_restore(NkAch *ach, const NkAchPendingUnlock *unlock, const char *owner_tag) {
    if (!ach || !unlock || unlock->achievement_id == 0u ||
        !ach_hash_ok(unlock->game_hash) || !ach_hash_ok(owner_tag)) return false;
    if (ach->pending_unlock_count != 0u && strcmp(ach->pending_owner, owner_tag) != 0)
        return false;
    memcpy(ach->pending_owner, owner_tag, NK_ACH_ACCOUNT_TAG_SIZE);
    size_t index = ach_pending_find(ach, unlock->game_hash, unlock->achievement_id);
    if (index != SIZE_MAX) {
        ach->pending_unlocks[index].restored = true;
        return true;
    }
    if (ach->pending_unlock_count == NK_ACH_PENDING_UNLOCKS_MAX) {
        ach_push(ach, NK_ACH_EVENT_QUEUE_FULL, unlock->achievement_id, 0u, 0u, NULL,
                 "The offline unlock queue is full; saved records were not loaded.");
        return false;
    }
    NkAchPendingEntry *entry = &ach->pending_unlocks[ach->pending_unlock_count++];
    entry->unlock = *unlock;
    entry->restored = true;
    return true;
}

bool nk_ach_restore_pending_unlock(NkAch *ach, const NkAchPendingUnlock *unlock,
                                   const char *owner_tag) {
    if (!ach) return false;
    ach_enter(ach);
    const bool restored = ach_restore(ach, unlock, owner_tag);
    if (restored) (void)ach_queue_claim(ach);
    ach_leave(ach);
    return restored;
}

/* What the answer to a saved unlock's award request means for its record. */
typedef enum AchReplayOutcome {
    ACH_REPLAY_AWARDED, /* the server has the unlock: the record leaves the queue */
    ACH_REPLAY_RETRY,   /* no definite answer (no connection, a busy server): the record waits */
    ACH_REPLAY_SIGN_IN, /* the server refused the account: every record waits for a sign-in */
    ACH_REPLAY_REFUSED  /* the server refused this unlock: the record leaves the queue, reported */
} AchReplayOutcome;

/* rc_client gives up an award the server answered with an error of its own and retries the rest
 * (rc_client_award_achievement_callback). A saved record follows the same rule, with two
 * differences that keep records: every 5xx, 408 and 429 waits, and a refused account (401, 403
 * or rcheevos' credential results) stops the replay until the next sign-in instead of dropping
 * each record in turn. */
static AchReplayOutcome ach_replay_outcome(const NkAchResponse *response, uint32_t id,
                                           char *message, size_t message_size) {
    if (message && message_size) message[0] = '\0';
    if (!response || !response->body || response->http_status < 200) return ACH_REPLAY_RETRY;
    const int status = response->http_status;
    if (status >= 500 || status == 408 || status == 429) return ACH_REPLAY_RETRY;

    rc_api_server_response_t server;
    memset(&server, 0, sizeof server);
    server.body = response->body;
    server.body_length = response->body_length;
    server.http_status_code = status;
    rc_api_award_achievement_response_t parsed;
    const int result = rc_api_process_award_achievement_server_response(&parsed, &server);
    AchReplayOutcome outcome = ACH_REPLAY_RETRY;
    if (status < 300 && result == RC_OK && parsed.response.succeeded &&
        (parsed.awarded_achievement_id == id || parsed.response.error_message != NULL)) {
        outcome = ACH_REPLAY_AWARDED;
    } else if (status == 401 || status == 403 || result == RC_INVALID_CREDENTIALS ||
               result == RC_EXPIRED_TOKEN || result == RC_ACCESS_DENIED) {
        outcome = ACH_REPLAY_SIGN_IN;
    } else if (result == RC_OK && !parsed.response.succeeded &&
               parsed.response.error_message != NULL) {
        outcome = ACH_REPLAY_REFUSED;
    }
    if (outcome != ACH_REPLAY_AWARDED && message && message_size &&
        parsed.response.error_message)
        snprintf(message, message_size, "%s", parsed.response.error_message);
    rc_api_destroy_award_achievement_response(&parsed);
    return outcome;
}

static void RC_CCONV ach_pending_replay_done(const NkAchResponse *response, void *done_ctx) {
    AchPendingReplay *replay = (AchPendingReplay *)done_ctx;
    NkAch *ach = replay->ach;
    ach_enter(ach);
    char message[sizeof(((NkAchEvent *)0)->message)];
    const AchReplayOutcome outcome =
        ach_replay_outcome(response, replay->achievement_id, message, sizeof message);

    bool restored = false;
    switch (outcome) {
    case ACH_REPLAY_AWARDED:
    case ACH_REPLAY_REFUSED: {
        const bool removed = ach_pending_remove(ach, replay->game_hash,
                                                 replay->achievement_id, &restored);
        ach->pending_replay_after_ms = 0u;
        if (outcome == ACH_REPLAY_REFUSED)
            ach_push(ach, NK_ACH_EVENT_UNLOCK_REFUSED, replay->achievement_id, 0u, 0u, NULL,
                     message);
        if (removed) ach_push(ach, NK_ACH_EVENT_QUEUE_CHANGED, replay->achievement_id,
                              0u, 0u, NULL, NULL);
        if (removed && restored && ach->pending_unlock_count == 0u)
            ach_push(ach, NK_ACH_EVENT_RECONNECTED, 0u, 0u, 0u, NULL, NULL);
        break;
    }
    case ACH_REPLAY_SIGN_IN:
        ach->pending_replay_blocked = true;
        ach_push(ach, NK_ACH_EVENT_SERVER_ERROR, replay->achievement_id, 0u, 0u, NULL,
                 message[0] ? message
                            : "The account was refused; saved unlocks wait for a sign-in.");
        break;
    case ACH_REPLAY_RETRY:
    default:
        ach->pending_replay_after_ms = ach->pending_replay_now_ms <= UINT64_MAX - 30000u
                                           ? ach->pending_replay_now_ms + 30000u : UINT64_MAX;
        break;
    }
    ach->pending_replay_inflight = false;
    rc_api_destroy_request(&replay->request);
    free(replay);
    ach_leave(ach);
}

static void ach_replay_pending_unlocks(NkAch *ach, uint64_t now_ms) {
    if (!ach->transport || ach->pending_replay_inflight ||
        ach->pending_replay_blocked || now_ms < ach->pending_replay_after_ms) return;
    const rc_client_user_t *user = rc_client_get_user_info(ach->client);
    if (!user || !user->username || !user->token) return;
    /* Only the account the records belong to sends them. */
    char tag[NK_ACH_ACCOUNT_TAG_SIZE];
    if (ach->pending_unlock_count == 0u || !nk_ach_account_tag(user->username, tag) ||
        strcmp(tag, ach->pending_owner) != 0) return;

    size_t index = SIZE_MAX;
    for (size_t i = 0u; i < ach->pending_unlock_count; i++) {
        if (ach->pending_unlocks[i].restored) {
            index = i;
            break;
        }
    }
    if (index == SIZE_MAX) return;

    const NkAchPendingUnlock unlock = ach->pending_unlocks[index].unlock;
    time_t wall_now = time(NULL);
    uint64_t elapsed = wall_now > 0 && (uint64_t)wall_now >= unlock.unlocked_at_utc
                           ? (uint64_t)wall_now - unlock.unlocked_at_utc : 0u;
    if (elapsed > UINT32_MAX) elapsed = UINT32_MAX;
    rc_api_award_achievement_request_t params;
    memset(&params, 0, sizeof params);
    params.username = user->username;
    params.api_token = user->token;
    params.achievement_id = unlock.achievement_id;
    params.hardcore = 0u;
    params.game_hash = unlock.game_hash;
    params.seconds_since_unlock = (uint32_t)elapsed;

    AchPendingReplay *replay = (AchPendingReplay *)calloc(1u, sizeof *replay);
    if (!replay) {
        ach_push(ach, NK_ACH_EVENT_SERVER_ERROR, unlock.achievement_id, 0u, 0u, NULL,
                 "The saved unlock could not be prepared for retry.");
        ach->pending_replay_after_ms = now_ms <= UINT64_MAX - 30000u
                                           ? now_ms + 30000u : UINT64_MAX;
        return;
    }
    if (rc_api_init_award_achievement_request(&replay->request, &params) != RC_OK ||
        !replay->request.url || !replay->request.post_data ||
        !nk_ach_url_allowed(replay->request.url)) {
        rc_api_destroy_request(&replay->request);
        free(replay);
        ach_push(ach, NK_ACH_EVENT_SERVER_ERROR, unlock.achievement_id, 0u, 0u, NULL,
                 "The saved unlock request was refused.");
        ach->pending_replay_after_ms = now_ms <= UINT64_MAX - 30000u
                                           ? now_ms + 30000u : UINT64_MAX;
        return;
    }

    replay->ach = ach;
    replay->achievement_id = unlock.achievement_id;
    snprintf(replay->game_hash, sizeof replay->game_hash, "%s", unlock.game_hash);
    replay->transport_request.url = replay->request.url;
    replay->transport_request.post_data = replay->request.post_data;
    replay->transport_request.content_type = replay->request.content_type;
    ach->pending_replay_inflight = true;
    ach->pending_replay_now_ms = now_ms;
    ach->transport(ach->transport_ctx, &replay->transport_request,
                   ach_pending_replay_done, replay);
}

void nk_ach_replay_pending_unlocks(NkAch *ach, uint64_t now_ms) {
    if (!ach) return;
    ach_enter(ach);
    ach_replay_pending_unlocks(ach, now_ms);
    ach_leave(ach);
}

static size_t ach_copy_achievement_list(const NkAch *ach, NkAchAchievement *out, size_t max,
                                        size_t *total) {
    rc_client_achievement_list_t *list = rc_client_create_achievement_list(
        ach->client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE,
        RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
    if (!list) return 0u;
    size_t copied = 0u;
    size_t count = 0u;
    for (uint32_t bucket = 0u; bucket < list->num_buckets; bucket++) {
        const rc_client_achievement_bucket_t *group = &list->buckets[bucket];
        for (uint32_t item = 0u; item < group->num_achievements; item++) {
            const rc_client_achievement_t *achievement = group->achievements[item];
            if (!achievement) continue;
            if (out && copied < max) {
                NkAchAchievement *entry = &out[copied++];
                memset(entry, 0, sizeof(*entry));
                snprintf(entry->title, sizeof entry->title, "%s",
                         achievement->title ? achievement->title : "Untitled achievement");
                snprintf(entry->description, sizeof entry->description, "%s",
                         achievement->description ? achievement->description : "");
                entry->points = achievement->points;
                entry->unlocked_softcore =
                    (achievement->unlocked & RC_CLIENT_ACHIEVEMENT_UNLOCKED_SOFTCORE) != 0u;
            }
            count++;
        }
    }
    if (total) *total = count;
    rc_client_destroy_achievement_list(list);
    return copied;
}

size_t nk_ach_copy_achievement_list(const NkAch *ach, NkAchAchievement *out, size_t max,
                                    size_t *total) {
    if (total) *total = 0u;
    if (!ach || !ach->client) return 0u;
    ach_enter(ach);
    const size_t copied = ach_copy_achievement_list(ach, out, max, total);
    ach_leave(ach);
    return copied;
}

size_t nk_ach_take_events(NkAch *ach, NkAchEvent *out, size_t max) {
    if (!ach || !out) return 0u;
    size_t n = 0;
    ach_enter(ach);
    while (n < max && ach->event_count > 0u) {
        out[n++] = ach->events[ach->event_head];
        ach->event_head = (ach->event_head + 1u) % NK_ACH_EVENT_QUEUE;
        ach->event_count--;
    }
    ach_leave(ach);
    return n;
}

uint32_t nk_ach_events_dropped(const NkAch *ach) {
    if (!ach) return 0u;
    ach_enter(ach);
    const uint32_t dropped = ach->events_dropped;
    ach_leave(ach);
    return dropped;
}

/* ---- the queue file (nk_achievements.h has the format) ---------------------------------- */

#define ACH_QUEUE_RECORD_SIZE 44u
#define ACH_QUEUE_HEADER_V1_SIZE 16u
#define ACH_QUEUE_HEADER_SIZE 48u
#define ACH_QUEUE_VERSION 2u
#define ACH_QUEUE_FILE_MAX \
    (ACH_QUEUE_HEADER_SIZE + NK_ACH_PENDING_UNLOCKS_MAX * ACH_QUEUE_RECORD_SIZE)

static const uint8_t k_queue_magic[8] = {'N', 'K', 'R', 'A', 'Q', '0', '0', '1'};

static void ach_queue_put_u32(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

static void ach_queue_put_u64(uint8_t *out, uint64_t value) {
    for (unsigned i = 0u; i < 8u; i++) out[i] = (uint8_t)(value >> (8u * i));
}

static uint32_t ach_queue_get_u32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static uint64_t ach_queue_get_u64(const uint8_t *bytes) {
    uint64_t value = 0u;
    for (unsigned i = 0u; i < 8u; i++) value |= (uint64_t)bytes[i] << (8u * i);
    return value;
}

/* The queue as the file's bytes; NULL when out of memory or a record is not well formed. */
static uint8_t *ach_queue_encode(const NkAch *ach, size_t *out_size) {
    const size_t count = ach->pending_unlock_count;
    if (count > NK_ACH_PENDING_UNLOCKS_MAX || (count != 0u && !ach_hash_ok(ach->pending_owner)))
        return NULL;
    const size_t size = ACH_QUEUE_HEADER_SIZE + count * ACH_QUEUE_RECORD_SIZE;
    uint8_t *bytes = (uint8_t *)malloc(size);
    if (!bytes) return NULL;
    memcpy(bytes, k_queue_magic, sizeof k_queue_magic);
    ach_queue_put_u32(bytes + 8u, ACH_QUEUE_VERSION);
    ach_queue_put_u32(bytes + 12u, (uint32_t)count);
    memset(bytes + 16u, '0', 32u);
    if (count != 0u) memcpy(bytes + 16u, ach->pending_owner, 32u);
    for (size_t i = 0u; i < count; i++) {
        const NkAchPendingUnlock *entry = &ach->pending_unlocks[i].unlock;
        uint8_t *record = bytes + ACH_QUEUE_HEADER_SIZE + i * ACH_QUEUE_RECORD_SIZE;
        if (!ach_hash_ok(entry->game_hash) || entry->achievement_id == 0u) {
            free(bytes);
            return NULL;
        }
        memcpy(record, entry->game_hash, 32u);
        ach_queue_put_u32(record + 32u, entry->achievement_id);
        ach_queue_put_u64(record + 36u, entry->unlocked_at_utc);
    }
    *out_size = size;
    return bytes;
}

typedef struct AchQueueFile {
    uint32_t version;
    uint32_t count;
    char owner[NK_ACH_ACCOUNT_TAG_SIZE]; /* version 2 */
    NkAchPendingUnlock *entries;
} AchQueueFile;

/* False for anything but a whole, well-formed version 1 or 2 file. */
static bool ach_queue_decode(const uint8_t *bytes, size_t size, AchQueueFile *file) {
    memset(file, 0, sizeof *file);
    if (!bytes || size < ACH_QUEUE_HEADER_V1_SIZE ||
        memcmp(bytes, k_queue_magic, sizeof k_queue_magic) != 0) return false;
    const uint32_t version = ach_queue_get_u32(bytes + 8u);
    const uint32_t count = ach_queue_get_u32(bytes + 12u);
    if ((version != 1u && version != ACH_QUEUE_VERSION) || count > NK_ACH_PENDING_UNLOCKS_MAX)
        return false;
    const size_t header = version == 1u ? ACH_QUEUE_HEADER_V1_SIZE : ACH_QUEUE_HEADER_SIZE;
    if (size != header + (size_t)count * ACH_QUEUE_RECORD_SIZE) return false;
    if (version == ACH_QUEUE_VERSION) {
        memcpy(file->owner, bytes + 16u, 32u);
        file->owner[32] = '\0';
        if (!ach_hash_ok(file->owner)) return false;
    }
    NkAchPendingUnlock *entries =
        (NkAchPendingUnlock *)calloc(count ? count : 1u, sizeof *entries);
    if (!entries) return false;
    for (uint32_t i = 0u; i < count; i++) {
        const uint8_t *record = bytes + header + (size_t)i * ACH_QUEUE_RECORD_SIZE;
        memcpy(entries[i].game_hash, record, 32u);
        entries[i].game_hash[32] = '\0';
        entries[i].achievement_id = ach_queue_get_u32(record + 32u);
        entries[i].unlocked_at_utc = ach_queue_get_u64(record + 36u);
        if (!ach_hash_ok(entries[i].game_hash) || entries[i].achievement_id == 0u) {
            free(entries);
            return false;
        }
    }
    file->version = version;
    file->count = count;
    file->entries = entries;
    return true;
}

static bool ach_queue_read(const char *path, uint8_t **out, size_t *out_size) {
    const int64_t raw_size = nk_platform_get_file_size(path);
    if (raw_size < 0 || (uint64_t)raw_size > ACH_QUEUE_FILE_MAX) return false;
    FILE *file = nk_fopen_utf8(path, "rb");
    if (!file) return false;
    const size_t size = (size_t)raw_size;
    uint8_t *bytes = (uint8_t *)malloc(size ? size : 1u);
    const bool ok = bytes && fread(bytes, 1u, size, file) == size && fgetc(file) == EOF;
    fclose(file);
    if (!ok) {
        free(bytes);
        return false;
    }
    *out = bytes;
    *out_size = size;
    return true;
}

/* Renames a file this version cannot read beside itself, under the first free of
 * "<path>.unreadable", "<path>.unreadable.2" ... ".99", so its bytes are kept. */
static bool ach_queue_set_aside(const char *path) {
    char kept[NK_MAX_PATH];
    for (unsigned n = 1u; n <= 99u; n++) {
        const int length = n == 1u ? snprintf(kept, sizeof kept, "%s.unreadable", path)
                                   : snprintf(kept, sizeof kept, "%s.unreadable.%u", path, n);
        if (length <= 0 || (size_t)length >= sizeof kept) return false;
        if (nk_platform_file_exists(kept)) continue;
        return nk_rename_utf8(path, kept) == 0;
    }
    return false;
}

bool nk_ach_load_queue_file(NkAch *ach, const char *path, const NkAchQueueLegacy *legacy,
                            NkAchQueueLoad *out) {
    NkAchQueueLoad report;
    memset(&report, 0, sizeof report);
    if (out) *out = report;
    if (!ach || !path || !path[0]) return false;
    if (!nk_platform_file_exists(path)) return true;

    uint8_t *bytes = NULL;
    size_t size = 0u;
    AchQueueFile file;
    memset(&file, 0, sizeof file);
    bool ok = ach_queue_read(path, &bytes, &size) && ach_queue_decode(bytes, size, &file);
    free(bytes);
    if (!ok) {
        report.unreadable = true;
        report.kept_aside = ach_queue_set_aside(path);
        if (!report.kept_aside) {
            ach_enter(ach);
            ach->queue_save_blocked = true;
            ach_leave(ach);
            report.save_blocked = true;
        }
    } else {
        char legacy_tag[NK_ACH_ACCOUNT_TAG_SIZE] = "";
        const bool legacy_known = file.version == 1u && legacy &&
                                  legacy->credential_written_utc != 0u &&
                                  nk_ach_account_tag(legacy->username, legacy_tag);
        report.legacy = file.version == 1u;
        ach_enter(ach);
        /* The file is read into an empty queue: records already waiting are not mixed with it. */
        if (ach->pending_unlock_count != 0u) ok = false;
        for (uint32_t i = 0u; ok && i < file.count; i++) {
            const char *owner = file.owner;
            if (file.version == 1u) {
                if (!legacy_known ||
                    file.entries[i].unlocked_at_utc <= legacy->credential_written_utc) {
                    report.unproven++;
                    continue;
                }
                owner = legacy_tag;
            }
            if (ach_restore(ach, &file.entries[i], owner)) report.loaded++;
            else ok = false;
        }
        if (ok) {
            (void)ach_queue_claim(ach);
        } else {
            /* Not loaded as a whole: what is on disk must not be replaced by less. */
            ach->queue_save_blocked = true;
            report.save_blocked = true;
        }
        ach_leave(ach);
    }
    free(file.entries);
    if (out) *out = report;
    return ok;
}

bool nk_ach_save_queue_file(NkAch *ach, const char *path) {
    char tmp_path[NK_MAX_PATH];
    if (!ach || !path || !path[0]) return false;
    const int length = snprintf(tmp_path, sizeof tmp_path, "%s.tmp", path);
    if (length <= 0 || (size_t)length >= sizeof tmp_path) return false;
    /* The bytes are taken inside the lock; the file is written outside it, so a slow disk
     * never holds a guest frame. */
    size_t size = 0u;
    ach_enter(ach);
    uint8_t *bytes = ach->queue_save_blocked ? NULL : ach_queue_encode(ach, &size);
    ach_leave(ach);
    if (!bytes) return false;
    FILE *file = nk_platform_fopen_private(tmp_path, "wb");
    bool ok = file != NULL;
    if (file) {
        ok = fwrite(bytes, 1u, size, file) == size;
        if (fflush(file) != 0) ok = false;
        if (fclose(file) != 0) ok = false;
    }
    free(bytes);
    if (ok && nk_rename_utf8(tmp_path, path) != 0) ok = false;
    if (!ok && file) (void)nk_remove_utf8(tmp_path);
    return ok;
}

struct rc_client_t *nk_ach_rc_client(NkAch *ach) {
    return ach ? ach->client : NULL;
}

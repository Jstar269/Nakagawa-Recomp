/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_ACHIEVEMENTS_H
#define NK_ACHIEVEMENTS_H

/* RetroAchievements (#1316): achievements for a PSP title, evaluated on guest memory by
 * rcheevos (third_party/rcheevos/, RetroAchievements' own C library) and its rc_client.
 *
 * This module is the bridge between rc_client and Nakagawa. It holds no network code and opens
 * no connection: every request rc_client makes reaches the caller's NkAchTransport, and only
 * after nk_ach_url_allowed() has passed its URL. The caller is the one place that may carry it
 * to the network, as an opt-in feature the user turned on (AGENTS.md section 5; the "Network
 * use" table in docs/SETUP.md lists it once that transport ships). With no transport, nothing
 * is sent and every request fails as a refused client call.
 *
 * Softcore only: hardcore mode needs RetroAchievements' compliance review first, so
 * nk_ach_create turns hardcore off and nothing here turns it on. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The only host rc_client's requests may name. rcheevos' default host is
 * "https://retroachievements.org" (src/rapi/rc_api_common.c), and this bridge never changes it. */
#define NK_ACH_HOST "retroachievements.org"
#define NK_ACH_URL_PREFIX "https://" NK_ACH_HOST "/"

/* RetroAchievements' console ID for the PSP (rc_consoles.h RC_CONSOLE_PSP). Its memory map
 * (src/rcheevos/consoleinfo.c) puts RA address 0 at guest 0x08000000: kernel RAM, user RAM
 * from 0x08800000, and extended RAM from 0x0A000000, up to RA address 0x03FFFFFF. */
#define NK_ACH_CONSOLE_PSP 41u
#define NK_ACH_PSP_USER_RAM_START 0x08800000u
#define NK_ACH_PSP_USER_RAM_SIZE 0x01800000u

/* Guest memory as the bridge may read it: the guest bytes [guest_start, guest_start + size)
 * live at host address base. A read outside that span reads nothing. */
typedef struct NkAchMemory {
    const uint8_t *base;
    uint32_t guest_start;
    uint32_t size;
} NkAchMemory;

/* One request rc_client wants sent, after nk_ach_url_allowed() passed its URL. */
typedef struct NkAchRequest {
    const char *url;
    const char *post_data;    /* may be NULL (a GET) */
    const char *content_type; /* may be NULL */
} NkAchRequest;

/* The answer: the HTTP status and the body. A transport that reached no server answers
 * NK_ACH_HTTP_CLIENT_ERROR (give up) or NK_ACH_HTTP_RETRY (rc_client retries later). */
#define NK_ACH_HTTP_CLIENT_ERROR (-1)
#define NK_ACH_HTTP_RETRY (-2)
typedef struct NkAchResponse {
    const char *body;
    size_t body_length;
    int http_status;
} NkAchResponse;

typedef void (*NkAchDone)(const NkAchResponse *response, void *done_ctx);
/* Sends one request and calls done exactly once, now or later, on any thread: done takes the
 * bridge's lock (NkAchLock) itself. The request's strings are valid only until done is called. */
typedef void (*NkAchTransport)(void *ctx, const NkAchRequest *request, NkAchDone done,
                               void *done_ctx);

typedef enum NkAchEventKind {
    NK_ACH_EVENT_LOGIN_OK = 1,
    NK_ACH_EVENT_LOGIN_FAILED,
    NK_ACH_EVENT_GAME_LOADED,      /* id: the RA game ID; count: achievements in its core set */
    NK_ACH_EVENT_GAME_FAILED,
    NK_ACH_EVENT_UNLOCKED,         /* id, title, points: the achievement just earned */
    NK_ACH_EVENT_SERVER_ERROR,     /* message: the server's error, not retried */
    NK_ACH_EVENT_DISCONNECTED,     /* an unlock is waiting to be sent */
    NK_ACH_EVENT_RECONNECTED,      /* every waiting unlock was sent */
    NK_ACH_EVENT_DISABLED,         /* id: an achievement rc_client turned off because guest memory
                                    * it reads could not be read; message: rc_client's note */
    NK_ACH_EVENT_QUEUE_FULL,       /* a real unlock could not fit the bounded offline queue */
    NK_ACH_EVENT_QUEUE_CHANGED,    /* the persisted pending queue gained or lost a record */
    NK_ACH_EVENT_UNLOCK_REFUSED,   /* id: a saved unlock the server answered with an error of its
                                    * own; it left the queue. message: the server's error */
    NK_ACH_EVENT_LOGIN_UNREACHABLE, /* a token sign-in got no answer from the server (no
                                    * connection, 5xx, 408, 429): nk_ach_token_login_due says
                                    * when to try it again */
    NK_ACH_EVENT_UNLOCKS_DISCARDED /* count: saved unlocks that belong to another account than
                                    * the one now signed in; they left the queue unsent */
} NkAchEventKind;

typedef struct NkAchEvent {
    NkAchEventKind kind;
    uint32_t id;
    uint32_t points;   /* UNLOCKED and LOGIN_OK only; 0 otherwise */
    uint32_t count;    /* GAME_LOADED and UNLOCKS_DISCARDED only; 0 otherwise */
    char title[96];
    char message[160];
} NkAchEvent;

#define NK_ACH_EVENT_QUEUE 32u
#define NK_ACH_PENDING_UNLOCKS_MAX 4096u

/* Token-free, owner-persistable record for an offline unlock. The queue the records wait in
 * belongs to one account, named by its tag (nk_ach_account_tag), never by a record. */
typedef struct NkAchPendingUnlock {
    char game_hash[33];
    uint32_t achievement_id;
    uint64_t unlocked_at_utc;
} NkAchPendingUnlock;

#define NK_ACH_DISPLAY_TITLE_MAX 128u
#define NK_ACH_DISPLAY_DESCRIPTION_MAX 192u
typedef struct NkAchAchievement {
    char title[NK_ACH_DISPLAY_TITLE_MAX];
    char description[NK_ACH_DISPLAY_DESCRIPTION_MAX];
    uint32_t points;
    bool unlocked_softcore;
} NkAchAchievement;

typedef struct NkAch NkAch;

/* The bridge may be entered from two threads: the one that serves guest frames (nk_ach_frame)
 * and the caller's own. Every function below that takes an NkAch, and the done callback a
 * transport is given, runs between enter and leave, so a transport is always called inside the
 * lock. The lock must be recursive: a transport may call done at once. Set it right after
 * nk_ach_create, before a second thread exists; with no lock the bridge is for one thread. */
typedef struct NkAchLock {
    void *context;
    void (*enter)(void *context);
    void (*leave)(void *context);
} NkAchLock;

/* The tag a saved queue names its account by: 32 lowercase hex digits, the MD5 of a fixed label
 * and the account name in lower case. It keeps the name itself out of the queue file; it is not
 * a secret. False for an empty name. */
#define NK_ACH_ACCOUNT_TAG_SIZE 33u
bool nk_ach_account_tag(const char *username, char out[NK_ACH_ACCOUNT_TAG_SIZE]);

/* True only for an https URL on NK_ACH_HOST itself (no other host, port, user part or
 * scheme). */
bool nk_ach_url_allowed(const char *url);

/* Hash the contents of PSP_GAME/PARAM.SFO followed by
 * PSP_GAME/SYSDIR/EBOOT.BIN from the user's ISO, as lowercase MD5. The disc is read
 * as supplied; this does not decrypt or modify it. False means either file or a
 * complete extent could not be read. */
bool nk_ach_psp_hash_iso(const char *iso_path, char out_hash[33]);

/* Reads up to n bytes at RA address ra_address through the PSP memory map. Returns the bytes
 * read: 0 when the address is outside the map or outside mem's span, fewer than n when the
 * span or the map ends first. The PSP's three regions are one run of guest memory, so a read may
 * cross from one into the next. Nothing outside mem's span is ever read. */
uint32_t nk_ach_read_memory(const NkAchMemory *mem, uint32_t ra_address, uint8_t *out,
                            uint32_t n);

/* A client over mem (copied) with transport, which may be NULL: then every request is refused
 * and nothing is sent. NULL when rc_client cannot be created. Hardcore is off. */
NkAch *nk_ach_create(const NkAchMemory *mem, NkAchTransport transport, void *transport_ctx);
void nk_ach_set_lock(NkAch *ach, const NkAchLock *lock);
void nk_ach_destroy(NkAch *ach);

/* The player updates this bounded guest-memory span when a runtime channel is opened. */
void nk_ach_set_memory(NkAch *ach, const NkAchMemory *mem);

/* Signs in with an RA API token (never a password: the token comes from the user's own
 * sign-in, kept in the OS credential store by the player). False only for a missing name or
 * token; otherwise the outcome arrives as a LOGIN_OK, LOGIN_FAILED or LOGIN_UNREACHABLE event. */
bool nk_ach_begin_login(NkAch *ach, const char *username, const char *token);
/* After LOGIN_UNREACHABLE: true once when the token sign-in is due again, 30 seconds after the
 * first call that follows the failure, then twice as long after each further one, up to ten
 * minutes. The bridge keeps no token: the caller reads it from the credential store again and
 * calls nk_ach_begin_login. A sign-in the server answered, a password sign-in, nk_ach_logout and
 * nk_ach_sign_out end the retries. */
bool nk_ach_token_login_due(NkAch *ach, uint64_t now_ms);
/* Exchanges the password once for the RA token. The bridge retains no password after rcheevos
 * has constructed its request; the player must clear its input buffers and store only the token. */
bool nk_ach_begin_login_with_password(NkAch *ach, const char *username, const char *password);
bool nk_ach_copy_user_token(const NkAch *ach, char *out, size_t out_size);
bool nk_ach_copy_user_name(const NkAch *ach, char *out, size_t out_size);
/* Ends the session and keeps the pending unlocks: the stored credential, and so the account,
 * stays (the Achievements switch turned off). */
void nk_ach_logout(NkAch *ach);
/* The user's Sign out: ends the session and discards every pending unlock, returning how many.
 * The queue is the signed-out account's, and nothing of that account stays on this PC. */
size_t nk_ach_sign_out(NkAch *ach);
/* Loads the achievement set for the disc whose RA hash (32 lowercase hex digits, the MD5 RA's
 * PSP rule takes of PARAM.SFO then EBOOT.BIN) is hash. False only for a malformed hash;
 * otherwise the outcome is a GAME_LOADED or GAME_FAILED event. */
bool nk_ach_begin_load_game(NkAch *ach, const char *hash);

/* Once per guest frame, at the vblank boundary: evaluates the achievements on guest memory. */
void nk_ach_frame(NkAch *ach);
/* While the guest is paused: keeps rc_client's retries and pings going without evaluating. */
void nk_ach_idle(NkAch *ach);

bool nk_ach_logged_in(const NkAch *ach);
bool nk_ach_game_loaded(const NkAch *ach);
/* Always false: see the softcore note at the top. */
bool nk_ach_hardcore(const NkAch *ach);
size_t nk_ach_copy_rich_presence(const NkAch *ach, char *out, size_t out_size);

/* Moves up to max queued events, oldest first, into out; returns how many. When more than
 * NK_ACH_EVENT_QUEUE are waiting, the oldest are dropped and nk_ach_events_dropped counts
 * them. */
size_t nk_ach_take_events(NkAch *ach, NkAchEvent *out, size_t max);
uint32_t nk_ach_events_dropped(const NkAch *ach);

/* The player persists these token-free records in an owner-only file. Restored records are
 * replayed with rcheevos' request builder through the same opt-in transport after sign-in,
 * one at a time. An unlock the server has leaves the queue; with no definite answer (no
 * connection, 5xx, 408, 429) it waits 30 seconds; an unlock the server refuses by its own error
 * leaves the queue as an UNLOCK_REFUSED event, so it cannot hold the others back; a refused
 * account (401, 403) keeps every record and stops the replay until the next sign-in.
 *
 * The queue belongs to one account, the first to put a record in it, and only that account's
 * session sends it. When another account signs in, or a record is added or restored while
 * another is signed in, the records are discarded and reported (UNLOCKS_DISCARDED): they are
 * never awarded to an account that did not earn them. */
size_t nk_ach_pending_unlock_count(const NkAch *ach);
size_t nk_ach_copy_pending_unlocks(const NkAch *ach, NkAchPendingUnlock *out, size_t max);
/* owner_tag: the account the record belongs to (nk_ach_account_tag). False for a malformed
 * record or tag, a full queue, or a queue that already holds another account's records. */
bool nk_ach_restore_pending_unlock(NkAch *ach, const NkAchPendingUnlock *unlock,
                                   const char *owner_tag);
/* The tag of the account the waiting records belong to; false when none waits. */
bool nk_ach_copy_pending_owner(const NkAch *ach, char out[NK_ACH_ACCOUNT_TAG_SIZE]);
void nk_ach_replay_pending_unlocks(NkAch *ach, uint64_t now_ms);

/* The queue file. All integers are little-endian.
 *   0   8 bytes  "NKRAQ001"
 *   8   u32      format version: 2 (1 is read, see below)
 *   12  u32      record count, at most NK_ACH_PENDING_UNLOCKS_MAX
 *   16  32 bytes the owner's account tag, lowercase hex; thirty-two '0' when count is 0
 *   48  count records of 44 bytes: 32 bytes game hash (lowercase hex), u32 achievement ID,
 *       u64 unlock time (seconds since 1970 UTC)
 * Version 1 had no owner: its header ends after the count and its records start at 16.
 *
 * nk_ach_save_queue_file writes the file through a private temporary file and a rename. It
 * refuses, and leaves the file as it is, after a load that could neither read the file nor set
 * it aside.
 *
 * nk_ach_load_queue_file reads it into an empty queue, once, before sign-in. No file is not an
 * error. A file this version cannot read (damaged, cut short, from a later version, or not
 * readable at all) is never loaded and never overwritten: it is renamed beside itself with the
 * ending ".unreadable" (".unreadable.2" and so on when that exists), and when even that fails
 * saving is refused for this run.
 *
 * A version 1 record is loaded only when it can be tied to the account whose credential is
 * stored: it was unlocked after that credential was last written. Every sign-in writes the
 * credential and a record is only made in a signed-in session, so, with a system clock that did
 * not go back in between, such a record was earned by that account. The others are counted in
 * unproven and not loaded; the caller reports them and saves, which drops them. */
typedef struct NkAchQueueLegacy {
    const char *username;            /* the stored credential's account, or NULL */
    uint64_t credential_written_utc; /* when it was last written; 0 when not known */
} NkAchQueueLegacy;
typedef struct NkAchQueueLoad {
    size_t loaded;      /* records now waiting */
    size_t unproven;    /* version 1 records not tied to the stored account: not loaded */
    bool legacy;        /* the file was version 1: save now, so the next start reads an owner */
    bool unreadable;    /* the file is not a queue this version reads */
    bool kept_aside;    /* ... and it was renamed with the ".unreadable" ending */
    bool save_blocked;  /* nk_ach_save_queue_file refuses for the rest of this run */
} NkAchQueueLoad;
bool nk_ach_load_queue_file(NkAch *ach, const char *path, const NkAchQueueLegacy *legacy,
                            NkAchQueueLoad *out);
bool nk_ach_save_queue_file(NkAch *ach, const char *path);
/* Copies only the official/core achievement set. `total` receives the full count even when
 * `out` is smaller; the caller owns all copied strings. */
size_t nk_ach_copy_achievement_list(const NkAch *ach, NkAchAchievement *out, size_t max,
                                    size_t *total);

/* The rc_client behind ach, for the bridge's own tests: they point it at another host to prove
 * that the request gate refuses it. Nothing else needs it. */
struct rc_client_t *nk_ach_rc_client(NkAch *ach);

#ifdef __cplusplus
}
#endif

#endif /* NK_ACHIEVEMENTS_H */

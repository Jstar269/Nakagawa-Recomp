/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_ACHIEVEMENTS_H
#define NK_ACHIEVEMENTS_H

/* RetroAchievements (#1316): achievements for a PSP title, evaluated on guest memory by
 * rcheevos (third_party/rcheevos/, RetroAchievements' own C library) and its rc_client.
 *
 * This module is the bridge between rc_client and Nakagawa. It holds no network code and opens
 * no connection: every request rc_client makes reaches the caller's NkAchTransport, and only
 * after nk_ach_url_allowed() has passed its URL; the one other request, a badge picture's
 * (nk_ach_request_badge), goes the same way after nk_ach_badge_url_allowed() has passed its
 * URL on RetroAchievements' media host. The caller is the one place that may carry it
 * to the network, as an opt-in feature the user turned on (AGENTS.md section 5; the "Network
 * use" table in docs/SETUP.md lists it once that transport ships). With no transport, nothing
 * is sent and every request fails as a refused client call.
 *
 * Softcore only: hardcore mode needs RetroAchievements' compliance review first, so
 * nk_ach_create turns hardcore off and nothing here turns it on. No setting, environment
 * variable or file reaches it either: the bridge turns hardcore off again before rc_client
 * evaluates a frame, signs in or loads a game, and refuses any request that would record
 * hardcore play (an "h" field other than 0, or a leaderboard entry). */

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

/* An achievement's badge name: what rc_client keeps of the server's "BadgeName" (rc_client.h,
 * rc_client_achievement_t.badge_name, 8 bytes), here only when it is one to seven ASCII letters
 * and digits (nk_ach_badge_name_ok); empty otherwise. */
#define NK_ACH_BADGE_NAME_MAX 8u

/* RetroAchievements' console ID for the PSP (rc_consoles.h RC_CONSOLE_PSP). Its memory map
 * (src/rcheevos/consoleinfo.c) puts RA address 0 at guest 0x08000000: kernel RAM, user RAM
 * from 0x08800000, and extended RAM from 0x0A000000, up to RA address 0x03FFFFFF. */
#define NK_ACH_CONSOLE_PSP 41u
#define NK_ACH_PSP_USER_RAM_START 0x08800000u
#define NK_ACH_PSP_USER_RAM_SIZE 0x01800000u
/* The guest span that whole map covers, [0x08000000, 0x0C000000): what a caller that holds all
 * of guest RAM hands the bridge (NkAchMemory), so every region rcheevos defines reads. */
#define NK_ACH_PSP_MAP_GUEST_START 0x08000000u
#define NK_ACH_PSP_MAP_GUEST_SIZE 0x04000000u

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
 * NK_ACH_HTTP_CLIENT_ERROR (give up) or NK_ACH_HTTP_RETRY; with no body, the failure reports "no
 * response from the RetroAchievements server". rc_client honours NK_ACH_HTTP_RETRY (and the
 * server's 429 and 5xx answers) only for unlocks and leaderboard submissions, which it sends again
 * later; sign-in and game load end at once, as LOGIN_FAILED (LOGIN_UNREACHABLE for a token
 * sign-in that reached no server, see nk_ach_token_login_due) or GAME_FAILED, and the caller
 * starts them again. */
#define NK_ACH_HTTP_CLIENT_ERROR (-1)
#define NK_ACH_HTTP_RETRY (-2)
typedef struct NkAchResponse {
    const char *body;
    size_t body_length;
    int http_status;
} NkAchResponse;

typedef void (*NkAchDone)(const NkAchResponse *response, void *done_ctx);
/* Sends one request and calls done exactly once, now or later, on any thread: done takes the
 * bridge's lock (NkAchLock) itself. The request's strings are the bridge's own copies: they stay
 * valid after the transport returns, until done is called. The bridge hands the transport one
 * request at a time, in the order rc_client and the saved queue made them: the next goes only
 * once done has been called for the last, so a transport needs one slot, and one that is busy
 * is never asked. */
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
                                    * it reads could not be read or its trigger could not be
                                    * parsed; message: rc_client's note */
    NK_ACH_EVENT_QUEUE_FULL,       /* a real unlock could not fit the bounded offline queue */
    NK_ACH_EVENT_QUEUE_CHANGED,    /* the persisted pending queue gained or lost a record */
    NK_ACH_EVENT_UNLOCK_REFUSED,   /* id: a saved unlock the server answered with an error of its
                                    * own; it left the queue. message: the server's error */
    NK_ACH_EVENT_LOGIN_UNREACHABLE, /* a token sign-in got no answer from the server (no
                                    * connection, 5xx, 408, 429): nk_ach_token_login_due says
                                    * when to try it again */
    NK_ACH_EVENT_UNLOCKS_DISCARDED, /* count: saved unlocks that belong to another account than
                                    * the one now signed in; they left the queue unsent */
    NK_ACH_EVENT_SET_COMPLETED     /* id: the core set's ID; title: the game's. Every achievement
                                    * in it is unlocked; raised by the frame after the award that
                                    * completed it, and never once its game is unloaded */
} NkAchEventKind;

typedef struct NkAchEvent {
    NkAchEventKind kind;
    uint32_t id;
    uint32_t points;   /* UNLOCKED and LOGIN_OK only; 0 otherwise */
    uint32_t count;    /* GAME_LOADED and UNLOCKS_DISCARDED only; 0 otherwise */
    char title[96];
    char message[160];
    char badge_name[NK_ACH_BADGE_NAME_MAX]; /* UNLOCKED only, as NkAchAchievement's */
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
    char badge_name[NK_ACH_BADGE_NAME_MAX];
    uint32_t points;
    bool unlocked_softcore;
} NkAchAchievement;

/* The core set's progress for the signed-in account: rc_client's own count
 * (rc_client_get_user_game_summary). */
typedef struct NkAchProgress {
    uint32_t unlocked;
    uint32_t total;
    uint32_t points_unlocked;
    uint32_t points_total;
} NkAchProgress;

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
/* Ends the client; ach is not used again. Requests still waiting their turn are answered as
 * refused and never sent. With a request the transport holds, the bridge and rc_client stay
 * alive until the transport calls its done (which it still must, exactly once): that late answer
 * is dropped, rc_client is told the request was refused, and that call frees everything. The
 * lock (NkAchLock), when one was set, must outlive that call. Nothing after nk_ach_destroy reads
 * guest memory or raises an event the caller sees. */
void nk_ach_destroy(NkAch *ach);

/* The player updates this bounded guest-memory span when a runtime channel is opened. */
void nk_ach_set_memory(NkAch *ach, const NkAchMemory *mem);

/* Signs in with an RA API token (never a password: the token comes from the user's own
 * sign-in, kept in the OS credential store by the player). False for a missing name or token,
 * or when another account was signed in last and no fresh rc_client can be made for this one
 * (rcheevos' retries of an award are rebuilt with whoever is signed in, so an rc_client serves
 * one account; nk_ach_sign_out ends one too). Otherwise the outcome arrives as a LOGIN_OK,
 * LOGIN_FAILED or LOGIN_UNREACHABLE event. */
bool nk_ach_begin_login(NkAch *ach, const char *username, const char *token);
/* After LOGIN_UNREACHABLE: true once when the token sign-in is due again, 30 seconds after the
 * first call that follows the failure, then twice as long after each further one, up to ten
 * minutes. The bridge keeps no token: the caller reads it from the credential store again and
 * calls nk_ach_begin_login. A sign-in the server answered, a password sign-in, nk_ach_logout and
 * nk_ach_sign_out end the retries. */
bool nk_ach_token_login_due(NkAch *ach, uint64_t now_ms);
/* Exchanges the password once for the RA token. The bridge keeps no password: it clears its own
 * copy of the request when the transport has answered, and clears rcheevos' request buffer before
 * rcheevos frees it (every request's, so the token's too). It cannot clear what it never sees: the
 * caller's own buffers (the player clears its prompt's), the transport's (the player's clears its
 * copy) and the operating system's. False as nk_ach_begin_login is. */
bool nk_ach_begin_login_with_password(NkAch *ach, const char *username, const char *password);
bool nk_ach_copy_user_token(const NkAch *ach, char *out, size_t out_size);
/* The signed-in account's name as the server gave it, cleaned as its other text is (control,
 * bidirectional and invisible characters removed); false when none, or when out is too small. */
bool nk_ach_copy_user_name(const NkAch *ach, char *out, size_t out_size);
/* Ends the session and keeps the pending unlocks: the stored credential, and so the account,
 * stays (the Achievements switch turned off). The bridge sends those unlocks itself after the
 * account's next sign-in (nk_ach_replay_pending_unlocks), as it does once the switch is off
 * (nk_ach_set_online): rcheevos gives up a retry that finds nobody signed in or is refused. That
 * sign-in starts on a fresh rc_client, so rcheevos' own retries scheduled before the logout never
 * send an unlock a second time. A game load still on its way is abandoned and raises nothing. */
void nk_ach_logout(NkAch *ach);
/* The user's Sign out: ends the session and discards every pending unlock, returning how many.
 * The queue is the signed-out account's, and nothing of that account stays on this PC: rcheevos'
 * own retries of its awards end too, and answers still on their way are dropped. */
size_t nk_ach_sign_out(NkAch *ach);
/* Loads the achievement set for the disc whose RA hash (32 lowercase hex digits, the MD5 RA's
 * PSP rule takes of PARAM.SFO then EBOOT.BIN) is hash. False only for a malformed hash;
 * otherwise the outcome is a GAME_LOADED or GAME_FAILED event. A set the server gives for another
 * console is unloaded and reported as GAME_FAILED naming that console. */
bool nk_ach_begin_load_game(NkAch *ach, const char *hash);
/* The game has ended (it exited, or its frames stopped): rc_client forgets it, so its Rich
 * Presence ping, which rc_client sends every two minutes while a game is loaded, goes out no
 * more. Pending unlocks and rc_client's retries of their awards stay. Nothing is sent. */
void nk_ach_unload_game(NkAch *ach);

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
 *
 * nk_ach_save_queue_file writes the file through a private temporary file and a rename. It
 * refuses, and leaves the file as it is, after a load that could neither read the file nor set
 * it aside.
 *
 * nk_ach_load_queue_file reads it into an empty queue, once, before sign-in. No file is not an
 * error. A file this version cannot read (damaged, cut short, from another version, or not
 * readable at all) is never loaded and never overwritten: it is renamed beside itself with the
 * ending ".unreadable" (".unreadable.2" and so on when that exists), and when even that fails
 * saving is refused for this run. */
typedef struct NkAchQueueLoad {
    size_t loaded;      /* records now waiting */
    bool unreadable;    /* the file is not a queue this version reads */
    bool kept_aside;    /* ... and it was renamed with the ".unreadable" ending */
    bool save_blocked;  /* nk_ach_save_queue_file refuses for the rest of this run */
} NkAchQueueLoad;
bool nk_ach_load_queue_file(NkAch *ach, const char *path, NkAchQueueLoad *out);
bool nk_ach_save_queue_file(NkAch *ach, const char *path);
/* Copies only the official/core achievement set: the unlocked ones first, then the locked, each
 * in the set's own order. `total` receives the full count even when `out` is smaller; the caller
 * owns all copied strings. */
size_t nk_ach_copy_achievement_list(const NkAch *ach, NkAchAchievement *out, size_t max,
                                    size_t *total);
/* False, with out zeroed, when no game is loaded. */
bool nk_ach_copy_progress(const NkAch *ach, NkAchProgress *out);

/* Badge pictures. An achievement's badge is a PNG on RetroAchievements' media host, at the URL
 * rcheevos builds for it (src/rapi/rc_api_common.c, rc_api_init_fetch_image_request_hosted with
 * no custom host, as rc_client_get_image_url does):
 *   https://media.retroachievements.org/Badge/<badge name>.png       (unlocked)
 *   https://media.retroachievements.org/Badge/<badge name>_lock.png  (locked)
 * The server may name another URL in a set's "BadgeURL" and "BadgeLockedURL"; the bridge never
 * uses those: it builds the URL from the badge name alone, so the media host above is the only
 * one a badge request can name. A badge request is a GET with no post data, no cookie, no token,
 * no account name and no console ID: the URL above is all it says. */
#define NK_ACH_BADGE_HOST "media.retroachievements.org"
#define NK_ACH_BADGE_URL_PREFIX "https://" NK_ACH_BADGE_HOST "/Badge/"
/* RetroAchievements' badges are 64 x 64 PNG pictures of a few kilobytes. A picture is refused by
 * name beyond these, before any pixel is decoded. */
#define NK_ACH_BADGE_MAX_BYTES (64u * 1024u)
#define NK_ACH_BADGE_MAX_SIDE 128u
/* True for one to seven ASCII letters and digits: no path separator, dot or other character a
 * file name or URL could be steered by. */
bool nk_ach_badge_name_ok(const char *name);
/* The badge URL for name, built by rcheevos as above; false, with out empty, for a name
 * nk_ach_badge_name_ok refuses, a URL nk_ach_badge_url_allowed refuses, or an out too small. */
bool nk_ach_badge_url(const char *name, bool locked, char *out, size_t out_size);
/* True only for NK_ACH_BADGE_URL_PREFIX followed by a valid badge name and ".png" or
 * "_lock.png", and nothing more. */
bool nk_ach_badge_url_allowed(const char *url);
/* True when bytes start as a PNG whose header (IHDR) gives a width and height of 1 to
 * NK_ACH_BADGE_MAX_SIDE, and size is at most NK_ACH_BADGE_MAX_BYTES. */
bool nk_ach_badge_png_ok(const uint8_t *bytes, size_t size);

/* One badge request's outcome. ok: png holds size bytes the caller frees with
 * nk_ach_badge_free; otherwise why names the refusal. */
typedef struct NkAchBadge {
    char name[NK_ACH_BADGE_NAME_MAX];
    bool locked;
    bool ok;
    uint8_t *png;
    size_t size;
    char why[96];
} NkAchBadge;
/* Asks for one badge through the transport, in turn with rc_client's own requests. Only while
 * the switch is on, an account is signed in and a game is loaded, for a valid name, and while no
 * other badge request is unanswered or untaken; false, with nothing sent, otherwise. The answer
 * is dropped, never handed out, when the switch goes off, the account ends or the game ends
 * before it arrives. */
bool nk_ach_request_badge(NkAch *ach, const char *name, bool locked);
/* True while a badge request waits for its answer. */
bool nk_ach_badge_pending(const NkAch *ach);
/* Moves the answered badge out; false when none waits. */
bool nk_ach_take_badge(NkAch *ach, NkAchBadge *out);
void nk_ach_badge_free(NkAchBadge *badge);

/* The user's Achievements switch. Off: nothing more reaches the transport. Requests waiting their
 * turn are answered as refused and never sent, rc_client's own retries and saved unlocks stay
 * unsent, and a token sign-in is not tried again; the request the transport already holds still
 * gets its answer. On (the default) sends again. nk_ach_online is false while it is off or there
 * is no transport. */
void nk_ach_set_online(NkAch *ach, bool online);
bool nk_ach_online(const NkAch *ach);

/* The User-Agent every request carries, in the form RetroAchievements documents for a client
 * (the rcheevos wiki, "rc_client integration"):
 *   NakagawaRecomp/<version> (<system>) rcheevos/<rcheevos version>
 * version is the player's version: only its numeric part ("0.1.0" of "0.1.0-dev") is sent, since
 * RetroAchievements reads a version made of numbers. system names the operating system
 * ("Windows 10.0.26100") and may be NULL or empty, which leaves out "(...)". The last part is
 * rc_client_get_user_agent_clause's. False, with out empty, for a version with no numeric part,
 * a system with a character other than printable ASCII or with a parenthesis, or an out too
 * small. */
#define NK_ACH_USER_AGENT_PRODUCT "NakagawaRecomp"
#define NK_ACH_USER_AGENT_MAX 192u
#define NK_ACH_USER_AGENT_SYSTEM_MAX 96u
bool nk_ach_user_agent(const char *version, const char *system, char *out, size_t out_size);

/* What rc_client asks of an emulator (rc_client.h, rc_client_reset and the progress functions):
 * a caller that resets the guest (a title restarted from its start) calls nk_ach_reset before the
 * next nk_ach_frame, and a caller that saves and loads guest state saves nk_ach_progress_size
 * bytes from nk_ach_serialize_progress with it and hands them to nk_ach_deserialize_progress when
 * it loads that state. Otherwise rc_client keeps the hit counts and earlier memory values of the
 * guest that was replaced, and an achievement can unlock on a change that never happened in the
 * game. The player has neither a guest reset nor a state load today (its runtime runs in its own
 * process); the first one that ships must call these. nk_ach_deserialize_progress is false for no
 * data, no game loaded, or data rcheevos refuses (it then resets every achievement). */
void nk_ach_reset(NkAch *ach);
size_t nk_ach_progress_size(NkAch *ach);
bool nk_ach_serialize_progress(NkAch *ach, uint8_t *out, size_t out_size);
bool nk_ach_deserialize_progress(NkAch *ach, const uint8_t *data, size_t size);

/* The rc_client behind ach, for the bridge's own tests: they point it at another host to prove
 * that the request gate refuses it. Nothing else needs it. */
struct rc_client_t *nk_ach_rc_client(NkAch *ach);

#ifdef __cplusplus
}
#endif

#endif /* NK_ACHIEVEMENTS_H */

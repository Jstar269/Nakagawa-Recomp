/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_PLAYER_DISCORD_PRESENCE_H
#define NK_PLAYER_DISCORD_PRESENCE_H

/* Discord Rich Presence (#2259): opt-in, off by default (owner decision 2026-11-08).
 *
 * The player writes to the Discord app on this PC through its local IPC pipe
 * (the named pipes discord-ipc-0 to discord-ipc-9 on Windows) and makes no network
 * connection itself. The
 * protocol is written from Discord's public RPC documentation: each message is a frame of a
 * little-endian opcode, a little-endian length and that many bytes of UTF-8 JSON. The client
 * sends HANDSHAKE {"v":1,"client_id":...}, waits for the READY dispatch, then SET_ACTIVITY.
 * No Discord SDK is used or vendored.
 *
 * What it sends is the game's title, the play mode and when the game started: never a
 * path, a save, a user name or anything from the game's memory. The SET_ACTIVITY command
 * also carries the player's process ID, which the protocol requires so Discord can clear
 * the status if the player ends without clearing it. Discord's READY answer names the
 * signed-in account; the client reads only its event name, stores nothing from it and
 * clears the frame's bytes once handled.
 *
 * The client never blocks the launcher: reads take only what has arrived, and a write
 * gives up after NK_DISCORD_WRITE_TIMEOUT_MS. When Discord is not running it fails
 * quietly, logs nothing and tries again at most once every NK_DISCORD_RETRY_MS. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nk_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The project's Discord application ID: Discord shows its name ("Playing ...") above the
 * activity. It is a public value, not a secret. Until the maintainer sets it this is the
 * placeholder "", which is not a valid ID, so the feature fails closed: Settings cannot
 * turn it on and the player never opens the pipe. */
#define NK_DISCORD_APPLICATION_ID ""

/* The pipe names the Discord app serves: the prefix followed by 0 to 9. */
#define NK_DISCORD_PIPE_PREFIX "discord-ipc-"
#define NK_DISCORD_PIPE_COUNT 10

/* At most one connection attempt (each pipe name once) per minute. */
#define NK_DISCORD_RETRY_MS 60000u
/* A connection that has not answered the handshake by then is dropped. */
#define NK_DISCORD_HANDSHAKE_TIMEOUT_MS 10000u
#define NK_DISCORD_WRITE_TIMEOUT_MS 250u

/* Discord's limits: a details or state string holds 2 to 128 characters. */
#define NK_DISCORD_TEXT_MAX 128
/* The largest frame the client accepts from Discord (READY is about 1 KB). */
#define NK_DISCORD_RX_MAX 16384
/* One update handles at most this many frames from Discord (and reads at most
 * NK_DISCORD_RX_MAX bytes), so a peer that never stops writing cannot hold the launcher. */
#define NK_DISCORD_FRAMES_PER_UPDATE 64u

/* Frame opcodes (Discord RPC documentation). */
enum {
    NK_DISCORD_OP_HANDSHAKE = 0,
    NK_DISCORD_OP_FRAME = 1,
    NK_DISCORD_OP_CLOSE = 2,
    NK_DISCORD_OP_PING = 3,
    NK_DISCORD_OP_PONG = 4
};

/* How the client reaches Discord. NULL in nk_discord_client_init uses the local pipe
 * (nk_platform_local_pipe_*); the tests pass a fake. `read` returns what has already
 * arrived without waiting: a byte count, 0 for nothing, -1 when the other end closed. */
typedef struct {
    bool (*open)(void *ctx, const char *pipe_name);
    bool (*write)(void *ctx, const void *data, size_t len);
    long (*read)(void *ctx, void *buf, size_t cap);
    void (*close)(void *ctx);
    void *ctx;
} NkDiscordTransport;

/* What the status shows. An empty details or state is left out. */
typedef struct {
    char details[NK_DISCORD_TEXT_MAX * 4 + 1]; /* the game's title, UTF-8 */
    char state[32];                            /* "Authentic", "Enhanced" or "Ace" */
    int64_t start_ms;                          /* Unix time in milliseconds; 0 omits it */
} NkDiscordActivity;

typedef enum {
    NK_DISCORD_CLOSED = 0,
    NK_DISCORD_WAITING_READY, /* handshake sent */
    NK_DISCORD_READY
} NkDiscordLink;

typedef struct {
    NkDiscordTransport transport;
    NkLocalPipe pipe; /* the default transport's pipe */
    char application_id[24];
    char pipe_prefix[48];
    unsigned long pid;
    NkDiscordLink link;
    bool attempted;            /* an attempt was made; next_attempt_ms then applies */
    uint64_t next_attempt_ms;
    uint64_t handshake_deadline_ms;
    bool shown;                /* `sent` is on Discord now */
    NkDiscordActivity sent;
    unsigned nonce;
    unsigned char rx[NK_DISCORD_RX_MAX];
    size_t rx_used;
    /* For tests and the developer view: connection attempts made, and the last error code
       Discord answered a command with (0 for none). */
    unsigned attempts;
    int last_error_code;
} NkDiscordClient;

/* A Discord application ID: a snowflake of 17 to 20 decimal digits. */
bool nk_discord_application_id_valid(const char *id);
/* Whether this build can show Discord status at all: NK_DISCORD_APPLICATION_ID is set and
 * the host has local pipes. False keeps the Settings switch off. */
bool nk_discord_available(void);

/* Fill an activity. The title, trimmed of spaces, is cut to 128 characters on a character
 * boundary, and a partial character at its end (a title cut short upstream) is dropped. A
 * title that is otherwise not valid UTF-8, is shorter than 2 characters, or holds a
 * backslash (it would be a Windows path, not a title) is left out. False when nothing would
 * be shown. */
bool nk_discord_activity_make(NkDiscordActivity *out, const char *title, const char *mode,
                              int64_t start_ms);

/* The SET_ACTIVITY command's JSON, NUL-terminated; NULL activity clears the status
 * ("activity":null). Returns its length, or 0 when it does not fit. */
size_t nk_discord_activity_json(char *out, size_t cap, unsigned long pid,
                                const NkDiscordActivity *activity, unsigned nonce);
/* One frame: opcode and length (little-endian uint32 each), then the payload. Returns the
 * frame's size, or 0 when it does not fit. */
size_t nk_discord_frame(unsigned char *out, size_t cap, uint32_t opcode, const void *payload,
                        size_t len);

/* Start a client. An invalid application ID leaves it unable to connect: every update is a
 * no-op. transport NULL selects the local pipe; pipe_prefix NULL selects
 * NK_DISCORD_PIPE_PREFIX (the tests name their own pipes). */
void nk_discord_client_init(NkDiscordClient *client, const char *application_id,
                            const NkDiscordTransport *transport, const char *pipe_prefix);
/* Call once per frame with what should show now: an activity while a game runs, NULL when
 * none does. It connects when there is something to show (at most once per
 * NK_DISCORD_RETRY_MS), sends the activity once Discord is READY and whenever it changes,
 * and answers PING. With NULL it clears what it showed; an open connection stays open for
 * the next game. */
void nk_discord_client_update(NkDiscordClient *client, const NkDiscordActivity *activity,
                              uint64_t now_ms);
/* Clear what it showed and close: the setting was turned off, or the player exits. */
void nk_discord_client_shutdown(NkDiscordClient *client);

#ifdef __cplusplus
}
#endif

#endif /* NK_PLAYER_DISCORD_PRESENCE_H */

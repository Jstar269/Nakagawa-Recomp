/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Discord Rich Presence over the Discord app's local IPC pipe (#2259). The design notes,
 * and what is and is not sent, are in discord_presence.h. */

#include "discord_presence.h"

#include <stdio.h>
#include <string.h>

#include "nk_json.h"

bool nk_discord_application_id_valid(const char *id) {
    if (!id) return false;
    size_t len = strlen(id);
    if (len < 17 || len > 20) return false;
    for (size_t i = 0; i < len; i++) {
        if (id[i] < '0' || id[i] > '9') return false;
    }
    return true;
}

bool nk_discord_available(void) {
    return nk_discord_application_id_valid(NK_DISCORD_APPLICATION_ID) &&
           nk_platform_local_pipe_supported();
}

/* The byte length of the UTF-8 character starting at s (already validated). */
static size_t utf8_char_len(unsigned char c) {
    if (c < 0x80u) return 1;
    if ((c & 0xE0u) == 0xC0u) return 2;
    if ((c & 0xF0u) == 0xE0u) return 3;
    return 4;
}

/* Copy at most `max_chars` characters of valid UTF-8 `src` into out[cap]; returns how many
   characters it copied. */
static size_t utf8_copy_chars(char *out, size_t cap, const char *src, size_t max_chars) {
    size_t used = 0;
    size_t chars = 0;
    while (src[used] != '\0' && chars < max_chars) {
        size_t n = utf8_char_len((unsigned char)src[used]);
        if (used + n + 1 > cap) break;
        used += n;
        chars++;
    }
    memcpy(out, src, used);
    out[used] = '\0';
    return chars;
}

bool nk_discord_activity_make(NkDiscordActivity *out, const char *title, const char *mode,
                              int64_t start_ms) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (title) {
        /* Leading and trailing spaces would count toward Discord's 2-character minimum. */
        while (*title == ' ') title++;
        size_t len = strlen(title);
        /* A title the library cut to NK_MAX_TITLE_LEN bytes may end inside a character: drop
           that partial character (at most 3 bytes) and keep the rest. */
        for (int k = 0; k < 3 && len > 0 &&
                        !nk_json_validate_utf8((const uint8_t *)title, len); k++) {
            len--;
        }
        while (len > 0 && title[len - 1] == ' ') len--;
        char trimmed[sizeof(out->details)];
        /* Titles hold ':' and '/' ("Fate/Extra"), never a backslash: one would be a
           Windows path, which must not leave the PC. */
        bool path_like = memchr(title, '\\', len) != NULL;
        if (len > 0 && len < sizeof(trimmed) && !path_like &&
            nk_json_validate_utf8((const uint8_t *)title, len)) {
            memcpy(trimmed, title, len);
            trimmed[len] = '\0';
            /* Discord's minimum is 2 characters, not bytes: one CJK character is 3 bytes. */
            if (utf8_copy_chars(out->details, sizeof(out->details), trimmed,
                                NK_DISCORD_TEXT_MAX) < 2) {
                out->details[0] = '\0';
            }
        }
    }
    if (mode && strlen(mode) >= 2 && strlen(mode) < sizeof(out->state)) {
        snprintf(out->state, sizeof(out->state), "%s", mode);
    }
    out->start_ms = start_ms > 0 ? start_ms : 0;
    return out->details[0] != '\0' || out->state[0] != '\0';
}

/* Append to out[cap] at *used; false (and *used unchanged) when it does not fit. */
static bool json_put(char *out, size_t cap, size_t *used, const char *text, size_t len) {
    if (*used + len + 1 > cap) return false;
    memcpy(out + *used, text, len);
    *used += len;
    out[*used] = '\0';
    return true;
}

static bool json_put_str(char *out, size_t cap, size_t *used, const char *text) {
    return json_put(out, cap, used, text, strlen(text));
}

/* A JSON string literal: quotes, backslashes and control characters escaped; UTF-8 kept. */
static bool json_put_string(char *out, size_t cap, size_t *used, const char *text) {
    if (!json_put(out, cap, used, "\"", 1)) return false;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        char esc[8];
        size_t n;
        if (*p == '"' || *p == '\\') {
            esc[0] = '\\';
            esc[1] = (char)*p;
            n = 2;
        } else if (*p < 0x20u) {
            n = (size_t)snprintf(esc, sizeof(esc), "\\u%04x", (unsigned)*p);
        } else {
            esc[0] = (char)*p;
            n = 1;
        }
        if (!json_put(out, cap, used, esc, n)) return false;
    }
    return json_put(out, cap, used, "\"", 1);
}

size_t nk_discord_activity_json(char *out, size_t cap, unsigned long pid,
                                const NkDiscordActivity *activity, unsigned nonce) {
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    size_t used = 0;
    char number[48];
    snprintf(number, sizeof(number), "%lu", pid);
    bool ok = json_put_str(out, cap, &used, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":") &&
              json_put_str(out, cap, &used, number) &&
              json_put_str(out, cap, &used, ",\"activity\":");
    if (ok && !activity) {
        ok = json_put_str(out, cap, &used, "null");
    } else if (ok) {
        bool first = true;
        ok = json_put_str(out, cap, &used, "{");
        if (ok && activity->details[0]) {
            ok = json_put_str(out, cap, &used, "\"details\":") &&
                 json_put_string(out, cap, &used, activity->details);
            first = false;
        }
        if (ok && activity->state[0]) {
            ok = json_put_str(out, cap, &used, first ? "\"state\":" : ",\"state\":") &&
                 json_put_string(out, cap, &used, activity->state);
            first = false;
        }
        if (ok && activity->start_ms > 0) {
            snprintf(number, sizeof(number), "%lld", (long long)activity->start_ms);
            ok = json_put_str(out, cap, &used,
                              first ? "\"timestamps\":{\"start\":" : ",\"timestamps\":{\"start\":") &&
                 json_put_str(out, cap, &used, number) && json_put_str(out, cap, &used, "}");
        }
        ok = ok && json_put_str(out, cap, &used, "}");
    }
    snprintf(number, sizeof(number), "%u", nonce);
    ok = ok && json_put_str(out, cap, &used, "},\"nonce\":\"") &&
         json_put_str(out, cap, &used, number) && json_put_str(out, cap, &used, "\"}");
    if (!ok) {
        out[0] = '\0';
        return 0;
    }
    return used;
}

static void put_le32(unsigned char *out, uint32_t value) {
    out[0] = (unsigned char)(value & 0xFFu);
    out[1] = (unsigned char)((value >> 8) & 0xFFu);
    out[2] = (unsigned char)((value >> 16) & 0xFFu);
    out[3] = (unsigned char)((value >> 24) & 0xFFu);
}

static uint32_t get_le32(const unsigned char *in) {
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) |
           ((uint32_t)in[3] << 24);
}

size_t nk_discord_frame(unsigned char *out, size_t cap, uint32_t opcode, const void *payload,
                        size_t len) {
    if (!out || (len > 0 && !payload) || len > 0xFFFFFFFFu - 8u || cap < 8u || len > cap - 8u) {
        return 0;
    }
    put_le32(out, opcode);
    put_le32(out + 4, (uint32_t)len);
    if (len > 0) memcpy(out + 8, payload, len);
    return len + 8u;
}

/* The default transport: the local pipe. */
static bool pipe_open(void *ctx, const char *name) {
    return nk_platform_local_pipe_open((NkLocalPipe *)ctx, name);
}

static bool pipe_write(void *ctx, const void *data, size_t len) {
    return nk_platform_local_pipe_write((NkLocalPipe *)ctx, data, len,
                                        NK_DISCORD_WRITE_TIMEOUT_MS);
}

static long pipe_read(void *ctx, void *buf, size_t cap) {
    return nk_platform_local_pipe_read((NkLocalPipe *)ctx, buf, cap);
}

static void pipe_close(void *ctx) {
    nk_platform_local_pipe_close((NkLocalPipe *)ctx);
}

void nk_discord_client_init(NkDiscordClient *client, const char *application_id,
                            const NkDiscordTransport *transport, const char *pipe_prefix) {
    if (!client) return;
    memset(client, 0, sizeof(*client));
    if (nk_discord_application_id_valid(application_id)) {
        snprintf(client->application_id, sizeof(client->application_id), "%s", application_id);
    }
    if (transport) {
        client->transport = *transport;
    } else {
        client->transport.open = pipe_open;
        client->transport.write = pipe_write;
        client->transport.read = pipe_read;
        client->transport.close = pipe_close;
        client->transport.ctx = &client->pipe;
    }
    snprintf(client->pipe_prefix, sizeof(client->pipe_prefix), "%s",
             pipe_prefix ? pipe_prefix : NK_DISCORD_PIPE_PREFIX);
    client->pid = nk_platform_process_id();
}

static void client_close(NkDiscordClient *client) {
    if (client->link != NK_DISCORD_CLOSED && client->transport.close) {
        client->transport.close(client->transport.ctx);
    }
    client->link = NK_DISCORD_CLOSED;
    client->shown = false;
    client->rx_used = 0;
}

static bool client_send(NkDiscordClient *client, uint32_t opcode, const void *payload,
                        size_t len) {
    unsigned char frame[NK_DISCORD_RX_MAX];
    size_t size = nk_discord_frame(frame, sizeof(frame), opcode, payload, len);
    if (size == 0 || !client->transport.write ||
        !client->transport.write(client->transport.ctx, frame, size)) {
        client_close(client);
        return false;
    }
    return true;
}

/* One attempt: each pipe name in turn until one opens, then the handshake. */
static void client_connect(NkDiscordClient *client, uint64_t now_ms) {
    client->attempted = true;
    client->attempts++;
    client->next_attempt_ms = now_ms + NK_DISCORD_RETRY_MS;
    if (!client->transport.open) return;
    for (int i = 0; i < NK_DISCORD_PIPE_COUNT; i++) {
        char name[64];
        snprintf(name, sizeof(name), "%s%d", client->pipe_prefix, i);
        if (client->transport.open(client->transport.ctx, name)) {
            char hello[96];
            int n = snprintf(hello, sizeof(hello), "{\"v\":1,\"client_id\":\"%s\"}",
                             client->application_id);
            client->link = NK_DISCORD_WAITING_READY;
            client->handshake_deadline_ms = now_ms + NK_DISCORD_HANDSHAKE_TIMEOUT_MS;
            if (n > 0 && (size_t)n < sizeof(hello)) {
                (void)client_send(client, NK_DISCORD_OP_HANDSHAKE, hello, (size_t)n);
            } else {
                client_close(client);
            }
            return;
        }
    }
}

/* A FRAME's JSON: READY completes the handshake; an ERROR keeps its code. Nothing else in
   it (READY names the signed-in account) is read or kept. */
static void client_frame(NkDiscordClient *client, const unsigned char *payload, size_t len) {
    NkJsonNode *root = nk_json_parse((const char *)payload, len, NULL, 0);
    if (!root) return;
    const char *evt = nk_json_get_string(nk_json_obj_get(root, "evt"));
    if (evt && strcmp(evt, "READY") == 0 && client->link == NK_DISCORD_WAITING_READY) {
        client->link = NK_DISCORD_READY;
    } else if (evt && strcmp(evt, "ERROR") == 0) {
        int64_t code = 0;
        client->last_error_code =
            nk_json_get_int64(nk_json_obj_get(nk_json_obj_get(root, "data"), "code"), &code)
                ? (int)code : -1;
    }
    nk_json_free(root);
}

/* Act on each whole frame that has arrived, reading more as needed. One call handles at most
   NK_DISCORD_FRAMES_PER_UPDATE frames and reads at most one buffer's worth of bytes, so a peer
   that writes without pause cannot hold the launcher's frame; the rest waits for the next
   update. */
static void client_pump(NkDiscordClient *client) {
    unsigned frames = 0;
    size_t read_total = 0;
    while (client->link != NK_DISCORD_CLOSED && client->transport.read) {
        while (client->link != NK_DISCORD_CLOSED && client->rx_used >= 8u) {
            uint32_t opcode = get_le32(client->rx);
            uint32_t len = get_le32(client->rx + 4);
            if (len > sizeof(client->rx) - 8u) {
                client_close(client);
                return;
            }
            if (client->rx_used < 8u + len) break;
            if (frames >= NK_DISCORD_FRAMES_PER_UPDATE) return;
            frames++;
            const unsigned char *payload = client->rx + 8;
            if (opcode == NK_DISCORD_OP_FRAME) {
                client_frame(client, payload, len);
            } else if (opcode == NK_DISCORD_OP_PING) {
                unsigned char echo[NK_DISCORD_RX_MAX];
                memcpy(echo, payload, len);
                bool sent = client_send(client, NK_DISCORD_OP_PONG, echo, len);
                memset(echo, 0, len);
                if (!sent) return;
            } else if (opcode == NK_DISCORD_OP_CLOSE) {
                client_close(client);
                return;
            }
            size_t consumed = 8u + len;
            memmove(client->rx, client->rx + consumed, client->rx_used - consumed);
            client->rx_used -= consumed;
            /* The frame's bytes (READY names the account) are cleared once handled. */
            memset(client->rx + client->rx_used, 0, consumed);
        }
        if (client->link == NK_DISCORD_CLOSED || read_total >= sizeof(client->rx)) return;
        if (client->rx_used >= sizeof(client->rx)) {
            client_close(client);
            return;
        }
        long got = client->transport.read(client->transport.ctx, client->rx + client->rx_used,
                                          sizeof(client->rx) - client->rx_used);
        if (got < 0) {
            client_close(client);
            return;
        }
        if (got == 0) return;
        client->rx_used += (size_t)got;
        read_total += (size_t)got;
    }
}

static bool activity_same(const NkDiscordActivity *a, const NkDiscordActivity *b) {
    return strcmp(a->details, b->details) == 0 && strcmp(a->state, b->state) == 0 &&
           a->start_ms == b->start_ms;
}

static void client_set(NkDiscordClient *client, const NkDiscordActivity *activity) {
    char json[NK_DISCORD_RX_MAX - 8];
    size_t len = nk_discord_activity_json(json, sizeof(json), client->pid, activity,
                                          ++client->nonce);
    if (len == 0) return;
    if (!client_send(client, NK_DISCORD_OP_FRAME, json, len)) return;
    if (activity) {
        client->sent = *activity;
        client->shown = true;
    } else {
        client->shown = false;
    }
}

void nk_discord_client_update(NkDiscordClient *client, const NkDiscordActivity *activity,
                              uint64_t now_ms) {
    if (!client || !client->application_id[0]) return;
    if (!activity) {
        /* Nothing to show: clear what Discord shows. An open connection stays for the next
           game, so it shows at once instead of after the retry interval. */
        if (client->link == NK_DISCORD_CLOSED) return;
        client_pump(client);
        if (client->link == NK_DISCORD_WAITING_READY &&
            now_ms >= client->handshake_deadline_ms) {
            client_close(client);
        } else if (client->link == NK_DISCORD_READY && client->shown) {
            client_set(client, NULL);
        }
        return;
    }
    if (client->link == NK_DISCORD_CLOSED) {
        if (client->attempted && now_ms < client->next_attempt_ms) return;
        client_connect(client, now_ms);
        if (client->link == NK_DISCORD_CLOSED) return;
    }
    client_pump(client);
    if (client->link == NK_DISCORD_WAITING_READY && now_ms >= client->handshake_deadline_ms) {
        client_close(client);
        return;
    }
    if (client->link == NK_DISCORD_READY &&
        (!client->shown || !activity_same(&client->sent, activity))) {
        client_set(client, activity);
    }
}

void nk_discord_client_shutdown(NkDiscordClient *client) {
    if (!client || client->link == NK_DISCORD_CLOSED) return;
    if (client->link == NK_DISCORD_READY && client->shown) client_set(client, NULL);
    client_close(client);
}

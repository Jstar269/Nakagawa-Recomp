/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2013- PPSSPP Project (the port offset), (c) 2014- PPSSPP Project (the login checks)
 * Copyright (C) 2026 the Nakagawa Recomp authors
 *
 * The ad hoc relay's wire format (#1139): see nk_adhoc_relay.h, which says what each part was
 * written from. The login checks (nk_relay_product_ok, nk_relay_mac_ok, nk_relay_nickname_ok and
 * the checks in nk_lobby_login) are translated from login_user_data in PPSSPP's
 * Core/HLE/proAdhocServer.cpp, and the port offset from offset_port_simple and reverse_port_simple
 * in Core/HLE/sceNetAdhoc.cpp, both at 3bc60270eae3069f13dfbfdb7292a7e1375de6f6
 * (GPL-2.0-or-later). Label: derived-translated (docs/provenance/INDEPENDENCE_MODEL.md). This file
 * is conveyed as GPL-3.0-or-later.
 *
 * Modified for this project (2026-11-12; see src/core/PROVENANCE.md):
 *   - C99. The checks take C strings where PPSSPP reads fixed packet fields, so a product code
 *     must end after its nine bytes and a nickname within its 128 bytes; a longer one does not fit
 *     the field and is refused. Inside the field the rules are PPSSPP's: nine characters A-Z or
 *     0-9, and a nickname whose first byte is not zero, terminated or not.
 *   - The port offset is passed in rather than read from settings, and a relay port of 0 comes
 *     back as 0, as port 0 goes out as 0 (PPSSPP's reverse_port_simple subtracts the offset).
 *   - nk_relay_nickname_field_ok checks a raw 128-byte field without reading past it.
 * All the login checks (nk_relay_product_ok, nk_relay_mac_ok, nk_relay_nickname_ok and the checks
 * in nk_lobby_login) are written from PPSSPP's login_user_data; none is from aemu_postoffice.
 * Provenance note: reviews of pull request #2549 found that an earlier version of this file had
 * carried them over, with small edits, from its 2026-11-06 version, which followed
 * aemu_postoffice, so on 2026-11-12 each was deleted and written again from login_user_data,
 * nk_relay_product_ok last. The session that
 * wrote nk_relay_product_ok again opened no aemu_postoffice source; it had read the old
 * product-check body earlier, while checking src/core/PROVENANCE.md's wording, and wrote the new
 * one from login_user_data without opening the old one again.
 *
 * How the codec is used: src/core/nk_adhoc_relay_client.c reads and writes the relay's TCP streams
 * with it, and src/rt/hle_network.c consumes what it decodes.
 *   - nk_lobby_parse checks only a packet's opcode and length. A CONNECT from a hostile server may
 *     carry the all-ones, all-zero or a group MAC, the console's own MAC, an empty nickname or a
 *     repeated peer id, a DISCONNECT an unknown id, and a SCAN a group name with any bytes and no
 *     terminator; each is decoded as it is. hle_network.c (adhoc_upsert_peer) drops a CONNECT with
 *     a zero, group or own MAC, and a repeated peer id replaces its older holder.
 *   - sceNetAdhocctlInit accepts a lower-case product code, as a PSP does; the HLE layer upper-cases
 *     it before nk_relay_product_ok and refuses by name one that still fails. The HLE layer sends
 *     at most 127 bytes of the nickname, which the server's 128-byte rule accepts.
 *   - An inbound relay port of 0 or of the offset itself (10000 by default) comes back as guest
 *     port 0. A peer that follows the protocol never sends the second, since only port 0 maps
 *     there and it goes out as 0; the relay client passes a 0 on as it is.
 *   - The codec keeps no state. The relay client buffers each TCP stream (the lobby and each data
 *     stream) up to 512 KiB (NK_RELAY_CHAN_MAX_BUF) and decodes whole packets from that buffer.
 *     There is no timeout: a packet a peer leaves unfinished keeps that stream waiting for the
 *     rest, which the game sees as no data on that ad hoc socket, or as no lobby event. A lobby
 *     packet with an unknown opcode, or a PDP or PTP length above the protocol's largest (10240
 *     and 51200 bytes), ends that stream, and the relay client's last error names it. */

#include "nk_adhoc_relay.h"

#include <string.h>

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* The login checks, written from login_user_data in PPSSPP's Core/HLE/proAdhocServer.cpp at
 * 3bc60270. The server accepts a LOGIN packet when all three hold,
 * checked in this order: each of the nine product-code bytes is A-Z or 0-9; the MAC is neither
 * FF:FF:FF:FF:FF:FF nor 00:00:00:00:00:00; the nickname's first byte is not zero. It refuses
 * anything else. It checks nothing more of the nickname: all 128 bytes of the field may be text,
 * with no terminator. This API takes C strings, so a product code or nickname must also end
 * within its field (nine and 128 bytes): a longer one cannot be sent. */

bool nk_relay_product_ok(const char *product) {
    if (product == NULL) return false;
    /* login_user_data: "Product Code Check", proAdhocServer.cpp:561-569 at 3bc60270. */
    int valid_product_code = 1;
    for (int i = 0; i < (int)NK_RELAY_PRODUCT_LEN && valid_product_code == 1; i++) {
        /* Valid characters; a terminator inside the nine fails here, before the next byte. */
        if (!((product[i] >= 'A' && product[i] <= 'Z') || (product[i] >= '0' && product[i] <= '9')))
            valid_product_code = 0;
    }
    /* The C-string rule (this project's): the code ends right after its nine characters. */
    return valid_product_code == 1 && product[NK_RELAY_PRODUCT_LEN] == '\0';
}

bool nk_relay_mac_ok(const uint8_t mac[NK_RELAY_MAC_LEN]) {
    static const uint8_t broadcast[NK_RELAY_MAC_LEN] = {0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu};
    static const uint8_t none[NK_RELAY_MAC_LEN] = {0u, 0u, 0u, 0u, 0u, 0u};
    if (!mac) return false;
    return memcmp(mac, broadcast, NK_RELAY_MAC_LEN) != 0 && memcmp(mac, none, NK_RELAY_MAC_LEN) != 0;
}

bool nk_relay_nickname_ok(const char *nickname) {
    if (!nickname || nickname[0] == '\0') return false;
    for (size_t i = 1; i <= NK_RELAY_NICKNAME_LEN; i++)
        if (nickname[i] == '\0') return true;
    return false;
}

bool nk_relay_nickname_field_ok(const uint8_t field[NK_RELAY_NICKNAME_LEN]) {
    return field != NULL && field[0] != 0u;
}

size_t nk_lobby_login(uint8_t out[NK_LOBBY_LOGIN_SIZE], const uint8_t mac[NK_RELAY_MAC_LEN],
                      const char *nickname, const char *product) {
    if (!out || !nk_relay_product_ok(product) || !nk_relay_mac_ok(mac) || !nk_relay_nickname_ok(nickname))
        return 0u;
    uint8_t *const name_field = out + 1u + NK_RELAY_MAC_LEN;
    out[0] = NK_LOBBY_LOGIN;
    memcpy(out + 1u, mac, NK_RELAY_MAC_LEN);
    memset(name_field, 0, NK_RELAY_NICKNAME_LEN);
    memcpy(name_field, nickname, strlen(nickname)); /* at most NK_RELAY_NICKNAME_LEN, checked */
    memcpy(name_field + NK_RELAY_NICKNAME_LEN, product, NK_RELAY_PRODUCT_LEN);
    return NK_LOBBY_LOGIN_SIZE;
}

size_t nk_lobby_connect(uint8_t out[NK_LOBBY_CONNECT_SIZE],
                        const uint8_t group[NK_RELAY_GROUP_LEN]) {
    if (!out || !group) return 0u;
    out[0] = NK_LOBBY_CONNECT;
    memcpy(out + 1, group, NK_RELAY_GROUP_LEN);
    return NK_LOBBY_CONNECT_SIZE;
}

size_t nk_lobby_bare(uint8_t out[NK_LOBBY_BARE_SIZE], NkLobbyOpcode opcode) {
    if (!out) return 0u;
    if (opcode != NK_LOBBY_PING && opcode != NK_LOBBY_SCAN && opcode != NK_LOBBY_DISCONNECT)
        return 0u;
    out[0] = (uint8_t)opcode;
    return NK_LOBBY_BARE_SIZE;
}

/* The whole 128-byte field, which a server may send with no terminator, then one. */
static void copy_name(char dst[NK_RELAY_NICKNAME_LEN + 1u], const uint8_t *src) {
    memcpy(dst, src, NK_RELAY_NICKNAME_LEN);
    dst[NK_RELAY_NICKNAME_LEN] = '\0';
}

int nk_lobby_parse(const uint8_t *buf, size_t len, NkLobbyEvent *ev) {
    if (!buf || !ev) return NK_LOBBY_BAD;
    if (len == 0u) return NK_LOBBY_NEED_MORE;
    size_t need;
    switch (buf[0]) {
    case NK_LOBBY_SCAN: need = NK_LOBBY_S2C_SCAN_SIZE; break;
    case NK_LOBBY_SCAN_COMPLETE: need = NK_LOBBY_S2C_SCAN_COMPLETE_SIZE; break;
    case NK_LOBBY_CONNECT: need = NK_LOBBY_S2C_CONNECT_SIZE; break;
    case NK_LOBBY_DISCONNECT: need = NK_LOBBY_S2C_DISCONNECT_SIZE; break;
    case NK_LOBBY_CONNECT_BSSID: need = NK_LOBBY_S2C_CONNECT_BSSID_SIZE; break;
    case NK_LOBBY_CHAT: need = NK_LOBBY_S2C_CHAT_SIZE; break;
    default: return NK_LOBBY_BAD; /* PING, LOGIN and unknown opcodes never come from a server */
    }
    if (len < need) return NK_LOBBY_NEED_MORE;
    memset(ev, 0, sizeof *ev);
    ev->kind = (NkLobbyOpcode)buf[0];
    switch (buf[0]) {
    case NK_LOBBY_SCAN:
        memcpy(ev->group, buf + 1, NK_RELAY_GROUP_LEN);
        memcpy(ev->bssid, buf + 1 + NK_RELAY_GROUP_LEN, NK_RELAY_MAC_LEN);
        break;
    case NK_LOBBY_CONNECT:
        copy_name(ev->nickname, buf + 1);
        memcpy(ev->mac, buf + 1 + NK_RELAY_NICKNAME_LEN, NK_RELAY_MAC_LEN);
        ev->peer_id = get32(buf + 1 + NK_RELAY_NICKNAME_LEN + NK_RELAY_MAC_LEN);
        break;
    case NK_LOBBY_DISCONNECT:
        ev->peer_id = get32(buf + 1);
        break;
    case NK_LOBBY_CONNECT_BSSID:
        memcpy(ev->bssid, buf + 1, NK_RELAY_MAC_LEN);
        break;
    case NK_LOBBY_CHAT:
        copy_name(ev->nickname, buf + 1 + 64);
        break;
    default:
        break;
    }
    return (int)need;
}

size_t nk_relay_init(uint8_t out[NK_RELAY_INIT_SIZE], NkRelayInit type,
                     const uint8_t src[NK_RELAY_MAC_LEN], uint16_t sport,
                     const uint8_t *dst, uint16_t dport) {
    if (!out || !src) return 0u;
    switch (type) {
    case NK_RELAY_INIT_PDP:
    case NK_RELAY_INIT_PTP_LISTEN:
        break;
    case NK_RELAY_INIT_PTP_CONNECT:
    case NK_RELAY_INIT_PTP_ACCEPT:
        if (!dst) return 0u;
        break;
    default:
        return 0u;
    }
    memset(out, 0, NK_RELAY_INIT_SIZE);
    put32(out, (uint32_t)type);
    memcpy(out + 4, src, NK_RELAY_MAC_LEN);
    put16(out + 12, sport);
    if (type == NK_RELAY_INIT_PTP_CONNECT || type == NK_RELAY_INIT_PTP_ACCEPT) {
        memcpy(out + 14, dst, NK_RELAY_MAC_LEN);
        put16(out + 22, dport);
    }
    return NK_RELAY_INIT_SIZE;
}

size_t nk_relay_pdp_header(uint8_t out[NK_RELAY_PDP_HEADER_SIZE],
                           const uint8_t mac[NK_RELAY_MAC_LEN], uint16_t port, uint32_t size) {
    if (!out || !mac || size > NK_RELAY_PDP_MAX) return 0u;
    memset(out, 0, NK_RELAY_PDP_HEADER_SIZE);
    memcpy(out, mac, NK_RELAY_MAC_LEN);
    put16(out + 8, port);
    put32(out + 10, size);
    return NK_RELAY_PDP_HEADER_SIZE;
}

bool nk_relay_parse_pdp_header(const uint8_t in[NK_RELAY_PDP_HEADER_SIZE],
                               uint8_t mac[NK_RELAY_MAC_LEN], uint16_t *port, uint32_t *size) {
    if (!in || !mac || !port || !size) return false;
    const uint32_t n = get32(in + 10);
    if (n > NK_RELAY_PDP_MAX) return false;
    memcpy(mac, in, NK_RELAY_MAC_LEN);
    *port = get16(in + 8);
    *size = n;
    return true;
}

bool nk_relay_parse_ptp_connect(const uint8_t in[NK_RELAY_PTP_CONNECT_SIZE],
                                uint8_t mac[NK_RELAY_MAC_LEN], uint16_t *port) {
    if (!in || !mac || !port) return false;
    memcpy(mac, in, NK_RELAY_MAC_LEN);
    *port = get16(in + 8);
    return true;
}

size_t nk_relay_ptp_data_header(uint8_t out[NK_RELAY_PTP_DATA_HEADER_SIZE], uint32_t size) {
    if (!out || size > NK_RELAY_PTP_MAX) return 0u;
    put32(out, size);
    return NK_RELAY_PTP_DATA_HEADER_SIZE;
}

bool nk_relay_parse_ptp_data_header(const uint8_t in[NK_RELAY_PTP_DATA_HEADER_SIZE],
                                    uint32_t *size) {
    if (!in || !size) return false;
    const uint32_t n = get32(in);
    if (n > NK_RELAY_PTP_MAX) return false;
    *size = n;
    return true;
}

/* Translated from PPSSPP's offset_port_simple and reverse_port_simple (Core/HLE/sceNetAdhoc.cpp
 * at 3bc60270eae3069f13dfbfdb7292a7e1375de6f6, GPL-2.0-or-later, Copyright (c) 2013- PPSSPP
 * Project); modified for this project: C99, the offset passed in rather than read from the
 * settings, and port 0 coming back as 0. */
uint16_t nk_relay_port_out(uint16_t port, uint16_t offset) {
    if (port == 0u) return 0u;
    const uint16_t shifted = (uint16_t)(port + offset);
    return shifted == 0u ? 65535u : shifted;
}

uint16_t nk_relay_port_in(uint16_t port, uint16_t offset) {
    if (port == 0u) return 0u; /* port 0 went out as 0; this project's addition */
    return (uint16_t)(port - offset);
}

/* The shipped servers (maintainer decisions of 2026-11-06 and 2026-11-19 on #1139): socom.cc,
 * PPSSPP's default aemu_postoffice server, is the default; the four ArenaAnywhere relays are
 * presets. All five are listed with "data_mode": "AemuPostoffice" in PPSSPP's
 * assets/adhoc-servers.json at 3bc60270eae3069f13dfbfdb7292a7e1375de6f6, and the labels give the
 * places that list names. Another preset is another host the player may contact: it needs its
 * own maintainer decision in that issue, a row under "Network use" in docs/SETUP.md and a line in
 * tools/test_prereq_fetcher.py (AGENTS.md section 5). */
static const NkAdhocServerPreset s_server_presets[] = {
    {"socom.cc", "Socom Adhoc Server (France)"},
    {"relay.arenaanywhere.site", "ArenaAnywhere (Europe)"},
    {"relay-usa.arenaanywhere.site", "ArenaAnywhere (US)"},
    {"relay-sa.arenaanywhere.site", "ArenaAnywhere (South Africa)"},
    {"relay-asia.arenaanywhere.site", "ArenaAnywhere (Singapore)"},
};

size_t nk_adhoc_server_preset_count(void) {
    return sizeof(s_server_presets) / sizeof(s_server_presets[0]);
}

const NkAdhocServerPreset *nk_adhoc_server_preset(size_t index) {
    if (index >= nk_adhoc_server_preset_count()) return NULL;
    return &s_server_presets[index];
}

static bool parse_u16_port(const char *s, const char *end, uint16_t *out) {
    if (!s || s >= end) return false;
    uint32_t v = 0u;
    for (const char *p = s; p < end; p++) {
        if (*p < '0' || *p > '9') return false;
        v = v * 10u + (uint32_t)(*p - '0');
        if (v > 65535u) return false;
    }
    if (v == 0u) return false;
    if (out) *out = (uint16_t)v;
    return true;
}

static bool host_char_ok(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '.';
}

/* The host check on host[0..len), which need not be terminated. */
static bool host_span_ok(const char *host, size_t len) {
    if (!host || len == 0u || len > NK_ADHOC_SERVER_MAX_LEN) return false;
    size_t label_start = 0u;
    size_t labels = 0u;
    bool last_all_digits = false;
    for (size_t i = 0u; i <= len; i++) {
        if (i < len && host[i] != '.') {
            if (!host_char_ok((unsigned char)host[i])) return false;
            continue;
        }
        const size_t label_len = i - label_start;
        if (label_len == 0u || label_len > 63u) return false;
        if (host[label_start] == '-' || host[i - 1u] == '-') return false;
        last_all_digits = true;
        for (size_t j = label_start; j < i; j++) {
            if (host[j] < '0' || host[j] > '9') last_all_digits = false;
        }
        labels++;
        label_start = i + 1u;
    }
    if (!last_all_digits) return true;
    /* A last label of digits only is an IPv4 literal: exactly four parts, each 0-255, no
       leading zero, so "1.2.3" and "01.2.3.4" are never read as some other address. */
    if (labels != 4u) return false;
    size_t start = 0u;
    for (size_t i = 0u; i <= len; i++) {
        if (i < len && host[i] != '.') continue;
        const size_t part = i - start;
        unsigned value = 0u;
        if (part > 3u || (part > 1u && host[start] == '0')) return false;
        for (size_t j = start; j < i; j++) {
            if (host[j] < '0' || host[j] > '9') return false;
            value = value * 10u + (unsigned)(host[j] - '0');
        }
        if (value > 255u) return false;
        start = i + 1u;
    }
    return true;
}

bool nk_adhoc_relay_host_ok(const char *host) {
    if (!host) return false;
    size_t len = 0u;
    while (len <= NK_ADHOC_SERVER_MAX_LEN && host[len]) len++;
    return host_span_ok(host, len);
}

bool nk_adhoc_relay_parse_server(const char *spec, char *host_out, size_t host_cap,
                                 uint16_t *lobby_port, uint16_t *data_port) {
    if (host_out && host_cap > 0u) host_out[0] = '\0';
    if (!spec) return false;
    const size_t total = strlen(spec);
    if (total == 0u || total > NK_ADHOC_SERVER_MAX_LEN) return false;

    const char *colon = strchr(spec, ':');
    const size_t host_len = colon ? (size_t)(colon - spec) : total;
    if (host_len == 0u || (host_out && host_len >= host_cap)) return false;
    if (!host_span_ok(spec, host_len)) return false;

    uint16_t lport = NK_RELAY_LOBBY_PORT;
    uint16_t dport = NK_RELAY_DATA_PORT;
    if (colon) {
        const char *pstr = colon + 1;
        const char *comma = strchr(pstr, ',');
        const char *end = spec + total;
        if (comma) {
            if (!parse_u16_port(pstr, comma, &lport) ||
                !parse_u16_port(comma + 1, end, &dport))
                return false;
        } else {
            if (!parse_u16_port(pstr, end, &lport)) return false;
        }
    }

    if (host_out && host_cap > 0u) {
        memcpy(host_out, spec, host_len);
        host_out[host_len] = '\0';
    }
    if (lobby_port) *lobby_port = lport;
    if (data_port) *data_port = dport;
    return true;
}

/* Whether s[i..] starts a Unicode bidirectional control in UTF-8: U+061C, U+200E/F,
   U+202A-202E, U+2066-2069. */
static bool server_bidi_at(const unsigned char *s, size_t i, size_t len) {
    if (i + 1u < len && s[i] == 0xD8u && s[i + 1u] == 0x9Cu) return true;
    if (i + 2u >= len || s[i] != 0xE2u) return false;
    if (s[i + 1u] == 0x80u) {
        return s[i + 2u] == 0x8Eu || s[i + 2u] == 0x8Fu || (s[i + 2u] >= 0xAAu && s[i + 2u] <= 0xAEu);
    }
    return s[i + 1u] == 0x81u && s[i + 2u] >= 0xA6u && s[i + 2u] <= 0xA9u;
}

const char *nk_adhoc_relay_server_problem(const char *spec) {
    if (nk_adhoc_relay_parse_server(spec, NULL, 0u, NULL, NULL)) return NULL;
    if (!spec || !spec[0]) return "it is empty";
    const unsigned char *s = (const unsigned char *)spec;
    const size_t len = strlen(spec);
    for (size_t i = 0u; i < len; i++) {
        if (server_bidi_at(s, i, len)) return "it has a bidirectional control character";
    }
    for (size_t i = 0u; i < len; i++) {
        if (s[i] < 0x20u || s[i] == 0x7Fu) return "it has a control character";
    }
    for (size_t i = 0u; i < len; i++) {
        if (s[i] >= 0x80u) return "it has a letter outside A to Z (only A-Z, 0-9, '-' and '.')";
    }
    if (strchr(spec, ' ')) return "it has a space";
    if (strstr(spec, "://")) return "it starts with a scheme such as http://: enter the host only";
    if (strchr(spec, '/')) return "it has a path (a '/'): enter the host only";
    if (len > NK_ADHOC_SERVER_MAX_LEN) return "it is longer than 127 characters";
    const char *colon = strchr(spec, ':');
    const size_t host_len = colon ? (size_t)(colon - spec) : len;
    if (!host_span_ok(spec, host_len)) {
        return "the host is not a host name or IPv4 address (letters, digits, '-' and '.')";
    }
    return "a port is not a whole number from 1 to 65535 (host:lobby_port or "
           "host:lobby_port,data_port)";
}

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
 *   - The port offset is passed in rather than read from settings.
 * The login checks were first carried over, with small edits, from an earlier version of this
 * file; a review of pull request #2549 found that one of them (a nickname of 128 bytes refused)
 * was not PPSSPP's rule, and on 2026-11-12 nk_relay_mac_ok and the checks in nk_lobby_login were
 * deleted and written again from login_user_data. nk_relay_product_ok was kept: its body is
 * unchanged apart from comments, and its behaviour is PPSSPP's product-code rule, the check in
 * login_user_data that each of the nine bytes of the product code is A-Z or 0-9. */

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

/* The login checks, rewritten on 2026-11-12 from login_user_data in PPSSPP's
 * Core/HLE/proAdhocServer.cpp at 3bc60270. The server accepts a LOGIN packet when all three hold,
 * checked in this order: each of the nine product-code bytes is A-Z or 0-9; the MAC is neither
 * FF:FF:FF:FF:FF:FF nor 00:00:00:00:00:00; the nickname's first byte is not zero. It refuses
 * anything else. It checks nothing more of the nickname: all 128 bytes of the field may be text,
 * with no terminator. This API takes C strings, so a product code or nickname must also end
 * within its field (nine and 128 bytes): a longer one cannot be sent. */

bool nk_relay_product_ok(const char *product) {
    if (!product) return false;
    for (size_t i = 0; i < NK_RELAY_PRODUCT_LEN; i++) {
        const char ch = product[i]; /* a terminator before the ninth byte fails here */
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))) return false;
    }
    return product[NK_RELAY_PRODUCT_LEN] == '\0';
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
 * settings. */
uint16_t nk_relay_port_out(uint16_t port, uint16_t offset) {
    if (port == 0u) return 0u;
    const uint16_t shifted = (uint16_t)(port + offset);
    return shifted == 0u ? 65535u : shifted;
}

uint16_t nk_relay_port_in(uint16_t port, uint16_t offset) {
    return (uint16_t)(port - offset);
}

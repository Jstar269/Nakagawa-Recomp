/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2013- PPSSPP Project (the port offset), (c) 2014- PPSSPP Project (the login checks)
 * Copyright (C) 2026 the Nakagawa Recomp authors
 *
 * The ad hoc relay's wire format (#1139): see nk_adhoc_relay.h, which says what each part was
 * written from. The login checks and the port offset are translated from PPSSPP at
 * 3bc60270eae3069f13dfbfdb7292a7e1375de6f6 (GPL-2.0-or-later), modified for this project
 * (2026-11-12): C99, the offset passed in. Label: derived-translated
 * (docs/provenance/INDEPENDENCE_MODEL.md). This file is conveyed as GPL-3.0-or-later. */

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

/* The login checks of PPSSPP's ad hoc server (Core/HLE/proAdhocServer.cpp login_user_data at
 * 3bc60270, GPL-2.0-or-later): it refuses the all-zero and the all-ones MAC, a product code with a
 * character outside A-Z and 0-9, and an empty nickname. */
bool nk_relay_mac_ok(const uint8_t mac[NK_RELAY_MAC_LEN]) {
    static const uint8_t k_zero[NK_RELAY_MAC_LEN] = {0, 0, 0, 0, 0, 0};
    static const uint8_t k_ones[NK_RELAY_MAC_LEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    return mac && memcmp(mac, k_zero, NK_RELAY_MAC_LEN) != 0 && memcmp(mac, k_ones, NK_RELAY_MAC_LEN) != 0;
}

bool nk_relay_product_ok(const char *product) {
    if (!product) return false;
    for (size_t i = 0; i < NK_RELAY_PRODUCT_LEN; i++) {
        const char ch = product[i];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))) return false;
    }
    return product[NK_RELAY_PRODUCT_LEN] == '\0'; /* the field holds nine characters, no more */
}

size_t nk_lobby_login(uint8_t out[NK_LOBBY_LOGIN_SIZE], const uint8_t mac[NK_RELAY_MAC_LEN],
                      const char *nickname, const char *product) {
    if (!out || !nickname || !nk_relay_mac_ok(mac) || !nk_relay_product_ok(product)) return 0u;
    size_t name_len = 0;
    while (name_len < NK_RELAY_NICKNAME_LEN && nickname[name_len] != '\0') name_len++;
    if (name_len == 0u || name_len == NK_RELAY_NICKNAME_LEN) return 0u;
    memset(out, 0, NK_LOBBY_LOGIN_SIZE);
    out[0] = NK_LOBBY_LOGIN;
    memcpy(out + 1, mac, NK_RELAY_MAC_LEN);
    memcpy(out + 7, nickname, name_len);
    memcpy(out + 7 + NK_RELAY_NICKNAME_LEN, product, NK_RELAY_PRODUCT_LEN);
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

static void copy_name(char dst[NK_RELAY_NICKNAME_LEN], const uint8_t *src) {
    memcpy(dst, src, NK_RELAY_NICKNAME_LEN - 1u);
    dst[NK_RELAY_NICKNAME_LEN - 1u] = '\0';
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
        memcpy(ev->mac, buf + 1 + NK_RELAY_GROUP_LEN, NK_RELAY_MAC_LEN);
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
        memcpy(ev->mac, buf + 1, NK_RELAY_MAC_LEN);
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

/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_ADHOC_RELAY_H
#define NK_ADHOC_RELAY_H

/* The wire format of the ad hoc relay (#1139): the protocol PPSSPP's online ad hoc play speaks, so
 * Nakagawa players can meet PPSSPP players and PSPs running the aemu plugin on the same servers.
 *
 * Two channels, both plain TCP with no encryption and no authentication, on one server:
 *   - the lobby ("adhocctl", port 27312): log in, find and join groups, learn who is in them;
 *   - the data relay ("postoffice", port 27313): one connection per ad hoc socket, carrying the
 *     game's own PDP datagrams and PTP streams between consoles.
 *
 * This file is project-authored. It speaks the protocol of Kethen/aemu_postoffice at commit
 * 02abf18fd161a1639d9f93c48c4c46638c7c6d15 (the commit PPSSPP 3bc60270 pins), written from that
 * project's design.md and its packed packet layouts (aemu_postoffice_packets.h,
 * adhocctl/packets_v1.h, adhocctl/pspstructs.h); no code is copied. Every multi-byte field is
 * little-endian. It encodes and decodes bytes only: it opens no connection and holds no host
 * name. The owner's decision on #1139 (2026-11-06) allows exactly these fields to leave the PC:
 * the disc ID, the nickname, the console profile's random MAC and the group name in the lobby,
 * and the game's own ad hoc packets with their MAC and port addressing in the relay. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NK_RELAY_LOBBY_PORT 27312u
#define NK_RELAY_DATA_PORT 27313u

/* Ports carried over the relay are shifted by this offset, as PPSSPP's default PortOffset
 * setting does; peers must agree on it, because the relay keys its sessions by MAC and port. */
#define NK_RELAY_PORT_OFFSET 10000u

#define NK_RELAY_MAC_LEN 6u
#define NK_RELAY_GROUP_LEN 8u
#define NK_RELAY_NICKNAME_LEN 128u /* the field; the text is at most 127 bytes */
#define NK_RELAY_PRODUCT_LEN 9u   /* the disc ID, for example ULUS12345 */

/* ---- lobby (adhocctl protocol v1) --------------------------------------------------------- */

typedef enum NkLobbyOpcode {
    NK_LOBBY_PING = 0,
    NK_LOBBY_LOGIN = 1,
    NK_LOBBY_CONNECT = 2,
    NK_LOBBY_DISCONNECT = 3,
    NK_LOBBY_SCAN = 4,
    NK_LOBBY_SCAN_COMPLETE = 5,
    NK_LOBBY_CONNECT_BSSID = 6,
    NK_LOBBY_CHAT = 7
} NkLobbyOpcode;

/* Client to server: one opcode byte, then a fixed body. */
#define NK_LOBBY_LOGIN_SIZE 144u  /* opcode, MAC 6, nickname 128, product code 9 */
#define NK_LOBBY_CONNECT_SIZE 9u  /* opcode, group 8 */
#define NK_LOBBY_BARE_SIZE 1u     /* PING, SCAN and DISCONNECT are the opcode alone */

/* Server to client. */
#define NK_LOBBY_S2C_SCAN_SIZE 15u          /* opcode, group 8, leader MAC 6 */
#define NK_LOBBY_S2C_SCAN_COMPLETE_SIZE 1u
#define NK_LOBBY_S2C_CONNECT_SIZE 139u      /* opcode, nickname 128, MAC 6, peer id 4 */
#define NK_LOBBY_S2C_DISCONNECT_SIZE 5u     /* opcode, peer id 4 */
#define NK_LOBBY_S2C_CONNECT_BSSID_SIZE 7u  /* opcode, group leader MAC 6 */
#define NK_LOBBY_S2C_CHAT_SIZE 193u         /* opcode, message 64, nickname 128 */

/* True when the server would accept this MAC: it refuses one whose last five bytes are zero. */
bool nk_relay_mac_ok(const uint8_t mac[NK_RELAY_MAC_LEN]);
/* True for exactly nine characters, each A-Z or 0-9, as the server requires of a product code. */
bool nk_relay_product_ok(const char *product);

/* The LOGIN packet. Returns NK_LOBBY_LOGIN_SIZE, or 0 (out untouched) when the MAC or product
 * code would be refused or the nickname is NULL or 128 bytes or longer. The nickname is
 * zero-padded; the field's last byte is always zero. */
size_t nk_lobby_login(uint8_t out[NK_LOBBY_LOGIN_SIZE], const uint8_t mac[NK_RELAY_MAC_LEN],
                      const char *nickname, const char *product);
/* The CONNECT packet (join or create the group). Returns NK_LOBBY_CONNECT_SIZE. */
size_t nk_lobby_connect(uint8_t out[NK_LOBBY_CONNECT_SIZE],
                        const uint8_t group[NK_RELAY_GROUP_LEN]);
/* A bare PING, SCAN or DISCONNECT. Returns 1, or 0 for any other opcode. */
size_t nk_lobby_bare(uint8_t out[NK_LOBBY_BARE_SIZE], NkLobbyOpcode opcode);

typedef struct NkLobbyEvent {
    NkLobbyOpcode kind;
    uint8_t group[NK_RELAY_GROUP_LEN];           /* SCAN */
    uint8_t mac[NK_RELAY_MAC_LEN];               /* SCAN (leader), CONNECT, CONNECT_BSSID */
    char nickname[NK_RELAY_NICKNAME_LEN];        /* CONNECT, CHAT; always NUL-terminated */
    uint32_t peer_id;                            /* CONNECT, DISCONNECT: the server's id */
} NkLobbyEvent;

#define NK_LOBBY_NEED_MORE 0
#define NK_LOBBY_BAD (-1)
/* Decodes the first server packet in buf[0..len). Returns the bytes it used (> 0) and fills ev;
 * NK_LOBBY_NEED_MORE when buf holds only part of a packet; NK_LOBBY_BAD for an opcode a server
 * never sends (LOGIN, PING), which ends the session. A CHAT packet is decoded and used up, but the
 * player shows no chat. */
int nk_lobby_parse(const uint8_t *buf, size_t len, NkLobbyEvent *ev);

/* ---- data relay (postoffice) --------------------------------------------------------------- */

typedef enum NkRelayInit {
    NK_RELAY_INIT_PDP = 0,
    NK_RELAY_INIT_PTP_LISTEN = 1,
    NK_RELAY_INIT_PTP_CONNECT = 2,
    NK_RELAY_INIT_PTP_ACCEPT = 3
    /* 4, PDP over unreliable ENet/UDP, is not used: Nakagawa speaks the TCP form only. */
} NkRelayInit;

#define NK_RELAY_INIT_SIZE 24u        /* type 4, src MAC 8 (6 used), sport 2, dst MAC 8, dport 2 */
#define NK_RELAY_PDP_HEADER_SIZE 14u  /* MAC 8 (6 used), port 2, size 4 */
#define NK_RELAY_PTP_CONNECT_SIZE 10u /* MAC 8 (6 used), port 2 */
#define NK_RELAY_PTP_DATA_HEADER_SIZE 4u
#define NK_RELAY_PDP_MAX (10u * 1024u)
#define NK_RELAY_PTP_MAX (50u * 1024u)

/* The packet that opens every relay connection. dst may be NULL for PDP and PTP_LISTEN, which
 * name only the local end; unused bytes are zero. Returns NK_RELAY_INIT_SIZE, or 0 for an unknown
 * type, a NULL src, or a missing dst for PTP_CONNECT and PTP_ACCEPT. Ports are as sent, after
 * nk_relay_port_out. */
size_t nk_relay_init(uint8_t out[NK_RELAY_INIT_SIZE], NkRelayInit type,
                     const uint8_t src[NK_RELAY_MAC_LEN], uint16_t sport,
                     const uint8_t *dst, uint16_t dport);

/* The header before each PDP datagram: to the server it names the receiver, from the server the
 * sender. Encoding returns NK_RELAY_PDP_HEADER_SIZE, or 0 when size exceeds NK_RELAY_PDP_MAX.
 * Decoding returns false for a size over the limit, which ends the session. */
size_t nk_relay_pdp_header(uint8_t out[NK_RELAY_PDP_HEADER_SIZE],
                           const uint8_t mac[NK_RELAY_MAC_LEN], uint16_t port, uint32_t size);
bool nk_relay_parse_pdp_header(const uint8_t in[NK_RELAY_PDP_HEADER_SIZE],
                               uint8_t mac[NK_RELAY_MAC_LEN], uint16_t *port, uint32_t *size);

/* What the server sends on a PTP listen connection for each incoming connect (the connector's
 * MAC and port), and once as the acknowledgement on a new connect or accept connection. */
bool nk_relay_parse_ptp_connect(const uint8_t in[NK_RELAY_PTP_CONNECT_SIZE],
                                uint8_t mac[NK_RELAY_MAC_LEN], uint16_t *port);

/* The size before each PTP block once a connection carries data. */
size_t nk_relay_ptp_data_header(uint8_t out[NK_RELAY_PTP_DATA_HEADER_SIZE], uint32_t size);
bool nk_relay_parse_ptp_data_header(const uint8_t in[NK_RELAY_PTP_DATA_HEADER_SIZE],
                                    uint32_t *size);

/* A PSP port as the relay carries it, and back. Port 0 stays 0, and a port the offset wraps to 0
 * becomes 65535, as PPSSPP's offset_port_simple does; the way back subtracts the offset. */
uint16_t nk_relay_port_out(uint16_t port, uint16_t offset);
uint16_t nk_relay_port_in(uint16_t port, uint16_t offset);

#ifdef __cplusplus
}
#endif

#endif /* NK_ADHOC_RELAY_H */

/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_ADHOC_NET_H
#define NK_ADHOC_NET_H

/* The ad hoc transport seam (#1139 phase 2). The PSP's ad hoc services (sceNetAdhocctl,
 * sceNetAdhoc) are HLE state machines whose PSP-visible results never depend on which transport
 * carries their traffic. They reach their peers only through NkAdhocTransport:
 *   - the lobby: log in, join or create a group, scan for groups, leave, and the events that say
 *     who is in the group;
 *   - PDP sockets: datagrams addressed by MAC and port.
 * PTP streams join this interface in a later slice.
 *
 * Every transport answers with the same results and the same lobby events (NkLobbyEvent, the
 * relay's own decoded form), so the HLE layer maps them to PSP codes once. NkNetResult codes are
 * this project's own: none is a recorded PSP answer. Where a result has no relay counterpart
 * either, its NkNetResult entry says so, and the HLE layer must map that case to a PSP-visible
 * result by name, as a decision that is measured or marked UNMEASURED, before it relies on this
 * seam for it.
 *
 * This file also holds the first transport: an in-process medium (NkAdhocLoopNet) that up to
 * NK_LOOPNET_CONSOLES consoles share. One console alone on it is a PSP with no peers in range, the
 * default with Online play off. Several on it are a simulation for tests, labelled as such: they
 * share one process, and no host socket or network API is used. Its lobby is translated from
 * PPSSPP's ad hoc server (Core/HLE/proAdhocServer.cpp at 3bc60270; see nk_adhoc_net.c), which
 * speaks the same lobby protocol as the relay, so a game meets the same rules on both:
 *   - groups belong to one product code (disc ID) and are found by name;
 *   - a join: for each member already in the group, newest member first, the member hears of the
 *     newcomer and then the newcomer of the member (CONNECT); last the newcomer gets CONNECT_BSSID
 *     with the group's BSSID;
 *   - a group's BSSID is a locally administered address of its own, not a member's MAC; it stays
 *     when the creator leaves, and a group created again gets a new one;
 *   - an empty group is removed;
 *   - a scan lists every group of the product, newest group first, each with its BSSID, then
 *     SCAN_COMPLETE;
 *   - leaving tells each remaining member, newest first (DISCONNECT with the leaver's id), and the
 *     leaver nothing; logging out leaves the group the same way;
 *   - a login is refused unless the product code is nine characters A-Z or 0-9, the MAC is
 *     neither all ones nor all zero and the nickname's first byte is not zero (a nickname may fill
 *     all 128 bytes of its field); the console stays logged out (NK_NET_BAD_ARG);
 *   - a login while logged in, a join while in a group, a group name with a character outside
 *     A-Z, a-z and 0-9 (up to the first zero byte), a leave outside a group and a scan inside one
 *     are protocol errors: the
 *     server logs the console out (it leaves its group, its sockets close), and the call answers
 *     NK_NET_NOT_LOGGED_IN. The HLE layer refuses such calls itself before they reach a transport.
 *   - an event a console's full queue cannot hold is dropped and counted in events_dropped, as
 *     PPSSPP's server logs a failed send and carries on; nothing is retried and no call blocks.
 *     The sender's call still answers NK_NET_OK, and only the receiver's count shows the loss, so
 *     two consoles can then hold different views of one group. What the receiving guest sees is
 *     the lobby without that event: a missed CONNECT leaves a member out of its peer list, a
 *     missed DISCONNECT keeps a member that has left, a missed SCAN leaves a group out of the scan
 *     result, and a missed CONNECT_BSSID or SCAN_COMPLETE never arrives. The count starts at zero
 *     at each login. nk_adhoc_drop_report gives the line that names the console and the count.
 *     The HLE layer does not link this seam yet (src/rt/hle_network.c); when it does, it logs
 *     that line on reading a count above zero and carries on, as PPSSPP does.
 * The medium's own rules: a MAC already logged in is refused (PPSSPP's server accepts it), a
 * login resets events_dropped and pdp_dropped (both count losses since the console's last login,
 * and its sockets close at each logout), and the peer id is the console's index plus one. The server's product-code overrides, which let some
 * product codes share groups, are not modelled: a game meets only consoles running its own
 * product code.
 * Datagrams follow UDP: one to a MAC or port nobody holds in the sender's group is dropped and
 * the send still succeeds; a full receive queue drops the datagram and counts it. One sender can
 * fill a socket's queue, so the other senders' datagrams to it are dropped; the count is the
 * receiver's and does not name the sender (a known limit for the wiring step: no per-peer
 * fairness). Broadcast is
 * the caller's to fan out over the peer list, as PPSSPP does, so a group-address destination is
 * refused. */

#include <stddef.h>
#include <stdint.h>

#include "nk_adhoc_relay.h"

typedef enum NkNetResult {
    NK_NET_OK = 0,
    NK_NET_AGAIN = -1,          /* nothing has arrived yet */
    NK_NET_BAD_ARG = -2,
    NK_NET_NOT_LOGGED_IN = -3,  /* also the call the lobby answered by logging its caller out (a
                                 * protocol error, as PPSSPP's server treats it); the HLE layer
                                 * refuses those calls first and maps this by name, measured or
                                 * UNMEASURED, before using it */
    NK_NET_NOT_IN_GROUP = -4,
    NK_NET_PORT_IN_USE = -5,
    NK_NET_TOO_BIG = -6,        /* a datagram over NK_RELAY_PDP_MAX */
    NK_NET_NO_SOCKET = -7,      /* the handle is not an open socket */
    NK_NET_NO_SLOT = -8,        /* every socket slot is in use */
    NK_NET_SHORT_BUFFER = -9    /* the next datagram is larger than the buffer; it stays queued */
} NkNetResult;

typedef struct NkAdhocTransport NkAdhocTransport;

typedef struct NkAdhocTransportOps {
    const char *name;
    /* The lobby. Results arrive later as events, never as return values. */
    int (*login)(NkAdhocTransport *t, const uint8_t mac[NK_RELAY_MAC_LEN], const char *nickname,
                 const char *product);
    int (*logout)(NkAdhocTransport *t);               /* leaves the group and closes every socket */
    int (*connect)(NkAdhocTransport *t, const uint8_t group[NK_RELAY_GROUP_LEN]);
    int (*disconnect)(NkAdhocTransport *t);
    int (*scan)(NkAdhocTransport *t);
    size_t (*take_events)(NkAdhocTransport *t, NkLobbyEvent *out, size_t max);
    uint32_t (*events_dropped)(const NkAdhocTransport *t);
    /* PDP. pdp_open returns a handle (>= 0) or an NkNetResult. */
    int (*pdp_open)(NkAdhocTransport *t, uint16_t port);
    int (*pdp_send)(NkAdhocTransport *t, int handle, const uint8_t mac[NK_RELAY_MAC_LEN],
                    uint16_t port, const void *data, uint32_t len);
    /* The oldest datagram: sender MAC, sender port and bytes, *len its size. With a buffer that
     * is too small it answers NK_NET_SHORT_BUFFER, sets *len, and keeps the datagram. */
    int (*pdp_recv)(NkAdhocTransport *t, int handle, uint8_t mac[NK_RELAY_MAC_LEN],
                    uint16_t *port, void *buf, uint32_t cap, uint32_t *len);
    int (*pdp_close)(NkAdhocTransport *t, int handle);
    uint32_t (*pdp_dropped)(const NkAdhocTransport *t);
} NkAdhocTransportOps;

struct NkAdhocTransport {
    const NkAdhocTransportOps *ops;
};

#define NK_LOOPNET_CONSOLES 4u
#define NK_NET_EVENT_QUEUE 64u
#define NK_NET_PDP_SOCKETS 16u
#define NK_NET_PDP_QUEUE 32u   /* datagrams waiting on one socket */

typedef struct NkAdhocLoopNet NkAdhocLoopNet;

NkAdhocLoopNet *nk_loopnet_create(void);
void nk_loopnet_destroy(NkAdhocLoopNet *net);
/* Console index's endpoint on the medium, or NULL past NK_LOOPNET_CONSOLES. */
NkAdhocTransport *nk_loopnet_console(NkAdhocLoopNet *net, unsigned index);

/* Test aid: one lobby event as the medium queued it. */
typedef struct NkLoopTrace {
    unsigned console;   /* the receiving console's index */
    NkLobbyOpcode kind;
    uint32_t peer_id;   /* CONNECT and DISCONNECT */
} NkLoopTrace;
#define NK_LOOPNET_TRACE 64u
/* The lobby events the medium queued on any console since the last call, in the order it queued
 * them, which is the order the server sends them. Copies the first min(max, NK_LOOPNET_TRACE)
 * and returns how many were queued; a return above NK_LOOPNET_TRACE means some were not kept.
 * Dropped events (events_dropped) are not in it. */
size_t nk_loopnet_trace(NkAdhocLoopNet *net, NkLoopTrace *out, size_t max);
/* Test aid: sets the medium's group creation counter, so a test can reach the 2^32 creations
 * after which a new group's number shares its low 32 bits (its BSSID) with an older one. Groups
 * are numbered with 64 bits and never wrap; a number whose BSSID a live group holds is skipped. */
void nk_loopnet_set_groups_made(NkAdhocLoopNet *net, uint64_t made);

/* For a log: when t's events_dropped is above zero, writes one line naming the transport, the
 * console (the caller's number for it: the medium's index, or the HLE layer's own) and the count,
 * cut to fit buf[cap] and always terminated when cap > 0, and returns the full line's length.
 * Returns 0, with an empty buf, when nothing was dropped or t is NULL. */
int nk_adhoc_drop_report(const NkAdhocTransport *t, unsigned console, char *buf, size_t cap);

#endif

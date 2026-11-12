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
 *   - a join while in a group, a group name with a character outside A-Z, a-z and 0-9 (up to the
 *     first zero byte), a leave outside a group and a scan inside one are protocol errors: the
 *     server logs the console out (it leaves its group, its sockets close), and the call answers
 *     NK_NET_NOT_LOGGED_IN. The HLE layer refuses such calls itself before they reach a transport.
 *   - an event a console's full queue cannot hold is dropped and counted in events_dropped, as
 *     PPSSPP's server logs a failed send and carries on. A console whose count is above zero has an
 *     incomplete view of the lobby; the HLE layer must treat that as a fault, never carry on.
 * The medium's own rules: a MAC already logged in is refused (PPSSPP's server accepts it), and the
 * peer id is the console's index plus one. The server's product-code overrides, which let some
 * product codes share groups, are not modelled: a game meets only consoles running its own
 * product code.
 * Datagrams follow UDP: one to a MAC or port nobody holds in the sender's group is dropped and
 * the send still succeeds; a full receive queue drops the datagram and counts it. Broadcast is
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

#endif

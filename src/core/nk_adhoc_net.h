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
 * relay's own decoded form), so the HLE layer maps them to PSP codes once.
 *
 * This file also holds the first transport: an in-process medium (NkAdhocLoopNet) that up to
 * NK_LOOPNET_CONSOLES consoles share. One console alone on it is a PSP with no peers in range, the
 * default with Online play off. Several on it are a simulation for tests, labelled as such: they
 * share one process, and no host socket or network API is used. The medium follows the lobby
 * rules of the relay server Nakagawa speaks to (aemu_postoffice 02abf18f, adhocctl server), so a
 * game sees the same event order on the loopback as through the relay:
 *   - groups belong to one product code (disc ID) and are found by name;
 *   - a join first leaves the current group; each existing member and the newcomer then learn of
 *     each other, members in MAC order; last the newcomer gets CONNECT_BSSID with the leader's MAC;
 *   - the leader is the group's creator and stays so; an empty group is removed;
 *   - a scan lists every group of the product in name order, then SCAN_COMPLETE;
 *   - leaving tells each remaining member (DISCONNECT with the leaver's id), and the leaver
 *     nothing;
 *   - a console whose event queue is full is removed from the lobby, as the server removes a client
 *     it cannot send to: it leaves its group (the members are told), its sockets close, the event
 *     is counted in events_dropped, and its lobby and socket calls then answer NK_NET_NOT_LOGGED_IN.
 *     The events it was already sent stay for it to take.
 * Two server rules have no counterpart here. The server keys a group by channel and name, but every
 * client of the protocol version Nakagawa speaks (adhocctl v1) is on its one default channel
 * (client.cpp V1_PROTOCOL_DEFAULT_CHANNEL), so the name alone decides. The server's crosslinks,
 * which let some product codes share groups, are not modelled: a game meets only consoles running
 * its own product code.
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
    NK_NET_NOT_LOGGED_IN = -3,
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

#endif

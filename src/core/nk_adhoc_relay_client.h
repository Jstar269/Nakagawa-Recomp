/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
#ifndef NK_ADHOC_RELAY_CLIENT_H
#define NK_ADHOC_RELAY_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

#include "nk_adhoc_net.h"

#ifdef __cplusplus
extern "C" {
#endif

/* TCP client implementing NkAdhocTransport for community ad hoc relay servers speaking the
 * aemu_postoffice / PPSSPP protocol (lobby on TCP 27312, data relay on TCP 27313 by default).
 * Non-blocking and unencrypted plain TCP, active only when opted in by configuration. */
typedef struct NkAdhocRelayClient NkAdhocRelayClient;

typedef struct NkAdhocRelayConfig {
    const char *host;     /* DNS name or IPv4 address (no port); NULL or empty is refused */
    uint16_t lobby_port;  /* 0 -> NK_RELAY_LOBBY_PORT (27312) */
    uint16_t data_port;   /* 0 -> NK_RELAY_DATA_PORT (27313) */
    uint16_t port_offset; /* offset added on wire (normally NK_RELAY_PORT_OFFSET = 10000) */
} NkAdhocRelayConfig;

NkAdhocRelayClient *nk_relay_client_create(const NkAdhocRelayConfig *cfg);
void nk_relay_client_destroy(NkAdhocRelayClient *client);

NkAdhocTransport *nk_relay_client_transport(NkAdhocRelayClient *client);
bool nk_relay_client_lobby_connected(const NkAdhocRelayClient *client);
bool nk_relay_client_lobby_dead(const NkAdhocRelayClient *client);
const char *nk_relay_client_last_error(const NkAdhocRelayClient *client);

typedef bool (*NkRelayTestResolveFn)(const char *host, uint8_t out_ipv4[4], void *ctx);
void nk_relay_client_set_resolve_hook_for_test(NkRelayTestResolveFn fn, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* NK_ADHOC_RELAY_CLIENT_H */

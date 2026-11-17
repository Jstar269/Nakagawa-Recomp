// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-11.
// See NOTICE.md for upstream lineage and modification provenance.
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later
// Copyright (c) 2012- PPSSPP Project and contributors

/* Networking: sceNet, sceHttp, sceSsl, sceWlan, and opt-in ad hoc networking (#1139).
 * Lone-console sceWlan, sceNetInit/Term, and offline sceNetAdhocctl stay PSP-measured (see below).
 * Online ad hoc tier: PPSSPP-derived, not PSP-measured (classified derived-translated from
 * https://github.com/hrydgard/ppsspp rev 3bc60270eae3069f13dfbfdb7292a7e1375de6f6:
 * Core/HLE/{proAdhoc.h,proAdhoc.cpp,sceNetAdhoc.cpp,sceNetAdhocMatching.cpp,ErrorCodes.h},
 * Common/Net/NetAdhocCommon.h, Core/Net/AdhocTransportRelay.cpp).
 * Extracted from hle.c on 2026-10-31 (#307). */

#include "hle_internal.h"
#include "nk_adhoc_net.h"
#include "nk_adhoc_relay.h"
#include "nk_adhoc_relay_client.h"

/* sceWlanGetEtherAddr(u8 *addr): the 6-byte MAC address of the console profile. The whole
 * output span must be guest memory; NULL or an unwritable span answers ILLEGAL_ADDR and writes
 * nothing (the PSP's answer is not measured: the console's own address is never read). */
static uint32_t h_WlanGetEtherAddr(CpuState *s) {
    if (!A0 || !sr_guest_span_writable(A0, 6u)) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    if (!console_profile_identity())
        return hle_unmodelled_stop("sceWlanGetEtherAddr", "has no address: the host's random "
                                   "source is unavailable");
    for (int i = 0; i < 6; i++) MEM_W8(A0 + (uint32_t)i, s_console.mac[i]);
    return 0;
}

/* sceWlanGetSwitchState and sceWlanDevIsPowerOn: the console profile's WLAN switch, 1 (on) unless
 * the overlay's console object sets "wlan_switch": 0. Both answer 1 on the PSP-3000 6.61 oracle
 * with its switch on (PSP-KERNEL-SEM-001 wlan-state, 2026-10-31); what DevIsPowerOn answers with
 * the switch off is UNMEASURED, so it follows the switch. */
static uint32_t h_WlanOn(CpuState *s) {
    (void)s;
    sreg_ensure_loaded();
    return s_console.wlan_switch_set ? s_console.wlan_switch : 1u;
}

/* sceNetEtherNtostr(const unsigned char *mac, char *name): format the six-byte
 * hardware address locally. This does not require a host network stack. */
static uint32_t h_NetEtherNtostr(CpuState *s) {
    (void)s;
    uint32_t mac = A0, out = A1;
    if (!mac || !out || !sr_guest_span_readable(mac, 6u) ||
        !sr_guest_span_writable(out, 18u))
        return 0u;
    char text[18];
    snprintf(text, sizeof text, "%02x:%02x:%02x:%02x:%02x:%02x",
             MEM_R8(mac), MEM_R8(mac + 1u), MEM_R8(mac + 2u),
             MEM_R8(mac + 3u), MEM_R8(mac + 4u), MEM_R8(mac + 5u));
    for (uint32_t i = 0; i < sizeof text; i++)
        MEM_W8(out + i, (uint8_t)text[i]);
    return out;
}

/* ---- The sceNet core library with no network (#341). Nakagawa has no network stack and opens
 * no connection (AGENTS.md section 5), but on a PSP the core library's start-up needs no network
 * either: sceNetInit only takes its memory pool and starts its threads, and the console says "no network"
 * later, when a game asks for an address, a group or an access point. pspautotests records that on a
 * PSP (hrydgard/pspautotests 70e72391, tests/net): sceNetInit(0x20000, 0x20, 0x1000, 0x20, 0x1000)
 * returns 0 in every net test, sceNetTerm returns 0, and Term followed by Init again returns 0
 * (net/adhoc/socketids). A game that treats a refused sceNetInit as impossible goes on with a
 * network object it never set up: LittleBigPlanet's "connect" jumped through a NULL method there.
 *
 * So sceNetInit and sceNetTerm keep the library's state and its pool, and the calls that need a
 * network answer as a PSP with no group and no access point connection does. Nothing here reaches a
 * host network API. UNMEASURED, from PPSSPP Core/HLE/sceNet.cpp (commit 3bc60270, a behaviour
 * reference): the argument checks, the pool taken from the user partition, and sceNetTerm before
 * sceNetInit (0). A second sceNetInit without Term starts over with one pool; PPSSPP terminates
 * before it checks the arguments, while here the arguments are checked first, so an invalid second
 * call leaves the first pool in place (the PSP's order is UNMEASURED). The library's two threads
 * (SceNetCallout, SceNetNetintr) are not created.
 *
 * Every recording above was made with the WLAN switch on (pspautotests net/inet/offline: "The WLAN
 * switch must be on"), which is the console profile's default. With the profile's switch off
 * (sceWlanGetSwitchState 0) sceNetInit still returns 0 here: that case is UNMEASURED, and PPSSPP
 * does not consult the switch either. */
static struct {
    int inited;
    uint32_t pool_uid;  /* the user-partition block holding the pool */
} s_netlib;

#define NET_POOL_NONE 0xffffffffu

/* sceNetInit(poolSize, calloutPri, calloutStack, netintrPri, netintrStack). */
static uint32_t h_NetInit(CpuState *s) {
    const uint32_t pool_size = A0, callout_pri = A1, netintr_pri = A3;
    if (pool_size == 0u) return 0x800201b7u;               /* SCE_KERNEL_ERROR_ILLEGAL_MEMSIZE */
    if (callout_pri < 0x08u || callout_pri > 0x77u || netintr_pri < 0x08u || netintr_pri > 0x77u)
        return SCE_KERNEL_ERROR_ILLEGAL_PRIORITY;
    if (s_netlib.inited && s_netlib.pool_uid != NET_POOL_NONE) free_block(s_netlib.pool_uid);
    s_netlib.inited = 0;
    s_netlib.pool_uid = alloc_block(pool_size);
    if (s_netlib.pool_uid == NET_POOL_NONE) return SCE_KERNEL_ERROR_NO_MEMORY;
    s_netlib.inited = 1;
    return 0u;
}

/* sceNetTerm(void): frees the pool; 0 (measured after sceNetInit). Every recording ends
 * sceNetAdhocctl first (adhoc.h adhocShutdown; loopback.c, socketids.c, timing.c and term.c all
 * call sceNetAdhocctlTerm before sceNetTerm), so sceNetTerm while sceNetAdhocctl is initialized
 * is UNMEASURED and stops, leaving both libraries as they were. */
static int adhocctl_inited(void);
static uint32_t h_NetTerm(CpuState *s) {
    (void)s;
    if (adhocctl_inited())
        return hle_unmodelled_stop("sceNetTerm", "while sceNetAdhocctl is initialized (UNMEASURED: every "
                                   "recording ends it first)");
    if (s_netlib.inited && s_netlib.pool_uid != NET_POOL_NONE) free_block(s_netlib.pool_uid);
    s_netlib.inited = 0;
    s_netlib.pool_uid = NET_POOL_NONE;
    return 0u;
}

/* ---- sceNetAdhocctl on a console alone (#1139 phase 1a). The ad hoc control library is a state
 * machine with a thread of its own; its first transport is "none": a console with no other
 * console in range, which is real PSP behaviour and needs no network. Nothing here reaches a host
 * network API. The transport seam (src/core/nk_adhoc_net.h) is not linked into the runtime yet,
 * so this slice keeps the lone-console medium in this file: no group has another member, a scan
 * finds nothing, and a Connect to a group nobody hosts ends with this console alone in it.
 *
 * Every value below comes from pspautotests 70e72391 tests/net/adhoc, recorded on a PSP with the
 * WLAN switch on, cited as <test>.expected:<line> (the .c beside it gives the arguments). A case
 * no recording covers is a named stop (hle_unmodelled_stop) marked UNMEASURED, not a guess.
 *
 * The thread. sceNetAdhocctlInit(stack, priority, id) starts the library's thread with that stack
 * and priority: a stack under 0xC00 is the library's own 0x80410b13 (adhocctl.expected:10-13),
 * and priority 0 or 0x80 is the kernel's ILLEGAL_PRIORITY 0x80020193 (adhocctl.expected:14-15),
 * which sched_create_service_thread returns from the kernel's user-thread checks. The thread runs
 * the game's handlers, handler(event, error, arg), as the recordings show: every result arrives
 * later as an event, never during the call ("state right after 0, event 1, state 1",
 * adhocctl.expected:48), and the state changes when the event comes (scanstate.c samples both;
 * timing.expected:19-20, 22-23, 27-28 put them within a sample of each other). The thread waits
 * in sched_service_wait and calls each handler through hle_dispatch_guest_scoped; a handler that
 * faults ends the thread and the library (later calls stop the run).
 *
 * Event times are the medians of ten rounds on the PSP (timing.expected): Create to its CONNECT
 * event 1116728 us (:20), Disconnect to DISCONNECT 104638 us (:23), Scan to SCAN_COMPLETE 991527
 * us (:27), and Connect to a group nobody hosts, which this console then holds alone, 4087045 us
 * (:43). A scan shows state 2 once the thread has run (scanstate.expected:5, "state 2 from 1-2
 * ms"), not straight after the call (adhocctl.expected:73, ctlinit.expected:12). */
#define ADHOCCTL_ERROR_ALREADY_CONNECTED  0x80410b02u
#define ADHOCCTL_ERROR_INVALID_ARG        0x80410b04u
#define ADHOCCTL_ERROR_INVALID_HANDLER_ID 0x80410b06u
#define ADHOCCTL_ERROR_ALREADY_INITIALIZED 0x80410b07u
#define SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED 0x80410b08u
#define ADHOCCTL_ERROR_DISCONNECTED       0x80410b09u
#define ADHOCCTL_ERROR_BUSY               0x80410b10u
#define ADHOCCTL_ERROR_HANDLER_MAX        0x80410b12u
#define ADHOCCTL_ERROR_STACK_TOO_SMALL    0x80410b13u
#define ADHOCCTL_PEER_LIST_DISCONNECTED   0x80410682u   /* adhocctl.expected:28 */
#define ADHOCCTL_REINIT_REFUSED           0x80410601u   /* adhocctl.expected:87 */
#define ADHOC_ERROR_NO_ENTRY              0x80410716u   /* adhocctl.expected:53-54 */
#define SCE_NET_ERROR_LOCAL_ADDR_UNAVAILABLE 0x80410180u
#define SCE_NET_ERROR_NO_SPACE            0x80410001u
#define SCE_NET_ERROR_INTERNAL            0x80410002u

#define SCE_NET_ADHOC_ERROR_INVALID_SOCKET_ID   0x80410701u
#define SCE_NET_ADHOC_ERROR_INVALID_ADDR        0x80410702u
#define SCE_NET_ADHOC_ERROR_INVALID_PORT        0x80410703u
#define SCE_NET_ADHOC_ERROR_INVALID_BUFLEN      0x80410704u
#define SCE_NET_ADHOC_ERROR_INVALID_DATALEN     0x80410705u
#define SCE_NET_ADHOC_ERROR_NOT_ENOUGH_SPACE    0x80400706u
#define SCE_NET_ADHOC_ERROR_SOCKET_DELETED      0x80410707u
#define SCE_NET_ADHOC_ERROR_SOCKET_ALERTED      0x80410708u
#define SCE_NET_ADHOC_ERROR_WOULD_BLOCK         0x80410709u
#define SCE_NET_ADHOC_ERROR_PORT_IN_USE         0x8041070au
#define SCE_NET_ADHOC_ERROR_NOT_CONNECTED       0x8041070bu
#define SCE_NET_ADHOC_ERROR_DISCONNECTED        0x8041070cu
#define SCE_NET_ADHOC_ERROR_NOT_OPENED          0x8040070du
#define SCE_NET_ADHOC_ERROR_NOT_LISTENED        0x8040070eu
#define SCE_NET_ADHOC_ERROR_SOCKET_ID_NOT_AVAIL 0x8041070fu
#define SCE_NET_ADHOC_ERROR_PORT_NOT_AVAIL      0x80410710u
#define SCE_NET_ADHOC_ERROR_INVALID_ARG         0x80410711u
#define SCE_NET_ADHOC_ERROR_NOT_INITIALIZED     0x80410712u
#define SCE_NET_ADHOC_ERROR_ALREADY_INITIALIZED 0x80410713u
#define SCE_NET_ADHOC_ERROR_BUSY                0x80410714u
#define SCE_NET_ADHOC_ERROR_TIMEOUT             0x80410715u
#define SCE_NET_ADHOC_ERROR_EXCEPTION_EVENT     0x80410717u
#define SCE_NET_ADHOC_ERROR_CONNECTION_REFUSED  0x80410718u
#define SCE_NET_ADHOC_ERROR_THREAD_ABORTED      0x80410719u

#define SCE_NET_ADHOC_MATCHING_ERROR_INVALID_MODE       0x80410801u
#define SCE_NET_ADHOC_MATCHING_ERROR_INVALID_PORT       0x80410802u
#define SCE_NET_ADHOC_MATCHING_ERROR_INVALID_MAXNUM     0x80410803u
#define SCE_NET_ADHOC_MATCHING_ERROR_RXBUF_TOO_SHORT    0x80410804u
#define SCE_NET_ADHOC_MATCHING_ERROR_INVALID_OPTLEN     0x80410805u
#define SCE_NET_ADHOC_MATCHING_ERROR_INVALID_ARG        0x80410806u
#define SCE_NET_ADHOC_MATCHING_ERROR_INVALID_ID         0x80410807u
#define SCE_NET_ADHOC_MATCHING_ERROR_ID_NOT_AVAIL       0x80410808u
#define SCE_NET_ADHOC_MATCHING_ERROR_NO_SPACE           0x80410809u
#define SCE_NET_ADHOC_MATCHING_ERROR_IS_RUNNING         0x8041080au
#define SCE_NET_ADHOC_MATCHING_ERROR_NOT_RUNNING        0x8041080bu
#define SCE_NET_ADHOC_MATCHING_ERROR_UNKNOWN_TARGET     0x8041080cu
#define SCE_NET_ADHOC_MATCHING_ERROR_TARGET_NOT_READY   0x8041080du
#define SCE_NET_ADHOC_MATCHING_ERROR_EXCEED_MAXNUM      0x8041080eu
#define SCE_NET_ADHOC_MATCHING_ERROR_REQUEST_IN_PROG    0x8041080fu
#define SCE_NET_ADHOC_MATCHING_ERROR_ALREADY_EST        0x80410810u
#define SCE_NET_ADHOC_MATCHING_ERROR_BUSY               0x80410811u
#define SCE_NET_ADHOC_MATCHING_ERROR_ALREADY_INIT       0x80410812u
#define SCE_NET_ADHOC_MATCHING_ERROR_NOT_INIT           0x80410813u
#define SCE_NET_ADHOC_MATCHING_ERROR_PORT_IN_USE        0x80410814u
#define SCE_NET_ADHOC_MATCHING_ERROR_STACKSIZE_TOO_SHORT 0x80410815u
#define SCE_NET_ADHOC_MATCHING_ERROR_INVALID_DATALEN    0x80410816u
#define SCE_NET_ADHOC_MATCHING_ERROR_NOT_EST            0x80410817u
#define SCE_NET_ADHOC_MATCHING_ERROR_DATA_BUSY          0x80410818u

#define ADHOC_F_NONBLOCK     0x0001u
#define ADHOC_F_ALERTSEND    0x0010u
#define ADHOC_F_ALERTRECV    0x0020u
#define ADHOC_F_ALERTPOLL    0x0040u
#define ADHOC_F_ALERTCONNECT 0x0080u
#define ADHOC_F_ALERTACCEPT  0x0100u
#define ADHOC_F_ALERTFLUSH   0x0200u
#define ADHOC_F_ALERTALL     0x03f0u

#define ADHOC_EV_SEND        0x0001u
#define ADHOC_EV_RECV        0x0002u
#define ADHOC_EV_CONNECT     0x0004u
#define ADHOC_EV_ACCEPT      0x0008u
#define ADHOC_EV_FLUSH       0x0010u
#define ADHOC_EV_INVALID     0x0100u
#define ADHOC_EV_DELETE      0x0200u
#define ADHOC_EV_ALERT       0x0400u
#define ADHOC_EV_DISCONNECT  0x0800u

#define ADHOC_PTP_STATE_CLOSED      0
#define ADHOC_PTP_STATE_LISTEN      1
#define ADHOC_PTP_STATE_SYN_SENT    2
#define ADHOC_PTP_STATE_ESTABLISHED 4

#define PSP_ADHOC_PDP_MFS 1444u
#define PSP_ADHOC_PDP_MTU 65523u
#define ADHOCCTL_PEER_INFO_SIZE 152u
#define ADHOCCTL_SCAN_INFO_SIZE 28u
#define ADHOCCTL_PARAM_WRITE_SIZE 146u
#define ADHOC_PUMP_INTERVAL_US 2000u
#define ADHOC_POLL_WAIT_STEP_US 1000u

#define ADHOCCTL_STACK_MIN       0xc00u
#define ADHOCCTL_HANDLERS        4      /* adhocctl.expected:34-36: ids 0 and 1, two more, then full */
#define ADHOCCTL_CREATE_US       1116728u
#define ADHOCCTL_CONNECT_US      4087045u
#define ADHOCCTL_DISCONNECT_US   104638u
#define ADHOCCTL_SCAN_US         991527u
#define ADHOCCTL_EVENT_ERROR     0
#define ADHOCCTL_EVENT_CONNECT   1
#define ADHOCCTL_EVENT_DISCONNECT 2
#define ADHOCCTL_EVENT_SCAN      3
#define ADHOCCTL_ERROR_EVENTS_MAX 16u

enum { ADHOCCTL_REQ_NONE = 0, ADHOCCTL_REQ_CREATE, ADHOCCTL_REQ_CONNECT, ADHOCCTL_REQ_SCAN,
       ADHOCCTL_REQ_DISCONNECT };

#define ADHOC_MAX_PEERS 16
#define ADHOC_MAX_SCANS 32

typedef struct AdhocPeerEntry {
    int in_list;
    int active;
    uint32_t peer_id;
    uint8_t mac[6];
    uint8_t nickname[128];
    uint64_t last_recv;
    uint64_t left_at;
} AdhocPeerEntry;

typedef struct AdhocScanEntry {
    uint8_t group[8];
    uint8_t bssid[6];
    int channel;
    int mode;
} AdhocScanEntry;

static NkAdhocTransport *s_adhoc_test_transport;
static NkAdhocRelayClient *s_adhoc_relay_client;
static NkAdhocTransport *s_adhoc_active_transport;
static int s_adhoc_exit_hooked;

void sr_hle_test_set_adhoc_transport(NkAdhocTransport *t) {
    s_adhoc_test_transport = t;
}

static int adhoc_parse_port_offset(uint16_t *out) {
    const char *raw = getenv("SR_ADHOC_PORT_OFFSET");
    if (!raw || !raw[0]) {
        *out = NK_RELAY_PORT_OFFSET;
        return 1;
    }
    char *end = NULL;
    unsigned long v = strtoul(raw, &end, 10);
    if (end == raw || *end != '\0' || v > 65535ul) return 0;
    *out = (uint16_t)v;
    return 1;
}

static int adhoc_online_mode(void) {
    if (s_adhoc_test_transport) return 1;
    const char *en = getenv("SR_ADHOC_ENABLED");
    const char *srv = getenv("SR_ADHOC_SERVER");
    if (!en || strcmp(en, "1") != 0) return 0;
    return nk_adhoc_relay_parse_server(srv, NULL, 0u, NULL, NULL) ? 1 : 0;
}

static uint32_t hle_adhoc_refuse_offline(CpuState *s) {
    return hle_controlled_refusal(s, 0x80010086u);
}

static int is_mac_match(const uint8_t a[6], const uint8_t b[6]) {
    /* Compare bytes 1..5 so titles tweaking byte 0 (locally administered/multicast bits)
     * continue to match peers and local address, matching PPSSPP isMacMatch (3bc60270). */
    return memcmp(a + 1, b + 1, 5u) == 0;
}

static int is_zero_mac(const uint8_t a[6]) {
    return (a[0] | a[1] | a[2] | a[3] | a[4] | a[5]) == 0u;
}

static int is_broadcast_mac(const uint8_t a[6]) {
    return (a[0] & a[1] & a[2] & a[3] & a[4] & a[5]) == 0xffu;
}

static AdhocPeerEntry s_adhoc_peers[ADHOC_MAX_PEERS];
static AdhocScanEntry s_adhoc_scans[ADHOC_MAX_SCANS];
static int s_adhoc_scan_count;

static struct {
    int inited;
    int reinit_refused;      /* a second Init tore the library down (adhocctl.expected:85-87) */
    int term_after_refused;  /* and Term ran after that: what Init answers then is UNMEASURED */
    int radio_used;          /* a Create, Connect or Scan has been asked for since Init */
    uint32_t gen;            /* the thread generation; a thread whose gen is old ends */
    uint32_t thread_uid;
    uint8_t id[13];          /* the AdhocId Init received: type (4 bytes) and product (9) */
    int state;               /* GetState: 0 disconnected, 1 in a group, 2 scanning */
    int request;             /* the request in flight, ADHOCCTL_REQ_* */
    uint64_t request_due;    /* guest time its event is due */
    int scan_shown;          /* the thread has shown the scan's state 2 */
    uint8_t group[8];        /* the group asked for, then the group held */
    int group_null;          /* that name was NULL */
    uint8_t bssid[6];        /* resolved group BSSID */
    int online_active;       /* online transport attached for this Init session */
    unsigned error_events;   /* ALREADY_CONNECTED events still to deliver */
    struct { int used; uint32_t fn, arg; } handlers[ADHOCCTL_HANDLERS];
} s_adhocctl;

static int adhocctl_inited(void) { return s_adhocctl.inited; }

/* A request already in flight. BUSY is recorded only for these pairs, each with valid arguments:
 * Scan and Create during a Create (ctlinit.expected:24, 29; a Create after a refused Disconnect,
 * abandon.expected:30), and Disconnect during a Create or a Connect (abandon.expected:6, 12).
 * Every other request while one is in flight is UNMEASURED and stops. Whether a PSP checks a
 * request's arguments or its busy state first is UNMEASURED too: adhocctl_start checks the
 * group name first, and an invalid name while a request is in flight stops by name instead of
 * answering BUSY or INVALID_ARG. Returns 1 when the call must answer `*out`. */
static int adhocctl_pending(const char *name, int request, uint32_t *out) {
    if (s_adhocctl.request == ADHOCCTL_REQ_NONE) return 0;
    const int pending = s_adhocctl.request;
    if ((pending == ADHOCCTL_REQ_CREATE &&
         (request == ADHOCCTL_REQ_SCAN || request == ADHOCCTL_REQ_CREATE ||
          request == ADHOCCTL_REQ_DISCONNECT)) ||
        (pending == ADHOCCTL_REQ_CONNECT && request == ADHOCCTL_REQ_DISCONNECT)) {
        *out = ADHOCCTL_ERROR_BUSY;
        return 1;
    }
    static const char *const names[] = {"", "create", "connect", "scan", "disconnect"};
    *out = hle_unmodelled_stop(name, "while a %s is in flight (UNMEASURED)", names[pending]);
    return 1;
}

/* A query while any request is in flight, or with ALREADY_CONNECTED events not yet delivered, is
 * UNMEASURED: every recorded query ran after the last event. */
static int adhocctl_query_settled(const char *name) {
    if (s_adhocctl.online_active) return 1;
    if (s_adhocctl.request == ADHOCCTL_REQ_NONE && s_adhocctl.error_events == 0u) return 1;
    hle_unmodelled_stop(name, "while a request is in flight (UNMEASURED)");
    return 0;
}

static int adhocctl_channel(void) {
    const uint32_t setting = sreg_systemparam_int(2u); /* PSP_SYSTEMPARAM_ID_INT_ADHOC_CHANNEL */
    return (int)((setting == 1u || setting == 6u || setting == 11u) ? setting : 11u);
}

static void adhoc_fill_nickname(uint8_t out[128]) {
    memset(out, 0, 128u);
    sreg_ensure_loaded();
    const int node = sreg_find_path("/CONFIG/SYSTEM/owner_name");
    if (node >= 0 && s_sreg_nodes[node].type == SREG_TYPE_STR) {
        for (uint32_t i = 0u; i < 127u && i < s_sreg_nodes[node].size && s_sreg_nodes[node].value[i]; i++)
            out[i] = (uint8_t)s_sreg_nodes[node].value[i];
        sreg_note_placeholder_read(node, "sceNetAdhocctlGetParameter");
    }
}

static void adhoc_close_sockets_and_matching(void);

static void adhoc_release_transport(void) {
    if (s_adhoc_active_transport && s_adhoc_active_transport->ops &&
        s_adhoc_active_transport->ops->logout) {
        (void)s_adhoc_active_transport->ops->logout(s_adhoc_active_transport);
    }
    if (s_adhoc_relay_client) {
        nk_relay_client_destroy(s_adhoc_relay_client);
        s_adhoc_relay_client = NULL;
    }
    s_adhoc_active_transport = NULL;
    memset(s_adhoc_peers, 0, sizeof(s_adhoc_peers));
    memset(s_adhoc_scans, 0, sizeof(s_adhoc_scans));
    s_adhoc_scan_count = 0;
}

static void adhoc_process_exit(void) {
    adhoc_close_sockets_and_matching();
    adhoc_release_transport();
}

static AdhocPeerEntry *adhoc_find_peer(const uint8_t mac[6]) {
    for (int i = 0; i < ADHOC_MAX_PEERS; i++) {
        if (s_adhoc_peers[i].active && is_mac_match(s_adhoc_peers[i].mac, mac))
            return &s_adhoc_peers[i];
    }
    return NULL;
}

static void adhoc_upsert_peer(const NkLobbyEvent *ev) {
    if (!ev || is_zero_mac(ev->mac)) return;
    if (console_profile_identity() && is_mac_match(s_console.mac, ev->mac)) return;
    AdhocPeerEntry *slot = NULL;
    for (int i = 0; i < ADHOC_MAX_PEERS; i++) {
        if (s_adhoc_peers[i].in_list && is_mac_match(s_adhoc_peers[i].mac, ev->mac)) {
            slot = &s_adhoc_peers[i];
            break;
        }
        if (!s_adhoc_peers[i].in_list && !slot) slot = &s_adhoc_peers[i];
    }
    if (!slot) {
        for (int i = 0; i < ADHOC_MAX_PEERS; i++) {
            if (!s_adhoc_peers[i].active) { slot = &s_adhoc_peers[i]; break; }
        }
    }
    if (!slot) return;
    memset(slot, 0, sizeof(*slot));
    slot->in_list = 1;
    slot->active = 1;
    slot->peer_id = ev->peer_id;
    memcpy(slot->mac, ev->mac, 6u);
    {
        size_t nlen = strlen(ev->nickname);
        if (nlen >= sizeof(slot->nickname)) nlen = sizeof(slot->nickname) - 1u;
        memcpy(slot->nickname, ev->nickname, nlen);
        slot->nickname[nlen] = '\0';
    }
    slot->last_recv = sched_vtime_us();
    slot->left_at = 0u;
}

static void adhoc_remove_peer_by_id(uint32_t peer_id) {
    for (int i = 0; i < ADHOC_MAX_PEERS; i++) {
        if (s_adhoc_peers[i].active && s_adhoc_peers[i].peer_id == peer_id) {
            s_adhoc_peers[i].active = 0;
            s_adhoc_peers[i].left_at = sched_vtime_us();
            if (s_adhoc_peers[i].left_at == 0u) s_adhoc_peers[i].left_at = 1u;
        }
    }
}

static void adhoc_record_scan(const NkLobbyEvent *ev) {
    if (!ev) return;
    for (int i = 0; i < s_adhoc_scan_count; i++) {
        if (memcmp(s_adhoc_scans[i].group, ev->group, 8u) == 0 &&
            memcmp(s_adhoc_scans[i].bssid, ev->bssid, 6u) == 0)
            return;
    }
    if (s_adhoc_scan_count >= ADHOC_MAX_SCANS) return;
    AdhocScanEntry *sc = &s_adhoc_scans[s_adhoc_scan_count++];
    memcpy(sc->group, ev->group, 8u);
    memcpy(sc->bssid, ev->bssid, 6u);
    sc->channel = adhocctl_channel();
    sc->mode = 0;
}

static int adhoc_local_mac(uint8_t out[6]) {
    if (!console_profile_identity()) return 0;
    memcpy(out, s_console.mac, 6u);
    return 1;
}

static void adhoc_fix_mac(uint8_t mac[6]) {
    uint8_t self_mac[6];
    if (adhoc_local_mac(self_mac) && is_mac_match(self_mac, mac)) {
        memcpy(mac, self_mac, 6u);
        return;
    }
    const AdhocPeerEntry *p = adhoc_find_peer(mac);
    if (p) memcpy(mac, p->mac, 6u);
}

static uint64_t adhoc_peer_left_at(const uint8_t mac[6]) {
    for (int i = 0; i < ADHOC_MAX_PEERS; i++) {
        if (s_adhoc_peers[i].in_list && !s_adhoc_peers[i].active &&
            is_mac_match(s_adhoc_peers[i].mac, mac))
            return s_adhoc_peers[i].left_at;
    }
    return 0u;
}

static void adhocctl_call_handlers(CpuState *s, uint32_t gen, uint32_t sp, uint32_t gp,
                                   int event, uint32_t error) {
    for (int i = 0; i < ADHOCCTL_HANDLERS; i++) {
        if (s_adhocctl.gen != gen) return;   /* Term, Init or LoadExec under a handler */
        if (!s_adhocctl.handlers[i].used) continue;
        const uint32_t fn = s_adhocctl.handlers[i].fn;
        s->r[4] = (uint32_t)event;
        s->r[5] = error;
        s->r[6] = s_adhocctl.handlers[i].arg;
        s->r[28] = gp;
        s->r[29] = sp;
        s->r[31] = SR_HOST_RETURN_PC;
        s->pc = fn;
        uint32_t fatal_target = 0u;
        if (hle_dispatch_guest_scoped(s, fn, "sceNetAdhocctl handler", &fatal_target)) {
            /* A fatal flow left on this thread's CpuState would be lost: the body returns,
             * the thread exits, and the next thread's context replaces s_cpu, so a game
             * waiting for what the handler should have done would wait forever. The run
             * stops here instead, with dispatch_call's exit for a dispatch it cannot complete
             * (recomp.c): the SEMANTIC_BOUNDARY line tools/nk_cli.py classifies, a flight-
             * recorder fatal, and exit 1. Like those exits it does not run the finalisation an
             * unsupported-import stop runs (trace close, compat report, videorec finish, perf
             * shutdown, hle.c hle_unmodelled_stop): neither hle.c nor recomp.c exposes that
             * sequence as a function, and this file does not copy it. */
            fprintf(stderr, "SEMANTIC_BOUNDARY: adhocctl-handler-fault; handler 0x%08x (id %d, "
                            "event %d) faulted at 0x%08x on the ad hoc control thread; the run "
                            "stops here\n",
                    fn, i, event, fatal_target);
            sr_flight_fatal(SR_FLIGHT_KIND_FATAL_DISPATCH, fn, fatal_target, (uint32_t)event);
            fflush(stderr);
            exit(1);
        }
    }
}

static void adhoc_pump_lobby(CpuState *s, uint32_t gen, uint32_t sp, uint32_t gp) {
    NkAdhocTransport *t = s_adhoc_active_transport;
    if (!t || !t->ops) return;
    if (t->ops->pump) t->ops->pump(t);
    NkLobbyEvent ev[16];
    size_t n = t->ops->take_events ? t->ops->take_events(t, ev, 16u) : 0u;
    for (size_t i = 0u; i < n && s_adhocctl.gen == gen; i++) {
        switch (ev[i].kind) {
        case NK_LOBBY_CONNECT:
            adhoc_upsert_peer(&ev[i]);
            break;
        case NK_LOBBY_DISCONNECT:
            adhoc_remove_peer_by_id(ev[i].peer_id);
            break;
        case NK_LOBBY_SCAN:
            adhoc_record_scan(&ev[i]);
            break;
        case NK_LOBBY_SCAN_COMPLETE:
            s_adhocctl.state = 0;
            if (s_adhocctl.request == ADHOCCTL_REQ_SCAN) s_adhocctl.request = ADHOCCTL_REQ_NONE;
            adhocctl_call_handlers(s, gen, sp, gp, ADHOCCTL_EVENT_SCAN, 0u);
            break;
        case NK_LOBBY_CONNECT_BSSID:
            memcpy(s_adhocctl.bssid, ev[i].bssid, 6u);
            s_adhocctl.state = 1;
            s_adhocctl.request = ADHOCCTL_REQ_NONE;
            adhocctl_call_handlers(s, gen, sp, gp, ADHOCCTL_EVENT_CONNECT, 0u);
            break;
        default:
            break;
        }
    }
}

/* The library's thread: shows a scan's state, delivers each event when it is due, and otherwise
 * waits. Handlers registered when an event is delivered receive it, in handler-id order (with
 * more than one handler the order is UNMEASURED). */
static void adhocctl_thread_body(CpuState *s, void *arg) {
    const uint32_t gen = (uint32_t)(uintptr_t)arg;
    const uint32_t sp = s->r[29], gp = s->r[28];
    while (s_adhocctl.gen == gen) {
        if (s_adhocctl.error_events) {
            /* adhocctl.expected:58-61: Create, Connect and Scan in a group each return 0 and each
             * gets event 0 with ALREADY_CONNECTED, within the 100 ms the test waits (UNMEASURED
             * more closely: delivered as soon as the thread runs). */
            s_adhocctl.error_events--;
            adhocctl_call_handlers(s, gen, sp, gp, ADHOCCTL_EVENT_ERROR, ADHOCCTL_ERROR_ALREADY_CONNECTED);
            continue;
        }
        if (s_adhocctl.request == ADHOCCTL_REQ_SCAN && !s_adhocctl.scan_shown) {
            s_adhocctl.scan_shown = 1;
            s_adhocctl.state = 2;
        }
        if (s_adhocctl.online_active) {
            if (s_adhocctl.request == ADHOCCTL_REQ_DISCONNECT &&
                sched_vtime_us() >= s_adhocctl.request_due) {
                s_adhocctl.state = 0;
                s_adhocctl.request = ADHOCCTL_REQ_NONE;
                memset(s_adhoc_peers, 0, sizeof(s_adhoc_peers));
                adhocctl_call_handlers(s, gen, sp, gp, ADHOCCTL_EVENT_DISCONNECT, 0u);
                continue;
            }
            adhoc_pump_lobby(s, gen, sp, gp);
            if (s_adhocctl.gen != gen) break;
            if (s_adhocctl.request == ADHOCCTL_REQ_DISCONNECT) {
                sched_service_wait(s_adhocctl.request_due);
            } else {
                sched_service_wait(sched_vtime_us() + ADHOC_PUMP_INTERVAL_US);
            }
            continue;
        }
        if (s_adhocctl.request != ADHOCCTL_REQ_NONE && sched_vtime_us() >= s_adhocctl.request_due) {
            int event;
            switch (s_adhocctl.request) {
            case ADHOCCTL_REQ_SCAN:       s_adhocctl.state = 0; event = ADHOCCTL_EVENT_SCAN; break;
            case ADHOCCTL_REQ_DISCONNECT: s_adhocctl.state = 0; event = ADHOCCTL_EVENT_DISCONNECT; break;
            default:                      s_adhocctl.state = 1; event = ADHOCCTL_EVENT_CONNECT; break;
            }
            s_adhocctl.request = ADHOCCTL_REQ_NONE;
            adhocctl_call_handlers(s, gen, sp, gp, event, 0u);
            continue;
        }
        sched_service_wait(s_adhocctl.request != ADHOCCTL_REQ_NONE ? s_adhocctl.request_due
                                                                   : UINT64_MAX);
    }
}

/* Leaves the library uninitialized and its thread ending (it ends when it next runs: "Term
 * returns before adhocctl's thread has stopped", adhocctl.c). Handlers go with it: ctlinit.c adds
 * one after each of its seven Inits without deleting any, and all seven succeed
 * (ctlinit.expected:3, 7, 11, 15, 18, 23, 28), past the four-handler limit. */
static void adhocctl_teardown(void) {
    const uint32_t uid = s_adhocctl.thread_uid;
    adhoc_release_transport();
    s_adhocctl.gen++;
    s_adhocctl.inited = 0;
    s_adhocctl.online_active = 0;
    s_adhocctl.thread_uid = 0u;
    s_adhocctl.state = 0;
    s_adhocctl.request = ADHOCCTL_REQ_NONE;
    s_adhocctl.error_events = 0u;
    s_adhocctl.radio_used = 0;
    memset(s_adhocctl.bssid, 0, sizeof(s_adhocctl.bssid));
    memset(s_adhocctl.handlers, 0, sizeof(s_adhocctl.handlers));
    if (uid) sched_service_kick(uid);
}

static int s_adhoc_tier_logged;

static void adhoc_log_ppsspp_tier_once(void) {
    if (s_adhoc_tier_logged) return;
    s_adhoc_tier_logged = 1;
    fprintf(stderr, "HLE: online ad hoc stack active (PPSSPP-derived, not PSP-measured; rev 3bc60270)\n");
}

/* PPSSPP-derived, not PSP-measured: attach NkAdhocTransport and log in using profile MAC/nick. */
static int adhocctl_attach_online_transport(const uint8_t id13[13]) {
    adhoc_log_ppsspp_tier_once();
    if (!console_profile_identity()) return 0;
    uint8_t nick_bytes[128];
    adhoc_fill_nickname(nick_bytes);
    char nick[129];
    memcpy(nick, nick_bytes, 128u);
    nick[128] = '\0';
    if (!nk_relay_nickname_ok(nick)) snprintf(nick, sizeof(nick), "Player");

    char product[10];
    for (int i = 0; i < 9; i++) {
        uint8_t ch = id13[4 + i];
        if (ch >= 'a' && ch <= 'z') ch = (uint8_t)(ch - 'a' + 'A');
        product[i] = (char)ch;
    }
    product[9] = '\0';
    if (!nk_relay_product_ok(product)) snprintf(product, sizeof(product), "TEST00000");

    NkAdhocTransport *t = s_adhoc_test_transport;
    if (!t) {
        char host[NK_ADHOC_SERVER_MAX_LEN + 1u];
        uint16_t lp = 0u, dp = 0u, off = NK_RELAY_PORT_OFFSET;
        if (!nk_adhoc_relay_parse_server(getenv("SR_ADHOC_SERVER"), host, sizeof(host), &lp, &dp))
            return 0;
        if (!adhoc_parse_port_offset(&off)) return 0;
        NkAdhocRelayConfig cfg = {host, lp, dp, off};
        s_adhoc_relay_client = nk_relay_client_create(&cfg);
        if (!s_adhoc_relay_client) return 0;
        if (!s_adhoc_exit_hooked) {
            s_adhoc_exit_hooked = 1;
            (void)atexit(adhoc_process_exit);
        }
        t = nk_relay_client_transport(s_adhoc_relay_client);
    }
    s_adhoc_active_transport = t;
    if (t->ops->login(t, s_console.mac, nick, product) != NK_NET_OK) {
        adhoc_release_transport();
        return 0;
    }
    return 1;
}

/* sceNetAdhocctlInit(int stacksize, int priority, const SceNetAdhocctlAdhocId *id). */
static uint32_t h_NetAdhocctlInit(CpuState *s) {
    const uint32_t stack = A0, id = A2;
    const int priority = (int)A1;
    if (!s_netlib.inited)
        return hle_unmodelled_stop("sceNetAdhocctlInit", "before sceNetInit (UNMEASURED: every "
                                   "recording starts sceNet first)");
    if (s_adhocctl.term_after_refused)
        return hle_unmodelled_stop("sceNetAdhocctlInit", "after a refused second Init and a Term "
                                   "(UNMEASURED)");
    if (s_adhocctl.reinit_refused) return ADHOCCTL_REINIT_REFUSED;       /* adhocctl.expected:87 */
    if (s_adhocctl.inited) {
        /* adhocctl.expected:85-86: a second Init answers ALREADY_INITIALIZED and leaves the
         * library uninitialized (GetState 0x80410b08), in a way no later Init repairs (:87). Its
         * order against the argument checks is UNMEASURED (the recorded call is valid). */
        if (s_adhocctl.request != ADHOCCTL_REQ_NONE || s_adhocctl.error_events)
            return hle_unmodelled_stop("sceNetAdhocctlInit", "again while a request is in flight "
                                       "(UNMEASURED)");
        adhocctl_teardown();
        s_adhocctl.reinit_refused = 1;
        return ADHOCCTL_ERROR_ALREADY_INITIALIZED;
    }
    /* The argument checks, each recorded alone, in an UNMEASURED order. */
    if (stack < ADHOCCTL_STACK_MIN) return ADHOCCTL_ERROR_STACK_TOO_SMALL;  /* adhocctl.expected:10-13 */
    if (!id) return ADHOCCTL_ERROR_INVALID_ARG;                            /* adhocctl.expected:16 */
    if (!sr_guest_span_readable(id, 13u))
        return hle_unmodelled_stop("sceNetAdhocctlInit", "with an unreadable id 0x%08x (UNMEASURED)", id);
    const uint32_t type = MEM_R32(id);
    if (type == 3u) return ADHOCCTL_ERROR_INVALID_ARG;                     /* adhocctl.expected:17 */
    if (type != 0u)
        return hle_unmodelled_stop("sceNetAdhocctlInit", "with id type %u (UNMEASURED: 0 is accepted "
                                   "and 3 refused)", type);
    const int online = adhoc_online_mode();
    uint8_t id_bytes[13];
    for (uint32_t i = 0; i < 13u; i++) id_bytes[i] = MEM_R8(id + i);
    if (online && !adhocctl_attach_online_transport(id_bytes)) return SCE_NET_ERROR_INTERNAL;

    /* The product is not checked: an upper-case and a lower-case first letter are both accepted
     * (adhocctl.expected:13, 18); other spellings are UNMEASURED and accepted the same way. */
    const uint32_t gen = s_adhocctl.gen + 1u;
    const uint32_t uid = sched_create_service_thread("SceNetAdhocctl", priority, stack,
                                                     adhocctl_thread_body, (void *)(uintptr_t)gen);
    if (uid >= 0x80000000u) {
        if (online) adhoc_release_transport();
        return uid; /* ILLEGAL_PRIORITY (adhocctl.expected:14-15), or the kernel's NO_MEMORY */
    }
    s_adhocctl.gen = gen;
    s_adhocctl.thread_uid = uid;
    for (uint32_t i = 0; i < 13u; i++) s_adhocctl.id[i] = id_bytes[i];
    s_adhocctl.inited = 1;
    s_adhocctl.online_active = online;
    s_adhocctl.state = 0;
    s_adhocctl.request = ADHOCCTL_REQ_NONE;
    s_adhocctl.error_events = 0u;
    s_adhocctl.radio_used = 0;
    memset(s_adhocctl.bssid, 0, sizeof(s_adhocctl.bssid));
    memset(s_adhocctl.handlers, 0, sizeof(s_adhocctl.handlers));
    return 0u;
}

/* sceNetAdhocctlTerm(void): 0 before Init, after Term and after a refused second Init
 * (adhocctl.expected:7, 91-92); in a group it leaves it with no event recorded, and the library is
 * uninitialized (term.expected:25-26). Term with a request or an event still in flight is
 * UNMEASURED. */
static uint32_t h_NetAdhocctlTerm(CpuState *s) {
    (void)s;
    if (s_adhocctl.online_active) {
        if (!s_adhocctl.inited) return SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
        adhocctl_teardown();
        return 0u;
    }
    if (!s_adhocctl.inited) {
        if (s_adhocctl.reinit_refused) s_adhocctl.term_after_refused = 1;
        return 0u;
    }
    if (s_adhocctl.request != ADHOCCTL_REQ_NONE || s_adhocctl.error_events)
        return hle_unmodelled_stop("sceNetAdhocctlTerm", "while a request is in flight (UNMEASURED)");
    adhocctl_teardown();
    return 0u;
}

/* Every call below answers NOT_INITIALIZED before Init: recorded for GetState, AddHandler and
 * Create (adhocctl.expected:4-6) and for DelHandler and GetState after a refused second Init
 * (:86, 90, 93); the other calls' pre-init answer is UNMEASURED and follows them. */

/* sceNetAdhocctlAddHandler(handler, void *arg): the lowest free id (0, then 1 for the same
 * handler again: adhocctl.expected:34-35), four in all (:36), NULL refused (:33). Which id a
 * freed slot gives back is UNMEASURED. */
static uint32_t h_NetAdhocctlAddHandler(CpuState *s) {
    if (!s_adhocctl.inited) return SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
    if (!A0) return ADHOCCTL_ERROR_INVALID_ARG;
    for (int i = 0; i < ADHOCCTL_HANDLERS; i++) {
        if (s_adhocctl.handlers[i].used) continue;
        s_adhocctl.handlers[i].used = 1;
        s_adhocctl.handlers[i].fn = A0;
        s_adhocctl.handlers[i].arg = A1;
        return (uint32_t)i;
    }
    return ADHOCCTL_ERROR_HANDLER_MAX;
}

/* sceNetAdhocctlDelHandler(int id): 0 for a registered id (adhocctl.expected:37), 0x80410b06 for
 * 100 and -1 (:38-39). An id in 0..3 that holds no handler is UNMEASURED. */
static uint32_t h_NetAdhocctlDelHandler(CpuState *s) {
    if (!s_adhocctl.inited) return SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
    const int32_t id = (int32_t)A0;
    if (id < 0 || id >= ADHOCCTL_HANDLERS) return ADHOCCTL_ERROR_INVALID_HANDLER_ID;
    if (!s_adhocctl.handlers[id].used)
        return hle_unmodelled_stop("sceNetAdhocctlDelHandler", "for free id %d (UNMEASURED)", (int)id);
    memset(&s_adhocctl.handlers[id], 0, sizeof(s_adhocctl.handlers[id]));
    return 0u;
}

/* sceNetAdhocctlGetState(int *state): leaves *state alone before Init (adhocctl.expected:4). */
static uint32_t h_NetAdhocctlGetState(CpuState *s) {
    if (!s_adhocctl.inited) return SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
    if (!A0 || !sr_guest_span_writable(A0, 4u))
        return hle_unmodelled_stop("sceNetAdhocctlGetState", "with output 0x%08x (UNMEASURED)", A0);
    MEM_W32(A0, (uint32_t)s_adhocctl.state);
    return 0u;
}

/* A group name: at most 8 bytes, ended early by a NUL. NULL is accepted (adhocctl.expected:42);
 * upper-case letters and digits are (TESTGRP1, ABANDON, CTLINIT1 ...); '-', ' ' and '_' are
 * refused with INVALID_ARG (:43-45). Any other byte, and the empty name, are UNMEASURED. Returns
 * 0 when valid, 1 for INVALID_ARG, -1 for a stop. */
static int adhocctl_check_group(const char *name, uint32_t addr, uint8_t out[8]) {
    memset(out, 0, 8u);
    if (!addr) return 0;
    uint32_t len = 0u;
    for (; len < 8u; len++) {
        if (!sr_guest_span_readable(addr + len, 1u)) {
            hle_unmodelled_stop(name, "with an unreadable group name 0x%08x (UNMEASURED)", addr);
            return -1;
        }
        const uint8_t c = MEM_R8(addr + len);
        if (c == 0u) break;
        if (c == '-' || c == ' ' || c == '_') return 1;
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
            hle_unmodelled_stop(name, "with group-name byte 0x%02x (UNMEASURED)", c);
            return -1;
        }
        out[len] = c;
    }
    if (len == 0u) {
        hle_unmodelled_stop(name, "with an empty group name (UNMEASURED)");
        return -1;
    }
    return 0;
}

/* The library's own thread is gone while the library is initialized: the guest terminated or
 * deleted it by uid (sceKernelTerminateThread, DeleteThread, TerminateDeleteThread). Nothing
 * would answer a request then, so a call that queues one for the thread stops by name. What a
 * PSP's library does after losing its thread is UNMEASURED. Returns 1 when the call must answer
 * `*out`. */
static int adhocctl_thread_lost(const char *name, uint32_t *out) {
    const uint32_t uid = s_adhocctl.thread_uid;
    SrThreadRunStatus status;
    if (uid && sched_thread_run_status(uid, &status) == 0 && !sched_is_dormant(uid)) return 0;
    *out = hle_unmodelled_stop(name, "after the guest ended the ad hoc control thread 0x%x "
                               "(UNMEASURED)", uid);
    return 1;
}

/* Create, Connect and Scan: in a group each answers 0 and later gets event 0 ALREADY_CONNECTED,
 * staying in the group (adhocctl.expected:58-62); otherwise the request starts and its event
 * follows. The order of the init, busy, name and state checks is UNMEASURED. */
static uint32_t adhocctl_start(const char *name, int request, uint32_t group_addr, int named) {
    if (!s_adhocctl.inited) return SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
    uint32_t lost;
    if (adhocctl_thread_lost(name, &lost)) return lost;
    uint8_t group[8];
    if (named) {
        const int bad = adhocctl_check_group(name, group_addr, group);
        if (bad < 0) return HLE_KERNEL_ERROR_NOT_IMPLEMENTED;
        if (bad && s_adhocctl.request != ADHOCCTL_REQ_NONE)
            return hle_unmodelled_stop(name, "with an invalid group name while a request is in flight "
                                       "(UNMEASURED: which check comes first)");
        if (bad) return ADHOCCTL_ERROR_INVALID_ARG;
    }
    uint32_t busy;
    if (adhocctl_pending(name, request, &busy)) return busy;
    if (s_adhocctl.state == 1) {
        if (s_adhocctl.error_events >= ADHOCCTL_ERROR_EVENTS_MAX)
            return hle_unmodelled_stop(name, "with %u ALREADY_CONNECTED events undelivered",
                                       s_adhocctl.error_events);
        s_adhocctl.error_events++;
        sched_service_kick(s_adhocctl.thread_uid);
        return 0u;
    }
    if (named) {
        memcpy(s_adhocctl.group, group, sizeof(group));
        s_adhocctl.group_null = group_addr == 0u;
    }
    if (s_adhocctl.online_active && s_adhoc_active_transport && s_adhoc_active_transport->ops) {
        NkAdhocTransport *t = s_adhoc_active_transport;
        int trc = NK_NET_OK;
        if (request == ADHOCCTL_REQ_SCAN) {
            s_adhoc_scan_count = 0;
            trc = t->ops->scan(t);
        } else {
            trc = t->ops->connect(t, s_adhocctl.group);
        }
        if (trc != NK_NET_OK) return ADHOCCTL_ERROR_DISCONNECTED;
        s_adhocctl.request = request;
        s_adhocctl.request_due = UINT64_MAX;
        s_adhocctl.scan_shown = 0;
        s_adhocctl.radio_used = 1;
        sched_service_kick(s_adhocctl.thread_uid);
        return 0u;
    }
    const uint32_t delay = request == ADHOCCTL_REQ_CREATE  ? ADHOCCTL_CREATE_US
                         : request == ADHOCCTL_REQ_CONNECT ? ADHOCCTL_CONNECT_US
                                                           : ADHOCCTL_SCAN_US;
    s_adhocctl.request = request;
    s_adhocctl.request_due = sched_vtime_us() + delay;
    s_adhocctl.scan_shown = 0;
    s_adhocctl.radio_used = 1;
    sched_service_kick(s_adhocctl.thread_uid);
    return 0u;
}

/* sceNetAdhocctlCreate(const char *group). */
static uint32_t h_NetAdhocctlCreate(CpuState *s) {
    return adhocctl_start("sceNetAdhocctlCreate", ADHOCCTL_REQ_CREATE, A0, 1);
}

/* sceNetAdhocctlConnect(const char *group): with nobody hosting the group, the console ends up
 * alone in it after about four seconds (timing.expected:38, 43; adhocctl.expected:78-80). */
static uint32_t h_NetAdhocctlConnect(CpuState *s) {
    return adhocctl_start("sceNetAdhocctlConnect", ADHOCCTL_REQ_CONNECT, A0, 1);
}

/* sceNetAdhocctlScan(void): nothing is in range, so the scan list stays empty (adhocctl.expected:75). */
static uint32_t h_NetAdhocctlScan(CpuState *s) {
    (void)s;
    return adhocctl_start("sceNetAdhocctlScan", ADHOCCTL_REQ_SCAN, 0u, 0);
}

/* sceNetAdhocctlDisconnect(void): 0, and event 2 later, in a group (adhocctl.expected:65, 81;
 * the state stays 1 until then, edges.expected:41-43) and also out of one (:30, 69-70;
 * ctlinit.expected:16). How long the event takes out of a group is UNMEASURED beyond "within
 * 500 ms"; it uses the in-group time. BUSY while a Create or Connect is in flight (abandon.expected:6). */
static uint32_t h_NetAdhocctlDisconnect(CpuState *s) {
    (void)s;
    if (!s_adhocctl.inited) return SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
    uint32_t busy;
    if (adhocctl_thread_lost("sceNetAdhocctlDisconnect", &busy)) return busy;
    if (adhocctl_pending("sceNetAdhocctlDisconnect", ADHOCCTL_REQ_DISCONNECT, &busy)) return busy;
    if (s_adhocctl.online_active && s_adhoc_active_transport && s_adhoc_active_transport->ops) {
        (void)s_adhoc_active_transport->ops->disconnect(s_adhoc_active_transport);
        s_adhocctl.request = ADHOCCTL_REQ_DISCONNECT;
        s_adhocctl.request_due = sched_vtime_us() + ADHOC_POLL_WAIT_STEP_US;
        sched_service_kick(s_adhocctl.thread_uid);
        return 0u;
    }
    s_adhocctl.request = ADHOCCTL_REQ_DISCONNECT;
    s_adhocctl.request_due = sched_vtime_us() + ADHOCCTL_DISCONNECT_US;
    sched_service_kick(s_adhocctl.thread_uid);
    return 0u;
}

/* sceNetAdhocctlJoin(const SceNetAdhocctlScanInfo *scan_info): extracts group_name (+8, 8 bytes)
 * and joins via the active online transport. Offline calls refuse with 0x80010086. */
static uint32_t h_NetAdhocctlJoin(CpuState *s) {
    if (!adhoc_online_mode()) return hle_adhoc_refuse_offline(s);
    if (!s_adhocctl.inited) return SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
    if (!A0 || !sr_guest_span_readable(A0, ADHOCCTL_SCAN_INFO_SIZE))
        return ADHOCCTL_ERROR_INVALID_ARG;
    return adhocctl_start("sceNetAdhocctlJoin", ADHOCCTL_REQ_CONNECT, A0 + 8u, 1);
}

/* sceNetAdhocctlGetAdhocId(SceNetAdhocctlAdhocId *id): the type and product Init received
 * (adhocctl.expected:22 compares those 13 bytes); the padding after them is left as it was
 * (UNMEASURED). */
static uint32_t h_NetAdhocctlGetAdhocId(CpuState *s) {
    if (!s_adhocctl.inited) return SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
    if (!A0 || !sr_guest_span_writable(A0, 13u))
        return hle_unmodelled_stop("sceNetAdhocctlGetAdhocId", "with output 0x%08x (UNMEASURED)", A0);
    for (uint32_t i = 0; i < 13u; i++) MEM_W8(A0 + i, s_adhocctl.id[i]);
    return 0u;
}

/* sceNetAdhocctlGetParameter(SceNetAdhocctlParameter *param): out of a group 0x80410b09
 * (adhocctl.expected:27, 67), writing nothing as far as is known. In a group (:50, 80):
 *   +0   int channel: one of 1, 6 or 11 was recorded, with the console's ad hoc channel setting
 *        at 1 (scanstate.expected "Ad hoc channel setting: 1"). Which channel a setting of 0
 *        (automatic, the profile's measured value) gives is UNMEASURED; it is 11, PPSSPP's
 *        choice (Core/HLE/proAdhoc.cpp:128, 1488 at 3bc60270), and a setting of 1, 6 or 11 is used
 *        as it is (UNMEASURED).
 *   +4   char group[8]: the group's name.
 *   +12  char nickname[128]: the registry's /CONFIG/SYSTEM/owner_name, NUL-padded, exactly as the
 *        registry holds it: the nickname the user chose in the player, else the neutral "Player"
 *        (#2197), so a nickname is set as adhocctl.expected:50 records on a PSP. It is never taken
 *        from the operating system.
 *   +140 u8 bssid[6]: not the console's own MAC, and with the locally administered bit set and the
 *        group bit clear ("unexpected (low bits 2)"). Its value is otherwise UNMEASURED: the
 *        profile MAC with those two bits set that way, which differs from the MAC in that bit.
 *   +146 padding: left as it was (UNMEASURED). */
static uint32_t h_NetAdhocctlGetParameter(CpuState *s) {
    if (!s_adhocctl.inited) return SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
    if (!adhocctl_query_settled("sceNetAdhocctlGetParameter")) return HLE_KERNEL_ERROR_NOT_IMPLEMENTED;
    if (s_adhocctl.state != 1) return ADHOCCTL_ERROR_DISCONNECTED;
    if (s_adhocctl.group_null)
        return hle_unmodelled_stop("sceNetAdhocctlGetParameter", "in a group created with a NULL "
                                   "name (UNMEASURED)");
    if (!A0 || !sr_guest_span_writable(A0, 146u))
        return hle_unmodelled_stop("sceNetAdhocctlGetParameter", "with output 0x%08x (UNMEASURED)", A0);
    if (!console_profile_identity())
        return hle_unmodelled_stop("sceNetAdhocctlGetParameter", "has no address: the host's random "
                                   "source is unavailable");
    const uint32_t setting = sreg_systemparam_int(2u);   /* PSP_SYSTEMPARAM_ID_INT_ADHOC_CHANNEL */
    const uint32_t channel = (setting == 1u || setting == 6u || setting == 11u) ? setting : 11u;
    MEM_W32(A0, channel);
    for (uint32_t i = 0; i < 8u; i++) MEM_W8(A0 + 4u + i, s_adhocctl.group[i]);
    uint8_t nickname[128] = {0};   /* the registry's owner_name, NUL-padded */
    sreg_ensure_loaded();
    const int node = sreg_find_path("/CONFIG/SYSTEM/owner_name");
    if (node >= 0 && s_sreg_nodes[node].type == SREG_TYPE_STR) {
        for (uint32_t i = 0; i < 127u && i < s_sreg_nodes[node].size && s_sreg_nodes[node].value[i]; i++)
            nickname[i] = (uint8_t)s_sreg_nodes[node].value[i];
        sreg_note_placeholder_read(node, "sceNetAdhocctlGetParameter");
    }
    for (uint32_t i = 0; i < 128u; i++) MEM_W8(A0 + 12u + i, nickname[i]);
    for (uint32_t i = 0; i < 6u; i++) {
        uint8_t b = s_adhocctl.online_active ? s_adhocctl.bssid[i] : s_console.mac[i];
        if (!s_adhocctl.online_active && i == 0u) b = (uint8_t)((b & 0xfcu) | 0x02u);
        MEM_W8(A0 + 140u + i, b);
    }
    return 0u;
}

/* Online ad hocctl peer/scan/lookup extensions (#1139):
 * PPSSPP-derived, not PSP-measured (derived-translated from PPSSPP rev 3bc60270
 * Core/HLE/proAdhoc.h & sceNetAdhoc.cpp SceNetAdhocctlScanInfoEmu / SceNetAdhocctlPeerInfoEmu). */
static void write_peer_info_emu(uint32_t addr, uint32_t next_addr, const uint8_t mac[6],
                                const uint8_t nick[128], uint64_t last_recv) {
    MEM_W32(addr, next_addr);
    for (uint32_t i = 0u; i < 127u; i++) MEM_W8(addr + 4u + i, nick[i]);
    MEM_W8(addr + 4u + 127u, 0u);
    for (uint32_t i = 0u; i < 6u; i++) MEM_W8(addr + 132u + i, mac[i]);
    MEM_W16(addr + 138u, 0u);
    MEM_W32(addr + 140u, 0x0400u);
    MEM_W32(addr + 144u, (uint32_t)(last_recv & 0xffffffffu));
    MEM_W32(addr + 148u, (uint32_t)(last_recv >> 32));
}

static uint32_t h_NetAdhocctlGetPeerList(CpuState *s) {
    if (!s_adhocctl.inited) return SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
    if (!s_adhocctl.online_active) {
        if (!adhocctl_query_settled("sceNetAdhocctlGetPeerList")) return HLE_KERNEL_ERROR_NOT_IMPLEMENTED;
        if (!A0 || !sr_guest_span_writable(A0, 4u) || (s_adhocctl.state != 1 && A1 != 0u))
            return hle_unmodelled_stop("sceNetAdhocctlGetPeerList", "with size 0x%08x and buffer 0x%08x "
                                       "in state %d (UNMEASURED)", A0, A1, s_adhocctl.state);
        MEM_W32(A0, 0u);
        return s_adhocctl.state == 1 ? 0u : ADHOCCTL_PEER_LIST_DISCONNECTED;
    }
    if (!A0 || !sr_guest_span_writable(A0, 4u)) return ADHOCCTL_ERROR_INVALID_ARG;
    if (s_adhocctl.state != 1) {
        if (A1 == 0u) MEM_W32(A0, 0u);
        return ADHOCCTL_PEER_LIST_DISCONNECTED;
    }
    int active_count = 0;
    for (int i = 0; i < ADHOC_MAX_PEERS; i++) {
        if (s_adhoc_peers[i].active) active_count++;
    }
    if (!A1) {
        MEM_W32(A0, (uint32_t)active_count * ADHOCCTL_PEER_INFO_SIZE);
        return 0u;
    }
    const int32_t req_bytes = (int32_t)MEM_R32(A0);
    if (req_bytes < 0) return ADHOCCTL_ERROR_INVALID_ARG;
    int max_entries = (int)((uint32_t)req_bytes / ADHOCCTL_PEER_INFO_SIZE);
    if (max_entries > active_count) max_entries = active_count;
    if (max_entries > 0 && !sr_guest_span_writable(A1, (uint32_t)max_entries * ADHOCCTL_PEER_INFO_SIZE))
        return hle_unmodelled_stop("sceNetAdhocctlGetPeerList", "with unwritable buffer 0x%08x", A1);
    int written = 0;
    const uint64_t now = sched_vtime_us();
    const uint64_t floor_ts = now > 10000u ? now - 10000u : 0u;
    for (int i = 0; i < ADHOC_MAX_PEERS && written < max_entries; i++) {
        if (!s_adhoc_peers[i].active) continue;
        if (s_adhoc_peers[i].last_recv < floor_ts) s_adhoc_peers[i].last_recv = floor_ts;
        const uint32_t entry_addr = A1 + (uint32_t)written * ADHOCCTL_PEER_INFO_SIZE;
        const uint32_t next_addr = (written + 1 < max_entries) ? entry_addr + ADHOCCTL_PEER_INFO_SIZE : 0u;
        write_peer_info_emu(entry_addr, next_addr, s_adhoc_peers[i].mac,
                            s_adhoc_peers[i].nickname, s_adhoc_peers[i].last_recv);
        written++;
    }
    MEM_W32(A0, (uint32_t)written * ADHOCCTL_PEER_INFO_SIZE);
    return 0u;
}

static uint32_t h_NetAdhocctlGetScanInfo(CpuState *s) {
    if (!s_adhocctl.inited) return SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
    if (!s_adhocctl.online_active) {
        if (!adhocctl_query_settled("sceNetAdhocctlGetScanInfo")) return HLE_KERNEL_ERROR_NOT_IMPLEMENTED;
        if (!A0 || !sr_guest_span_writable(A0, 4u) || A1 != 0u)
            return hle_unmodelled_stop("sceNetAdhocctlGetScanInfo", "with size 0x%08x and buffer 0x%08x "
                                       "(UNMEASURED)", A0, A1);
        MEM_W32(A0, 0u);
        return 0u;
    }
    if (!A0 || !sr_guest_span_writable(A0, 4u)) return ADHOCCTL_ERROR_INVALID_ARG;
    if (!A1) {
        MEM_W32(A0, (uint32_t)s_adhoc_scan_count * ADHOCCTL_SCAN_INFO_SIZE);
        return 0u;
    }
    const int32_t req_bytes = (int32_t)MEM_R32(A0);
    if (req_bytes < 0) return ADHOCCTL_ERROR_INVALID_ARG;
    int max_entries = (int)((uint32_t)req_bytes / ADHOCCTL_SCAN_INFO_SIZE);
    if (max_entries > s_adhoc_scan_count) max_entries = s_adhoc_scan_count;
    if (max_entries > 0 && !sr_guest_span_writable(A1, (uint32_t)max_entries * ADHOCCTL_SCAN_INFO_SIZE))
        return hle_unmodelled_stop("sceNetAdhocctlGetScanInfo", "with unwritable buffer 0x%08x", A1);
    for (int i = 0; i < max_entries; i++) {
        const uint32_t entry_addr = A1 + (uint32_t)i * ADHOCCTL_SCAN_INFO_SIZE;
        const uint32_t next_addr = (i + 1 < max_entries) ? entry_addr + ADHOCCTL_SCAN_INFO_SIZE : 0u;
        MEM_W32(entry_addr, next_addr);
        MEM_W32(entry_addr + 4u, (uint32_t)s_adhoc_scans[i].channel);
        for (uint32_t k = 0u; k < 8u; k++) MEM_W8(entry_addr + 8u + k, s_adhoc_scans[i].group[k]);
        for (uint32_t k = 0u; k < 6u; k++) MEM_W8(entry_addr + 16u + k, s_adhoc_scans[i].bssid[k]);
        MEM_W16(entry_addr + 22u, 0u);
        MEM_W32(entry_addr + 24u, (uint32_t)s_adhoc_scans[i].mode);
    }
    MEM_W32(A0, (uint32_t)max_entries * ADHOCCTL_SCAN_INFO_SIZE);
    return 0u;
}

static uint32_t h_NetAdhocctlGetPeerInfo(CpuState *s) {
    if (!s_adhocctl.inited) return SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
    if (!s_adhocctl.online_active) {
        if (!adhocctl_query_settled("sceNetAdhocctlGetPeerInfo")) return HLE_KERNEL_ERROR_NOT_IMPLEMENTED;
        if (s_adhocctl.state != 1 || !A0 || !sr_guest_span_readable(A0, 6u))
            return hle_unmodelled_stop("sceNetAdhocctlGetPeerInfo", "with mac 0x%08x in state %d "
                                       "(UNMEASURED)", A0, s_adhocctl.state);
        return ADHOC_ERROR_NO_ENTRY;
    }
    if (!A0 || !sr_guest_span_readable(A0, 6u) || !A2) return ADHOCCTL_ERROR_INVALID_ARG;
    if ((int32_t)A1 < (int32_t)ADHOCCTL_PEER_INFO_SIZE) return SCE_NET_ADHOC_ERROR_INVALID_BUFLEN;
    if (!sr_guest_span_writable(A2, ADHOCCTL_PEER_INFO_SIZE))
        return hle_unmodelled_stop("sceNetAdhocctlGetPeerInfo", "with unwritable output 0x%08x", A2);
    if (s_adhocctl.state != 1) return ADHOCCTL_ERROR_DISCONNECTED;
    uint8_t target[6];
    for (uint32_t i = 0u; i < 6u; i++) target[i] = MEM_R8(A0 + i);
    uint8_t self_mac[6];
    if (adhoc_local_mac(self_mac) && is_mac_match(self_mac, target)) return ADHOC_ERROR_NO_ENTRY;
    AdhocPeerEntry *peer = adhoc_find_peer(target);
    if (!peer) return ADHOC_ERROR_NO_ENTRY;
    const uint64_t now = sched_vtime_us();
    const uint64_t floor_ts = now > 10000u ? now - 10000u : 0u;
    if (peer->last_recv < floor_ts) peer->last_recv = floor_ts;
    write_peer_info_emu(A2, 0u, target, peer->nickname, peer->last_recv);
    return 0u;
}

static uint32_t h_NetAdhocctlGetNameByAddr(CpuState *s) {
    if (!adhoc_online_mode()) return hle_adhoc_refuse_offline(s);
    if (!s_adhocctl.inited) return SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
    if (!A0 || !sr_guest_span_readable(A0, 6u) || !A1 || !sr_guest_span_writable(A1, 128u))
        return ADHOCCTL_ERROR_INVALID_ARG;
    uint8_t target[6];
    for (uint32_t i = 0u; i < 6u; i++) target[i] = MEM_R8(A0 + i);
    uint8_t self_mac[6];
    if (adhoc_local_mac(self_mac) && is_mac_match(self_mac, target)) return ADHOC_ERROR_NO_ENTRY;
    const AdhocPeerEntry *peer = adhoc_find_peer(target);
    if (!peer) return ADHOC_ERROR_NO_ENTRY;
    for (uint32_t i = 0u; i < 128u; i++) MEM_W8(A1 + i, peer->nickname[i]);
    return 0u;
}

static uint32_t h_NetAdhocctlGetAddrByName(CpuState *s) {
    if (!adhoc_online_mode()) return hle_adhoc_refuse_offline(s);
    if (!s_adhocctl.inited) return SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
    char nick[128];
    if (!A0 || guest_cstr(A0, nick, (int)sizeof(nick)) < 0 ||
        !A1 || !sr_guest_span_writable(A1, 4u))
        return ADHOCCTL_ERROR_INVALID_ARG;
    int matches = 0;
    for (int i = 0; i < ADHOC_MAX_PEERS; i++) {
        if (s_adhoc_peers[i].active &&
            strncmp((const char *)s_adhoc_peers[i].nickname, nick, 127u) == 0)
            matches++;
    }
    if (!A2) {
        MEM_W32(A1, (uint32_t)matches * ADHOCCTL_PEER_INFO_SIZE);
        return 0u;
    }
    const int32_t req_bytes = (int32_t)MEM_R32(A1);
    if (req_bytes < 0) return ADHOCCTL_ERROR_INVALID_ARG;
    int max_entries = (int)((uint32_t)req_bytes / ADHOCCTL_PEER_INFO_SIZE);
    if (max_entries > matches) max_entries = matches;
    if (max_entries > 0 && !sr_guest_span_writable(A2, (uint32_t)max_entries * ADHOCCTL_PEER_INFO_SIZE))
        return hle_unmodelled_stop("sceNetAdhocctlGetAddrByName", "with unwritable buffer 0x%08x", A2);
    int written = 0;
    for (int i = 0; i < ADHOC_MAX_PEERS && written < max_entries; i++) {
        if (!s_adhoc_peers[i].active ||
            strncmp((const char *)s_adhoc_peers[i].nickname, nick, 127u) != 0)
            continue;
        const uint32_t addr = A2 + (uint32_t)written * ADHOCCTL_PEER_INFO_SIZE;
        const uint32_t next_addr = (written + 1 < max_entries) ? addr + ADHOCCTL_PEER_INFO_SIZE : 0u;
        write_peer_info_emu(addr, next_addr, s_adhoc_peers[i].mac,
                            s_adhoc_peers[i].nickname, s_adhoc_peers[i].last_recv);
        written++;
    }
    MEM_W32(A1, (uint32_t)written * ADHOCCTL_PEER_INFO_SIZE);
    return 0u;
}

/* sceNetGetLocalEtherAddr(u8 *mac): the address exists only once an ad hoc group operation or an
 * access-point connection has started. Before that a PSP returns 0x80410180 and leaves the buffer
 * untouched (pspautotests net/adhoc/apctlinit, before, during and after sceNetApctlInit; also
 * adhocctl.expected:23 after sceNetAdhocctlInit, and net/adhoc/pdp before a group). In a group it
 * is the console's MAC (adhocctl.expected:51 "local", the address sceWlanGetEtherAddr gives), also
 * after sceNetAdhocTerm (term.expected:11); after sceNetAdhocctlTerm it is unavailable again
 * (term.expected:26). After a scan or a group has ended, and while a request is in flight, the
 * answer is UNMEASURED. */
static uint32_t h_NetGetLocalEtherAddr(CpuState *s) {
    if (!s_adhocctl.inited || !s_adhocctl.radio_used) return SCE_NET_ERROR_LOCAL_ADDR_UNAVAILABLE;
    if (s_adhocctl.state != 1 || s_adhocctl.request != ADHOCCTL_REQ_NONE) {
        if (s_adhocctl.online_active) return SCE_NET_ERROR_LOCAL_ADDR_UNAVAILABLE;
        return hle_unmodelled_stop("sceNetGetLocalEtherAddr", "after a scan or group request, out of "
                                   "a group (UNMEASURED)");
    }
    if (!A0 || !sr_guest_span_writable(A0, 6u))
        return hle_unmodelled_stop("sceNetGetLocalEtherAddr", "with output 0x%08x (UNMEASURED)", A0);
    if (!console_profile_identity())
        return hle_unmodelled_stop("sceNetGetLocalEtherAddr", "has no address: the host's random "
                                   "source is unavailable");
    for (uint32_t i = 0; i < 6u; i++) MEM_W8(A0 + i, s_console.mac[i]);
    return 0u;
}

/* ---- sceNetAdhoc (PDP / PTP / PollSocket / SocketAlert) -----------------------------------
 * PPSSPP-derived, not PSP-measured (derived-translated from PPSSPP rev 3bc60270
 * Core/HLE/{proAdhoc.cpp,proAdhoc.h,sceNetAdhoc.cpp} and Common/Net/NetAdhocCommon.h). */

#define ADHOC_MAX_SOCKETS 255
#define ADHOC_SOCK_PDP 1
#define ADHOC_SOCK_PTP 2
#define ADHOC_LOCAL_LOOP_QUEUE 32

typedef struct LocalDgram {
    uint8_t src_mac[6];
    uint16_t sport;
    uint32_t len;
    uint8_t *data;
} LocalDgram;

typedef struct AdhocSockSlot {
    int used;
    uint32_t gen;
    int type; /* ADHOC_SOCK_PDP / ADHOC_SOCK_PTP */
    int transport_handle;
    int flags;         /* ADHOC_F_ALERT* */
    int alerted_flags;
    uint32_t bufsize;
    int state;         /* PTP state: ADHOC_PTP_STATE_* */
    uint8_t laddr[6];
    uint16_t lport;
    uint8_t paddr[6];
    uint16_t pport;
    int is_client;
    int ptp_connected_once;
    LocalDgram local_q[ADHOC_LOCAL_LOOP_QUEUE];
    unsigned local_head;
    unsigned local_count;
} AdhocSockSlot;

static struct {
    int inited;
    uint32_t gen;
    uint32_t next_sock_gen;
    AdhocSockSlot slots[ADHOC_MAX_SOCKETS];
    uint16_t next_ephemeral_port;
} s_adhoc;

static void adhoc_free_local_q(AdhocSockSlot *sk) {
    for (unsigned i = 0u; i < sk->local_count; i++) {
        LocalDgram *d = &sk->local_q[(sk->local_head + i) % ADHOC_LOCAL_LOOP_QUEUE];
        free(d->data);
        d->data = NULL;
    }
    sk->local_head = 0u;
    sk->local_count = 0u;
}

static AdhocSockSlot *adhoc_sock_get(int id) {
    if (id < 1 || id > ADHOC_MAX_SOCKETS || !s_adhoc.slots[id - 1].used) return NULL;
    return &s_adhoc.slots[id - 1];
}

static int adhoc_pdp_port_in_use(uint16_t port) {
    for (int i = 0; i < ADHOC_MAX_SOCKETS; i++) {
        if (s_adhoc.slots[i].used && s_adhoc.slots[i].type == ADHOC_SOCK_PDP &&
            s_adhoc.slots[i].lport == port)
            return 1;
    }
    return 0;
}

static int adhoc_ptp_port_in_use(uint16_t port, int for_listen, const uint8_t dst[6], uint16_t dport) {
    for (int i = 0; i < ADHOC_MAX_SOCKETS; i++) {
        const AdhocSockSlot *sk = &s_adhoc.slots[i];
        if (!sk->used || sk->type != ADHOC_SOCK_PTP || sk->lport != port) continue;
        if (for_listen) {
            if (sk->state == ADHOC_PTP_STATE_LISTEN) return 1;
        } else if (sk->state == ADHOC_PTP_STATE_ESTABLISHED && sk->pport == dport && dst &&
                   is_mac_match(sk->paddr, dst)) {
            return 1;
        }
    }
    return 0;
}

static uint16_t adhoc_alloc_ephemeral_port(int is_ptp) {
    if (s_adhoc.next_ephemeral_port < 0x1000u || s_adhoc.next_ephemeral_port >= 0x8000u)
        s_adhoc.next_ephemeral_port = 0x1000u;
    for (unsigned tries = 0u; tries < 0x7000u; tries++) {
        const uint16_t port = s_adhoc.next_ephemeral_port++;
        if (s_adhoc.next_ephemeral_port >= 0x8000u) s_adhoc.next_ephemeral_port = 0x1000u;
        if (!is_ptp && !adhoc_pdp_port_in_use(port)) return port;
        if (is_ptp && !adhoc_ptp_port_in_use(port, 1, NULL, 0u)) return port;
    }
    return 0u;
}

static void adhoc_close_slot_internal(int id) {
    AdhocSockSlot *sk = adhoc_sock_get(id);
    if (!sk) return;
    NkAdhocTransport *t = s_adhoc_active_transport;
    if (sk->transport_handle >= 0 && t && t->ops) {
        if (sk->type == ADHOC_SOCK_PDP && t->ops->pdp_close) {
            (void)t->ops->pdp_close(t, sk->transport_handle);
        } else if (sk->type == ADHOC_SOCK_PTP && t->ops->ptp_close) {
            (void)t->ops->ptp_close(t, sk->transport_handle);
        }
    }
    adhoc_free_local_q(sk);
    memset(sk, 0, sizeof(*sk));
    sk->transport_handle = -1;
}

static void adhoc_pump_all(void) {
    NkAdhocTransport *t = s_adhoc_active_transport;
    if (!t || !t->ops) return;
    if (t->ops->pump) t->ops->pump(t);
    for (int i = 0; i < ADHOC_MAX_SOCKETS; i++) {
        AdhocSockSlot *sk = &s_adhoc.slots[i];
        if (!sk->used || sk->type != ADHOC_SOCK_PTP || sk->transport_handle < 0) continue;
        if (sk->state == ADHOC_PTP_STATE_SYN_SENT && t->ops->ptp_connect_poll) {
            const int poll = t->ops->ptp_connect_poll(t, sk->transport_handle);
            if (poll == 1) {
                sk->state = ADHOC_PTP_STATE_ESTABLISHED;
                sk->ptp_connected_once = 1;
            } else if (poll < 0) {
                sk->state = ADHOC_PTP_STATE_CLOSED;
            }
        }
    }
}

static int adhoc_pdp_open_internal(uint16_t port, uint32_t bufsize) {
    NkAdhocTransport *t = s_adhoc_active_transport;
    if (!s_adhoc.inited || !t || !t->ops || !t->ops->pdp_open) return -1;
    uint8_t self_mac[6];
    if (!adhoc_local_mac(self_mac)) return -1;
    if (port == 0u) port = adhoc_alloc_ephemeral_port(0);
    if (port == 0u || adhoc_pdp_port_in_use(port)) return -1;
    int free_idx = -1;
    for (int i = 0; i < ADHOC_MAX_SOCKETS; i++) {
        if (!s_adhoc.slots[i].used) { free_idx = i; break; }
    }
    if (free_idx < 0) return -1;
    const int th = t->ops->pdp_open(t, port);
    if (th < 0) return -1;
    AdhocSockSlot *sk = &s_adhoc.slots[free_idx];
    memset(sk, 0, sizeof(*sk));
    sk->used = 1;
    sk->gen = ++s_adhoc.next_sock_gen;
    sk->type = ADHOC_SOCK_PDP;
    sk->transport_handle = th;
    sk->bufsize = bufsize ? bufsize : PSP_ADHOC_PDP_MFS;
    memcpy(sk->laddr, self_mac, 6u);
    sk->lport = port;
    return free_idx + 1;
}

static uint32_t adhoc_pdp_deliver_local(uint16_t sport, uint16_t dport, const void *data, uint32_t len) {
    uint8_t self_mac[6];
    if (!adhoc_local_mac(self_mac)) return 0u;
    for (int i = 0; i < ADHOC_MAX_SOCKETS; i++) {
        AdhocSockSlot *dst = &s_adhoc.slots[i];
        if (!dst->used || dst->type != ADHOC_SOCK_PDP || dst->lport != dport) continue;
        if (dst->local_count >= ADHOC_LOCAL_LOOP_QUEUE) return 0u;
        uint8_t *copy = len ? malloc(len) : NULL;
        if (len && !copy) return 0u;
        if (len) memcpy(copy, data, len);
        LocalDgram *d = &dst->local_q[(dst->local_head + dst->local_count) % ADHOC_LOCAL_LOOP_QUEUE];
        memcpy(d->src_mac, self_mac, 6u);
        d->sport = sport;
        d->len = len;
        d->data = copy;
        dst->local_count++;
        return 0u;
    }
    return 0u;
}

static uint32_t adhoc_pdp_send_raw(int id, const uint8_t dst_mac[6], uint16_t dport,
                                   const void *data, uint32_t len) {
    AdhocSockSlot *sk = adhoc_sock_get(id);
    if (!sk || sk->type != ADHOC_SOCK_PDP) return SCE_NET_ADHOC_ERROR_INVALID_SOCKET_ID;
    if (sk->flags & ADHOC_F_ALERTSEND) {
        sk->alerted_flags |= ADHOC_F_ALERTSEND;
        return SCE_NET_ADHOC_ERROR_SOCKET_ALERTED;
    }
    uint8_t self_mac[6];
    if (!adhoc_local_mac(self_mac)) return SCE_NET_ERROR_INTERNAL;
    if (!is_broadcast_mac(dst_mac) && is_mac_match(self_mac, dst_mac)) {
        return adhoc_pdp_deliver_local(sk->lport, dport, data, len);
    }
    if (s_adhocctl.state != 1) return 0u; /* out of a group, datagrams drop silently */
    NkAdhocTransport *t = s_adhoc_active_transport;
    if (!t || !t->ops || !t->ops->pdp_send) return SCE_NET_ERROR_INTERNAL;
    if (is_broadcast_mac(dst_mac)) {
        for (int i = 0; i < ADHOC_MAX_PEERS; i++) {
            if (!s_adhoc_peers[i].active) continue;
            (void)t->ops->pdp_send(t, sk->transport_handle, s_adhoc_peers[i].mac, dport, data, len);
        }
        return 0u;
    }
    uint8_t target[6];
    memcpy(target, dst_mac, 6u);
    adhoc_fix_mac(target);
    const int rc = t->ops->pdp_send(t, sk->transport_handle, target, dport, data, len);
    if (rc == NK_NET_TOO_BIG) return SCE_NET_ADHOC_ERROR_INVALID_DATALEN;
    if (rc == NK_NET_AGAIN) return SCE_NET_ADHOC_ERROR_WOULD_BLOCK;
    if (rc < 0 && rc != NK_NET_NOT_IN_GROUP) return SCE_NET_ERROR_INTERNAL;
    return 0u;
}

static uint32_t adhoc_pdp_recv_raw(int id, uint8_t src_mac[6], uint16_t *sport, void *buf,
                                   uint32_t cap, uint32_t *out_len) {
    AdhocSockSlot *sk = adhoc_sock_get(id);
    if (!sk || sk->type != ADHOC_SOCK_PDP) return SCE_NET_ADHOC_ERROR_INVALID_SOCKET_ID;
    if (sk->local_count > 0u) {
        LocalDgram *d = &sk->local_q[sk->local_head];
        *out_len = d->len;
        if (d->len > cap) return SCE_NET_ADHOC_ERROR_NOT_ENOUGH_SPACE;
        if (src_mac) memcpy(src_mac, d->src_mac, 6u);
        if (sport) *sport = d->sport;
        if (d->len && buf) memcpy(buf, d->data, d->len);
        free(d->data);
        d->data = NULL;
        sk->local_head = (sk->local_head + 1u) % ADHOC_LOCAL_LOOP_QUEUE;
        sk->local_count--;
        return 0u;
    }
    NkAdhocTransport *t = s_adhoc_active_transport;
    if (!t || !t->ops || !t->ops->pdp_recv) return SCE_NET_ADHOC_ERROR_WOULD_BLOCK;
    uint8_t tmp_mac[6];
    uint16_t tmp_port = 0u;
    const int rc = t->ops->pdp_recv(t, sk->transport_handle, tmp_mac, &tmp_port, buf, cap, out_len);
    if (rc == NK_NET_OK) {
        if (src_mac) memcpy(src_mac, tmp_mac, 6u);
        if (sport) *sport = tmp_port;
        return 0u;
    }
    if (rc == NK_NET_SHORT_BUFFER) return SCE_NET_ADHOC_ERROR_NOT_ENOUGH_SPACE;
    return SCE_NET_ADHOC_ERROR_WOULD_BLOCK;
}

static uint32_t h_NetAdhocInit(CpuState *s) {
    if (!adhoc_online_mode()) return hle_adhoc_refuse_offline(s);
    adhoc_log_ppsspp_tier_once();
    if (s_adhoc.inited) return SCE_NET_ADHOC_ERROR_ALREADY_INITIALIZED;
    s_adhoc.inited = 1;
    s_adhoc.gen++;
    return 0u;
}

static uint32_t h_NetAdhocTerm(CpuState *s) {
    if (!adhoc_online_mode()) return hle_adhoc_refuse_offline(s);
    for (int i = 0; i < ADHOC_MAX_SOCKETS; i++) {
        if (s_adhoc.slots[i].used) adhoc_close_slot_internal(i + 1);
    }
    s_adhoc.inited = 0;
    s_adhoc.gen++;
    return 0u;
}

static uint32_t h_NetAdhocPdpCreate(CpuState *s) {
    if (!adhoc_online_mode()) return hle_adhoc_refuse_offline(s);
    if (!s_netlib.inited) return 0x800201cau;
    if (!s_adhoc.inited) return SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
    const uint32_t mac_ptr = A0, flag = A3;
    int32_t port = (int32_t)(uint16_t)A1;
    const int32_t bufsize = (int32_t)A2;
    if (!mac_ptr || !sr_guest_span_readable(mac_ptr, 6u) || flag != 0u)
        return SCE_NET_ADHOC_ERROR_INVALID_ARG;
    uint8_t req_mac[6];
    for (uint32_t i = 0u; i < 6u; i++) req_mac[i] = MEM_R8(mac_ptr + i);
    if (!s_adhocctl.inited || is_zero_mac(req_mac) || (req_mac[0] & 1u) != 0u)
        return SCE_NET_ADHOC_ERROR_INVALID_ADDR;
    if (port == 0xffff) return SCE_NET_ADHOC_ERROR_INVALID_PORT;
    if (port != 0 && adhoc_pdp_port_in_use((uint16_t)port)) return SCE_NET_ADHOC_ERROR_PORT_IN_USE;
    uint32_t effective_buf = bufsize > 0 ? (uint32_t)bufsize : PSP_ADHOC_PDP_MFS;
    const int id = adhoc_pdp_open_internal((uint16_t)port, effective_buf);
    if (id < 0) return SCE_NET_ADHOC_ERROR_SOCKET_ID_NOT_AVAIL;
    s_adhoc.slots[id - 1].is_client = (port == 0);
    return (uint32_t)id;
}

static uint32_t h_NetAdhocPdpDelete(CpuState *s) {
    if (!adhoc_online_mode()) return hle_adhoc_refuse_offline(s);
    if (!s_adhoc.inited) return SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
    const int id = (int)(int32_t)A0;
    AdhocSockSlot *sk = adhoc_sock_get(id);
    if (!sk || sk->type != ADHOC_SOCK_PDP) return SCE_NET_ADHOC_ERROR_INVALID_SOCKET_ID;
    adhoc_close_slot_internal(id);
    return 0u;
}

static uint32_t h_NetAdhocPdpSend(CpuState *s) {
    if (!adhoc_online_mode()) return hle_adhoc_refuse_offline(s);
    const int id = (int)(int32_t)A0;
    const uint32_t mac_ptr = A1, port = A2, data_ptr = A3;
    const int32_t len = (int32_t)stack_arg(s, 0);
    const uint32_t flag = stack_arg(s, 2);
    if (!mac_ptr || !sr_guest_span_readable(mac_ptr, 6u) || (flag & ~ADHOC_F_NONBLOCK) != 0u)
        return SCE_NET_ADHOC_ERROR_INVALID_ARG;
    if (!s_adhoc.inited) return SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
    if (!adhoc_sock_get(id) || adhoc_sock_get(id)->type != ADHOC_SOCK_PDP)
        return SCE_NET_ADHOC_ERROR_INVALID_SOCKET_ID;
    uint8_t daddr[6];
    for (uint32_t i = 0u; i < 6u; i++) daddr[i] = MEM_R8(mac_ptr + i);
    if (is_zero_mac(daddr) || ((daddr[0] & 1u) != 0u && !is_broadcast_mac(daddr)))
        return SCE_NET_ADHOC_ERROR_INVALID_ADDR;
    const uint16_t dport = (uint16_t)port;
    if (dport == 0u || dport == 0xffffu) return SCE_NET_ADHOC_ERROR_INVALID_PORT;
    if (len < 0 || (uint32_t)len > PSP_ADHOC_PDP_MTU || (data_ptr == 0u && len != 0))
        return SCE_NET_ADHOC_ERROR_INVALID_DATALEN;
    if (len > 0 && !sr_guest_span_readable(data_ptr, (uint32_t)len))
        return hle_unmodelled_stop("sceNetAdhocPdpSend", "with unreadable payload 0x%08x", data_ptr);
    uint8_t *tmp = len > 0 ? malloc((size_t)len) : NULL;
    if (len > 0 && !tmp) return SCE_NET_ERROR_NO_SPACE;
    for (int32_t i = 0; i < len; i++) tmp[i] = MEM_R8(data_ptr + (uint32_t)i);
    const uint32_t rc = adhoc_pdp_send_raw(id, daddr, dport, tmp, (uint32_t)len);
    free(tmp);
    return rc;
}

static uint32_t h_NetAdhocPdpRecv(CpuState *s) {
    if (!adhoc_online_mode()) return hle_adhoc_refuse_offline(s);
    const int id = (int)(int32_t)A0;
    const uint32_t addr_ptr = A1, port_ptr = A2, buf_ptr = A3;
    const uint32_t len_ptr = stack_arg(s, 0);
    const uint32_t timeout = stack_arg(s, 1);
    const uint32_t flag = stack_arg(s, 2);
    if (!buf_ptr || !len_ptr || !sr_guest_span_writable(len_ptr, 4u) ||
        (flag & ~ADHOC_F_NONBLOCK) != 0u)
        return SCE_NET_ADHOC_ERROR_INVALID_ARG;
    if (!s_adhoc.inited) return SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
    AdhocSockSlot *sk = adhoc_sock_get(id);
    if (!sk || sk->type != ADHOC_SOCK_PDP) return SCE_NET_ADHOC_ERROR_INVALID_SOCKET_ID;
    if (addr_ptr && !sr_guest_span_writable(addr_ptr, 6u))
        return hle_unmodelled_stop("sceNetAdhocPdpRecv", "with unwritable addr 0x%08x", addr_ptr);
    if (port_ptr && !sr_guest_span_writable(port_ptr, 2u))
        return hle_unmodelled_stop("sceNetAdhocPdpRecv", "with unwritable port 0x%08x", port_ptr);
    if (sk->flags & ADHOC_F_ALERTRECV) {
        sk->alerted_flags |= ADHOC_F_ALERTRECV;
        return SCE_NET_ADHOC_ERROR_SOCKET_ALERTED;
    }
    const int32_t cap_s = (int32_t)MEM_R32(len_ptr);
    const uint32_t cap = cap_s > 0 ? (uint32_t)cap_s : 0u;
    if (cap > 0u && !sr_guest_span_writable(buf_ptr, cap))
        return hle_unmodelled_stop("sceNetAdhocPdpRecv", "with unwritable buf 0x%08x", buf_ptr);
    uint8_t *tmp = cap > 0u ? malloc(cap) : NULL;
    if (cap > 0u && !tmp) return SCE_NET_ERROR_NO_SPACE;

    const uint32_t mod_gen = s_adhoc.gen;
    const uint32_t sock_gen = sk->gen;
    const uint64_t deadline = timeout ? sched_vtime_us() + timeout : UINT64_MAX;
    uint32_t got = 0u, rc = SCE_NET_ADHOC_ERROR_WOULD_BLOCK;
    uint8_t src_mac[6] = {0};
    uint16_t sport = 0u;
    for (;;) {
        adhoc_pump_all();
        rc = adhoc_pdp_recv_raw(id, src_mac, &sport, tmp, cap, &got);
        if (rc != SCE_NET_ADHOC_ERROR_WOULD_BLOCK) break;
        if (flag & ADHOC_F_NONBLOCK) break;
        if (sched_vtime_us() >= deadline) {
            rc = SCE_NET_ADHOC_ERROR_TIMEOUT;
            break;
        }
        sched_delay_current(ADHOC_POLL_WAIT_STEP_US);
        if (s_adhoc.gen != mod_gen || !s_adhoc.inited) { rc = SCE_NET_ADHOC_ERROR_THREAD_ABORTED; break; }
        AdhocSockSlot *cur = adhoc_sock_get(id);
        if (!cur || cur->gen != sock_gen) { rc = SCE_NET_ADHOC_ERROR_SOCKET_DELETED; break; }
        if (cur->flags & ADHOC_F_ALERTRECV) {
            cur->alerted_flags |= ADHOC_F_ALERTRECV;
            rc = SCE_NET_ADHOC_ERROR_SOCKET_ALERTED;
            break;
        }
    }
    if (rc == 0u) {
        for (uint32_t i = 0u; i < got; i++) MEM_W8(buf_ptr + i, tmp[i]);
        MEM_W32(len_ptr, got);
        if (addr_ptr) {
            for (uint32_t i = 0u; i < 6u; i++) MEM_W8(addr_ptr + i, src_mac[i]);
        }
        if (port_ptr) MEM_W16(port_ptr, sport);
    } else if (rc == SCE_NET_ADHOC_ERROR_NOT_ENOUGH_SPACE) {
        MEM_W32(len_ptr, got);
    }
    free(tmp);
    return rc;
}

static uint32_t h_NetAdhocGetPdpStat(CpuState *s) {
    if (!adhoc_online_mode()) return hle_adhoc_refuse_offline(s);
    if (!s_adhoc.inited) return SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
    if (!A0 || !sr_guest_span_writable(A0, 4u)) return SCE_NET_ADHOC_ERROR_INVALID_ARG;
    adhoc_pump_all();
    int total = 0;
    for (int i = 0; i < ADHOC_MAX_SOCKETS; i++) {
        if (s_adhoc.slots[i].used && s_adhoc.slots[i].type == ADHOC_SOCK_PDP) total++;
    }
    if (!A1) {
        MEM_W32(A0, (uint32_t)total * 20u);
        return 0u;
    }
    const int32_t req = (int32_t)MEM_R32(A0);
    int max_n = req > 0 ? (int)((uint32_t)req / 20u) : 0;
    if (max_n > total) max_n = total;
    if (max_n > 0 && !sr_guest_span_writable(A1, (uint32_t)max_n * 20u))
        return hle_unmodelled_stop("sceNetAdhocGetPdpStat", "with unwritable buf 0x%08x", A1);
    NkAdhocTransport *t = s_adhoc_active_transport;
    int written = 0;
    for (int i = 0; i < ADHOC_MAX_SOCKETS && written < max_n; i++) {
        const AdhocSockSlot *sk = &s_adhoc.slots[i];
        if (!sk->used || sk->type != ADHOC_SOCK_PDP) continue;
        uint32_t rx_cc = (t && t->ops && t->ops->pdp_peek) ? t->ops->pdp_peek(t, sk->transport_handle) : 0u;
        if (sk->local_count > 0u) rx_cc += sk->local_q[sk->local_head].len;
        const uint32_t addr = A1 + (uint32_t)written * 20u;
        MEM_W32(addr, (written + 1 < max_n) ? addr + 20u : 0u);
        MEM_W32(addr + 4u, (uint32_t)(i + 1));
        for (uint32_t k = 0u; k < 6u; k++) MEM_W8(addr + 8u + k, sk->laddr[k]);
        MEM_W16(addr + 14u, sk->lport);
        MEM_W32(addr + 16u, rx_cc);
        written++;
    }
    MEM_W32(A0, (uint32_t)written * 20u);
    return 0u;
}

static uint32_t ptp_poll_connect_blocking(int id, uint32_t timeout, uint32_t flag) {
    AdhocSockSlot *sk = adhoc_sock_get(id);
    if (!sk || sk->type != ADHOC_SOCK_PTP) return SCE_NET_ADHOC_ERROR_INVALID_SOCKET_ID;
    const uint32_t mod_gen = s_adhoc.gen;
    const uint32_t sock_gen = sk->gen;
    const uint64_t deadline = timeout ? sched_vtime_us() + timeout : UINT64_MAX;
    for (;;) {
        adhoc_pump_all();
        sk = adhoc_sock_get(id);
        if (!sk || sk->gen != sock_gen || s_adhoc.gen != mod_gen)
            return SCE_NET_ADHOC_ERROR_SOCKET_DELETED;
        if (sk->state == ADHOC_PTP_STATE_ESTABLISHED) return 0u;
        if (sk->state == ADHOC_PTP_STATE_CLOSED) return SCE_NET_ADHOC_ERROR_CONNECTION_REFUSED;
        if (flag & ADHOC_F_NONBLOCK) return SCE_NET_ADHOC_ERROR_WOULD_BLOCK;
        if (sched_vtime_us() >= deadline) return SCE_NET_ADHOC_ERROR_TIMEOUT;
        sched_delay_current(ADHOC_POLL_WAIT_STEP_US);
    }
}

static uint32_t h_NetAdhocPtpOpen(CpuState *s) {
    if (!adhoc_online_mode()) return hle_adhoc_refuse_offline(s);
    if (!s_netlib.inited) return 0x800201cau;
    if (!s_adhoc.inited) return SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
    const uint32_t smac_ptr = A0, dmac_ptr = A2;
    uint16_t sport = (uint16_t)A1;
    const uint16_t dport = (uint16_t)A3;
    const int32_t bufsize = (int32_t)stack_arg(s, 0);
    const uint32_t flag = stack_arg(s, 3);
    if (!smac_ptr || !sr_guest_span_readable(smac_ptr, 6u) ||
        !dmac_ptr || !sr_guest_span_readable(dmac_ptr, 6u) || flag != 0u)
        return SCE_NET_ADHOC_ERROR_INVALID_ARG;
    uint8_t saddr[6], daddr[6];
    for (uint32_t i = 0u; i < 6u; i++) {
        saddr[i] = MEM_R8(smac_ptr + i);
        daddr[i] = MEM_R8(dmac_ptr + i);
    }
    if (!s_adhocctl.inited || is_zero_mac(saddr) || (saddr[0] & 1u) != 0u ||
        is_zero_mac(daddr) || (daddr[0] & 1u) != 0u)
        return SCE_NET_ADHOC_ERROR_INVALID_ADDR;
    if (sport == 0xffffu || dport == 0u || dport == 0xffffu) return SCE_NET_ADHOC_ERROR_INVALID_PORT;
    if (sport != 0u && adhoc_ptp_port_in_use(sport, 0, daddr, dport))
        return SCE_NET_ADHOC_ERROR_PORT_IN_USE;
    if (sport == 0u) sport = adhoc_alloc_ephemeral_port(1);
    if (sport == 0u) return SCE_NET_ADHOC_ERROR_SOCKET_ID_NOT_AVAIL;
    uint8_t self_mac[6];
    if (!adhoc_local_mac(self_mac)) return SCE_NET_ERROR_INTERNAL;
    if (is_mac_match(self_mac, daddr) && !adhoc_ptp_port_in_use(dport, 1, NULL, 0u))
        return SCE_NET_ADHOC_ERROR_CONNECTION_REFUSED;
    int free_idx = -1;
    for (int i = 0; i < ADHOC_MAX_SOCKETS; i++) {
        if (!s_adhoc.slots[i].used) { free_idx = i; break; }
    }
    if (free_idx < 0) return SCE_NET_ADHOC_ERROR_SOCKET_ID_NOT_AVAIL;
    NkAdhocTransport *t = s_adhoc_active_transport;
    if (!t || !t->ops || !t->ops->ptp_open) return SCE_NET_ERROR_INTERNAL;
    uint8_t target[6];
    memcpy(target, daddr, 6u);
    adhoc_fix_mac(target);
    const int th = t->ops->ptp_open(t, sport, target, dport);
    if (th == NK_NET_PORT_IN_USE) return SCE_NET_ADHOC_ERROR_PORT_IN_USE;
    if (th < 0) return SCE_NET_ADHOC_ERROR_CONNECTION_REFUSED;
    AdhocSockSlot *sk = &s_adhoc.slots[free_idx];
    memset(sk, 0, sizeof(*sk));
    sk->used = 1;
    sk->gen = ++s_adhoc.next_sock_gen;
    sk->type = ADHOC_SOCK_PTP;
    sk->transport_handle = th;
    sk->state = ADHOC_PTP_STATE_SYN_SENT;
    sk->bufsize = bufsize > 0 ? (uint32_t)bufsize : PSP_ADHOC_PDP_MFS;
    memcpy(sk->laddr, self_mac, 6u);
    sk->lport = sport;
    memcpy(sk->paddr, daddr, 6u);
    sk->pport = dport;
    sk->is_client = 1;
    adhoc_pump_all();
    return (uint32_t)(free_idx + 1);
}

static uint32_t h_NetAdhocPtpConnect(CpuState *s) {
    if (!adhoc_online_mode()) return hle_adhoc_refuse_offline(s);
    if (!s_adhoc.inited) return SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
    const int id = (int)(int32_t)A0;
    const uint32_t timeout = A1, flag = A2;
    if (flag & ~ADHOC_F_NONBLOCK) return SCE_NET_ADHOC_ERROR_INVALID_ARG;
    AdhocSockSlot *sk = adhoc_sock_get(id);
    if (!sk || sk->type != ADHOC_SOCK_PTP) return SCE_NET_ADHOC_ERROR_INVALID_SOCKET_ID;
    if (sk->flags & ADHOC_F_ALERTCONNECT) {
        sk->alerted_flags |= ADHOC_F_ALERTCONNECT;
        return SCE_NET_ADHOC_ERROR_SOCKET_ALERTED;
    }
    if (sk->state == ADHOC_PTP_STATE_ESTABLISHED) return 0u;
    if (sk->state != ADHOC_PTP_STATE_SYN_SENT) return SCE_NET_ADHOC_ERROR_CONNECTION_REFUSED;
    return ptp_poll_connect_blocking(id, timeout, flag);
}

static uint32_t h_NetAdhocPtpListen(CpuState *s) {
    if (!adhoc_online_mode()) return hle_adhoc_refuse_offline(s);
    if (!s_netlib.inited) return 0x800201cau;
    if (!s_adhoc.inited) return SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
    const uint32_t smac_ptr = A0;
    uint16_t sport = (uint16_t)A1;
    const int32_t bufsize = (int32_t)A2;
    const int32_t backlog = (int32_t)stack_arg(s, 1);
    const uint32_t flag = stack_arg(s, 2);
    if (!smac_ptr || !sr_guest_span_readable(smac_ptr, 6u) || backlog <= 0 || flag != 0u)
        return SCE_NET_ADHOC_ERROR_INVALID_ARG;
    uint8_t saddr[6];
    for (uint32_t i = 0u; i < 6u; i++) saddr[i] = MEM_R8(smac_ptr + i);
    if (!s_adhocctl.inited || is_zero_mac(saddr) || (saddr[0] & 1u) != 0u)
        return SCE_NET_ADHOC_ERROR_INVALID_ADDR;
    if (sport == 0xffffu) return SCE_NET_ADHOC_ERROR_INVALID_PORT;
    if (sport != 0u && adhoc_ptp_port_in_use(sport, 1, NULL, 0u))
        return SCE_NET_ADHOC_ERROR_PORT_IN_USE;
    if (sport == 0u) sport = adhoc_alloc_ephemeral_port(1);
    if (sport == 0u) return SCE_NET_ADHOC_ERROR_SOCKET_ID_NOT_AVAIL;
    uint8_t self_mac[6];
    if (!adhoc_local_mac(self_mac)) return SCE_NET_ERROR_INTERNAL;
    int free_idx = -1;
    for (int i = 0; i < ADHOC_MAX_SOCKETS; i++) {
        if (!s_adhoc.slots[i].used) { free_idx = i; break; }
    }
    if (free_idx < 0) return SCE_NET_ADHOC_ERROR_SOCKET_ID_NOT_AVAIL;
    NkAdhocTransport *t = s_adhoc_active_transport;
    if (!t || !t->ops || !t->ops->ptp_listen) return SCE_NET_ERROR_INTERNAL;
    const int th = t->ops->ptp_listen(t, sport, backlog);
    if (th == NK_NET_PORT_IN_USE) return SCE_NET_ADHOC_ERROR_PORT_IN_USE;
    if (th < 0) return SCE_NET_ERROR_NO_SPACE;
    AdhocSockSlot *sk = &s_adhoc.slots[free_idx];
    memset(sk, 0, sizeof(*sk));
    sk->used = 1;
    sk->gen = ++s_adhoc.next_sock_gen;
    sk->type = ADHOC_SOCK_PTP;
    sk->transport_handle = th;
    sk->state = ADHOC_PTP_STATE_LISTEN;
    sk->bufsize = bufsize > 0 ? (uint32_t)bufsize : PSP_ADHOC_PDP_MFS;
    memcpy(sk->laddr, self_mac, 6u);
    sk->lport = sport;
    adhoc_pump_all();
    return (uint32_t)(free_idx + 1);
}

static uint32_t h_NetAdhocPtpAccept(CpuState *s) {
    if (!adhoc_online_mode()) return hle_adhoc_refuse_offline(s);
    if (!s_adhoc.inited) return SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
    const int id = (int)(int32_t)A0;
    const uint32_t pmac_ptr = A1, pport_ptr = A2, timeout = A3, flag = stack_arg(s, 0);
    if (flag & ~ADHOC_F_NONBLOCK) return SCE_NET_ADHOC_ERROR_INVALID_ARG;
    AdhocSockSlot *sk = adhoc_sock_get(id);
    if (!sk || sk->type != ADHOC_SOCK_PTP) return SCE_NET_ADHOC_ERROR_INVALID_SOCKET_ID;
    if (sk->state != ADHOC_PTP_STATE_LISTEN) return SCE_NET_ADHOC_ERROR_NOT_LISTENED;
    if (pmac_ptr && !sr_guest_span_writable(pmac_ptr, 6u))
        return hle_unmodelled_stop("sceNetAdhocPtpAccept", "with unwritable mac 0x%08x", pmac_ptr);
    if (pport_ptr && !sr_guest_span_writable(pport_ptr, 2u))
        return hle_unmodelled_stop("sceNetAdhocPtpAccept", "with unwritable port 0x%08x", pport_ptr);
    if (sk->flags & ADHOC_F_ALERTACCEPT) {
        sk->alerted_flags |= ADHOC_F_ALERTACCEPT;
        return SCE_NET_ADHOC_ERROR_SOCKET_ALERTED;
    }
    NkAdhocTransport *t = s_adhoc_active_transport;
    if (!t || !t->ops || !t->ops->ptp_accept) return SCE_NET_ERROR_INTERNAL;
    const uint32_t mod_gen = s_adhoc.gen;
    const uint32_t sock_gen = sk->gen;
    const uint64_t deadline = timeout ? sched_vtime_us() + timeout : UINT64_MAX;
    uint8_t peer_mac[6] = {0};
    uint16_t peer_port = 0u;
    int child_th = NK_NET_AGAIN;
    for (;;) {
        adhoc_pump_all();
        child_th = t->ops->ptp_accept(t, sk->transport_handle, peer_mac, &peer_port);
        if (child_th >= 0) break;
        if (child_th != NK_NET_AGAIN) return SCE_NET_ADHOC_ERROR_DISCONNECTED;
        if (flag & ADHOC_F_NONBLOCK) return SCE_NET_ADHOC_ERROR_WOULD_BLOCK;
        if (sched_vtime_us() >= deadline) return SCE_NET_ADHOC_ERROR_TIMEOUT;
        sched_delay_current(ADHOC_POLL_WAIT_STEP_US);
        if (s_adhoc.gen != mod_gen || !s_adhoc.inited) return SCE_NET_ADHOC_ERROR_THREAD_ABORTED;
        sk = adhoc_sock_get(id);
        if (!sk || sk->gen != sock_gen) return SCE_NET_ADHOC_ERROR_SOCKET_DELETED;
    }
    int free_idx = -1;
    for (int i = 0; i < ADHOC_MAX_SOCKETS; i++) {
        if (!s_adhoc.slots[i].used) { free_idx = i; break; }
    }
    if (free_idx < 0) {
        if (t->ops->ptp_close) (void)t->ops->ptp_close(t, child_th);
        return SCE_NET_ADHOC_ERROR_SOCKET_ID_NOT_AVAIL;
    }
    AdhocSockSlot *ch = &s_adhoc.slots[free_idx];
    memset(ch, 0, sizeof(*ch));
    ch->used = 1;
    ch->gen = ++s_adhoc.next_sock_gen;
    ch->type = ADHOC_SOCK_PTP;
    ch->transport_handle = child_th;
    ch->state = ADHOC_PTP_STATE_ESTABLISHED;
    ch->ptp_connected_once = 1;
    ch->bufsize = sk->bufsize;
    memcpy(ch->laddr, sk->laddr, 6u);
    ch->lport = sk->lport;
    memcpy(ch->paddr, peer_mac, 6u);
    ch->pport = peer_port;
    if (pmac_ptr) {
        for (uint32_t i = 0u; i < 6u; i++) MEM_W8(pmac_ptr + i, peer_mac[i]);
    }
    if (pport_ptr) MEM_W16(pport_ptr, peer_port);
    return (uint32_t)(free_idx + 1);
}

static uint32_t h_NetAdhocPtpSend(CpuState *s) {
    if (!adhoc_online_mode()) return hle_adhoc_refuse_offline(s);
    if (!s_adhoc.inited) return SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
    const int id = (int)(int32_t)A0;
    const uint32_t data_ptr = A1, size_ptr = A2, timeout = A3, flag = stack_arg(s, 0);
    if (!data_ptr || !size_ptr || !sr_guest_span_writable(size_ptr, 4u) ||
        (flag & ~ADHOC_F_NONBLOCK) != 0u)
        return SCE_NET_ADHOC_ERROR_INVALID_ARG;
    AdhocSockSlot *sk = adhoc_sock_get(id);
    if (!sk || sk->type != ADHOC_SOCK_PTP) return SCE_NET_ADHOC_ERROR_INVALID_SOCKET_ID;
    if (sk->state == ADHOC_PTP_STATE_CLOSED) return SCE_NET_ADHOC_ERROR_DISCONNECTED;
    if (sk->state != ADHOC_PTP_STATE_ESTABLISHED) return SCE_NET_ADHOC_ERROR_NOT_CONNECTED;
    if (sk->flags & ADHOC_F_ALERTSEND) {
        sk->alerted_flags |= ADHOC_F_ALERTSEND;
        return SCE_NET_ADHOC_ERROR_SOCKET_ALERTED;
    }
    const int32_t req_len_s = (int32_t)MEM_R32(size_ptr);
    if (req_len_s < 0) return SCE_NET_ADHOC_ERROR_INVALID_DATALEN;
    const uint32_t req_len = (uint32_t)req_len_s;
    if (req_len > 0u && !sr_guest_span_readable(data_ptr, req_len))
        return hle_unmodelled_stop("sceNetAdhocPtpSend", "with unreadable payload 0x%08x", data_ptr);
    NkAdhocTransport *t = s_adhoc_active_transport;
    if (!t || !t->ops || !t->ops->ptp_send) return SCE_NET_ERROR_INTERNAL;
    uint8_t *tmp = req_len > 0u ? malloc(req_len) : NULL;
    if (req_len > 0u && !tmp) return SCE_NET_ERROR_NO_SPACE;
    for (uint32_t i = 0u; i < req_len; i++) tmp[i] = MEM_R8(data_ptr + i);

    const uint32_t mod_gen = s_adhoc.gen;
    const uint32_t sock_gen = sk->gen;
    const uint64_t deadline = timeout ? sched_vtime_us() + timeout : UINT64_MAX;
    uint32_t total_sent = 0u, rc = 0u;
    while (total_sent < req_len) {
        uint32_t chunk_sent = 0u;
        adhoc_pump_all();
        const int sr = t->ops->ptp_send(t, sk->transport_handle, tmp + total_sent,
                                        req_len - total_sent, &chunk_sent);
        if (sr == NK_NET_OK) {
            total_sent += chunk_sent;
            if (total_sent == req_len || (flag & ADHOC_F_NONBLOCK)) break;
            continue;
        }
        if (sr == NK_NET_DEAD) {
            sk->state = ADHOC_PTP_STATE_CLOSED;
            rc = SCE_NET_ADHOC_ERROR_DISCONNECTED;
            break;
        }
        if (flag & ADHOC_F_NONBLOCK) {
            if (total_sent == 0u) rc = SCE_NET_ADHOC_ERROR_WOULD_BLOCK;
            break;
        }
        if (sched_vtime_us() >= deadline) {
            if (total_sent == 0u) rc = SCE_NET_ADHOC_ERROR_TIMEOUT;
            break;
        }
        sched_delay_current(ADHOC_POLL_WAIT_STEP_US);
        if (s_adhoc.gen != mod_gen || !s_adhoc.inited) { rc = SCE_NET_ADHOC_ERROR_THREAD_ABORTED; break; }
        sk = adhoc_sock_get(id);
        if (!sk || sk->gen != sock_gen) { rc = SCE_NET_ADHOC_ERROR_SOCKET_DELETED; break; }
    }
    free(tmp);
    MEM_W32(size_ptr, total_sent);
    return rc;
}

static uint32_t h_NetAdhocPtpRecv(CpuState *s) {
    if (!adhoc_online_mode()) return hle_adhoc_refuse_offline(s);
    if (!s_adhoc.inited) return SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
    const int id = (int)(int32_t)A0;
    const uint32_t buf_ptr = A1, size_ptr = A2, timeout = A3, flag = stack_arg(s, 0);
    if (!buf_ptr || !size_ptr || !sr_guest_span_writable(size_ptr, 4u) ||
        (flag & ~ADHOC_F_NONBLOCK) != 0u)
        return SCE_NET_ADHOC_ERROR_INVALID_ARG;
    AdhocSockSlot *sk = adhoc_sock_get(id);
    if (!sk || sk->type != ADHOC_SOCK_PTP) return SCE_NET_ADHOC_ERROR_INVALID_SOCKET_ID;
    if (sk->state == ADHOC_PTP_STATE_CLOSED) return SCE_NET_ADHOC_ERROR_DISCONNECTED;
    if (sk->state != ADHOC_PTP_STATE_ESTABLISHED) return SCE_NET_ADHOC_ERROR_NOT_CONNECTED;
    if (sk->flags & ADHOC_F_ALERTRECV) {
        sk->alerted_flags |= ADHOC_F_ALERTRECV;
        return SCE_NET_ADHOC_ERROR_SOCKET_ALERTED;
    }
    const int32_t cap_s = (int32_t)MEM_R32(size_ptr);
    if (cap_s <= 0) return SCE_NET_ADHOC_ERROR_INVALID_DATALEN;
    const uint32_t cap = (uint32_t)cap_s;
    if (!sr_guest_span_writable(buf_ptr, cap))
        return hle_unmodelled_stop("sceNetAdhocPtpRecv", "with unwritable buf 0x%08x", buf_ptr);
    NkAdhocTransport *t = s_adhoc_active_transport;
    if (!t || !t->ops || !t->ops->ptp_recv) return SCE_NET_ERROR_INTERNAL;
    uint8_t *tmp = malloc(cap);
    if (!tmp) return SCE_NET_ERROR_NO_SPACE;

    const uint32_t mod_gen = s_adhoc.gen;
    const uint32_t sock_gen = sk->gen;
    const uint64_t deadline = timeout ? sched_vtime_us() + timeout : UINT64_MAX;
    uint32_t got = 0u, rc = 0u;
    for (;;) {
        adhoc_pump_all();
        const int rr = t->ops->ptp_recv(t, sk->transport_handle, tmp, cap, &got);
        if (rr == NK_NET_OK) break;
        if (rr == NK_NET_DEAD) {
            sk->state = ADHOC_PTP_STATE_CLOSED;
            rc = SCE_NET_ADHOC_ERROR_DISCONNECTED;
            break;
        }
        if (flag & ADHOC_F_NONBLOCK) { rc = SCE_NET_ADHOC_ERROR_WOULD_BLOCK; break; }
        if (sched_vtime_us() >= deadline) { rc = SCE_NET_ADHOC_ERROR_TIMEOUT; break; }
        sched_delay_current(ADHOC_POLL_WAIT_STEP_US);
        if (s_adhoc.gen != mod_gen || !s_adhoc.inited) { rc = SCE_NET_ADHOC_ERROR_THREAD_ABORTED; break; }
        sk = adhoc_sock_get(id);
        if (!sk || sk->gen != sock_gen) { rc = SCE_NET_ADHOC_ERROR_SOCKET_DELETED; break; }
    }
    if (rc == 0u) {
        for (uint32_t i = 0u; i < got; i++) MEM_W8(buf_ptr + i, tmp[i]);
        MEM_W32(size_ptr, got);
    }
    free(tmp);
    return rc;
}

static uint32_t h_NetAdhocPtpFlush(CpuState *s) {
    if (!adhoc_online_mode()) return hle_adhoc_refuse_offline(s);
    if (!s_adhoc.inited) return SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
    const int id = (int)(int32_t)A0;
    const uint32_t flag = A2;
    if (flag & ~ADHOC_F_NONBLOCK) return SCE_NET_ADHOC_ERROR_INVALID_ARG;
    AdhocSockSlot *sk = adhoc_sock_get(id);
    if (!sk || sk->type != ADHOC_SOCK_PTP) return SCE_NET_ADHOC_ERROR_INVALID_SOCKET_ID;
    if (sk->state == ADHOC_PTP_STATE_CLOSED) return SCE_NET_ADHOC_ERROR_DISCONNECTED;
    if (sk->state != ADHOC_PTP_STATE_ESTABLISHED) return SCE_NET_ADHOC_ERROR_NOT_CONNECTED;
    if (sk->flags & ADHOC_F_ALERTFLUSH) {
        sk->alerted_flags |= ADHOC_F_ALERTFLUSH;
        return SCE_NET_ADHOC_ERROR_SOCKET_ALERTED;
    }
    adhoc_pump_all();
    return 0u;
}

static uint32_t h_NetAdhocPtpClose(CpuState *s) {
    if (!adhoc_online_mode()) return hle_adhoc_refuse_offline(s);
    if (!s_adhoc.inited) return SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
    const int id = (int)(int32_t)A0;
    AdhocSockSlot *sk = adhoc_sock_get(id);
    if (!sk || sk->type != ADHOC_SOCK_PTP) return SCE_NET_ADHOC_ERROR_INVALID_SOCKET_ID;
    adhoc_close_slot_internal(id);
    return 0u;
}

static uint32_t h_NetAdhocGetPtpStat(CpuState *s) {
    if (!adhoc_online_mode()) return hle_adhoc_refuse_offline(s);
    if (!s_adhoc.inited) return SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
    if (!A0 || !sr_guest_span_writable(A0, 4u)) return SCE_NET_ADHOC_ERROR_INVALID_ARG;
    adhoc_pump_all();
    int total = 0;
    for (int i = 0; i < ADHOC_MAX_SOCKETS; i++) {
        if (s_adhoc.slots[i].used && s_adhoc.slots[i].type == ADHOC_SOCK_PTP) total++;
    }
    if (!A1) {
        MEM_W32(A0, (uint32_t)total * 36u);
        return 0u;
    }
    const int32_t req = (int32_t)MEM_R32(A0);
    int max_n = req > 0 ? (int)((uint32_t)req / 36u) : 0;
    if (max_n > total) max_n = total;
    if (max_n > 0 && !sr_guest_span_writable(A1, (uint32_t)max_n * 36u))
        return hle_unmodelled_stop("sceNetAdhocGetPtpStat", "with unwritable buf 0x%08x", A1);
    NkAdhocTransport *t = s_adhoc_active_transport;
    int written = 0;
    for (int i = 0; i < ADHOC_MAX_SOCKETS && written < max_n; i++) {
        const AdhocSockSlot *sk = &s_adhoc.slots[i];
        if (!sk->used || sk->type != ADHOC_SOCK_PTP) continue;
        uint32_t rx_cc = (sk->state != ADHOC_PTP_STATE_LISTEN && t && t->ops && t->ops->ptp_peek)
                             ? t->ops->ptp_peek(t, sk->transport_handle)
                             : sk->bufsize;
        if (sk->state == ADHOC_PTP_STATE_LISTEN) rx_cc = sk->bufsize;
        const uint32_t addr = A1 + (uint32_t)written * 36u;
        MEM_W32(addr, (written + 1 < max_n) ? addr + 36u : 0u);
        MEM_W32(addr + 4u, (uint32_t)(i + 1));
        for (uint32_t k = 0u; k < 6u; k++) MEM_W8(addr + 8u + k, sk->laddr[k]);
        for (uint32_t k = 0u; k < 6u; k++) MEM_W8(addr + 14u + k, sk->paddr[k]);
        MEM_W16(addr + 20u, sk->lport);
        MEM_W16(addr + 22u, sk->pport);
        MEM_W32(addr + 24u, 0u);
        MEM_W32(addr + 28u, rx_cc);
        MEM_W32(addr + 32u, (uint32_t)sk->state);
        written++;
    }
    MEM_W32(A0, (uint32_t)written * 36u);
    return 0u;
}

static uint32_t h_NetAdhocSetSocketAlert(CpuState *s) {
    if (!adhoc_online_mode()) return hle_adhoc_refuse_offline(s);
    const int id = (int)(int32_t)A0;
    AdhocSockSlot *sk = adhoc_sock_get(id);
    if (!sk) return SCE_NET_ADHOC_ERROR_INVALID_SOCKET_ID;
    sk->flags = (int)(A1 & ADHOC_F_ALERTALL);
    sk->alerted_flags = 0;
    return 0u;
}

static uint32_t h_NetAdhocGetSocketAlert(CpuState *s) {
    if (!adhoc_online_mode()) return hle_adhoc_refuse_offline(s);
    if (!A1 || !sr_guest_span_writable(A1, 4u)) return SCE_NET_ADHOC_ERROR_INVALID_ARG;
    const int id = (int)(int32_t)A0;
    const AdhocSockSlot *sk = adhoc_sock_get(id);
    if (!sk) return SCE_NET_ADHOC_ERROR_INVALID_SOCKET_ID;
    MEM_W32(A1, (uint32_t)sk->flags);
    return 0u;
}

static int adhoc_poll_once(uint32_t sds_ptr, int count, uint32_t *alert_or_exc) {
    adhoc_pump_all();
    NkAdhocTransport *t = s_adhoc_active_transport;
    int affected = 0, disconnected = 0;
    for (int i = 0; i < count; i++) {
        const uint32_t base = sds_ptr + (uint32_t)i * 12u;
        const int sid = (int)(int32_t)MEM_R32(base);
        const uint32_t ev = MEM_R32(base + 4u);
        AdhocSockSlot *sk = adhoc_sock_get(sid);
        if (!sk) {
            MEM_W32(base + 8u, ADHOC_EV_INVALID);
            *alert_or_exc = SCE_NET_ADHOC_ERROR_INVALID_SOCKET_ID;
            return -1;
        }
        uint32_t rev = 0u;
        if (sk->type == ADHOC_SOCK_PDP) {
            const uint32_t peek = ((t && t->ops && t->ops->pdp_peek) ? t->ops->pdp_peek(t, sk->transport_handle) : 0u) +
                                  (sk->local_count ? sk->local_q[sk->local_head].len : 0u);
            if ((ev & ADHOC_EV_SEND)) rev |= ADHOC_EV_SEND;
            if ((ev & ADHOC_EV_RECV) && peek > 0u) rev |= ADHOC_EV_RECV;
        } else {
            if (sk->state == ADHOC_PTP_STATE_LISTEN) {
                const bool can_acc = (t && t->ops && t->ops->ptp_accept_pending)
                                         ? t->ops->ptp_accept_pending(t, sk->transport_handle)
                                         : false;
                if (can_acc && (ev & ADHOC_EV_ACCEPT)) rev |= ADHOC_EV_ACCEPT;
            } else if (sk->state == ADHOC_PTP_STATE_SYN_SENT) {
                const int cp = (t && t->ops && t->ops->ptp_connect_poll)
                                   ? t->ops->ptp_connect_poll(t, sk->transport_handle)
                                   : -1;
                if (cp > 0) {
                    sk->state = ADHOC_PTP_STATE_ESTABLISHED;
                    sk->ptp_connected_once = 1;
                    rev = (ADHOC_EV_SEND | ADHOC_EV_FLUSH | ADHOC_EV_CONNECT) & ev;
                } else if (cp < 0) {
                    sk->state = ADHOC_PTP_STATE_CLOSED;
                    rev = ADHOC_EV_DISCONNECT;
                    disconnected = 1;
                }
            } else if (sk->state == ADHOC_PTP_STATE_ESTABLISHED) {
                const uint32_t rx = (t && t->ops && t->ops->ptp_peek) ? t->ops->ptp_peek(t, sk->transport_handle) : 0u;
                const int dead = (t && t->ops && t->ops->ptp_is_dead) ? (t->ops->ptp_is_dead(t, sk->transport_handle) ? 1 : 0) : 0;
                if (dead && rx == 0u) {
                    rev = ADHOC_EV_DISCONNECT;
                    disconnected = 1;
                } else {
                    rev = (ADHOC_EV_SEND | ADHOC_EV_FLUSH | ADHOC_EV_CONNECT | (rx > 0u ? ADHOC_EV_RECV : 0u)) & ev;
                }
            } else if (sk->state == ADHOC_PTP_STATE_CLOSED && sk->ptp_connected_once) {
                rev = ADHOC_EV_DISCONNECT;
                disconnected = 1;
            }
        }
        if (sk->flags & ADHOC_F_ALERTPOLL) {
            sk->alerted_flags |= ADHOC_F_ALERTPOLL;
            MEM_W32(base + 8u, ADHOC_EV_ALERT);
            *alert_or_exc = SCE_NET_ADHOC_ERROR_SOCKET_ALERTED;
            return -1;
        }
        MEM_W32(base + 8u, rev);
        if (rev) affected++;
    }
    if (disconnected) {
        *alert_or_exc = SCE_NET_ADHOC_ERROR_EXCEPTION_EVENT;
        return -1;
    }
    return affected;
}

static uint32_t h_NetAdhocPollSocket(CpuState *s) {
    if (!adhoc_online_mode()) return hle_adhoc_refuse_offline(s);
    const uint32_t sds_ptr = A0, timeout = A2, nonblock = A3;
    const int32_t count = (int32_t)A1;
    if (nonblock & ~ADHOC_F_NONBLOCK) return SCE_NET_ADHOC_ERROR_INVALID_ARG;
    if (!s_adhoc.inited) return SCE_NET_ADHOC_ERROR_NOT_INITIALIZED;
    if (!sds_ptr || count <= 0 || (uint64_t)count * 12u > 0x10000000u ||
        !sr_guest_span_writable(sds_ptr, (uint32_t)count * 12u))
        return SCE_NET_ADHOC_ERROR_INVALID_ARG;
    const uint32_t mod_gen = s_adhoc.gen;
    const uint64_t deadline = timeout ? sched_vtime_us() + timeout : UINT64_MAX;
    for (;;) {
        uint32_t exc = 0u;
        const int res = adhoc_poll_once(sds_ptr, (int)count, &exc);
        if (res < 0) return exc;
        if (res > 0 || (nonblock & ADHOC_F_NONBLOCK)) return (uint32_t)res;
        if (sched_vtime_us() >= deadline) return SCE_NET_ADHOC_ERROR_TIMEOUT;
        sched_delay_current(ADHOC_POLL_WAIT_STEP_US);
        if (s_adhoc.gen != mod_gen || !s_adhoc.inited) return SCE_NET_ADHOC_ERROR_THREAD_ABORTED;
    }
}

/* ---- sceNetAdhocMatching (over PDP socket, PPSSPP/PSP wire compatible) --------------------
 * PPSSPP-derived, not PSP-measured (derived-translated from PPSSPP rev 3bc60270
 * Core/HLE/{sceNetAdhocMatching.cpp,sceNetAdhocMatching.h,proAdhoc.h}). */

#define MATCH_MAX_CONTEXTS 15
#define MATCH_MAX_PEERS 16
#define MATCH_MAX_EVENTS 64
#define MATCH_SCRATCH_BYTES 4096u
#define MATCH_PKT_PING   0
#define MATCH_PKT_HELLO  1
#define MATCH_PKT_JOIN   2
#define MATCH_PKT_ACCEPT 3
#define MATCH_PKT_CANCEL 4
#define MATCH_PKT_BULK   5
#define MATCH_PKT_BIRTH  7
#define MATCH_PKT_DEATH  8
#define MATCH_PKT_BYE    9

#define MATCH_EV_HELLO       1
#define MATCH_EV_REQUEST     2
#define MATCH_EV_LEAVE       3
#define MATCH_EV_DENY        4
#define MATCH_EV_CANCEL      5
#define MATCH_EV_ACCEPT      6
#define MATCH_EV_ESTABLISHED 7
#define MATCH_EV_TIMEOUT     8
#define MATCH_EV_BYE         10
#define MATCH_EV_DATA        11
#define MATCH_EV_DATA_ACK    12
#define MATCH_EV_DATA_TO     13

enum {
    MATCH_PEER_SELF = 0,
    MATCH_PEER_OFFER = 1,
    MATCH_PEER_PARENT = 2,
    MATCH_PEER_CHILD = 3,
    MATCH_PEER_P2P = 4,
    MATCH_PEER_INCOMING = 5,
    MATCH_PEER_OUTGOING = 6
};

typedef struct MatchPeer {
    int used;
    uint8_t mac[6];
    int state;
    int sending;
    int accepting;
    int ack_aborted;
    uint64_t lastping;
    uint64_t data_timeout_at;
    uint16_t port;
} MatchPeer;

typedef struct MatchEvent {
    int opcode;
    uint8_t mac[6];
    int optlen;
    uint8_t *opt;
} MatchEvent;

typedef struct MatchContext {
    int used;
    int id;
    uint32_t ctx_gen;
    int mode;
    int maxpeers;
    uint16_t port;
    int rxbuflen;
    uint8_t *rxbuf;
    int hello_int;
    int keepalive_int;
    int resend_int;
    int resend_cnt;
    uint64_t timeout;
    uint32_t callback;
    int running;
    uint32_t thread_uid;
    int pdp_id;
    uint8_t self_mac[6];
    int hellolen;
    uint8_t *hello;
    uint64_t last_hello;
    uint64_t last_ping;
    uint64_t parent_first_at;
    uint32_t cb_block_uid;
    uint32_t cb_guest_addr;
    MatchPeer peers[MATCH_MAX_PEERS];
    MatchEvent events[MATCH_MAX_EVENTS];
    int ev_head;
    int ev_count;
} MatchContext;

static struct {
    int inited;
    uint32_t pool_size;
    uint32_t gen;
    MatchContext ctx[MATCH_MAX_CONTEXTS];
} s_matching;

static MatchContext *match_find_ctx(int id) {
    for (int i = 0; i < MATCH_MAX_CONTEXTS; i++) {
        if (s_matching.ctx[i].used && s_matching.ctx[i].id == id) return &s_matching.ctx[i];
    }
    return NULL;
}

static MatchPeer *match_find_peer(MatchContext *ctx, const uint8_t mac[6]) {
    for (int i = 0; i < MATCH_MAX_PEERS; i++) {
        if (ctx->peers[i].used && is_mac_match(ctx->peers[i].mac, mac)) return &ctx->peers[i];
    }
    return NULL;
}

static MatchPeer *match_find_by_state(MatchContext *ctx, int st) {
    for (int i = 0; i < MATCH_MAX_PEERS; i++) {
        if (ctx->peers[i].used && ctx->peers[i].state == st) return &ctx->peers[i];
    }
    return NULL;
}

static int match_count_state(const MatchContext *ctx, int st) {
    int n = 0;
    for (int i = 0; i < MATCH_MAX_PEERS; i++) {
        if (ctx->peers[i].used && ctx->peers[i].state == st) n++;
    }
    return n;
}

static MatchPeer *match_add_peer(MatchContext *ctx, const uint8_t mac[6], int st) {
    MatchPeer *existing = match_find_peer(ctx, mac);
    if (existing) return existing;
    for (int i = 0; i < MATCH_MAX_PEERS; i++) {
        if (!ctx->peers[i].used) {
            memset(&ctx->peers[i], 0, sizeof(ctx->peers[i]));
            ctx->peers[i].used = 1;
            memcpy(ctx->peers[i].mac, mac, 6u);
            ctx->peers[i].state = st;
            ctx->peers[i].lastping = sched_vtime_us();
            ctx->peers[i].port = ctx->port;
            return &ctx->peers[i];
        }
    }
    return NULL;
}

static void match_spawn_event(MatchContext *ctx, int op, const uint8_t mac[6], int optlen,
                              const void *opt) {
    if (ctx->ev_count >= MATCH_MAX_EVENTS) return;
    uint8_t *copy = NULL;
    if (optlen > 0 && opt) {
        copy = malloc((size_t)optlen);
        if (!copy) return;
        memcpy(copy, opt, (size_t)optlen);
    }
    MatchEvent *ev = &ctx->events[(ctx->ev_head + ctx->ev_count) % MATCH_MAX_EVENTS];
    ev->opcode = op;
    memcpy(ev->mac, mac, 6u);
    ev->optlen = opt ? optlen : 0;
    ev->opt = copy;
    ctx->ev_count++;
}

static void match_clear_events(MatchContext *ctx) {
    for (int i = 0; i < ctx->ev_count; i++) {
        MatchEvent *ev = &ctx->events[(ctx->ev_head + i) % MATCH_MAX_EVENTS];
        free(ev->opt);
        ev->opt = NULL;
    }
    ctx->ev_head = 0;
    ctx->ev_count = 0;
}

static void match_send_pkt(MatchContext *ctx, const uint8_t mac[6], const uint8_t *pkt, uint32_t len) {
    if (ctx->pdp_id <= 0) return;
    const MatchPeer *p = match_find_peer(ctx, mac);
    const uint16_t dport = (p && p->port) ? p->port : ctx->port;
    (void)adhoc_pdp_send_raw(ctx->pdp_id, mac, dport, pkt, len);
}

static void match_send_hello(MatchContext *ctx) {
    const uint32_t total = 5u + (uint32_t)(ctx->hellolen > 0 ? ctx->hellolen : 0);
    uint8_t *pkt = malloc(total);
    if (!pkt) return;
    pkt[0] = MATCH_PKT_HELLO;
    const uint32_t hlen = (uint32_t)(ctx->hellolen > 0 ? ctx->hellolen : 0);
    pkt[1] = (uint8_t)hlen;
    pkt[2] = (uint8_t)(hlen >> 8);
    pkt[3] = (uint8_t)(hlen >> 16);
    pkt[4] = (uint8_t)(hlen >> 24);
    if (hlen > 0u && ctx->hello) memcpy(pkt + 5, ctx->hello, hlen);
    for (int i = 0; i < ADHOC_MAX_PEERS; i++) {
        if (!s_adhoc_peers[i].active) continue;
        (void)adhoc_pdp_send_raw(ctx->pdp_id, s_adhoc_peers[i].mac, ctx->port, pkt, total);
    }
    free(pkt);
}

static void match_send_opt_pkt(MatchContext *ctx, uint8_t opcode, const uint8_t mac[6],
                               int optlen, const void *opt) {
    const uint32_t olen = (uint32_t)(optlen > 0 ? optlen : 0);
    uint8_t *pkt = malloc(5u + olen);
    if (!pkt) return;
    pkt[0] = opcode;
    pkt[1] = (uint8_t)olen;
    pkt[2] = (uint8_t)(olen >> 8);
    pkt[3] = (uint8_t)(olen >> 16);
    pkt[4] = (uint8_t)(olen >> 24);
    if (olen > 0u && opt) memcpy(pkt + 5, opt, olen);
    match_send_pkt(ctx, mac, pkt, 5u + olen);
    free(pkt);
}

static void match_send_accept_pkt(MatchContext *ctx, const uint8_t mac[6], int optlen, const void *opt) {
    int siblings = 0;
    if (ctx->mode == 1) {
        for (int i = 0; i < MATCH_MAX_PEERS; i++) {
            if (ctx->peers[i].used && ctx->peers[i].state == MATCH_PEER_CHILD &&
                !is_mac_match(ctx->peers[i].mac, mac))
                siblings++;
        }
    }
    const uint32_t olen = (uint32_t)(optlen > 0 ? optlen : 0);
    const uint32_t total = 9u + olen + (uint32_t)siblings * 6u;
    uint8_t *pkt = malloc(total);
    if (!pkt) return;
    pkt[0] = MATCH_PKT_ACCEPT;
    pkt[1] = (uint8_t)olen;
    pkt[2] = (uint8_t)(olen >> 8);
    pkt[3] = (uint8_t)(olen >> 16);
    pkt[4] = (uint8_t)(olen >> 24);
    pkt[5] = (uint8_t)siblings;
    pkt[6] = (uint8_t)(siblings >> 8);
    pkt[7] = (uint8_t)(siblings >> 16);
    pkt[8] = (uint8_t)(siblings >> 24);
    if (olen > 0u && opt) memcpy(pkt + 9, opt, olen);
    uint32_t off = 9u + olen;
    if (siblings > 0) {
        for (int i = 0; i < MATCH_MAX_PEERS; i++) {
            if (ctx->peers[i].used && ctx->peers[i].state == MATCH_PEER_CHILD &&
                !is_mac_match(ctx->peers[i].mac, mac)) {
                memcpy(pkt + off, ctx->peers[i].mac, 6u);
                off += 6u;
            }
        }
    }
    match_send_pkt(ctx, mac, pkt, total);
    free(pkt);
    match_spawn_event(ctx, MATCH_EV_ESTABLISHED, mac, 0, NULL);
}

static void match_send_birth(MatchContext *ctx, const uint8_t newborn_mac[6]) {
    uint8_t pkt[7];
    pkt[0] = MATCH_PKT_BIRTH;
    memcpy(pkt + 1, newborn_mac, 6u);
    for (int i = 0; i < MATCH_MAX_PEERS; i++) {
        if (ctx->peers[i].used && ctx->peers[i].state == MATCH_PEER_CHILD &&
            !is_mac_match(ctx->peers[i].mac, newborn_mac))
            match_send_pkt(ctx, ctx->peers[i].mac, pkt, 7u);
    }
}

static uint32_t le32_at(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void match_handle_packet(MatchContext *ctx, const uint8_t sender[6], uint16_t sport, int len) {
    if (len <= 0) return;
    const uint64_t now = sched_vtime_us();
    AdhocPeerEntry *ap = adhoc_find_peer(sender);
    if (ap) ap->last_recv = now;
    MatchPeer *existing = match_find_peer(ctx, sender);
    if (existing) existing->port = sport ? sport : ctx->port;

    const uint8_t op = ctx->rxbuf[0];
    switch (op) {
    case MATCH_PKT_PING:
        if (existing) existing->lastping = now;
        break;
    case MATCH_PKT_HELLO: {
        if ((ctx->mode == 2 && !match_find_by_state(ctx, MATCH_PEER_PARENT)) ||
            (ctx->mode == 3 && !match_find_by_state(ctx, MATCH_PEER_P2P))) {
            if (len < 5) break;
            const int32_t olen = (int32_t)le32_at(ctx->rxbuf + 1);
            if (olen < 0 || olen > len - 5) break;
            MatchPeer *p = match_add_peer(ctx, sender, MATCH_PEER_OFFER);
            if (!p) break;
            p->port = sport ? sport : ctx->port;
            if (p->state != MATCH_PEER_OUTGOING && p->state != MATCH_PEER_INCOMING) {
                match_spawn_event(ctx, MATCH_EV_HELLO, sender, olen, olen ? ctx->rxbuf + 5 : NULL);
            }
        }
        break;
    }
    case MATCH_PKT_JOIN: {
        if (ctx->mode == 2) break;
        const int room = (ctx->mode == 1 && match_count_state(ctx, MATCH_PEER_CHILD) < ctx->maxpeers - 1) ||
                         (ctx->mode == 3 && !match_find_by_state(ctx, MATCH_PEER_P2P));
        if (!room || len < 5) {
            match_send_opt_pkt(ctx, MATCH_PKT_CANCEL, sender, 0, NULL);
            break;
        }
        const int32_t olen = (int32_t)le32_at(ctx->rxbuf + 1);
        if (olen < 0 || olen > len - 5) break;
        if (existing && ctx->mode == 1 &&
            (existing->state == MATCH_PEER_INCOMING || existing->state == MATCH_PEER_CHILD))
            break;
        MatchPeer *p = match_add_peer(ctx, sender, MATCH_PEER_INCOMING);
        if (p) {
            p->state = MATCH_PEER_INCOMING;
            p->lastping = now;
            p->port = sport ? sport : ctx->port;
            match_spawn_event(ctx, MATCH_EV_REQUEST, sender, olen, olen ? ctx->rxbuf + 5 : NULL);
        }
        break;
    }
    case MATCH_PKT_ACCEPT: {
        if (ctx->mode == 1 || len < 9) break;
        if ((ctx->mode == 2 && match_find_by_state(ctx, MATCH_PEER_PARENT)) ||
            (ctx->mode == 3 && match_find_by_state(ctx, MATCH_PEER_P2P)))
            break;
        const int32_t olen = (int32_t)le32_at(ctx->rxbuf + 1);
        const int32_t sib = (int32_t)le32_at(ctx->rxbuf + 5);
        if (olen < 0 || sib < 0 || (uint64_t)len < 9u + (uint64_t)olen + (uint64_t)sib * 6u) break;
        MatchPeer *req = match_find_by_state(ctx, MATCH_PEER_OUTGOING);
        if (!req || req != existing) break;
        req->state = (ctx->mode == 2) ? MATCH_PEER_PARENT : MATCH_PEER_P2P;
        req->lastping = now;
        for (int i = 0; i < MATCH_MAX_PEERS; i++) {
            if (ctx->peers[i].used && &ctx->peers[i] != req) ctx->peers[i].used = 0;
        }
        if (ctx->mode == 2) {
            for (int i = 0; i < sib; i++) {
                const uint8_t *sm = ctx->rxbuf + 9 + olen + i * 6;
                if (!is_mac_match(sm, ctx->self_mac)) (void)match_add_peer(ctx, sm, MATCH_PEER_CHILD);
            }
            (void)match_add_peer(ctx, ctx->self_mac, MATCH_PEER_SELF);
            ctx->parent_first_at = now + (uint64_t)(ctx->keepalive_int > 0 ? ctx->keepalive_int : 2000000);
        }
        match_spawn_event(ctx, MATCH_EV_ACCEPT, sender, olen, olen ? ctx->rxbuf + 9 : NULL);
        match_spawn_event(ctx, MATCH_EV_ESTABLISHED, sender, 0, NULL);
        break;
    }
    case MATCH_PKT_CANCEL: {
        if (!existing || len < 5) break;
        const int32_t olen = (int32_t)le32_at(ctx->rxbuf + 1);
        if (olen < 0 || olen > len - 5) break;
        const void *opt = olen ? ctx->rxbuf + 5 : NULL;
        MatchPeer *req = match_find_by_state(ctx, MATCH_PEER_OUTGOING);
        if (req == existing) {
            match_spawn_event(ctx, MATCH_EV_DENY, sender, olen, opt);
            existing->state = MATCH_PEER_OFFER;
        } else if (existing->state == MATCH_PEER_INCOMING) {
            match_spawn_event(ctx, MATCH_EV_CANCEL, sender, olen, opt);
            existing->state = MATCH_PEER_OFFER;
        } else if (existing->state == MATCH_PEER_PARENT || existing->state == MATCH_PEER_CHILD ||
                   existing->state == MATCH_PEER_P2P) {
            match_spawn_event(ctx, MATCH_EV_LEAVE, sender, olen, opt);
            existing->used = 0;
        }
        break;
    }
    case MATCH_PKT_BULK: {
        if (!existing || len <= 5) break;
        if (existing->state != MATCH_PEER_PARENT && existing->state != MATCH_PEER_CHILD &&
            existing->state != MATCH_PEER_P2P)
            break;
        const int32_t dlen = (int32_t)le32_at(ctx->rxbuf + 1);
        if (dlen > 0 && dlen <= len - 5)
            match_spawn_event(ctx, MATCH_EV_DATA, sender, dlen, ctx->rxbuf + 5);
        break;
    }
    case MATCH_PKT_BIRTH:
        if (ctx->mode == 2 && len >= 7 && existing && existing->state == MATCH_PEER_PARENT &&
            !is_mac_match(ctx->rxbuf + 1, ctx->self_mac))
            (void)match_add_peer(ctx, ctx->rxbuf + 1, MATCH_PEER_CHILD);
        break;
    case MATCH_PKT_DEATH:
        if (ctx->mode == 2 && len >= 7 && existing && existing->state == MATCH_PEER_PARENT) {
            MatchPeer *dk = match_find_peer(ctx, ctx->rxbuf + 1);
            if (dk && dk->state == MATCH_PEER_CHILD) {
                match_spawn_event(ctx, MATCH_EV_LEAVE, dk->mac, 0, NULL);
                dk->used = 0;
            }
        }
        break;
    case MATCH_PKT_BYE:
        if (existing) {
            if (ctx->mode != 2 || existing->state == MATCH_PEER_PARENT)
                match_spawn_event(ctx, MATCH_EV_BYE, sender, 0, NULL);
            existing->used = 0;
        }
        break;
    default:
        break;
    }
}

static void match_deliver_event(CpuState *s, MatchContext *ctx, uint32_t sp, uint32_t gp,
                                const MatchEvent *ev) {
    if (!ctx->callback || !ctx->cb_guest_addr) return;
    for (uint32_t i = 0u; i < 6u; i++) MEM_W8(ctx->cb_guest_addr + i, ev->mac[i]);
    uint32_t optlen = (uint32_t)(ev->optlen > 0 ? ev->optlen : 0);
    if (optlen > MATCH_SCRATCH_BYTES - 8u) optlen = MATCH_SCRATCH_BYTES - 8u;
    for (uint32_t i = 0u; i < optlen; i++) MEM_W8(ctx->cb_guest_addr + 8u + i, ev->opt[i]);
    s->r[4] = (uint32_t)ctx->id;
    s->r[5] = (uint32_t)ev->opcode;
    s->r[6] = ctx->cb_guest_addr;
    s->r[7] = optlen;
    s->r[8] = optlen ? (ctx->cb_guest_addr + 8u) : 0u;
    s->r[28] = gp;
    s->r[29] = sp;
    s->r[31] = SR_HOST_RETURN_PC;
    s->pc = ctx->callback;
    uint32_t fatal_target = 0u;
    if (hle_dispatch_guest_scoped(s, ctx->callback, "sceNetAdhocMatching handler", &fatal_target)) {
        fprintf(stderr, "SEMANTIC_BOUNDARY: matching-handler-fault; callback 0x%08x (id %d, event %d) "
                        "faulted at 0x%08x\n",
                ctx->callback, ctx->id, ev->opcode, fatal_target);
        sr_flight_fatal(SR_FLIGHT_KIND_FATAL_DISPATCH, ctx->callback, fatal_target, (uint32_t)ev->opcode);
        fflush(stderr);
        exit(1);
    }
}

static void matching_thread_body(CpuState *s, void *arg) {
    const uint64_t packed = (uint64_t)(uintptr_t)arg;
    const int idx = (int)(packed >> 32);
    const uint32_t cgen = (uint32_t)(packed & 0xffffffffu);
    const uint32_t mgen = s_matching.gen;
    const uint32_t sp = s->r[29], gp = s->r[28];
    if (idx < 0 || idx >= MATCH_MAX_CONTEXTS) return;
    MatchContext *ctx = &s_matching.ctx[idx];
    while (s_matching.gen == mgen && ctx->used && ctx->ctx_gen == cgen && ctx->running) {
        const uint64_t now = sched_vtime_us();
        adhoc_pump_all();
        if (((ctx->mode == 1 && match_count_state(ctx, MATCH_PEER_CHILD) < ctx->maxpeers - 1) ||
             (ctx->mode == 3 && !match_find_by_state(ctx, MATCH_PEER_P2P))) &&
            ctx->hello_int > 0 && now >= ctx->last_hello + (uint64_t)ctx->hello_int) {
            match_send_hello(ctx);
            ctx->last_hello = now;
        }
        if (ctx->keepalive_int > 0 && now >= ctx->last_ping + (uint64_t)ctx->keepalive_int) {
            const uint8_t ping = MATCH_PKT_PING;
            for (int i = 0; i < MATCH_MAX_PEERS; i++) {
                const MatchPeer *p = &ctx->peers[i];
                if (!p->used || p->state == MATCH_PEER_SELF || p->state == MATCH_PEER_OFFER) continue;
                match_send_pkt(ctx, p->mac, &ping, 1u);
            }
            ctx->last_ping = now;
        }
        for (int i = 0; i < MATCH_MAX_PEERS; i++) {
            MatchPeer *p = &ctx->peers[i];
            if (p->used && p->data_timeout_at && now >= p->data_timeout_at) {
                p->data_timeout_at = 0u;
                match_spawn_event(ctx, MATCH_EV_DATA_TO, p->mac, 0, NULL);
            }
            if (p->used && p->state != MATCH_PEER_SELF && p->state != MATCH_PEER_OFFER &&
                ctx->timeout > 0u && now >= p->lastping + ctx->timeout) {
                match_spawn_event(ctx, MATCH_EV_TIMEOUT, p->mac, 0, NULL);
                p->used = 0;
            }
        }
        for (;;) {
            uint8_t smac[6];
            uint16_t sport = 0u;
            uint32_t rlen = 0u;
            if (adhoc_pdp_recv_raw(ctx->pdp_id, smac, &sport, ctx->rxbuf, (uint32_t)ctx->rxbuflen,
                                   &rlen) != 0u)
                break;
            match_handle_packet(ctx, smac, sport, (int)rlen);
        }
        if (ctx->ev_count > 0) {
            MatchEvent ev = ctx->events[ctx->ev_head];
            ctx->events[ctx->ev_head].opt = NULL;
            ctx->ev_head = (ctx->ev_head + 1) % MATCH_MAX_EVENTS;
            ctx->ev_count--;
            MatchPeer *ep = match_find_peer(ctx, ev.mac);
            if (ep) {
                if (ev.opcode == MATCH_EV_ESTABLISHED) ep->accepting = 0;
                if (ev.opcode == MATCH_EV_DATA_ACK || ev.opcode == MATCH_EV_DATA_TO) ep->sending = 0;
            }
            match_deliver_event(s, ctx, sp, gp, &ev);
            free(ev.opt);
            continue;
        }
        sched_service_wait(sched_vtime_us() + ADHOC_PUMP_INTERVAL_US);
    }
}

static void match_stop_ctx_internal(MatchContext *ctx) {
    if (!ctx->used) return;
    if (ctx->running && ctx->pdp_id > 0) {
        const uint8_t bye = MATCH_PKT_BYE;
        for (int i = 0; i < MATCH_MAX_PEERS; i++) {
            if (ctx->peers[i].used && ctx->peers[i].state != MATCH_PEER_SELF)
                match_send_pkt(ctx, ctx->peers[i].mac, &bye, 1u);
        }
        adhoc_close_slot_internal(ctx->pdp_id);
        ctx->pdp_id = 0;
    }
    ctx->running = 0;
    ctx->ctx_gen++;
    if (ctx->thread_uid) {
        sched_service_kick(ctx->thread_uid);
        ctx->thread_uid = 0u;
    }
    match_clear_events(ctx);
}

static void adhoc_close_sockets_and_matching(void) {
    s_matching.gen++;
    for (int i = 0; i < MATCH_MAX_CONTEXTS; i++) {
        if (!s_matching.ctx[i].used) continue;
        match_stop_ctx_internal(&s_matching.ctx[i]);
        free(s_matching.ctx[i].rxbuf);
        free(s_matching.ctx[i].hello);
        memset(&s_matching.ctx[i], 0, sizeof(s_matching.ctx[i]));
    }
    s_matching.inited = 0;
    s_matching.pool_size = 0u;
    s_adhoc.gen++;
    for (int i = 0; i < ADHOC_MAX_SOCKETS; i++) {
        if (s_adhoc.slots[i].used) adhoc_close_slot_internal(i + 1);
    }
    s_adhoc.inited = 0;
}

static uint32_t h_NetAdhocMatchingInit(CpuState *s) {
    if (!adhoc_online_mode()) return hle_adhoc_refuse_offline(s);
    adhoc_log_ppsspp_tier_once();
    if (s_matching.inited) return SCE_NET_ADHOC_MATCHING_ERROR_ALREADY_INIT;
    s_matching.pool_size = A0;
    s_matching.inited = 1;
    s_matching.gen++;
    return 0u;
}

static uint32_t h_NetAdhocMatchingTerm(CpuState *s) {
    if (!adhoc_online_mode()) return hle_adhoc_refuse_offline(s);
    if (!s_matching.inited) return 0u;
    s_matching.gen++;
    for (int i = 0; i < MATCH_MAX_CONTEXTS; i++) {
        if (!s_matching.ctx[i].used) continue;
        match_stop_ctx_internal(&s_matching.ctx[i]);
        if (s_matching.ctx[i].cb_block_uid) (void)free_block(s_matching.ctx[i].cb_block_uid);
        free(s_matching.ctx[i].rxbuf);
        free(s_matching.ctx[i].hello);
        memset(&s_matching.ctx[i], 0, sizeof(s_matching.ctx[i]));
    }
    s_matching.inited = 0;
    return 0u;
}

static uint32_t h_NetAdhocMatchingCreate(CpuState *s) {
    if (!adhoc_online_mode()) return hle_adhoc_refuse_offline(s);
    if (!s_matching.inited) return SCE_NET_ADHOC_MATCHING_ERROR_NOT_INIT;
    const int32_t mode = (int32_t)A0, maxnum = (int32_t)A1, port = (int32_t)(uint16_t)A2;
    const int32_t rxbuflen = (int32_t)A3;
    const int32_t hello_int = (int32_t)stack_arg(s, 0), keep_int = (int32_t)stack_arg(s, 1);
    const int32_t init_cnt = (int32_t)stack_arg(s, 2), rexmt_int = (int32_t)stack_arg(s, 3);
    if (s->r[29] && !sr_guest_span_readable(s->r[29], 4u))
        return hle_unmodelled_stop("sceNetAdhocMatchingCreate", "with unreadable stack arg");
    const uint32_t cb = stack_arg(s, 4);
    if (maxnum <= 1 || maxnum > 16) return SCE_NET_ADHOC_MATCHING_ERROR_INVALID_MAXNUM;
    if (rxbuflen < 6 * maxnum + 4) return SCE_NET_ADHOC_MATCHING_ERROR_RXBUF_TOO_SHORT;
    if (mode < 1 || mode > 3) return SCE_NET_ADHOC_MATCHING_ERROR_INVALID_MODE;
    if (((mode == 1 || mode == 3) && (hello_int == 0 || keep_int == 0)) || init_cnt < 0 || rexmt_int == 0)
        return SCE_NET_ADHOC_MATCHING_ERROR_INVALID_ARG;
    for (int i = 0; i < MATCH_MAX_CONTEXTS; i++) {
        if (s_matching.ctx[i].used && s_matching.ctx[i].port == (uint16_t)port)
            return SCE_NET_ADHOC_MATCHING_ERROR_PORT_IN_USE;
    }
    int slot = -1;
    for (int i = 0; i < MATCH_MAX_CONTEXTS; i++) {
        if (!s_matching.ctx[i].used) { slot = i; break; }
    }
    if (slot < 0) return SCE_NET_ADHOC_MATCHING_ERROR_ID_NOT_AVAIL;
    uint8_t self_mac[6] = {0};
    (void)adhoc_local_mac(self_mac);
    const uint32_t cb_uid = alloc_block(MATCH_SCRATCH_BYTES);
    if (cb_uid == NET_POOL_NONE) return SCE_NET_ADHOC_MATCHING_ERROR_NO_SPACE;
    uint8_t *rxbuf = calloc(1, (size_t)rxbuflen);
    if (!rxbuf) {
        (void)free_block(cb_uid);
        return SCE_NET_ADHOC_MATCHING_ERROR_NO_SPACE;
    }
    MatchContext *ctx = &s_matching.ctx[slot];
    memset(ctx, 0, sizeof(*ctx));
    ctx->used = 1;
    ctx->id = slot + 1;
    ctx->mode = mode;
    ctx->maxpeers = maxnum;
    ctx->port = (uint16_t)port;
    ctx->rxbuflen = rxbuflen;
    ctx->rxbuf = rxbuf;
    ctx->hello_int = hello_int;
    ctx->keepalive_int = keep_int > 0 ? keep_int : 2000000;
    ctx->resend_int = rexmt_int;
    ctx->resend_cnt = init_cnt;
    ctx->timeout = (uint64_t)ctx->keepalive_int * (uint64_t)(init_cnt > 0 ? init_cnt : 1) + 500000u;
    ctx->callback = cb;
    ctx->cb_block_uid = cb_uid;
    ctx->cb_guest_addr = block_addr(cb_uid);
    memcpy(ctx->self_mac, self_mac, 6u);
    return (uint32_t)ctx->id;
}

static uint32_t matching_start_impl(int id, int evth_pri, int evth_stack, int inth_pri,
                                    int inth_stack, int32_t optlen, uint32_t opt_ptr) {
    if (!s_matching.inited) return SCE_NET_ADHOC_MATCHING_ERROR_NOT_INIT;
    if (evth_stack < 0x100 || inth_stack < 0x100)
        return SCE_NET_ADHOC_MATCHING_ERROR_STACKSIZE_TOO_SHORT;
    (void)evth_pri;
    MatchContext *ctx = match_find_ctx(id);
    if (!ctx) return SCE_NET_ADHOC_MATCHING_ERROR_INVALID_ID;
    if (ctx->running) return SCE_NET_ADHOC_MATCHING_ERROR_IS_RUNNING;
    if (optlen < 0 || (optlen > 0 && (!opt_ptr || !sr_guest_span_readable(opt_ptr, (uint32_t)optlen))))
        return SCE_NET_ADHOC_MATCHING_ERROR_INVALID_OPTLEN;
    free(ctx->hello);
    ctx->hello = NULL;
    ctx->hellolen = 0;
    if (optlen > 0) {
        ctx->hello = malloc((size_t)optlen);
        if (!ctx->hello) return SCE_NET_ADHOC_MATCHING_ERROR_NO_SPACE;
        for (int32_t i = 0; i < optlen; i++) ctx->hello[i] = MEM_R8(opt_ptr + (uint32_t)i);
        ctx->hellolen = optlen;
    }
    if (!s_adhoc.inited || !adhoc_local_mac(ctx->self_mac))
        return SCE_NET_ADHOC_MATCHING_ERROR_NO_SPACE;
    ctx->pdp_id = adhoc_pdp_open_internal(ctx->port, PSP_ADHOC_PDP_MFS);
    if (ctx->pdp_id < 0) return SCE_NET_ADHOC_MATCHING_ERROR_PORT_IN_USE;
    memset(ctx->peers, 0, sizeof(ctx->peers));
    (void)match_add_peer(ctx, ctx->self_mac, MATCH_PEER_SELF);
    ctx->running = 1;
    ctx->ctx_gen++;
    ctx->last_hello = 0u;
    ctx->last_ping = sched_vtime_us();
    const uint64_t packed = ((uint64_t)(ctx - s_matching.ctx) << 32) | (uint64_t)ctx->ctx_gen;
    ctx->thread_uid = sched_create_service_thread("SceNetAdhocMatch", inth_pri, (uint32_t)inth_stack,
                                                  matching_thread_body, (void *)(uintptr_t)packed);
    if (ctx->thread_uid >= 0x80000000u) {
        adhoc_close_slot_internal(ctx->pdp_id);
        ctx->pdp_id = 0;
        ctx->running = 0;
        ctx->thread_uid = 0u;
        return SCE_NET_ADHOC_MATCHING_ERROR_NO_SPACE;
    }
    return 0u;
}

static uint32_t h_NetAdhocMatchingStart(CpuState *s) {
    if (!adhoc_online_mode()) return hle_adhoc_refuse_offline(s);
    return matching_start_impl((int)(int32_t)A0, (int)(int32_t)A1, (int)(int32_t)A2,
                               (int)(int32_t)A3, (int)(int32_t)stack_arg(s, 0),
                               (int32_t)stack_arg(s, 1), stack_arg(s, 2));
}

static uint32_t h_NetAdhocMatchingStart2(CpuState *s) {
    if (!adhoc_online_mode()) return hle_adhoc_refuse_offline(s);
    if (s->r[29] && !sr_guest_span_readable(s->r[29], 4u))
        return hle_unmodelled_stop("sceNetAdhocMatchingStart2", "with unreadable stack arg");
    const int32_t ev_stack = (int32_t)A2, ev_part = (int32_t)A3;
    const int32_t in_pri = (int32_t)stack_arg(s, 0), in_stack = (int32_t)stack_arg(s, 1);
    const int32_t in_part = (int32_t)stack_arg(s, 2);
    if (ev_stack < 0x100 || in_stack < 0x100)
        return SCE_NET_ADHOC_MATCHING_ERROR_STACKSIZE_TOO_SHORT;
    if (ev_part < 1 || ev_part > 6 || in_part < 1 || in_part > 6)
        return SCE_NET_ADHOC_MATCHING_ERROR_INVALID_ID;
    return matching_start_impl((int)(int32_t)A0, (int)(int32_t)A1, ev_stack, in_pri, in_stack,
                               (int32_t)stack_arg(s, 3), stack_arg(s, 4));
}

static uint32_t h_NetAdhocMatchingStop(CpuState *s) {
    if (!adhoc_online_mode()) return hle_adhoc_refuse_offline(s);
    if (!s_matching.inited) return SCE_NET_ADHOC_MATCHING_ERROR_NOT_INIT;
    MatchContext *ctx = match_find_ctx((int)(int32_t)A0);
    if (!ctx) return SCE_NET_ADHOC_MATCHING_ERROR_INVALID_ID;
    if (!ctx->running) return SCE_NET_ADHOC_MATCHING_ERROR_NOT_RUNNING;
    match_stop_ctx_internal(ctx);
    return 0u;
}

static uint32_t h_NetAdhocMatchingDelete(CpuState *s) {
    if (!adhoc_online_mode()) return hle_adhoc_refuse_offline(s);
    if (!s_matching.inited) return SCE_NET_ADHOC_MATCHING_ERROR_NOT_INIT;
    MatchContext *ctx = match_find_ctx((int)(int32_t)A0);
    if (!ctx) return SCE_NET_ADHOC_MATCHING_ERROR_INVALID_ID;
    if (ctx->running) return SCE_NET_ADHOC_MATCHING_ERROR_IS_RUNNING;
    if (ctx->cb_block_uid) (void)free_block(ctx->cb_block_uid);
    free(ctx->rxbuf);
    free(ctx->hello);
    memset(ctx, 0, sizeof(*ctx));
    return 0u;
}

static uint32_t h_NetAdhocMatchingSelectTarget(CpuState *s) {
    if (!adhoc_online_mode()) return hle_adhoc_refuse_offline(s);
    if (!s_matching.inited) return SCE_NET_ADHOC_MATCHING_ERROR_NOT_INIT;
    if (!A1 || !sr_guest_span_readable(A1, 6u)) return SCE_NET_ADHOC_MATCHING_ERROR_INVALID_ARG;
    MatchContext *ctx = match_find_ctx((int)(int32_t)A0);
    if (!ctx) return SCE_NET_ADHOC_MATCHING_ERROR_INVALID_ID;
    if (!ctx->running) return SCE_NET_ADHOC_MATCHING_ERROR_NOT_RUNNING;
    uint8_t tmac[6];
    for (uint32_t i = 0u; i < 6u; i++) tmac[i] = MEM_R8(A1 + i);
    MatchPeer *peer = match_find_peer(ctx, tmac);
    if (!peer) return SCE_NET_ADHOC_MATCHING_ERROR_UNKNOWN_TARGET;
    const int32_t olen = (int32_t)A2;
    if (olen < 0 || (olen > 0 && (!A3 || !sr_guest_span_readable(A3, (uint32_t)olen))))
        return SCE_NET_ADHOC_MATCHING_ERROR_INVALID_OPTLEN;
    uint8_t *opt = olen > 0 ? malloc((size_t)olen) : NULL;
    if (olen > 0 && !opt) return SCE_NET_ADHOC_MATCHING_ERROR_NO_SPACE;
    for (int32_t i = 0; i < olen; i++) opt[i] = MEM_R8(A3 + (uint32_t)i);
    uint32_t rc = 0u;
    if (ctx->mode == 1) {
        if (peer->state == MATCH_PEER_CHILD && peer->accepting) rc = SCE_NET_ADHOC_MATCHING_ERROR_REQUEST_IN_PROG;
        else if (peer->state == MATCH_PEER_CHILD) rc = SCE_NET_ADHOC_MATCHING_ERROR_ALREADY_EST;
        else if (match_count_state(ctx, MATCH_PEER_CHILD) >= ctx->maxpeers - 1) rc = SCE_NET_ADHOC_MATCHING_ERROR_EXCEED_MAXNUM;
        else if (peer->state == MATCH_PEER_INCOMING) {
            peer->state = MATCH_PEER_CHILD;
            peer->accepting = 1;
            match_send_birth(ctx, peer->mac);
            match_send_accept_pkt(ctx, peer->mac, olen, opt);
        } else {
            rc = SCE_NET_ADHOC_MATCHING_ERROR_TARGET_NOT_READY;
        }
    } else if (ctx->mode == 2) {
        if (match_find_by_state(ctx, MATCH_PEER_PARENT)) rc = SCE_NET_ADHOC_MATCHING_ERROR_ALREADY_EST;
        else if (match_find_by_state(ctx, MATCH_PEER_OUTGOING)) rc = SCE_NET_ADHOC_MATCHING_ERROR_REQUEST_IN_PROG;
        else if (peer->state == MATCH_PEER_OFFER) {
            peer->state = MATCH_PEER_OUTGOING;
            match_send_opt_pkt(ctx, MATCH_PKT_JOIN, peer->mac, olen, opt);
        } else {
            rc = SCE_NET_ADHOC_MATCHING_ERROR_TARGET_NOT_READY;
        }
    } else {
        if (match_find_by_state(ctx, MATCH_PEER_P2P)) rc = SCE_NET_ADHOC_MATCHING_ERROR_ALREADY_EST;
        else if (peer->state == MATCH_PEER_INCOMING) {
            peer->state = MATCH_PEER_P2P;
            peer->accepting = 1;
            match_send_accept_pkt(ctx, peer->mac, olen, opt);
        } else if (match_find_by_state(ctx, MATCH_PEER_OUTGOING)) {
            rc = SCE_NET_ADHOC_MATCHING_ERROR_REQUEST_IN_PROG;
        } else if (peer->state == MATCH_PEER_OFFER) {
            peer->state = MATCH_PEER_OUTGOING;
            match_send_opt_pkt(ctx, MATCH_PKT_JOIN, peer->mac, olen, opt);
        } else {
            rc = SCE_NET_ADHOC_MATCHING_ERROR_TARGET_NOT_READY;
        }
    }
    free(opt);
    return rc;
}

static uint32_t matching_cancel_impl(CpuState *s, int with_opt) {
    if (!adhoc_online_mode()) return hle_adhoc_refuse_offline(s);
    if (!s_matching.inited) return SCE_NET_ADHOC_MATCHING_ERROR_NOT_INIT;
    if (!A1 || !sr_guest_span_readable(A1, 6u)) return SCE_NET_ADHOC_MATCHING_ERROR_INVALID_ARG;
    MatchContext *ctx = match_find_ctx((int)(int32_t)A0);
    if (!ctx) return SCE_NET_ADHOC_MATCHING_ERROR_INVALID_ID;
    if (!ctx->running) return SCE_NET_ADHOC_MATCHING_ERROR_NOT_RUNNING;
    uint8_t tmac[6];
    for (uint32_t i = 0u; i < 6u; i++) tmac[i] = MEM_R8(A1 + i);
    MatchPeer *peer = match_find_peer(ctx, tmac);
    if (!peer || peer->state == MATCH_PEER_SELF) return SCE_NET_ADHOC_MATCHING_ERROR_UNKNOWN_TARGET;
    const int32_t olen = with_opt ? (int32_t)A2 : 0;
    if (olen < 0 || (olen > 0 && (!A3 || !sr_guest_span_readable(A3, (uint32_t)olen))))
        return SCE_NET_ADHOC_MATCHING_ERROR_INVALID_OPTLEN;
    uint8_t *opt = olen > 0 ? malloc((size_t)olen) : NULL;
    if (olen > 0 && !opt) return SCE_NET_ADHOC_MATCHING_ERROR_NO_SPACE;
    for (int32_t i = 0; i < olen; i++) opt[i] = MEM_R8(A3 + (uint32_t)i);
    match_send_opt_pkt(ctx, MATCH_PKT_CANCEL, peer->mac, olen, opt);
    if (peer->state == MATCH_PEER_INCOMING || peer->state == MATCH_PEER_OUTGOING ||
        peer->state == MATCH_PEER_CHILD || peer->state == MATCH_PEER_PARENT || peer->state == MATCH_PEER_P2P)
        peer->state = MATCH_PEER_OFFER;
    free(opt);
    return 0u;
}

static uint32_t h_NetAdhocMatchingCancelTarget(CpuState *s) { return matching_cancel_impl(s, 0); }
static uint32_t h_NetAdhocMatchingCancelTargetWithOpt(CpuState *s) { return matching_cancel_impl(s, 1); }

static uint32_t h_NetAdhocMatchingSetHelloOpt(CpuState *s) {
    if (!adhoc_online_mode()) return hle_adhoc_refuse_offline(s);
    if (!s_matching.inited) return SCE_NET_ADHOC_MATCHING_ERROR_NOT_INIT;
    MatchContext *ctx = match_find_ctx((int)(int32_t)A0);
    if (!ctx) return SCE_NET_ADHOC_MATCHING_ERROR_INVALID_ID;
    if (!ctx->running) return SCE_NET_ADHOC_MATCHING_ERROR_NOT_RUNNING;
    const int32_t olen = (int32_t)A1;
    if (olen < 0 || (olen > 0 && (!A2 || !sr_guest_span_readable(A2, (uint32_t)olen))))
        return SCE_NET_ADHOC_MATCHING_ERROR_INVALID_OPTLEN;
    free(ctx->hello);
    ctx->hello = NULL;
    ctx->hellolen = 0;
    if (olen > 0) {
        ctx->hello = malloc((size_t)olen);
        if (!ctx->hello) return SCE_NET_ADHOC_MATCHING_ERROR_NO_SPACE;
        for (int32_t i = 0; i < olen; i++) ctx->hello[i] = MEM_R8(A2 + (uint32_t)i);
        ctx->hellolen = olen;
    }
    return 0u;
}

static uint32_t h_NetAdhocMatchingGetHelloOpt(CpuState *s) {
    if (!adhoc_online_mode()) return hle_adhoc_refuse_offline(s);
    if (!s_matching.inited) return SCE_NET_ADHOC_MATCHING_ERROR_NOT_INIT;
    MatchContext *ctx = match_find_ctx((int)(int32_t)A0);
    if (!ctx) return SCE_NET_ADHOC_MATCHING_ERROR_INVALID_ID;
    if (!ctx->running) return SCE_NET_ADHOC_MATCHING_ERROR_NOT_RUNNING;
    if (!A1 || !sr_guest_span_writable(A1, 4u)) return SCE_NET_ADHOC_ERROR_INVALID_ARG;
    if (!A2) {
        MEM_W32(A1, (uint32_t)ctx->hellolen);
        return 0u;
    }
    const int32_t req = (int32_t)MEM_R32(A1);
    const uint32_t copy_n = (req > 0 && ctx->hellolen > 0)
                                ? ((uint32_t)req < (uint32_t)ctx->hellolen ? (uint32_t)req : (uint32_t)ctx->hellolen)
                                : 0u;
    if (copy_n > 0u && !sr_guest_span_writable(A2, copy_n))
        return hle_unmodelled_stop("sceNetAdhocMatchingGetHelloOpt", "with unwritable buf 0x%08x", A2);
    for (uint32_t i = 0u; i < copy_n; i++) MEM_W8(A2 + i, ctx->hello[i]);
    MEM_W32(A1, copy_n);
    return 0u;
}

static uint32_t h_NetAdhocMatchingGetMembers(CpuState *s) {
    if (!adhoc_online_mode()) return hle_adhoc_refuse_offline(s);
    if (!s_matching.inited) return SCE_NET_ADHOC_MATCHING_ERROR_NOT_INIT;
    if (!A1 || !sr_guest_span_writable(A1, 4u)) return SCE_NET_ADHOC_MATCHING_ERROR_INVALID_ARG;
    MatchContext *ctx = match_find_ctx((int)(int32_t)A0);
    if (!ctx) return SCE_NET_ADHOC_MATCHING_ERROR_INVALID_ID;
    if (!ctx->running) return SCE_NET_ADHOC_MATCHING_ERROR_NOT_RUNNING;
    uint8_t macs[MATCH_MAX_PEERS][6];
    int count = 0;
    const int parent_first = (ctx->mode == 2 && ctx->parent_first_at != 0u &&
                              sched_vtime_us() >= ctx->parent_first_at &&
                              match_find_by_state(ctx, MATCH_PEER_PARENT) != NULL);
    if (!parent_first) {
        memcpy(macs[count++], ctx->self_mac, 6u);
    }
    for (int i = 0; i < MATCH_MAX_PEERS && count < MATCH_MAX_PEERS; i++) {
        const MatchPeer *p = &ctx->peers[i];
        if (!p->used) continue;
        if (p->state == MATCH_PEER_PARENT || p->state == MATCH_PEER_CHILD || p->state == MATCH_PEER_P2P)
            memcpy(macs[count++], p->mac, 6u);
    }
    if (!A2) {
        MEM_W32(A1, (uint32_t)count * 12u);
        return 0u;
    }
    const int32_t req = (int32_t)MEM_R32(A1);
    int max_n = req > 0 ? (int)((uint32_t)req / 12u) : 0;
    if (max_n > count) max_n = count;
    if (max_n > 0 && !sr_guest_span_writable(A2, (uint32_t)max_n * 12u))
        return hle_unmodelled_stop("sceNetAdhocMatchingGetMembers", "with unwritable buf 0x%08x", A2);
    for (int i = 0; i < max_n; i++) {
        const uint32_t addr = A2 + (uint32_t)i * 12u;
        MEM_W32(addr, (i + 1 < max_n) ? addr + 12u : 0u);
        for (uint32_t k = 0u; k < 6u; k++) MEM_W8(addr + 4u + k, macs[i][k]);
        MEM_W16(addr + 10u, 0u);
    }
    MEM_W32(A1, (uint32_t)max_n * 12u);
    return 0u;
}

static uint32_t h_NetAdhocMatchingSendData(CpuState *s) {
    if (!adhoc_online_mode()) return hle_adhoc_refuse_offline(s);
    if (!s_matching.inited) return SCE_NET_ADHOC_MATCHING_ERROR_NOT_INIT;
    if (!A1 || !sr_guest_span_readable(A1, 6u)) return SCE_NET_ADHOC_MATCHING_ERROR_INVALID_ARG;
    MatchContext *ctx = match_find_ctx((int)(int32_t)A0);
    if (!ctx) return SCE_NET_ADHOC_MATCHING_ERROR_INVALID_ID;
    if (!ctx->running) return SCE_NET_ADHOC_MATCHING_ERROR_NOT_RUNNING;
    uint8_t tmac[6];
    for (uint32_t i = 0u; i < 6u; i++) tmac[i] = MEM_R8(A1 + i);
    MatchPeer *peer = match_find_peer(ctx, tmac);
    if (!peer) return SCE_NET_ADHOC_MATCHING_ERROR_UNKNOWN_TARGET;
    if (peer->state != MATCH_PEER_PARENT && peer->state != MATCH_PEER_CHILD &&
        peer->state != MATCH_PEER_P2P)
        return SCE_NET_ADHOC_MATCHING_ERROR_NOT_EST;
    if (peer->sending) return SCE_NET_ADHOC_MATCHING_ERROR_DATA_BUSY;
    const int32_t dlen = (int32_t)A2;
    if (dlen <= 0 || (uint32_t)dlen > NK_RELAY_PDP_MAX - 5u || !A3 ||
        !sr_guest_span_readable(A3, (uint32_t)dlen))
        return SCE_NET_ADHOC_MATCHING_ERROR_INVALID_DATALEN;
    uint8_t *pkt = malloc(5u + (size_t)dlen);
    if (!pkt) return SCE_NET_ADHOC_MATCHING_ERROR_NO_SPACE;
    pkt[0] = MATCH_PKT_BULK;
    pkt[1] = (uint8_t)dlen;
    pkt[2] = (uint8_t)(dlen >> 8);
    pkt[3] = (uint8_t)(dlen >> 16);
    pkt[4] = (uint8_t)(dlen >> 24);
    for (int32_t i = 0; i < dlen; i++) pkt[5 + i] = MEM_R8(A3 + (uint32_t)i);
    peer->sending = 1;
    match_send_pkt(ctx, peer->mac, pkt, 5u + (uint32_t)dlen);
    free(pkt);
    if (adhoc_peer_left_at(peer->mac) != 0u) {
        peer->data_timeout_at = sched_vtime_us() +
            (uint64_t)ctx->resend_int * (uint64_t)(ctx->resend_cnt > 0 ? ctx->resend_cnt : 1);
    } else {
        match_spawn_event(ctx, MATCH_EV_DATA_ACK, peer->mac, 0, NULL);
    }
    sched_service_kick(ctx->thread_uid);
    return 0u;
}

static uint32_t h_NetAdhocMatchingAbortSendData(CpuState *s) {
    if (!adhoc_online_mode()) return hle_adhoc_refuse_offline(s);
    if (!s_matching.inited) return SCE_NET_ADHOC_MATCHING_ERROR_NOT_INIT;
    if (!A1 || !sr_guest_span_readable(A1, 6u)) return SCE_NET_ADHOC_MATCHING_ERROR_INVALID_ARG;
    MatchContext *ctx = match_find_ctx((int)(int32_t)A0);
    if (!ctx) return SCE_NET_ADHOC_MATCHING_ERROR_INVALID_ID;
    if (!ctx->running) return SCE_NET_ADHOC_MATCHING_ERROR_NOT_RUNNING;
    uint8_t tmac[6];
    for (uint32_t i = 0u; i < 6u; i++) tmac[i] = MEM_R8(A1 + i);
    MatchPeer *peer = match_find_peer(ctx, tmac);
    if (!peer) return SCE_NET_ADHOC_MATCHING_ERROR_UNKNOWN_TARGET;
    peer->sending = 0;
    peer->ack_aborted = 1;
    return 0u;
}

static uint32_t match_any_ctx_live(void) {
    for (int i = 0; i < MATCH_MAX_CONTEXTS; i++) {
        if (s_matching.ctx[i].used) return 1u;
    }
    return 0u;
}

static uint32_t h_NetAdhocMatchingGetPoolMaxAlloc(CpuState *s) {
    if (!adhoc_online_mode()) return hle_adhoc_refuse_offline(s);
    const uint32_t pool = s_matching.pool_size >= 0x20u ? s_matching.pool_size - 0x20u : 0u;
    return match_any_ctx_live() ? pool / 2u : 0u;
}

static uint32_t h_NetAdhocMatchingGetPoolStat(CpuState *s) {
    if (!adhoc_online_mode()) return hle_adhoc_refuse_offline(s);
    if (!s_matching.inited) return SCE_NET_ADHOC_MATCHING_ERROR_NOT_INIT;
    if (!A0 || !sr_guest_span_writable(A0, 12u)) return SCE_NET_ADHOC_MATCHING_ERROR_INVALID_ARG;
    const uint32_t pool = s_matching.pool_size >= 0x20u ? s_matching.pool_size - 0x20u : 0u;
    const uint32_t max_alloc = match_any_ctx_live() ? pool / 2u : 0u;
    MEM_W32(A0, pool);
    MEM_W32(A0 + 4u, max_alloc);
    MEM_W32(A0 + 8u, pool - max_alloc);
    return 0u;
}

/* A new program (LoadExec) starts with the libraries uninitialized. The sceNet pool lives in the
 * user partition, which the same reset rebuilds, and the scheduler reset retires the ad hoc
 * control thread, so nothing is freed here. */
void hle_net_reset(void) {
    adhoc_close_sockets_and_matching();
    adhoc_release_transport();
    s_netlib.inited = 0;
    s_netlib.pool_uid = NET_POOL_NONE;
    const uint32_t gen = s_adhocctl.gen + 1u;
    memset(&s_adhocctl, 0, sizeof(s_adhocctl));
    s_adhocctl.gen = gen;
}
int sr_hle_test_adhocctl_state(void) { return s_adhocctl.inited ? s_adhocctl.state : -1; }
uint32_t sr_hle_test_adhocctl_thread(void) { return s_adhocctl.thread_uid; }
int sr_hle_test_net_inited(void) { return s_netlib.inited; }
uint32_t sr_hle_test_net_pool_uid(void) { return s_netlib.pool_uid; }

/* Registration runs of the shared helpers in hle.c (#307). Each holds one run of this file's
 * registrations, in their order, and is called from the helper where the run stood. */

/* From hle_register_console_profile_handlers(). */
void hle_register_console_profile_network(void) {
    sr_hle_register(0x0c622081, "sceWlanGetEtherAddr", h_WlanGetEtherAddr);
    sr_hle_register(0x93440b11, "sceWlanDevIsPowerOn", h_WlanOn);
    sr_hle_register(0xd7763699, "sceWlanGetSwitchState", h_WlanOn);
}

/* From hle_register_unregistered_import_batch(). */
void hle_register_unregistered_import_network_1(void) {
    /* The core library with no network (h_NetInit): it starts and stops as on a PSP, and the
     * local address is unavailable because no group or access point ever connects. */
    sr_hle_register(0x0bf0a3aeu, "sceNetGetLocalEtherAddr", h_NetGetLocalEtherAddr);
    sr_hle_register(0x89360950u, "sceNetEtherNtostr", h_NetEtherNtostr);
    sr_hle_register(0x39af39a6u, "sceNetInit", h_NetInit);
    sr_hle_register(0x281928a9u, "sceNetTerm", h_NetTerm);
    /* The net stack's thread and memory bookkeeping, and ad hoc discovery, which has no peers to
     * find, stay refused with the offline network code 0x80010086 (SCE_KERNEL_ERROR_ERRNO_
     * FUNCTION_NOT_SUPPORTED): their answers with no network are not modelled yet. */
    sr_hle_register_unsupported(0x50647530u, "sceNetFreeThreadinfo", 0x80010086u);
    sr_hle_register_unsupported(0xcc393e48u, "sceNetGetMallocStat", 0x80010086u);
    sr_hle_register_unsupported(0xf7d13214u, "sceNetAdhocDiscoverStop", 0x80010086u);
    /* sceHttpsEnd follows the offline HTTP policy of sceHttpEnd and sceHttpInit: no network stack. */
    sr_hle_register_unsupported(0xf9d8eb63u, "sceHttpsEnd", 0x80010086u);
    /* The SSL library has no handler in this runtime, which has no network stack. sceSslEnd follows
     * the same offline policy as the HTTP family (0x80010086); the other SSL calls are not modelled. */
    sr_hle_register_unsupported(0x191cdeffu, "sceSslEnd", 0x80010086u);
    /* sceNetApctl is the access-point control library; with no network stack the runtime refuses its
     * handler calls under the offline network policy (0x80010086). */
    sr_hle_register_unsupported(0x5963991bu, "sceNetApctlDelHandler", 0x80010086u);
    /* The public ad hoc libraries (#1139 phase 0) sit above sceNet: the PSPSDK prototypes in
     * pspnet_adhoc.h (22), pspnet_adhocctl.h (21) and pspnet_adhocmatching.h (16), each NID
     * sha1(name) as tools/nid_corpus.json records for its pspsdk-sourced names. With no network
     * stack they refuse under the offline network policy, with the runtime's documented
     * SCE_KERNEL_ERROR_ERRNO_FUNCTION_NOT_SUPPORTED (0x80010086). The code is not a measured ad
     * hoc result; PSP-visible ad hoc state and errors are unmeasured. sceNetAdhocctl's
     * lone-console calls are the exception (h_NetAdhocctlInit, #1139 phase 1a); its Join, game-mode
     * and name-lookup calls stay refused: no lone-console recording covers them. */
    sr_hle_register_unsupported(0x7f75c338u, "sceNetAdhocGameModeCreateMaster", 0x80010086u);
    sr_hle_register_unsupported(0x3278ab0cu, "sceNetAdhocGameModeCreateReplica", 0x80010086u);
    sr_hle_register_unsupported(0xa0229362u, "sceNetAdhocGameModeDeleteMaster", 0x80010086u);
    sr_hle_register_unsupported(0x0b2228e9u, "sceNetAdhocGameModeDeleteReplica", 0x80010086u);
    sr_hle_register_unsupported(0x98c204c8u, "sceNetAdhocGameModeUpdateMaster", 0x80010086u);
    sr_hle_register_unsupported(0xfa324b4eu, "sceNetAdhocGameModeUpdateReplica", 0x80010086u);
    sr_hle_register(0xc7c1fc57u, "sceNetAdhocGetPdpStat", h_NetAdhocGetPdpStat);
    sr_hle_register(0xb9685118u, "sceNetAdhocGetPtpStat", h_NetAdhocGetPtpStat);
    sr_hle_register(0x4d2ce199u, "sceNetAdhocGetSocketAlert", h_NetAdhocGetSocketAlert);
    sr_hle_register(0xe1d621d7u, "sceNetAdhocInit", h_NetAdhocInit);
    sr_hle_register(0x6f92741bu, "sceNetAdhocPdpCreate", h_NetAdhocPdpCreate);
    sr_hle_register(0x7f27bb5eu, "sceNetAdhocPdpDelete", h_NetAdhocPdpDelete);
    sr_hle_register(0xdfe53e03u, "sceNetAdhocPdpRecv", h_NetAdhocPdpRecv);
    sr_hle_register(0xabed3790u, "sceNetAdhocPdpSend", h_NetAdhocPdpSend);
    sr_hle_register(0x7a662d6bu, "sceNetAdhocPollSocket", h_NetAdhocPollSocket);
    sr_hle_register(0x9df81198u, "sceNetAdhocPtpAccept", h_NetAdhocPtpAccept);
    sr_hle_register(0x157e6225u, "sceNetAdhocPtpClose", h_NetAdhocPtpClose);
    sr_hle_register(0xfc6fc07bu, "sceNetAdhocPtpConnect", h_NetAdhocPtpConnect);
    sr_hle_register(0x9ac2eeacu, "sceNetAdhocPtpFlush", h_NetAdhocPtpFlush);
    sr_hle_register(0xe08bdac1u, "sceNetAdhocPtpListen", h_NetAdhocPtpListen);
    sr_hle_register(0x877f6d66u, "sceNetAdhocPtpOpen", h_NetAdhocPtpOpen);
    sr_hle_register(0x8bea2b3eu, "sceNetAdhocPtpRecv", h_NetAdhocPtpRecv);
    sr_hle_register(0x4da4c788u, "sceNetAdhocPtpSend", h_NetAdhocPtpSend);
    sr_hle_register(0x73bfd52du, "sceNetAdhocSetSocketAlert", h_NetAdhocSetSocketAlert);
    sr_hle_register(0xa62c6f57u, "sceNetAdhocTerm", h_NetAdhocTerm);
    sr_hle_register(0x20b317a0u, "sceNetAdhocctlAddHandler", h_NetAdhocctlAddHandler);
    sr_hle_register(0x0ad043edu, "sceNetAdhocctlConnect", h_NetAdhocctlConnect);
    sr_hle_register(0xec0635c1u, "sceNetAdhocctlCreate", h_NetAdhocctlCreate);
    sr_hle_register_unsupported(0xa5c055ceu, "sceNetAdhocctlCreateEnterGameMode", 0x80010086u);
    sr_hle_register(0x6402490bu, "sceNetAdhocctlDelHandler", h_NetAdhocctlDelHandler);
    sr_hle_register(0x34401d65u, "sceNetAdhocctlDisconnect", h_NetAdhocctlDisconnect);
    sr_hle_register_unsupported(0xcf8e084du, "sceNetAdhocctlExitGameMode", 0x80010086u);
    sr_hle_register(0x99560abeu, "sceNetAdhocctlGetAddrByName", h_NetAdhocctlGetAddrByName);
    sr_hle_register(0x362cbe8fu, "sceNetAdhocctlGetAdhocId", h_NetAdhocctlGetAdhocId);
    sr_hle_register_unsupported(0x5a014ce0u, "sceNetAdhocctlGetGameModeInfo", 0x80010086u);
    sr_hle_register(0x8916c003u, "sceNetAdhocctlGetNameByAddr", h_NetAdhocctlGetNameByAddr);
    sr_hle_register(0xded9d28eu, "sceNetAdhocctlGetParameter", h_NetAdhocctlGetParameter);
    sr_hle_register(0x8db83fdcu, "sceNetAdhocctlGetPeerInfo", h_NetAdhocctlGetPeerInfo);
    sr_hle_register(0xe162cb14u, "sceNetAdhocctlGetPeerList", h_NetAdhocctlGetPeerList);
    sr_hle_register(0x81aee1beu, "sceNetAdhocctlGetScanInfo", h_NetAdhocctlGetScanInfo);
    sr_hle_register(0x75ecd386u, "sceNetAdhocctlGetState", h_NetAdhocctlGetState);
    sr_hle_register(0xe26f226eu, "sceNetAdhocctlInit", h_NetAdhocctlInit);
    sr_hle_register(0x5e7f79c9u, "sceNetAdhocctlJoin", h_NetAdhocctlJoin);
    sr_hle_register_unsupported(0x1ff89745u, "sceNetAdhocctlJoinEnterGameMode", 0x80010086u);
    sr_hle_register(0x08fff7a0u, "sceNetAdhocctlScan", h_NetAdhocctlScan);
    sr_hle_register(0x9d689e13u, "sceNetAdhocctlTerm", h_NetAdhocctlTerm);
    sr_hle_register(0xec19337du, "sceNetAdhocMatchingAbortSendData", h_NetAdhocMatchingAbortSendData);
    sr_hle_register(0xea3c6108u, "sceNetAdhocMatchingCancelTarget", h_NetAdhocMatchingCancelTarget);
    sr_hle_register(0x8f58bedfu, "sceNetAdhocMatchingCancelTargetWithOpt", h_NetAdhocMatchingCancelTargetWithOpt);
    sr_hle_register(0xca5eda6fu, "sceNetAdhocMatchingCreate", h_NetAdhocMatchingCreate);
    sr_hle_register(0xf16eaf4fu, "sceNetAdhocMatchingDelete", h_NetAdhocMatchingDelete);
    sr_hle_register(0xb5d96c2au, "sceNetAdhocMatchingGetHelloOpt", h_NetAdhocMatchingGetHelloOpt);
    sr_hle_register(0xc58bcd9eu, "sceNetAdhocMatchingGetMembers", h_NetAdhocMatchingGetMembers);
    sr_hle_register(0x40f8f435u, "sceNetAdhocMatchingGetPoolMaxAlloc", h_NetAdhocMatchingGetPoolMaxAlloc);
    sr_hle_register(0x9c5cfb7du, "sceNetAdhocMatchingGetPoolStat", h_NetAdhocMatchingGetPoolStat);
    sr_hle_register(0x2a2a1e07u, "sceNetAdhocMatchingInit", h_NetAdhocMatchingInit);
    sr_hle_register(0x5e3d4b79u, "sceNetAdhocMatchingSelectTarget", h_NetAdhocMatchingSelectTarget);
    sr_hle_register(0xf79472d7u, "sceNetAdhocMatchingSendData", h_NetAdhocMatchingSendData);
    sr_hle_register(0xb58e61b7u, "sceNetAdhocMatchingSetHelloOpt", h_NetAdhocMatchingSetHelloOpt);
    sr_hle_register(0x93ef3843u, "sceNetAdhocMatchingStart", h_NetAdhocMatchingStart);
    sr_hle_register(0xe8454c65u, "sceNetAdhocMatchingStart2", h_NetAdhocMatchingStart2);
    sr_hle_register(0x32b156b3u, "sceNetAdhocMatchingStop", h_NetAdhocMatchingStop);
    sr_hle_register(0x7945ecdau, "sceNetAdhocMatchingTerm", h_NetAdhocMatchingTerm);
}

/* From hle_register_unregistered_import_batch(). */
void hle_register_unregistered_import_network_2(void) {
    sr_hle_register_unsupported(0x0282a3bdu, "sceHttpGetContentLength", 0x80010086u);
    sr_hle_register_unsupported(0x03d9526fu, "sceHttpSetResolveRetry", 0x80010086u);
    sr_hle_register_unsupported(0x1f0fc3e3u, "sceHttpSetRecvTimeOut", 0x80010086u);
    sr_hle_register_unsupported(0x2255551eu, "sceHttpGetNetworkPspError", 0x80010086u);
    sr_hle_register_unsupported(0x3eaba285u, "sceHttpAddExtraHeader", 0x80010086u);
    sr_hle_register_unsupported(0xab1abe07u, "sceHttpInit", 0x80010086u);
    sr_hle_register_unsupported(0xd1c8945eu, "sceHttpEnd", 0x80010086u);
}

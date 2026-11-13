// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-11.
// See NOTICE.md for upstream lineage and modification provenance.
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later
// Copyright (c) 2012- PPSSPP Project and contributors

/* Networking: sceNet, sceHttp, sceSsl and sceWlan, refused or answered without a network.
 * Extracted from hle.c on 2026-10-31 (#307). */

#include "hle_internal.h"

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
    if (s_adhocctl.request == ADHOCCTL_REQ_NONE && s_adhocctl.error_events == 0u) return 1;
    hle_unmodelled_stop(name, "while a request is in flight (UNMEASURED)");
    return 0;
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
    s_adhocctl.gen++;
    s_adhocctl.inited = 0;
    s_adhocctl.thread_uid = 0u;
    s_adhocctl.state = 0;
    s_adhocctl.request = ADHOCCTL_REQ_NONE;
    s_adhocctl.error_events = 0u;
    s_adhocctl.radio_used = 0;
    memset(s_adhocctl.handlers, 0, sizeof(s_adhocctl.handlers));
    if (uid) sched_service_kick(uid);
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
    /* The product is not checked: an upper-case and a lower-case first letter are both accepted
     * (adhocctl.expected:13, 18); other spellings are UNMEASURED and accepted the same way. */
    const uint32_t gen = s_adhocctl.gen + 1u;
    const uint32_t uid = sched_create_service_thread("SceNetAdhocctl", priority, stack,
                                                     adhocctl_thread_body, (void *)(uintptr_t)gen);
    if (uid >= 0x80000000u) return uid;   /* ILLEGAL_PRIORITY (adhocctl.expected:14-15), or the
                                           * kernel's NO_MEMORY (UNMEASURED here) */
    s_adhocctl.gen = gen;
    s_adhocctl.thread_uid = uid;
    for (uint32_t i = 0; i < 13u; i++) s_adhocctl.id[i] = MEM_R8(id + i);
    s_adhocctl.inited = 1;
    s_adhocctl.state = 0;
    s_adhocctl.request = ADHOCCTL_REQ_NONE;
    s_adhocctl.error_events = 0u;
    s_adhocctl.radio_used = 0;
    memset(s_adhocctl.handlers, 0, sizeof(s_adhocctl.handlers));
    return 0u;
}

/* sceNetAdhocctlTerm(void): 0 before Init, after Term and after a refused second Init
 * (adhocctl.expected:7, 91-92); in a group it leaves it with no event recorded, and the library is
 * uninitialized (term.expected:25-26). Term with a request or an event still in flight is
 * UNMEASURED. */
static uint32_t h_NetAdhocctlTerm(CpuState *s) {
    (void)s;
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
    s_adhocctl.request = ADHOCCTL_REQ_DISCONNECT;
    s_adhocctl.request_due = sched_vtime_us() + ADHOCCTL_DISCONNECT_US;
    sched_service_kick(s_adhocctl.thread_uid);
    return 0u;
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
        uint8_t b = s_console.mac[i];
        if (i == 0u) b = (uint8_t)((b & 0xfcu) | 0x02u);
        MEM_W8(A0 + 140u + i, b);
    }
    return 0u;
}

/* sceNetAdhocctlGetPeerList(int *size, SceNetAdhocctlPeerInfo *buf): nobody else is in a group,
 * so *size becomes 0. Out of a group 0x80410682 with *size 0 (adhocctl.expected:28, 68;
 * ctlinit.expected:20, a NULL buffer); in a group 0 with *size 0, with a NULL buffer
 * (adhocctl.expected:52) and with a buffer (abandon.expected "Peer list: 00000000, 0 peers").
 * A buffer out of a group is UNMEASURED. */
static uint32_t h_NetAdhocctlGetPeerList(CpuState *s) {
    if (!s_adhocctl.inited) return SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
    if (!adhocctl_query_settled("sceNetAdhocctlGetPeerList")) return HLE_KERNEL_ERROR_NOT_IMPLEMENTED;
    if (!A0 || !sr_guest_span_writable(A0, 4u) || (s_adhocctl.state != 1 && A1 != 0u))
        return hle_unmodelled_stop("sceNetAdhocctlGetPeerList", "with size 0x%08x and buffer 0x%08x "
                                   "in state %d (UNMEASURED)", A0, A1, s_adhocctl.state);
    MEM_W32(A0, 0u);
    return s_adhocctl.state == 1 ? 0u : ADHOCCTL_PEER_LIST_DISCONNECTED;
}

/* sceNetAdhocctlGetScanInfo(int *size, SceNetAdhocctlScanInfo *buf): nothing is in range, so 0
 * with *size 0, in and out of a group and after a scan (adhocctl.expected:29, 55, 75;
 * ctlinit.expected:21), each with a NULL buffer. A buffer is UNMEASURED. */
static uint32_t h_NetAdhocctlGetScanInfo(CpuState *s) {
    if (!s_adhocctl.inited) return SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
    if (!adhocctl_query_settled("sceNetAdhocctlGetScanInfo")) return HLE_KERNEL_ERROR_NOT_IMPLEMENTED;
    if (!A0 || !sr_guest_span_writable(A0, 4u) || A1 != 0u)
        return hle_unmodelled_stop("sceNetAdhocctlGetScanInfo", "with size 0x%08x and buffer 0x%08x "
                                   "(UNMEASURED)", A0, A1);
    MEM_W32(A0, 0u);
    return 0u;
}

/* sceNetAdhocctlGetPeerInfo(const u8 *mac, int size, SceNetAdhocctlPeerInfo *info): in a group,
 * nobody is a peer, the console itself included: NO_ENTRY for its own MAC and another
 * (adhocctl.expected:53-54). Out of a group it is UNMEASURED. */
static uint32_t h_NetAdhocctlGetPeerInfo(CpuState *s) {
    if (!s_adhocctl.inited) return SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED;
    if (!adhocctl_query_settled("sceNetAdhocctlGetPeerInfo")) return HLE_KERNEL_ERROR_NOT_IMPLEMENTED;
    if (s_adhocctl.state != 1 || !A0 || !sr_guest_span_readable(A0, 6u))
        return hle_unmodelled_stop("sceNetAdhocctlGetPeerInfo", "with mac 0x%08x in state %d "
                                   "(UNMEASURED)", A0, s_adhocctl.state);
    return ADHOC_ERROR_NO_ENTRY;
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
    if (s_adhocctl.state != 1 || s_adhocctl.request != ADHOCCTL_REQ_NONE)
        return hle_unmodelled_stop("sceNetGetLocalEtherAddr", "after a scan or group request, out of "
                                   "a group (UNMEASURED)");
    if (!A0 || !sr_guest_span_writable(A0, 6u))
        return hle_unmodelled_stop("sceNetGetLocalEtherAddr", "with output 0x%08x (UNMEASURED)", A0);
    if (!console_profile_identity())
        return hle_unmodelled_stop("sceNetGetLocalEtherAddr", "has no address: the host's random "
                                   "source is unavailable");
    for (uint32_t i = 0; i < 6u; i++) MEM_W8(A0 + i, s_console.mac[i]);
    return 0u;
}

/* A new program (LoadExec) starts with the libraries uninitialized. The sceNet pool lives in the
 * user partition, which the same reset rebuilds, and the scheduler reset retires the ad hoc
 * control thread, so nothing is freed here. */
void hle_net_reset(void) {
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
    sr_hle_register_unsupported(0xc7c1fc57u, "sceNetAdhocGetPdpStat", 0x80010086u);
    sr_hle_register_unsupported(0xb9685118u, "sceNetAdhocGetPtpStat", 0x80010086u);
    sr_hle_register_unsupported(0xe1d621d7u, "sceNetAdhocInit", 0x80010086u);
    sr_hle_register_unsupported(0x6f92741bu, "sceNetAdhocPdpCreate", 0x80010086u);
    sr_hle_register_unsupported(0x7f27bb5eu, "sceNetAdhocPdpDelete", 0x80010086u);
    sr_hle_register_unsupported(0xdfe53e03u, "sceNetAdhocPdpRecv", 0x80010086u);
    sr_hle_register_unsupported(0xabed3790u, "sceNetAdhocPdpSend", 0x80010086u);
    sr_hle_register_unsupported(0x9df81198u, "sceNetAdhocPtpAccept", 0x80010086u);
    sr_hle_register_unsupported(0x157e6225u, "sceNetAdhocPtpClose", 0x80010086u);
    sr_hle_register_unsupported(0xfc6fc07bu, "sceNetAdhocPtpConnect", 0x80010086u);
    sr_hle_register_unsupported(0x9ac2eeacu, "sceNetAdhocPtpFlush", 0x80010086u);
    sr_hle_register_unsupported(0xe08bdac1u, "sceNetAdhocPtpListen", 0x80010086u);
    sr_hle_register_unsupported(0x877f6d66u, "sceNetAdhocPtpOpen", 0x80010086u);
    sr_hle_register_unsupported(0x8bea2b3eu, "sceNetAdhocPtpRecv", 0x80010086u);
    sr_hle_register_unsupported(0x4da4c788u, "sceNetAdhocPtpSend", 0x80010086u);
    sr_hle_register_unsupported(0xa62c6f57u, "sceNetAdhocTerm", 0x80010086u);
    sr_hle_register(0x20b317a0u, "sceNetAdhocctlAddHandler", h_NetAdhocctlAddHandler);
    sr_hle_register(0x0ad043edu, "sceNetAdhocctlConnect", h_NetAdhocctlConnect);
    sr_hle_register(0xec0635c1u, "sceNetAdhocctlCreate", h_NetAdhocctlCreate);
    sr_hle_register_unsupported(0xa5c055ceu, "sceNetAdhocctlCreateEnterGameMode", 0x80010086u);
    sr_hle_register(0x6402490bu, "sceNetAdhocctlDelHandler", h_NetAdhocctlDelHandler);
    sr_hle_register(0x34401d65u, "sceNetAdhocctlDisconnect", h_NetAdhocctlDisconnect);
    sr_hle_register_unsupported(0xcf8e084du, "sceNetAdhocctlExitGameMode", 0x80010086u);
    sr_hle_register_unsupported(0x99560abeu, "sceNetAdhocctlGetAddrByName", 0x80010086u);
    sr_hle_register(0x362cbe8fu, "sceNetAdhocctlGetAdhocId", h_NetAdhocctlGetAdhocId);
    sr_hle_register_unsupported(0x5a014ce0u, "sceNetAdhocctlGetGameModeInfo", 0x80010086u);
    sr_hle_register_unsupported(0x8916c003u, "sceNetAdhocctlGetNameByAddr", 0x80010086u);
    sr_hle_register(0xded9d28eu, "sceNetAdhocctlGetParameter", h_NetAdhocctlGetParameter);
    sr_hle_register(0x8db83fdcu, "sceNetAdhocctlGetPeerInfo", h_NetAdhocctlGetPeerInfo);
    sr_hle_register(0xe162cb14u, "sceNetAdhocctlGetPeerList", h_NetAdhocctlGetPeerList);
    sr_hle_register(0x81aee1beu, "sceNetAdhocctlGetScanInfo", h_NetAdhocctlGetScanInfo);
    sr_hle_register(0x75ecd386u, "sceNetAdhocctlGetState", h_NetAdhocctlGetState);
    sr_hle_register(0xe26f226eu, "sceNetAdhocctlInit", h_NetAdhocctlInit);
    sr_hle_register_unsupported(0x5e7f79c9u, "sceNetAdhocctlJoin", 0x80010086u);
    sr_hle_register_unsupported(0x1ff89745u, "sceNetAdhocctlJoinEnterGameMode", 0x80010086u);
    sr_hle_register(0x08fff7a0u, "sceNetAdhocctlScan", h_NetAdhocctlScan);
    sr_hle_register(0x9d689e13u, "sceNetAdhocctlTerm", h_NetAdhocctlTerm);
    sr_hle_register_unsupported(0xec19337du, "sceNetAdhocMatchingAbortSendData", 0x80010086u);
    sr_hle_register_unsupported(0xea3c6108u, "sceNetAdhocMatchingCancelTarget", 0x80010086u);
    sr_hle_register_unsupported(0x8f58bedfu, "sceNetAdhocMatchingCancelTargetWithOpt", 0x80010086u);
    sr_hle_register_unsupported(0xca5eda6fu, "sceNetAdhocMatchingCreate", 0x80010086u);
    sr_hle_register_unsupported(0xf16eaf4fu, "sceNetAdhocMatchingDelete", 0x80010086u);
    sr_hle_register_unsupported(0xb5d96c2au, "sceNetAdhocMatchingGetHelloOpt", 0x80010086u);
    sr_hle_register_unsupported(0xc58bcd9eu, "sceNetAdhocMatchingGetMembers", 0x80010086u);
    sr_hle_register_unsupported(0x40f8f435u, "sceNetAdhocMatchingGetPoolMaxAlloc", 0x80010086u);
    sr_hle_register_unsupported(0x9c5cfb7du, "sceNetAdhocMatchingGetPoolStat", 0x80010086u);
    sr_hle_register_unsupported(0x2a2a1e07u, "sceNetAdhocMatchingInit", 0x80010086u);
    sr_hle_register_unsupported(0x5e3d4b79u, "sceNetAdhocMatchingSelectTarget", 0x80010086u);
    sr_hle_register_unsupported(0xf79472d7u, "sceNetAdhocMatchingSendData", 0x80010086u);
    sr_hle_register_unsupported(0xb58e61b7u, "sceNetAdhocMatchingSetHelloOpt", 0x80010086u);
    sr_hle_register_unsupported(0x93ef3843u, "sceNetAdhocMatchingStart", 0x80010086u);
    sr_hle_register_unsupported(0x32b156b3u, "sceNetAdhocMatchingStop", 0x80010086u);
    sr_hle_register_unsupported(0x7945ecdau, "sceNetAdhocMatchingTerm", 0x80010086u);
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

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// nk_xr_mailbox: a lock-free latest-frame mailbox (a triple buffer) between one
// producer, the presenter, which publishes once per presented frame, and one
// consumer, the XR frame thread, which reads the newest published frame once per
// headset frame.
//
// The mailbox holds no pixel data. It names which of three slots (0, 1, 2) the
// producer last filled; the caller owns three images or buffers indexed by slot.
//
// Roles (single producer, single consumer; neither role may be shared):
//   producer: nk_xr_mailbox_write_slot(), nk_xr_mailbox_publish()
//   consumer: nk_xr_mailbox_acquire()
//
// Invariant: the three slots are always split one each between the producer's
// private "back" slot, the shared "middle" slot and the consumer's private
// "front" slot. The slot the producer may write is never the slot the consumer
// reads, so a read cannot be torn by construction.
//
// Progress: the producer never waits. nk_xr_mailbox_write_slot() is a plain load
// of producer-private state and nk_xr_mailbox_publish() is one atomic exchange. A
// slow or stalled consumer only means older frames are overwritten in the middle
// slot; it never delays the producer. The consumer's acquire is also one atomic
// exchange and does not wait for the producer.
//
// Memory ordering: the shared word is exchanged with memory_order_acq_rel.
//   - The producer's writes into its slot, and the frame id stored by publish,
//     happen before the publishing exchange (release). The consumer's acquiring
//     exchange (acquire) observes that exchange, so it sees the whole slot.
//   - The consumer finishes reading its old front slot before it exchanges that
//     slot back into the shared word (release). The producer receives it in its
//     next publishing exchange (acquire) before it writes it again.
// No other synchronisation is needed.

#ifndef NK_XR_MAILBOX_H
#define NK_XR_MAILBOX_H

#include <stdalign.h>
#include <stdatomic.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NK_XR_MAILBOX_SLOTS 3

typedef struct NkXrMailbox {
    // Shared word: low two bits are the middle slot, bit 2 is set when that
    // slot holds a frame the consumer has not taken yet.
    alignas(64) atomic_uint middle;
    // Producer-private.
    alignas(64) unsigned back;
    // Consumer-private.
    alignas(64) unsigned front;
    int have_front;
    // Frame id of each slot. Written by the producer before the slot is
    // published, read by the consumer after it acquires the slot.
    uint64_t frame_id[NK_XR_MAILBOX_SLOTS];
} NkXrMailbox;

// Prepare an empty mailbox. Call before either role starts.
void nk_xr_mailbox_init(NkXrMailbox *m);

// Producer: the slot it may fill now. Never the consumer's slot. Wait-free.
int nk_xr_mailbox_write_slot(const NkXrMailbox *m);

// Producer: the slot returned by nk_xr_mailbox_write_slot() is complete; make it
// the latest frame under `frame_id`. After this call the producer's next write
// slot is a different slot. Wait-free (one atomic exchange).
void nk_xr_mailbox_publish(NkXrMailbox *m, uint64_t frame_id);

// Consumer: the slot that holds the latest published frame. When a newer frame
// was published since the last call, the consumer moves to it; otherwise it
// keeps its current slot, which stays valid and unmodified until the next call
// that finds a newer frame. Returns -1 before the first publish. `frame_id` may
// be NULL; when not NULL it receives the id of the returned slot's frame.
int nk_xr_mailbox_acquire(NkXrMailbox *m, uint64_t *frame_id);

#ifdef __cplusplus
}
#endif

#endif

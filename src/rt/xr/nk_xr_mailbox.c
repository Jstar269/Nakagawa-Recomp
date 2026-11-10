// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// Latest-frame triple-buffer mailbox. The design, roles and memory ordering are
// documented in nk_xr_mailbox.h.

#include "xr/nk_xr_mailbox.h"

#include <stddef.h>

#define NK_XR_MB_SLOT_MASK 3u
#define NK_XR_MB_DIRTY 4u

void nk_xr_mailbox_init(NkXrMailbox *m) {
    // Back = 0 (producer), middle = 1 (clean), front = 2 (consumer).
    atomic_init(&m->middle, 1u);
    m->back = 0u;
    m->front = 2u;
    m->have_front = 0;
    for (int i = 0; i < NK_XR_MAILBOX_SLOTS; i++) {
        m->frame_id[i] = 0u;
    }
}

int nk_xr_mailbox_write_slot(const NkXrMailbox *m) {
    return (int)m->back;
}

void nk_xr_mailbox_publish(NkXrMailbox *m, uint64_t frame_id) {
    m->frame_id[m->back] = frame_id;
    // Hand the finished slot over as the new, unread middle slot and take the
    // previous middle slot (read or not) as the next write slot.
    unsigned old = atomic_exchange_explicit(&m->middle, m->back | NK_XR_MB_DIRTY, memory_order_acq_rel);
    m->back = old & NK_XR_MB_SLOT_MASK;
}

int nk_xr_mailbox_acquire(NkXrMailbox *m, uint64_t *frame_id) {
    if (atomic_load_explicit(&m->middle, memory_order_relaxed) & NK_XR_MB_DIRTY) {
        // Swap our slot into the middle as clean and take the newest frame. The
        // dirty bit only ever goes from clear to set between our load and this
        // exchange, so the slot we receive always holds a published frame.
        unsigned old = atomic_exchange_explicit(&m->middle, m->front, memory_order_acq_rel);
        m->front = old & NK_XR_MB_SLOT_MASK;
        m->have_front = 1;
    }
    if (!m->have_front) {
        return -1;
    }
    if (frame_id != NULL) {
        *frame_id = m->frame_id[m->front];
    }
    return (int)m->front;
}

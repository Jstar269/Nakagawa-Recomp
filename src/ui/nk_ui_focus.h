/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_UI_FOCUS_H
#define NK_UI_FOCUS_H

/* Focus graph for the nk_ui core.

   Pure C over rectangles: no Clay, no SDL, no allocation after
   nk_ui_focus_graph_create except when the entry lists grow. Each frame the
   screen registers its focusable elements (rect = last layout, in logical UI
   pixels), then asks the graph to move. The graph keeps the focused id and one
   remembered id per scope between frames. */

#include "nk_ui.h"

/* Entry flags. */
enum {
    NK_UI_FOCUS_DISABLED = 1u << 0,   /* registered (keeps its rect) but never focusable */
    NK_UI_FOCUS_ROW_MEMBER = 1u << 1, /* left/right stay in this entry's row: a move
                                         never jumps to another row, it just fails at the end */
    NK_UI_FOCUS_DEFAULT = 1u << 2,    /* preferred first focus when its scope activates
                                         with no remembered id */
    NK_UI_FOCUS_NO_DEFAULT = 1u << 3, /* never taken as a scope's first focus (a destructive
                                         action): reached only by moving to it */
    NK_UI_FOCUS_OWNS_HORIZONTAL = 1u << 4, /* left and right change this control (a segmented
                                              control, a slider): they never move focus off it */
    NK_UI_FOCUS_POINTER_ONLY = 1u << 5     /* a pointer target inside a control that takes the keys
                                              (one choice in a row of choices): a press focuses
                                              it, but no move, Tab or first focus lands on it */
};

/* Scopes stack from the bottom: root, overlay, drawer, dialog. The highest scope that
   has an enabled entry this frame is the active scope and traps focus: directional
   moves, Tab and pointer hits never reach a lower scope. */
enum {
    NK_UI_SCOPE_ROOT = 0,
    NK_UI_SCOPE_OVERLAY = 1,
    NK_UI_SCOPE_DRAWER = 2,
    NK_UI_SCOPE_DIALOG = 3,
    NK_UI_FOCUS_MAX_SCOPES = 8
};

typedef struct {
    NkUiId id;          /* nonzero, unique within a frame */
    NkUiRect rect;      /* as laid out, including the current scroll offset */
    uint32_t flags;     /* NK_UI_FOCUS_* */
    uint32_t scope;     /* NK_UI_SCOPE_*; entries at or above NK_UI_FOCUS_MAX_SCOPES are rejected */
    NkUiId scroller;    /* 0, or the id of the registered scroll container that clips this entry */
    NkUiId up;          /* 0, or the entry an up move from this one lands on, whatever is nearer:
                           the first row under a tab strip names the strip's active tab. */
    NkUiId down;        /* the same for a down move. Either is ignored, and the move is by
                           position, when that entry is missing, disabled, pointer-only or in
                           another scope. */
} NkUiFocusEntry;

/* A scroll container. viewport is its visible rect; scroll_y and content_h describe
   the current vertical scroll state (scroll_y is how far the content has moved up,
   0 at the top). Horizontal scrolling uses scroll_x and content_w; nk_ui registers
   vertical scrollers only today (scroll_x 0, content_w the viewport width), so a
   request's scroll_x stays 0 there and nk_ui applies scroll_y alone. parent is the scroll
   container this one sits in (a list inside a page that scrolls), 0 for none. */
typedef struct {
    NkUiId id;
    NkUiRect viewport;
    float scroll_x;
    float scroll_y;
    float content_w;
    float content_h;
    NkUiId parent;
} NkUiFocusScroller;

/* How deep scroll containers nest for bringing a control into view. */
#define NK_UI_FOCUS_SCROLL_DEPTH 4

/* A request, produced by a focus move, to set a container's scroll offsets so the
   newly focused entry is fully visible. The host applies it to its scroll state. */
typedef struct {
    NkUiId scroller;
    float scroll_x;
    float scroll_y;
} NkUiFocusScrollRequest;

typedef struct NkUiFocusGraph NkUiFocusGraph;

NkUiFocusGraph *nk_ui_focus_graph_create(void);
void nk_ui_focus_graph_destroy(NkUiFocusGraph *graph);
/* How far around an entry a scroll that brings it into view shows too (0 at creation;
   negative or not finite is taken as 0): the host's focus ring, drawn outside the entry's
   rect, is shown whole with it. A container scrolls no further than its content. */
void nk_ui_focus_set_reveal_margin(NkUiFocusGraph *graph, float margin);

/* Clears the registered entries and scrollers. Focus and scope memory persist. */
void nk_ui_focus_begin_frame(NkUiFocusGraph *graph);
/* Registers one entry. Declaration order is the Tab order. Returns false for a NULL
   argument, a zero or duplicate id, a non-finite or negative-size rect, a scope out
   of range, or out of memory. */
bool nk_ui_focus_register(NkUiFocusGraph *graph, const NkUiFocusEntry *entry);
bool nk_ui_focus_register_scroller(NkUiFocusGraph *graph, const NkUiFocusScroller *scroller);
/* After a layout: each entry and scroller takes the rect the layout gave it,
   and each scroller its scroll offset and content height, so moves made before
   the next frame see the screen as it was drawn. Ids the layout did not place
   keep what they were registered with. For the two layouts after focus changed, the
   focused entry is brought into view again where this layout put it: a page that
   describes its focused control changes shape with the focus, after the move was
   judged. */
void nk_ui_focus_place(NkUiFocusGraph *graph,
                       bool (*rect_of)(void *user, NkUiId id, NkUiRect *rect),
                       bool (*scroll_of)(void *user, NkUiId id, float *scroll_y,
                                         float *content_h),
                       void *user);
/* Resolves the active scope and repairs focus: remembers the focus of the scope that
   was active, enters the new one (its remembered id, else a DEFAULT entry, else its
   first enabled entry), and drops focus that no longer exists or became disabled. */
void nk_ui_focus_end_frame(NkUiFocusGraph *graph);
/* Drops the focus and what the active scope remembers, so the next
   nk_ui_focus_end_frame enters that scope afresh: its DEFAULT entry, else its
   first enabled entry. */
void nk_ui_focus_clear(NkUiFocusGraph *graph);

NkUiId nk_ui_focus_current(const NkUiFocusGraph *graph);
/* The active scope of the last completed frame, or -1 when nothing is focusable. */
int nk_ui_focus_active_scope(const NkUiFocusGraph *graph);
/* The id remembered for a scope (0 if none). */
NkUiId nk_ui_focus_remembered(const NkUiFocusGraph *graph, uint32_t scope);

/* Focuses id when it is registered, enabled and in the active scope. Does not scroll. */
bool nk_ui_focus_set(NkUiFocusGraph *graph, NkUiId id);
/* Directional move within the active scope. Returns the newly focused id, or 0 when
   no move happened (nothing in that direction, or nothing focused). An entry its scroll
   container clips away whole is reached only from inside that container, which a control
   in a container nested in it is too, or as a named neighbour. */
NkUiId nk_ui_focus_move(NkUiFocusGraph *graph, NkUiNavDir direction);
/* Tab / Shift+Tab: next or previous enabled entry of the active scope in
   declaration order, wrapping, passing over NK_UI_FOCUS_POINTER_ONLY entries. With
   nothing focused it picks the first (or last). */
NkUiId nk_ui_focus_cycle(NkUiFocusGraph *graph, bool reverse);

/* A scroll request produced by the last successful move, cycle or set, or by the layout
   after it (nk_ui_focus_place). Returns true once per request and clears it: one for each
   container that has to scroll to show the focused entry whole, the entry's own container
   first, then the containers around it. */
bool nk_ui_focus_take_scroll(NkUiFocusGraph *graph, NkUiFocusScrollRequest *request);
/* Computes the scroll request that brings id fully into its own container's viewport
   (minimal movement). Returns false when id has no scroller or is already visible. */
bool nk_ui_focus_scroll_into_view(const NkUiFocusGraph *graph, NkUiId id,
                                  NkUiFocusScrollRequest *request);

/* True when the focused entry is enabled and has NK_UI_FOCUS_OWNS_HORIZONTAL. */
bool nk_ui_focus_owns_horizontal(const NkUiFocusGraph *graph);

/* Whether a pointer may press or hover id: it is registered, enabled and in
   the active scope. Clay finds what is under the pointer; hover never changes
   focus. */
bool nk_ui_focus_accepts_pointer(const NkUiFocusGraph *graph, NkUiId id);

/* Stateless best-candidate pick over a plain entry list, for tests and one-off use:
   the best enabled entry from the scope of `current` in `direction`, or `current`
   when there is none. A NULL list or unknown `current` returns `current`. */
NkUiId nk_ui_focus_directional(const NkUiFocusEntry *entries, size_t count,
                               NkUiId current, NkUiNavDir direction);

#endif /* NK_UI_FOCUS_H */

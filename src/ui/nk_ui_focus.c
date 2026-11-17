/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "nk_ui_focus.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Direction scoring: score = main + 2 * cross, lower wins.
   Horizontal moves: main is the center distance along x; cross is 0 when the rows
   overlap and the center distance along y otherwise.
   Vertical moves: main is the center distance along y; cross is 0.3 * |dx| when the
   columns overlap and |dx| otherwise. Ties go to reading order (top, then left,
   then declaration order). An entry that names its up or down neighbour moves there
   instead, when that neighbour can take focus. An entry clipped away whole by its scroll
   container is a candidate only for moves from inside that container. */
#define NK_UI_FOCUS_EPS 0.01f
#define NK_UI_FOCUS_TIE 0.0001f
#define NK_UI_FOCUS_COLUMN_WEIGHT 0.3f
#define NK_UI_FOCUS_CROSS_WEIGHT 2.0f
/* Layouts after a focus change that bring the focused entry into view again: the one the
   change itself shapes, and the one after the scroll that answers it. */
#define NK_UI_FOCUS_REVEAL_FRAMES 2

struct NkUiFocusGraph {
    NkUiFocusEntry *entries;
    size_t count;
    size_t capacity;
    NkUiFocusScroller *scrollers;
    size_t scroller_count;
    size_t scroller_capacity;
    NkUiId focused;
    NkUiId memory[NK_UI_FOCUS_MAX_SCOPES];
    /* The scrolls that bring the focused entry into view, innermost container first. */
    size_t scroll_count;
    NkUiFocusScrollRequest scroll[NK_UI_FOCUS_SCROLL_DEPTH];
    /* Layouts still to come after a focus change on which the entry is brought into view
       again (nk_ui_focus_place). */
    int reveal_frames;
};

static size_t nk_ui_focus_reveal(const NkUiFocusGraph *graph, NkUiId id,
                                 NkUiFocusScrollRequest *out, size_t cap);

static bool nk_ui_focus_rect_valid(NkUiRect r) {
    return isfinite(r.x) && isfinite(r.y) && isfinite(r.w) && isfinite(r.h) &&
           r.w >= 0.0f && r.h >= 0.0f && isfinite(r.x + r.w) && isfinite(r.y + r.h);
}

static float nk_ui_focus_cx(NkUiRect r) { return r.x + r.w * 0.5f; }
static float nk_ui_focus_cy(NkUiRect r) { return r.y + r.h * 0.5f; }

static bool nk_ui_focus_overlap(float a0, float a1, float b0, float b1) {
    return a0 < b1 && b0 < a1;
}

static bool nk_ui_focus_enabled(const NkUiFocusEntry *e) {
    return (e->flags & NK_UI_FOCUS_DISABLED) == 0u;
}

/* Index of the entry with this id, or -1. */
static long nk_ui_focus_find(const NkUiFocusEntry *entries, size_t count, NkUiId id) {
    size_t i;
    if (id == 0u) {
        return -1;
    }
    for (i = 0; i < count; ++i) {
        if (entries[i].id == id) {
            return (long)i;
        }
    }
    return -1;
}

/* Highest scope that holds an enabled entry, or -1. */
static int nk_ui_focus_top_scope(const NkUiFocusEntry *entries, size_t count) {
    int top = -1;
    size_t i;
    for (i = 0; i < count; ++i) {
        if (nk_ui_focus_enabled(&entries[i]) && (int)entries[i].scope > top) {
            top = (int)entries[i].scope;
        }
    }
    return top;
}

static const NkUiFocusScroller *nk_ui_focus_scroller(const NkUiFocusScroller *scrollers,
                                                     size_t scroller_count, NkUiId id) {
    size_t i;
    for (i = 0; id != 0u && scrollers != NULL && i < scroller_count; ++i) {
        if (scrollers[i].id == id) {
            return &scrollers[i];
        }
    }
    return NULL;
}

/* Whether scroll container `inner` is `outer` or sits inside it. */
static bool nk_ui_focus_scroller_inside(const NkUiFocusScroller *scrollers,
                                        size_t scroller_count, NkUiId inner, NkUiId outer) {
    int depth;
    for (depth = 0; inner != 0u && depth < NK_UI_FOCUS_SCROLL_DEPTH; ++depth) {
        const NkUiFocusScroller *s;
        if (inner == outer) {
            return true;
        }
        s = nk_ui_focus_scroller(scrollers, scroller_count, inner);
        inner = s != NULL ? s->parent : 0u;
    }
    return false;
}

/* Whether a move from `from` may land on `cand`: an entry that its scroll container clips
   away whole is reached only from inside that container (its neighbours scroll it into
   view), never from a control outside it, which would jump to something not on screen. A
   control in a container nested in that one is inside it: the last row of a list in a page
   that scrolls reaches the button under the list, though the page has it out of view. */
static bool nk_ui_focus_on_screen_for(const NkUiFocusEntry *from, const NkUiFocusEntry *cand,
                                      const NkUiFocusScroller *scrollers,
                                      size_t scroller_count) {
    size_t i;
    if (cand->scroller == 0u || scrollers == NULL ||
        nk_ui_focus_scroller_inside(scrollers, scroller_count, from->scroller,
                                    cand->scroller)) {
        return true;
    }
    for (i = 0; i < scroller_count; ++i) {
        const NkUiFocusScroller *s = &scrollers[i];
        if (s->id == cand->scroller) {
            return nk_ui_focus_overlap(cand->rect.y, cand->rect.y + cand->rect.h, s->viewport.y,
                                       s->viewport.y + s->viewport.h) &&
                   nk_ui_focus_overlap(cand->rect.x, cand->rect.x + cand->rect.w, s->viewport.x,
                                       s->viewport.x + s->viewport.w);
        }
    }
    return true;
}

static long nk_ui_focus_pick(const NkUiFocusEntry *entries, size_t count,
                             size_t current, NkUiNavDir dir,
                             const NkUiFocusScroller *scrollers, size_t scroller_count) {
    const NkUiFocusEntry *from = &entries[current];
    float from_cx = nk_ui_focus_cx(from->rect);
    float from_cy = nk_ui_focus_cy(from->rect);
    bool horizontal = dir == NK_UI_DIR_LEFT || dir == NK_UI_DIR_RIGHT;
    long best = -1;
    float best_score = 0.0f;
    size_t i;
    if (dir != NK_UI_DIR_LEFT && dir != NK_UI_DIR_RIGHT &&
        dir != NK_UI_DIR_UP && dir != NK_UI_DIR_DOWN) {
        return -1;
    }
    if (horizontal && (from->flags & NK_UI_FOCUS_OWNS_HORIZONTAL) != 0u) {
        return -1;
    }
    /* A named neighbour wins over position (the tab strip's active tab, however far to the
       side it sits from a full-width row). */
    if (!horizontal) {
        NkUiId named = dir == NK_UI_DIR_UP ? from->up : from->down;
        long to = nk_ui_focus_find(entries, count, named);
        if (to >= 0 && (size_t)to != current && nk_ui_focus_enabled(&entries[to]) &&
            entries[to].scope == from->scope &&
            (entries[to].flags & NK_UI_FOCUS_POINTER_ONLY) == 0u) {
            return to;
        }
    }
    for (i = 0; i < count; ++i) {
        const NkUiFocusEntry *cand = &entries[i];
        float dx;
        float dy;
        float main_axis;
        float cross;
        float score;
        bool in_half_plane;
        if (i == current || !nk_ui_focus_enabled(cand) || cand->scope != from->scope ||
            (cand->flags & NK_UI_FOCUS_POINTER_ONLY) != 0u ||
            !nk_ui_focus_on_screen_for(from, cand, scrollers, scroller_count)) {
            continue;
        }
        dx = nk_ui_focus_cx(cand->rect) - from_cx;
        dy = nk_ui_focus_cy(cand->rect) - from_cy;
        switch (dir) {
        case NK_UI_DIR_RIGHT: in_half_plane = dx > NK_UI_FOCUS_EPS; break;
        case NK_UI_DIR_LEFT: in_half_plane = dx < -NK_UI_FOCUS_EPS; break;
        case NK_UI_DIR_DOWN: in_half_plane = dy > NK_UI_FOCUS_EPS; break;
        default: in_half_plane = dy < -NK_UI_FOCUS_EPS; break;
        }
        if (!in_half_plane) {
            continue;
        }
        if (horizontal) {
            bool same_row = nk_ui_focus_overlap(from->rect.y, from->rect.y + from->rect.h,
                                                cand->rect.y, cand->rect.y + cand->rect.h);
            if (!same_row && (from->flags & NK_UI_FOCUS_ROW_MEMBER) != 0u) {
                continue;
            }
            /* A control on another row that sits under or over the source is reached
               with up and down. Left and right must not pick it just because the
               centres differ by a few pixels, which a change of label width can do. */
            if (!same_row && nk_ui_focus_overlap(from->rect.x, from->rect.x + from->rect.w,
                                                 cand->rect.x, cand->rect.x + cand->rect.w)) {
                continue;
            }
            main_axis = fabsf(dx);
            cross = same_row ? 0.0f : fabsf(dy);
        } else {
            bool same_col = nk_ui_focus_overlap(from->rect.x, from->rect.x + from->rect.w,
                                                cand->rect.x, cand->rect.x + cand->rect.w);
            main_axis = fabsf(dy);
            cross = same_col ? NK_UI_FOCUS_COLUMN_WEIGHT * fabsf(dx) : fabsf(dx);
        }
        score = main_axis + NK_UI_FOCUS_CROSS_WEIGHT * cross;
        if (best < 0 || score < best_score - NK_UI_FOCUS_TIE) {
            best = (long)i;
            best_score = score;
        } else if (score <= best_score + NK_UI_FOCUS_TIE) {
            const NkUiFocusEntry *best_entry = &entries[best];
            float best_cy = nk_ui_focus_cy(best_entry->rect);
            float best_cx = nk_ui_focus_cx(best_entry->rect);
            float cand_cy = nk_ui_focus_cy(cand->rect);
            float cand_cx = nk_ui_focus_cx(cand->rect);
            /* A tie in score goes to the candidate that reads first: higher, then
               further left. */
            if (cand_cy < best_cy - NK_UI_FOCUS_TIE ||
                (fabsf(cand_cy - best_cy) <= NK_UI_FOCUS_TIE &&
                 cand_cx < best_cx - NK_UI_FOCUS_TIE)) {
                best = (long)i;
                best_score = score;
            }
        }
    }
    return best;
}

NkUiId nk_ui_focus_directional(const NkUiFocusEntry *entries, size_t count,
                               NkUiId current, NkUiNavDir direction) {
    long cur;
    long next;
    if (entries == NULL) {
        return current;
    }
    cur = nk_ui_focus_find(entries, count, current);
    if (cur < 0) {
        return current;
    }
    next = nk_ui_focus_pick(entries, count, (size_t)cur, direction, NULL, 0u);
    return next >= 0 ? entries[next].id : current;
}

NkUiFocusGraph *nk_ui_focus_graph_create(void) {
    return (NkUiFocusGraph *)calloc(1, sizeof(NkUiFocusGraph));
}

void nk_ui_focus_graph_destroy(NkUiFocusGraph *graph) {
    if (graph == NULL) {
        return;
    }
    free(graph->entries);
    free(graph->scrollers);
    free(graph);
}

void nk_ui_focus_begin_frame(NkUiFocusGraph *graph) {
    if (graph == NULL) {
        return;
    }
    graph->count = 0;
    graph->scroller_count = 0;
}

bool nk_ui_focus_register(NkUiFocusGraph *graph, const NkUiFocusEntry *entry) {
    if (graph == NULL || entry == NULL || entry->id == 0u ||
        entry->scope >= NK_UI_FOCUS_MAX_SCOPES || !nk_ui_focus_rect_valid(entry->rect) ||
        nk_ui_focus_find(graph->entries, graph->count, entry->id) >= 0) {
        return false;
    }
    if (graph->count == graph->capacity) {
        size_t capacity = graph->capacity != 0u ? graph->capacity * 2u : 32u;
        NkUiFocusEntry *grown;
        if (capacity < graph->capacity) {
            return false;
        }
        grown = (NkUiFocusEntry *)realloc(graph->entries, capacity * sizeof(*grown));
        if (grown == NULL) {
            return false;
        }
        graph->entries = grown;
        graph->capacity = capacity;
    }
    graph->entries[graph->count++] = *entry;
    return true;
}

bool nk_ui_focus_register_scroller(NkUiFocusGraph *graph, const NkUiFocusScroller *scroller) {
    size_t i;
    if (graph == NULL || scroller == NULL || scroller->id == 0u ||
        !nk_ui_focus_rect_valid(scroller->viewport) || !isfinite(scroller->scroll_x) ||
        !isfinite(scroller->scroll_y) || !isfinite(scroller->content_w) ||
        !isfinite(scroller->content_h)) {
        return false;
    }
    for (i = 0; i < graph->scroller_count; ++i) {
        if (graph->scrollers[i].id == scroller->id) {
            return false;
        }
    }
    if (graph->scroller_count == graph->scroller_capacity) {
        size_t capacity = graph->scroller_capacity != 0u ? graph->scroller_capacity * 2u : 8u;
        NkUiFocusScroller *grown = (NkUiFocusScroller *)realloc(
            graph->scrollers, capacity * sizeof(*grown));
        if (grown == NULL) {
            return false;
        }
        graph->scrollers = grown;
        graph->scroller_capacity = capacity;
    }
    graph->scrollers[graph->scroller_count++] = *scroller;
    return true;
}

/* Picks the focus for a scope with no valid focus: its remembered id, a DEFAULT
   entry, then its first enabled entry. Returns 0 when the scope is empty. */
static NkUiId nk_ui_focus_enter_scope(const NkUiFocusGraph *graph, int scope) {
    long found;
    size_t i;
    if (scope < 0) {
        return 0u;
    }
    found = nk_ui_focus_find(graph->entries, graph->count, graph->memory[scope]);
    if (found >= 0 && nk_ui_focus_enabled(&graph->entries[found]) &&
        (int)graph->entries[found].scope == scope &&
        (graph->entries[found].flags & NK_UI_FOCUS_POINTER_ONLY) == 0u) {
        return graph->entries[found].id;
    }
    for (i = 0; i < graph->count; ++i) {
        const NkUiFocusEntry *e = &graph->entries[i];
        if ((int)e->scope == scope && nk_ui_focus_enabled(e) &&
            (e->flags & NK_UI_FOCUS_DEFAULT) != 0u &&
            (e->flags & NK_UI_FOCUS_POINTER_ONLY) == 0u) {
            return e->id;
        }
    }
    for (i = 0; i < graph->count; ++i) {
        const NkUiFocusEntry *e = &graph->entries[i];
        if ((int)e->scope == scope && nk_ui_focus_enabled(e) &&
            (e->flags & (NK_UI_FOCUS_NO_DEFAULT | NK_UI_FOCUS_POINTER_ONLY)) == 0u) {
            return e->id;
        }
    }
    return 0u;
}

/* The scope's first enabled entry that keys can reach, a destructive one included. */
static NkUiId nk_ui_focus_first_enabled(const NkUiFocusGraph *graph, int scope) {
    size_t i;
    for (i = 0; i < graph->count; ++i) {
        const NkUiFocusEntry *e = &graph->entries[i];
        if ((int)e->scope == scope && nk_ui_focus_enabled(e) &&
            (e->flags & NK_UI_FOCUS_POINTER_ONLY) == 0u) {
            return e->id;
        }
    }
    return 0u;
}

void nk_ui_focus_place(NkUiFocusGraph *graph,
                       bool (*rect_of)(void *user, NkUiId id, NkUiRect *rect),
                       bool (*scroll_of)(void *user, NkUiId id, float *scroll_y,
                                         float *content_h),
                       void *user) {
    size_t i;
    if (graph == NULL || rect_of == NULL || scroll_of == NULL) {
        return;
    }
    for (i = 0; i < graph->count; ++i) {
        NkUiRect rect;
        if (rect_of(user, graph->entries[i].id, &rect) && nk_ui_focus_rect_valid(rect)) {
            graph->entries[i].rect = rect;
        }
    }
    for (i = 0; i < graph->scroller_count; ++i) {
        NkUiFocusScroller *s = &graph->scrollers[i];
        NkUiRect rect;
        float scroll_y;
        float content_h;
        if (rect_of(user, s->id, &rect) && nk_ui_focus_rect_valid(rect) &&
            scroll_of(user, s->id, &scroll_y, &content_h) && isfinite(scroll_y) &&
            isfinite(content_h)) {
            s->viewport = rect;
            s->content_w = rect.w;
            s->scroll_y = scroll_y;
            s->content_h = content_h;
        }
    }
    /* A move is judged on the layout before it, and the layout it leads to can differ: a
       panel that describes the focused control changes height with it and pushes the
       control down the page, part of it out of view. So the layouts just after a focus
       change bring the focused entry into view again, where they put it. */
    if (graph->reveal_frames > 0) {
        --graph->reveal_frames;
        graph->scroll_count = nk_ui_focus_reveal(graph, graph->focused, graph->scroll,
                                                 NK_UI_FOCUS_SCROLL_DEPTH);
    }
}

void nk_ui_focus_end_frame(NkUiFocusGraph *graph) {
    int top;
    long cur;
    int scope;
    if (graph == NULL) {
        return;
    }
    top = nk_ui_focus_top_scope(graph->entries, graph->count);
    /* A scope above the active one has closed, so it forgets where it was. */
    for (scope = top + 1; scope < NK_UI_FOCUS_MAX_SCOPES; ++scope) {
        graph->memory[scope] = 0u;
    }
    if (top < 0) {
        graph->focused = 0u;
        return;
    }
    cur = nk_ui_focus_find(graph->entries, graph->count, graph->focused);
    if (cur < 0 || !nk_ui_focus_enabled(&graph->entries[cur]) ||
        (int)graph->entries[cur].scope != top) {
        graph->focused = nk_ui_focus_enter_scope(graph, top);
    }
    graph->memory[top] = graph->focused;
}

NkUiId nk_ui_focus_current(const NkUiFocusGraph *graph) {
    return graph != NULL ? graph->focused : 0u;
}

int nk_ui_focus_active_scope(const NkUiFocusGraph *graph) {
    return graph != NULL ? nk_ui_focus_top_scope(graph->entries, graph->count) : -1;
}

NkUiId nk_ui_focus_remembered(const NkUiFocusGraph *graph, uint32_t scope) {
    if (graph == NULL || scope >= NK_UI_FOCUS_MAX_SCOPES) {
        return 0u;
    }
    return graph->memory[scope];
}

/* The scroll that shows `rect` whole in container s, or as much of it from its top left as
   fits (minimal movement). Returns false when s need not move. *rect becomes the part of
   it s shows after that scroll. */
static bool nk_ui_focus_scroll_for(const NkUiFocusScroller *s, NkUiRect *rect,
                                   NkUiFocusScrollRequest *request) {
    float nx = s->scroll_x;
    float ny = s->scroll_y;
    float max_x = s->content_w - s->viewport.w;
    float max_y = s->content_h - s->viewport.h;
    float x0;
    float y0;
    float x1;
    float y1;
    if (rect->y < s->viewport.y || rect->h > s->viewport.h) {
        ny += rect->y - s->viewport.y;
    } else if (rect->y + rect->h > s->viewport.y + s->viewport.h) {
        ny += (rect->y + rect->h) - (s->viewport.y + s->viewport.h);
    }
    if (rect->x < s->viewport.x || rect->w > s->viewport.w) {
        nx += rect->x - s->viewport.x;
    } else if (rect->x + rect->w > s->viewport.x + s->viewport.w) {
        nx += (rect->x + rect->w) - (s->viewport.x + s->viewport.w);
    }
    if (max_x < 0.0f) max_x = 0.0f;
    if (max_y < 0.0f) max_y = 0.0f;
    if (nx > max_x) nx = max_x;
    if (ny > max_y) ny = max_y;
    if (nx < 0.0f) nx = 0.0f;
    if (ny < 0.0f) ny = 0.0f;
    request->scroller = s->id;
    request->scroll_x = nx;
    request->scroll_y = ny;
    x0 = rect->x - (nx - s->scroll_x);
    y0 = rect->y - (ny - s->scroll_y);
    x1 = x0 + rect->w;
    y1 = y0 + rect->h;
    if (x0 < s->viewport.x) x0 = s->viewport.x;
    if (y0 < s->viewport.y) y0 = s->viewport.y;
    if (x1 > s->viewport.x + s->viewport.w) x1 = s->viewport.x + s->viewport.w;
    if (y1 > s->viewport.y + s->viewport.h) y1 = s->viewport.y + s->viewport.h;
    rect->x = x0;
    rect->y = y0;
    rect->w = x1 > x0 ? x1 - x0 : 0.0f;
    rect->h = y1 > y0 ? y1 - y0 : 0.0f;
    return nx != s->scroll_x || ny != s->scroll_y;
}

bool nk_ui_focus_scroll_into_view(const NkUiFocusGraph *graph, NkUiId id,
                                  NkUiFocusScrollRequest *request) {
    long idx;
    const NkUiFocusScroller *s;
    NkUiFocusScrollRequest found;
    NkUiRect rect;
    if (graph == NULL || request == NULL) {
        return false;
    }
    idx = nk_ui_focus_find(graph->entries, graph->count, id);
    if (idx < 0) {
        return false;
    }
    s = nk_ui_focus_scroller(graph->scrollers, graph->scroller_count,
                             graph->entries[idx].scroller);
    rect = graph->entries[idx].rect;
    if (s == NULL || !nk_ui_focus_scroll_for(s, &rect, &found)) {
        return false;
    }
    *request = found;
    return true;
}

/* The scrolls that bring entry id into view: its own container's, then the one of each
   container around that, which has to show what the container inside it shows of the
   entry. Returns how many were written to out. */
static size_t nk_ui_focus_reveal(const NkUiFocusGraph *graph, NkUiId id,
                                 NkUiFocusScrollRequest *out, size_t cap) {
    long idx = nk_ui_focus_find(graph->entries, graph->count, id);
    const NkUiFocusScroller *s;
    NkUiRect rect;
    size_t count = 0u;
    int depth;
    if (idx < 0) {
        return 0u;
    }
    rect = graph->entries[idx].rect;
    s = nk_ui_focus_scroller(graph->scrollers, graph->scroller_count,
                             graph->entries[idx].scroller);
    for (depth = 0; s != NULL && count < cap && depth < NK_UI_FOCUS_SCROLL_DEPTH; ++depth) {
        if (nk_ui_focus_scroll_for(s, &rect, &out[count])) {
            ++count;
        }
        if (rect.w <= 0.0f || rect.h <= 0.0f) {
            break;
        }
        s = nk_ui_focus_scroller(graph->scrollers, graph->scroller_count, s->parent);
    }
    return count;
}

static void nk_ui_focus_commit(NkUiFocusGraph *graph, NkUiId id, int scope) {
    graph->focused = id;
    if (scope >= 0 && scope < NK_UI_FOCUS_MAX_SCOPES) {
        graph->memory[scope] = id;
    }
    graph->scroll_count = nk_ui_focus_reveal(graph, id, graph->scroll,
                                             NK_UI_FOCUS_SCROLL_DEPTH);
    graph->reveal_frames = NK_UI_FOCUS_REVEAL_FRAMES;
}

bool nk_ui_focus_set(NkUiFocusGraph *graph, NkUiId id) {
    long idx;
    int top;
    if (graph == NULL) {
        return false;
    }
    idx = nk_ui_focus_find(graph->entries, graph->count, id);
    top = nk_ui_focus_top_scope(graph->entries, graph->count);
    if (idx < 0 || !nk_ui_focus_enabled(&graph->entries[idx]) ||
        (int)graph->entries[idx].scope != top) {
        return false;
    }
    nk_ui_focus_commit(graph, id, top);
    return true;
}

bool nk_ui_focus_owns_horizontal(const NkUiFocusGraph *graph) {
    long cur;
    if (graph == NULL) {
        return false;
    }
    cur = nk_ui_focus_find(graph->entries, graph->count, graph->focused);
    return cur >= 0 && nk_ui_focus_enabled(&graph->entries[cur]) &&
           (graph->entries[cur].flags & NK_UI_FOCUS_OWNS_HORIZONTAL) != 0u;
}

NkUiId nk_ui_focus_move(NkUiFocusGraph *graph, NkUiNavDir direction) {
    long cur;
    long next;
    int top;
    if (graph == NULL) {
        return 0u;
    }
    top = nk_ui_focus_top_scope(graph->entries, graph->count);
    cur = nk_ui_focus_find(graph->entries, graph->count, graph->focused);
    if (cur < 0 || (int)graph->entries[cur].scope != top ||
        !nk_ui_focus_enabled(&graph->entries[cur])) {
        /* Nothing usable has focus, as on a screen whose only control is
           destructive: a move lands on the scope's first focus. */
        NkUiId first = nk_ui_focus_enter_scope(graph, top);
        if (first == 0u) {
            first = nk_ui_focus_first_enabled(graph, top);
        }
        if (first == 0u) {
            return 0u;
        }
        nk_ui_focus_commit(graph, first, top);
        return graph->focused;
    }
    next = nk_ui_focus_pick(graph->entries, graph->count, (size_t)cur, direction,
                            graph->scrollers, graph->scroller_count);
    if (next < 0) {
        return 0u;
    }
    nk_ui_focus_commit(graph, graph->entries[next].id, top);
    return graph->focused;
}

NkUiId nk_ui_focus_cycle(NkUiFocusGraph *graph, bool reverse) {
    long cur;
    int top;
    size_t i;
    size_t n;
    size_t start;
    if (graph == NULL || graph->count == 0u) {
        return 0u;
    }
    top = nk_ui_focus_top_scope(graph->entries, graph->count);
    if (top < 0) {
        return 0u;
    }
    n = graph->count;
    cur = nk_ui_focus_find(graph->entries, graph->count, graph->focused);
    /* With no usable focus, start just before the first (or after the last) entry so
       the first step lands on the first (or last) enabled one. */
    if (cur < 0) {
        start = reverse ? 0u : n - 1u;
    } else {
        start = (size_t)cur;
    }
    for (i = 1; i <= n; ++i) {
        size_t idx = reverse ? (start + n - (i % n)) % n : (start + i) % n;
        const NkUiFocusEntry *e = &graph->entries[idx];
        if (nk_ui_focus_enabled(e) && (int)e->scope == top &&
            ((e->flags & NK_UI_FOCUS_POINTER_ONLY) == 0u || (cur >= 0 && idx == (size_t)cur))) {
            if (cur >= 0 && idx == (size_t)cur) {
                return 0u; /* the only focusable entry: nothing moves */
            }
            nk_ui_focus_commit(graph, e->id, top);
            return e->id;
        }
    }
    return 0u;
}

bool nk_ui_focus_take_scroll(NkUiFocusGraph *graph, NkUiFocusScrollRequest *request) {
    size_t i;
    if (graph == NULL || request == NULL || graph->scroll_count == 0u) {
        return false;
    }
    *request = graph->scroll[0];
    --graph->scroll_count;
    for (i = 0; i < graph->scroll_count; ++i) {
        graph->scroll[i] = graph->scroll[i + 1u];
    }
    return true;
}

bool nk_ui_focus_accepts_pointer(const NkUiFocusGraph *graph, NkUiId id) {
    long index;
    if (graph == NULL || id == 0u) {
        return false;
    }
    index = nk_ui_focus_find(graph->entries, graph->count, id);
    return index >= 0 && nk_ui_focus_enabled(&graph->entries[index]) &&
           (int)graph->entries[index].scope == nk_ui_focus_top_scope(graph->entries, graph->count);
}

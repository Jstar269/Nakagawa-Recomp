/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * glyph_recon.h - sharp game text: the CPU half of the Vulkan GE renderer's glyph reconstruction
 * (#2110). ge_gpu.c owns every Vulkan object; this module owns the setting, the refusals, the
 * rebuild of one glyph rectangle and the check that the rebuild agrees with the guest's own bytes.
 *
 * The problem: a game draws its text as small textured rectangles cut from a glyph page that holds
 * 1x anti-aliased coverage, one texel per screen pixel. Above render scale 1x every texel is
 * stretched over several device pixels, so text looks soft (bilinear) or blocky (nearest), and a
 * general-purpose scaler smears the cell's neighbours into it. The rebuild takes only the
 * rectangle a draw samples (its UV rectangle, in whole texels) and makes a version at the render
 * scale whose every f x f block still averages to the guest's coverage texel, with the coverage
 * pushed to the edge the neighbouring texels imply. It needs no font outline and no hook in the
 * game's code, so it works whichever code drew the page, a font the user supplied included.
 *
 * What it touches: only host copies. The input is a page already decoded on the host from main RAM
 * (never from VRAM, so no renderer readback); the output goes to a host-only image the GPU samples.
 * Guest memory, the CLUT and render targets are never read or written here.
 *
 * Off is authentic: the feature takes effect only in an Ace Mode run (g_ace_on, ace.h), whose
 * launch line the launcher always sets, so an authentic run can never turn it on.
 *
 * Setting: the launch line's glyphs key (ace.h; glyphs=on, a missing key or glyphs=off is off), which
 * the renderer hands to nk_glyph_init once when it starts. ace.c parses the key; this unit reads no
 * environment.
 *
 * The feature is guest-visible (docs/ENHANCEMENT_CONTRACT.md, the section 11.3 entry "Sharp Game
 * Text"): each present commits the frame, rebuilt glyphs included, to guest VRAM. It is not admitted
 * under section 11.2: it has a launch-line key but no library or per-title switch yet, so a run that
 * turns it on says so at start.
 */
#ifndef NK_GLYPH_RECON_H
#define NK_GLYPH_RECON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A rectangle is rebuilt only when each side lies in this range of texels: smaller is not text, and
 * larger is a picture. The rebuilt tile is at most 64 * 6 = 384 texels on a side. */
#define NK_GLYPH_MIN_SIDE 4
#define NK_GLYPH_MAX_SIDE 64
/* The factors a rebuild takes: the render scale, which the draw's device pixels line up with. The Vulkan
 * renderer runs at 1 to 4 (MAX_SCALE in ge_gpu.c, asserted there), so a run uses 2 to 4; the rebuild itself is
 * tested up to 6. */
#define NK_GLYPH_MIN_FACTOR 2
#define NK_GLYPH_MAX_FACTOR 6
/* How much of a block is rebuilt as an edge, decided by the largest difference between two neighbouring texels in
 * the 3 x 3 around it (coverage, 0 to 1). Below NK_GLYPH_EDGE_LOW the block is the smooth bicubic field shifted to
 * keep the texel's coverage; above NK_GLYPH_EDGE_HIGH it is a one-pixel ramp fitted to that coverage; between them
 * the two are mixed. The basis: an edge that falls inside one texel leaves a step of at least 0.5 to one side (a
 * texel of coverage c next to 0 and next to 1 has steps c and 1 - c), so 0.55 and above is an edge. A gradient
 * whose steps stay under 0.35 is at least three texels wide: a one-pixel ramp over it would start over at every
 * block (a ribbed gradient). The two values are a choice by those cases, not a tuned optimum. */
#define NK_GLYPH_EDGE_LOW 0.35f
#define NK_GLYPH_EDGE_HIGH 0.55f
/* Soft art or text, decided for the whole rectangle: its partly covered texels (between NK_GLYPH_PARTIAL_LOW and
 * NK_GLYPH_PARTIAL_HIGH of its highest alpha, in 1/256) are counted and divided by its total variation of
 * coverage (the sum of the steps between neighbouring texels, in steps from blank to that alpha; a hard step with
 * no partly covered texel beside it counts nothing). An edge box filtered by the texel grid that reaches
 * solid ink crosses as many texels as it changes steps, at any angle (ratio 1), and a ramp w texels wide crosses w
 * times as many; a stroke about a texel wide that never reaches solid has a ratio of about 1 over its peak. Above
 * NK_GLYPH_SOFT_RATIO the rectangle is rebuilt smooth throughout, which does not rib on a source that does not run
 * back: a ramp fitted to every block of a soft region starts over at each block, and a texel's own neighbours cannot
 * tell a soft ramp from a slanted edge (both have two partly covered texels side by side). The ratio is a choice and
 * the populations overlap: box-filtered text measured 0.54 to 1.44 (small and filtered text more), the soft art to
 * catch starts at 1.23 and a glow with a 1.25-texel falloff is 1.18. Some text goes smooth and some soft art ribs
 * (section 10.13 of the enhancement contract gives the figures). */
#define NK_GLYPH_PARTIAL_LOW 16
#define NK_GLYPH_PARTIAL_HIGH 240
#define NK_GLYPH_SOFT_RATIO 1.2f
/* Rectangles checked or rebuilt per present, a fixed count, never a time budget: a run is the same
 * whatever the host's speed. A glyph past the count is drawn as the game made it for that frame and is
 * offered again by the next draw. */
#define NK_GLYPH_PER_PRESENT 48
/* The downsample check: each texel of the 1x rectangle must equal the mean of its f x f block of
 * the rebuilt tile within this many 8-bit steps. Quantizing the tile costs at most half a step. */
#define NK_GLYPH_CHECK_TOLERANCE 2
/* A rectangle looks like anti-aliased text when at least this many of its inked texels, and at
 * least NK_GLYPH_MIN_PARTIAL texels, are partly covered (alpha neither 0 nor 255): a hard-edged
 * bitmap font has none, and a rebuild would only blur it. The fraction is 1/8. */
#define NK_GLYPH_MIN_PARTIAL 2
#define NK_GLYPH_PARTIAL_DIV 8

/* ---- the setting ---------------------------------------------------------------------------- */

typedef struct NkGlyphConfig {
    int on;   /* 0 off, 1 on */
} NkGlyphConfig;

/* Take the launch line's glyphs key (`requested`, nonzero for glyphs=on) for an Ace run (`enhanced`
 * nonzero), print its start lines when it is on or asked for in an authentic run, and make it the
 * run's configuration. Returns the configuration in force: off whenever `enhanced` is 0. */
const NkGlyphConfig *nk_glyph_init(int enhanced, int requested);
/* The line naming a guest-visible setting that is on outside section 11.2 admission. Writes it to
 * `out` (without a newline) and returns its length when on; returns 0 and an empty string when
 * off. */
size_t nk_glyph_admission_line(const NkGlyphConfig *cfg, char *out, size_t cap);
/* The lines the last nk_glyph_init printed, each ending in a newline ("" for none), for
 * the self-test. */
const char *nk_glyph_start_lines(void);
/* Tests: set the configuration directly (no launch line, no start line). */
void nk_glyph_configure(const NkGlyphConfig *cfg);
const NkGlyphConfig *nk_glyph_config(void);

/* ---- what a draw's source may be ------------------------------------------------------------- */

/* The storage the page was decoded from. Only a GE texture format with an alpha channel (or a CLUT
 * that gives one) can hold anti-aliased coverage; the last three name buffers a game may keep
 * (sceFont's reversed 4-bit and 24-bit bitmaps, a page in VRAM) that no rebuild reads. */
typedef enum NkGlyphFormat {
    NK_GLYPH_FMT_4444 = 0,
    NK_GLYPH_FMT_8888,
    NK_GLYPH_FMT_T4,           /* 4-bit indexed, through a CLUT */
    NK_GLYPH_FMT_T8,           /* 8-bit indexed, through a CLUT */
    NK_GLYPH_FMT_5650,         /* no alpha channel */
    NK_GLYPH_FMT_5551,         /* one-bit alpha: not anti-aliased */
    NK_GLYPH_FMT_T16,
    NK_GLYPH_FMT_T32,
    NK_GLYPH_FMT_DXT,          /* block-compressed: lossy, never text */
    NK_GLYPH_FMT_T4_REVERSED,  /* a 4-bit buffer with the nibbles swapped; no GE format reads it */
    NK_GLYPH_FMT_RGB24,        /* a 24-bit buffer; no GE format reads it */
    NK_GLYPH_FMT_COUNT
} NkGlyphFormat;

typedef struct NkGlyphSource {
    NkGlyphFormat format;
    int in_vram;        /* the page lies in VRAM: reading it back would be a renderer readback */
    int mip_levels;     /* levels the draw samples: 1 for a plain page */
    int render_scale;   /* the renderer's scale; the rebuild's factor */
} NkGlyphSource;

typedef enum NkGlyphVerdict {
    NK_GLYPH_OK = 0,
    NK_GLYPH_REFUSE_VRAM,         /* page in VRAM: main RAM only */
    NK_GLYPH_REFUSE_FORMAT,       /* no usable alpha, lossy, or no GE format */
    NK_GLYPH_REFUSE_REVERSED,     /* 4-bit reversed buffer */
    NK_GLYPH_REFUSE_RGB24,        /* 24-bit buffer */
    NK_GLYPH_REFUSE_MIP,          /* a mip chain: a level is not a glyph page */
    NK_GLYPH_REFUSE_SCALE,        /* render scale 1 or beyond the factors: nothing to sharpen */
    NK_GLYPH_REFUSE_SIZE,         /* the rectangle is not a glyph's size, or leaves the page */
    NK_GLYPH_REFUSE_EMPTY,        /* nothing inked */
    NK_GLYPH_REFUSE_COLOUR,       /* inked texels differ in colour: art, not coverage */
    NK_GLYPH_REFUSE_HARD_EDGE,    /* no anti-aliasing: a bitmap font */
    NK_GLYPH_REFUSE_DOWNSAMPLE,   /* the rebuild disagrees with the guest's bytes */
    NK_GLYPH_REFUSE_MEMORY,
    NK_GLYPH_VERDICT_COUNT
} NkGlyphVerdict;

const char *nk_glyph_verdict_name(NkGlyphVerdict v);

/* Whether a draw from this source may be rebuilt at all: NK_GLYPH_OK, or the first refusal, in the
 * order VRAM, reversed, 24-bit, format, mip, scale. */
NkGlyphVerdict nk_glyph_source_check(const NkGlyphSource *src);

/* ---- one rectangle ---------------------------------------------------------------------------- */

/* `page` is a decoded texture, `pw` x `ph` texels of R in the low byte, then G, B, A (as ge.c's
 * decoder writes). The rectangle is [x0, x0 + w) x [y0, y0 + h) in whole texels. Classifies it:
 * NK_GLYPH_OK (and the shared colour in `*colour`, the low 24 bits of a texel) when it looks like
 * anti-aliased text, else the first refusal in the order SIZE, EMPTY, COLOUR, HARD_EDGE. */
NkGlyphVerdict nk_glyph_rect_check(const uint32_t *page, int pw, int ph, int x0, int y0, int w, int h,
                                   uint32_t *colour);

/* The downsample check: `tile` is (w * factor) x (h * factor) texels of the same layout. OK when
 * every texel of the rectangle equals the mean of its factor x factor block of the tile's alpha
 * within NK_GLYPH_CHECK_TOLERANCE, and every tile texel with any alpha has the rectangle's colour.
 * NK_GLYPH_REFUSE_DOWNSAMPLE otherwise. */
NkGlyphVerdict nk_glyph_verify(const uint32_t *page, int pw, int ph, int x0, int y0, int w, int h,
                               int factor, const uint32_t *tile);

/* Rebuild the rectangle at `factor` into `tile` (w * factor x h * factor texels): classify it, build
 * it, check it. Returns NK_GLYPH_OK with `tile` filled, or the refusal with `tile` untouched. */
NkGlyphVerdict nk_glyph_rebuild(const uint32_t *page, int pw, int ph, int x0, int y0, int w, int h,
                                int factor, uint32_t *tile);

#ifdef __cplusplus
}
#endif

#endif

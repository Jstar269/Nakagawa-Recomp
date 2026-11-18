# PROVENANCE.md — `third_party/clay/`

Single-header Clay layout library vendored for the native player's UI. The
launcher, the in-game overlay and their tests lay out through it:
`src/ui/nk_ui_clay.c` is the one source that compiles the implementation, and
`tests/native/test_clay_layout.c` checks the header on its own.

## Upstream source

- Project: Clay
- Repository: `https://github.com/nicbarker/clay`
- Pin: commit `e6cc36941ab2af5d81107617039d6f527a1c660b` (2026-05-20), the
  head of the default branch `main` when it was imported on 2026-10-29. This is
  a branch-head pin by owner decision: no release has been tagged since `v0.14`
  (commit `b25a31c1a152915cd7dd6796e6592273e5a10aac`, the previous pin), and
  `main` is 99 commits ahead of it.
- License: `Zlib`. Upstream's `LICENSE.md` calls it the "zlib/libpng license";
  the complete upstream license text is kept beside the header.
- Import method: `clay.h` and `LICENSE.md` were downloaded from the pinned
  commit. Their Git blob IDs match the corresponding files in that commit, and
  the SHA-256 values below were calculated from the local bytes.

## File manifest

| File | Kind | Upstream blob SHA at the pin | SHA-256 |
| --- | --- | --- | --- |
| `clay.h` | imported + LOCAL DELTA | `7d9cf4cc7a918d1bdd766867c180851c949a3310` | `35bb5743f123cf323086487e7f16da75e6f5c2aa400bc623c5e5f50494d2483e` |
| `LICENSE.md` | imported byte-identical | `2968c61bcdc6ecbef470e02edd45b47aba1d8c44` | `5dd30f9f52093076b7574458f5efb8242fe6cbbd4669d4a2914e7616810d0df8` |
| `PROVENANCE.md` | project-authored manifest | n/a | n/a |

No generated Clay file is included. The upstream `clay.h` bytes have SHA-256
`54aef6d5f96b14e296bebb6db4a755543e98fd7e6c188411afb2f22a4b4f7728`;
applying the changes below to them gives the SHA-256 in the table.

## Local deltas

Each change is marked `LOCAL DELTA` in the file, and
`tests/native/test_clay_layout.c` fails on the upstream bytes for each of them.
The file's second and third lines say it was modified and point here. The first
four changes were made on 2026-10-29, the rest on 2026-11-17; the first was
also amended on 2026-11-17.

- `clay.h`, `Clay__Array_Allocate_Arena`: `capacity * itemSize` is computed as
  `(size_t)capacity * itemSize`, so the product is formed in `size_t` instead of
  32 bits before the arena bounds check (CodeQL "multiplication result
  converted to larger type"). Every in-range result is unchanged. Upstream at
  the pin still multiplies in 32 bits, so a request of 65536 items of 65536
  bytes wraps to 0 and is granted; the selftest checks that it is refused.
  Since 2026-11-17 a negative capacity, or a product that does not fit
  `size_t`, is refused before the multiplication, and the bounds check compares
  the size with the room left after the aligned offset, so neither the product
  nor the sum can wrap on any `size_t` width. The 2026-10-29 form let a
  capacity of -1 become a size just under `SIZE_MAX` whose sum with the offset
  wrapped below the capacity: the request got memory already handed out and
  moved the arena's next allocation backwards. The selftest checks that -1
  items are refused and the arena is left as it was.
- `clay.h`, `Clay__CalculateFinalLayout`: a wrapped text element's minimum height
  is raised to its wrapped height, and the pass that carries wrapped heights up
  the tree also carries the minimum heights, with the rule an element's close
  uses (a sum along a column, the tallest child across a row, nothing for a
  vertical clip container). Upstream at the pin still takes the minimum before
  wrapping, as one line, so compressing an overfull parent squeezes wrapped text
  back toward one line and the next element paints over its later lines; the
  selftest checks the overfull case.
- `clay.h`, `Clay__AddHashMapItem`: the element-id map is full only when a new
  slot is needed and its free list is empty. Upstream at the pin checks the
  slot array's length first, before a repeated id or a freed slot can be used,
  and that array never shrinks: after one layout passes the element limit,
  every later layout fails with `HASH_MAP_CAPACITY_EXCEEDED`, though pruning
  has freed the slots of the elements that are gone. The selftest lays out past
  the limit, then checks that the next layout succeeds without an error.
- `clay.h`, `Clay_UpdateScrollContainers`: a released drag keeps momentum only
  when its frames passed time. Momentum is the drag's distance over its time,
  and upstream at the pin divides by that time even when it is zero (a host
  whose clock stood still across the drag's frames): the infinite momentum pins
  the container at one end until the next scroll. The selftest drags with a
  `deltaTime` of 0 and checks the container stays where the drag left it.
- `clay.h`, `Clay__IntToString`: the digits of a number the debug view prints
  go to `dynamicStringData`, one char per element, at the end of the arena.
  Upstream writes them without checking the space left, so a debug view that
  lists more ids than fit (about 800 ids of 10 digits) writes past the arena.
  The number now shows as `?` when its 11 chars do not fit, and
  `ARENA_CAPACITY_EXCEEDED` is reported once a layout. The selftest lists 1000
  ids in an arena with guard bytes behind it and checks the guard is intact.
- `clay.h`, `Clay__CalculateFinalLayout`: each pass starts the wrapped-line pool
  afresh, as it wraps every text again, and a line counts in its text's slice
  only when the pool took it. Upstream never resets the pool between the two
  passes of a layout with a transition, so the second pass gets half of it,
  and a full pool still lengthens the slice, which then reads past the pool.
  The selftest lays out 5000 one-line texts in a container with a transition.
- `clay.h`, `Clay__CalculateFinalLayout`: a newline right after a word too wide
  for its line ends that word's line. Upstream adds another line for it: an
  empty one, or one of length -1 when the long word ends in a space, since the
  trailing-space trim then reads the char before an empty line. The trim is now
  made only on a line with chars. The selftest wraps `"abcdef \nnext"` and
  `"abcdefg\nnext"` in a 60 px paragraph and checks for two lines.
- `clay.h`, `Clay_UpdateScrollContainers`: removing a scroll container that is
  gone swaps the last one into its slot, and the loop now visits that slot
  again. Upstream moves on, so the container swapped in misses that update: its
  momentum, its open-this-frame reset and the wheel or drag over it. The
  selftest drops the first of two scroll containers and checks the second takes
  the wheel in that update.
- `clay.h`, `Clay__MeasureTextCached`: the word cache has room for the words on
  its free list as well as its unused slots, and a check before each word keeps
  a newline's two words and the last word inside the array. Upstream checks the
  slot array's length alone, and that array never shrinks: once it has peaked,
  every new text fails with `TEXT_MEASUREMENT_CAPACITY_EXCEEDED`, though
  eviction has freed the words of the texts that are gone. A text that does not
  fit now gives back the words it took and its cache item; upstream leaks both.
  The selftest fills the cache to its last slot, has that text evicted, and
  measures a text in its bucket; it also measures a text after one with more
  words than the cache holds.
- `clay.h`, `Clay__MeasureTextCached` and `Clay__HashStringContentsWithConfig`:
  a cache entry is a text's only when its whole key matches: the text's length,
  its address if it is static or else its 64-bit content hash, and the font,
  size and letter spacing. Upstream matches the 32-bit id alone, so two texts
  whose ids collide share one entry: the second gets the first one's widths,
  and its wrapped lines are cut at the first one's word offsets, which can run
  past its end. The selftest finds two texts of different lengths with the same
  id by a birthday search and checks each is measured as itself.
- `clay.h`, `Clay__MeasureTextCached`: a text's measure-cache bucket is its id
  modulo the bucket array's capacity, as `Clay__AddHashMapItem` takes it.
  Upstream at the pin takes it modulo the word-cache size divided by 32, which
  has no tie to that array, sized by the element count: a word cache under 32
  words divides by zero on the first text measured, and one over 32 times the
  element count indexes past the array. The selftest measures a text with a
  16-word cache.

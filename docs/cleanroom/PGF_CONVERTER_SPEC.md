# Project-authored behaviour specification - deterministic OpenType/TTF to PGF converter

**Status:** specification with implementation. The converter ships as
`tools/ttf2pgf.py` (issue #313), writes through the deterministic writer
`tools/pgf_writer.py`, and is verified by `tools/test_ttf2pgf.py` against the
public reader `src/rt/pgf_public.c`. This document is also the converter's
independence and provenance record; per
[`../provenance/INDEPENDENCE_MODEL.md`](../provenance/INDEPENDENCE_MODEL.md)
the defensible claim is that this unit's behaviour is independently established
and its expression is project-authored - not the term "clean room", which the
model reserves for a process this project did not follow.

Written under the campaign protocol (`../INDEPENDENCE_CAMPAIGN.md`) rules for
spec-first work: from the public OpenType format documentation, the
project-authored reader specification `PGF_SPEC.md`, the route decision in
[`../provenance/FONT_ORIGINS.md`](../provenance/FONT_ORIGINS.md), and
project-owned measurements. Every behavioural rule below is either cited to
those sources or marked as a project decision with its rationale.

## 1. Role and scope

The converter turns one pinned TrueType-outline font plus a metric-target
policy into a public-safe PGF fixture:

- **In scope.** sfnt parsing of static `glyf` fonts; cmap lookup for a
  contiguous 16-bit code range, or an explicit ascending code-point set that
  yields a sparse character map; unhinted rasterization to 4-bit alpha;
  26.6 metric derivation; honest output naming; a machine-readable conversion
  manifest; refusal of every malformed, unsupported, or policy-violating
  input named in section 3.
- **Out of scope.** CFF/OTF and variable fonts; hint execution; pair kerning
  (PGF carries per-glyph advances only - `PGF_SPEC.md` section 3.10); text
  shaping; authentic firmware-font provisioning (route #300, which remains
  preferable when fidelity to the PSP system fonts is required); claiming any
  pixel identity with a firmware font (non-goal of issue #313).
- **Japanese project font.** The project pins Noto Sans JP Regular (the UI
  font's static instance) and M PLUS 1p Regular, both under SIL OFL 1.1, and
  converts the `--japanese-text` repertoire described below from Noto Sans JP,
  with M PLUS 1p drawing through `--fallback` the 341 points of that set Noto
  Sans JP lacks (#2005, #1300). The converter is
  codepoint-driven; any additional font needs its own pin (a pin file, or
  for a font the project already ships its record in the static font list,
  section 7) and coverage set. The output is not claimed to be pixel-identical to a firmware
  font (`FONT_ORIGINS.md` records the source and conversion).

## 2. Input contract

### 2.1 sfnt container

The input must be a TrueType sfnt: scaler type `0x00010000` or Apple `true`.
`OTTO` and any other scaler are refused by name. The directory must hold
1..512 unique ASCII tags whose byte ranges lie inside the file. Required
tables: `cmap`, `glyf`, `head`, `hhea`, `hmtx`, `loca`, `maxp`. Table
checksums are **not** verified: integrity of a pinned input is established by
its SHA-256 in the pin, and `head.checkSumAdjustment` bookkeeping varies
across legitimate toolchains (measured: the pinned fixture's own `head`
checksum does not match its recomputed value). A malformed `head`, `maxp`,
`hhea`, `hmtx`, or `loca` is refused with a section-named reason.

### 2.2 Character map

Subtables are considered in a fixed preference order - Unicode platform
records first, then Microsoft full-repertoire, Microsoft BMP, Microsoft
symbol, then legacy records - and the first subtable in a supported format
(0, 4, 6, or 12) parses successfully wins. Ties keep directory order. A
mapping to glyph 0 (`.notdef`) counts as absent. Every code in the requested
range or set must resolve, or the conversion is refused with
`missing-code-point` naming the first gap in ascending order.

### 2.3 Outlines

Simple glyphs are read as documented: contour ends, hint length (skipped,
never executed), repeat-coded flags, delta-encoded coordinates. Composite
glyphs support XY component offsets with identity, uniform, or axis-aligned
scale transforms (including the scaled-offset flag). Point-matching
arguments, 2x2 transforms, nesting beyond 8 levels, and expansions beyond 64
components are refused by name; component hint programs are skipped. All
component coordinates are composed exactly with rational arithmetic before
scaling.

### 2.4 Reason catalogue

| Reason | Meaning |
| --- | --- |
| `unsupported-sfnt-version` | Scaler type is not a TrueType sfnt. |
| `unsupported-outline-format` | `OTTO`, `"CFF "`, or `CFF2`: PostScript outlines. |
| `unsupported-variable-font` | `fvar`/`gvar`/`cvar`/`avar`/`HVAR`/`MVAR`/`VVAR` present. A `STAT` table alone is accepted: static family members carry it too. |
| `unsupported-loca-format` | `indexToLocFormat` is neither 0 nor 1. |
| `unsupported-cmap-format` | No usable Unicode subtable in formats 0/4/6/12. |
| `unsupported-composite-point-args` | Composite matches points instead of XY offsets. |
| `unsupported-composite-transform` | Composite uses a 2x2 transform. |
| `missing-table` | A required table is absent. |
| `truncated-table` | A directory entry claims bytes past end of file. |
| `malformed-font` / `malformed-head` / `malformed-maxp` / `malformed-hhea` / `malformed-hmtx` / `malformed-loca` / `malformed-cmap` / `malformed-glyph` | Bounds or invariant violation in that section. |
| `missing-code-point` | A requested code has no glyph (in either font, with `--fallback`). |
| `bad-code-range` | `--codes` is unparseable, inverted, or outside 16 bits. |
| `bad-codepoint-list` | A `--codepoints` file is unreadable, not ASCII, holds a line that is not a code point, or leaves 16 bits. |
| `codepoints-unsorted` | A `--codepoints` file repeats a code or is not in ascending order. |
| `empty-codepoint-set` | A `--codepoints` file lists no code point. |
| `japanese-codec-unavailable` / `japanese-mapping-invalid` | A built-in ASCII/EUC-JP/CP932/Shift_JIS-2004 codec or its pinned Japanese mapping is unavailable or has changed. |
| `latin-mapping-invalid` / `latin-codec-unavailable` | A built-in Latin mapping has changed from its pinned digest or count, a code page codec is unavailable, or the face does not draw all of printable ASCII for `--latin-cyrillic-text`. |
| `ppem-out-of-range` | Size outside 1..128. |
| `embolden-out-of-range` | `--embolden-x`, `--embolden-y`, or `--embolden-advance` outside 0..8 sub-samples. |
| `baseline-ellipsis-unavailable` | `--baseline-ellipsis` without U+2026 in the code set, without an outline for U+2026 or U+002E, or with U+2026 drawn by the `--fallback` font. |
| `tracking-out-of-range` | `--tracking` outside -8..16 sub-samples. |
| `digit-advance-out-of-range` / `digit-advance-unavailable` | `--digit-advance` outside 64..`MAX_EDGE * 64` (26.6), or without all of the digits 0-9 in the code set. |
| `fallback-unused` | `--fallback` given, but the first font draws every requested code. |
| `snap-overshoot-out-of-range` | `--snap-overshoot` outside 0..8 sub-samples. |
| `snap-overshoot-unavailable` | `--snap-overshoot` with a drawing font that lacks an outline for `H` or `x`, or whose flat edges do not ascend (section 3, rule 13). |
| `snap-overshoot-zones-overlap` | At the chosen ppem the band above the x-height reaches the cap height (section 3, rule 13). |
| `scale-x-out-of-range` | `--scale-x` outside 800..1250 thousandths (section 3, rule 14). |
| `vertical-metric-out-of-range` | `--vertical-advance` or `--vertical-origin` outside 1..2000 thousandths of the em (section 3, rule 16). |
| `glyph-too-large` | A raster exceeds the writer's 7-bit record fields (127 px). |
| `composite-too-deep` / `composite-too-many-components` | Composite expansion bounds. |
| `metric-targets-invalid` / `metric-target-unreachable` | Policy document is malformed / no size satisfies it. |
| `conflicting-parameters` | `--ppem` together with `--metric-targets`; `--fallback-pin` without `--fallback`; or only one of `--pin` and `--fallback-pin` when a fallback is given. (`--codes`, `--codepoints`, `--jis-x-0208-japanese`, `--japanese-text`, `--latin-1-text`, `--latin-cyrillic-text`, and `--ks-x-1001-korean` are mutually exclusive at the CLI.) |
| `pin-invalid` / `pin-digest-mismatch` / `pin-size-mismatch` / `pin-license-mismatch` | Source pin does not bind the exact input and licence bytes. |
| `reserved-font-name` / `camouflage-font-name` | Output name policy violation (section 6). |
| writer reasons (`font-field-invalid`, `nominal-size-out-of-range`, `metric-table-overflow`, ...) | Re-raised unchanged from `pgf_writer.py`. |

`--codepoints FILE` supplies the code points instead of `--codes`. The file lists
one code point per line as `0x41`, `U+0041`, or decimal; `#` starts a comment.
Codes must be strictly ascending, so a set has exactly one reading, and every
listed code needs a glyph (`missing-code-point` otherwise). The file's SHA-256
and leaf name go into the manifest. The writer then emits a sparse character map
over the span from the smallest to the largest code, and codes outside the set
are misses.

`--jis-x-0208-japanese` uses Python's built-in codecs for JIS X 0201 Roman
U+0020-U+007E (95 code points) and half-width katakana U+FF61-U+FF9F (63),
plus the EUC-JP mapping for assigned JIS X 0208 cells in rows 1..8 and 16..47:
524 non-kanji code points and all 2,965 level-1 kanji, respectively. The
sorted set contains 3,647 unique Unicode points and has the pinned SHA-256
`abc5302681a3c700179f29ac22a7c8082ed087a07b4cf8c3de4de5f73e7e0889` in the
converter. An unavailable or changed built-in mapping is refused by name. It
is a sparse map just like `--codepoints`, and its built-in mapping id and
digest are recorded in the manifest.

`--latin-1-text` combines ASCII U+0020-U+007E (95 code points), Latin-1
Supplement U+00A0-U+00FF (96), Latin Extended-A U+0100-U+017F (128), and the
assigned punctuation and symbols U+2010-U+2027, U+2030, U+2039-U+203A, U+20AC,
and U+2122 (29). The set contains 348 unique points with canonical SHA-256
`7fbcaac92248acdb1c2704c3fef2181e7bc49f2b0e9d0f970cd029fb58057340`.

`--latin-cyrillic-text` (#2005, #1300) is a rule over public standards, applied
to the face being converted: every code point of the Unicode blocks and the
single-byte code pages below that the face's own character map draws. Code
pages are read through Python's standard codecs, bytes 0x20-0xFF, without the
C1 controls and the Private Use Area. Invisible format controls (Unicode general
category Cf, such as the bidirectional marks of General Punctuation) and the
line and paragraph separators U+2028 and U+2029 (categories Zl and Zp, which a
face such as Geist maps but which end a line rather than draw) are left out,
except the soft hyphen U+00AD that Latin-1 text writes. A face that does
not draw all of printable ASCII is refused (`latin-mapping-invalid`). No list
of characters is taken from a console font. The blocks, each with its reason:

| Block | Why it is in |
| --- | --- |
| Basic Latin U+0020-007E | ASCII: every Latin-script text. |
| Latin-1 Supplement U+00A0-00FF | The letters and signs of Western European text. |
| Latin Extended-A U+0100-017F | The letters of Central and Eastern European, Baltic, Turkish and Esperanto text. |
| Spacing Modifier Letters U+02B0-02FF | The accents text writes on their own (`ˆ ˇ ˘ ˙ ˚ ˛ ˜ ˝`). |
| General Punctuation U+2000-206F | Dashes, quotation marks, the ellipsis, daggers, the bullet and the per mille sign. |
| Letterlike Symbols U+2100-214F | The trade mark sign, the numero sign and the other symbols written in text. |

The code pages, each with its reason:

| Code page | Why it is in |
| --- | --- |
| Windows-1250, -1251, -1252, -1254 and -1257 (`cp1250`, `cp1251`, `cp1252`, `cp1254`, `cp1257`) | The character sets of Central European, Cyrillic, Western, Turkish and Baltic text on Windows: the Cyrillic alphabets, `ƒ`, `€` and their punctuation. |
| ISO 8859-16 (`iso8859_16`) | The Romanian letters with a comma below (`Ș ș Ț ț`). |
| Mac Cyrillic (`mac_cyrillic`) | The symbols of Macintosh text (`∆ √ ∞ ≈ ≠ ≤ ≥`). |

Left out, with the reason:

| Block or code page | Why it is out |
| --- | --- |
| Latin Extended-B, Cyrillic (as a whole block), Currency Symbols | Their digraphs, historic letters and some currency signs are wider or taller than any letter, so they would grow the font's glyph box and maximum advance; games size glyph buffers and place lines by those (Hot Shots Tennis' dialog moved). Their text letters come in through the code pages. |
| Combining Diacritical Marks | PGF text is drawn glyph by glyph and cannot set a mark over a letter; the precomposed letters are in. |
| Mathematical Operators, Arrows, Number Forms | Not text characters. Hot Shots Tennis writes the minus sign U+2212 in its dialog and draws its own character where the font has none. |
| Mac Roman, KOI8-R, CP866 | Their integral, box-drawing and block characters are taller than any letter (the glyph box again). |

With each face of the build (Liberation, or Geist with its Liberation Sans
`--fallback`, which the set's `draws` test counts) the set has 556 points, the
same for all eight faces (canonical SHA-256
`7eb9461a9b5e1012ff653746e9b0e3ff7947b8d51b27cc9fa224003a1705647d`); the
manifest records the set's digest and source `builtin:latin-text-blocks`.

`--japanese-text` (#2005, #1300) has a pinned core and a rule over public
standards. The core is the `--jis-x-0208-japanese` set widened by the EUC-JP
mapping of JIS X 0208 rows 48..84 (the 3,390 level-2 kanji), every assigned
double-byte character of Python's standard `cp932` table outside the
user-defined area (lead bytes 0xF0-0xF9, Unicode Private Use; 7,326
characters, with the NEC and IBM extensions) and the Latin-1 Supplement
U+00A0-U+00FF: 7,575 unique points with the pinned canonical SHA-256
`7c737cfedb45385d553dbfeecce9949135f185b5ca31ce486428cd4082904497`. The
command line adds every character of JIS X 0213:2004 and of the Unicode blocks
below that the input face, or the `--fallback` face where the input has no
glyph, draws. JIS X 0213 is read through Python's `shift_jis_2004` codec over
lead bytes 0x81-0x9F and 0xE0-0xFC, keeping cells that map to one code point
up to U+FFFF (10,905 points, pinned by count and digest; a cell that maps to a
base letter and a combining mark has no single code). No list of characters is
taken from a console font. The blocks, each with its reason:

| Block | Why it is in |
| --- | --- |
| Latin Extended-A U+0100-017F | Romanized Japanese with macrons (`ā ō ū`). |
| Superscripts and Subscripts U+2070-209F | Superscript and subscript digits in units and notes. |
| Combining Diacritical Marks for Symbols U+20D0-20FF | The enclosing circle and other marks JIS X 0213 uses with symbols. |
| Letterlike Symbols U+2100-214F | The trade mark sign (Ace Combat: Joint Assault writes "Memory Stick™"), `℃`, `№`, `℡`. |
| Number Forms U+2150-218F | Roman numerals and fractions. |
| Arrows U+2190-21FF | Arrows in menus and instructions. |
| Miscellaneous Technical U+2300-23FF | The arc `⌒` and the technical symbols of JIS X 0213. |
| Enclosed Alphanumerics U+2460-24FF | Circled and parenthesized numbers and letters. |
| Box Drawing U+2500-257F | Table and frame lines, the rounded corners included. |
| Block Elements U+2580-259F | Shades and half blocks. |
| Geometric Shapes U+25A0-25FF | Circles, triangles, squares and diamonds used as markers. |
| Dingbats U+2700-27BF | Check marks and negative circled numbers. |
| CJK Radicals Supplement U+2E80-2EFF, Kangxi Radicals U+2F00-2FDF | The radical forms of dictionaries and kanji lessons. |
| Hiragana U+3040-309F, Katakana U+30A0-30FF, Katakana Phonetic Extensions U+31F0-31FF | The kana beyond JIS X 0208 (`ゔ`, the small kana of Ainu). |
| Enclosed CJK Letters and Months U+3200-32FF | Parenthesized and circled kanji (`㈱`, `㊤`). |
| CJK Compatibility U+3300-33FF | Squared words and units (`㌔`, `㍻`). |
| CJK Unified Ideographs Extension A U+3400-4DBF, CJK Unified Ideographs U+4E00-9FFF | Kanji outside JIS X 0208: names, places and older forms. |
| Alphabetic Presentation Forms U+FB00-FB4F | The Latin ligatures (`ﬁ`, `ﬂ`). |
| CJK Compatibility Forms U+FE30-FE4F | The vertical forms of punctuation for text set in columns. |

Left out, with the reason. What CP932 and JIS X 0213 hold of these blocks is in
through the core and JIS X 0213:

| Block | Why it is out |
| --- | --- |
| Mathematical Operators, CJK Symbols and Punctuation | Their remaining characters (multiple integrals, the two-cell vertical repeat marks, the tone marks) are wider or taller than a full-width cell, so they would grow the font's glyph box that games size glyph buffers and place lines by. |
| Latin Extended-B, IPA Extensions, Combining Diacritical Marks, Greek and Coptic, Cyrillic, Latin Extended Additional, General Punctuation, Currency Symbols, Miscellaneous Symbols, Halfwidth and Fullwidth Forms | Their glyphs' sizes would overflow the 255-entry dimension table (section 6, `metric-table-overflow`); Western scripts are the Latin fonts' text. |

With Noto Sans JP and M PLUS 1p the set has 15,631 points (M PLUS 1p draws
341 of them); the manifest records the set's digest and source
`builtin:jis-x-0208-cp932-japanese+jis-x-0213+blocks`.

`--ks-x-1001-korean` decodes assigned cells in KS X 1001 rows 1-4 and 6-9
through Python's standard `euc_kr` codec, adds ASCII, and includes every modern
precomposed Hangul syllable U+AC00-U+D7A3. The 2,350 Hangul syllables in KS X
1001 rows 16-40 are contained in that modern range. The Korean text repertoire
has 11,951 unique points and canonical SHA-256
`50596a7f6e0e5158561147eae40a38edc12c28b1a4be3efff6d717c74b638bc4`. Its
sparse PGF map spans U+0020-U+FFE6 (65,479 map entries) and has 11,951 glyph
pointers; both counts are below the reader and writer's 1,048,576 ceiling.
Hanja is outside this repertoire. Rows 5 and 10-12 (Roman numerals, Japanese
kana, Greek, and Cyrillic) are outside the Korean text scope.

A refusal means no byte is written: the CLI builds the complete image and
manifest in memory first and writes only after every stage succeeds.

## 3. Determinism contract

1. **Scaling.** Font units to 26.6: `floor(value * ppem * 64 / upem + 1/2)`
   in exact rational arithmetic, rounded exactly once. Ties round toward
   positive infinity.
2. **Curve flattening.** Quadratic contours are expanded with implied
   on-curve midpoints, then subdivided by exact rational de Casteljau halving
   until the second-difference manhattan norm is at most 2 (26.6 units), or a
   hard depth of 24. No floating point participates anywhere. Each flattened
   vertex is then rounded once to an integer 26.6 coordinate by
   `floor(v + 1/2)`, ties toward positive infinity, the same rule as scaling.
   Contour `endPtsOfContours` indices must strictly increase; a violation is
   refused as `malformed-glyph`.
3. **Bounding box.** `left = floor(min_x / 64)`, `right = ceil(max_x / 64)`
   (likewise vertically) over the flattened, rounded outline. Flattening plus
   final rounding stays within `2/8 + 1/2` = 0.75 units of the true curve,
   while the first sub-sample centre sits 4 units inside a pixel boundary, so
   the grid can never clip a covered sub-sample nor include an empty border
   row or column.
4. **Supersample grid.** 8x8 samples per pixel. Sub-scanline centres are at
   `top26 - (row * 8 + step) * 8 - 4` in y-up 26.6; sub-column centres at
   `8 * j + 4` relative to `left26`.
5. **Fill rule.** Non-zero winding over edges using the half-open vertical
   rule `y0 <= y < y1`. Crossing intervals keep sub-columns strictly inside
   the active span (centres on an interval boundary belong to neither side),
   so shared contour vertices can never double-count a sample.
6. **Coverage rounding.** `floor((count * 15 + 32) / 64)` with `count` in
   0..64, mapping to the writer's 4-bit samples.
7. **Advances.** `hmtx` advance scaled by rule 1 (with the upem of the font
   that draws the code, rule 12), plus `(--embolden-advance + --tracking) * 8`
   26.6 units; `advance_y` is rule 16's. `--tracking K` (-8..16 sub-samples, default
   0) spaces every glyph alike. `--digit-advance D` (26.6) replaces the scaled
   advance of each digit 0-9 with `D` before that addition (tabular figures),
   and shifts that digit's outline right by `floor((D - natural) / 2)` 26.6
   units, `natural` being its scaled `hmtx` advance, so its ink stays centred
   in the new width.
8. **Iteration order.** Codes ascend; the writer sorts glyphs, metric tables,
   and map entries itself; manifest JSON uses sorted keys, 2-space indent,
   ASCII, and a trailing newline.
9. **No ambient input.** No clock, timestamp, locale, environment variable,
   hash-order iteration, or random source is read. The only dates anywhere
   are static provenance fields inside the pin (section 7).
10. **Synthetic emboldening** (optional, default off). `--embolden-x N` and
    `--embolden-y M` (each 0..8, in sub-samples, so eighths of a pixel) thicken
    strokes on the supersample grid after the fill rule and before the coverage
    count: every covered sub-column also covers the next `N` sub-columns to its
    right, then every sub-scanline also takes the coverage of the `M`
    sub-scanlines below it, so ink grows rightwards and upwards and the baseline
    and left edge stay put. The box first grows by `ceil(N / 8)` columns on the
    right and `ceil(M / 8)` rows on the top so the extra ink is never clipped.
    With both at 0 the output is byte-identical to the plain rasterizer.
11. **Baseline ellipsis** (optional, default off). `--baseline-ellipsis` shifts
    the U+2026 outline, in font units, so that its lowest point meets the
    lowest point of U+002E. A Japanese font centres the ellipsis on the em,
    which reads as three raised dots in Latin text.
12. **Second font** (optional, default off). `--fallback FILE` (with
    `--fallback-pin` when `--pin` is given) names a second TrueType font. A
    requested code the first font's character map lacks is drawn from the
    second, scaled by its own `unitsPerEm`; every other code comes from the
    first. Both fonts pass the same input checks, and the output name must
    satisfy both pins. The codes the second font draws are recorded in the
    manifest, and a fallback that draws none is refused.
13. **Overshoot alignment** (optional, default off). `--snap-overshoot N`
    (1..8, in sub-samples, so eighths of a pixel; 0 is off) sets a round
    letter's fractional-pixel overshoot on the flat height it overshoots.
    Each font that draws a code (rule 12) supplies three flat edges in font
    units, measured from its own outlines of two reference glyphs, looked up
    in its character map whether or not the code set includes them: the
    baseline `B`, the lowest point of `H` (U+0048); the x-height `X`, the
    highest point of `x` (U+0078); and the cap height `C`, the highest point
    of `H`. A depth `d` is *in the band* when `d > 0` and
    `d * ppem * 8 < N * unitsPerEm`, that is, less than `N` eighths of a pixel
    at the conversion's ppem, compared exactly in integers or rationals. A
    contour's box is the minimum and maximum `x` and `y` over all its points;
    a contour is *enclosed* when another contour of the same glyph has a
    different box that contains its box (a counter, or an island inside a
    letter). Then, for every contour of every converted glyph that is not
    enclosed, after rule 11's shift and before rule 7's digit centring and
    rule 1's scaling:
    - if the contour's highest point lies above `B` and its lowest point lies
      below `B` by a depth in the band, every point of that contour lying
      below `B` by a depth in the band gets `y = B`;
    - if the contour's lowest point lies below `X` and its highest point lies
      above `X` by a depth in the band, every point of that contour lying
      above `X` by a depth in the band gets `y = X`; likewise for `C`, with
      the same condition on the lowest point (below `X`).

    On-curve and off-curve points are treated alike and `x` never changes.
    A contour whose extreme lies on or inside the edge, or past it by the
    band or more (a descender, a cedilla), an enclosed contour, and a contour
    lying wholly above the x-height (an accent, the dot of `i`) are left
    exactly as they are. Every point of a moved
    contour then lies on or inside the edge, and a quadratic segment stays
    inside the hull of its points, so after rules 2 and 3 the glyph's raster
    box starts on the same row as the reference glyph's: a round letter and a
    flat one share their bottom row, and their top row at the x-height or cap
    height. The edges themselves are not moved to the pixel grid, the
    glyph's other points are untouched, and no hint program runs.
    *Project decision* (#1300), from a measurement: Liberation's round letters
    overshoot the baseline by 4 to 31 units of 2048 and the x-height and cap
    height by 7 to 23. A depth of `d` units is `d * ppem / 2048` px: the
    deepest, 31 units, is 0.30 px at 20 ppem and 0.20 px at 13, and any
    depth past the first sub-scanline centre (1/16 px, rule 4) covers
    samples in the next row. Rule 6 turns that into a whole extra row at 2
    or 3 of 15 coverage below the baseline, so in game text such as
    `Reload? No save data found.` an `e`, `o`, `a`, `s`, `d`, `t` or `u` ends
    one row below an `R`, `l` or `n`. An overshoot of `N` eighths of a pixel
    or more is kept as drawn. The project's Latin fonts use `N = 3`: the
    largest of those overshoots is 0.30 px at 20 ppem, under 3/8, and the
    nearest flat height that is not an overshoot, the top of Liberation
    Sans `f` 73 units above the cap height, is 0.46 px at 13 ppem, over 3/8,
    so ascenders keep their own height. The Geist faces (#1300), with the
    same `N` (the bold ones drawn at 19 and 12 ppem), set their round letters
    on the same rows as their flat ones too, with `l` as drawn without the
    option (`tools/test_ttf2pgf.py`). The rule does not tell an overshoot
    from a small deliberate excursion: at 13 ppem the extremes of `/`, `\`,
    `ø`, `Ø`, `§`, `∞`, `ƒ`, `@`, `×`, `÷`, the fractions `¼` and `¾`, and
    the spur of Liberation Serif `q`, which lie within 3/8 px of an edge,
    are set on it as well. The Japanese and Korean fonts do not use it: an
    ideograph has no x-height, and its strokes near those heights are not
    overshoots. The flat edges must ascend, `B < X < C`, and at
    the chosen ppem the band above `X` must end below `C`
    (`(C - X) * ppem * 8 > N * unitsPerEm`), so no point is in two bands.

14. **Horizontal scale** (optional, default off). `--scale-x P` (800..1250, in
    thousandths; 1000 is off) multiplies every `x` of every outline, in font
    units, by exactly `P / 1000` as a rational, after rule 11's shift and
    rule 13's alignment (which never moves `x`) and before rule 7's digit
    centring and rule 1's scaling; the `hmtx` advance of each code is
    multiplied the same way before rule 1. `y`, and so every row, is
    untouched, and at 1000 the output is byte-identical to a conversion
    without the option. Rule 7's digit centring uses the scaled advance.
    *Project decision* (#1300): the sans-serif stand-ins (Geist for the
    regular and bold styles, Liberation Sans for the italic ones) have letters
    that at the console's sizes come out narrower than the console fonts' and
    spaced looser to keep the console's line lengths. A horizontal scale moves
    ink from the gaps into the letters; the project's parameters were tuned
    against a private comparison with the console's own fonts, and only those
    parameters are recorded.
15. **Ink trim** (always on, from converter 0.7.0). After rule 6, each glyph
    image is cut to the columns and rows that hold a nonzero sample: the
    empty columns at its left and right and the empty rows at its top and
    bottom are removed, and the box's `left` grows by the columns cut on the
    left and its `top` falls by the rows cut on the top, so every kept sample
    stays on the same pixel relative to the pen and the baseline. The advance
    (rule 7) is not changed. A glyph with no nonzero sample becomes a
    zero-area record with only its advance, like a space. Rule 3's box holds
    every covered sub-sample, but a pixel with one or two covered sub-samples
    rounds to 0 under rule 6, and rule 10 grows the box by whole pixels for
    ink that may not reach the new column or row, so without the cut an image
    could start or end in a blank column or row.
    *Project decision* (#1300), from a measurement: the console's Latin fonts
    store their images cut to the ink in both directions (in three of them,
    at most 9 of the 94 printable ASCII images have a blank column or row on
    any one side, against 82 blank last columns in a 0.6.0 stand-in),
    and their space is a zero-area record. A game may place text by the image
    rather than by the advance: Hot Shots Tennis puts each glyph one pixel
    after the previous image's right edge, so every blank column widened its
    lines. Only that rule is taken from the measurement; no glyph data is.
16. **Vertical metrics** (always on, from converter 0.8.0). After rule 15,
    every record, a zero-area one included, gets the three vertical fields
    the character information returns as vertical bearing X (`x_center`,
    `0x28`), vertical bearing Y (`y_top`, `0x2C`) and vertical advance
    (`advance_y`, `0x34`). Let `em` be the em the header declares: the
    nominal size (26.6 points) at the resolution (26.6 dpi),
    `nominal * resolution / 4608` in 26.6 px (1152, 18 px, for 10.125 points
    at 128 dpi), and `frac(T)` be `T / 1000` of it, rounded half up to a
    whole 26.6 unit.
    - `x_center = -floor(dimension_width / 2)`: the box centred on the
      vertical origin.
    - `advance_y = frac(A)`, `A` from `--vertical-advance A` (1..2000,
      default 1000: one em).
    - With `--vertical-origin O` (1..2000): `y_top = frac(O) - y_base`, the
      distance from `O / 1000` em above the baseline down to the glyph's top.
      Without it: `y_top = floor((advance_y - (ascender - descender)) / 2)`,
      the same for every glyph, with the header's ascender and descender
      (`--header-ascender` and `--header-descender`, else the extremes the
      writer derives).
    The header's minimum centre X, maximum top Y and maximum vertical
    advance follow from the records. No row, column or horizontal metric
    changes, so text set horizontally draws the same pixels.
    *Project decision* (#1300), from a measurement of one console's fonts
    through the project's reader: in its Latin, Japanese and Korean fonts
    every glyph's vertical bearing X is minus half its 26.6 width, and each
    font has one vertical advance: 0.950 em in the larger Latin size, 0.917 em
    in the smaller, one em in the Japanese font and 1.185 em in the Korean.
    The Latin and Korean fonts give every glyph the same vertical bearing Y,
    half the room that advance leaves beyond the font's ascender-to-descender
    height (17 or 18, 11 or 12, and 57 or 58 26.6 units); the Japanese font
    gives each glyph the distance from 0.88 em above the baseline down to its
    top (503 of 529 glyphs sampled; the others are 25 full-width small Latin
    letters and the full-width dollar sign). The stand-ins wrote the box's
    left edge plus half the advance, its top and 0. Only these rules and
    ratios are taken from the measurement; no glyph data is. The project's
    reader passes the three fields to the game unchanged and draws nothing
    by them, and the console library draws by the image alone
    (`PGF_SPEC.md` section 3.5, #1055); a game that sets text vertically or
    steps lines by the vertical advance would read them.

Same pinned input plus same parameters plus same converter version therefore
produce byte-identical `.pgf` and manifest output on every supported host,
which `tools/test_ttf2pgf.py` asserts by repeated runs.

## 4. Geometry to PGF metrics

For a glyph whose raster box, cut to its ink by rule 15, is
`left, bottom, right, top` in whole pixels (relative to the origin on the
baseline, y up):

| Field | Value |
| --- | --- |
| `width`, `height` | box size in pixels (7-bit record fields). |
| `dimension_width`, `dimension_height` | `width * 64`, `height * 64`. |
| `x_left` | `left * 64` (bitmap left edge relative to the pen). |
| `x_center` | `-floor(dimension_width / 2)` (rule 16). |
| `y_base` | `top * 64` (pixels of bitmap above the baseline). |
| `y_top` | rule 16: `frac(O) - y_base` with `--vertical-origin`, else the margin above the ascender. |
| `advance_y` | rule 16: `frac(A)`, one em by default. |
| descender (derived) | `y_base - dimension_height = bottom * 64`, matching `PGF_SPEC.md` section 3.9. |
| `adjust_x`, `adjust_y` | `left`, `top`: the reader returns them as the character information's bitmap left and top (`0x08`, `0x0C`); its draw path uses only the caller's position (`PGF_SPEC.md` section 3.10). |
| `row_order` | 1 (raster order). |

Header extrema are then whatever `pgf_writer` derives from those records:
`ascender = max(y_base)`, `descender = min(y_base - dimension_height)`, and
the maxima of dimension, advance, and pixel size. `--header-ascender` and
`--header-descender` replace those two extrema, in 26.6 pixels, for a font that
stands in for one with design values there. The header's horizontal and
vertical size fields (`0x24`, `0x28`) are not extrema: they carry the nominal
em size, `ppem * 64` in 26.6 units unless `--nominal-size` gives another, so a
game reads the chosen size rather than the largest advance. `--resolution` sets
the two resolution fields (`0x2C`, `0x30`), 26.6 dpi, default 1920. A glyph with no covered
pixel (a space) is emitted as a zero-area record with only its advance, which
the reader reports as present-but-unmapped exactly like `PGF_SPEC.md`
section 3.9 describes.

## 5. Metric-target policy

Sizing policy is a separate input from the outlines so PSP-like metrics can
be produced and tested without embedding any retail font bytes
(`FONT_ORIGINS.md` route (2) asks for ascender 9..10 px and descender -2 px
as the small-latin target). `--metric-targets FILE` takes:

```json
{
  "ascender_px": 9,
  "descender_px": -2,
  "tolerance_px": 1,
  "ppem_min": 6,
  "ppem_max": 24
}
```

The search picks the **smallest** `ppem` in `[ppem_min, ppem_max]` whose
exact header extrema (rule set of section 4) land within `tolerance_px` of
both targets; a cheap control-point bound prunes candidate sizes and only
candidates within tolerance plus two pixels are measured exactly. If no size
qualifies, the refusal is `metric-target-unreachable` - the policy is never
silently relaxed. The chosen ppem and both achieved values are recorded in
the manifest's `metric_targets` block. `--ppem` and `--metric-targets` are
mutually exclusive.

## 6. Output and naming policy

The container is written entirely by `tools/pgf_writer.py`: revision 2, the
392-byte base header, direct character map only, no shadow records, no
subcharmap tables - the subset `PGF_SPEC.md` documents for revision 2 and the
writer's own contract (a code span with a sparse character map when the code
set has gaps, at most 255 entries per metric table). Kerning beyond per-glyph
advances does not exist in PGF and none is fabricated (`PGF_SPEC.md` section
3.10).

The default output name is `Nakagawa Open Latin` / `Regular`. Before anything
is written the converter refuses:

1. PSP firmware font names (`jpn0`, `kr0`, `ltn0`..`ltn15`, with or without
   `.pgf`) - enforced by the writer and re-raised unchanged.
2. The vendor camouflage markers recorded in `FONT_ORIGINS.md` section 5.3
   (`camouflage-font-name`), case-insensitive. `FTT` and `SONY` match only as
   separate words; the other markers match anywhere in the name.
3. The Adobe OFL Reserved Font Name `source`, per `FONT_ORIGINS.md` route (2),
   and the reserved or trademark names `Atkinson`, `Hyperlegible`, `Sawarabi`,
   `Gowun`, `Nanum`, `Gudea`. These match anywhere in the name, as does
   `source`. `Noto`, `Lato`, `Ume` and `M PLUS` match separate words only
   (`reserved-font-name`).
4. Any Reserved Font Name listed by the active source pin (for the pinned
   fixture: `Gudea`), per OFL section 3.

The rules for items 2 and 3 are the single table `DENIED_FONT_NAME_RULES` in
`tools/pgf_writer.py`, which the writer applies to every output name as well.

Generated PGFs are fixtures, never an authenticity claim.

## 7. Source pin and conversion manifest

`--pin FILE` must bind the exact input bytes (schema `font-pin/v1`):

| Pin field | Verified against |
| --- | --- |
| `sha256`, `size_bytes` | SHA-256 and length of the input file. |
| `license.license_text`, `license.license_text_sha256` | Licence file beside the pin, read as UTF-8. |
| `license.spdx`, `license.copyright`, `license.reserved_font_names` | Carried into the manifest verbatim. |
| `source.url`, `source.repository`, `source.path`, `source.retrieved` | Static provenance of the pinned bytes; carried into the manifest. |

A font the project already ships may instead be pinned by the static font list
that records it (`assets/fonts/ui/manifest.json`: a JSON object with
`schema_version` 1, an `upstream` object and a `fonts` list, and no `schema`
field). The converter takes the one entry whose `sha256` is the input's
digest and refuses the input (`pin-digest-mismatch`) when no entry, or more
than one, has it. It then requires, each a `pin-invalid` refusal otherwise:
the entry's `file` names the input file (the same leaf name), `static` is
`true`, and `license_file` is in the same folder as `file`. `size_bytes` is
verified as for a pin (`pin-size-mismatch`), and the licence text is the file
of that leaf name beside the input, verified against `license_sha256`
(`pin-license-mismatch`). The list records no copyright line, so the licence
text's first paragraph (up to the first blank line), which must begin with
`Copyright`, is the manifest's copyright line, whitespace runs joined by single
spaces. The Reserved Font Names are the entry's `reserved_font_names` (none when
absent), and a copyright paragraph that mentions a reserved font name while the
entry lists none is refused, so a reserved name cannot be missed. The
manifest's `source` record is the list's `upstream.repository` and
`upstream.commit` with the entry's `upstream_path`, `upstream_sha256`,
`source_kind` and, for a variable-font instance, its `instance` axes
(`wght=400`). `family` is the entry's. The CLI passes the input and
`--fallback` paths for this; a pin file needs neither.

`--manifest FILE` (which requires `--pin`) emits canonical JSON with:

| Block | Contents |
| --- | --- |
| `converter` | Name, version, and this specification's path. |
| `input` | File name (no directory), SHA-256, size, `unitsPerEm`, glyph count, outline format, tables used. |
| `output` | File name (no directory), SHA-256, size, font name/type, revision, first/last glyph, the header size fields and extrema read back from the emitted bytes. |
| `coverage` | First/last code, converted code count, code span, and `U+XXXX..U+YYYY` range. |
| `code_points` | For `--codepoints`: the file's leaf name, count, and SHA-256; `null` for a range. |
| `glyphs` | Count, zero-area codes, maximum pixel dimensions. |
| `metric_targets` | The policy, the chosen ppem, and achieved 26.6 values - or `null`. |
| `parameters` | ppem, nominal em size (26.6), resolution (26.6), any header ascender and descender given, whether the ellipsis was set on the baseline, supersample, coverage bits, rounding formulas, hinting mode, and the `embolden` strengths (x, y, advance, in sub-samples); `tracking_sub_samples` and `digit_advance_26_6` only when given; `snap_overshoot` only when given: its unit, threshold `N`, and the flat edges `B`, `X` and `C` in font units of the first font (`zones`) and, with a fallback that draws codes, of the second (`fallback_zones`); `scale_x_thousandths` only when it is not 1000; `vertical_advance_thousandths` and `vertical_origin_thousandths` only when given. |
| `fallback` | Only with `--fallback`: the second font's `input` record (as above), its `license` block, and the ascending `codes` it draws. |
| `license` | SPDX id, copyright line, reserved font names, the **full licence text**, its digest, and the source pin's provenance record - or `null` without a pin. |

## 8. Independence and provenance record

- **Sources used.** The public OpenType/TrueType format documentation (table
  layouts and semantics as published by the format stewards), the
  project-authored reader specification `PGF_SPEC.md` and its public reader
  `src/rt/pgf_public.c`, the writer contract of `tools/pgf_writer.py`, the
  route decision and measurements in `FONT_ORIGINS.md`, and project-owned
  tests.
- **Sources deliberately not used.** `pgftool` (no licence grant,
  `FONT_ORIGINS.md` section 4), `ttf2pgfj.exe` (provenance unknown),
  PPSSPP's font or PGF converter sources, JPCSP or sal063 sources, and any
  retail, firmware, or camouflaged font payload. No bytes of those inputs are
  read, embedded, or compared against. This record exists so the claim in
  issue #313 is checkable rather than asserted.
- **Classification.** Candidate `project-authored-independent` under
  `INDEPENDENCE_MODEL.md`: behaviour established from public format facts and
  project-owned specification, expression written for this project. The
  authoritative attestation is the maintainer's trusted ledger entry, not
  this paragraph; publication admission of the new paths is maintainer-side.
- **Pinned fixture.** `tests/fixtures/fonts/gudea/` carries `Gudea-Regular.ttf`
  (SHA-256 `a4b5410090a821ea17021627312babdf3d8db0271217fd8c4d9f629ef0a2d90b`,
  22,932 bytes), its `OFL.txt` (SHA-256
  `315a576cbc7ab61c9e347b5725893bc8498fdcb8fc10831793c6864bc2cefba8`), and
  `PIN.json`. The source is the SIL Open Font License 1.1 family *Gudea* by
  Agustina Mingote as published in `google/fonts` at `ofl/gudea/`. OFL
  section 2 material ships alongside the bytes; the output name avoids the
  family's Reserved Font Name.
- **Generated PGFs.** Test runs write PGFs only to temporary or ignored
  (`build/`) paths; this change commits no generated `.pgf` and no
  `font/*.pgf` payload, so `public_source_profile.json` keeps excluding the
  firmware-font route unchanged.

## 9. Verification map

| Property | Test |
| --- | --- |
| Byte-identical repeat conversions (in-process and CLI) | `ConversionDeterminismTests` |
| Manifest digests, coverage, licence material, header agreement | `ConversionDeterminismTests` |
| Pin binds exact input and licence bytes | `SourcePinTests` |
| Reader round trip, drawn samples, `H` shape, zero-area space, composites | `ReaderRoundTripTests` |
| Metric targets land header extrema in tolerance | `MetricTargetTests` |
| Code-point set converts only the listed codes, sparse map, digest in manifest, nominal size | `CodePointSetTests` |
| Sparse code-point set reads back through the reader; absent codes are misses | `ReaderRoundTripTests` |
| Every named refusal writes nothing | `RefusalTests`, `CompositeRefusalTests` |
| Tracking, tabular digits, the horizontal scale and the second font: effect, manifest record and refusals | `SpacingAndFallbackOptionTests` |
| Built-in Latin, Japanese and Korean sets and the pinned fonts' coverage of them; the block and code-page rules equal their definition over the faces' character maps; the small sans face keeps its glyph box | `ProjectCodePointCoverageTests`, `JapaneseCodePointCoverageTests` |
| Project Latin faces: one per console index, line widths, class widths, ink, and the sans faces' x-height and cap-height rows | `ProjectLatinStrikeTests` |
| Overshoot alignment: round and flat letters share their rows, flat letters and deep features unchanged, manifest record and refusals | `OvershootSnapTests`, `ProjectLatinStrikeTests` |
| Ink trim: no image starts or ends in a blank column or row, a string drawn through the reader by advance keeps every pixel, advances unchanged | `InkTrimTests`, `ReaderRoundTripTests` |
| Vertical metrics: the reader returns bearing X, bearing Y and advance by rule 16 with and without each option; the project fonts carry the console's values; out-of-range options refused | `ReaderRoundTripTests`, `ProjectLatinStrikeTests`, `ProjectCjkSizeTests`, `SpacingAndFallbackOptionTests` |
| Project Japanese and Korean nominal size and advances | `ProjectCjkSizeTests` |
| Independent structural validation | `nk_core.fonts.validate_pgf_data` on the output |

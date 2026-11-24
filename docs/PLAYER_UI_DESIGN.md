# Player UI design system

The visual spec for the native player's launcher UI (the only launcher since v0.0.1; the old renderer was removed
in #2071). It is the reference every
screen lane builds against: Library and Title detail (C5, built), Settings (C6), first run (C7) and error dialogs
(C8). The tokens and components it names live in code:

| Layer | File | Holds |
| --- | --- | --- |
| Tokens | `src/ui/nk_ui_theme.h`, `src/ui/nk_ui_theme.c` | colours, spacing, radii, borders, control sizes, layout grid, motion, font slots, type scale |
| Components | `src/ui/nk_ui_widgets.h`, `src/ui/nk_ui_widgets.c` | button, game card, add card, hero, status pill, tag, badge, eyebrow, card, divider, top bar, prompt bar, vector art |
| Core | `src/ui/nk_ui.h`, `src/ui/nk_ui.c` | layout (Clay), text runs, the focus ring, frame text storage, custom elements |
| Screens | `src/player/ui/scr_*.c` | compose components; never pick raw colours or sizes |

It follows the owner's interactive prototype (version 8) and the "Player" boards of the design canvas, both private.
Where this page and the code disagree, the code is current and this page is updated in the same change.

Read this page before you change anything the launcher or the in-game overlay draws. Every token is also listed in a
machine-readable block at the end, the [Token record](#token-record), with the source symbol it comes from and the
text-on-background pairs the UI draws. `PlayerUiDesignTokenTests` in `tests/native/test_player_ui.py` reads that
block and the C source and fails when they differ, when a colour literal in the UI source is missing from the
record, or when a pair falls below its [contrast](#contrast) target and is not a [known shortfall](#known-shortfalls).

## Principles

- **One dark theme, one accent.** Ground `#0B0F14`; every surface steps up in colour, not with shadows. The
  accent is the brand blue the project website uses (`#6B9FF0`); the status colours keep clear of it (`info` is
  violet).
- **Controller first.** Every action is reachable with a D-pad and two buttons, focus is always visible, and the
  prompt bar names the buttons that work on this screen. Nothing needs a pointer or hover.
- **Status is a word and a shape, then a colour:** `●` Ready, `▲` needs setup, `◆` experimental, `?` untested,
  `✕` unsupported. Colour never carries meaning alone.
- **Honest copy.** Words describe what the build does today. Compatibility words come from data
  (`docs/COMPATIBILITY.md`), never from layout code.
- **Bring your own game.** No game data, art, music or fonts ship with the player; placeholders say where the
  user's own disc art will appear.
- **Say it once, where the user decides.** The privacy promise ("Your games stay on this PC. Nakagawa never
  uploads anything, and downloads only the build tools you approve.") appears on the setup wizard's Welcome step
  and on About, and nowhere else: not in prompt-bar corners, not on the Add card, not in page footers. A screen
  that asks for consent (the build-tool download) says exactly what it fetches, from where, and how big it is.
- **No console trade dress.** No PSP system icons, sounds or names, no XMB-style horizontal category bar. The
  waves, the palette and the seasonal arc are the project's own.
- **No key-file UI anywhere.** The player decrypts the user's own disc with built-in tables (#1494) and reads no key
  file. No screen, step, setting, dialog or path row asks for, finds or names a key file. A disc whose program
  is encrypted gets the plain note the host supplies ("Nakagawa decrypts this game's program from your own disc, on
  this PC"), and one it can't decrypt gets the named reason.

## Tokens

### Colour

| Token (`NkUiTheme`) | Value | Use |
| --- | --- | --- |
| `surface` | `#0B0F14` | window ground, add card |
| `surface_raised` | `#121821` | cards, panels, prompt bar |
| `surface_high` | `#18202B` | chips, focused secondary button, focused row |
| `line` | `#1C232D` | hairlines, card edges |
| `line_strong` | `#2E3846` | control and chip borders, focused card edge |
| `selected_neutral_edge` | `#6B7684` | edge of a current value marked without the accent (4.2:1 on the ground, 3.5:1 on `surface_high`) |
| `selected_neutral_edge_dimmed` | `#576270` | that edge on a dimmed row, a step weaker (3.1:1 on the ground; 2.6:1 on `surface_high`, its own box, under 3:1 and accepted because a dimmed row is inert) |
| `paper` | `#F5F7FA` | primary text |
| `muted` | `#9AA4B2` | secondary text (7.6:1 on the ground) |
| `dim` | `#6B7684` | disabled text and footnotes only (4.2:1, not for body text) |
| `accent_dark` | `#6B9FF0` | the brand blue, the website's dark accent: focus ring, primary button, active tab rule, toggles, sliders, progress; as a ring 7.2:1 on the ground, 6.7:1 on `surface_raised`, 6.1:1 on `surface_high`, 3.7:1 on the scrim over a white frame, 4.4:1 over the brightest wave |
| `focus` | `#6B9FF0` | the focus ring (`nk_ui_focus_ring` draws this field); the same value as `accent_dark`, kept as its own field |
| `ink` | `#10151C` | text on an accent fill (6.8:1 on `accent_dark`) |
| `accent_hover` | `#8FB8F2` | primary button under the pointer, the website's mark blue (ink on it 9.0:1) |
| `accent_pressed` | `#4F86DC` | pressed primary button (ink on it 5.0:1) |
| `pressed` / `pressed_danger` | `#222C39` / `#2A1A1C` | pressed secondary or row / danger button |
| `ready` | `#2DD4BF` | Ready; never the accent |
| `warning` | `#DBB024` | needs setup |
| `info` | `#C29AF4` | experimental, untested, informational notes: violet, 50 degrees of hue from the blue accent (8.4:1 on the ground, 7.2:1 on `surface_high`) |
| `error` | `#FF7B72` | unsupported, failed, remove |
| `scrim` | ground at 78% | over the frozen game frame behind the in-game overlay (`src/rt/gui.c`); the launcher's dialogs draw no scrim (see [Loose ends](#loose-ends-in-the-code)) |
| `art_scrim` | ground at 72% | under a badge on title art |
| `art_label` | `#C9D6E3` | captions over title art |
| `placeholder` | `#3A4552` | art placeholder outline, inactive glyph dots |
| `capture` | `#223046` | a row waiting for a button press (C6 controls): the accent at 18% over `surface_raised`, paper on it 12.4:1; no screen draws it on `main` (see [Loose ends](#loose-ends-in-the-code)) |
| `accent_light` | `#2F6FD0` | accent on a light surface, the website's light accent (4.9:1 on white; reserved, the UI has none) |

### Spacing, radii, borders

- Spacing is a 4 px scale: `NK_UI_SPACE_1` 4, `_2` 8, `_3` 12, `_4` 16, `_5` 20, `_6` 24, `_7` 32, `_8` 48, `_9` 64.
- Radii: chip 4 (pills, badges), control 8 (buttons, rows), tile 12 (cards, panels, hero), dialog 16, pill (full).
- Borders are 1 px hairlines (`NK_UI_BORDER_HAIRLINE`). Rounded borders draw straight runs plus antialiased quarter
  rings; every edge rounds to whole device pixels at every UI scale.
- Elevation is a colour step plus an edge: 0 page; 1 card (`surface_raised` + `line`); 2 control or focused row
  (`surface_high` + `line_strong`); 3 dialog (`surface_raised` + `line_strong` over the scrim).

### Type

Fonts are SIL OFL 1.1 files in `assets/fonts/ui/`, registered in eight slots (`NK_UI_FONT_*`); every face falls
back to Geist and then Noto Sans JP. Sizes are logical px; tracking is CSS letter-spacing (it follows every
character).

| Role (`NK_UI_TYPE_*`) | Face | Size / line | Tracking | Use |
| --- | --- | --- | --- | --- |
| `DISPLAY_XL` | Chakra Petch Bold, caps | 40 / 44 | 0.04em | first-run headline |
| `DISPLAY` | Chakra Petch Bold, caps | 30 / 32 | 0.04em | screen names (LIBRARY, SETTINGS) |
| `WORDMARK` | Chakra Petch Bold, caps | 18 / 20 | 0.06em | NAKAGAWA RECOMP in the top bar |
| `TITLE_XL` | Geist SemiBold | 30 / 34 | 0 | a game's name on its page |
| `TITLE` | Geist SemiBold | 22 / 28 | 0 | dialog and panel titles |
| `HEADING` | Geist SemiBold | 17 / 24 | 0 | card headings |
| `TILE_TITLE` | Geist SemiBold | 15 / 20 | 0 | game card names (two lines, then an ellipsis) |
| `BODY_L` | Geist | 17 / 26 | 0 | lead paragraphs |
| `BODY` | Geist | 15 / 22 | 0 | running text |
| `BODY_S` | Geist | 14 / 21 | 0 | card paragraphs |
| `CAPTION` | Geist | 13 / 19 | 0 | hints, counts, notes |
| `FOOTNOTE` | Geist, `dim` | 12 / 16 | 0 | sources and fine print |
| `LABEL` | Geist Mono Medium, caps | 12 / 16 | 0.08em | buttons, prompts, tabs |
| `LABEL_L` | Geist Mono Medium, caps | 14 / 18 | 0.12em | the large primary button |
| `LABEL_S` | Geist Mono Medium, caps | 11 / 14 | 0.06em | chips, card meta rows |
| `EYEBROW` | Geist Mono Medium, caps, `muted` | 11 / 14 | 0.12em | section headers |
| `MICRO` | Geist Mono Medium, caps | 10 / 12 | 0.08em | badges over art |
| `DATA` | Geist Mono | 12 / 16 | 0 | disc IDs, figures, paths |

Labels, chips and eyebrows are drawn in caps (`nk_ui_caps`); the words stay sentence case in data.

### Text: wrapping and length

- **Which roles wrap.** `nk_ui_type` turns wrapping on for `TITLE_XL` through `FOOTNOTE` (titles, headings and
  running text) and off for the display roles, labels, `EYEBROW`, `MICRO` and `DATA`. A screen may turn it on for one
  run, as the error card does for its `DATA` details box.
- **Long text that can be read elsewhere** (a value with its own editor or detail page, a game card's name, a log
  line) is shortened with `nk_ui_text_clamp`: it keeps as many whole lines as the call allows and ends the last in
  an ellipsis. A game card's name keeps two lines.
- **Text with nowhere else to be read** wraps whole, and the page or card it sits in scrolls when it is taller than
  the room between the bars (see [Layout grid](#layout-grid) for the cases at larger text sizes).
- Wrapped text stays inside its box and keeps its wrapped height when a column is overfull (see
  [Layout grid](#layout-grid)).
- **Pending.** A long value that wraps inside the Settings options detail panel, and a panel whose text scrolls when
  it is taller than its room, are in [#2754](https://github.com/Jstar269/nakagawa-recomp/pull/2754), a Draft
  that is not on `main`; this page describes `main`.
- The launcher ships one English catalog for screen and shared-widget text.
  [Adding launcher strings](../CONTRIBUTING.md#launcher-strings) describes the catalog and the pseudo-translation check.
  [#2953](https://github.com/Jstar269/Nakagawa-Recomp/issues/2953) tracks the in-game overlay and messages produced outside the launcher screens.

### Focus

- The ring is drawn by the core outside the focused element, so the layout never moves when focus moves: 3 px
  `accent_dark`, 3 px gap, radius = the element's radius + 6. It is a Clay floating element on the control, drawn
  over everything else and clipped like the control, so a control half scrolled out of a list shows half a ring
  (`test_focus_ring_floats` in `tests/native/test_nk_ui_core.c`).
- The ring is one element whichever control has it, so when focus moves Clay sees it change parent and eases it
  across over `NK_UI_MOTION_FAST`. A control that was not on screen in the last frame, a new page's, gets a new ring
  at once rather than one flying in from the last page (`test_focus_ring_moves`).
- High-contrast focus (`NkUiPrefs.high_contrast_focus`): a 4 px white ring between 2 px black keylines. The
  launcher draws it when Settings > Accessibility > High contrast is On, or Automatic while Windows uses a contrast
  theme.
- A focusable control calls `nk_ui_register_button` and, when focused, `nk_ui_focus_ring(ui, id, radius)`. A focused
  secondary button also fills with `surface_high`; a focused card's edge becomes `line_strong`.
- Arrow keys move to the nearest control in that direction. A control may name the control an Up and a Down move
  from it land on instead (`nk_ui_register_button_between`, `nk_ui_button_between`; a tab of `nk_ui_section_tabs`
  names its Down neighbour in `NkUiSection.down`), for a place where the nearest one by position is not the next
  one to a person: a list of full-width rows under a strip of small tabs. Position measures to a control's middle,
  so under a narrow tab a small button further down can be nearer than the full-width row just below the tab, and
  a Down move would pass the whole list. A named neighbour wins over position and is taken even when its scroll
  container has it out of view; the move then scrolls it in.
- A control that a scroll container clips away whole is otherwise reached only from inside that container, whose
  rows scroll it into view, never from a control outside it. A scroll container knows the one it sits in
  (`NkUiFocusScroller.parent`), and a control in the inner one counts as inside the outer one: the last row of a
  list in a page that scrolls reaches the button under the list while the page has that button out of view
  (`test_nested_scrollers` in `tests/native/test_nk_ui_focus.c`).
- A control made of several pointer targets (a row of values) registers the row as the focus stop and each target
  with `NK_UI_BUTTON_POINTER_ONLY`: a press focuses a target and its release activates it, no move, Tab or first
  focus lands on it, and the screen hands focus back to the row.
- The pointer is the other way in: a press focuses the control under it, and the release on the same control
  activates it. Clay finds what is under the pointer from the last layout (`Clay_SetPointerState`, every frame),
  inside clip containers, so a control scrolled or clipped out of view takes no click, and only controls of the
  active scope respond: nothing under a dialog. A button with `NK_UI_BUTTON_ROW` keeps Left and Right in
  its row, so a row of actions under cards of different heights is walked end to end.
- When the screen changes and the old focus is gone, focus lands on the page's first enabled control that may be the
  default: the title page's PLAY or BUILD PACKAGE, the new card after ADD TO LIBRARY. Top-bar tabs
  (`NK_UI_BUTTON_NOT_DEFAULT`) and danger buttons never are; a screen whose only control is a danger button (CANCEL
  BUILD while a build runs) starts with nothing focused, and a move reaches it. PLAY or BUILD PACKAGE gets focus back
  when it comes back on after STARTING… or a package check, unless the user moved meanwhile.

### Scrolling

A list longer than its room is a scroll container (`NkUiLayout.scroll`), scrolled by Clay
(`Clay_UpdateScrollContainers`, every frame, with the time since the last one): a wheel notch over it moves it 48 px,
three of its 16 px lines; a drag with the mouse held, or a finger, moves it with the pointer and a fling keeps going a
moment; a drag that scrolls more than 8 px is no click on the control it started on. A control inside it that takes
focus, by key, pad or pointer, is scrolled fully into view, by the least it takes. Where scroll containers nest (a
list inside a page that scrolls, up to `NK_UI_FOCUS_SCROLL_DEPTH` deep) every container around the control scrolls,
the innermost first, each to show what the one inside it shows of the control. A move is judged on the layout
before it, and a page that describes its focused control changes height with the focus, so the two layouts after a
focus change bring the focused control into view again where they put it (`NK_UI_FOCUS_REVEAL_FRAMES`). The
launcher draws at 60 Hz while a list, or anything else, still moves (`player_ui_nk_wait_ms`) and waits for input
again once it settles. A wheel over anything else is
the screen's own: the library turns its page with it. Only the lines a long list shows are laid out, with spacers
for the rest, so its length costs nothing (`test_scroll_container` in `tests/native/test_nk_ui_core.c`).

The library pages instead of scrolling. Its pager is the controller's way through a large library (a trigger is a page,
and a page's cards are the D-pad's whole field), and only the page shown reads its disc icons and checks its packages,
so a page costs what a small library does; a scrolled grid would read every icon it passes.

### Hover and press

Hover and press are looks for the pointer, never needed to use the launcher. While the pointer is over a control
and was the last input, the control shows hover: a secondary, danger or tab control fills with `surface_high`, a
primary button with `accent_hover`, a card fills with `surface_high` and its edge becomes `line_strong`, and the
add card fills with `surface_raised`. Held down on the control it went down on, it shows press: `accent_pressed`,
`pressed_danger`, or `pressed` for every other control (the add card: `surface_high`). A key or a pad button hides hover until the pointer moves
again, and the pointer leaving the window clears it. Hover never moves focus, and only the ring shows focus
(`test_pointer_hover_and_press` and `test_pointer_respects_clip` in `tests/native/test_nk_ui_core.c`).

### Layout grid

| Measure | Value |
| --- | --- |
| Top bar / prompt bar | 64 / 56 px, separated from the body by a `line` hairline |
| Page gutter | 32 px; 16 px below 900 px wide (`NK_UI_GUTTER`, `NK_UI_GUTTER_NARROW`) |
| Body padding | 24 px top, 16 px bottom; 16 px between blocks |
| Library grid | columns = how many 236 px cards fit with 20 px gaps (1 to 6); rows = how many fit; 16 px row gap |
| Game card | art panel 0.53 x width, body 90 px |
| Title detail | 520 px aside (at most 45% of the body) and a growing column, 32 px apart; hero at 480:272 |
| Minimum target | 44 px high (`NK_UI_CONTROL_HEIGHT`); large 52, small 34 |
| Safe area | nothing closer than 16 px to a window edge |

Layout is logical px; the UI scale (Auto follows the display, or 100/125/150/200%) gives the device density, and
the logical width decides the columns. The text size (`NkUiViewport.text_size_pct`: 90, 100, 125 or 150) scales type
alone, on top of the density: each run is measured, given its letter spacing and line height, and drawn at
`size x density x text size`, while boxes, gaps, padding and the logical size keep the density alone, and the layout
reflows around the larger text. At 100% every screen is laid out exactly as before. A component designed around one
line of text (a button, chip, badge, tab, row) keeps its height while the line fits with a quarter of a line above
and below it, and grows past that (`nk_ui_line_box`); at 150% that is a chip or badge, never a button. Screens
reserve room for text from its own measurements (`nk_ui_text_width`, `nk_ui_button_width`, `nk_ui_line_height`),
not from constants. At a larger text size:

- a column sized for its words grows to the widest of them (check rows, the setting rows' button column, the licence
  card, the Controls page's CONTROLLER and KEYBOARD pair and stick button);
- a row of buttons wraps onto a second row when it would run past its column (`nk_ui_flow_begin`: the Settings
  actions, the title page's actions);
- the setting rows on Accessibility, the online options, Updates and build tools put each sentence under its
  button, at the full width, when beside it a sentence would be cut, and there it wraps whole;
- the Library's narrowest card grows with the text, so fewer columns fit and a page holds fewer cards
  (`player_ui_library_page_size_at`), and its heading row leaves the LIBRARY heading out when the row's controls
  would not fit beside it (the tab names the page);
- the prompt bar's hint ends in an ellipsis in the room the prompts leave;
- a page body (Library, title page, Settings, About) or a flow card taller than the room between the bars scrolls
  there, and a control taking focus scrolls into view. The title page's body scrolls at any text size, because what
  a game's page shows varies (#2245); the screen tests check that its fullest form scrolls at 1280x720 with the
  prompt bar whole.

The launcher offers it as Settings > Accessibility > Text size; the in-game overlay stays at 100%. The launcher's
screens are laid out on at least its
1280 x 720 design size (`NkUiViewport.min_logical_w` / `min_logical_h`): a smaller window, down to the 960 x 540
minimum, lowers the density so the same layout is drawn smaller, instead of pushing buttons and the prompt bar off
the window. A larger window keeps its density and gains room. Clay clips at most 100 containers per layout: shorten
long text with `nk_ui_text_clamp` instead of clipping it, but only text the player can read whole another way (a
value with its own editor or detail page); text with no other place to be read wraps whole on a page that scrolls.
One exception is by design: at the design text size a setting's sentence beside its button is clamped to two lines,
so the rows keep their side-by-side layout (Settings > Enhanced uses compact value rows and one focused-option detail panel instead). Above it, a sentence that would be cut makes the rows stack, and under
its button the sentence wraps whole.

A word wider than its whole box (a folder's name, a long identifier) would still run past it, as the layout wraps
only between words: text that can hold one goes through `nk_ui_text_break`, which breaks such a word across lines
of the box's width and leaves every other word to the layout.

Wrapped text stays inside its box. Clay adds a run up word by word, and a text backend measures in whole pixels, so
words measured one at a time, each with a lone space, fell short of the line as drawn (8 px for a ten-word line of
14 px Geist): a line was kept that ran into the padding beside it, and a one-line run's box was narrower than its
glyphs. `nk_ui_clay_measure` therefore counts a word as the growth of its line (the line up to the word's end, less
the line up to the end of the word before it, less the lone space Clay adds), measured from the line's start or, in a
long line, from a word at most 256 bytes back. The words of a line add up to what `nk_ui_text_clamp` and the
renderer measure for it.

Wrapped text keeps its wrapped height when its page is overfull. Clay took a paragraph's minimum height before
wrapping, as one line, so compressing an overfull column squeezed the paragraph back and the next block painted over
its later lines; the vendored `clay.h` carries the wrapped height into the minimum (`third_party/clay/PROVENANCE.md`).
Clay's layout errors are for developers: the launcher writes each one to the developer log (`ui` source) with Clay's
own text, and the in-game overlay writes it to stderr, once when it starts and not again while it repeats frame after
frame. A duplicate id, a floating element without its parent, or a percentage over 100% still lays out a whole frame,
so that frame is drawn. Every other error leaves elements, words or memory out of the layout; that frame is dropped and
the window shows the ground colour until a whole frame comes back (`test_layout_errors_reported` in
`tests/native/test_nk_ui_core.c`).

`test_every_screen_fits_the_window` in `tests/native/test_player_screens.c` lays every screen out in launcher windows
of 1280 x 720, 960 x 540, 1366 x 705 and 853 x 480 pixels and fails when a box or text run leaves the window or text
is painted over.

### Motion

Tokens are in seconds (`NK_UI_MOTION_*`): fast 0.12 (focus, press), base 0.20 (toast, tab rule), slow 0.50 (art
crossfade); art appears after a 0.6 s rest. The preview-music tokens (after 1.1 s, fading in over 1.0 s and out
over 0.45 s) are reserved: the player has no preview music. The waves step at 30 Hz. Reduce motion (the setting, or
with it on Automatic, Windows' Animation effects turned off) makes every duration a cut and holds the waves on one
frame. Nothing ever needs motion to be understood.

The art rest: once focus has rested on a game's card for 0.6 s (`NK_UI_MOTION_ART_REST`), the player reads that
disc's PIC1 and it fades in behind the library over slow (`nk_ui_backdrop`), over the waves and under a scrim of the
page colour. Moving on fades it out, so browsing past cards reads no pictures and changes nothing behind them. The
scrim is solved for every step of the fade so that a white picture over the brightest waves stays as dark as the
waves' own limit, and every text colour keeps its contrast (`nk_ui_backdrop_scrim`, `test_backdrop_rests_and_fades`).
The demos (their art is the project's own drawing), the find box and Reduce motion leave it out: it is decoration.

The launcher's loop asks nk_ui how long it may wait for input (`nk_ui_wait_s`, `test_wait_follows_motion`): one
display frame while something moves, until the waves' next 30 Hz step while they play, else 50 ms. It used to wait
50 ms whenever nothing scrolled, so the waves stepped at 20 Hz.

Moves and colour changes are Clay transitions: a container's `NkUiStyle.transition` names what eases (its colours,
its box) and `transition_s` over how long, with Clay's ease-out, from its old value to its new one. A container's
first frame and its last are cuts. Under Reduce motion nk_ui leaves the transition out, so the change is a cut.

- The focus ring eases to the newly focused control over fast (see Focus).
- Hover, press and focus fills of buttons, game cards, the add card and tab labels ease over fast
  (`test_hover_fill_eases`).
- The 3 px tab rule is one element in the active tab's slot and slides to a newly active tab over base
  (`test_tab_rule_slides`).
- A disc's picture that arrives on a card already on screen, read after the card was drawn, or that changes in
  place, fades in over slow. One that comes with its card, on a new page, is there at once (`test_art_fades_in`).
  A picture is an nk_ui custom element, which Clay transitions cannot fade, so nk_ui eases the alpha it draws it
  with, on Clay's ease-out curve.

Clay counts a frame's time into a transition from the frame it starts on. The time since the last frame was idle when
nothing moved then, so a transition starting now is given one display frame of it (`NK_UI_MOTION_FRAME`) and starts
its course at the beginning. nk_ui gives Clay the layout's size at the start of every frame, as Clay's lifecycle asks:
Clay holds moves back on a frame whose size changed, and given the size only on a resize it took every frame for one.
The in-game overlay follows the player's Reduce motion choice too: the launcher exports it as `SR_REDUCE_MOTION`
(`on`, `off`, or `system` for Automatic, which the overlay resolves against Windows' setting as it draws), and the
overlay keeps drawing while a transition eases. The goldens are stills at a clock that never moves, so their contexts
cut motion too; the motion itself is tested at a moving clock in `tests/native/test_nk_ui_core.c`.

### Background waves

Four project-authored ribbons drift behind every launcher screen (`nk_ui_waves_element`, drawn first and outside
the layout), never in game. Their combined alpha is capped so every text colour keeps WCAG AA over the brightest
point. The SDL_Renderer and Vulkan backends both draw them through the CPU reference at a third of the window
size, scaled up with a linear filter, and keep the frame until the command changes.

## Components

| Component | Call | Spec |
| --- | --- | --- |
| Button | `nk_ui_button(ui, id, label, flags)` | label centred, `LABEL` caps; 44 px (`_LARGE` 52 with `LABEL_L`, `_SMALL` 34 with `LABEL_S`); fits its label (`_GROW` fills); primary = accent fill, ink text; secondary = clear fill, `line_strong` edge, paper text; danger = `error` text; disabled = `dim` text, `line` edge (primary keeps a `surface_high` fill) |
| Game card | `nk_ui_tile(ui, id, &spec)` | `surface_raised`, `line` edge, radius 12; art panel flush with the top (tint per title, badge top-left; the disc's own `ICON0.PNG` once read, a showcase demo's picture, or the "ART FROM YOUR DISC" placeholder); name in `TILE_TITLE` (two lines); meta row: marker + status word in the status colour, disc ID right in `dim`. The badge says YOUR DISC, or BETA / UNOFFICIAL for a disc whose own ID is not a retail disc ID (a beta or prototype; its disc ID is the X... key Nakagawa derived from its contents, [Betas and other non-retail discs](YOUR_OWN_GAMES.md#betas-and-other-non-retail-discs)) |
| Add card | `nk_ui_add_tile` | ground fill, `line_strong` edge; 44 px outlined plus in accent, centred name and one-line note |
| Hero | `nk_ui_hero` | the title's art panel at 480:272 with a badge caption and the size note; the disc's `PIC1.PNG` fills it, else its `ICON0.PNG` sits centred, else the placeholder |
| Status pill | `nk_ui_status_pill` | 26 px, `surface_high`, `line_strong`, radius 4, `LABEL_S` in the status colour, marker first |
| Tag | `nk_ui_tag` | as the pill, `muted` text (YOUR DISC, BETA / UNOFFICIAL, SHOWCASE DEMO) |
| Badge | `nk_ui_badge` | 20 px on `art_scrim`, radius 4, `MICRO` |
| Eyebrow | `nk_ui_eyebrow` | `EYEBROW`; names a section or a card |
| Card | `nk_ui_card_begin` | `surface_raised`, `line`, radius 12, 16/20 px padding |
| Top bar | `nk_ui_top_bar` | N1 mark (28 px) and wordmark; section tabs in `LABEL` with a 3 px accent rule under the active one; input chip on the right (dot in `ready` for a controller) |
| Prompt bar | `nk_ui_prompt_bar` | `surface_raised`; an optional hint in `CAPTION` `muted` on the left, used only for a screen-specific tip (never a standing disclaimer); glyph (24 px, current prompt style) + `LABEL` per action on the right, 18 px apart |
| Vector art | `nk_ui_shapes` | rects, circles and convex polygons in a view box, fitted and centred; used for the N1 mark, the showcase demos' pictures and icons |

Prompt glyphs come from `nk_ui_prompt_glyph`, and the whole bar shows one set chosen from the active device
(`nk_ui_input_prompt_style`), never a mix. The player draws Kenney's CC0 "Input Prompts" glyphs
(`third_party/kenney/`, embedded by `tools/gen_ui_assets.py`), tinted `paper`; any glyph that is missing or fails to
decode falls back to the project-drawn geometric form, which tests and SDL-free builds also use:

- **Keyboard and mouse** (no pad connected, or a key or click was the last input): ENTER, ESC and the other keys as
  keycaps, the arrow keys for MOVE, and the top bar's chip says KEYBOARD AND MOUSE.
- **Controller** (a pad connected or was the last used): its D-pad for MOVE and its own face buttons, picked from the
  label SDL reports on the south button, never from the pad's name: A south gives the Xbox-style set, B south (A
  east) the Switch-style set, a cross the PlayStation-style set, and anything else the neutral position dots.
  Confirm and back follow position (south confirms), so a pad with A east shows B for confirm. The chip names the
  pad the way a player would: XBOX-STYLE CONTROLLER, NINTENDO-STYLE CONTROLLER, PLAYSTATION-STYLE CONTROLLER, or
  CONTROLLER for the neutral set.
- A pad that connects switches the set at once; when it disconnects the set falls back to another connected pad or
  to the keyboard. Pointer motion alone never switches.

Disc art is read on a background thread from the user's own disc image, one disc at a time, and decoded with
stb_image (`third_party/stb/`) inside fixed limits (512 x 512 for `ICON0.PNG`, 1024 x 1024 for `PIC1.PNG`, 1 MiB
encoded). It lives in memory only: nothing is written to disk or kept in the repository, and a disc without art, or
art that fails the checks, keeps the placeholder. A picture is drawn by nk_ui itself (`nk_ui_image_art`): a cover
picture fills the art panel, cropped, and a contain picture (the icon) sits whole over it, both snapped to whole
device pixels. Clay's image elements hold one picture each, cannot crop to fill, and keep an aspect ratio only by
deriving the height from the width, so they would not make this simpler.

Optional launcher sounds (Kenney's CC0 "Interface Sounds": a tick when focus moves, a chime on confirm, a click on
back) are off by default; **UI sounds** in Settings (`ui_sounds` in `settings.json`) turns them on. While they are off
no audio device is opened, and they never play while a game runs.

Optional launcher music ([#2207](https://github.com/Jstar269/nakagawa-recomp/issues/2207)) is off by default too:
**Launcher music** in Settings (`launcher_music` in `settings.json`) turns it on, and **Music volume** sets its own
level (`launcher_music_volume`, 0 to 100%, 80% by default; the track is mastered at about -22 LUFS, so 100% is that
level). Like the menu sounds it does not follow **Game volume**, which is the game's. The track is "Chill lofi
inspired" by omfgdude (CC0, from OpenGameArt; [`assets/music/launcher/`](../assets/music/launcher/PROVENANCE.md)). It
ships beside the player and is decoded with the vendored stb_vorbis; nothing is downloaded. It plays only while the
launcher shows (the library, Settings, a game's page, the launcher's cards): it fades in over 1.5 s, and loops from the
recording's first to its last sound, so the loop has no gap and no click. When a game starts it fades out over 0.5 s
and the launcher lets go of the audio device before the game's process starts, so it never mixes into the game's
sound. A game started while the track is still decoding stops the decode, and the launch waits for its worker to
finish first. The decoded track (about 22 MB) is not kept while a game runs: once the launcher is back and the music
would be heard (the window has focus and Music volume is above 0), the track is decoded again on its worker (about
half a second) and fades in from where it stopped. It also fades out and lets go
of the device while the launcher window does not have focus (a launcher that opens without focus waits for it), and
at Music volume 0. Turning the setting off fades it out over 50 ms. If the audio device stops taking data during a
fade, the launcher closes it at the fade's length plus 0.25 s and logs `LAUNCHER_MUSIC_RELEASE_TIMEOUT` with the time
it waited. While the setting is off nothing is read and no audio device is opened. A track that is missing, cannot
be decoded, is a file larger than 64 MB or would decode to more than 64 MB of samples, or an audio device that cannot
be opened, is named on the setting's row (On: file
missing, On: file damaged, On: no sound device) and in the log (`LAUNCHER_MUSIC_MISSING`, `LAUNCHER_MUSIC_DAMAGED`,
`LAUNCHER_MUSIC_NO_DEVICE`); the launcher stays silent and carries on.

A game's page plays that game's own startup clip, the `PSP_GAME/SND0.AT3` a PSP's home menu plays for it. **Game page
music** in Settings (`launcher_game_music` in `settings.json`, on by default) turns it off, and it plays at the **Music
volume**. The library list stays silent. When a game's page opens, the clip is read from that game's own disc image
through the ISO reader (a file larger than 4 MiB is refused unread), decoded in memory on a worker thread with the
runtime's ATRAC3 / ATRAC3+ decoder (`src/rt/atrac3p/`, FFmpeg n4.4; `src/player/ui/player_ui_at3.c` reads the RIFF
format and data chunks), and played: it fades in over 0.3 s and loops from its first to its last sound. Leaving the
page or losing focus fades it out over 0.3 s and lets go of the audio device; starting a game stops a read still
running and fades the clip out before the game's process starts, and nothing of it is kept while the game runs. While
a clip loads or plays the launcher music pauses, and it picks up where it stopped afterwards. The clip is never written
to disk, cached, uploaded or logged. A disc without `SND0.AT3` is silent with no message; a clip that cannot be read or
decoded, decodes to silence or would decode to more than 32 MiB of samples, or no audio device, is silent for that game
and named once in the log (`GAME_MUSIC_DAMAGED`, `GAME_MUSIC_NO_DEVICE`) by its reason, never its path.

Only actions the screen handles appear in its prompt bar.

## Screens

### Library (C5, built)

Top bar with LIBRARY, ABOUT and SETTINGS tabs (SETTINGS opens the Settings view, whose page shows that tab active;
Start or F1, the menu prompt, opens it too); `LIBRARY` in `DISPLAY` with the count of the user's games in `CAPTION` beside it ("7 titles"); a pager
(small PREVIOUS and NEXT buttons around "PAGE 1 OF 2") on the right of that row when the grid spans pages; the card
grid of the user's games, ending with the add card ("Add a disc image you own", "Choose an .iso file"); prompt bar with MOVE, OPEN and
SETTINGS and no corner text. Past 1600 px of content the side margins grow, so a wide window centres the heading,
the grid and TEST THIS PC in one column, as Settings centres its cards. The first card, never the pager, has the first
focus. Page Down and Page Up, the pad's
right and left triggers and the mouse wheel turn the page too; a focused card hands focus to the card in the same place
on the new page (the last one when that page is shorter). Home and End go to the first game and to the last one, on
its page (`NK_UI_EV_EDGE`). A small SORT button in the heading row names the sort and steps to the next: Recently
played (the default: the last played first, then the games never played in the order they were added), Name A to Z,
and Recently added (the last added first). The sort is saved as `library_sort`, and the grid starts again from its
first page.

FIND, beside it, opens the find box; so do the screen's Options action (the pad's west button, Space) and, from a
hardware keyboard, "/" or Ctrl+F. The box takes TEST THIS PC's room: a field showing what is typed ("Type a name or
disc ID" until then) and, 4 px under it, the player's shared on-screen keyboard (`src/player/ui/ui_keyboard.c`, the
nickname editor's too). Its keys are small controls (34 px at 100% text) in rows 4 px apart, the last 8 px below
the others: digits, three QWERTY rows with `'`, `,`, `.` and `-`, a row of symbols, and SHIFT, SPACE, DELETE, CANCEL
and DONE. The letters are lower case until SHIFT, which also gives the other keys a second character
(`$ % ^ = / \ ; " | ~` on the digits, `:` on `'`, ``< > ` `` on `, . -`, `[ ] { }` on `( ) _ +`), so the two layers
type every printable ASCII character, as a RetroAchievements password may need. The field is a key's height, so a row of cards still fits under
the box at 1280 x 720; with larger text the grid scrolls under it.
The box takes printable characters, up to 63 bytes. The grid shows the games whose name or disc ID holds what is
typed, ignoring case, without the add card or the demos; the count reads "3 of 40 titles", and no match says "No
games match". With a controller the keyboard's shortcuts work while the box is open: X deletes, Y types a space, L
or R is SHIFT and Start is DONE (instead of the next tab and Settings). DONE moves focus to the first match, and the
box stays open. A hardware keyboard types straight in, through the same filter and limit (SDL's text input is on
while the box is open, so an input method works too), Backspace deletes, and letters that are launcher shortcuts
(O, S, D, Q, E) are typed instead. Opened from the keyboard, focus is on DONE, so Enter moves to the first match; from
a controller or the mouse, on q. CANCEL, Esc or B clears and closes the box and focus returns to FIND. The prompt bar
reads MOVE, SELECT, DELETE, DONE and CLEAR while it is open.

The sorts are those three. Find is one
model for every input: a Find action (a button, and a controller button) opens a find box that filters the library
as you type, with an on-screen keyboard for controllers, and Esc or B clears and closes it. Typing a letter to open it
would collide with the letter shortcuts, so "/" and Ctrl+F open it from a hardware keyboard and every letter shortcut stays (#1742). A library holds up to 4096 games; only the page shown reads
its disc icons and checks its packages, so a page of a large library costs what a small library does. Empty library: one centred 640 px card with a disc icon, `TITLE` "Add your first game", a `BODY`
line, the primary START SETUP WIZARD button (the default focus), CHOOSE A DISC IMAGE, and a caption about dropping an
.iso onto the window.

TEST THIS PC: the demos that come with Nakagawa (the showcase packages beside the player) never join the grid. They
sit in their own row under it, or under the empty-library card while the user has no games, so the setup wizard is
offered on a first start even when the demos are there. The row's first grid column holds the `EYEBROW` "TEST THIS
PC" and one `CAPTION` line ("Play a demo that comes with Nakagawa to check graphics, sound and controls."); each
demo is a compact 72 px card one grid column wide (its project picture on the left, the name in `TILE_TITLE` over
two lines at most, and the status in `LABEL_S`). The cards are focusable, never the default focus, and open the
demo's title page like a game card. The grid gives up rows to fit the row; where the empty-library card and the row
do not both fit (the 960 x 540 minimum), the card leaves out its icon.

Disc check (#1731): each time the user adds a disc, its image is hashed after the add, on a worker, one disc at a
time, and compared with the disc list the user imported ([COMPATIBILITY.md](COMPATIBILITY.md#whole-library)); so is
any other image of its disc ID not hashed yet. None starts while a game starts or runs or a package builds. While
one runs, the heading row carries a `CAPTION` `muted` line, "Checking disc image: 40%" ("Checking disc images: 40%,
2 more" with more waiting), and a small CANCEL that stops it and the ones waiting (it stays while the check stops);
the games stay, unchecked. A check that could not finish leaves a `warning` line ("A disc image could not be
checked", "The disc list could not be read", or "Some disc images were not checked" when too many wait) with HIDE,
and the reason goes to the developer log. The line stays until HIDE; a later check shows its progress meanwhile.
Neither button is ever the default focus. The progress is a number, not an animation, and it is on that line
only: the card's badge, YOUR DISC until then, changes once, to VERIFIED RETAIL, MODIFIED DISC or UNKNOWN DUMP (a
beta's BETA / UNOFFICIAL stays over UNKNOWN DUMP). A disc added again reads YOUR DISC until its new check ends,
since its file may hold other bytes now.

### Title detail (C5, built)

Aside: the hero, then a PACKAGE card (eyebrow, status pill, the status detail in `BODY_S` `muted`, and any input
note in an inset `info` box). The detail follows the package state (ready, checking, building, check failed,
missing, starting, running), never the primary action: while its game runs, a ready package says "Running now.
Stop the game to rebuild or change settings." The input note about decrypted files shows only while the package
still needs them. Main column: eyebrow LIBRARY › TITLE, the name in `TITLE_XL`, the disc ID in `DATA`, the status
pill and tag (YOUR DISC, BETA / UNOFFICIAL or the disc check's word, as on the card), then for a checked disc
one `BODY_S` `muted` sentence on what the check found (for UNKNOWN DUMP: not in the disc list, which doesn't
mean bad, only not shown to be retail, and its disc ID isn't one the list names), then a PICTURE card when the last run of this game fell back to a
slower drawing path (#2151): a DRAWN IN SOFTWARE pill in the brand blue, as a fact and not a warning, one `BODY_S`
`paper` paragraph naming the renderer that did not start and what that costs (Internal resolution has no effect, the
game may run slower, a graphics driver update may help), and a small Hide button, never the default focus, that hides
that fallback's notice for the rest of the session; then, when the game starts in Authentic and at least one of the
18 PSP fonts is a stand-in or a replacement font, a FONTS card: one `BODY_S` `paper` sentence ("Text uses stand-in
fonts. For the original look, get the fonts from your PSP.") and two 44 px secondary buttons, never the default focus: GET
FONTS FROM MY PSP opens **Settings > PSP fonts** and looks for a connected PSP there (Back returns to the game's
page), and Hide hides the card for that game for good (`settings.json` `"font_note_hidden"`). It never blocks PLAY,
and Enhanced and Ace pages have no such card; then one action row on the column's left edge with every button 52 px (the primary action, PLAY when
the package is ready, STARTING… while the launch starts, STOP GAME while a game runs, then SAVES (the
[Saves](#saves-built) dialog) and BACK), and the WHAT TO EXPECT card. Its eyebrow carries a player tier chip (PERFECT and PLAYABLE `ready`, IN-GAME
and MENU `info`, TITLE SCREEN `warning`, WON'T BOOT `error`; no chip for a title no one has tried). Below it come
one or two plain sentences on what to expect on this PC in `BODY_S` `paper`, then the known issues in `muted`
when there are any. The tier comes from the title's own player record, or else from its `docs/COMPATIBILITY.md`
status word through the table in [Status words](COMPATIBILITY.md#status-words). The status word itself describes
the project's test route and read as a claim about the user's game (#1297), so only developer mode names it, in a
`FOOTNOTE`. A title with an enhancement package then has an ENHANCEMENTS card (eyebrow and a tag, OPTIONAL for a
package the maintainer released and EXPERIMENTAL for any other; [ENHANCEMENT_CONTRACT.md](ENHANCEMENT_CONTRACT.md#910-experimental-or-released)
section 9.10). Only a package found in the source tree's `assets/enhancements/` can read OPTIONAL, never one from
the user's data folder, which is tagged EXPERIMENTAL whatever it declares. The card holds the switch 60 FPS GAME
LOGIC: ON or OFF (`PRIMARY` while on, never the default focus, disabled while a game starts or runs or the title's
package builds), then a `BODY_S` line saying it is an optional enhancement made for this game and that off plays the
original game as released ([ENHANCEMENT_CONTRACT.md](ENHANCEMENT_CONTRACT.md#9-declarative-frame-rate-rescale-packages-60-hz-logic)
section 9). When the switch is saved on but the package is gone, the card stays and says why in `error`, and the
primary action reads ENHANCEMENT UNAVAILABLE. Under the cards, one row that wraps at a larger text size: ADD TO
STEAM (the [Add to Steam](#add-to-steam-built) dialog), REPORT A PROBLEM, and REMOVE FROM LIBRARY (a `DANGER`
button). ADD TO STEAM and REMOVE FROM LIBRARY are for a disc the user added and are left off the showcase demos;
ADD TO STEAM is also left off a disc whose ID is not letters, digits, `-` and `_`, which a shortcut cannot name.
REMOVE FROM LIBRARY is also left off while that game starts or runs, a package builds or a disc is being copied.
Prompt bar: MOVE, SELECT, BACK.

### Stop game (built)

STOP GAME on the title page asks first, as quitting with a game running does. A flow card, 640 px wide, opened over
the title page: the eyebrow STOP GAME with a `warning` triangle, then "Stop *game*?" in `TITLE` (the game that runs),
and one `BODY_S` `muted` line: "The game closes now, and anything since its last save is lost." KEEP PLAYING has the
first focus and Back closes the card as it does; STOP GAME is a `DANGER` button, never the default focus. The card
closes by itself if the game ends meanwhile. Prompt bar: MOVE, SELECT, KEEP PLAYING. The no-new-frames card's own
STOP GAME is already that choice, so it stops the game without a second question.

### Remove from library (built)

A flow card, 640 px wide, opened over the title page. Cancel has the first focus, and Back closes it, as Cancel
does. The eyebrow is REMOVE FROM LIBRARY with a `warning` triangle, then "Remove *title*?" in `TITLE`. A box lists
what Nakagawa built for the game, each item with its size: the built package, the build cache (packages and
bring-up), and the disc files Nakagawa copied. A `warning` FREES line gives the total. A `ready` line says the disc
image on the user's PC is not touched. The game's saves are the copied disc's Memory Stick folder and its
per-user save folder. They get their own box, unticked every time the dialog opens; under it, "Your saves stay." in
`muted`, or in `error` once the box is ticked, "Your saves for this game will be deleted too. A backup of them is
kept first: Saves can restore it if you add the game again." REMOVE backs those folders up before it deletes
anything, into the backup folder [Saves](#saves-built) lists, which a removal leaves alone; when a backup can't be
written, nothing is removed and the error card says why. A game with nothing
built and no saves says so. A second box, also unticked every time, reads "Also delete the decrypted files and disc
copy (*size*)" (#2003): the decrypted files the disc check wrote (`titles/<DISC_ID>/decrypted`, the folder where a
user may also put their own) and the disc image Nakagawa expanded or converted (`<user data>/discs/<DISC_ID>`).
Under it, "They stay, so adding this game again is quicker." in `muted`, or in `error` once ticked, "They go too,
with any decrypted files you added yourself. Your disc image stays." REMOVE (`DANGER`) removes the title, deletes
those folders (the saves and the disc copy only when their boxes are ticked), and returns to the library. Focus lands on the tile that took the removed title's place, or on the add
card when it was the last, and the page turns to it. The folders are found from the disc ID under the player's own
roots, never from a path stored in the library. The disc image on the user's PC and the shared manifests are
never on the list. Those folders and saves are the disc ID's, so while another image with the same
disc ID stays in the library (a retail disc beside its fan translation), the boxes are left out and the lead says
what Nakagawa built for it and its saves stay, because the other image uses them; only the record goes. Prompt bar:
MOVE, SELECT, CANCEL.

### Saves (built)

SAVES on the title page opens a flow card, 640 px wide, over it (#1883). The eyebrow is SAVES with an
`accent_dark` diamond, then the game's name in `TITLE` and one `BODY_S` `muted` line: "Its saves and game data take
*size*. Backups are copies kept on this PC.", or, with nothing saved yet, "No saves yet. Add a save copies one in
from a folder you pick." One row of buttons follows, wrapping at a larger text size: BACK UP SAVES (`PRIMARY`, off
with no saves), ADD A SAVE, OPEN SAVE FOLDER (the folder the old title-page button opened) and BACK. A `BODY_S` line
under them says what the last action did, in `ready` when it worked. A `LABEL_S` `muted` line counts the backups
("BACKUPS 1-4 OF 9, NEWEST FIRST", or NO BACKUPS YET), then a box lists four at a time, each row its date and time in
`paper`, what made it (Backed up, Before a restore, Before adding a save, Before a removal) and its size in `DATA`
`muted`, and a small RESTORE button that is never the default focus. NEWER and OLDER under the box turn the page.

RESTORE asks first, in the same card: "Restore the backup from *date*?" in `BODY` `paper`, "Your saves now are
backed up first and kept, so you can go back to them." in `muted`, then CANCEL (first focus) and RESTORE
(`PRIMARY`). ADD A SAVE opens the system's folder picker; the folder picked is a save's own folder, the one holding
its `PARAM.SFO` inside a `SAVEDATA` folder, and it is copied into the game's `PSP/SAVEDATA`. A save of the same name
already there asks "A save named *name* is already here. Replace it?" with CANCEL (first focus) and REPLACE
(`DANGER`). Back answers a question with Cancel; with no question up, Back closes the card.

Every change backs the saves up first. A restore checks the whole backup before it touches anything, writes it into
a new folder beside the save folder, and only then renames the save folder to `<folder>-backup-<date>` and puts the
new one in its place; any failure before that swap leaves the saves as they were, and a failed swap puts the old
folder back. A failure shows the error card ("Your saves were not backed up", "Your saves were not restored" or "The
save was not added") with the reason in plain words, and BACK returns to the card. While the game starts or runs, every
button that changes the saves is off and a `warning` line says "Stop the game first: it uses its save folder while it
runs." The save folder is `<saves>/<DISC_ID>`, the game's Memory Stick; backups are store-only ZIP files in
`<user data>/titles/<DISC_ID>/save-backups`, named `<DISC_ID>-saves-<date>-<time>.zip` with a `-before-restore`,
`-before-add` or `-before-remove` ending for the automatic ones. Nothing is uploaded and nothing is deleted
automatically. Prompt bar: MOVE, SELECT, BACK.

### Add to Steam (built)

ADD TO STEAM on the title page opens a flow card, 640 px wide, over it (#2152). It finds Steam and its accounts on
this PC when it opens and writes nothing until ADD TO STEAM is chosen. The eyebrow is ADD TO STEAM with an
`accent_dark` diamond, then the game's name in `TITLE` and one `BODY_S` `muted` line: "Adds a shortcut to your
Steam library that starts this game directly, for Steam Input and Big Picture. Nakagawa changes only Steam's list
of non-Steam games and shortcut artwork on this PC, after saving a copy of it, and downloads nothing." A box follows: STARTS in
`LABEL_S` `muted` over the command in `DATA` (the program's file name and its launch options, such as
`nakagawa_player.exe --launch-title=ULUS10041`), and, for a single account, STEAM ACCOUNT over its name. With
several accounts, WHICH STEAM ACCOUNT? in `LABEL_S` `muted` heads a row of small buttons, one per account, named
with the account's Steam name (or "Account *ID*" when Steam has no name for it); the chosen one is `PRIMARY` and
reads "CHOSEN: *name*", so colour is not the only sign. One `BODY_S` `muted` line says "Exit Steam first: it
rewrites this list when it exits."; a portable copy adds that moving its folder needs Add to Steam again here. Then
ADD TO STEAM (`PRIMARY`, the first focus: it only adds, after a backup) and BACK.

A `BODY_S` line under the buttons says what the last add did and what to do, in `ready` for an added or updated
shortcut and `warning` for a refusal: added (start Steam to find it; no restart is needed; where the copy of the
list from before is), updated (the game's shortcut from another Nakagawa folder now starts this one; its play time
and collections stay), already there (nothing changed), Steam running (nothing changed; exit Steam, then choose
Add to Steam again; no restart follows), the list unreadable (left as it was), or no verified backup (nothing
changed). With no Steam, no signed-in account, or a system where Nakagawa cannot find Steam itself, the card shows
only that sentence in `warning` and BACK. Back closes the card. Prompt bar: MOVE, SELECT, BACK.

### About (built)

Reached from the top bar's ABOUT tab (L / R or Q / E switch tabs; Back returns to the screen it was opened from). A centred
880 px column:

- the N1 mark at 72 px beside `NAKAGAWA RECOMP` in `DISPLAY_XL`, with the version line under it in `LABEL_L`
  accent: `v`, then `NK_PLAYER_VERSION`, then the build date as `(MM-DD-YYYY)`, for example `v0.0.2 (11-07-2026)`;
- a `BODY_L` `muted` line saying what it is (a PSP static recompiler: your own game, recompiled to run natively),
  followed by the privacy promise;
- a LICENCE card (GNU GPL version 3, and a THIRD-PARTY NOTICES button that opens the existing licences screen) beside
  a CREDITS card (the upstream lineage in three lines, pointing at `CREDITS.md` and `NOTICE.md`);
- the non-affiliation notice ending "No game data is included.", the project link in `DATA`, and the short source
  commit as a `FOOTNOTE`.

The version is written once, `NK_PLAYER_VERSION` in `src/player/ui/player_ui_nk.h`. The date is the build's
`SOURCE_DATE_EPOCH` when set, else the source commit's time (Makefile and CMake both), formatted in UTC as
MM-DD-YYYY; without either the line reads `(dev build)`. No tag or release is implied by it.

### Game found (built)

The card a disc gets when it is added from the Library ([#1302](https://github.com/Jstar269/nakagawa-recomp/issues/1302)),
in the dialog card the wizard and error screens use, 820 px wide. A status pill (GAME FOUND `ready`, NOT TESTED YET
`info`, CAN'T PLAY YET or NOT ON THE LIST YET `error`) with the disc ID and YOUR DISC (or BETA / UNOFFICIAL) tags; the game's name in
`TITLE` ("PSP game" when the disc has none); one `BODY` lead saying what Nakagawa can do with it; then the checks in an
inset box, one row each: the status word in its colour (132 px), what was checked (124 px), and the sentence in
`muted`, two lines of it from a 680 px tall window and one below that. Every row is a player sentence from
`player_preflight_plain_message`, never the developer message, which stays in the log. Only a check that keeps the disc
out of the library is `error`; anything still to do (not built yet, PSP fonts missing) is `warning`, so nothing red
sits beside ADD TO LIBRARY. A disc this version cannot use is not offered to the library: its reason replaces the
lead's second line and the card offers CHOOSE ANOTHER DISC and BACK. While the disc is read, the card shows its path,
an indeterminate bar and CANCEL. The whole card fits the 960 x 540 minimum window.

A disc whose program files are encrypted, and that only needs decrypted copies the user makes from their own disc,
is not a dead end ([#308](https://github.com/Jstar269/nakagawa-recomp/issues/308)). Its pill reads NEEDS YOUR FILES
(`warning` triangle), the lead says the game needs files from the user's own disc, and a FILES TO ADD box takes the
place of the check rows: one sentence that Nakagawa couldn't decrypt some files and can use decrypted copies, one row per
file (NEEDED in `warning` or REPLACE in `error`, the file name to put in the folder in `DATA`, and "copy of" the disc
file in `muted`), a count line, a REPLACE sentence when a copy there is not a working program file, and the folder
in `DATA`. Below a 680 px tall window the box is compact: two rows, two lines of the sentence, one of the folder. The
buttons are CHECK AGAIN (primary, the first focus), OPEN FOLDER (it makes the folder first) and BACK. Check again
reads the same disc image again, with no file picker and no new import, and the card becomes NOT TESTED YET with ADD
TO LIBRARY once every file is in place. It never asks for a key and never says how to decrypt. The wizard's Verify
step shows the same box, with CHECK AGAIN, OPEN FOLDER, CHOOSE A DIFFERENT ONE and CANCEL, and the title page shows
it under the action row while BUILD PACKAGE waits for the files, with CHECK AGAIN and OPEN FOLDER.

### Settings (C6)

Built now (#1302): one page under the top bar with SETTINGS active, two cards of named rows: GAME (resolution, VSync,
fullscreen, scaling and Play mode) and LAUNCHER AND SOUND (game volume with - and +, launcher fullscreen,
accessibility, menu sounds, controller settings, CHECK FOR UPDATES, launcher music and music volume with - and +,
then the controller and the saves folder as captions; CHECK FOR UPDATES opens the Updates card and asks GitHub, see
[`SETUP.md`](SETUP.md#network-use)). Under them DONE (primary: saves and returns to the Library), ONLINE AND DEVELOPER,
THIRD-PARTY NOTICES and BUILD TOOLS AND CACHE (#1306: the share libraries switch, CLEAR SHARED LIBRARY CACHE and
REMOVE BUILD TOOLS); the online options, the notices, build tools and cache and the removal check are dialog cards.
ACCESSIBILITY (#1739) is a dialog card of four settings, each a button that steps through its values beside a
sentence saying what it does and what Windows asks for now: INTERFACE SIZE (Automatic follows the display scale, or
100, 125, 150 or 200%; a larger size needs a window large enough for the 1280 x 720 layout at that size), TEXT SIZE
(Standard, Large or Larger: 100, 125 or 150% type, with controls and the layout's spacing kept, in any window), HIGH
CONTRAST and REDUCE MOTION (Automatic, On or Off; Automatic follows Windows' contrast themes and Animation effects,
read every frame, so a change there applies at once). The four buttons share one column; at a larger text size it is
as wide as the widest label, and the sentences go under the buttons when beside it a sentence would be cut.
PLAY MODE opens Settings > Enhanced (the play modes, docs/ENHANCEMENT_CONTRACT.md section 10). The library's global
GRAPHICS PRESET and PLAY MODE remain pinned above six category tabs: PICTURE, MOTION, SOUND, CONTROLS, ON-SCREEN and
ADVANCED. L and R on a controller, or Q and E on a keyboard, switch tabs; clicking a tab selects it. The explicit
part-group table covers every PlayerEnhancedPart once, with the existing part IDs, values and settings keys intact.
PICTURE contains TEXTURE FILTERING; WIDESCREEN; SHARPER ENLARGING; TEXTURE PACK; FULL 32-BIT COLOUR; SUPERSAMPLING;
SMOOTH FAR TEXTURES; TEXTURE SCALING; SMART 2D FILTER; CRISP SYSTEM TEXT. MOTION contains SMOOTH MOTION and the
existing global FRAME PACING control. SOUND contains CLEAN PITCHED SOUND and UNCLIPPED SOUND. CONTROLS contains
STICK DEAD ZONE. ON-SCREEN contains SPEED READOUT and GAME BADGE. ADVANCED contains FINER PATCHES, WIDER CULLING,
LOADING SPEED, LATE PAD SAMPLING and SOFT SHADOWS. Loading speed offers Automatic, Short waits (Ace) and Off.
Automatic works in Enhanced or Ace without an Ace package. Older game packages need rebuilding to use Automatic.
Short waits requires Ace and a package with declared loading waits. On a game's page TEXTURE PACK FOLDER and, with a folder chosen,
REMOVE TEXTURE PACK are the last rows of Picture.
Each option is one sentence-case row, as high as a control (44 px), and one focus stop: its name on the left, with an
ACE ONLY tag beside the name where it applies, and on the right every value it can take, each in its own small box
(34 px). The current value is filled with the accent, with `ink` text, when it is on or chosen (16X, ON, FSR 1).
A current OFF is never filled with the accent: it is marked by the raised box (`surface_high`) with a
`selected_neutral_edge` edge and `paper` text, so a tab whose options are off is not a column of accent boxes. That
edge stands 4.2:1 from the list's ground and 3.5:1 from the box, so the mark is told from an unselected value's
hairline (`line`, 1.2:1) without the accent. A dimmed row's mark has the weaker `selected_neutral_edge_dimmed`
(3.1:1 on the ground), so that an inert row among live ones still reads as dimmed. Against its own box fill
(`surface_high`) that edge is 2.6:1, under 3:1: accepted because a dimmed row is inert, so its mark shows a value
and offers nothing to act on. Up and Down move row by row: Down from PLAY MODE lands on the active tab, Down from
a tab on its first row, Down from the last row on BACK, and Up goes back the same way. The rows name those
neighbours (`nk_ui_register_button_between`), because a full-width row and a small tab are not each other's nearest
by position, and every tab names the active tab's first row as its Down neighbour (`NkUiSection.down`): at Larger
text (150%) in a 1280 x 720 or 960 x 540 window the list keeps two rows, BACK under the strip was nearer to a tab
than the middle of the first row, and Down went from the tabs to BACK past every option. The d-pad walk is tested
at 100, 125 and 150% text (`test_enhanced_walk_at_every_text_size` in `tests/native/test_player_screens.c`, and
`test_next_ui_enhanced_dpad_walk_reaches_every_row_without_changing_values` in `tests/native/test_player_ui.py`).
Left and Right step the focused row's value, Confirm takes the next one, and a click picks any value; a
value box is a pointer target only (`NK_UI_BUTTON_POINTER_ONLY`), never a focus stop. GRAPHICS PRESET, PLAY MODE
and FRAME PACING show their value as `< value >` in one box, as does a row whose values no longer fit beside its
name at a larger text size. Category tabs are focusable. The panel on the right shows the focused row's name, its
value, its whole description and, for a dimmed row, the reason; with a tab or BACK focused it shows the tab's first
option. The panel is as tall as the list box beside it, whatever it says, so its text never pushes BACK down: text
taller than that scrolls inside the panel, with a thin thumb at its right edge as the list has, by the wheel, a drag,
Page Up and Page Down or the pad's triggers (a panel less one line a press, as on the developer log), while focus
stays on the row; another option's text starts at the panel's top. Every line wraps at its words, and a word wider
than the panel (a texture pack folder's name, a long compound) is broken across lines inside it, after a `/`, `\`,
`_`, `-` or `.` where one keeps at least half the line, else at the last letter that fits (`nk_ui_text_break`):
nothing is cut, ellipsized or drawn past the panel (`test_enhanced_panel_fits_its_content` in
`tests/native/test_player_screens.c`, at four window sizes, two UI scales and every text size). On a game's page, a row's first value is the library's, LIBRARY (value), in a raised box with muted text,
and a value of the game's own is marked as above. An option that cannot apply as the page stands stays in the walk, dimmed
and inert, with the reason in the panel; WIDESCREEN under SMOOTH MOTION marks OFF, the value that runs, and keeps
the choice it had for when SMOOTH MOTION is off. The list shows as many whole rows as the window leaves (four at
1280 x 720, a whole tab at 1920 x 1080), the same number on every tab so that changing tabs moves nothing. A longer
tab scrolls inside the list, with a thin thumb at the list's right edge, and the panel and BACK stay on screen. At a
larger text size the panel narrows so that the six tabs stay whole, and a page taller than the window scrolls as a
whole, as every page does. The title page's ENHANCEMENTS card
shows PLAY MODE for every game (LIBRARY DEFAULT, AUTHENTIC, ENHANCED, and ACE only for a game with a usable package;
`PRIMARY` while Enhanced or Ace is in effect, never the default focus), ENHANCED OPTIONS, which opens the same tabs
for that game alone, and, while Ace is in effect, Ace's 60 FPS switch in the same row. Under the buttons come what is
on and, as its own `BODY_S` `muted` paragraph, the last session. Both sentences wrap whole, however many parts are
on: the page body scrolls, so the card grows instead of ending either one in an ellipsis. A library card where
Enhanced or Ace is in effect carries an `ENHANCED` or `ACE` badge at the art's top right. In game, the pause menu's
caption names the mode and its parts and its Settings page has a live SPEED READOUT row; the launch notice (ENHANCED
or ACE, top right, 3 s, a cut with Reduce motion) and the readout (top left) are chips on the 16 px safe area. When
60 FPS logic falls behind with the HUD off, its red `60 FPS logic: <n>/s - SLOWED` warning is a chip under the readout,
never in the readout's corner. A second the host could not keep up with ends the readout in GPU LIMITED or PC LIMITED,
in `warning` (the rule is docs/ENHANCEMENT_CONTRACT.md section 10.3). The notice and the caption list the parts that
are on, joined by middle dots. A list too long for one line wraps between its parts, never inside one, and never ends
in an ellipsis: the notice takes up to eleven lines, each at most half the window wide (one line while the on-screen
keyboard is open), and the caption takes the lines its card has room for above RESUME. Where a list is longer than
that, its last line counts the parts it leaves out ("and 9 more"). With every part on, the notice shows them all in
every window, and the caption shows them all in the default 960 x 544 game window and any taller one
(`ace-ui-selftest`, `nk-ui-overlay-screens-selftest`).
GRAPHICS PRESET is the first row on Settings > Enhanced and each game's ENHANCED OPTIONS page (#2150). It steps
through LOW, MEDIUM, HIGH, ULTRA and HANDHELD. While the row has focus, the panel beside the list carries the
preset's note, which says where the preset is used and which Enhanced parts it turns on.
A preset is used only by Enhanced and Ace sessions, where its values take the place of the RESOLUTION, SCALING,
VSYNC and GAME FULLSCREEN rows and of up to three Enhanced parts' library values: CRISP SYSTEM TEXT, SHARPER
ENLARGING (the FSR 1 upscaler) and, under ULTRA, FULL 32-BIT COLOUR. Choosing one changes none of those rows or
parts, and an Authentic session always runs with the rows as the user set them, whatever the preset. The reason is
that a render scale above 1x is not host-only: every present commits the frame to guest VRAM, so a game that copies its
screen (a save picture) keeps the larger picture (`docs/ENHANCEMENT_CONTRACT.md` section 10.3, Pixels), and Authentic
launch output must not change with a preset. Editing one of the rows a preset sets makes it CUSTOM, which means the rows
apply in every session and the parts the preset set keep its values as the library's (below); GAME FULLSCREEN does so
only under HANDHELD, the one preset that sets it. While a preset is chosen, the page's notice line says that it is used
in Enhanced and Ace, names the parts it turns on and off, and says that Authentic uses the rows, in short words (`Ultra
is used in Enhanced and Ace, with crisp text, FSR 1 and full 32-bit colour on. Authentic uses your settings.`, the
longest, on one line at 960 x 540 and 853 x 480). The settings file keeps the preset's name beside the rows, and a file
without it, or with a name this build doesn't know, loads as CUSTOM. The values are the project's choice, and FSR 1's
cost is measured only on an RTX 3080 (`docs/SETUP.md`); FULL 32-BIT COLOUR's is not measured on any GPU. They live in
one table, `k_graphics_presets` in `src/player/player_state.c`:

| Preset | Resolution | Scaling | VSync | Game fullscreen | Crisp system text | Sharper enlarging | Full 32-bit colour |
| --- | --- | --- | --- | --- | --- | --- | --- |
| LOW | 1x (480x272) | Sharp | On | the row's | On | Off | the library's |
| MEDIUM | 2x | Sharp | On | the row's | On | On | the library's |
| HIGH | 4x | Sharp | On | the row's | On | Off | the library's |
| ULTRA | 4x | Sharp | On | the row's | On | On | On |
| HANDHELD | 2x | Sharp | On | On | On | On | the library's |

A preset sets only presentation parts, which never reach the guest. CRISP SYSTEM TEXT draws the on-screen keyboard at
the window's size and costs nothing while the keyboard is down, so every preset turns it on. SHARPER ENLARGING runs
only where the window enlarges the frame on both axes. AMD's published FSR 1 quality modes span 1.3x (Ultra Quality)
to 2x (Performance) per axis ([GPUOpen: FidelityFX Super Resolution
1](https://gpuopen.com/fidelityfx-superresolution/)). MEDIUM's and HANDHELD's 2x frame on a 1080p or handheld screen
and ULTRA's 4x frame on a 1440p or 4K screen are in that range; on a 1440p or 4K screen MEDIUM's and HANDHELD's frame
is enlarged 2.7x to 4x, past it, where HIGH or ULTRA is the better choice. LOW turns it off: its 1x frame is enlarged
about 4x or more on any screen of 1080p or more, past that range everywhere, and LOW is for the weakest GPUs, where
every pass counts. HIGH turns it off as ULTRA without it, the plain 4x picture. ULTRA also turns FULL 32-BIT COLOUR
on (#2280), which changes only the picture the window shows; the other presets leave it to the library, so its
second draw of each render op and its GPU memory stay off unless asked for. Where the GPU renderer cannot run FSR
1, the normal scaling filter stays, as for the part. `fsr1-selftest` reports the pass's cost at each of these sizes
(`docs/SETUP.md`). No preset sets frame pacing, the play mode, SMOOTH MOTION, WIDESCREEN, TEXTURE FILTERING, CLEAN
PITCHED SOUND, FINER PATCHES, STICK DEAD ZONE, LOADING SPEED, WIDER CULLING, LATE PAD SAMPLING, SOFT SHADOWS, UNCLIPPED
SOUND, TEXTURE SCALING, the SMART 2D FILTER, the SPEED READOUT or
the GAME BADGE: they change timing, framing, input, sound, guest-visible output or the HUD, or have no measured cost on
low-end GPUs, so they stay the library's and each game's. FULL 32-BIT COLOUR stays out of every preset but ULTRA for
the same reason: its cost (a second draw of each render op and the twin's GPU memory) is not measured on any GPU, so
only ULTRA, the preset for GPUs with headroom, turns it on, and under the others it stays the library's and each
game's.

The parts a preset sets are resolved with the rest (`player_play_resolve`): a game's own value, else the preset's,
else the library's, so the launch line, `SR_PRESENT_UPSCALE` and both Enhanced pages agree. Settings > Enhanced shows
a part the preset sets with the preset's value and name (`ON (ULTRA)`), and a game's page following the library shows
that value (`LIBRARY (ON)`). With an Authentic library the rows are dimmed but keep those values, for when a game
plays in Enhanced. Editing such a part on Settings > Enhanced steps from the value shown and sets the preset to
CUSTOM. Going CUSTOM by any edit keeps what the pages showed: the parts the preset set become the library's values
(`graphics_preset_row_edited`), so editing RESOLUTION under ULTRA leaves SHARPER ENLARGING on. Editing a part for one
game keeps the preset, since the game's own value wins over it. A settings file from v0.0.2 with a preset chosen keeps
its library parts as written, and an Enhanced session takes the preset's parts over them (HIGH with SHARPER ENLARGING
on in the library launches with FSR 1 off; the page says `OFF (HIGH)`).
Left and Right change a focused value, while Q and E or the controller shoulders switch Enhanced tabs. Every change is saved as it is made. Below a 680 px tall window the `SETTINGS` heading is left out (the tab names the
page) so every row fits 960 x 540. The proposed whole-Settings tabbed layout below remains a design that is not built:

- Top bar with SETTINGS active; `SETTINGS` in `DISPLAY`; a tab row (`LABEL`, 10/12 px padding, 3 px accent rule
  under the active tab, `line` underneath): DISPLAY, AUDIO, CONTROLS, PATHS AND STORAGE, ACCESSIBILITY, ABOUT.
  L and R (Q and E) switch tabs.
- Body: a column of rows and, from 1100 px wide, a 340 px help panel (`card`, a 20 px Geist SemiBold heading,
  `BODY_S` `muted` text) that explains the focused row and names its `settings.json` key.
- Row: at least 56 px, 8/16 px padding, radius 8; label in Geist Medium 15 with a 12 px `muted` description; the
  control on the right. A focused row fills with `surface_high` and the ring surrounds the row.
- Segmented control: 3 px padding in a ground-filled, `line_strong`, radius 9 track; options 30 px, 13 px `muted`;
  the selected one `surface_high` with paper text, and a 2 px inset accent edge while its row has focus.
  Left and right change it.
- Toggle: 46 x 26 track (off `line_strong`, on accent) with a 20 px knob (off `muted`, on ink) and ON / OFF in
  `LABEL_S` before it.
- Slider: 280 px track 6 px high (accent fill, `line_strong` rest), 18 px paper thumb, the value in mono 13 right.
- Paths rows: name, the path in `DATA` (middle-truncated), status line, small actions (OPEN, REMOVE). No key-file
  row.
- Controls tab: a 30 px mapping header in `EYEBROW` and 38 px mapping rows; a row waiting for a press fills with
  `capture` and the prompt bar shows only CANCEL.

### Controller settings (built)

Settings > Controls opens one page under the top bar with SETTINGS active. Under the `CONTROLS` heading (left out
below 680 px tall), CONTROLLER and KEYBOARD (the one shown filled; Up from the first binding) choose whose bindings the
page shows, beside one status line: what the last binding changed, a load or save problem in `error`, the prompt
while a button is awaited, or which controller is connected. Two cards side by side:

- BUTTONS: the eyebrow names the mapping being edited (`BUTTONS · GLOBAL MAPPING`, or the game's own in `ready`),
  then the 14 PSP controls in two columns of name and binding. The binding is a small button with the host button's
  name; the control the status line names is in `accent` (`error` when it was left with nothing); the control waiting
  for a press shows `Press a button…` as the primary button. Choosing it again, or Esc, cancels the wait. Between the name and the button a 20 px prompt glyph
  (`nk_ui_prompt_glyph_in`, #1299) shows the binding as the device shown prints it, whatever device drives the page:
  the controller in the set of the pad last used or connected (the neutral position dots with none), its face
  buttons by position, the shoulders, Start and the D-pad as the prompt bar draws them, and any other input as a pill
  named the way that pad names it (VIEW, ZL, L3); the keyboard as a keycap with the key's printed legend (SHIFT, ENTER,
  X) or the arrow keys. A control with nothing bound has no glyph.
- STICK AND TRIGGERS: PSP STICK with LEFT STICK or RIGHT STICK (which controller stick drives it), dead zone and
  trigger threshold with - and +, CALIBRATE STICK, and LIVE INPUT in `DATA`: the raw stick, the PSP value it maps to,
  the triggers and the buttons held.

An input another control already uses moves to the control chosen, and that control takes the chosen one's input in
its place, so one input never drives two controls and every mapping can be saved. The status line names what changed
("A (Cross) was Cross's. Cross now uses B (Circle)."); a control left with nothing (the chosen one had no input to
give) is named in `error` so it can be bound next. Nothing a player does here can take the launcher's own keys and
buttons, which are fixed: the arrows, Enter and Esc, and the pad's D-pad, south and east buttons.

With KEYBOARD (#1299) the rows show each control's keys by their names on a US keyboard (`KEYS · GLOBAL MAPPING`),
the waiting control shows `Press a key…`, and the next key pressed is its key; Esc or a controller press stops the
wait. A key another control uses moves as a controller input does; a key that cannot be bound (Esc and the function
keys, which the runtime keeps) is refused and the status line says why in `error`. The stick card is ANALOG STICK:
the stick's four keys (UP, DOWN, LEFT, RIGHT; I, K, J and L by default) as rows like the buttons', where a held key
pushes that axis to its edge in game, then the keys Nakagawa keeps. Under the card, beside BACK, MOUSE AIM: OFF or
MOUSE AIM: ON says whether the mouse moves that stick too; the card has no room for it at the largest text size.

MOUSE AIM (#1791) opens the mouse aiming settings, which replace the cards with a 720 px dialog card as a calibration
step does: the mapping they belong to in the eyebrow (`MOUSE AIMING · GLOBAL MAPPING`), a lead that says the mouse
pushes the PSP stick in a game, that the game keeps the pointer until it is paused, and that a change applies when
a game next starts, then five rows in a box, each with its control on the left as a Settings row has it: MOUSE AIM (ON or OFF, the button outlined in the accent while on), SPEED and
SMOOTHING with - and + (the speed in steps of 10% up to 200% and of 50% from there to 1000%, the smoothing in steps of
5% up to 95%; a value a hand-edited profile holds between two steps moves to the next step), INVERT LEFT AND RIGHT and
INVERT UP AND DOWN. Each change is written to the input profile as it is made. DONE, Esc or East returns to the keys,
on MOUSE AIM. The settings are the keyboard and mouse's: RESET TO DEFAULTS with KEYBOARD shown turns aiming off at
100%, and with CONTROLLER shown leaves it as set. Every row shows whole in every launcher window at every text size
(`test_controller_mouse_aim`).

A host input is named by the label a pad prints on it, with its other common name in brackets (`A (Cross)`,
`LB (L1)`, `RT (R2)`), and a key pair of one modifier reads as the modifier alone (`Shift` for Left and Right Shift), so
every default binding, two inputs included, and any one input fit their button whole: at most 20 characters
(`INPUT_SETTINGS_BINDING_TEXT_MAX`), the width `test_binding_labels_fit` checks at every launcher size. The margin
leaves room for the prompt glyph beside the binding (#1299).

Under the cards RESET TO DEFAULTS (the device shown only) and, with a game selected, USE THIS GAME'S OWN MAPPING or
USE THE GLOBAL MAPPING; BACK sits at the right end, under the stick card. There is no save step: every change is
written to the input profile as it is made, and a write that fails says so in the status line. The page opens on the
first binding.
Calibration replaces the cards with a 720 px dialog card per step: rest (a progress bar, CANCEL only), range
(FINISH) and result (ACCEPT), each with its live numbers. Esc or East cancels a step and returns to CALIBRATE STICK.
Every page, the mouse aiming settings included, fits 960 x 540.

### Developer log (built)

F12 in developer mode opens one card under a top bar without tabs: a `warning` `▲ DEVELOPER MODE` eyebrow, the
privacy line (on this PC only, paths as `%USERPROFILE%`), then the whole log in `DATA` on the ground colour, each line
cut to the card's width, in a box that scrolls: it opens on the newest lines and follows new ones while it shows the
end, and the wheel, a drag, Page Up and Page Down (the pad's triggers; a page is the box less one line) scroll it.
Under it, COPY, SAVE, LAYOUT INSPECTOR: OFF (or ON) and CLOSE (primary, focused first), with the last notice beside
them. Esc or F12 closes it. The layout inspector is Clay's own (`nk_ui_set_layout_debug`): a panel down the right of
every launcher screen that lists the layout and outlines the element under the pointer, until it is turned off or
developer mode ends. The screens keep their width, so the panel covers their right side.

### First run (C7)

Built now: the setup wizard's four steps (Welcome, Options, Game, Verify) on the existing wizard state, in the
dialog card the build and error screens use. Copying the disc's files at Verify ends on the library card, where the
game is built and played, and the PSP fonts are imported in Settings > PSP fonts, so the step chips name only those
four steps. The older Fonts and Ready step states remain only for state-only callers.
Options ("Online and developer options", [`ONLINE_FEATURES.md`](ONLINE_FEATURES.md)) comes right after Welcome because it needs no game: one plain line that online features aren't built (an Online mode switch shows only when a stored setting has online mode on), one switch row (a 220 px button reading "Developer mode: On" or "Off",
accent while on, with its sentence beside it), then CONTINUE (the default focus, so Enter never flips a switch), SKIP and BACK. A strip of step chips sits at the top (current: accent fill; done:
`ready` edge and a check); each step has one primary action first, then Back and Cancel. Every control returns
`PLAYER_UI_ACTION_WIZARD` with an op that `main.c` runs through the handlers the existing wizard calls. The Welcome
step says the privacy promise once. The left rail below is a design that is not built:

- Left rail 260 px (32/24 px padding, `line` edge on the right) listing the steps: Welcome, Disc image, System
  fonts, Build, Play. There is no key-file step. Each step is 44 px with a 26 px numbered circle (`line_strong`
  edge); current: accent fill and ink number, name in SemiBold; done: `ready` edge and number; failed: `error`.
  Below 900 px the rail hides and the step shows as an eyebrow.
- Content: 36/56 px padding; `DISPLAY_XL` headline; `BODY_L` `muted` lead, at most 720 px wide; choice cards
  (`card`, 18 px padding, tag chip, `HEADING`, `BODY_S`); check rows (38 px, `line` underneath: name 170 px,
  code in `DATA` 140 px, status word 120 px in its colour, detail `CAPTION`).
- Build: stage rows 50 px with a 26 px circle (running: accent fill), name 170 px, and a 6 px progress bar (accent
  on `line_strong`); a log box (`#080B0F`, `line`, radius 10, mono 12/1.6 `muted`).
- One primary action per step at the bottom left (CONTINUE, BUILD, PLAY), secondary actions after it.

### Error dialogs (C8)

- Over the scrim, centred, 16 px from the edges; at most 700 px wide, or 880 px with a details box (a path list or
  a boundary), whose line budget grows by one per 16 px of window height past 540 so its fix is never cut off;
  `surface_raised`,
  `line_strong` edge, radius 16, 26/28/22 px padding, 14 px gap.
- The card (`nk_ui_dialog_begin`, also every flow card and the in-game overlay's) is a Clay floating element,
  centred on the area it opens in: it moves nothing on the page, draws over it, and takes the pointer, so nothing
  under it is hovered or clicked (`test_modal_card_floats`).
- Order: a status eyebrow in its colour with marker ("▲ CONTROLLER DISCONNECTED · GAME PAUSED"), a `TITLE` that says
  what happened in plain words, a `BODY` `muted` paragraph that says what to do, an optional detail box (ground
  fill, `line`, radius 10, `DATA` lines for paths or codes), then the button row: the recommended action first as
  primary, the way out last ("NOT NOW", "CANCEL"); a destructive action is a danger button and is never the
  default focus.
- Focus is trapped in the dialog and starts on the first button; BACK closes it with the safe choice.
- Cases to cover: unreadable or unsupported disc image (a store package is not a disc image), a different release of
  a known game (add as Experimental), build failure (stage named, log path shown), controller disconnected, quit to
  library. A disc whose program can't be decrypted gets the plain reason and its folder, never a request for a
  key file.

### In-game overlay (C9c, built)

Drawn by the runtime over the frozen game frame under the `scrim` while the game is paused
(`src/rt/overlay/`, hosted by the pause presenter in `src/rt/gui.c`). No top bar and no waves: one card centred
16 px from the edges, and the prompt bar along the bottom. The card is the error dialog's (`nk_ui_dialog_begin`).

- Menu (480 px card): `GAME PAUSED` eyebrow in `muted`, the game's name in `TITLE` (two lines at most, else
  "Paused"), the disc ID in `DATA` `muted`, then a full-width column of buttons: RESUME (primary, 52 px, the
  default focus), SETTINGS, CONTROLS, QUIT TO LIBRARY. In developer mode (the library's setting, passed to the
  runtime as `SR_DEVELOPER_MODE`) VRAM VIEWER and PROFILE come between CONTROLS and QUIT TO LIBRARY. Prompts: MOVE,
  SELECT, RESUME.
- Settings (640 px, or 880 px with shorter rows when the window is under 720 px high): Settings rows as in C6, in
  this order. Fullscreen is a toggle, shown only when the game window can change mode. Game scaling is a segmented
  control (SHARP, INTEGER, SMOOTH; left and right step it, Confirm cycles it). Frame pacing is a segmented control
  with the library's two choices (PSP TIMING, NO EXTRA CAP), shown only when one of them is in force, so a
  developer's numeric `SR_FPS_CAP` gets no row. The performance HUD is a toggle, and Ace Mode's speed readout one
  more in an Ace run that can draw it. Dump textures, last, is a toggle shown only in an Enhanced or Ace run on the
  Vulkan renderer; its description says what it saves, or why the last start was refused, and while it is on how
  many files it has saved, why it stopped when it did, and how many textures that change every frame it no longer
  saves. A `FOOTNOTE` says the changes apply now and last until the game closes; the player's Settings keep the
  saved values. BACK.
- Controls (640 px, or 880 px in two side-by-side tables when the window is under 720 px high and there are more
  than six rows): a PSP / KEYBOARD / CONTROLLER header in `EYEBROW` and 38 px rows (34 px when split) with `line`
  dividers; an unbound control shows an em dash. The rows describe the mapping in force: the window's own key
  table and the input profile, with the pad's buttons named for its family (A, B, X, Y and LB for an Xbox-style
  pad, Cross and L1 for a PlayStation-style one, the position for an unknown pad), the prompt bar's rule. A
  `FOOTNOTE` points at the player's Settings for remapping. BACK.
- VRAM viewer (developer mode, 920 px): `GAME PAUSED · DEVELOPER` eyebrow, "VRAM viewer", rows for Source, Address,
  Stride and Format (and Capture when the run recorded VRAM captures), the decoded window, and a `FOOTNOTE` that the
  view is read-only. BACK.
- Profile (developer mode, 760 px): the same eyebrow, "Profile", the host's formatted performance counters as label
  and value rows ("No interval has closed yet." before the first), and one bar per second for cpu, ge wait, present
  and idle. BACK.
- Quit to library (560 px): `▲ QUIT TO LIBRARY` in `warning`, "Quit" and the game's name, "Anything the game has not saved is
  lost.", then KEEP PLAYING (primary, the default) and QUIT TO LIBRARY (danger, never the default). While a save is
  in progress the dialog says so ("The game is saving") and offers RESUME and BACK only.
- Controller disconnected (560 px): `▲ CONTROLLER DISCONNECTED · GAME PAUSED` in `warning` and what to do; once a
  pad is back, `● CONTROLLER CONNECTED · GAME PAUSED` in `ready`. RESUME (primary) and PAUSE MENU.
- Input: Esc and the pad's back button go back a page and resume from the menu; F1 and the pad's Start resume from
  any page; Guide resumes from anywhere. Back on the controller dialog goes to the menu, never into the game.
- Without SDL3_ttf or the bundled fonts the runtime shows the frozen frame under the scrim alone (C9b's overlay),
  and says why once on stderr; Esc, F1, Guide and the pad's back button resume.

## States

- **Focused**: the ring (see [Focus](#focus)), plus the fill a component names: `surface_high` for a secondary
  button, a row or a tab, `line_strong` for a card's edge.
- **Hover and press**: pointer looks only (see [Hover and press](#hover-and-press)).
- **Disabled** (`NK_UI_BUTTON_DISABLED`): `dim` label, `line` edge, no fill (a primary button keeps `surface_high`).
  A disabled control takes no activation, and focus moves pass over it (`nk_ui_focus_enabled`). WCAG's contrast
  rule exempts inactive controls; this page still lists their pairs under [Known shortfalls](#known-shortfalls).
- **Dimmed row** (a Settings option that cannot apply as the page stands): the name in `muted`, a stepper's value
  and the current chip in `muted`, the other chips and the arrows in `dim`, the arrows inert, and the current value's
  edge in `selected_neutral_edge_dimmed`. The row stays in the focus walk, and the detail panel gives the reason.
- **Selected**: a value that is on or chosen fills with the accent and takes `ink` text; a selected Off, or the
  library's own value, is a `surface_high` box with a `selected_neutral_edge` edge and `paper` text, so a list of
  options that are off is not a column of accent boxes (`settings_enhanced_chip` in `scr_settings.c`). The active
  tab has `paper` text and the 3 px accent rule.
- **Status**: a marker shape and a word, then the status colour (see [Principles](#principles)).
- **Error**: a cross marker and a badge word in `error`, a plain title, and what to do (see [Messages](#messages)).
  A row that has a problem names it on the row in words (the launcher music row's "On: file missing", the
  controller page's status line in `error`).
- **Busy**: a progress bar (`nk_ui_progress_bar`: an accent fill on a 6 px `line_strong` track for a known
  fraction, a sweeping segment when the fraction is unknown) and a sentence that says what is happening.

## Messages

**Error cards** (`scr_error.c`, the [Error dialogs](#error-dialogs-c8) layout):

1. Eyebrow: `✕` and a short badge in `error`. The badge comes from the failure code (`player_error_badge` in
   `src/player/player_state.c`: `RUNTIME_PREMATURE_EXIT` reads "Game didn't start"); a code with no badge falls back
   to "Problem", and `ErrorCodeSourceTests` fails when the player raises a code that has none. The codes are stable
   and listed in [`FAILURE_CODES.md`](FAILURE_CODES.md).
2. Title (`TITLE`, `paper`): what happened, in plain words; "Something went wrong" only when the caller gave none.
3. The failed stage, when there is one, as a `warning` status pill ("Failed stage: …").
4. Message (`BODY`, `muted`): what to do next.
5. Details (`DATA`, `muted`, wrapped in a ground-filled box): paths, the boundary, the log file.
6. Buttons: the recovery action first and primary (default "Back to library"), then a retry when the failure can
   be retried, then COPY DETAILS.
7. A game failure adds a report row: "This is a known issue (#N)." with VIEW ISSUE #N ON GITHUB when the cause is known, REPORT
   THIS GAME and SEE EXISTING REPORTS; the prompt-bar hint says "Copy details uses the clipboard and Report opens
   your browser. Nothing is sent anywhere."

**The problem report** (Settings > Problem report, `settings_problem_report` in `scr_settings.c`) says before it
starts what the ZIP holds and what it leaves out ("Your user name and folders are replaced, and game files, saves
and keys are left out. Nothing is uploaded."), lists the files it would hold, and when it is written reminds the
user that GitHub issues are public. Its result line is `ready` when the report is written and `warning` when it
could not be written or Python is missing, and the warning names what to run instead.

**Wording rules** written down today: say what happened in plain words, then what to do; name the recovery action
on its button; say what is sent and what is not; describe what the build does, never what it may do later
([Principles](#principles)); never ask for or name a key file. A rule against blaming the user, and a check on
wording, do not exist yet.

## Accessibility

- Text contrast is at least 4.5:1 for everything except `dim`, which is reserved for disabled controls and
  footnotes; over the brightest wave point muted and error text stay at 4.6:1. The measured pairs and the places
  `dim` falls short are under [Contrast](#contrast).
- Focus ring 7.2:1 on the ground, at least 6.1:1 on any surface, 4.4:1 over the brightest wave point and 3.7:1 on
  the scrim over a white game frame; high-contrast focus for any background, from Settings > Accessibility or a
  Windows contrast theme.
- Targets are at least 44 px high, except small controls (34 px at 100% text): the library's heading-row buttons
  and the on-screen keyboard's keys, small so that a row of cards still fits under the find box at 1280 x 720. The
  interface size (100 to 200%) scales the whole layout through the density, and the text size (100, 125 or 150%)
  scales type alone, with the layout reflowing around it.
- Reduce motion holds the waves and makes transitions cuts; Automatic follows Windows' Animation effects.
- Launcher sounds are off by default and never replace a visual cue.
- Colour never carries meaning alone: a status is a marker shape and a word, a selected value has an edge or a fill
  as well as its text colour, and a key the on-screen keyboard cannot type loses its outline as well as dimming.
- Not there yet: no screen-reader support
  ([#2748](https://github.com/Jstar269/nakagawa-recomp/issues/2748)); English is the only shipped language;
  no light theme and no colour-blind palette;
  a Windows contrast theme changes the focus ring only. The full list, with workarounds, is in
  [`ACCESSIBILITY.md`](../ACCESSIBILITY.md#known-limitations).

## Contrast

Each text colour is measured against each background the UI draws it on, with the WCAG 2.x formula: relative
luminance from the sRGB channels, and (lighter + 0.05) / (darker + 0.05). The targets are WCAG's, used as a guide
for the launcher (see [`ACCESSIBILITY.md`](../ACCESSIBILITY.md#what-we-aim-for)):

| Kind | Target | What it covers |
| --- | --- | --- |
| Normal text | 4.5:1 | every role under 24 px, or under 18.66 px (14 pt) in a bold face |
| Large text | 3:1 | 24 px and up, or 18.66 px and up in a bold face; the test counts Geist SemiBold and Chakra Petch Bold as bold |
| Focus indicator | 3:1 | the ring against what is around it |
| Selection edge | 3:1 | the edge that marks a current value without the accent, against what is around it |

Sizes are logical px at the 100% text size; a larger text size only makes text larger. A pair's kind comes from the
smallest role the record lists for it, so a pair used for both headings and captions is held to 4.5:1.

How the backgrounds are measured:

- A colour with alpha is composited over what it is drawn on before measuring, channel by channel in sRGB bytes
  as `nk_ui_composite_luminance` does. The worst case is used: `art_scrim` under a badge over a white picture, and
  `scrim` over a white game frame under the in-game overlay.
- Text on the ground can sit over the waves or the library backdrop. Their brightness is capped at a luminance of
  `NK_UI_WAVES_LUMINANCE_MAX` (`nk_ui_waves_compute_cap`, `nk_ui_backdrop_scrim`), so the record measures the
  ground pairs a second time against a background of exactly that luminance (`wave_limit`).
- Text inside a box that is filled with the ground (a details box, a Settings stepper) counts as text on the ground.

Not measured, because the code has no rule for them yet: control and card edges (`line` is 1.2:1 and `line_strong`
1.6:1 on the ground), decorative vector art, the waves themselves, and the inactive prompt glyph dots
(`placeholder`).

### Known shortfalls

These pairs are drawn today and fall below their target. They are recorded, not fixed: how the UI looks is the
owner's decision. `KNOWN_SHORTFALLS` in `tests/native/test_player_ui.py` holds the same list with the same ratios,
so an improvement or a further drop fails the test until both are updated.

| Text or edge | On | Ratio | Target | Where |
| --- | --- | --- | --- | --- |
| `dim` | `ground` | 4.16 | 4.5 | About's source commit and data folder lines on the page; in ground-filled boxes, the update link in Settings and the file being copied in first run; disabled button labels; a dimmed row's chips and arrows |
| `dim` | `wave_limit` | 2.53 | 4.5 | About's two lines on the page where they sit over the brightest waves allowed |
| `dim` | `panel` | 3.86 | 4.5 | the disc ID on a game card; the empty library's "Or drop an .iso onto this window."; the title page's developer line and About's credits note, in cards; footnotes on the in-game overlay's pages; the build-tool download's per-item line; disabled buttons on a card |
| `dim` | `control` | 3.55 | 4.5 | the disc ID on a hovered or focused game card; a disabled primary button |
| `dim` | `pressed` | 3.06 | 4.5 | the disc ID on a game card while it is pressed |
| `dim` | `art_tint_0` | 3.71 | 4.5 | the art placeholder's caption ("ART FROM YOUR DISC SHOWS HERE"), `MICRO`, on a title's art tint |
| `dim` | `art_tint_1` | 2.86 | 4.5 | the same caption on the blue art tint |
| `dim` | `art_tint_2` | 3.56 | 4.5 | the same caption |
| `dim` | `art_tint_3` | 3.59 | 4.5 | the same caption |
| `selected_neutral_edge_dimmed` | `control` | 2.64 | 3 | a dimmed row's current value against its own `surface_high` box (accepted when it was added, as a dimmed row is inert) |

WCAG's contrast rule exempts the disabled controls among these; footnotes, the disc ID and the placeholder caption
are not exempt.

## Building and checking a screen

Before review, check each point:

- [ ] Every colour, size, radius and duration comes from the theme (`nk_ui_theme(ui)`, `NK_UI_*`, `nk_ui_type`); a
  new one is added to `src/ui/nk_ui_theme.h` and `.c`, to the tables above and to the [Token record](#token-record)
  in the same change.
- [ ] Each new text colour on a new background is a pair in the record's `pairs`, with where it is drawn, and meets
  its [contrast](#contrast) target.
- [ ] Every action works with a D-pad and two buttons, and with the keyboard; the prompt bar names each one.
- [ ] The default focus is a safe action, never a danger button.
- [ ] Long text either wraps whole on a page that scrolls or has another place to be read in full
  ([Text](#text-wrapping-and-length)).
- [ ] A failure says what happened and what to do, and has a named way out ([Messages](#messages)).
- [ ] `python -m unittest discover -s tests/native -p test_player_ui.py -k PlayerUiDesignTokenTests` passes, and
  the gates below do.

The gates:

1. Compose the screen from the components above; give every element a stable id (sub-parts use
   `nk_ui_child_id`). Ids follow one scheme (`src/ui/nk_ui.h`): a screen makes its ids with its own macro over
   `NK_UI_ID(screen, offset)`, the screen's byte coming from the table there, so two screens never collide; a run of
   ids made by adding to a base declares `<BASE>_SPAN`, and nothing else may fall inside it; ids that several screens
   name are `PLAYER_UI_ID_*` in screen 0x00. `PlayerUiIdSourceTests` in `tests/native/test_player_ui.py` checks the
   table, the ranges and the spans, and the screen tests fail on any id Clay reports twice in one frame.
2. `mingw32-make nk-ui-player-screens-selftest` compares the draw-list dumps in `tests/fixtures/nk_ui/*.golden` and runs
   the layout checks (ring outside the focused card, centred labels, grid gaps). After an intended change, run
   `build/test_player_screens --write-goldens` and say in the pull request why each golden changed.
3. `mingw32-make nk-ui-player-screens-capture NK_UI_CAPTURE_DIR=<dir>` saves offscreen captures of each state at
   1280 x 800, 1920 x 1200 at 150% and 960 x 600, from synthetic fixture titles only. Compare them with the
   design boards before review.
4. The in-game overlay's pages have their own gates: `mingw32-make nk-ui-overlay-screens-selftest` (flow, focus,
   fit at those three sizes, and `tests/fixtures/nk_ui/overlay.golden`; rewrite it with
   `build/test_overlay_screens --write-goldens`) and `mingw32-make nk-ui-overlay-host-selftest` (the runtime host on
   SDL3_ttf, the bundled fonts and real key events). `mingw32-make nk-ui-overlay-screens-capture
   NK_UI_CAPTURE_DIR=<dir>` saves every overlay page, the developer pages included, at those three sizes, drawn as
   the overlay host draws them, for review by eye.
   - The Controls page lays its rows out in two tables side by side on a window under 720 px tall, so all ten
     show at once. One scrolling list would hide rows that a controller reaches only by focusing them, and the
     rows are read, not chosen.

## Loose ends in the code

Found while writing the [Token record](#token-record). None is changed here: each is a question for the owner, and
until one is settled the record lists the code as it is.

- `capture` is in the theme and in the Colour table, but no screen draws it.
- `focus` and `accent_dark` are two fields with one value (`#6B9FF0`): the ring reads `focus`, while focused
  steppers and overlay rows draw their edge in `accent_dark`.
- Copies of theme colours outside the theme, which a theme change would not reach: `NK_UI_ACCENT_RGBA` and
  `NK_UI_PAPER_RGBA` (the N1 mark and the showcase art, `nk_ui_widgets.c`), and the empty library's disc icon
  (`library_empty` in `scr_library.c`: the accent and `surface_high` as literals). The waves read the
  ground from the theme (`nk_ui_theme_n1()->surface`), not from a copy.
- The build and developer log boxes fill with `#080B0F`, darker than the ground, written as a literal in
  `scr_build.c` and again in `scr_devlog.c`; there is no token for it.
- The art tints (`nk_ui_art_tint`), the showcase fills (`nk_ui_art_fill`) and the showcase pictures' colours
  (`#3DD6C6`, `#8B7CFF`, `#FF5A5F`, near but not equal to `ready`, `info` and `error`) are literals in
  `nk_ui_widgets.c`.
- The high-contrast ring's white and black are literals in `nk_ui_focus_ring` (`nk_ui.c`), not theme fields.
- Component metrics off the scales: the detail and log boxes' radius 10 (`scr_flow_box_style`), the art
  placeholder's radius 6, a stepper arrow's radius 7 (`NK_UI_RADIUS_CONTROL - 1`), the progress track's radius 3;
  the dialog's 26/28/22 px padding and 14 px gap, the button's 18/22/34 px side padding, and the overlay row edge of
  2 px. Component-internal metrics have no rule yet.
- The in-game overlay's quick settings (`overlay_quick.c`) build text styles by hand (Geist Medium at 15/22, 12 and
  13/18 px) instead of taking a role from `nk_ui_type`; Geist Medium has no role in the type scale.
- `NK_UI_FONT_DISPLAY` (Chakra Petch SemiBold) is registered, but no role or screen uses it: the display roles use
  `NK_UI_FONT_DISPLAY_BOLD`.
- `PLAYER_UI_READY`, `PLAYER_UI_WARNING` and `PLAYER_UI_INFO` (`player_ui_nk.c`) read `nk_ui_theme_n1()` directly
  rather than the theme the frame draws with (`nk_ui_theme(ui)`). There is one theme, so the two agree today.
- This page's [Error dialogs](#error-dialogs-c8) and elevation levels put a dialog "over the scrim", but the
  launcher's dialogs and flow cards draw no scrim; only the in-game overlay does (`src/rt/gui.c`).
- `NK_UI_MOTION_BASE`'s comment names a toast, and the UI has no toast.

## Token record

The block below is the machine-readable form of this page's tokens. `PlayerUiDesignTokenTests` in
`tests/native/test_player_ui.py` reads it and checks that:

- every `NkUiTheme` field is under `colours` with its value in `g_n1_theme` (`src/ui/nk_ui_theme.c`), and the
  Colour table above agrees;
- every opaque colour literal in `src/ui/*.[ch]`, `src/player/ui/*.[ch]` and `src/rt/overlay/*.[ch]` is a theme
  value or is under `outside_theme` with its file, and every `outside_theme` entry is still in its file;
- every `#define NK_UI_*` in `src/ui/nk_ui_theme.h` and `src/ui/nk_ui_waves.h` is under `metrics` with its value;
- `type` matches `g_type_scale` and the wrap rule in `nk_ui_type`, and the Type table above agrees; `fonts` matches
  the `NK_UI_FONT_*` slots and `g_font_paths`;
- every pair in `pairs` meets its [contrast](#contrast) target, or is a [known shortfall](#known-shortfalls) with
  the ratio recorded there and in the test.

Colours are `#RRGGBB`, or `#RRGGBBAA` when not opaque. In `backgrounds`, `colour` names a colour above, `over`
composites it over another colour, and `luminance` names the metric that gives a background by its luminance alone.
In `pairs`, `kind` `text` lists the type `roles` drawn there; `focus` and `edge` are held to 3:1.

<!-- player-ui-tokens -->
```json
{
  "colours": [
    {"name": "ink", "value": "#10151C", "role": "text and icons on an accent fill"},
    {"name": "paper", "value": "#F5F7FA", "role": "primary text; prompt glyph tint"},
    {"name": "surface", "value": "#0B0F14", "role": "window ground; boxes inside cards (details, steppers, wizard boxes)"},
    {"name": "surface_raised", "value": "#121821", "role": "cards, panels, dialogs, flow cards, prompt bar"},
    {"name": "surface_high", "value": "#18202B", "role": "chips, pills, tags; focused or hovered secondary button, row and tab; disabled primary button"},
    {"name": "accent_dark", "value": "#6B9FF0", "role": "brand blue: primary button, lit option, active tab rule, progress, accent eyebrows and text"},
    {"name": "accent_light", "value": "#2F6FD0", "role": "accent on a light surface; reserved, the UI has none"},
    {"name": "accent_pressed", "value": "#4F86DC", "role": "pressed primary button or lit option"},
    {"name": "ready", "value": "#2DD4BF", "role": "Ready, done steps, a written report"},
    {"name": "warning", "value": "#DBB024", "role": "needs setup, failed stage, developer mode"},
    {"name": "info", "value": "#C29AF4", "role": "experimental, untested, informational notes"},
    {"name": "error", "value": "#FF7B72", "role": "unsupported, failed, remove; danger button text"},
    {"name": "muted", "value": "#9AA4B2", "role": "secondary text"},
    {"name": "focus", "value": "#6B9FF0", "role": "the focus ring"},
    {"name": "line", "value": "#1C232D", "role": "hairlines, card edges, disabled button edge"},
    {"name": "line_strong", "value": "#2E3846", "role": "control and chip edges, focused card edge, progress track"},
    {"name": "dim", "value": "#6B7684", "role": "disabled text and footnotes"},
    {"name": "pressed", "value": "#222C39", "role": "pressed secondary button, row, tab or card"},
    {"name": "pressed_danger", "value": "#2A1A1C", "role": "pressed danger button"},
    {"name": "scrim", "value": "#0B0F14C7", "role": "the ground at 78% over the frozen game frame under the in-game overlay"},
    {"name": "art_scrim", "value": "#0B0F14B8", "role": "the ground at 72% under a badge on title art"},
    {"name": "art_label", "value": "#C9D6E3", "role": "the size note on the hero art"},
    {"name": "placeholder", "value": "#3A4552", "role": "art placeholder outline, inactive prompt glyph dots"},
    {"name": "capture", "value": "#223046", "role": "a row waiting for a button press; drawn nowhere on main"},
    {"name": "accent_hover", "value": "#8FB8F2", "role": "primary button or lit option under the pointer"},
    {"name": "selected_neutral_edge", "value": "#6B7684", "role": "edge of a current value marked without the accent"},
    {"name": "selected_neutral_edge_dimmed", "value": "#576270", "role": "that edge on a dimmed row"}
  ],
  "outside_theme": [
    {"name": "hc_ring_white", "source": "src/ui/nk_ui.c", "symbol": "nk_ui_focus_ring", "value": "#FFFFFF", "role": "high-contrast focus ring"},
    {"name": "hc_keyline_black", "source": "src/ui/nk_ui.c", "symbol": "nk_ui_focus_ring", "value": "#000000", "role": "the keylines either side of the high-contrast ring"},
    {"name": "log_box_build", "source": "src/player/ui/scr_build.c", "symbol": "scr_flow_box_style fill", "value": "#080B0F", "role": "the build log box"},
    {"name": "log_box_devlog", "source": "src/player/ui/scr_devlog.c", "symbol": "scr_flow_box_style fill", "value": "#080B0F", "role": "the developer log box"},
    {"name": "art_tint_0", "source": "src/ui/nk_ui_widgets.c", "symbol": "nk_ui_art_tint", "value": "#161C24", "role": "a title's art panel fill, chosen by its key"},
    {"name": "art_tint_1", "source": "src/ui/nk_ui_widgets.c", "symbol": "nk_ui_art_tint", "value": "#16324A", "role": "a title's art panel fill"},
    {"name": "art_tint_2", "source": "src/ui/nk_ui_widgets.c", "symbol": "nk_ui_art_tint", "value": "#1A1F2E", "role": "a title's art panel fill"},
    {"name": "art_tint_3", "source": "src/ui/nk_ui_widgets.c", "symbol": "nk_ui_art_tint", "value": "#12202A", "role": "a title's art panel fill"},
    {"name": "art_showcase_scene", "source": "src/ui/nk_ui_widgets.c", "symbol": "nk_ui_art_fill", "value": "#0E1A2B", "role": "the scene demo's art panel fill"},
    {"name": "art_showcase_breakout", "source": "src/ui/nk_ui_widgets.c", "symbol": "nk_ui_art_fill", "value": "#14101F", "role": "the breakout demo's art panel fill"},
    {"name": "art_accent", "source": "src/ui/nk_ui_widgets.c", "symbol": "NK_UI_ACCENT_RGBA", "value": "#6B9FF0", "role": "the N1 mark and showcase pictures; a copy of accent_dark"},
    {"name": "art_paper", "source": "src/ui/nk_ui_widgets.c", "symbol": "NK_UI_PAPER_RGBA", "value": "#F5F7FA", "role": "the N1 mark and showcase pictures; a copy of paper"},
    {"name": "showcase_teal", "source": "src/ui/nk_ui_widgets.c", "symbol": "g_showcase_scene, g_showcase_breakout", "value": "#3DD6C6", "role": "showcase pictures"},
    {"name": "showcase_violet", "source": "src/ui/nk_ui_widgets.c", "symbol": "g_showcase_scene", "value": "#8B7CFF", "role": "showcase picture"},
    {"name": "showcase_red", "source": "src/ui/nk_ui_widgets.c", "symbol": "g_showcase_breakout", "value": "#FF5A5F", "role": "showcase picture"},
    {"name": "empty_disc_accent", "source": "src/player/ui/scr_library.c", "symbol": "library_empty", "value": "#6B9FF0", "role": "the empty library's disc icon; a copy of accent_dark"},
    {"name": "empty_disc_hole", "source": "src/player/ui/scr_library.c", "symbol": "library_empty", "value": "#18202B", "role": "the empty library's disc icon; a copy of surface_high"},
    {"name": "wave_base_0", "source": "src/ui/nk_ui_waves.c", "symbol": "g_base_colors", "value": "#5E9CC2", "role": "ribbon colour"},
    {"name": "wave_base_1", "source": "src/ui/nk_ui_waves.c", "symbol": "g_base_colors", "value": "#2F6FD0", "role": "ribbon colour"},
    {"name": "wave_base_2", "source": "src/ui/nk_ui_waves.c", "symbol": "g_base_colors", "value": "#6B9FF0", "role": "ribbon colour"},
    {"name": "wave_base_3", "source": "src/ui/nk_ui_waves.c", "symbol": "g_base_colors", "value": "#A9C8F7", "role": "ribbon colour"},
    {"name": "wave_month_01", "source": "src/ui/nk_ui_waves.c", "symbol": "g_month_tints", "value": "#5E8CB8", "role": "January's tint, mixed half into each ribbon"},
    {"name": "wave_month_02", "source": "src/ui/nk_ui_waves.c", "symbol": "g_month_tints", "value": "#5894C4", "role": "month tint"},
    {"name": "wave_month_03", "source": "src/ui/nk_ui_waves.c", "symbol": "g_month_tints", "value": "#4F9CCB", "role": "month tint"},
    {"name": "wave_month_04", "source": "src/ui/nk_ui_waves.c", "symbol": "g_month_tints", "value": "#4BA3C9", "role": "month tint"},
    {"name": "wave_month_05", "source": "src/ui/nk_ui_waves.c", "symbol": "g_month_tints", "value": "#59A9DD", "role": "month tint"},
    {"name": "wave_month_06", "source": "src/ui/nk_ui_waves.c", "symbol": "g_month_tints", "value": "#6AA9EC", "role": "month tint"},
    {"name": "wave_month_07", "source": "src/ui/nk_ui_waves.c", "symbol": "g_month_tints", "value": "#6B9FF0", "role": "month tint"},
    {"name": "wave_month_08", "source": "src/ui/nk_ui_waves.c", "symbol": "g_month_tints", "value": "#84AEF2", "role": "month tint"},
    {"name": "wave_month_09", "source": "src/ui/nk_ui_waves.c", "symbol": "g_month_tints", "value": "#7C96DE", "role": "month tint"},
    {"name": "wave_month_10", "source": "src/ui/nk_ui_waves.c", "symbol": "g_month_tints", "value": "#6F86C8", "role": "month tint"},
    {"name": "wave_month_11", "source": "src/ui/nk_ui_waves.c", "symbol": "g_month_tints", "value": "#6479AE", "role": "month tint"},
    {"name": "wave_month_12", "source": "src/ui/nk_ui_waves.c", "symbol": "g_month_tints", "value": "#5F7F9F", "role": "month tint"},
    {"name": "theme_wave_accent", "source": "src/player/ui/launcher_themes.c", "symbol": "LAUNCHER_THEME_WAVE_ACCENT", "value": "#4CC4E8", "role": "the unlocked launcher theme's seed accent (sea spray)"},
    {"name": "theme_wave_ground", "source": "src/player/ui/launcher_themes.c", "symbol": "LAUNCHER_THEME_WAVE_GROUND", "value": "#061A22", "role": "the unlocked launcher theme's seed page colour (deep sea)"},
    {"name": "theme_derive_tone", "source": "src/player/ui/launcher_themes.c", "symbol": "launcher_theme_derive", "value": "#C8D2DC", "role": "the blue-grey a derived theme's surfaces step toward, mixed with its accent"},
    {"name": "theme_derive_paper", "source": "src/player/ui/launcher_themes.c", "symbol": "launcher_theme_derive", "value": "#F5F7FA", "role": "a derived theme's starting text colour, moved until it keeps 7:1"},
    {"name": "theme_contrast_white", "source": "src/player/ui/launcher_themes.c", "symbol": "RGBA_WHITE", "value": "#FFFFFF", "role": "the lightest end a derived colour is moved toward for contrast"},
    {"name": "theme_contrast_black", "source": "src/player/ui/launcher_themes.c", "symbol": "RGBA_BLACK", "value": "#000000", "role": "the darkest end a derived colour is moved toward for contrast"}
  ],
  "metrics": [
    {"name": "NK_UI_SPACE_1", "source": "src/ui/nk_ui_theme.h", "value": 4, "role": "spacing step, px"},
    {"name": "NK_UI_SPACE_2", "source": "src/ui/nk_ui_theme.h", "value": 8, "role": "spacing step, px"},
    {"name": "NK_UI_SPACE_3", "source": "src/ui/nk_ui_theme.h", "value": 12, "role": "spacing step, px"},
    {"name": "NK_UI_SPACE_4", "source": "src/ui/nk_ui_theme.h", "value": 16, "role": "spacing step, px"},
    {"name": "NK_UI_SPACE_5", "source": "src/ui/nk_ui_theme.h", "value": 20, "role": "spacing step, px"},
    {"name": "NK_UI_SPACE_6", "source": "src/ui/nk_ui_theme.h", "value": 24, "role": "spacing step, px"},
    {"name": "NK_UI_SPACE_7", "source": "src/ui/nk_ui_theme.h", "value": 32, "role": "spacing step, px"},
    {"name": "NK_UI_SPACE_8", "source": "src/ui/nk_ui_theme.h", "value": 48, "role": "spacing step, px"},
    {"name": "NK_UI_SPACE_9", "source": "src/ui/nk_ui_theme.h", "value": 64, "role": "spacing step, px"},
    {"name": "NK_UI_RADIUS_CHIP", "source": "src/ui/nk_ui_theme.h", "value": 4, "role": "radius: pills, badges, keycap glyphs"},
    {"name": "NK_UI_RADIUS_CONTROL", "source": "src/ui/nk_ui_theme.h", "value": 8, "role": "radius: buttons, rows, segmented controls"},
    {"name": "NK_UI_RADIUS_TILE", "source": "src/ui/nk_ui_theme.h", "value": 12, "role": "radius: game cards, panels, the hero"},
    {"name": "NK_UI_RADIUS_DIALOG", "source": "src/ui/nk_ui_theme.h", "value": 16, "role": "radius: dialogs and sheets"},
    {"name": "NK_UI_RADIUS_PILL", "source": "src/ui/nk_ui_theme.h", "value": 999, "role": "radius: fully round ends, clamped to half the height"},
    {"name": "NK_UI_BORDER_HAIRLINE", "source": "src/ui/nk_ui_theme.h", "value": 1, "role": "edge width, px"},
    {"name": "NK_UI_FOCUS_RING_WIDTH", "source": "src/ui/nk_ui_theme.h", "value": 3, "role": "focus ring width, px"},
    {"name": "NK_UI_FOCUS_RING_GAP", "source": "src/ui/nk_ui_theme.h", "value": 3, "role": "gap between an element and its ring, px"},
    {"name": "NK_UI_FOCUS_HC_RING_WIDTH", "source": "src/ui/nk_ui_theme.h", "value": 4, "role": "high-contrast white ring width, px"},
    {"name": "NK_UI_FOCUS_HC_KEYLINE", "source": "src/ui/nk_ui_theme.h", "value": 2, "role": "high-contrast black keyline width, px"},
    {"name": "NK_UI_FOCUS_RING_OUTSET", "source": "src/ui/nk_ui_theme.h", "value": 8, "role": "how far the wider ring reaches outside its control; a scroll into view shows this much around it, px"},
    {"name": "NK_UI_CONTROL_HEIGHT", "source": "src/ui/nk_ui_theme.h", "value": 44, "role": "button and row height; the minimum target"},
    {"name": "NK_UI_CONTROL_HEIGHT_LARGE", "source": "src/ui/nk_ui_theme.h", "value": 52, "role": "large button height"},
    {"name": "NK_UI_CONTROL_HEIGHT_SMALL", "source": "src/ui/nk_ui_theme.h", "value": 34, "role": "small button height"},
    {"name": "NK_UI_CHIP_HEIGHT", "source": "src/ui/nk_ui_theme.h", "value": 26, "role": "pill and tag height"},
    {"name": "NK_UI_BADGE_HEIGHT", "source": "src/ui/nk_ui_theme.h", "value": 20, "role": "badge height"},
    {"name": "NK_UI_GLYPH_SIZE", "source": "src/ui/nk_ui_theme.h", "value": 24, "role": "prompt glyph size"},
    {"name": "NK_UI_TOP_BAR_HEIGHT", "source": "src/ui/nk_ui_theme.h", "value": 64, "role": "top bar height"},
    {"name": "NK_UI_PROMPT_BAR_HEIGHT", "source": "src/ui/nk_ui_theme.h", "value": 56, "role": "prompt bar height"},
    {"name": "NK_UI_GUTTER", "source": "src/ui/nk_ui_theme.h", "value": 32, "role": "page side margin"},
    {"name": "NK_UI_GUTTER_NARROW", "source": "src/ui/nk_ui_theme.h", "value": 16, "role": "page side margin below NK_UI_NARROW_WIDTH"},
    {"name": "NK_UI_NARROW_WIDTH", "source": "src/ui/nk_ui_theme.h", "value": 900, "role": "logical width below which the gutter narrows"},
    {"name": "NK_UI_BODY_PAD_TOP", "source": "src/ui/nk_ui_theme.h", "value": 24, "role": "body padding above"},
    {"name": "NK_UI_BODY_PAD_BOTTOM", "source": "src/ui/nk_ui_theme.h", "value": 16, "role": "body padding below"},
    {"name": "NK_UI_TILE_MIN_WIDTH", "source": "src/ui/nk_ui_theme.h", "value": 236, "role": "narrowest game card; the library's columns are how many fit"},
    {"name": "NK_UI_TILE_GAP", "source": "src/ui/nk_ui_theme.h", "value": 20, "role": "gap between game cards"},
    {"name": "NK_UI_TILE_ROW_GAP", "source": "src/ui/nk_ui_theme.h", "value": 16, "role": "gap between rows of game cards"},
    {"name": "NK_UI_TILE_MAX_COLUMNS", "source": "src/ui/nk_ui_theme.h", "value": 6, "role": "most library columns"},
    {"name": "NK_UI_TILE_ART_RATIO", "source": "src/ui/nk_ui_theme.h", "value": 0.53, "role": "game card art height / card width"},
    {"name": "NK_UI_TILE_BODY_HEIGHT", "source": "src/ui/nk_ui_theme.h", "value": 90, "role": "game card body at the 100% text size"},
    {"name": "NK_UI_DETAIL_ASIDE_WIDTH", "source": "src/ui/nk_ui_theme.h", "value": 520, "role": "title page hero column"},
    {"name": "NK_UI_DETAIL_GAP", "source": "src/ui/nk_ui_theme.h", "value": 32, "role": "title page column gap"},
    {"name": "NK_UI_HERO_ASPECT", "source": "src/ui/nk_ui_theme.h", "value": "NK_PSP_DISPLAY_HEIGHT_F / NK_PSP_DISPLAY_WIDTH_F", "role": "hero art height / width, the PSP screen's 272:480"},
    {"name": "NK_UI_SAFE_AREA_MIN", "source": "src/ui/nk_ui_theme.h", "value": 16, "role": "nothing closer to a window edge"},
    {"name": "NK_UI_MOTION_INSTANT", "source": "src/ui/nk_ui_theme.h", "value": 0, "role": "a cut, s"},
    {"name": "NK_UI_MOTION_FAST", "source": "src/ui/nk_ui_theme.h", "value": 0.12, "role": "focus ring move, hover and press fills, s"},
    {"name": "NK_UI_MOTION_BASE", "source": "src/ui/nk_ui_theme.h", "value": 0.2, "role": "tab rule slide, s"},
    {"name": "NK_UI_MOTION_SLOW", "source": "src/ui/nk_ui_theme.h", "value": 0.5, "role": "art fade, s"},
    {"name": "NK_UI_MOTION_ART_REST", "source": "src/ui/nk_ui_theme.h", "value": 0.6, "role": "rest on a card before its backdrop art shows, s"},
    {"name": "NK_UI_MOTION_MUSIC_REST", "source": "src/ui/nk_ui_theme.h", "value": 1.1, "role": "reserved: preview music, which the player does not have, s"},
    {"name": "NK_UI_MOTION_MUSIC_FADE_IN", "source": "src/ui/nk_ui_theme.h", "value": 1.0, "role": "reserved: preview music fade in, s"},
    {"name": "NK_UI_MOTION_MUSIC_FADE_OUT", "source": "src/ui/nk_ui_theme.h", "value": 0.45, "role": "reserved: preview music fade out, s"},
    {"name": "NK_UI_MOTION_WAVE_FRAME", "source": "src/ui/nk_ui_theme.h", "value": "1.0 / 30.0", "role": "the waves step at most at 30 Hz, s"},
    {"name": "NK_UI_MOTION_FRAME", "source": "src/ui/nk_ui_theme.h", "value": "1.0 / 60.0", "role": "one 60 Hz frame given to a transition that starts after an idle frame, s"},
    {"name": "NK_UI_WAVES_RIBBON_COUNT", "source": "src/ui/nk_ui_waves.h", "value": 4, "role": "ribbons behind the launcher"},
    {"name": "NK_UI_WAVES_TINT_COUNT", "source": "src/ui/nk_ui_waves.h", "value": 12, "role": "month tints"},
    {"name": "NK_UI_WAVES_ALPHA_CAP_MAX", "source": "src/ui/nk_ui_waves.h", "value": 0.24, "role": "most alpha a ribbon may have"},
    {"name": "NK_UI_WAVES_LUMINANCE_MAX", "source": "src/ui/nk_ui_waves.h", "value": 0.0399, "role": "brightest luminance the waves and the backdrop may reach behind text"}
  ],
  "fonts": [
    {"name": "NK_UI_FONT_BODY", "slot": 0, "file": "assets/fonts/ui/geist/Geist-Regular.ttf"},
    {"name": "NK_UI_FONT_DISPLAY", "slot": 1, "file": "assets/fonts/ui/chakra-petch/ChakraPetch-SemiBold.ttf"},
    {"name": "NK_UI_FONT_MONO", "slot": 2, "file": "assets/fonts/ui/geist-mono/GeistMono-Regular.ttf"},
    {"name": "NK_UI_FONT_CJK", "slot": 3, "file": "assets/fonts/ui/noto-sans-jp/NotoSansJP-Regular.ttf"},
    {"name": "NK_UI_FONT_BODY_SEMIBOLD", "slot": 4, "file": "assets/fonts/ui/geist/Geist-SemiBold.ttf"},
    {"name": "NK_UI_FONT_BODY_MEDIUM", "slot": 5, "file": "assets/fonts/ui/geist/Geist-Medium.ttf"},
    {"name": "NK_UI_FONT_MONO_MEDIUM", "slot": 6, "file": "assets/fonts/ui/geist-mono/GeistMono-Medium.ttf"},
    {"name": "NK_UI_FONT_DISPLAY_BOLD", "slot": 7, "file": "assets/fonts/ui/chakra-petch/ChakraPetch-Bold.ttf"}
  ],
  "type": [
    {"name": "DISPLAY_XL", "font": "NK_UI_FONT_DISPLAY_BOLD", "size_px": 40, "line_px": 44, "tracking_em": 0.04, "wraps": false, "role": "first-run headline, About's name"},
    {"name": "DISPLAY", "font": "NK_UI_FONT_DISPLAY_BOLD", "size_px": 30, "line_px": 32, "tracking_em": 0.04, "wraps": false, "role": "screen names"},
    {"name": "WORDMARK", "font": "NK_UI_FONT_DISPLAY_BOLD", "size_px": 18, "line_px": 20, "tracking_em": 0.06, "wraps": false, "role": "the top bar's wordmark"},
    {"name": "TITLE_XL", "font": "NK_UI_FONT_BODY_SEMIBOLD", "size_px": 30, "line_px": 34, "tracking_em": 0, "wraps": true, "role": "a game's name on its page, a wizard step's title"},
    {"name": "TITLE", "font": "NK_UI_FONT_BODY_SEMIBOLD", "size_px": 22, "line_px": 28, "tracking_em": 0, "wraps": true, "role": "dialog and panel titles"},
    {"name": "HEADING", "font": "NK_UI_FONT_BODY_SEMIBOLD", "size_px": 17, "line_px": 24, "tracking_em": 0, "wraps": true, "role": "card headings"},
    {"name": "TILE_TITLE", "font": "NK_UI_FONT_BODY_SEMIBOLD", "size_px": 15, "line_px": 20, "tracking_em": 0, "wraps": true, "role": "game card names"},
    {"name": "BODY_L", "font": "NK_UI_FONT_BODY", "size_px": 17, "line_px": 26, "tracking_em": 0, "wraps": true, "role": "lead paragraphs"},
    {"name": "BODY", "font": "NK_UI_FONT_BODY", "size_px": 15, "line_px": 22, "tracking_em": 0, "wraps": true, "role": "running text"},
    {"name": "BODY_S", "font": "NK_UI_FONT_BODY", "size_px": 14, "line_px": 21, "tracking_em": 0, "wraps": true, "role": "card paragraphs"},
    {"name": "CAPTION", "font": "NK_UI_FONT_BODY", "size_px": 13, "line_px": 19, "tracking_em": 0, "wraps": true, "role": "hints, counts, notes"},
    {"name": "FOOTNOTE", "font": "NK_UI_FONT_BODY", "size_px": 12, "line_px": 16, "tracking_em": 0, "wraps": true, "role": "sources and fine print"},
    {"name": "LABEL", "font": "NK_UI_FONT_MONO_MEDIUM", "size_px": 12, "line_px": 16, "tracking_em": 0.08, "wraps": false, "role": "buttons, prompts, tabs"},
    {"name": "LABEL_L", "font": "NK_UI_FONT_MONO_MEDIUM", "size_px": 14, "line_px": 18, "tracking_em": 0.12, "wraps": false, "role": "the large button"},
    {"name": "LABEL_S", "font": "NK_UI_FONT_MONO_MEDIUM", "size_px": 11, "line_px": 14, "tracking_em": 0.06, "wraps": false, "role": "chips, card meta rows, small buttons"},
    {"name": "EYEBROW", "font": "NK_UI_FONT_MONO_MEDIUM", "size_px": 11, "line_px": 14, "tracking_em": 0.12, "wraps": false, "role": "section headers"},
    {"name": "MICRO", "font": "NK_UI_FONT_MONO_MEDIUM", "size_px": 10, "line_px": 12, "tracking_em": 0.08, "wraps": false, "role": "badges and notes over art"},
    {"name": "DATA", "font": "NK_UI_FONT_MONO", "size_px": 12, "line_px": 16, "tracking_em": 0, "wraps": false, "role": "disc IDs, figures, paths, log lines"}
  ],
  "backgrounds": [
    {"name": "ground", "colour": "surface", "role": "the page, and ground-filled boxes inside cards"},
    {"name": "wave_limit", "luminance": "NK_UI_WAVES_LUMINANCE_MAX", "role": "the ground under the brightest waves or library backdrop allowed"},
    {"name": "panel", "colour": "surface_raised", "role": "cards, dialogs, flow cards, bars, the in-game overlay's card"},
    {"name": "control", "colour": "surface_high", "role": "pills, tags, focused or hovered controls and rows"},
    {"name": "pressed", "colour": "pressed", "role": "a control or card while pressed"},
    {"name": "pressed_danger", "colour": "pressed_danger", "role": "a danger button while pressed"},
    {"name": "accent_fill", "colour": "accent_dark", "role": "primary button, lit option, current step"},
    {"name": "accent_hover_fill", "colour": "accent_hover", "role": "primary button under the pointer"},
    {"name": "accent_pressed_fill", "colour": "accent_pressed", "role": "pressed primary button"},
    {"name": "log_box", "colour": "log_box_build", "role": "the build and developer log boxes"},
    {"name": "badge_over_white_art", "colour": "art_scrim", "over": "#FFFFFF", "role": "a badge over the brightest possible title art"},
    {"name": "scrim_over_white_frame", "colour": "scrim", "over": "#FFFFFF", "role": "the in-game overlay's scrim over a white game frame"},
    {"name": "art_tint_0", "colour": "art_tint_0", "role": "a title's art panel"},
    {"name": "art_tint_1", "colour": "art_tint_1", "role": "a title's art panel"},
    {"name": "art_tint_2", "colour": "art_tint_2", "role": "a title's art panel"},
    {"name": "art_tint_3", "colour": "art_tint_3", "role": "a title's art panel"},
    {"name": "art_showcase_scene", "colour": "art_showcase_scene", "role": "the scene demo's art panel"},
    {"name": "art_showcase_breakout", "colour": "art_showcase_breakout", "role": "the breakout demo's art panel"},
    {"name": "hc_keyline", "colour": "hc_keyline_black", "role": "the high-contrast ring's keylines"}
  ],
  "pairs": [
    {"text": "paper", "on": "ground", "kind": "text", "roles": ["DISPLAY", "TITLE_XL", "BODY", "DATA", "LABEL"], "where": "screen names and text on the page; secondary button labels (nk_ui_button_text_rgba); text in ground boxes (scr_error.c details, scr_form.c wizard boxes)"},
    {"text": "paper", "on": "wave_limit", "kind": "text", "roles": ["DISPLAY", "TITLE_XL", "BODY", "DATA", "LABEL"], "where": "the same text on the page over the waves or backdrop"},
    {"text": "muted", "on": "ground", "kind": "text", "roles": ["BODY_S", "CAPTION", "EYEBROW", "DATA", "LABEL"], "where": "captions and eyebrows on the page; tab labels (nk_ui_section_tabs); option chips and stepper values (scr_settings.c); details and file lists in ground boxes"},
    {"text": "muted", "on": "wave_limit", "kind": "text", "roles": ["CAPTION", "EYEBROW", "LABEL"], "where": "captions, eyebrows and tab labels on the page over the waves or backdrop"},
    {"text": "dim", "on": "ground", "kind": "text", "roles": ["FOOTNOTE", "CAPTION", "DATA", "LABEL"], "where": "About's source commit and data folder (scr_about.c); the update link and the copied file in ground boxes (scr_settings.c, scr_wizard.c); disabled button labels (nk_ui_button_text_rgba); a dimmed row's chips and arrows (scr_settings.c)"},
    {"text": "dim", "on": "wave_limit", "kind": "text", "roles": ["FOOTNOTE"], "where": "About's source commit and data folder over the waves"},
    {"text": "error", "on": "ground", "kind": "text", "roles": ["LABEL"], "where": "danger button labels on the page (nk_ui_button_text_rgba)"},
    {"text": "error", "on": "wave_limit", "kind": "text", "roles": ["LABEL"], "where": "danger button labels over the waves"},
    {"text": "info", "on": "ground", "kind": "text", "roles": ["CAPTION"], "where": "the title page's tier note in its ground box (scr_title.c)"},
    {"text": "paper", "on": "panel", "kind": "text", "roles": ["TITLE", "HEADING", "TILE_TITLE", "BODY", "BODY_S", "DATA", "LABEL"], "where": "cards, dialogs, flow cards, the prompt bar's labels and glyphs, the in-game overlay's card"},
    {"text": "muted", "on": "panel", "kind": "text", "roles": ["BODY", "BODY_S", "CAPTION", "EYEBROW", "DATA", "LABEL_S"], "where": "dialog messages (scr_error.c), card captions, the prompt bar's hint, the input chip, eyebrows in cards"},
    {"text": "dim", "on": "panel", "kind": "text", "roles": ["FOOTNOTE", "CAPTION", "DATA", "LABEL_S"], "where": "a game card's disc ID (nk_ui_tile); the empty library's note (scr_library.c); the developer line and credits note in cards (scr_title.c, scr_about.c); overlay footnotes (overlay_pause.c, overlay_quick.c, overlay_dev.c); the download's per-item line (scr_prereq.c); disabled buttons on a card"},
    {"text": "accent_dark", "on": "panel", "kind": "text", "roles": ["EYEBROW", "LABEL_L", "BODY_S"], "where": "flow-card eyebrows (scr_flow_eyebrow callers); About's version line (scr_about.c); the building row (scr_build.c); the noted control (scr_controller.c)"},
    {"text": "ready", "on": "panel", "kind": "text", "roles": ["EYEBROW", "BODY_S", "LABEL_S"], "where": "a game card's Ready word (nk_ui_tile); a written problem report (scr_settings.c); the overlay's controller-connected eyebrow"},
    {"text": "warning", "on": "panel", "kind": "text", "roles": ["EYEBROW", "BODY_S", "CAPTION", "LABEL_S"], "where": "developer mode (scr_devlog.c); a report that failed (scr_settings.c); the overlay's quit and controller eyebrows; a card's needs-setup word"},
    {"text": "info", "on": "panel", "kind": "text", "roles": ["EYEBROW", "LABEL_S"], "where": "flow eyebrows (scr_prereq.c, scr_rebuild.c, scr_form.c); a card's experimental or untested word"},
    {"text": "error", "on": "panel", "kind": "text", "roles": ["EYEBROW", "BODY_S", "LABEL"], "where": "the error card's eyebrow (scr_error.c); a conflicting control (scr_controller.c); danger buttons in cards"},
    {"text": "paper", "on": "control", "kind": "text", "roles": ["BODY", "LABEL", "LABEL_S"], "where": "focused or hovered secondary buttons (nk_ui_button_between), rows, tabs and selected chips"},
    {"text": "muted", "on": "control", "kind": "text", "roles": ["BODY", "LABEL", "LABEL_S"], "where": "tags (nk_ui_tag); a dimmed row's name and selected chip; steps not reached (scr_wizard.c, scr_build.c)"},
    {"text": "dim", "on": "control", "kind": "text", "roles": ["LABEL", "LABEL_S"], "where": "a disabled primary button (nk_ui_button_configuration); a hovered or focused game card's disc ID"},
    {"text": "ready", "on": "control", "kind": "text", "roles": ["LABEL_S"], "where": "Ready pills (nk_ui_status_pill); done steps (scr_wizard.c, scr_build.c)"},
    {"text": "warning", "on": "control", "kind": "text", "roles": ["LABEL_S"], "where": "needs-setup pills; the error card's failed-stage pill (scr_error.c)"},
    {"text": "info", "on": "control", "kind": "text", "roles": ["LABEL_S"], "where": "experimental and untested pills"},
    {"text": "error", "on": "control", "kind": "text", "roles": ["LABEL", "LABEL_S"], "where": "unsupported pills; a hovered or focused danger button"},
    {"text": "accent_dark", "on": "control", "kind": "text", "roles": ["LABEL"], "where": "a focused item on the overlay's developer pages (overlay_dev.c)"},
    {"text": "paper", "on": "pressed", "kind": "text", "roles": ["LABEL", "TILE_TITLE"], "where": "a pressed secondary button, tab, row or game card"},
    {"text": "muted", "on": "pressed", "kind": "text", "roles": ["TILE_TITLE", "LABEL_S"], "where": "a pressed game card's muted name or meta word"},
    {"text": "dim", "on": "pressed", "kind": "text", "roles": ["LABEL_S"], "where": "a pressed game card's disc ID"},
    {"text": "ready", "on": "pressed", "kind": "text", "roles": ["LABEL_S"], "where": "a pressed game card's status word"},
    {"text": "warning", "on": "pressed", "kind": "text", "roles": ["LABEL_S"], "where": "a pressed game card's status word"},
    {"text": "info", "on": "pressed", "kind": "text", "roles": ["LABEL_S"], "where": "a pressed game card's status word"},
    {"text": "error", "on": "pressed", "kind": "text", "roles": ["LABEL_S"], "where": "a pressed game card's status word"},
    {"text": "error", "on": "pressed_danger", "kind": "text", "roles": ["LABEL"], "where": "a pressed danger button"},
    {"text": "ink", "on": "accent_fill", "kind": "text", "roles": ["LABEL", "LABEL_L", "LABEL_S"], "where": "primary buttons (nk_ui_button_text_rgba), lit options (scr_settings.c), the current step (scr_wizard.c, scr_build.c)"},
    {"text": "ink", "on": "accent_hover_fill", "kind": "text", "roles": ["LABEL", "LABEL_L"], "where": "a primary button or lit option under the pointer"},
    {"text": "ink", "on": "accent_pressed_fill", "kind": "text", "roles": ["LABEL", "LABEL_L"], "where": "a pressed primary button or lit option"},
    {"text": "muted", "on": "log_box", "kind": "text", "roles": ["DATA"], "where": "build and developer log lines (scr_build.c, scr_devlog.c)"},
    {"text": "paper", "on": "badge_over_white_art", "kind": "text", "roles": ["MICRO"], "where": "a badge over title art (nk_ui_badge)"},
    {"text": "art_label", "on": "art_tint_0", "kind": "text", "roles": ["MICRO"], "where": "the hero's size note (nk_ui_hero)"},
    {"text": "art_label", "on": "art_tint_1", "kind": "text", "roles": ["MICRO"], "where": "the hero's size note"},
    {"text": "art_label", "on": "art_tint_2", "kind": "text", "roles": ["MICRO"], "where": "the hero's size note"},
    {"text": "art_label", "on": "art_tint_3", "kind": "text", "roles": ["MICRO"], "where": "the hero's size note"},
    {"text": "art_label", "on": "art_showcase_scene", "kind": "text", "roles": ["MICRO"], "where": "the hero's size note on a showcase demo"},
    {"text": "art_label", "on": "art_showcase_breakout", "kind": "text", "roles": ["MICRO"], "where": "the hero's size note on a showcase demo"},
    {"text": "dim", "on": "art_tint_0", "kind": "text", "roles": ["MICRO"], "where": "the art placeholder's caption (nk_ui_art_content)"},
    {"text": "dim", "on": "art_tint_1", "kind": "text", "roles": ["MICRO"], "where": "the art placeholder's caption"},
    {"text": "dim", "on": "art_tint_2", "kind": "text", "roles": ["MICRO"], "where": "the art placeholder's caption"},
    {"text": "dim", "on": "art_tint_3", "kind": "text", "roles": ["MICRO"], "where": "the art placeholder's caption"},
    {"text": "focus", "on": "ground", "kind": "focus", "where": "the ring around a control on the page (nk_ui_focus_ring)"},
    {"text": "focus", "on": "wave_limit", "kind": "focus", "where": "the ring over the brightest waves or backdrop allowed"},
    {"text": "focus", "on": "panel", "kind": "focus", "where": "the ring around a control in a card, dialog or bar"},
    {"text": "focus", "on": "control", "kind": "focus", "where": "the ring beside a focused or hovered neighbour"},
    {"text": "focus", "on": "scrim_over_white_frame", "kind": "focus", "where": "the overlay's ring where it meets the scrim over a white game frame"},
    {"text": "hc_ring_white", "on": "hc_keyline", "kind": "focus", "where": "the high-contrast ring between its keylines (nk_ui_focus_ring)"},
    {"text": "selected_neutral_edge", "on": "ground", "kind": "edge", "where": "a selected Off or the library's value, against the list (settings_enhanced_chip)"},
    {"text": "selected_neutral_edge", "on": "control", "kind": "edge", "where": "that edge against its own box"},
    {"text": "selected_neutral_edge_dimmed", "on": "ground", "kind": "edge", "where": "a dimmed row's current value, against the list"},
    {"text": "selected_neutral_edge_dimmed", "on": "control", "kind": "edge", "where": "that edge against its own box"}
  ]
}
```

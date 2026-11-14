<!-- markdownlint-disable-file MD041 -- the centred header starts with a div -->
<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/images/brand/nakagawa-mark-dark.svg">
  <img src="docs/images/brand/nakagawa-mark-light.svg" width="96" height="96" alt="Nakagawa mark">
</picture>

# Nakagawa Recomp

**Play PSP games you own as native Windows programs.**

**[Download](https://github.com/Jstar269/Nakagawa-Recomp/releases/tag/v0.0.2) · [Website](https://jstar269.github.io/Nakagawa-Recomp/) · [Compatibility](docs/COMPATIBILITY.md) · [Discussions](https://github.com/Jstar269/Nakagawa-Recomp/discussions)**

[![CI](https://github.com/Jstar269/Nakagawa-Recomp/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/Jstar269/Nakagawa-Recomp/actions/workflows/ci.yml)
[![License: GPL-3.0-or-later](https://img.shields.io/badge/license-GPL--3.0--or--later-2F6FD0.svg)](LICENSE)
[![Platform: Windows 11 x64](https://img.shields.io/badge/platform-Windows%2011%20x64-2F6FD0.svg)](docs/SETUP.md)

</div>

Nakagawa Recomp is a static recompiler for the PlayStation Portable. It translates a game's MIPS code into C once,
before you play, and compiles it into a native Windows program. It isn't an emulator. You supply the game from your
own disc: Nakagawa includes no games, game data, firmware or system fonts.

**What works today:** v0.0.2 is an experimental pre-release. *Hot Shots Tennis: Get a Grip* is playable, with full
matches and Story Mode. It runs in **Authentic** mode, as released, or in **Enhanced** mode, which adds options such
as texture filtering, Smooth motion and Widescreen. *PAC-MAN Championship Edition* plays a stage, about a dozen more
games start and get part of the way in, and most others stop early and name what is missing.

**Try it:** [download v0.0.2](#download), extract the ZIP, start `bin\nakagawa_player.exe`, drag in your game's
`.iso`, then press **BUILD PACKAGE** and **PLAY**. Without a game of your own, the library's **TEST THIS PC** row has
two demos the project wrote, which need no game files.

[![A rally in Hot Shots Tennis: Get a Grip, running natively in Nakagawa Recomp; play the clip](docs/media/hst-rally-poster.png)](https://jstar269.github.io/Nakagawa-Recomp/media/hst-rally.mp4)

[Watch a 28-second rally with sound](https://jstar269.github.io/Nakagawa-Recomp/media/hst-rally.mp4) (MP4, 4.5 MB),
recorded with v0.0.1. The [v0.0.2 release page](https://github.com/Jstar269/Nakagawa-Recomp/releases/tag/v0.0.2) has
screenshots of Enhanced mode.

<div align="center"><sub>Hot Shots Tennis: Get a Grip belongs to its publisher. This repository contains no game data: you need your own copy of the game, and nothing here downloads one.</sub></div>

## Download

[Download v0.0.2](https://github.com/Jstar269/Nakagawa-Recomp/releases/tag/v0.0.2), the latest release: one 260 MB
ZIP for Windows 11, 64-bit. It includes the build tools, so there is nothing else to install.

1. Extract the ZIP and start `bin\nakagawa_player.exe`.
2. Drag your game's `.iso` into the player window, or choose **START SETUP WIZARD**. Nakagawa decrypts your own disc
   on your PC, the way PPSSPP does. It never touches firmware.
3. Press **BUILD PACKAGE**, then **PLAY**. A first build takes several minutes.

The ZIP isn't code-signed, so Windows may warn you the first time. The
[release notes](docs/RELEASE_NOTES_v0.0.2.md) say what works, what doesn't, and
[how to check the download](docs/RELEASE_NOTES_v0.0.2.md#checking-your-download).

**Updating from v0.0.1:** extract v0.0.2 into a new folder. It finds your library, saves and settings, and
**Rebuild all** in the library rebuilds the games v0.0.1 built. Settings > **CHECK FOR UPDATES** asks GitHub for a
newer version when you select it and links to its release page.

You need:

- Windows 11, 64-bit, with a graphics card that supports Vulkan.
- 8 GB of RAM (16 GB recommended) and 5 GB of free disk space, ideally on an SSD
  ([system requirements](docs/SETUP.md#system-requirements)).
- Your own copy of the game as an uncompressed `.iso` disc image, or the `EBOOT.PBP` of a PSN release of a UMD game.
  The player also takes a compressed `.cso`, `.zso` or `.chd` image and expands it once, when you add it, into a
  plain `.iso` in its data folder, so leave room for the full `.iso` ([details](docs/SETUP.md#compressed-disc-images)).
  If a file can't be decrypted, [`docs/YOUR_OWN_GAMES.md`](docs/YOUR_OWN_GAMES.md) says what to do.

## Compatibility

| Game | Rating | Notes |
| --- | --- | --- |
| *Hot Shots Tennis: Get a Grip* | Playable | Full matches and Story Mode play. Its title file ships with Nakagawa. On the reference PC, a 2026-10-27 measurement of current main ([#1296](https://github.com/Jstar269/nakagawa-recomp/issues/1296)) ran at about 29.5 of the game's own 30 frames per second in matches and 29.2 to 29.6 in menus. Sound effects and text clicks can drop out while the game loads. The opening movie shows black in v0.0.1 and plays from v0.0.2. |
| PAC-MAN Championship Edition | In-game | Reaches its menu, plays a stage and resumes from its own save, through general fixes with no game-specific code ([#552](https://github.com/Jstar269/nakagawa-recomp/pull/552), [#556](https://github.com/Jstar269/nakagawa-recomp/pull/556), [#1126](https://github.com/Jstar269/nakagawa-recomp/issues/1126)). The rest of the game hasn't been tried. |
| *Super Stardust Portable* | Title screen | Goes through its menus to difficulty select, then stops by name at a VFPU instruction Nakagawa doesn't support yet ([#326](https://github.com/Jstar269/Nakagawa-Recomp/issues/326)). |
| Eleven more games, *LittleBigPlanet* among them | No tier | Seen to start or run, in the project's testing or in user reports on v0.0.1, but not checked by the maintainer. Several reach menus or gameplay, most well below full speed. [`docs/COMPATIBILITY.md`](docs/COMPATIBILITY.md) says how far each one gets. |
| Any other PSP game | No tier | The player imports it, builds a package and names anything that isn't supported. For most games it stops at one of those named limits ([#308](https://github.com/Jstar269/Nakagawa-Recomp/issues/308)). |
| Two showcase demos | Playable | Small PSP programs the project wrote as tests. They ship with the release and need no game files. |

Ratings use the player's words. Hot Shots Tennis and the two showcase demos are rated from the player's own record;
every other rating comes from the game's status in [`docs/COMPATIBILITY.md`](docs/COMPATIBILITY.md#status-words),
which has the full detail for every game and every part of the PSP. To report how a game runs, post in
[Compatibility reports](https://github.com/Jstar269/Nakagawa-Recomp/discussions/categories/compatibility-reports).

## How it works

Nakagawa translates the game's MIPS code into C ahead of time and compiles it together with a runtime that stands in
for the PSP's system software, graphics and sound. The player is written in C with SDL3 and Vulkan.

- Every game has two play modes. **Authentic** plays it as released and is the default. **Enhanced** adds options
  that work for any game, such as texture filtering, Smooth motion, Widescreen and sharper enlarging with AMD FSR 1,
  and each one can be turned off.
- When a game needs something that isn't supported yet, the player stops and names the missing piece.
- No games, firmware or Sony system fonts ship here, and no game data is ever downloaded. Games show the project's
  own open fonts ([#313](https://github.com/Jstar269/nakagawa-recomp/issues/313)) unless you import the fonts from
  your own PSP in Settings > **PSP FONTS**, or with `python tools/nk_cli.py fonts import <folder>`
  ([how to copy them](docs/SETUP.md#copying-the-fonts-from-your-psp)).
- The player works offline. It connects to the internet only when you agree: to fetch build tools when they
  aren't already present, and to check for updates when you ask ([network use](docs/SETUP.md#network-use)).

[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md#pipeline) describes the pipeline, and
[`docs/CODE_TOUR.md`](docs/CODE_TOUR.md) follows one game through the code.

## Build from source

You need Windows 11 x64 with a Vulkan GPU, PowerShell 7.6+, MSYS2 UCRT64, CPython 3.15, the Vulkan SDK and Git.
[`docs/SETUP.md`](docs/SETUP.md#prerequisites) lists the exact packages and the setup steps.

1. Build the player from a checkout: `mingw32-make player`.
2. Start `build\nakagawa_player.exe`, then drag a game's `.iso` into the window or choose **START SETUP WIZARD**.
   Later games go in through the **Add a disc image you own** tile in the library.
3. The player checks the disc. If the game's code is encrypted, Nakagawa decrypts your own disc on your PC into that
   game's folder. If a file can't be decrypted, the player names it and the reason, and you can add a decrypted copy
   from your own disc instead.
4. Press **BUILD PACKAGE**, then **PLAY**. If your PC doesn't have the build tools yet, consent once and the player
   downloads the exact versions it needs.

The release ZIP includes two showcase demos that need no game files. From a checkout,
`mingw32-make player showcase` builds them, and the player lists them on its own. On Windows this needs WSL 2 and
PSPDEV ([steps](docs/SETUP.md#showcase-demos-on-windows-wsl-2-and-pspdev)).

| ![Nakagawa 3D Showcase: a brightly textured cube rotating against a dark background in the recompiled demo](docs/images/showcase-ge-scene.gif) | ![Nakagawa Breakout Showcase: the ball moving through a colored brick field above the paddle and score](docs/images/showcase-breakout.gif) |
| :---: | :---: |
| **3D Showcase** | **Breakout Showcase** |

<div align="center"><sub>Two project-written PSP programs, recompiled and running at the PSP's native 480×272. No commercial game is shown in these two demos, and none is included.</sub></div>

## Matching decompilations

[![hst-decomp code matched](https://img.shields.io/badge/dynamic/json?url=https%3A%2F%2Fraw.githubusercontent.com%2FJstar269%2Fhst-decomp%2Fmain%2Fdocs%2Fobjdiff-report.json&query=%24.measures.matched_code_percent&suffix=%25&label=hst-decomp%20code%20matched&color=2F6FD0)](https://github.com/Jstar269/hst-decomp)

[hst-decomp](https://github.com/Jstar269/hst-decomp) rebuilds the code of Hot Shots Tennis: Get a Grip as C and C++
that compiles back to the original bytes. [lbp-psp-decomp](https://github.com/Jstar269/lbp-psp-decomp) does the same
for LittleBigPlanet on the PSP. Both are source-only and contain no game data.

No decomp source is copied into Nakagawa, which is meant for any PSP game. The decomp's function names can label the
C that Nakagawa generates ([#1495](https://github.com/Jstar269/Nakagawa-Recomp/pull/1495)), and what it shows about
the game helps replace Nakagawa's Hot Shots Tennis workarounds with general fixes
([#363](https://github.com/Jstar269/Nakagawa-Recomp/issues/363)).

Work belongs in the recompiler and runtime when its cause is general, such as a system call, an instruction, a
display-list command or a build step, because the fix then helps every game and can be tested with synthetic
fixtures. It belongs in a decompilation project only when it rebuilds one function's source so that it compiles back to
the original bytes, which needs your own copy of the game. If you are unsure, open an issue here and describe what you
saw.

## Contributing

Testing is the most useful help right now: try a game you own and
[report how far it gets](https://github.com/Jstar269/Nakagawa-Recomp/discussions/categories/compatibility-reports).
Bugs go in [issues](https://github.com/Jstar269/Nakagawa-Recomp/issues), and
[good first issues](https://github.com/Jstar269/Nakagawa-Recomp/labels/good%20first%20issue) are a place to start
on code. [`CONTRIBUTING.md`](CONTRIBUTING.md) has the rules and the checks to run. To carry the project on in a
fork of your own, start with [`docs/FORKING.md`](docs/FORKING.md).

[All documentation](docs/README.md) · [Accessibility](ACCESSIBILITY.md) · [Getting help](.github/SUPPORT.md) ·
[Security](SECURITY.md) · [Code of conduct](CODE_OF_CONDUCT.md) · [AI use](docs/AI_USAGE.md)

## Project health

[![CodeQL](https://github.com/Jstar269/Nakagawa-Recomp/actions/workflows/codeql.yml/badge.svg?branch=main)](https://github.com/Jstar269/Nakagawa-Recomp/actions/workflows/codeql.yml)
[![Coverage (Python tooling and native core tests)](https://codecov.io/gh/Jstar269/Nakagawa-Recomp/graph/badge.svg?branch=main)](https://app.codecov.io/gh/Jstar269/Nakagawa-Recomp)
[![OpenSSF Best Practices](https://www.bestpractices.dev/projects/15701/badge)](https://www.bestpractices.dev/projects/15701)
[![OpenSSF Baseline](https://www.bestpractices.dev/projects/15701/baseline)](https://www.bestpractices.dev/projects/15701)
[![OpenSSF Scorecard](https://api.scorecard.dev/projects/github.com/Jstar269/Nakagawa-Recomp/badge)](https://scorecard.dev/viewer/?uri=github.com/Jstar269/Nakagawa-Recomp)

## License and lineage

GPL-3.0-or-later ([LICENSE](LICENSE)); some files keep GPL-2.0-or-later or their upstream terms. The project began as
a fork of [sal063's PSP Recompilation Project](https://github.com/sal063/PSP-recompilation-project) and still contains
code from it and code adapted from [PPSSPP](https://github.com/hrydgard/ppsspp); the ATRAC audio and H.264 video
decoders come from FFmpeg. It is not endorsed by those upstream authors. Details: [NOTICE.md](NOTICE.md) and
[CREDITS.md](CREDITS.md).

## Legal

Nakagawa Recomp is an independent fan project. It is not affiliated with or endorsed by Sony Interactive
Entertainment, Clap Hanz, or any game publisher. PlayStation and PSP are trademarks of Sony Interactive
Entertainment Inc., and game titles are trademarks of their owners; they are named here only to say what the project
works with. This repository contains no game files, firmware or Sony system fonts. Nakagawa decrypts your own disc
locally with the public game key tables PPSSPP ships; it never downloads games and never decrypts firmware.

Rights holders with a concern about anything here can contact the maintainer privately through
[GitHub private reporting](https://github.com/Jstar269/Nakagawa-Recomp/security/advisories/new) or the contact on
the [maintainer's profile](https://github.com/Jstar269). We aim to respond promptly and will remove material where
appropriate.

## Dedication

Dedicated to Blitzen: One of the greatest pets you could ever have. A companion from beginning to end.

An absolute sweetheart.

2012-2026

# Build and development setup

Windows 11 x64 remains the supported desktop baseline. Linux also has a bounded,
headless showcase route for the runtime. For an ISO/player workflow, start with
[`YOUR_OWN_GAMES.md`](YOUR_OWN_GAMES.md). For development, see
[`PLATFORM_PORTABILITY.md`](PLATFORM_PORTABILITY.md) and
[`LINUX_DEVELOPMENT.md`](LINUX_DEVELOPMENT.md). The native player is the repository's only user interface.

## Prerequisites

Everything a new Windows machine needs, in one list. The versions are floors; the
[supported baseline](#supported-development-baseline) says what is tested.

1. **Windows 11 x64** with a Vulkan-capable GPU and its current graphics driver. The player loads the
   driver's `vulkan-1.dll`.
2. **PowerShell 7.6+** (`pwsh`), the shell you type commands into. Windows PowerShell 5.1 is not
   supported for that.
3. **MSYS2 UCRT64** with the packages in [section 1](#1-install-the-core-toolchain): GCC, GNU Make,
   SDL3, SDL3_ttf, and the Vulkan headers and loader.
4. **CPython 3.15.x** (3.14.x is also accepted) on `PATH`. Read [PATH order](#path-order) before you install the Python tooling.
5. **The Vulkan SDK**, a current release, from [LunarG](https://vulkan.lunarg.com/sdk/home).
   LunarG Vulkan SDK 1.4.363.1 is recommended for Windows development.
6. **Git for Windows**, to clone the repository and fetch optional third-party source.
7. **Your own copy of the game** as an `.iso`, or as a compressed `.cso`, `.zso` or `.chd`
   ([Compressed disc images](#compressed-disc-images)). If its executable is encrypted, the player decrypts it
   locally; see [Encrypted executables](#encrypted-executables).
   Nothing in this repository ships or downloads a game; see [`YOUR_OWN_GAMES.md`](YOUR_OWN_GAMES.md).

For the two showcase demos only, add **WSL 2** with Ubuntu and **PSPDEV v20261101**; see
[Showcase demos on Windows](#showcase-demos-on-windows-wsl-2-and-pspdev).

The player's **BUILD PACKAGE** can download pinned build tools on first use. A consent card lists each
download first ([details](#player-build-package-prerequisites)).

**Which PowerShell.** Type the PowerShell commands in this guide into PowerShell 7.6+ (`pwsh`). The
build itself still calls the built-in Windows PowerShell 5.1 (`powershell.exe`) for one step,
copying the build assets (`tools/copy_build_assets.ps1`, through `POWERSHELL` in the
`Makefile`). That script supports 5.1, so you do not need to change or install anything for it.
The Python tools that start PowerShell (`tools/stage_install.py`, `tools/psp_readiness.py` and the
report bundle) use PowerShell 7 (`pwsh`) when it is on `PATH` and fall back to Windows PowerShell
5.1 otherwise. Set `NK_POWERSHELL` to a full path to choose one yourself.

## System requirements

Measured on a Windows 11 x64 PC building and running Hot Shots Tennis with the player's
**BUILD PACKAGE**:

| | Minimum | Recommended |
| --- | --- | --- |
| Operating system | Windows 11 x64 | Windows 11 x64 |
| Graphics | A GPU with a current Vulkan driver | Same |
| Processor | Any 64-bit x86 CPU | 4 or more cores |
| Memory | 8 GB RAM | 16 GB RAM |
| Free disk space | 5 GB | 10 GB or more, on an SSD |

- **Memory.** Building a game is the heavy part. The translated game code is compiled in
  parts of about 4 MB of C each, and one compiler process peaked at about 530 MB on such a
  part. BUILD PACKAGE budgets 200 bytes of memory for each byte of C in a part, at least
  768 MB (a 4 MB part gets 768 MB), keeps 2 GB of the available memory free for everything
  else, and runs one compile job for each further 768 MB. With 4.2 GB available it runs
  two jobs, with 5 GB four and with 7 GB six, so an 8 GB PC compiles several parts at once
  depending on what else is running. A part larger than the usual size is budgeted at 256
  bytes per byte, so a build that contains one runs fewer jobs. If less than 2.75 GB is
  available, one job runs and the compiler may fail when memory runs out. If Windows gives
  no memory reading, the build runs two jobs (one on a PC with a single processor). The
  figures are in GB as Windows reports them (1024 x 1024 x 1024 bytes). The compiler is the
  measured limit; the other build steps and the final link are not measured separately.
  Playing a built game needs far less.
- **Disk space.** Each game keeps three things in the player's data folder: its imported disc
  image (about 1.4 GB for Hot Shots Tennis), the built game (about 470 MB) and the build cache
  (about 400 MB). If the player downloads the build tools, they add a 102 MB download, and
  more once unpacked.
- **SSD.** Builds read and write thousands of small files, so a hard disk makes them much
  slower. Keep the player's data folder on an SSD if you can.
- **Processor.** Settings > Build tools and cache > Build speed sets how much of the PC a
  build takes. Balanced, the default, runs up to eight compile jobs at a time, one per logical
  processor, as far as memory allows. Background runs at most two, with the whole build at low
  priority, so a game you play meanwhile stays smooth and the build takes longer. Fastest runs
  one per logical processor, as far as memory allows. No speed runs more jobs than the free
  memory allows. The build screen names the speed, the job count and what limited it, and counts
  the parts as they finish; the build log names them too. When free memory, a single processor
  or `NK_PACKAGE_JOBS` limits it to one job, the build screen says so and that the build will
  take longer. If Windows gives no memory reading, every speed runs two jobs (one on a PC with a
  single processor). If the PC does not report its processor count, every speed runs one job
  and the build screen says nothing about processors. A first build takes several minutes. To
  choose the job count yourself, set the `NK_PACKAGE_JOBS` environment variable to a number from
  1 to 8 before starting the player: it replaces the speed's job count (a Background build stays
  at low priority), and the build log and screen say so. `nk_cli.py build-package` takes the
  same speeds as `--build-speed`.
- **Enhanced options page.** The shared Graphics preset and Play mode stay above six tabs: Picture, Motion, Sound,
  Controls, On-screen and Advanced. The same tabs appear on the library and each game's Enhanced options page. Each
  option is one row with all of its values. Up and Down move from row to row, Left and Right change the focused
  row's value, and a click picks a value; Q and E, or a controller's shoulder buttons, change tabs. A panel on the
  right shows the focused option's value, description and caveats, and the graphics preset's note. On a game's page,
  inherited values appear as `Library (value)` and a game's own value is highlighted. Ace-only options carry a tag,
  and options that cannot apply as the page stands stay visible but dimmed, with the reason in the panel. Frame
  pacing is on Motion. A game's Texture pack folder and Remove texture pack rows are the last rows of Picture.
- **Texture scaling and the Smart 2D filter.** Two parts of Enhanced and Ace, off by default and in no graphics
  preset: Settings > Enhanced offers **Texture scaling** (2x to 6x, or Off) and **Smart 2D filter** (On or Off), for
  the library or for one game. Texture scaling draws each texture from a larger copy made with xBRZ. The Smart 2D
  filter keeps HUD art and sprites that a game draws one texel to one pixel sharp at an internal resolution above
  1x; in a game that draws smooth-edged text that way it turns the text blocky (*Hot Shots Tennis* and
  *LittleBigPlanet* do, so leave it off for them). The launch line carries `texscale=<factor>` and `smart2d=on`.
  Both change the frames the game can read back; they are admitted as Enhanced items and stay off by default
  ([section 11.3.3](ENHANCEMENT_CONTRACT.md#1133-texture-scaling-and-the-smart-2d-filter-texscale-smart2d)).
  The `SR_TEX_SCALE` and `SR_SMART_2D` environment variables no longer switch them. A game started from the
  player ignores them (the player hands the game an empty value for each, and for `SR_GLYPHS`, so the settings are
  what count); a game's program started by hand with one set stops at once and names the setting to use.
  Each texture a game uploads is scaled once on worker threads (one for each logical CPU but two, 2 to 8), off
  the game's thread: on the development PC (`-O2`, one thread) a 256x256 texture took about 2.0 ms at 2x, 2.8 ms at 4x
  and 4.6 ms at 6x, so a scene change that uploads many textures shows some of them unscaled for a moment. Each
  copy costs factor x factor times the texture's GPU memory (a 256x256 texture at 4x is 4 MB), up to a budget of
  an eighth of the graphics card's memory by default, 512 MB to 2 GB (about 1.2 GB on a 10 GB card;
  `SR_TEX_SCALE_BUDGET_MB` sets another); past it, copies idle for about a second are freed, and when the
  copies in use fill it, new textures are drawn unscaled. Storing copies costs the game's thread up to 8 MB of
  uploads per frame, which can make a busy game drop frames. The scaler builds no lookup table, so it adds no fixed
  memory cost. The rules are in
  [`ENHANCEMENT_CONTRACT.md`](ENHANCEMENT_CONTRACT.md#1010-texture-scaling-and-the-smart-2d-filter).
- **Sharp game text (Enhanced, not yet in the player).** It is the launch line's `glyphs=on` key and is not yet an
  admitted Enhanced item, because it changes the frames the game can read back
  ([section 11.3.5](ENHANCEMENT_CONTRACT.md#1135-sharp-game-text-glyphs-not-yet-admitted)). The player has no
  setting for it and writes the launch line itself, so a player launch cannot turn it on; only a title's runtime
  started directly, with `SR_ACE` set by hand to an Enhanced or Ace line that carries `;glyphs=on`, can. At a
  render scale of 2 to 4, a small sprite that maps one texel to one pixel over a page of anti-aliased text is redrawn from a version of just
  that rectangle rebuilt at the render scale, checked against the game's own bytes. A page with a rebuilt rectangle
  costs the page's GPU memory times the render scale squared (a 256x256 page at 4x is 4 MB), taken from the
  same budget as texture scaling (`SR_TEX_SCALE_BUDGET_MB`). At most 48 rectangles are checked or rebuilt per
  frame, on the game's thread. Text drawn as triangles, or from a page in VRAM, is drawn as the game made it. The
  rules are in [`ENHANCEMENT_CONTRACT.md`](ENHANCEMENT_CONTRACT.md#1013-sharp-game-text).
- **Finer Bezier and spline patches (Finer patches).** A part of Enhanced and Ace, off by default and in no graphics
  preset: Settings > Enhanced offers it as **Finer patches** (2x, 3x, 4x or Off), for the library or for one game,
  and the launch line carries it as `tess=`. It is not on by choosing Enhanced or Ace, because it changes the frames
  the game can read back and alters the GE timing model's clocks
  ([section 11.3.7](ENHANCEMENT_CONTRACT.md#1137-finer-bezier-and-spline-patches-tess)). Each curved patch is drawn
  with factor x factor times the triangles, on the game's thread, up to 262144 vertices a draw, so a scene with many
  patches takes longer to draw. With the GE timing model on (`SR_GE_TIMING=1`) the extra triangles also change
  when the game's display lists complete and when `sceGeDrawSync` returns in game time: on 8888 targets without
  depth test, later for small patches and possibly earlier for large ones; elsewhere by the finer surface's
  coverage, in either direction. A run's log ends with a `PATCH_TESS: exit` line
  that counts the draws the vertex cap lowered. The rules are in
  [`ENHANCEMENT_CONTRACT.md`](ENHANCEMENT_CONTRACT.md#1014-finer-bezier-and-spline-patches).
- **Supersampling.** A part of Enhanced and Ace, off by default and in no graphics preset: Settings > Enhanced
  offers it as **Supersampling** (2x, 4x, 4x anyway or Off), for the library or for one game, and the launch line
  carries it as `ssaa=`. At a render scale of 3x or 4x (the Resolution row's, or the graphics preset's), 4x runs at
  2x and the row says why: "About 16 times the graphics work at this resolution; may stutter. Runs at 2x; pick 4x
  anyway." **4x anyway** keeps 4x at any render scale. A settings file from before this choice existed that has 4x
  runs at 2x at those render scales until 4x anyway is picked. It draws every frame at 2 or 4 times the render scale
  on each axis and shows each pixel as the average of
  its samples, which smooths jagged edges; the frames the game can read back do not change. It costs the GPU 4
  times (2x) or 16 times (4x) the drawing work of the render scale alone, on top of it, and GPU memory for the
  larger images: by the renderer's estimate about 17 MB (2x) or 64 MB (4x) at render scale 1, 68 or 255 MB at 2,
  and 272 or 1020 MB at 4. At start the renderer checks that the images fit the GPU; when 4x does not, the run
  uses 2x, and when neither does, it runs without supersampling, and the log says why in either case. The rules
  are in [`ENHANCEMENT_CONTRACT.md`](ENHANCEMENT_CONTRACT.md#1021-supersampling-ssaa).
- **Full 32-bit colour.** A part of Enhanced and Ace, off for a new library and turned on by the Ultra graphics
  preset; Low, Medium, High and Handheld leave it as the library has it. Settings > Enhanced offers it as **Full
  32-bit colour** (On or Off), for the library or for one game, and the launch line carries it as `twin=`. It shows
  16-bit games in full colour without dither by drawing each render op a second time into a host-only copy of each
  render target and depth buffer at the render scale, so it costs extra GPU work and memory; the frames the game can
  read back do not change. The rules are in
  [`ENHANCEMENT_CONTRACT.md`](ENHANCEMENT_CONTRACT.md#1011-full-32-bit-colour-the-host-twin).
- **Smooth far textures.** A part of Enhanced and Ace, off by default and in no graphics preset: Settings >
  Enhanced offers it as **Smooth far textures** (On or Off), for the library or for one game, and the launch
  line carries it as `mips=on`. It is not on by choosing Enhanced or Ace, because it changes the frames the game can
  read back ([section 11.3.8](ENHANCEMENT_CONTRACT.md#1138-smooth-far-textures-mips)). A texture the game gives
  only one level gets smaller copies of itself, which a far 3D surface samples at a high render scale. That
  reduces shimmer on such surfaces, but by little when texture filtering is on: on LittleBigPlanet's first level
  at 4x, distant shimmer fell by 0.1 % with 16x filtering and by 2.2 % without it. The copies are built on the
  game's own thread at the first draw that shows the texture small, so a scene that brings in many new textures
  can stutter; each frame builds about two 256x256 textures' worth, plus at most one more that started under that limit (the rest draw unsmoothed until a later frame), and textures that
  change every frame, such as movies, keep their one level while they change. The copies add up to a third of
  the texture's GPU memory. Through-mode menus and text, alpha-tested cut-outs, textures drawn at their own size
  or larger, and
  textures the game already shrinks itself are drawn as before. A run's log ends with a `HOST_MIPS: exit` line
  that counts the textures and the build time. The rules are in
  [`ENHANCEMENT_CONTRACT.md`](ENHANCEMENT_CONTRACT.md#1016-smooth-far-textures-host-mip-chains).
- **Texture packs (Texture pack).** A part of Enhanced and Ace, off by default and in no graphics preset. On a
  game's Enhanced page, **Texture pack folder** picks the folder of a pack you made or downloaded for that game (a folder
  that holds a `textures.zip` is read from that ZIP), and turns **Texture pack** on for it; the row shows the folder's name and how many textures it lists. Packs use the
  layout PSP emulators read (a `textures.ini` and hash-named PNG files), and Nakagawa ships none. A pack whose folder,
  ZIP or `textures.ini` cannot be read, or whose `[games]` section lists other games only, is refused with the reason,
  before it is stored and again before each launch, where **Play without the pack** starts that run without it. An image the pack lists that is missing or does not decode is
  skipped on its own, and that texture is drawn as the game made it.
  The launch line carries `texpack=on` and the folder travels as `SR_TEX_REPLACE`. It changes the frames the game
  can read back ([section 11.3.4](ENHANCEMENT_CONTRACT.md#1134-texture-replacement-texpackon)); the rules are in
  [`ENHANCEMENT_CONTRACT.md`](ENHANCEMENT_CONTRACT.md#1012-texture-replacement), and how to use one is in
  [`YOUR_OWN_GAMES.md`](YOUR_OWN_GAMES.md#texture-packs).
- **Soft shadows.** A part of Ace only, off by default and in no graphics preset: Settings > Enhanced offers it as
  **Soft shadows** (On or Off), for the library or for one game. In a game whose package names its shadow
  textures, it draws those small blob shadows with a smooth edge instead of a stepped one, at every render scale. No
  game's package names any yet, so a run with it on says on one `ACE_MODE: shadow unavailable` line that nothing is
  declared and draws as before. The launch line carries `shadow=soft`. It changes the frames the game can read back
  ([section 11.3.13](ENHANCEMENT_CONTRACT.md#11313-soft-shadows-shadowsoft-not-yet-admitted)); the rules are in
  [`ENHANCEMENT_CONTRACT.md`](ENHANCEMENT_CONTRACT.md#1023-soft-shadows).
- **Unclipped sound.** A part of Enhanced and Ace, off by default and in no graphics preset: Settings > Enhanced
  offers it as **Unclipped sound** (On or Off), for the library or for one game, and the launch line carries it as
  `peak=limit`. Some games add up more sound than the PSP's 16-bit mix can hold, and the PSP cuts the peaks off;
  with the part on you hear the same mix with those peaks turned down smoothly by a limiter instead, and everything
  below them at the PSP's own level. Only the sound you hear changes: the game's own sound data is the PSP's, and a
  recording keeps the game's copy. The sound plays 1.6 ms later, and the limiter runs on every sound the game
  plays. The rules are in
  [`ENHANCEMENT_CONTRACT.md`](ENHANCEMENT_CONTRACT.md#1022-unclipped-sas-mixes-through-a-true-peak-limiter-peaklimit).
- **Where your data lives.** Imported discs, built games, saves and settings are in the per-user data
  folder (`%LOCALAPPDATA%\Nakagawa\data`, [below](#2-supply-local-game-inputs)), never inside the unpacked
  release. An update unpacks into a new folder and leaves the data folder alone, so deleting an old release
  folder deletes no games or saves. A fresh install keeps its settings, data and saves in one `Nakagawa`
  folder: `%LOCALAPPDATA%\Nakagawa` on Windows, `$XDG_DATA_HOME/Nakagawa` (else
  `~/.local/share/Nakagawa`) on Linux, and `~/Library/Application Support/Nakagawa` on macOS. Linux keeps
  the cache in `~/.cache/nakagawa-recomp` and the logs in `~/.local/state/nakagawa-recomp/logs` (or under
  `$XDG_CACHE_HOME` and `$XDG_STATE_HOME`); an XDG variable that is not an absolute path is ignored, as
  the XDG specification says. Without `HOME` (or an absolute `$XDG_DATA_HOME`) there is no data folder,
  and the Python tools stop with `DATA_DIR_UNAVAILABLE`.
- **If you used an earlier Linux or macOS build.** While any of its folders exists, Nakagawa keeps
  using it, even an empty one: on Linux `~/.local/share/nakagawa-recomp` (or
  `$XDG_DATA_HOME/nakagawa-recomp`), its `saves` folder, and `~/.config/nakagawa-recomp` (or
  `$XDG_CONFIG_HOME/nakagawa-recomp`); on macOS the `config`, `data` and `saves` folders in
  `~/Library/Application Support/NakagawaRecomp`. Nothing is moved, copied, merged or deleted, and
  Nakagawa offers no move. If the new `Nakagawa` folder exists too, the library screen and the log
  name both and point to [Switching to the new data folder](#switching-to-the-new-data-folder),
  which has the steps and two known limits. Windows and Android have one data folder and nothing
  to switch.

### Switching to the new data folder

This is for a Linux or macOS install that still uses its earlier folders (the previous section).
Nakagawa never moves them itself. A switch by hand works when every piece goes to its place in the
new folder and the library's saved paths are corrected:

| Earlier folder | Goes to |
| --- | --- |
| Linux: `~/.local/share/nakagawa-recomp/saves` (or `$XDG_DATA_HOME/nakagawa-recomp/saves`) | `~/.local/share/Nakagawa/saves` (or `$XDG_DATA_HOME/Nakagawa/saves`) |
| Linux: the rest of `~/.local/share/nakagawa-recomp` (or `$XDG_DATA_HOME/nakagawa-recomp`) | `~/.local/share/Nakagawa/data` (or `$XDG_DATA_HOME/Nakagawa/data`) |
| Linux: `~/.config/nakagawa-recomp` (or `$XDG_CONFIG_HOME/nakagawa-recomp`) | `~/.local/share/Nakagawa/config` (or `$XDG_DATA_HOME/Nakagawa/config`) |
| macOS: `~/Library/Application Support/NakagawaRecomp/config`, `data` and `saves` | `~/Library/Application Support/Nakagawa/config`, `data` and `saves` |

On Linux the saves sit inside the earlier data folder, so they move out of it first: copied along
with the rest into `Nakagawa/data`, they would be where nothing reads them. The new settings folder
is in the data home, not in `$XDG_CONFIG_HOME`. Use the `$XDG_DATA_HOME` forms only when
`XDG_DATA_HOME` is set to an absolute path; otherwise the new folder is under `~/.local/share`. The
cache and the logs are not part of the switch: on Linux they stay in `~/.cache/nakagawa-recomp` and
`~/.local/state/nakagawa-recomp/logs`; on macOS the earlier `~/Library/Caches/NakagawaRecomp` and
`~/Library/Logs/NakagawaRecomp` can stay or go, and the new ones are made in the `Nakagawa` folder.

`library.json` in the data folder keeps the full path of each game's disc image and staged copy
(`iso_path`, `prepared_root`). A disc image you added from your own folder stays where it is and
keeps working; built games are found by disc ID in the data folder, so they move with it. A disc
image Nakagawa made inside the data folder (from a PSN or homebrew `EBOOT.PBP`, or a CSO or CHD
image, in `data/discs`) and a staged copy (`data/games`) are still named at the earlier path after
the move. The library lists those games as before, but **Play** stops with an error (for a disc
image, "Game source ISO not found" and the earlier path), and building one says "The original disc
image is missing. Put it back or add the disc again." Nothing is removed from the library and
nothing is rebuilt by itself; step 5 corrects the paths, or you can add each such game again from
its original file.

1. Close Nakagawa and any Nakagawa tool.
2. If a `Nakagawa` folder exists already (the library screen's notice names it), it holds what a
   start that used it wrote. Rename it, for example to `Nakagawa.unused`, so the moves below do not
   land on it; copy anything you want from it into the new folder afterwards.
3. Linux:

   ```sh
   mkdir -p ~/.local/share/Nakagawa
   mv ~/.local/share/nakagawa-recomp/saves ~/.local/share/Nakagawa/saves   # if it exists
   mv ~/.local/share/nakagawa-recomp ~/.local/share/Nakagawa/data
   mv ~/.config/nakagawa-recomp ~/.local/share/Nakagawa/config
   ```

   With the XDG variables set, use the paths in the table instead.
4. macOS: rename `~/Library/Application Support/NakagawaRecomp` to `Nakagawa`; its `config`, `data`
   and `saves` folders are already where the new layout expects them.
5. In the new `data/library.json`, replace every occurrence of the earlier data folder's full path
   with the new one, with a text editor or with `sed`:

   ```sh
   # Linux (with the XDG variables set, the paths in the table)
   sed -i "s|$HOME/.local/share/nakagawa-recomp/|$HOME/.local/share/Nakagawa/data/|g" \
       "$HOME/.local/share/Nakagawa/data/library.json"
   # macOS
   sed -i '' "s|$HOME/Library/Application Support/NakagawaRecomp/data/|$HOME/Library/Application Support/Nakagawa/data/|g" \
       "$HOME/Library/Application Support/Nakagawa/data/library.json"
   ```

6. Start Nakagawa. With none of the earlier folders left it uses the `Nakagawa` folder, and the
   library screen shows no notice. Do not leave a link at an earlier path: an earlier folder that
   exists in any form, a link included, keeps the earlier layout in use.

Two known limits:

- **A start that cannot see the earlier folders uses the new folder for that run.** When the
  earlier folders are there but that start cannot see them (they are on a drive that is not mounted
  yet, or a folder above them may not be searched), it uses `Nakagawa`, and everything written in that run stays
  there: settings, saves, games added to the library, imported fonts and built games, not only
  settings. The next start that sees the earlier folders uses them again and reads none of it; the
  library screen then names both folders. Move what you want to keep by hand.
- **An older Nakagawa brings the earlier layout back.** Running a Nakagawa from before this change
  once after a fresh install (an earlier release, or a player or tool from an older source checkout)
  creates an earlier folder such as `~/.local/share/nakagawa-recomp`. From then on every start uses
  the earlier layout and the `Nakagawa` folder goes unused, until you move the earlier folders away
  with the steps above. The library screen names both folders while this lasts.

## Install, update and uninstall (Windows)

Nakagawa is a portable ZIP ([#324](https://github.com/Jstar269/nakagawa-recomp/issues/324)), and a release can also
carry a per-user installer, [`Setup.exe`](#windows-installer-setupexe); v0.0.3 is the ZIP only, and the installer
is planned for v0.1.0. The unpacked folder, or the installed
program, holds nothing you create, so installing and removing it never touches your games or saves.

**Install.** Unpack `nakagawa-recomp-<version>-windows-x64.zip` into a folder of your choice (a read-only folder
works) and start `bin\nakagawa_player.exe`. For a Start-menu or Desktop entry, right-click the executable and
choose **Pin to Start**, or send a shortcut to the Desktop. From a source checkout,
`python tools/stage_install.py --dest "<folder>" --start-menu-shortcut --desktop-shortcut` stages the same layout and writes
both shortcuts with the player's taskbar identity, so the launcher and the games it starts share one taskbar
button. The ZIP itself writes no shortcut. Staging checks the player build's `.player-identity-*` stamp against
the commit exported into `source/` and stops with `INSTALL_PLAYER_SOURCE_MISMATCH` if the executable was built
for another commit or an unfinished build; pass `--allow-player-source-mismatch` to downgrade that stop to a
warning (executables without a stamp warn and still stage).

**Add to Steam** ([#2152](https://github.com/Jstar269/nakagawa-recomp/issues/2152)). To start a game from Steam
(for Steam Input, Big Picture or a handheld), exit Steam, open the game's page in the player, and choose
**Add to Steam**. Choose the Steam account if more than one has signed in on this system, then **Add to Steam**
again. The shortcut starts the player executable with `--launch-title=<disc ID>` from the player's own folder, which
starts that game without the launcher window and waits for it to end, so Steam shows it as running. When two images in your
library share a disc ID (a retail disc and its translation), the value also names the image, `<disc ID>:<SHA-1>`; a
player started with `--user-data-root` passes that folder on too. Start Steam afterwards to find the game in your
library; no restart is needed.

This works on your system only and makes no network call or download. On Windows, Steam's folder comes from the
current user's registry setting. On Linux, it checks `$HOME/.local/share/Steam` and `$HOME/.steam/steam`; it does not
require Steam to be installed through the system package manager. The account names come from Steam's own
`config/loginusers.vdf`. It edits the binary `shortcuts.vdf` in that account's `userdata/<account>/config` folder.
On Linux, Steam must be closed: Nakagawa checks `$HOME/.steam/steam.pid`; a missing file means Steam is closed,
while a live PID or an unreadable or malformed file means it may be running, so the write is refused. Before it
writes, it copies the file to
`shortcuts.vdf.nakagawa-backup-<date>-<time>` and reads the copy back; if that fails it writes nothing. It writes a
temporary file and renames it over the original. Steam rewrites this file when it exits, so while Steam runs
nothing is written: exit Steam first (Nakagawa never closes it). Entries Nakagawa did not write are copied byte for
byte, and a file it cannot read is left untouched. Choosing **Add to Steam** again for a game already there changes
nothing. After an update, which unpacks into a new folder, or after moving a portable copy, choosing it again updates
that game's shortcut to the new folder instead of adding a second one: only its program and start-in folder change,
and its play time and collections stay. When **Add to Steam** writes a shortcut cleanly, it also copies the game's own
local `ICON0.PNG` and `PIC1.PNG` (from the disc or prepared game folder; nothing is uploaded or fetched online) into
that account's `userdata\<account id>\config\grid\` folder under that game's own shortcut ID (`<appid>_icon.png`
and wide capsule `<appid>.png` from `ICON0.PNG`, falling back to `PIC1.PNG` for the wide capsule if `ICON0.PNG` is
absent; and hero banner `<appid>_hero.png` from `PIC1.PNG`). Steam takes a shortcut's icon only from the shortcut
itself, so a new shortcut names `<appid>_icon.png` as its icon; a shortcut that was already there keeps its own icon
setting. Both PSP images are landscape (`144x80` and `480x272`) and
do not fit Steam's 2:3 vertical portrait capsule (`<appid>p.png`), so the portrait capsule is left empty rather than
cropping, stretching, or inventing art; you can set portrait cover art manually in Steam. The pictures are optional: one
that cannot be read, is empty, is over 1 MiB or is not a well-formed PNG is left out and named, and the shortcut is
still added. As with `shortcuts.vdf`, nothing is written while Steam runs, existing pictures for that app ID are kept
rather than silently overwritten unless explicitly replaced, and every written picture is read back to verify. If a
picture cannot be written after the list changed, the list is put back and the failure is named; if the list cannot be
put back, its saved copy is kept and named. Nakagawa never removes a shortcut or the pictures it copied.

**Update.** A copy `Setup.exe` installed updates by running the next release's Setup (see [below](#windows-installer-setupexe)). For a ZIP copy, Check for updates unpacks the new version into a new folder next to the running one and leaves the old
one. Your data is in the per-user folders below, so the new version finds it. Delete the old folder when you
are done with it.

**Uninstall the program.**

- A ZIP you unpacked: close the player and delete its folder. Nothing was written anywhere else, except any
  shortcut you made yourself.
- A folder made by `tools/stage_install.py --dest`: run
  `python tools/stage_install.py --uninstall --dest "<folder>"` (add `--dry-run` to see the list first). It removes
  `bin\`, `source\`, the icon, the install marker and the shortcuts and desktop entries it recorded, and it
  skips any recorded path it would not have written itself. It never reads or deletes the per-user folders,
  and a file you added to the install folder stays.
- Portable mode ([below](#portable-mode-portabletxt)): the `data` folder beside the executable is your data. The
  uninstall keeps `bin\data` and `bin\portable.txt`; delete the folder yourself when you want it gone.

**What stays after an uninstall, and how to remove it.** Everything the player writes is under
`%LOCALAPPDATA%\Nakagawa` (or the `data` folder in portable mode, or the folder given with `--user-data-root`):

| Folder | Holds | To remove it |
| --- | --- | --- |
| `data/` | the library, built games and their build cache, imported discs and decrypted files (`titles\<DISC_ID>`, `discs\<DISC_ID>`), imported fonts, downloaded build tools | **Remove from library** on a game removes what Nakagawa built for it; its disc copy and decrypted files go only when you tick them, and the size is shown first. **Settings > Factory reset** moves the folders aside instead of deleting them. |
| `cache/` | the platform cache folder | **Settings > Build tools and cache** deletes the shared library cache; or delete the folder |
| `saves/` | each game's Memory Stick folder | **Remove from library** keeps it unless you tick the saves (it backs them up first); otherwise delete the folder by hand |
| `config/` | `settings.json` | **Factory reset**, or delete the folder |
| `logs/` | the player's, the games' and the builds' logs, in one folder | **Settings** opens it; delete what you like |

Your disc images on your own drives are never touched. To remove everything Nakagawa ever wrote, uninstall the
program as above and then delete `%LOCALAPPDATA%\Nakagawa`. Factory reset leaves a `-backup-<time>` copy of each
folder it moved, and those are yours to delete.

## Wine and Proton

The Windows ZIP is the only player release; there is no Linux player release ([#306](https://github.com/Jstar269/nakagawa-recomp/issues/306)).
Wine or Proton is not a supported platform, and nothing in the project is changed to make it work. These
steps are the smoke run from [#1876](https://github.com/Jstar269/nakagawa-recomp/issues/1876). It needs no game disc and
no network.

1. Unpack `nakagawa-recomp-<version>-windows-x64.zip` into an empty folder and check the unpacked files
   against `SHA256SUMS.txt`.
2. Start `bin\nakagawa_player.exe` (`wine bin/nakagawa_player.exe`, or add it to Steam as a non-Steam game
   and pick a Proton version). The first run used Wine 9.0 only; no minimum version is claimed.
3. Optional, the one step that needs a disc: pick a folder of your own discs in the library and confirm the
   scan lists them. Without a disc, record this step as `NOT_RUN`.
4. Run a showcase demo from **Test this PC** (they ship in `bin\demos\` and need no disc) and confirm it
   draws and runs. The repository's `display-smoke` target needs the MSYS2 toolchain from a source checkout
   and is not part of the ZIP.

Record the Wine or Proton version, the graphics driver and each step's result as `PASS`, `FAIL` or
`NOT_RUN` in the issue or the pull request. A failure is reported as a failure.

Known unknowns, each to be recorded by the first run rather than assumed:

- **BUILD PACKAGE.** The player compiles a game on your machine with the bundled `prereqs/` toolchain
  (`gcc.exe` and `mingw32-make.exe`). Whether that toolchain starts under Wine, and how long a build takes
  there, has not been measured.
- **Audio and gamepads** depend on the Wine audio driver and on SDL3's gamepad support under Wine.
- **Check for updates** does the same network request as on Windows, so it works only if the prefix has
  network access.

Result of the first run (2026-11-02, [#1876](https://github.com/Jstar269/nakagawa-recomp/issues/1876)),
on the ZIP layout from `tools/stage_install.py --dest` and `--portable-zip` built from the pull request's
head (the player, its runtime DLLs and `source/`; not the attested release ZIP, so no `prereqs/`, no demos):
Wine 9.0 (Ubuntu 24.04 `wine64`, 64-bit prefix only) under WSL 2, Xvfb as the display, Mesa software Vulkan drivers installed, no
GPU, no audio device, no game disc.

| Step | Result |
| --- | --- |
| Unpack the ZIP, check `SHA256SUMS.txt` | `PASS` |
| `nakagawa_player.exe --help` | `PASS` |
| Player renders the library, About and Settings views to 1280x720 screenshots | `PASS` (non-blank) |
| The synthetic display-smoke fixture, built natively, run under Wine with `--sched` | `PASS` (`DRIVER_EXPECT_U32 ... status=PASS`) |
| The same fixture with `--gui` in a window, and with `--sched --gui` offscreen | `PASS` |
| Scan a folder of real discs | `NOT_RUN` (no disc; the scan is not exercised by the synthetic route) |
| BUILD PACKAGE with the bundled `prereqs/` toolchain | `NOT_RUN` (the local ZIP has no `prereqs/`) |
| Audio, gamepad, Check for updates | `NOT_RUN` |
| Proton | `NOT_RUN` (not installed) |

Wine printed `failed to open C:\windows\syswow64\rundll32.exe` once while creating the prefix, and every 64-bit program above ran regardless.

## Showcase demos on Windows: WSL 2 and PSPDEV

The two showcase demos are project-written PSP programs. Their build uses PSPDEV, the PSP toolchain,
which needs a POSIX system, so on Windows it runs inside WSL 2. Skip this section if you only want the
player and your own game.

1. In an administrator PowerShell, install WSL 2 with Ubuntu, and reboot if Windows asks:

   ```powershell
   wsl --install
   ```

   Open Ubuntu once and create your Linux user. Make Ubuntu the default distribution, because the build
   runs `wsl -e` without naming one: `wsl --set-default Ubuntu`. `wsl -l -v` marks the default with `*`.

2. In Ubuntu, download `pspdev-ubuntu-latest-x86_64.tar.gz` from the `v20261101` release at
   <https://github.com/pspdev/pspdev/releases>. The asset name, release and archive hash are recorded in
   [`assets/upstream/pspdev.lock.json`](../assets/upstream/pspdev.lock.json).

3. Check the archive. Its SHA-256 must equal `distribution.archive_sha256` in the lock file:

   ```bash
   sha256sum pspdev-ubuntu-latest-x86_64.tar.gz
   ```

4. Unpack it into `/usr/local`. The archive's top folder is `pspdev/`, so this creates `/usr/local/pspdev`:

   ```bash
   sudo tar -xzf pspdev-ubuntu-latest-x86_64.tar.gz -C /usr/local
   ```

5. Check the install. The `psp-gcc` hash must equal its `sha256=` value under `local_verification.tool_versions`
   in the lock file:

   ```bash
   test -x /usr/local/pspdev/bin/psp-gcc && test -d /usr/local/pspdev/psp/sdk && echo "PSPDEV found"
   sha256sum /usr/local/pspdev/bin/psp-gcc
   ```

6. From the repository root, in a PowerShell window where `$env:Path` starts with `C:\msys64\ucrt64\bin`
   (see [section 3](#3-build)):

   ```powershell
   mingw32-make player showcase     # PSPDEV runs in WSL 2 (steps 1-5)
   mingw32-make showcase-smoke      # the demos need WSL 2 and PSPDEV too
   ```

   The demo build takes several minutes. `showcase-smoke` runs both demos headlessly and prints
   `SHOWCASE_SMOKE: PASS` for each one.

## Linux headless showcase

Linux can build and run the two source-owned showcase packages through the runtime.
The route needs GCC, GNU Make, Python 3.15 (3.14 is also accepted), CMake, Ninja, `libvulkan-dev`, SDL3 3.4.20,
and PSPDEV v20261101 installed at `/usr/local/pspdev`. CI builds SDL3 from its pinned
3.4.20 release commit and verifies the PSPDEV archive against
[`pspdev.lock.json`](../assets/upstream/pspdev.lock.json).

Run the showcase from the repository root:

```bash
make CC=gcc showcase-linux   # needs PSPDEV at /usr/local/pspdev, as above
```

The smoke selects SDL's dummy video and audio drivers and runs without a desktop or
GPU. This confirms the Linux runtime builds and runs the showcase fixtures; general
consumer ISO compatibility and interactive Linux presentation are not supported
([#306](https://github.com/Jstar269/nakagawa-recomp/issues/306)).

## Supported development baseline

The supported and tested core development environment is:

- Windows 11 x64. Older or unsupported Windows versions may work, but receive no compatibility guarantee.
- PowerShell 7.6+ (`pwsh`) as the shell you type into. Windows PowerShell 5.1 is not supported for that,
  although the build tools fall back to it where PowerShell 7 is missing ([Which PowerShell](#prerequisites)).
- CPython 3.15.x (3.14.x is also accepted, `>=3.14,<3.16`), with `python` resolving to that feature line. 3.15 is the default: the pinned prerequisite and hosted CI both use 3.15.2.
- Current MSYS2 UCRT64 GCC/G++, GNU Make, SDL3, and Vulkan loader packages. The release
  manifest's `toolchain_policy` records only the floors the code actually needs: a C11 compiler
  (GCC 4.9+), GNU Make 4.0+ (the title link uses `$(file ...)`), SDL3 3.x, a Vulkan SDK for API 1.1+, and Python 3.15 (the manifest says `>=3.15,<3.16`; the tooling itself also accepts 3.14, see the first bullet). The rolling
  MSYS2 packages are not pinned; `tools/record_toolchain.py` records the versions a release
  candidate was really built with (#367).
- A current Vulkan SDK and Vulkan-capable GPU.

PowerShell 7.6 is the current floor: it is the LTS line Microsoft supports until 2028-11-14,
and Microsoft ends support for both 7.4 and 7.5 on 2026-11-10.
[Issue #337](https://github.com/Jstar269/nakagawa-recomp/issues/337) records the
source inventory and the cross-version test evidence (every PowerShell-backed test passed on
7.4, 7.5 and 7.6). Scripts treat empty and absent environment values alike. The doctor and each
script's `#requires -Version 7.6` header refuse an older line.

The environment doctor is the executable form of this contract:

```powershell
python tools/nk_doctor.py --scope build
```

For the Vulkan SDK, discovery is explicit and fail-closed: `-VulkanSdk` wins first, then
`VULKAN_SDK`, then the newest numerically named usable installation under `C:\VulkanSDK`. A usable
installation contains the Vulkan headers and loader import library required by the build. Do not
copy a patch version from another machine into setup instructions.

## 1. Install the core toolchain

Install [MSYS2](https://www.msys2.org/), open its **UCRT64** terminal, and run:

```bash
pacman -Syu
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-make mingw-w64-ucrt-x86_64-sdl3 mingw-w64-ucrt-x86_64-sdl3-ttf mingw-w64-ucrt-x86_64-vulkan-headers mingw-w64-ucrt-x86_64-vulkan-loader
```

Also install:

- CPython 3.15.x (3.14.x is also accepted) on `PATH`.
- The [Vulkan SDK](https://vulkan.lunarg.com/sdk/home). The manager follows the discovery order above; pass `-VulkanSdk "C:\path\to\sdk"` or set `VULKAN_SDK` when an explicit location is needed.
- Git, to fetch optional third-party source.

From the repository root, install the declared Python tooling dependencies once:

```powershell
python -m pip install .
```

This installs the declared tooling dependencies, including `compiledb`. Run it before you add the MSYS2
line to `PATH`; see [PATH order](#path-order).

`glslc` from the Vulkan SDK is only needed when regenerating the checked-in shader headers. LLD, Clang, CMake, Ninja, and Node.js are not required for the Windows core build (the CMake build used on Linux and Android is described in [`PLATFORM_PORTABILITY.md`](PLATFORM_PORTABILITY.md)).

### PATH order

Some folders hold a second copy of a tool that the build also calls, so the order of `PATH` matters.

- **MSYS2's Python.** `C:\msys64\ucrt64\bin` may contain Python 3.14.8, which the tooling accepts but
  CI does not use; the default is CPython 3.15.x. Run Python tooling with the Windows CPython installed
  from python.org. Before building from a UCRT64 shell, set `PYTHON` to that interpreter's full path
  (for example, `C:/path/to/Python315/python.exe`); Make recipes and the player package builder use
  that explicit path even when MSYS2 is first on `PATH`. Run direct tooling commands through the
  same `PYTHON` path. Install the declared Python tooling dependencies before adding MSYS2 to `PATH`.
- **The player's package build.** BUILD PACKAGE puts one UCRT64 toolchain's `bin` in front of the build
  child's `PATH`, so Python, `mingw32-make` and `gcc` come from the same place: the release bundle, else
  the per-user download, else a system MSYS2 UCRT64 install, tried in this order: a UCRT64 `bin`
  already on `PATH`, then `MSYS_PATH`, then `C:\msys64\ucrt64`. A bundled or downloaded toolchain
  needs only `gcc.exe` in its `bin`; a system install also needs its `include` to hold the SDL3
  headers. A UCRT64 `bin` on `PATH` counts only when it is `<prefix named ucrt64>\bin`, and a complete
  one is used even when `MSYS_PATH` names another install. A MinGW from another package manager ahead
  of MSYS2 on your `PATH`, such as Chocolatey's, therefore no longer wins. With no complete UCRT64
  toolchain the `PATH` is left alone and the build stops at `sdl3-check`, which names the `gcc` it
  found. The `PATH` is also left alone in the rare case that it is already so long that the toolchain
  folder cannot be added within the 32767-character Windows limit.
- **POSIX shell.** If a Make recipe needs `sh`, put Git for Windows' `usr\bin` (`%ProgramFiles%\Git\usr\bin`)
  ahead of MSYS2's `usr\bin` (`C:\msys64\usr\bin`), as the [Windows host pitfalls](#windows-host-pitfalls)
  table describes.

### Deliberate path conventions

Absolute paths in first-party tooling are limited to platform defaults, never a
developer's machine layout. The conventions in force are:

- `C:\msys64\ucrt64\bin` — the MSYS2 UCRT64 install root used above. Tools accept
  an explicit location first (`-MsysPath`, `MSYS_PATH`, or a `gcc` already on
  `PATH`) and otherwise assume this default install.
- `C:\VulkanSDK` — the default Vulkan SDK root, after `-VulkanSdk` and `VULKAN_SDK`.
- `C:\Windows` — the Windows system directory, used only when `WINDIR` is unset
  (system font discovery).

Any other absolute path must come from the repository root, an environment
variable, or an explicit flag. `tools/test_workspace_paths.py` fails the tracked
tree when a user-profile path or a workspace root appears in a first-party file.

### Runtime DLLs (SDL3.dll, SDL3_ttf.dll & vulkan-1.dll)

The native player (`nakagawa_player.exe`) requires `SDL3.dll` and the host Vulkan loader (`vulkan-1.dll`). It also loads `SDL3_ttf.dll` at run time for the readable UI font: `mingw32-make player` stages `SDL3_ttf.dll` and its full runtime dependency closure beside `build\nakagawa_player.exe` (`tools/stage_runtime_dlls.py`, with `SDL3_TTF_DLL` as the override in the same pattern as `SDL3_DLL`), so the same TTF font path is used whether the player is started from Explorer, a plain `cmd.exe` with no MSYS2 on `PATH`, a built title package, or the release layout. In a release archive, the matching `SDL3.dll`, `SDL3_ttf.dll` and its closure are already beside `bin/nakagawa_player.exe`; leave those files in `bin/`. In a source checkout, `tools/copy_build_assets.ps1` copies SDL3 and any imported MinGW runtime DLLs beside the built game executable and generates their third-party notices; the player target stages its own typography runtime the same way. Without `SDL3_ttf.dll` the launcher cannot start: the player says why in a message box and on stderr and exits with code 4, and the workspace doctor reports it as `RUNTIME_SDL3_TTF`.

The player and every title executable start by limiting where Windows looks for a library they load after start-up (`SetDefaultDllDirectories`): the executable's own folder and System32, so such a library in the folder they were started from or on `PATH` is not picked up. This does not change how the DLLs an executable imports are found (Windows resolves those before the program starts, beside the executable first); the staged DLLs keep loading as before. `SDL3_ttf.dll`, which the player and the in-game overlay load after start-up, must be beside the executable or beside the `SDL3.dll` in use, not only on `PATH`.

#### 1. SDL3.dll

For a source build, install the MSYS2 UCRT64 SDL3 package described above. Do not copy the DLL into the repository root; the build asset script resolves the toolchain copy and stages it beside the player. For a release package, use the SDL3 DLL already present in `bin/`.

#### 1a. SDL3_ttf.dll (UI typography)

Install the MSYS2 UCRT64 `sdl3-ttf` package (with the `sdl3` package above). `mingw32-make player` then stages `SDL3_ttf.dll` and the mechanically resolved dependency closure (FreeType, HarfBuzz, Graphite2, libpng, zlib, bzip2, Brotli, GLib, libintl, libiconv, PCRE2 and the MinGW runtimes) beside `build\nakagawa_player.exe`, together with their licence texts in `THIRD_PARTY_NOTICES/`. Set `SDL3_TTF_DLL` to an explicit `SDL3_ttf.dll` file to override the toolchain copy, exactly like `SDL3_DLL`.

#### 2. vulkan-1.dll

This is the host Vulkan loader, normally installed by the graphics driver. The release package records it as host-resolved and does not redistribute it by default.

Windows normally resolves it from the installed NVIDIA, AMD, or Intel graphics driver. The Vulkan SDK's loader is useful for development diagnosis, but it is not copied into the release package; install or repair a Vulkan-capable graphics driver if the loader is missing.

#### 3. Verifying Correctness & Compatibility

To ensure your runtime DLLs are compatible and up to date:

- **64-bit (x64) Architecture Check:** The player and bundled SDL3 DLL are 64-bit. A mismatched DLL can prevent Windows from starting the player.
- **Version/Metadata Verification:**
  To check details, right-click the DLL file in Windows Explorer, select **Properties**, and navigate to the **Details** tab:
  - For `SDL3.dll`: The package generator records the version in the SBOM and notices.
  - For `vulkan-1.dll`: The driver supplies the loader; match it to the installed graphics driver rather than copying a different SDK build beside the player.

Confirm the commands visible to PowerShell 7:

```powershell
python --version
pwsh --version
mingw32-make --version
gcc --version
g++ --version
```

Run `glslc --version` only when regenerating shader headers; it is not a core build prerequisite when
the checked-in generated shader includes are current.

## 2. Supply local game inputs

The repository intentionally excludes all game content. Keep private inputs in
the Git-ignored `place_game_here/` folder. The canonical runtime/build layout
is:

```text
place_game_here/
├── EBOOT.elf
├── ISO/<game>.iso
└── EXTRACTED/
    ├── decrypted/
    │   ├── libfont.prx
    │   ├── scePsmf_library.prx
    │   └── scePsmfP_library.prx
    └── PSP_GAME/
        ├── SYSDIR/EBOOT.BIN
        └── USRDIR/xbdata_extracted/
```

The manager resolves these paths directly. Root-level `eboot.elf` and
`game.iso` links remain a legacy fallback, not a requirement. Any source PBP,
document, or other retail container stays outside the working set; it is not
needed once the canonical local inputs exist.

Missing decrypted PRXs prevent late-import registration; missing extracted XB
data breaks plain-file asset lookups. `SYSDIR/EBOOT.BIN` supplies the PSP
header/BSS metadata while `place_game_here/EBOOT.elf` remains the flat
translation input. Do not replace the flat input mechanically until the
repacker preserves its current memory layout.

For automated or pre-loaded runs that require installed game data:

- Utility savedata uses the hierarchical
  `memstick/PSP/SAVEDATA/<game><save>/` tree.
- Ordinary guest `sceIoOpen("ms0:...")` calls and utility savedata share one
  canonical host Memory Stick root (`SR_MEMSTICK`, default `memstick/`) and
  one path resolver. Title-specific cached assets and save identities are
  private inputs; this public guide deliberately does not enumerate them.
- Legacy hierarchical trees under `fs/` are no longer listed or auto-migrated;
  a read-open miss may import a legacy flat `fs/` file once into the unified
  root, but write/create never creates under `fs/`.

The unified Memory Stick root is the production contract; `fs/` remains only
as a read-only legacy-import source until existing menu routes are
revalidated against the unified tree.

To regenerate the extracted asset tree, run the extractor. It has no
third-party dependency:

```powershell
python tools/extract_xb.py place_game_here/EXTRACTED/PSP_GAME/USRDIR/xbdata --output place_game_here/EXTRACTED/PSP_GAME/USRDIR/xbdata_extracted -v
```

Extraction is entirely repository-owned: `tools/xb_probe.py` parses the archive
and decodes every member — including the nested `DEFLATE → LZS` layer — under
the budgets declared at the top of `tools/extract_xb.py`, and that same module
normalizes each member name once and writes it itself. The name that is
validated is the name that is written, on every host. An archive containing an
escaping, ambiguous, or colliding member name, one that breaches a decode
budget, or one the reader cannot parse at all is refused rather than extracted;
each archive is built in a staging directory and promoted only after the whole
of it succeeds. The extractor refuses to reuse a non-empty destination or
replace an existing generated file unless `--overwrite` is passed, `--workers`
defaults to a small cap rather than the CPU count because each worker holds a
whole archive in memory, and the number of in-flight worker tasks is bounded
independently of how many archives were found.

[libxb](https://github.com/kiwi515/libxb) is not used by the build, runtime, or
extractor. Its formerly audited `0.2.0` snapshot and the measured comparison boundary
are retained as historical evidence in
[`ISSUE196_DIRECT_XB.md`](archive/ISSUE196_DIRECT_XB.md); no optional checkout is required.

`third_party/` and `place_game_here/` are local-only and ignored by Git. If you use `tools/validate_assets.py`, its optional `tools/reference_hashes.json` reference file is also local-only; it is not required by the normal build.

### Plain module inputs

Some titles load additional modules at runtime. The runtime accepts only plain (unencrypted)
ELF/PRX files. The player decrypts a disc's encrypted modules into the per-title folder (see [Encrypted executables](#encrypted-executables)). For what the player needs
and where your own unencrypted files go, see [`YOUR_OWN_GAMES.md`](YOUR_OWN_GAMES.md). When you already have plain modules from your own
copy of the game, place them at the paths the local manifest expects, for example:

```text
place_game_here/EXTRACTED/decrypted/libfont.prx
place_game_here/EXTRACTED/decrypted/scePsmf_library.prx
place_game_here/EXTRACTED/decrypted/scePsmfP_library.prx
```

A valid plain module begins with the ELF magic bytes `7F 45 4C 46`; a file beginning with `~SCE`
or `~PSP` is an encrypted container and is rejected. Never copy game or firmware material into
Git history.

### Per-title decrypted input folder (standalone player and CLI)

For the standalone player (`nakagawa_player.exe`) and `tools/nk_cli.py`:
When an imported ISO contains an encrypted executable (`EBOOT.BIN`), the player's disc check decrypts it and the disc's encrypted modules into a per-title folder under the per-user data directory, and `nk_cli inspect` and `nk_cli build-package` read them from there (the command-line tools decrypt nothing themselves):

```text
<user data>/titles/<DISC_ID>/decrypted/
├── EBOOT.elf
└── <module>.prx
```

On Windows, the normal per-user data directory is `%LOCALAPPDATA%\Nakagawa\data`. The player and the Python tools both use `%LOCALAPPDATA%` first; when it is unset, the player also asks Windows for `FOLDERID_LocalAppData`. Both fall back to `%APPDATA%` and fail with `DATA_DIR_UNAVAILABLE` if neither application-data location resolves. `USERPROFILE` alone is never used as a data root. If the legacy `%USERPROFILE%\Nakagawa\data` exists while the current root does not, the player and Doctor report its exact path; move its contents manually. When a valid plain MIPS ELF32 `EBOOT.elf` is placed in the current folder, the player and CLI select it automatically for analysis ([#428](https://github.com/Jstar269/nakagawa-recomp/pull/428)). If an experimental profile was created while the disc executable was still encrypted (binding no executable), supplying `EBOOT.elf` in this folder is automatically used by `nk_cli build-package` without requiring re-import.

Decrypted guest modules in `<user data>/titles/<DISC_ID>/decrypted/` may be named either after their file name on the disc (for example `psmf.prx`) or after their manifest module name (for example `scePsmf_library.prx`). When a module is encrypted, invalid, or missing, error messages display both names (for example `psmf.prx (scePsmf_library.prx)`).

### Encrypted executables

The player decrypts the encrypted `EBOOT.BIN` and modules of the disc you added, locally, the way PPSSPP does, with the public game key tables in `src/core/nk_psp_builtin_keys.c` ([#1494](https://github.com/Jstar269/nakagawa-recomp/pull/1494)). It reads no key file and makes no network request. Each output is written beside its destination in the per-title folder above, checked as a PSP ELF32/MIPS program, then renamed into place; a file already there is never replaced. Firmware is never decrypted: an updater archive (PSAR), a bare KIRK block (the boot loader's form) and any module with a firmware-only tag stop as `FIRMWARE_OUT_OF_SCOPE` before any key is read. `nk_decrypt` (built by `tools/nk_core/decrypt_tool.py`) runs the same boundary from the command line.

A file the boundary can't decrypt stops with a named result:

```text
ENCRYPTED_EXECUTABLE: EBOOT.BIN is encrypted and built-in decryption stopped: KEY_REQUIRED (<detail>). Place a decrypted EBOOT.elf from your own copy in <user data>/titles/<DISC_ID>/decrypted.
```

The results are `KEY_REQUIRED` (a tag the built-in tables don't carry), `MALFORMED_INPUT`, `INTEGRITY_FAILURE`, `DECOMPRESS_FAILURE`, `VALIDATION_FAILURE`, `FIRMWARE_OUT_OF_SCOPE`, `IO_FAILURE`, `RESOURCE_LIMIT` and `INTERNAL_ERROR`. An encrypted guest module stops the same way with `ENCRYPTED_MODULE: <name> is encrypted and built-in decryption stopped: ...`, naming the module. A plain copy you place there is always used and never overwritten. Nothing is written next to the ISO or into this repository.

A PSN release of a UMD game (an `EBOOT.PBP` whose `DATA.PSAR` is an NPUMDIMG disc image) is picked like a disc. The player converts it once, on a background worker with progress and cancel, into `<user data>/discs/<DISC_ID>/<DISC_ID>.iso`: the image is written beside that name, checked (an ISO9660 volume descriptor and a `PARAM.SFO` `DISC_ID` equal to the package's), then renamed into place, and it is then added exactly as a picked ISO. A cancelled or failed conversion removes the partial file. Each time the package is added, the worker reads all of it for its SHA-256 and reuses the image in place only when the image's record names that SHA-256; its size, write time, change time and file identity are noted before it is read and checked again before the record is written, and it is read a second time then, so a package that changed meanwhile is refused as `DISC_IMAGE_READ_FAILED` and its image removed. On Windows the package is also held during the conversion so that no other program can write to it (on a local drive, or a server that enforces this); a package another program already has open for writing, such as a download or sync tool, is read shared and relies on those checks, and the log says so (what the checks catch on each system: [Compressed disc images](#compressed-disc-images)). `--iso=<EBOOT.PBP>` is taken the same way once the player's window is open (with `--stage`, `--stage-only`, `--launch-now` or `--headless-launch` it is converted before inspection, with progress on the console), and `nk_decrypt npumdimg --in EBOOT.PBP --out game.iso` runs the conversion from the command line. A PSOne Classic stops as `PSONE_CLASSIC_UNSUPPORTED` and an updater as `FIRMWARE_OUT_OF_SCOPE` before any key is read; DRM bound to the console that bought the game stops as `CONTEXT_REQUIRED` and is never run with a default. Other PSN (NPDRM) packages and PGD-protected data are not decrypted yet.

### Compressed disc images

The player also takes a compressed disc image: a `.cso` (CISO, versions 1 and 2), a `.zso` (ZISO) or a `.chd`
made with `chdman createdvd`. It expands the image once, when you add it, into a plain `.iso` at
`<user data>/discs/<DISC_ID>/<DISC_ID>.<identity>.<cso|zso|chd>.iso`, and every later step reads that `.iso`
exactly as it reads one you added yourself. The identity is the SHA-256 of every byte of the compressed file (what
`sha256sum` prints for it); the name carries its first 16 hexadecimal digits, or all 64 when another image already
has that name, so two different images of one disc, such as a retail dump and a fan translation, keep two
expansions. This needs free space for the whole `.iso` (up to 1.8 GB for a UMD) beside the compressed file, which
you can keep or move. The work runs on a background worker with progress and cancel: each add reads the whole
file for its identity, and a file with the same bytes as one added before, under any name or in any folder,
reuses that expansion. Otherwise the expansion is written beside its final name, its size is checked, the file is
read once more to check that it did not change meanwhile, and the expansion is renamed into place, so a cancelled
or failed expansion leaves nothing behind. The file's size, write time, change time and file identity are noted
before it is read and checked again before anything is recorded; with the second read, they stop a file that
changed while it was added as `DISC_IMAGE_READ_FAILED`. What that catches depends on the system:

- On Windows the file is also held while it is added, so no other program can write to, rename or replace it (on
  a local drive, or a server that enforces this): there is no change to catch.
- On Windows, a file another program already has open for writing, such as a download or sync tool, is read
  shared, and the log says so. That program can still write to it (not rename or replace it). A change still in
  place at the second read is always caught; one it undoes before then is caught by the change time, unless the
  program also sets the change time back, which Windows allows.
- On Linux nothing holds the file, and any change is caught, even one undone before the second read (the bytes
  and write time restored, or the file renamed away and back), because no ordinary program can set a file's
  change time.
- A drive formatted FAT32 or exFAT stores no change time, so there an undone change may be missed unless it
  leaves the size or write time different.

A CHD whose header records the
SHA-1 of its data (every `chdman` image does) is checked against it, and a mismatch stops the import as damaged.
An image added by an earlier version, which recorded a sample of the file (its size, write time and first 64 KiB)
instead of its identity, is used again only after it is compared byte for byte with the file; its record is then
updated in place. Adding the same file from a second window or tool while the first is still adding it is refused
as `DISC_IMAGE_IMPORT_BUSY`. The library keeps the expanded `.iso` for the build and shows the file you picked.
`--iso=<file>` takes a compressed image the same way once the player's window is open; with `--stage`,
`--stage-only`, `--launch-now` or `--headless-launch` it is expanded before inspection, with progress on the
console.

A file it can't use stops with a named result: `DISC_IMAGE_TRUNCATED`, `DISC_IMAGE_HEADER_INVALID`,
`DISC_IMAGE_VERSION_UNSUPPORTED`, `DISC_IMAGE_SIZE_INVALID`, `DISC_IMAGE_INDEX_INVALID` or
`DISC_IMAGE_BLOCK_CORRUPT` for a damaged or unknown header or block, `DISC_IMAGE_WRITE_FAILED` when the `.iso`
can't be written (often a full drive), and `DISC_NOT_PSP` when the image holds no PSP game. A CHD made as a CD
image (`chdman createcd`) stops as `DISC_IMAGE_CHD_CD_UNSUPPORTED`: make it again from the `.iso` with
`chdman createdvd`. A hard-disk, laserdisc or parent-linked CHD stops as `DISC_IMAGE_CHD_UNSUPPORTED`. CHD is
read with the vendored libchdr ([`third_party/libchdr/`](../third_party/libchdr/PROVENANCE.md)); CSO and ZSO
are read by the project's own code in `src/core/nk_disc_expand.c`. `python tools/nk_cli.py bringup` and
`tools/nk_core/iso_inspect.py` take the same files and expand them through the same C code, with the
`nk_disc_expand` tool that `tools/nk_core/disc_expand_tool.py` builds.

### User title manifests (`<user data>/manifests`)

For titles requiring a title manifest overlay, place the manifest JSON files in:

```text
<user data>/manifests/
└── <manifest_name>.json
```

On start-up, `nakagawa_player.exe` automatically scans `<user data>/manifests` and loads every `*.json` file as an overlay in alphabetical filename order (via `nk_title_manifest_load_overlay_dir`, capped at `NK_MANIFEST_MAX_OVERLAYS` = 8). Any malformed or skipped files are reported to stderr.

Similarly, `python tools/nk_cli.py build-package` searches `assets/titles` first and then `<user data>/manifests`, resolving title manifests identically to the native player so user-configured titles build without command-line overlay arguments.

### Windows installer (`Setup.exe`)

A release can also carry `nakagawa-recomp-<version>-windows-x64-setup.exe`
([#2208](https://github.com/Jstar269/nakagawa-recomp/issues/2208)), a per-user installer with the same files as the
ZIP. v0.0.3 does not carry it: the installer is planned for v0.1.0. It needs no administrator and installs to `%LOCALAPPDATA%\Programs\Nakagawa Recomp` (you can choose another
folder). It adds a Start-menu entry and, if you tick it, a Desktop one, both with the player's taskbar identity, and
an entry under Apps & features. It makes no network call.

- **Update:** run the next release's Setup; it uses the same folder. It replaces the program files with the new
  release's, then deletes each file the previous release installed there (as listed in that install's own
  `SHA256SUMS.txt`) that the new release no longer carries, but only if the file is unchanged since Setup installed
  it, and removes the folders that leaves empty. Files you added or changed in the install folder, a portable
  `bin\data` and `bin\portable.txt` are kept, nothing is deleted through a junction or symbolic link, and
  `%LOCALAPPDATA%\Nakagawa` (your games, saves and settings) is not touched. The player's Settings page offers the
  release page for a copy Setup installed, not a ZIP download.
- **Uninstall:** Apps & features, or `unins000.exe` in the install folder. It removes the program and keeps
  `%LOCALAPPDATA%\Nakagawa` (the `config`, `data`, `cache`, `logs` and `saves` folders) unless you tick "Also delete my
  games, saves and settings" on the uninstall page and confirm a second time; a silent uninstall never deletes them. A
  junction or symbolic link inside those folders is removed as a link and what it points to is left alone. Close the
  player first: the uninstaller does not close it, and stops without changing anything while it runs.
- **One copy per shortcut name:** the installer and `tools/stage_install.py` write the same Start-menu shortcut, so
  Setup refuses to install while a shortcut of that name starts another copy (for example one made by
  `stage_install.py`), and `stage_install.py --shortcut`, `--start-menu-shortcut` and `--desktop-shortcut` refuse to
  replace a shortcut that starts another existing copy (a shortcut whose program is gone is replaced). Uninstall or
  delete that copy and its shortcut first. The two also never share a folder: Setup refuses a folder
  `stage_install.py` set up, and `stage_install.py` refuses a folder with Setup's `unins000.exe`
  (`INSTALL_DESTINATION_INSTALLER_OWNED`).

### Portable mode (`portable.txt`)

By default every per-user folder lives under the per-user data root (on Windows,
`%LOCALAPPDATA%\Nakagawa\data`). To keep the library, saves, settings, logs, cache and downloads
beside the program instead, for a USB stick, a handheld PC or two versions that must not share
settings, create an empty file named `portable.txt` in the same folder as `nakagawa_player.exe`.

- The data root is then `data` in that folder (`<program folder>\data`, made on first start); the
  config, cache, logs and saves folders sit inside it, exactly as with
  `--user-data-root=<folder>`.
- The decision is made once, in `src/core/nk_portable.h`, and handed to the platform's one
  override (`nk_platform_set_app_data_dir_override`), so the library, packages, fonts, settings,
  saves, logs and the folders the player hands to the build tools and the game all follow it.
- An explicit `--user-data-root=<folder>` wins over the marker.
- If the folder cannot be written (a read-only install, a full or write-protected drive) the player
  refuses to start with `PORTABLE_NOT_WRITABLE` (a dialog on Windows, stderr elsewhere). It never
  falls back to the per-user root, so nothing is saved somewhere you did not choose. Delete
  `portable.txt` to go back to the per-user root; the two data folders are separate and neither is
  moved or deleted.
- **About** shows the data folder in use and says when it is the portable one.
- Updating: unpack the new version, then copy `portable.txt` and the `data` folder beside the new
  program. Nakagawa does not move or delete the old folder.

### Start a game from another launcher

A front-end such as Quiver, Pegasus, ES-DE, LaunchBox or Steam ROM Manager starts a game by running a program
with arguments. Nakagawa Recomp works with any of them that can run an executable with a command line.

- **Program:** `bin\nakagawa_player.exe` inside the unpacked release ZIP
  (`nakagawa-recomp-<version>-windows-x64.zip`).
- **A game already in your library:** `nakagawa_player.exe --launch-title=<disc ID>`, for example
  `--launch-title=ULUS10041` (or `<disc ID>:<SHA-1>` to name one disc image). No launcher window opens; the player
  waits for the game and passes its exit code on. **Add to Steam** writes this form for you.
- **A disc image the front-end hands over:** `nakagawa_player.exe --iso=<path> --launch-now`. The player adds the
  image to your library if it is not there and starts the game. Without `--launch-now` the launcher window opens
  instead (`--stage` adds the image first; `--stage-only` adds it and exits).
- **Spaces in a path:** quote the whole path in the front-end's template, as in `--iso "<path>"` or
  `"--iso=<path>"`. Every option that takes a value accepts `--name=value` or `--name value`, so either
  spelling works; a value cannot itself start with `--`.
- **Nothing is ignored.** An unknown option, a game path without `--iso`, an option missing its value, or a value
  given to a switch is refused: the player names the problem on standard error (and in `player-output.txt` in the
  logs folder when it has no console), shows no window and exits with code 2. `nakagawa_player.exe --help` lists
  the options and `--version` prints `Nakagawa Recomp v<version> (commit <hash>)` on one line and exits 0.
- **Where the data lives:** per user by default (`%LOCALAPPDATA%\Nakagawa\data`). An empty `portable.txt` beside
  `nakagawa_player.exe` (see Portable mode above; Quiver can add one from its Manage menu) moves it to
  `<program folder>\data`, and `--user-data-root=<folder>` wins over both. A launcher that backs up saves reads
  them from the saves folder inside that data root.
- **One launcher per data folder.** If Nakagawa Recomp is already open on the same data folder, a second start
  brings that window forward, starts nothing, says so on standard error and exits with code 0. Runs with
  `--screenshot`, `--view`, `--stage-only`, `--headless-launch` and the test harness
  (`--ui-test-*`) are not affected: they run beside an open launcher.

Exit codes of a start from the command line (`--help` prints the same list; a code can have several causes):

- `0`: Done. A game that was started returns its own exit code instead, whatever it is.
- `1`: The launcher window could not be created, a UI test failed, a --screenshot could not be saved, the manifest overlay could not be loaded, or a --stage-only or --headless-launch run could not read, convert or inspect the disc image.
- `2`: The command line, the data folder, the --launch-title name or the library entry was refused, or staging needs a supported --iso.
- `3`: The game has no runtime prepared for this PC (build it in Nakagawa Recomp), or staging (--stage-only, or --launch-now with --headless-launch) cannot set it up (not in the catalog, no profile, out of memory).
- `4`: The game or the launcher could not start (a missing SDL3_ttf.dll included), an experimental profile could not be made, or staging (--stage-only, or --launch-now with --headless-launch) failed.
- `5`: The game's window request was lost.
- `6`: The game ended abnormally (a crash), or in staging (--stage-only, or --launch-now with --headless-launch) the game's folder path is too long for the library record.
- `7`: A headless run timed out, or in staging (--stage-only, or --launch-now with --headless-launch) the game could not be saved to the library.
- `8`: A headless acceptance checkpoint was not reached, or in staging (--stage-only, or --launch-now with --headless-launch) the game's data folder could not be checked.
- `9`: In staging (--stage-only, or --launch-now with --headless-launch), another run holds the game's staging, or a file takes the game's folder name.

When a `--launch-title` start is refused, the player also shows a message box, so a front-end with no console still tells the user.

### System fonts

Firmware follows PPSSPP's model. The operating system is HLE, the modules a game ships on its disc may run as LLE, and Nakagawa ships no firmware and decrypts none. It ships open replacement fonts. Authentic in-game typography needs the PSP's own system fonts in Sony's PlayStation Glyph Format (`.pgf`): you may copy those plain files from your own console's `flash0:/font/` into a local folder that Nakagawa reads. They stay on your PC. The project never bundles, commits, packages or uploads them, and it does not decrypt anything from `flash0`.

> [!NOTE]
> Do not confuse system fonts with `libfont.prx`. `libfont.prx` (located under `place_game_here/EXTRACTED/decrypted/libfont.prx` or `<user data>/titles/<DISC_ID>/decrypted/libfont.prx`) is a game-supplied guest middleware PRX executable module implementing the `sceFont` API; it does not contain the actual font glyph outlines. Typography requires the separate `.pgf` font files.

#### What the user supplies

From your own PSP, copy its PGF font files to a folder on a USB drive, a memory stick, or your PC. Copy all eighteen: `jpn0.pgf`, `ltn0.pgf` through `ltn15.pgf`, and `kr0.pgf`. A game that asks for a font you have not imported gets a stand-in, which looks different from the PSP's font: a nearby imported Latin size, the older single Latin import, or the project's own font. Where nothing stands in, it gets no font. The import reads every `.pgf` file directly in the folder you choose and checks each one with the same native reader the player uses. It keeps the source filename long enough to identify firmware Latin files:

- Japanese: covers kana (U+3042 or U+30A2).
- Korean: covers Hangul (U+AC00 or U+D55C).
- Latin: covers the Latin letters (U+0041 and U+0061) and neither of the above. Files named `ltn0.pgf` through `ltn15.pgf` are imported under their matching registry indices; all supplied files in that range are imported independently.

A file that covers both kana and Hangul, or neither, names no slot and is skipped. Files over 16 MiB are refused. Other Latin filenames retain the single legacy Latin import; if several such files name the same slot, none is imported until you choose one with `--choose latin=<file>`. Duplicate case variants of the same `ltnN.pgf` index are left unimported as ambiguous.

If you already copied your own PSP's fonts into a folder for another program, point the import at that folder; replacement fonts that other programs ship in place of a PSP's are accepted too, but they are shown as stand-ins, never as your PSP's own.

Fonts must come only from your own device. The project provides no download links or third-party repositories. The import makes no network request, uses no key, and does no decryption, and it never uploads anything.

#### Copying the fonts from your PSP

The fonts are optional. Without them, games still run, and their text uses the project's own open fonts, which look different from the PSP's.

The font files live in the PSP's system storage, `flash0:/font/`. They are plain files, not encrypted, but the PSP's official firmware doesn't show its system storage to a PC: on a PSP with official firmware only, there is no way to copy them yet. A PSP with custom firmware can show it as a read-only USB drive. These steps are for ARK (ARK-4 or ARK-5); the PSP only reads, and nothing on it changes:

1. On the PSP, open **Settings > Custom Firmware Settings**.
2. Set **USB Device** to **Flash 0**, and **USB Read-Only** to **On**.
3. Connect the PSP to your PC with a USB cable, then open **Settings > USB Connection** and press **X**. A new drive appears on the PC. It holds the PSP's system files (folders such as `font`, `kd` and `vsh`), not your Memory Stick. If it shows your Memory Stick's folders (`PSP`, `ISO`) instead, end the connection and check step 2 again.
4. On the PC, copy the drive's `font` folder to a folder of your own, for example `Documents\PSP fonts`. Copy only: don't delete, rename or add anything on that drive.
5. On the PSP, press **O** to end the USB connection. In **Custom Firmware Settings**, set **USB Device** back to **Memory Stick**.
6. In the player, open **Settings**, then **PSP FONTS**, choose **CHOOSE FONT FILES**, and pick the `.pgf` files in the folder you copied. The page then says how many fonts it imported and lists where each font comes from. You can also skip step 4: while the PSP's drive is still connected, choose **GET FONTS FROM MY PSP**, and the player finds the drive and imports its `font` folder itself.

On PRO or LME custom firmware, the VSH menu (press **SELECT** in the XMB) has the same **USB DEVICE** setting: choose **Flash 0**, then follow steps 3 to 6, setting it back to **Memory Stick** afterwards.

If no drive appears at all, a plugin that uses the USB port, such as PSPLink, is probably running. Turn it off in **Settings > Plugins**, restart the PSP, check step 2 again (after a restart **USB Device** can be back on **Memory Stick**), and repeat from step 3. Turn the plugin back on when you're done.

You can copy the whole `font` folder. The import reads only the `.pgf` files in it: `gb3s1518.bwfon` and `imagefont.bin` are not read, and `arib.pgf` is listed as refused because no game font slot uses it. The sixteen Latin fonts (`ltn0.pgf` to `ltn15.pgf`), the Japanese font `jpn0.pgf` and the Korean font `kr0.pgf` are all used exactly, so all 18 game font slots come from your own PSP ([#1536](https://github.com/Jstar269/nakagawa-recomp/pull/1536)). If you imported fonts with an older build, import them again to pick up the Japanese and Korean ones.

##### Without a Flash 0 USB setting: Nakagawa Font Copy

If your custom firmware has no Flash 0 USB setting, use Nakagawa Font Copy, a small PSP app built from this project's source in `src/psp_companion/` (#737). It reads the PSP's font files, decrypts nothing and changes nothing on the PSP. It writes only to its own folder, `NAKAGAWA/fonts`, on the Memory Stick, or on a PSP Go's internal storage when you start it from there.

1. Build the app with the PSPSDK (`make -C src/psp_companion`; see [PSPDEV integration](PSPDEV_LOCAL_VERIFICATION.md)), then copy `src/psp_companion/build/EBOOT.PBP` to `PSP/GAME/NAKAGAWAFONTS/EBOOT.PBP` on the Memory Stick. Releases do not include the app.
2. On the PSP, open **Game > Memory Stick > Nakagawa Font Copy** and press **X**. The app copies every `.pgf` file in `flash0:/font` and checks each copy against the original, then says how many fonts it copied. Running it again replaces the copies.
3. Leave the app, connect the PSP to your PC with a USB cable, and turn on **Settings > USB Connection** on the PSP. The Memory Stick appears on the PC as a drive.
4. In the player, open **Settings**, then **PSP FONTS**, and choose **GET FONTS FROM MY PSP**: the player finds the Memory Stick's `NAKAGAWA\fonts` folder and imports it. You can instead choose **CHOOSE FONT FILES**, open that folder and pick its `.pgf` files. From a terminal, `python tools/nk_cli.py fonts import <drive>:\NAKAGAWA\fonts` does the same, with your PSP's drive letter in place of `<drive>`.

The app only lists `flash0:/font` and opens its files read-only. If the PSP does not let it list the folder, it opens the eighteen firmware font files (`jpn0.pgf`, `kr0.pgf` and `ltn0.pgf` to `ltn15.pgf`) by name instead. Its build pins the file calls it may make, so no remove, rename or file-control call can be in it. The copy code is the hardware oracle's `font-files` case ([`HARDWARE_ORACLE.md`](HARDWARE_ORACLE.md)), and `tools/test_psp_oracle.py` checks which files the app selects and that it writes only inside its folder.

#### Importing fonts

```powershell
python tools/nk_cli.py fonts import <folder>
python tools/nk_cli.py fonts status
python tools/nk_cli.py fonts remove --all
```

Available options:

- `--choose <slot>=<file>`: Pick a file for a generic slot that several files name (`japanese`, `latin`, or `korean`). Named `ltn0.pgf` through `ltn15.pgf` files select their own Latin indices and need no choice.
- `--user-data-root <path>`: Override the target per-user data directory (default: `%LOCALAPPDATA%`, then `%APPDATA%`, under `Nakagawa\data` on Windows; `~/Library/Application Support/Nakagawa/data` on macOS; `$XDG_DATA_HOME/Nakagawa/data` or `~/.local/share/Nakagawa/data` on other POSIX systems, or an earlier install's own folder, as **Where your data lives** says).
- `--json`: Emit a machine-readable report of the outcome for each file, and of each slot's status.

The import copies each accepted file into the per-user cache and writes `manifest.json` with `schema_version: 2` and an ISO 8601 UTC `import_time`. Generic files remain in `files`; indexed Latin files are listed in `latin_indices` with their index, size, SHA-256, user source, and reader version. The originals are not touched. Re-importing is idempotent, and entries not changed by the new run are retained:

```text
<user data>/fonts/v2/
├── manifest.json
├── nkjpn.pgf   (Japanese slot)
├── nkltn.pgf   (legacy unindexed Latin slot, if imported)
├── ltn0.pgf    (Latin registry index 1)
├── ...
├── ltn15.pgf   (Latin registry index 16)
└── nkkr.pgf    (Korean slot)
```

Japanese and Korean keep their existing imported-file-then-project-file resolution. Each Latin index uses its exact imported `ltnN.pgf` first. If it is absent or invalid, the runtime uses the nearest valid indexed import in that index's eight-file size group (the lower index wins an equal-distance tie). A cache containing only the older single-file Latin import continues to serve that file for all indices, preserving pre-index behavior. Other caches fall back to the project's face for that index, which has its family, style and size (`nkltn.pgf` for `ltn0`, `nkltn1.pgf` to `nkltn7.pgf` for `ltn1` to `ltn7`, `nkltns.pgf` for `ltn8` and `nkltn9.pgf` to `nkltn15.pgf` for `ltn9` to `ltn15`), then to the project's regular face of that size, then to the legacy import only when no project Latin font is available. What the project fonts still do differently from a PSP's is listed in [`font/README.md`](../font/README.md#without-your-psps-fonts-what-changes). The player status and one startup log line per index identify every fallback, including an absent exact file. Those lines list every font a game could ask for; a `flash0: game opened` line, once per file, names each font the game does open, with the file that serves it and its source. The log line names the requested PSP file, the file that backs it and its source: `selection=exact` for the user's own file, `selection=replacement` for an imported replacement font, `selection=substitute fallback=same-size`, `fallback=legacy` or `fallback=project` for a stand-in, and `backing=none fallback=missing` when nothing serves the index. The Japanese and Korean fonts (registry indices 0 and 17, `jpn0.pgf` and `kr0.pgf` on a PSP) get one such line each, where the project font is a substitute. `fonts remove --slot latin` removes the generic Latin import and all sixteen indexed files; the other slot removals are unchanged.

The per-user data directory is the one font root. The import, the status and the preflight in the player, the launcher, and the runtime all read `<user data>/fonts/v2`; the launcher hands that root to the runtime as `SR_USER_DATA_ROOT`. A runtime root (`--runtime-root`) does not hold fonts, so an import written under `<runtime root>/fonts/v2` by an earlier build is no longer read. The launcher's old lookup under a showcase demo's `<demos>/fonts/v2` is removed for the same reason: the demos read the per-user directory too, so an import placed there is no longer read. To use it again, run `python tools/nk_cli.py fonts import <folder>` on the original `.pgf` folder (with the same `--user-data-root` you use for the player), or copy that `fonts/v2` folder unchanged to `<user data>/fonts/v2`. The manifest's size and SHA-256 are checked on use, so a copy that does not match is refused rather than served.

> [!NOTE]
> The in-game font path serves the slot files in two ways. The HLE `sceFont` shim and the read-only `flash0:/font/` device share the same index resolver. Japanese and Korean keep their current source order. Each Latin registry entry reports its requested `ltnN.pgf` name and reads the exact indexed file, a same-size-group indexed fallback, the legacy single-file import when it is the only Latin import, or the project Latin PGF. Sizes and names come from the bytes actually served. Exact and same-group indexed files report the served index's measured style; project and legacy files report Regular. Missing exact files remain visible in the setup prompt and startup log.

#### Verification and player status

To verify that system fonts are in place:

1. **CLI:** `python tools/nk_cli.py fonts status` prints Japanese and Korean status plus one line for each of the 16 Latin indices. A line names an exact import, same-size fallback, project fallback, legacy fallback, or missing file; an imported replacement font says "a replacement font" (Japanese, Korean) or `replacement` in place of `exact` (Latin). A last line says how many of the 18 PSP fonts are your PSP's own, which no replacement font counts toward; `--json` gives that number as `own_font_count`. `fonts import` says the same of each replacement it imports.
2. **Player preflight (#300):** the `SYSTEM_FONTS` check is `OK` when every Japanese, Korean, and Latin registry index resolves to your own imported font for that exact index. It is `SYNTHETIC` (the player shows **Synthetic**, in amber) when every index resolves but at least one is a stand-in: a project font, another imported size, a legacy import or an imported replacement font ([#1300](https://github.com/Jstar269/nakagawa-recomp/issues/1300)). Games still start; the text that uses a stand-in looks different. It is `MISSING` when an index has no source, and its message includes the exact file or fallback for every Latin index. It is `INVALID` ([#313](https://github.com/Jstar269/nakagawa-recomp/issues/313)) when the manifest is malformed or a listed file fails its check. Whenever a font is not your own import, the check's sentence in the player says which fonts in plain words (for example "the Japanese font, 10 of the 16 Latin fonts and the Korean font"), says that games asking for them get a stand-in font that looks different, or no font where nothing stands in, and points to Settings, PSP fonts; the check's message in the log keeps the exact file for every font ([#1300](https://github.com/Jstar269/nakagawa-recomp/issues/1300)).
3. **Player settings:** **Settings > PSP FONTS** opens with one line that says how many of the 18 PSP fonts (Japanese, Korean and the 16 Latin indices) are your PSP's own, each served by its exact import rather than a stand-in. Below it the page shows the Japanese and Korean status and all 16 Latin indices, including each fallback, and has three buttons:
   - **GET FONTS FROM MY PSP** imports the fonts straight from a PSP connected by USB. It finds the PSP's system storage shown as a USB drive (a `font` folder at the drive's root, next to its `kd` and `vsh` folders; see [Copying the fonts from your PSP](#copying-the-fonts-from-your-psp)), or the `NAKAGAWA/fonts` folder that Nakagawa Font Copy fills on the Memory Stick. It looks at the root of every fixed or removable drive on Windows, in each folder of `/Volumes` on macOS, and in `/media`, `/media/<name>`, `/run/media/<name>` and `/mnt` elsewhere. It only lists folders, and nothing leaves the computer. When it finds no PSP, the page says what to do.
   - **CHOOSE FONT FILES** opens a file picker; the import reads every `.pgf` file in the folder of the files you pick.
   - **REMOVE IMPORTED FONTS** removes the imports.

   Each button reports its outcome on that page. The setup wizard's **Fonts and display** step shows the same line and has the same three buttons. Both work with a keyboard, a mouse or a controller.
4. **Game page:** when a game starts in Authentic and any of the 18 fonts is a stand-in or a replacement, its page says "Text uses stand-in fonts. For the original look, get the fonts from your PSP." with **GET FONTS FROM MY PSP**, which opens **Settings > PSP fonts** and looks for a connected PSP, and **HIDE**, which hides the line for that game for good. The game starts either way.

## 3. Build

The public checkout's end-to-end display build and player launch is:

```powershell
$env:Path = "C:\msys64\ucrt64\bin;$env:Path"
mingw32-make --no-print-directory display-smoke-player
```

This generates the source-owned display fixture, runs the full two-phase pipeline,
verifies the presented framebuffer, builds the native player, and exercises its Play
route without private input. Use `mingw32-make --no-print-directory display-smoke` for
the headless build/verification without opening a window. Generated C is split into a
dynamic number of translation units based on the discovered function count and
`FUNCS_PER_CHUNK`, and on `CHUNK_TARGET_BYTES` when it is set (package builds set it to
4,000,000 bytes).

For a validated local title manifest, use the manager so every title-derived value
comes from that manifest:

```powershell
.\tools\nk_manager.ps1 -Action BuildFull -TitleManifest C:\path\to\manifest.json -GameName game
.\tools\nk_manager.ps1 -Action BuildFast -TitleManifest C:\path\to\manifest.json -GameName game
```

If `-GameName` is omitted, the manager uses the manifest's `game_name` or a portable
ID fallback; an explicit value overrides that selection. Direct Make is title-neutral
but does not run the manager planner; it must receive the validated
`TITLE_MANIFEST` and every title-derived Make value explicitly. Direct Make also does
not discover the Vulkan SDK, so set `VULKAN_SDK` or pass it as a Make variable.

To build a generated v1 runtime package for an imported disc in the player library:

```powershell
python tools/nk_cli.py build-package <disc_id>
```

This extracts the plaintext executable (or automatically uses `EBOOT.elf` and guest PRXs from `<user data>/titles/<DISC_ID>/decrypted/`), runs the build pipeline, stages runtime assets, and promotes the package to `<user data>/packages/<DISC_ID>/`. The native player (`build/nakagawa_player.exe`) discovers, validates, and launches packages from that directory ([#297](https://github.com/Jstar269/nakagawa-recomp/issues/297)). An explicit `--module-dir <directory>` can be passed when modules are located elsewhere. When a manifest reads BSS metadata from the disc's `~PSP` executable header (`bss_metadata_source: "psp-header"`), `build-package` extracts it directly from the disc image into `cache/packages/<DISC_ID>/selected.psp` and verifies the `~PSP` magic, so `--psp-header` is only needed if the file on the disc lacks that header.

The built package ships its required guest modules inside `<package>/modules/` (copied under their manifest names), and the launcher sets `SR_MODULE_DIR` to `<package>/modules` so launches are self-contained without external dependencies. A run begins at the title's resolved run entry (`run_entry`: a declared `runtime_bindings.fallback_entry`, else `executable.entry`), ensuring that titles requiring a fallback entry point start there across both the native player and CLI launchers. Re-adding an already imported disc in the library preserves its completed staging and extraction state (`player_merge_readded_game`) when the disc ID, version, and image size match. `build-package` always builds with the public runtime backends; PGD-protected game data remains unavailable at run time ([#308](https://github.com/Jstar269/nakagawa-recomp/issues/308)). The private-backend mode (`PUBLIC_SAFE=0`) was retired on 2026-10-25, and `make` refuses it.

Package work is content-addressed below `<user data>/cache/packages/<DISC_ID>/`; the cache key covers executable bytes, analyzer/codegen semantics, codegen options, generated-code/runtime ABI epochs, compiler identity/target, and native flags. An unchanged key reuses the validated runtime package. A compatible native/runtime change reuses generated C from the separate AOT-stage cache, while compiled objects remain in the bounded object-key cache. New package schema v3 installs omit compiler intermediates but still validate every retained file through `completion-manifest.json`; the player continues to check v2 packages with their original generated-object list. The builder writes and verifies the manifest before an atomic directory promotion; same-volume installs share package files with their cache entry by hard link. Damage to a hard-linked installed artifact also invalidates that cache copy, so the next build verifies the AOT stage and rebuilds from the current inputs. The full contract and rebuild-component names are in [`RUNTIME_PACKAGING_ARCHITECTURE.md`](RUNTIME_PACKAGING_ARCHITECTURE.md#6-private-content-addressed-cache-contract-316).

To inspect a private package cache with Doctor, pass the user-data root and disc ID:

```powershell
python tools/nk_doctor.py --scope products `
  --user-data-root "$env:LOCALAPPDATA\Nakagawa\data" `
  --disc-id <disc_id>
```

Doctor reports a missing, incomplete, or stale cache entry and names the changed key component when a current key is available. Cache cleanup protects entries used by installed package variants and retains at most one previous entry and AOT stage per disc.

Builds also share compiled objects. The runtime compiles once for every game, an unchanged rebuild compiles nothing, and the system libraries a disc carries are compiled once for every game that carries the same library. These objects stay in `<user data>/cache/objects/` and `<user data>/cache/shared-libraries/` and are never uploaded or shared. Settings > BUILD TOOLS AND CACHE turns library sharing off and clears its folder. [`RUNTIME_PACKAGING_ARCHITECTURE.md`](RUNTIME_PACKAGING_ARCHITECTURE.md#shared-compiled-objects-1306) has the keys and limits.

The GPU renderer keeps a pipeline cache in `<user data>/cache/gegpu/`, one `gegpu-<vendor>-<device>.vkpc` file per graphics card ([#2149](https://github.com/Jstar269/nakagawa-recomp/issues/2149)). It holds the compiled draw states the graphics driver built in earlier runs, so a game's first scenes stutter less on the next start. It changes start-up time only, never a pixel. The file is checked before the driver sees it. A file written for another card, another driver version, another build's shaders, or one that is truncated or damaged is ignored, and the run starts with an empty cache and replaces it at exit. A file that passes the checks is never replaced by less data than it held, so a short run, or a driver that ignores the file, cannot shrink the cache. The log says on one line at start which of these happened. The file is safe to delete at any time, and set `SR_GPU_PIPELINE_CACHE=0` to run without it.

Alternatively, to build a local AOT package directly from a validated title manifest and plaintext executable ELF, use
the planner's package action. It runs the same two-phase Make pipeline and writes the executable,
generated objects, `package.json`, and `build-report.json` under one dedicated untracked directory:

```powershell
$env:Path = "C:\msys64\ucrt64\bin;$env:Path"
python tools/title_codegen_plan.py assets/titles/my-title.json `
  --package `
  --game-elf place_game_here/EBOOT.elf `
  --output-dir build/my-title
```

For manifests with guest PRXs, add `--module-dir <directory>`; every required manifest module must
exist there, and optional modules are included only when named with
`--include-optional-module <manifest-name>`. Add `--psp-header <path>` when the manifest selects
`bss_metadata_source: "psp-header"`. `--public-safe` is available for synthetic fixture builds.
The output directory must be untracked and dedicated. The package schema and explicit unsupported
semantic-boundary records are defined in
[`RUNTIME_PACKAGING_ARCHITECTURE.md`](RUNTIME_PACKAGING_ARCHITECTURE.md#5-local-aot-package-contract-v3).
Inputs whose paths contain spaces or shell-sensitive characters (common under a Windows profile
directory) are copied into `<output-dir>/staged-inputs/` so the Make recipes can use them. When the
output directory contains spaces, the route builds in a Make-safe workspace—resolved from
`NK_BUILD_ROOT` if set, or the parent directory's 8.3 short name on Windows—and atomically promotes
the finished package to the destination. If neither fallback is available, or if the path contains
characters that Make recipes cannot quote, the route stops with the named `PACKAGE_UNSUPPORTED_PATH`
boundary.
An enhancement data root is read in place rather than copied. When its path has a space or another
character Make cannot take, the route uses its 8.3 short form on Windows, and stops with
`PACKAGE_UNSUPPORTED_PATH` naming the data root when no short form exists (a volume with 8.3 names
turned off, or any other platform). `NK_BUILD_ROOT` does not help there; move the data root to a
location without such characters.

`assets/titles/hst-ucus98701.json` is the HST title manifest, part of the repository since 2026-10-27: it carries HST's guest-address runtime bindings, and a `GAME_NAME=hst` build refuses to compile without it rather than silently producing a runtime with every title binding disabled. See [`assets/titles/README.md`](../assets/titles/README.md).

The manifest declares no `game_name`, so its build name is derived from its id
(`hst-ucus98701`); the manager's HST commands pass `-GameName hst`. Generic launch
resolution (issue #366) derives every runtime and image path from validated title identity only,
never from a launcher default: a developer runtime is found only under the build name the
manifest gives, and a missing one fails closed with a missing-runtime error instead of inheriting
another title's build.

The requirement is enforced incrementally, not only on a clean tree: the generated title-config
header is keyed to a content-addressed identity of the effective configuration, so dropping
`TITLE_MANIFEST` from a previously bound build directory refuses rather than reusing the header
that build left behind.

## 4. Run and test

Run `pwsh -NoProfile -File tools\nk_manager.ps1` without an action for usage (exit code 0).
The canonical manager accepts these actions:

| Action | Description |
| --- | --- |
| `BuildFull` | Clean and rebuild the selected title through the full pipeline. |
| `BuildFast` | Incrementally build the selected title without cleaning. |
| `Run` | Launch the selected title with the chosen runtime profile. |
| `Inspect` | Locate a generated C function using `-InspectFunc`. |
| `Clean` | Stop tracked build processes and clear local tracking logs. |
| `Test` | Run the Makefile `selftest` target (C++ reference-runtime selftest), not the Python suite. |
| `Verify` | Run Python tests, native selftests, and import/publication audits. |
| `DiffFunc` | Compare a function against a reference trace using `-DiffTarget` and `-DiffOracle`. |
| `FindSymbol` | Search the optional symbol reference using `-FindName`. |
| `Fuzz` | Run the Makefile `vfpu_fuzz` target. |
| `VisualOracle` | Replay a route and archive visual regression captures. |

```powershell
.\tools\nk_manager.ps1 -Action Test
mingw32-make --no-print-directory display-smoke-gui
.\tools\nk_manager.ps1 -TitleManifest C:\path\to\manifest.json -GameName game -Action Run
.\tools\nk_manager.ps1 -TitleManifest C:\path\to\manifest.json -GameName game -Action Run -SoftwareRender
.\tools\nk_manager.ps1 -TitleManifest C:\path\to\manifest.json -GameName game -Action Run -NoGui -Duration 30
```

The manager examples require a local validated manifest and its declared inputs. For
normal Vulkan runs, an explicit profile can isolate the intended task:

```powershell
.\tools\nk_manager.ps1 -TitleManifest C:\path\to\manifest.json -GameName game -Action Run -Profile Performance # log-free smoke test
.\tools\nk_manager.ps1 -TitleManifest C:\path\to\manifest.json -GameName game -Action Run -Profile Benchmark   # 1 Hz telemetry + logs/perf.csv
.\tools\nk_manager.ps1 -TitleManifest C:\path\to\manifest.json -GameName game -Action Run -Profile Benchmark -GuestProfile # plus guest-PC hotspot summary
```

`Performance` redirects the runtime's stdout and stderr to the null device; it is not a
debugging mode. `Benchmark` retains bounded startup messages and records actual presented
FPS, vblank rate, CPU/host time, GPU-wait time, present time, idle/scheduler time, and
submit/wait counts. `-GuestProfile` additionally enables the generated-PC call/block profiler and
dumps its summary at exit; use it as a second run because the instrumentation itself adds overhead.
The manager also takes `-GuestProfilePeriod N` (default 3,600 vblanks) to control bounded periodic
captures for duration-limited runs; `0` disables periodic dumps.

When `SR_FPS_CAP` is unset or `native`, the runtime caps host presentation to the PSP's
60000/1001 Hz display scanout, so repeated guest framebuffer submissions within one
scanout do no extra GPU/WSI work. The scheduler and PSP vblank continue at that rate,
and scenes below the cap are not delayed. The launchers use the same scanout pacing by
default: the native player and `nk_launch`, `python tools/nk_cli.py launch` and
`tools/nk_core/launcher.py` export `SR_FPS_CAP=native`. Set `SR_FPS_CAP=0` for
uncapped diagnostics or A/B measurement; a positive value selects that explicit host
presentation cap.

Output slots sit one cap period apart on the host clock, and a submission takes the slot
nearest to it: one that arrives up to half a period early is still presented. A guest that
flips once per VBLANK therefore keeps every frame when its flips reach the host with
scheduling jitter, while a second submission inside the same slot is still dropped and the
long-run present rate never exceeds the cap (`gui-present-selftest`).

### Presentation upscaler (`SR_PRESENT_UPSCALE`)

In the player this is **Sharper enlarging** on Settings > Enhanced and on each game's page, off for a
new library. The Medium, Ultra and Handheld graphics presets turn it on and Low and High turn it off; a
game's own value wins over the preset ([PLAYER_UI_DESIGN.md](PLAYER_UI_DESIGN.md)). The launcher sets
`SR_PRESENT_UPSCALE` from the result for an Enhanced or Ace run.

`SR_PRESENT_UPSCALE=fsr1` makes the Vulkan presenter enlarge the picture to its place in the
window with AMD FidelityFX Super Resolution 1: EASU, an edge-adaptive upscaler, then RCAS
sharpening. `off`, the default, keeps the `SR_PRESENT_SCALING` filter. It runs only when the
picture is enlarged on both axes, on every frame the presenter shows except a movie frame (a
CPU-presented one, or a GE frame that shows a decoded movie) while `SR_MOVIE_UPSCALE` is on. It is presentation only:
the guest's frame, guest VRAM and captures are byte for byte what they are with it off
(`gpu-capture-selftest` and, with a Vulkan device and a display, `display-smoke-fsr1`). A run
recording (`SR_VIDEOREC_DIR`) is enlarged by FSR 1 too, at the recording's size, and its
`record.json` names the filter ([DEBUGGING.md](DEBUGGING.md)). The pause
overlay is drawn at the window's size on the CPU, so the paused picture is not upscaled and looks
softer than the running one. Its cost is small. On an RTX 3080, EASU and RCAS took 0.40 ms per
present from 1920x1088 to 3840x2160, 0.20 ms to 2560x1440, 0.11 ms from 960x544 to 1920x1080, 0.06 ms
to 1280x800 and 0.11 ms from 480x272 to 1920x1080 (`fsr1-selftest`, which times each graphics preset's
sizes, median of 15; measured on 2026-11-08 for #2150, and a second, independent run matched within
0.01 ms). No low-end or handheld GPU has been measured. An unknown value, or a device without
compute, leaves it off with a line in the log; so does a size whose images cannot be made, which
is then not tried again. The launcher turns it off for an authentic session, and a run with it on
is stamped as enhanced (`ACE_MODE: present upscale fsr1`), which `boot_gate.py` reads. The picture
gate and the library sweep set it `off` for their own runs.

### Movie upscaler (`SR_MOVIE_UPSCALE`)

`SR_MOVIE_UPSCALE` gives movie frames their own upscaler: frames the CPU presents, which are decoded
movies (and every frame when the software GE draws), and on the Vulkan GE renderer the GE frames that
show a decoded movie (#2111): `fsr1` scales them with FSR 1, and `anime4k` first doubles
them once or twice with Anime4K's CNN x2 (M) network, which suits the flat colours and dark lines
of animated and pre-rendered movies, then takes them to the window's size with FSR 1 (unless the
doubled picture is already that size, as at the default 960x544 window). `off`, the
default, leaves them to `SR_PRESENT_UPSCALE`. It applies in the window and in a recording
(`SR_VIDEOREC_DIR`), each with its own passes at its own size. Use `anime4k` for line art and movies
only: on other pictures, such as fine regular stripes, it adds a faint ripple along edges. A movie
the GE presents is found by its provenance: the movie decoder notes where each picture lands, and a
GE frame is a movie frame when pictures drawn as a texture (Hot Shots Tennis draws a byte-exact copy
of each picture), or decoded into the displayed target with no GE drawing after them, cover at least
half of it ([ENHANCEMENT_CONTRACT.md](ENHANCEMENT_CONTRACT.md), section 2.2). A movie the game
converts or scales on its way to the screen, or decodes behind a swizzled VRAM view, stays a GE
frame and is not upscaled.
Like `SR_PRESENT_UPSCALE` it is presentation only: the decoded movie, the guest's frame, guest VRAM
and captures are unchanged (`display-smoke-anime4k`, which needs a Vulkan device and a display). A
run with it on is stamped as enhanced (`ACE_MODE: present upscale movie-anime4k`, a
`phase=present_upscale` boot event and `runtime.enhancements` in the flight bundle), so boot and
flight evidence never takes it as authentic; an Authentic session from the launcher, the picture
gate and the library sweep run with it `off`. A 480x272 movie frame costs 0.31 ms through two
Anime4K stages and 0.75 ms with FSR 1 to 3811x2160 on an RTX 3080 (`anime4k-selftest`).

Runtime logs are written under `logs/`. Use `SR_DEBUG=0xFF` for all categories or consult [DEBUGGING.md](DEBUGGING.md) for targeted logging.
On Windows, runtime output redirected to a file or pipe is buffered: a line reaches the log within 50 ms,
and everything is written when the game exits, aborts or writes a crash report. A process killed from
outside, such as by the player's Stop or Task Manager, can lose its last 50 ms of output.

Keyboard controls mirror a PSP layout: `X` = Cross/confirm, `Z` = Circle/back, `A` = Square,
`S` = Triangle, `Q`/`W` = L/R, Enter = Start, Shift = Select, and arrow keys = D-pad.
SDL3 gamepads use the south/east/west/north face buttons as Cross/Circle/Square/Triangle.
Short presses are latched until one PSP controller sample consumes them, so normal taps work
even while a frame is slow.

The full `make verify` command needs external trace data that is not in the repository. Missing `CODEGEN_ORACLE`, `MICROTEST_MODULE`, or `MICROTEST_ORACLE` inputs report `NOT_RUN` with a non-zero result. The hardware differential gate is optional: with neither `PSP_HARDWARE_TRACE` nor `LOCAL_COSIM_TRACE` it reports `NOT_RUN` without changing the result, supplying only one fails, and a matching pair reports `STRICT_V2_AGREEMENT` from the traces' own tier metadata, which is not device attestation. Legacy v1 and `PPSSPP_CORROBORATIVE` traces are corroborative only for hardware evidence.

### Cheats (CWCheat files)

Cheats are off unless you turn them on ([#2639](https://github.com/Jstar269/nakagawa-recomp/issues/2639)).
The project ships no cheats and downloads none: you supply the file, as text in the CWCheat format
that PPSSPP's per-game cheat files and `cheat.db` use.

**Where the file goes.** One file per game, named by the disc ID without its hyphen, in the
`cheats` folder of the user data folder (create the folder if it is not there):

```text
<user data>/cheats/<DISC_ID>.ini      for example  <user data>/cheats/ULUS12345.ini
```

**Turning them on.** Either of:

- **Cheats** on Settings > Online and developer options in the player (off by default; stored as
  the optional `"cheats"` key of `settings.json`).
- `nakagawa_player --cheats`, for that session only, or `nakagawa_player --cheats-file=<path>`
  to read one file you name instead of the game's own (the file must exist). Neither is saved.

The launcher then starts the game with `SR_CHEATS=1` and `SR_CHEATS_FILE=<the file>`. It
exports both for every game it starts (`SR_CHEATS=0` and an empty file name while cheats are off),
so a value left in your environment never turns cheats on. With cheats off the runtime opens no
cheat file and writes nothing.

**The file.**

```text
_S ULUS-12345
_G The game's name
_C1 A cheat that is on
_L 0x20001234 0x000003E7
_C0 A cheat that is off
_L 0x10001238 0x00000063
```

`_S` names the disc the lines after it are for (hyphens and letter case do not count; a file with
no `_S` line is for the game it is read for, and cheats under another disc's `_S` are counted
and left out). `_G` is the game's name and is not read. `_C0` starts a cheat that is off and
`_C1` one that is on; each `_L` line under it is one code line of two hexadecimal words. `//`
and `#` start a comment. To turn a cheat on or off, change its `_C0` or `_C1` and start the
game again. A `_C0` or `_C1` line with no name after it starts no cheat, as in PPSSPP: the log
notes it, and the `_L` lines after it stay with the cheat before it.

**While a cheat is on** the game's window shows a marker at its top left, `Cheats on: 2`, or
`Cheats on: 2 (1 refused)` when the file also turned on a cheat that cannot run. The marker cannot
be hidden. It is drawn on the window only, not into captures, recordings or the game's own frame.
The run's log says what the file held and names every refusal on lines that start `CHEATS:`.

**When they apply.** The cheats that are on are applied once per VBLANK, 59.94 times a second,
after that VBLANK's interrupt handlers and its pad sample. A button condition reads that sample,
which is the one the game reads next. The one exception is an Ace run with late pad sampling
(`pad=late`), which may refresh the sample after the VBLANK and before the game reads it: the
condition was then tested on the earlier state, one frame's difference.

PPSSPP applies a cheat file on a timer instead, every 77 ms of emulated time unless you change its
refresh interval, about 13 times a second. A code that sets a value behaves the same at either
rate. A code that changes a value every time it runs (the `0x3` add and subtract forms, a `0x7`
XOR) changes it about 4.6 times as often here as in PPSSPP at that default, so a cheat tuned to
PPSSPP's timer counts faster here. There is no setting for the rate.

A cheat applies whole on a VBLANK or not at all: when a pointer it follows leads outside user
memory or into the game's code on some VBLANK, what it had already written on that VBLANK is put
back and the cheat is held until the pointer is usable again. The log says so once for each
cause, and once when the cheat first applies again; a cheat that is held and released again and
again adds no more lines, and the end of the run gives how many VBLANKs each such cheat was held
and applied on.

**Where an address is.** A code names a PSP address: `aaaaaaa` in the table below is an offset
from `0x08800000`, and a game's own image starts at `0x08804000` on a PSP. For almost every game
the package's memory is laid out the same way and an address is used as written. A few title
profiles load the game's image at address 0 instead (a title manifest whose executable has
`"base": 0`; Hot Shots Tennis, `UCUS98701`, is the one in this repository). There the image is
somewhere else and so are the heap and the stacks, so:

- an address inside the game's image (the data and variables the executable itself holds) is
  moved with the image and the cheat works; a write into the image's code is refused there as
  everywhere;
- a cheat with an address anywhere else (memory the game allocates while it runs) is refused by
  name, because where that memory is in such a run is not known and a cheat is never applied to a
  guess;
- a pointer cheat whose pointer is still null (before the game has set it up) is held: null plus
  an offset is not an address the game set up, and offsets are not moved with the image, so in
  such a run it could land on the moved image's own bytes or on its heap. A PSP holds the same
  cheat only when the offset is below `0x08800000`, the start of user memory.

The log says so at the start, on a line that begins `CHEATS: this game's image is at`. If the game
starts another program (a launcher that loads the real game, for example), cheats stop from there
on, by name: the file was read for the first program's memory.

**Code forms that run.** `aaaaaaa` is an offset from `0x08800000`
([where an address is](#cheats-cwcheat-files), above). Comparisons are unsigned.

| First word, second word (and a following line) | What it does |
| --- | --- |
| `0x0aaaaaaa 0x000000vv` | writes 8 bits |
| `0x1aaaaaaa 0x0000vvvv` | writes 16 bits |
| `0x2aaaaaaa 0xvvvvvvvv` | writes 32 bits |
| `0x301000vv 0x0aaaaaaa`, `0x302000vv 0x0aaaaaaa` | adds to, or subtracts from, 8 bits |
| `0x3030vvvv 0x0aaaaaaa`, `0x3040vvvv 0x0aaaaaaa` | adds to, or subtracts from, 16 bits |
| `0x30500000 0x0aaaaaaa` then `0xvvvvvvvv 0x00000000` (`0x306`: subtract) | adds to, or subtracts from, 32 bits |
| `0x4aaaaaaa 0xnnnnssss` then `0xvvvvvvvv 0xiiiiiiii` | `nnnn` 32-bit writes, `ssss` * 4 bytes apart, the value growing by `iiiiiiii` each time |
| `0x8aaaaaaa 0xnnnnssss` then `0x000000vv 0xiiiiiiii` | the same in 8 bits, `ssss` bytes apart |
| `0x8aaaaaaa 0xnnnnssss` then `0x1000vvvv 0xiiiiiiii` | the same in 16 bits, `ssss` * 2 bytes apart |
| `0x5aaaaaaa 0xnnnnnnnn` then `0x0bbbbbbb 0x00000000` | copies `nnnnnnnn` bytes from `a` to `b` (the two may not overlap) |
| `0x6aaaaaaa 0xvvvvvvvv` then `0x000tnnnn 0xiiiiiiii` | through the pointer stored at `a`: writes `v` at pointer + `i` (`t` = 0, 1, 2 for 8, 16, 32 bits) or pointer - `i` (`t` = 3, 4, 5). `nnnn` - 1 more lines follow, each `0x2ooooooo` or `0x3ooooooo`: the pointer becomes the pointer stored at pointer + `o` or pointer - `o` |
| `0x7aaaaaaa 0x000t00vv` or `0x000tvvvv` | OR (`t` = 0, 1), AND (2, 3) or XOR (4, 5) on 8 or 16 bits |
| `0xDaaaaaaa 0x00c0vvvv` or `0x20c000vv` | runs the next line only if 16 or 8 bits at `a` are equal (`c` = 0), not equal (1), less (2) or greater (3) |
| `0xD00000nn 0x1bbbbbbb` | runs the next `nn` + 1 lines only while every button of the mask is held (`0x3bbbbbbb`: unless they all are) |
| `0xDaaaaaaa 0x4bbbbbbb` then `0xnnnnnnnn 0x0000000s` | runs the next `n` lines only if the values at `a` and `b` are equal (`0x5`: not equal, `0x6`: `a` less, `0x7`: `a` greater); `s` = 0, 1, 2 for 8, 16, 32 bits, the two orderings only in 8 and 16 bits |
| `0xE0nnvvvv 0xcaaaaaaa`, `0xE1nn00vv 0xcaaaaaaa` | runs the next `nn` lines only if 16 or 8 bits at `a` compare as `c` says |

**What is refused, and why.** A cheat with a line that cannot run exactly as written is refused
whole when the file is read: none of its lines is applied, the log names the cheat, the line and
the reason, and the marker counts it. Nothing is skipped silently and no cheat is half applied.
The other cheats in the file still run.

| Refused | Why |
| --- | --- |
| A line that is not `_L` and two hexadecimal words, or a cheat with no code line | it does not say what to do |
| Code types `0x9`, `0xA`, `0xB`, `0xC` and `0xF`, and `_M` and `_N` lines | not supported: `0xA` is PPSSPP's own additions, `0xB` a delay and `0xC` a code stopper; `0x9` and `0xF` have no meaning in the reference |
| A form above with digits set where it shows zeros, a value wider than the form holds, a count of 0 | the reference gives it no single meaning, so none is chosen |
| The `0x6` pointer lines `0x1ooooooo` (copy) and `0x9ooooooo` (several addresses), and a `0x6` with a base offset | not supported |
| A condition that guards no line, runs past the end of its cheat or ends inside a two-line code | half a code would run |
| An address outside user memory (`0x08800000` up to `0x0C000000`), or a span that leaves it | cheats write the game's memory, never the kernel's, VRAM or anything past the end |
| In a game whose image is loaded at address 0: an address outside the game's image | where that memory is in such a run is not known ([where an address is](#cheats-cwcheat-files)) |
| A cheat that could write more than 1 MiB in one VBLANK | it cannot be put back if it has to be held |
| A cheat that does not fit what the file's cheats may do in one VBLANK together: 16,384 writes, 1 MiB written and 16,384 codes, handed out in file order | the cheats run between two frames of the game, so their time is the game's; a file that asked for more used to stall the game without a word. Turn another cheat off to use the refused one |
| Code lines past the 262,144 the reader keeps for one disc; cheats past the first 4,096 for one disc are not read and are counted | a file cannot make the game start with gigabytes of cheat records (the bound is about 20 MiB at the peak of reading a file and about 17 MiB after it, beside the file's own text; a file over 64 MiB is refused whole) |
| A write that lands in the game's code (a "code write") | a recompiled game runs the code translated when its package was built and does not read its code from memory again, so the write could not change what the game does |

The last row is the difference from an emulator that matters most: cheats that patch instructions
(many "60 FPS" and "widescreen" codes) cannot work here, and are refused instead of doing nothing
quietly. Cheats that write values (health, money, items, timers the game stores as data) work.

**Runs that refuse every cheat**, each with a `CHEATS:` line saying so:

- a run that records or replays (`SR_RECORD`, `SR_REPLAY`), because a recording has to replay
  without a cheat file;
- an acceptance checkpoint (`SR_HEADLESS_ACCEPT_PRESENTED_FRAMES`), which is evidence of the game
  as it is. The player's `--headless-accept-presented-frames` starts its game without cheats, the
  launcher refuses to start one with them, and a runtime started by hand refuses them itself;
- a run in the GDI window (`SR_VIDEO=gdi`, or when Vulkan does not start), because that window
  cannot show the marker; and a run whose window stops drawing the marker later, from the next
  VBLANK on;
- a run with no game image placed by the host loader, and the rest of a run whose game started
  another program;
- a run whose game image is in user memory but not at `0x08804000` (the display-smoke fixture's
  `0x08810000`, for one): a PSP loads a relocatable image at `0x08804000`, so a cheat written for
  it would land the distance off, and such a run cannot tell a moved image from one linked where
  it is.

A package carries the runtime it was built with. One built before cheats existed would ignore the
switch without a word, so the launcher does not start it with cheats on: it says that the package
was built before cheats existed, and to rebuild the package or turn cheats off.

RetroAchievements runs in Casual Mode only here, and it is off for a game started with cheats on: no
achievement set is loaded and nothing is earned. The game's page and Settings > Online and developer
say so.

The start of a run names at most 32 refused cheats one by one and counts the rest on one line.

The meaning of each code form is PPSSPP's: the reader and applier are translated from its CWCheat
engine, the public reference for the format (lineage in
[`src/core/PROVENANCE.md`](../src/core/PROVENANCE.md)). It is PPSSPP-derived, not measured on a
PSP, and the log says so once per run, at the first VBLANK that applies a cheat.

### Public synthetic verification routes

These routes verify the toolchain and the pipeline without any proprietary game
input. Run them from a shell whose `PATH` includes the MSYS2 UCRT64 tools: a
UCRT64 terminal, or PowerShell after `$env:Path = "C:\msys64\ucrt64\bin;$env:Path"`.

```powershell
.\tools\nk_manager.ps1 -Action Test            # selftest gate (make selftest)
mingw32-make player                      # build/nakagawa_player.exe, the native player
mingw32-make production-smoke            # complete two-phase pipeline smoke test
mingw32-make platform-ladder             # relocations, scheduler, scalar FPU, filesystem
mingw32-make cosim-selftest              # differential AOT vs. interpreter cosimulation
mingw32-make showcase showcase-smoke     # build the showcase demos (WSL 2 + PSPDEV on Windows), then run both headlessly
```

- The **differential cosimulation harness** verifies semantic parity between
  AOT-generated code and the fail-closed interpreter floor.
- The **platform ladder** exercises relocations, scheduler threading, scalar FPU,
  and filesystem semantics across synthetic workloads.
- The **production and display smoke fixtures** (`mingw32-make production-smoke`,
  `mingw32-make display-smoke`) test the complete two-phase build pipeline and
  display bring-up without proprietary inputs; `mingw32-make display-smoke-player`
  also launches the fixture through the native player.
- The **source-owned showcase demos** are project-authored PSP programs that
  traverse the whole pipeline into validated packages the player discovers on
  its own ([`SHOWCASE.md`](SHOWCASE.md)).

What hosted CI runs, and what each check proves, is defined in [`CI.md`](CI.md);
the pipeline itself is described in [`ARCHITECTURE.md`](ARCHITECTURE.md).

### Player BUILD PACKAGE prerequisites

**BUILD PACKAGE** in the native player re-runs this toolchain from inside the app, so
it needs all of the following:

- `tools/nk_cli.py` reachable. The player searches, in order: the `NK_INSTALL_ROOT`
  environment variable (a folder containing `tools/`), `<player exe>/tools`,
  `<player exe>/../tools`, `<player exe>/../source/tools` (the v0.0.1 release
  layout). The working directory is never searched, so a `tools/nk_cli.py` in the
  folder the player starts from is never run. A player built in a checkout's
  `build/` finds it automatically.
- `python`, `gcc`, and `mingw32-make` from either the current environment or the
  player's per-user downloaded build tools. The player changes `PATH` only for
  the build child; it never edits system `PATH` or the registry.
- PowerShell. The unpacking of the downloaded Python archive (`Expand-Archive`)
  uses PowerShell 7 (`pwsh`) when it is installed and otherwise the built-in
  Windows PowerShell 5.1, so nothing needs installing; `NK_POWERSHELL` names a
  specific one. The asset-copy step uses `powershell.exe` (5.1) through
  `POWERSHELL` in the `Makefile`.

A release folder can carry the pinned prerequisites itself, in `prereqs/` beside `bin/` and
`source/` ([#1285](https://github.com/Jstar269/nakagawa-recomp/issues/1285)). BUILD PACKAGE
then uses that folder ahead of `PATH` and the per-user download, so it shows no consent card
and opens no network connection. Before each build the player checks the folder against
`source/assets/prereq_manifest.json`: `prereqs/installed.json` must record exactly the pinned
artifacts with their pinned version, size and SHA-256, and every file listed in
`prereqs/bundle-inventory.txt` (one `<sha256> <size> <path>` line per file) must exist with
its recorded size. A folder that fails the check is refused with
`PREREQUISITE_BUNDLE_INVALID`, naming the first missing or changed file, and nothing is
downloaded in its place: extract the release zip again into a short folder path. The player
does not hash the bundled files, about 7,600 files and 700 MB, because that would hold BUILD
PACKAGE for seconds; packaging verifies them. **Settings → About & Licenses** then lists the
bundled components and opens `prereqs/notices`, and **Remove Downloaded Build Tools** never
touches `prereqs/`.

When a pinned Windows prerequisite is missing, BUILD PACKAGE opens one consent
card showing each component's name, version, source host, download size, and
licence, plus the total. Nothing is downloaded until **DOWNLOAD** is selected;
the answer applies only to that build. If CPython is missing, the native player
first downloads the pinned embeddable archive and checks its HTTPS host, exact
size, and SHA-256 before extracting it. The verified runtime then runs the
Python fetcher for the remaining packages. A progress card shows the current
component and received/total bytes. **CANCEL** removes partial downloads and
staged extraction data. A successful install resumes BUILD PACKAGE
automatically. Named error cards explain offline, redirect, size, hash, and disk
errors and offer a retry.

The pinned candidate download set is recorded in
[`assets/prereq_manifest.json`](../assets/prereq_manifest.json): CPython 3.15.2
and 34 MSYS2 UCRT64 packages (GCC/binutils, make, SDL3 and `SDL3_ttf` with the
readable-font runtime closure — FreeType, HarfBuzz, Graphite2, libpng, bzip2,
Brotli, GLib, and PCRE2 — Vulkan headers/loader, and their runtime dependencies),
totaling 101,892,265 bytes. Package hashes and
sizes come from the signed MSYS2 repository database; the Python hash is from
python.org's release page. The manifest's `source_snapshot_date` dates the MSYS2
package pins only, and a package re-pinned after that snapshot (Brotli 1.2.1 for
CVE-2026-111497; SDL3, the mingw-w64 CRT, headers and winpthreads, libiconv, libpng, tzdata and
zlib on 2026-11-05) records its own `sha256_source.retrieved_utc`; the locked developer-tool pins carry their own retrieval
date in `assets/pypi_tool_metadata_*.json`. `tools/requirements-lock.txt` contains developer and
build-generation tools; the consumer `build-package` path needs no third-party
Python packages, and `glslc` is only used by opt-in shader regeneration.

Each artifact also names its corresponding source in the manifest's `sources` list: the MSYS2
source package (`*.src.tar.zst`) of its pkgbase at the same version, or the CPython source
release, pinned by URL, size and SHA-256 ([#1285](https://github.com/Jstar269/nakagawa-recomp/issues/1285)).
The CPython entry also names the source releases of the libraries built into its runtime
(`bundled_sources`), and `source_notices` copies licence files out of the pinned sources for the
packages whose binaries carry none. The player never downloads them. A release publishes them as one archive next to its zip, and
the zip's bundled build tools leave out the GFDL-licensed manuals in `share/doc`, `share/info`
and `share/man` while keeping every licence file; the policy is in
[`PREVIEW_RELEASE.md`](PREVIEW_RELEASE.md#bundled-prerequisite-licensing-policy).

Archives are unpacked only after their hash matches, and every member must be a
plain relative name a Windows folder stores as written. A backslash stops the
install with `MANIFEST_INVALID`; an absolute path, a `..` part, a colon (a
drive-relative `a/D:x.dll` or a data stream), a character Windows refuses
(`<>"|?*` or a control character), a trailing dot or space, or a device name
such as `NUL` or `COM1.txt` stops it with `ARCHIVE_PATH_REJECTED`. A tar link
whose target has one of those parts stops it with `ARCHIVE_LINK_REJECTED`.
Nothing is kept from a refused archive.

Downloaded tools, verified archives, and extracted licence texts live under the
current user's Nakagawa data folder in `prerequisites/`. **Settings → About &
Licenses** lists installed components, versions, and licence identifiers and
opens the notice folder. **Settings → Remove Downloaded Build Tools** removes
that prerequisites folder after confirmation; it does not remove the game
library, ISO files, saves, or built packages. Every pinned entry carries a resolved
SPDX licence expression ([#1285](https://github.com/Jstar269/nakagawa-recomp/issues/1285)): the
public-domain tzdata and windows-default-manifest packages use `LicenseRef-` identifiers, and
their `notice_location` names the notice. `test_every_pinned_license_is_a_resolved_spdx_expression`
in `tools/test_prereq_fetcher.py` refuses `NOASSERTION`; the consent card would still mark such an
entry as under review.

This automatic prerequisite flow currently targets Windows x64 with the
UCRT64 package set. Linux prerequisite installation is not supported
([#306](https://github.com/Jstar269/nakagawa-recomp/issues/306)); the wider
distribution and update contract remains tracked by [#324](https://github.com/Jstar269/nakagawa-recomp/issues/324).

The player's UI typography loads `SDL3_ttf.dll` by full path from beside the
executable, then from beside the `SDL3.dll` it runs (with that folder searched for
the library's own dependencies); it is never searched for by name, so a copy that
is only in the current folder or on `PATH` is not used. Placing `SDL3_ttf.dll`
(and its dependency closure) next to `nakagawa_player.exe` is enough — no rebuild
required.

### Network use

Nakagawa works offline. The player and the runtime make no network connection unless you
agree to one, and ordinary single-player play reaches no host. Every connection the player or
runtime can make is listed below, and each is opt-in and off by default.

| What | Host | When | Controlled by |
| --- | --- | --- | --- |
| Build-tool download | `www.python.org`, `repo.msys2.org` (HTTPS) | Only after you select **DOWNLOAD** on the BUILD PACKAGE consent card | That consent card, per build |
| Check for updates | `api.github.com` (HTTPS) | When you select **CHECK FOR UPDATES** (Settings > Updates), or once at start if you turn on **Check at start** (off by default) | That button, and that switch |
| Update download | `github.com`, which redirects to `release-assets.githubusercontent.com` (HTTPS) | Only after a check finds a newer version and you select **DOWNLOAD** | That button, per download |
| RetroAchievements | `retroachievements.org`, and `media.retroachievements.org` for badge pictures (HTTPS) | Only after you enable **Achievements** in Settings > Online and developer options and accept the consent screen; sign-in, achievement-set, session, unlock and Rich Presence requests follow while it is enabled, and the badge pictures of a loaded set while you are signed in | The Achievements switch and its consent screen |
| Download cover art | `thumbnails.libretro.com` (HTTPS) | When **Download cover art** is on (Settings > Online and developer, off by default), once per verified Redump disc not yet cached | That switch, and **Refresh cover art** (Settings > Build tools and cache) |
| Online play (ad hoc relay) | The shipped servers `socom.cc`, `relay.arenaanywhere.site`, `relay-usa.arenaanywhere.site`, `relay-sa.arenaanywhere.site` and `relay-asia.arenaanywhere.site`, a relay server you pick from PPSSPP's list (any host it names), or a server you enter yourself (`27312` lobby, `27313` data, plain TCP) | Only after you select **TURN ON** on the Online play card, which names that exact server, and only while a game that starts PSP ad hoc networking runs | The Online play card; **Settings > Online and developer > Online play** (`online_play` in `settings.json`, off by default) turns it off and ends the connection |
| Online play server list | `metadata.ppsspp.org` (HTTPS) | Only while Online play is on, when you open the server picker or select **Refresh** in it | Online play being on; no download starts at start, in the background, or while Online play is off, and one already under way when you turn it off finishes but its list is thrown away |

**RetroAchievements** is off by default. Its consent card, a launcher page, names `retroachievements.org` and
`media.retroachievements.org` and the data sent:
your RetroAchievements user name and API token, the game hash, achievement unlocks and Rich Presence text,
which a running game sends every two minutes. Leaderboard entries are not sent. Every request goes to
`https://retroachievements.org/dorequest.php`, except the achievement badge pictures: while you are
signed in, the player asks `https://media.retroachievements.org/Badge/<badge>.png` (`<badge>_lock.png`
for a locked achievement) for each badge of the loaded set, one at a time and each at most once while
its game's page lists it, with a plain GET that carries only the user agent below: no user name, token, cookie or
console ID. A picture that does not come, is larger than 64 KB or 128 x 128 pixels, or is not a PNG is
not shown; the game's page and the in-game notice draw their own icon instead. Fetched pictures are kept
in the `achievements-badges` folder under the player data folder (at most 2,048 files). **Sign out**
deletes that folder; turning **Achievements** off fetches nothing more and leaves it until the next
**Sign out**, and you can delete it yourself at any time. The pictures are RetroAchievements' public
artwork and hold no user data; a token replaced outside the player leaves them until the next
**Sign out**. No other host is contacted. During sign-in, your user name and password are sent once to obtain the token; the
password is not saved. The token is kept in Windows Credential Manager. Other platforms without that
credential store refuse sign-in. Nakagawa identifies a game by hashing the raw `PSP_GAME/PARAM.SFO` and
`PSP_GAME/SYSDIR/EBOOT.BIN` bytes from your readable disc image, in that order; it does not decrypt them for
this check. Game files, saves and guest memory stay on this PC. Offline unlocks wait in an owner-only file
under the player data folder until they can be submitted. The file names its account by a tag (a hash of
the account name, not the name), and only that account's session sends its unlocks: **Sign out** discards
the unlocks still waiting, and unlocks another account left are discarded and reported, never sent for
the account now signed in. A queue file that cannot be read is kept beside itself as
`unlock-queue.bin.unreadable`, never written over. An unlock the server refuses leaves the file and is
reported on the **RetroAchievements** account line. When the sign-in at start cannot reach the server, the stored token is
tried again by itself (after 30 seconds, then at doubling waits up to ten minutes); no password is asked. The feature offers only Casual Mode, the mode RetroAchievements formerly called Softcore:
Hardcore is not available, and a request that would claim it is refused before it is sent. Achievements
are off for a game started with cheats on or with a package that changes its speed (60 FPS game logic);
the game's page says why. Turning the switch off sends nothing more: a request waiting its turn is dropped, and only one already on its way finishes.
Its requests identify themselves in the form RetroAchievements documents:
`NakagawaRecomp/<version> (<operating system and version>) rcheevos/<version>`, such as
`NakagawaRecomp/0.1.0 (Windows 10.0.26100) rcheevos/12.5`. No telemetry is sent.
To turn it on, open **Settings > Online and developer options**, switch on **Achievements**, read the consent card and
choose **Agree and turn on**. The launcher's sign-in page opens: choose **Username** and **Password** (each opens the
on-screen keyboard, which a keyboard, mouse or controller can use; the password shows as stars), then **Sign in**.
RetroAchievements offers no sign-in web page or OAuth for other apps (rcheevos signs in only with a password or a token
it issued), so the password is entered in the launcher, as in other emulators. The game's page lists the core
achievements after the set loads.

A release folder that carries a verified `prereqs/` tree never makes the build-tool
download ([#1285](https://github.com/Jstar269/nakagawa-recomp/issues/1285)).

**Online play** (Settings > Online and developer > Online play, off by default;
[#1139](https://github.com/Jstar269/nakagawa-recomp/issues/1139)) carries PSP local wireless ad hoc
multiplayer over the internet through an aemu_postoffice / PPSSPP community relay server
(`src/core/nk_adhoc_relay_client.c`, runtime opt-in `SR_ADHOC_ENABLED` and `SR_ADHOC_SERVER`).
The Online play card is the only way to turn it on: choosing the switch, or picking a different
server, opens the card, which names that exact server and lists what is sent, with **TURN ON**,
**NOT NOW** and **CHANGE SERVER**. Nothing connects until you select **TURN ON** for that server.
Its two connections are **unencrypted plain TCP** with no sign-in: lobby port `27312` and data
relay port `27313` by default, so anyone on the network path or operating the server can read the
traffic. Only the ID of the game you start, the console nickname (your **Nickname** setting, or
the one a game saved in its place, as a PSP game can), the random console ID (a MAC address
Nakagawa made, never the PC's), the name of the ad hoc group (room) the game makes or joins, and
the game's PDP/PTP packets are sent (`ONLINE_FEATURES.md` details the exact protocol); no game
files, saves, guest memory, host paths, or PC identifiers are sent. **New console ID** replaces
that random ID. The advanced **Port offset** (default `10000`, the value PPSSPP uses) shifts the
game's ports on the relay; players who want to play together need the same value. Turning Online
play off ends every connection of a running game within a moment (the lobby and each of the
game's data streams, even after the lobby was lost), and the game opens no new one until it is
started again. Custom server entries stay local in `settings.json`. When Online play is off, no
host socket opens and strictly-online ad hoc imports stay fail-closed.

The card, the turn-off and the masking of inherited values are the player's. It always sets
`SR_ADHOC_ENABLED`, `SR_ADHOC_SERVER`, `SR_ADHOC_PORT_OFFSET` and `SR_ADHOC_OFF_FILE` for the game
it starts (`src/core/nk_launch.c`), off unless Online play is on, so an inherited value never turns
it on. A developer who runs a game executable directly with `SR_ADHOC_ENABLED=1` and
`SR_ADHOC_SERVER` set connects to that server without the card, and without a turn-off file
(`SR_ADHOC_OFF_FILE`) nothing but closing the game ends the connection.

The server picker shows the shipped servers and your own server's row. While Online play is on it
also shows PPSSPP's public list of ad hoc servers, downloaded over HTTPS from `metadata.ppsspp.org`
when you open the picker or select **Refresh** (`src/player/adhoc_servers.c`, user agent
`NakagawaRecomp/OnlinePlay`), and never while Online play is off. The download is capped at 64 KiB and 64 servers, read as untrusted input, and kept for that session
only; an answer from another host is refused, and the list's icon, web, status and Discord links
are never fetched. The download runs on its own thread and cannot be cancelled: if you turn Online
play off while it runs, it finishes and its list is thrown away. Only relay servers this player can
talk to (`data_mode` `AemuPostoffice`, a reading of PPSSPP's code that is PPSSPP-derived, not
PSP-measured) are offered. Where the download is unavailable, the picker says
why and offers the copy shipped with the player instead, marked as that copy, the relay
entries of PPSSPP's list at revision `3bc60270`: `socom.cc`, `psp.mgn.pub`, `eahub.eu`,
`psi-hate.com`, `jpa36a7.glddns.com`, `relay-sa.arenaanywhere.site`, `relay.arenaanywhere.site`,
`relay-usa.arenaanywhere.site`, `relay-asia.arenaanywhere.site`, `sayaakanlawan.my.id`,
`adhoc.gamesnexus.ovh`, `cvn-play.top`, `ntarena.duckdns.org`, `38.54.80.95`, `cat.wjg.jp`,
`brazil-01.sprawl-relay.link`, `pspbrasil.duckdns.org`, `jeddah-relay.ddns.net`, `140.245.237.12`,
`185.183.157.234`, `psp.andexplaystream.online` and `132.226.189.224`. The downloaded list may name
any host (a maintainer decision of 2026-11-20 on #1139), so these are only its shipped copy. No entry
ever connects by itself: a server from either list is reached only after you pick it and select
**TURN ON** on the card that names it.

The disc check needs no connection either: Redump's PSP list ships beside the player
(`assets/redump/`), and a newer list is one you download yourself and import
([Disc check](COMPATIBILITY.md#updating-the-disc-list)).

**Cheats** (Settings > Online and developer options, off by default) make no network connection
either: the cheat file is one you put in the user data folder yourself
([Cheats](#cheats-cwcheat-files)).

**Discord status** (Settings > Online and developer, off by default) makes no network
connection either. While a game runs, the player passes three things to the Discord app on this
PC through Discord's local named pipe (`discord-ipc-0` to `discord-ipc-9`): the game's title as
the disc names it (its disc ID when the disc names no title), the play mode (Authentic, Enhanced
or Ace) and when the game started, with the player's process ID, which Discord's protocol needs. The
Discord app then shows them on your profile, so Discord learns which game you play. Nothing else
is sent: no path, save, user name or game memory. If Discord isn't running, the player tries
again at most once a minute and says nothing; the status clears when the game exits. A build
without the project's Discord application ID has no Discord status switch
([`ONLINE_FEATURES.md`](ONLINE_FEATURES.md#discord-status)).

Enhanced and Ace (Settings > Enhanced) make no network connection: its features run on this PC, and its per-game report
stays in the user data folder.

Links the player shows, such as a release page, **Report this game**, **See existing reports** and a
known issue's page, are not player connections: selecting one opens it in your own browser, as
following any link does, and nothing opens on its own. A report link holds only the disc ID and
version, the Nakagawa version, the system and the stop reason, never a path, user name, log or
setting ([`ONLINE_FEATURES.md`](ONLINE_FEATURES.md#reports)).

The update check asks GitHub's public Releases API which versions of
`Jstar269/nakagawa-recomp` exist, and nothing else. The request is anonymous: it names itself
`NakagawaRecomp/UpdateCheck` and carries no ID and no information about your PC or your games. The
Updates page then shows this version, the newest one (pre-releases count), the release page link and the
first lines of its notes. **DOWNLOAD** first fetches the release's `.sha256` file and its signature,
`.sha256.minisig` (two small files from the same hosts), and checks the signature with the project's
update-signing public key, which is built into the player
([`RELEASE_SIGNING.md`](RELEASE_SIGNING.md)). Only then does it fetch the Windows ZIP, and it keeps the
ZIP in the `downloads` folder of Nakagawa's data folder (`%LOCALAPPDATA%\Nakagawa\data\downloads`) only
if its SHA-256 matches that signed checksum file, and GitHub's own checksum of the ZIP when GitHub lists
one; on any mismatch the file is deleted. A release with no signature, a signature that doesn't match,
or a build with no signing key gets no download from the player: the Updates page offers the release
page instead. **INSTALL** is a separate click: it unpacks the checked ZIP into a new folder
beside the running version (or in the downloads folder when Nakagawa doesn't run from a release folder) and
tells you to start the new `bin\nakagawa_player.exe` yourself. It never replaces the running version.
Your library, saves and settings stay in Nakagawa's data folders, outside the install folder, so the new
version finds them. There are no retries and no background checks: a failure shows one "Couldn't Reach
GitHub" card, and a failed check at start stays in the developer log. The release ZIP has no
build-provenance attestation of its own yet; the signed checksum is what ties the download to the
project.

There is no telemetry or crash upload. The `network_mode` entry in
`settings.json` is reserved and always reads `"offline"`; a build treats any other value as
offline. `tools/test_prereq_fetcher.py` fails if shipped code outside the explicit egress allowlist
(`package_builder.c`, `prereq_fetcher.py`, opt-in `nk_adhoc_relay_client.c` and the other modules it
names) uses a network API, if a pinned download, update check, cover art host, shipped relay preset or
other opt-in feature names a host or port not listed in this section, or if new code calls the native
download transport without adding its own documented opt-in.

Release packaging is a maintainer step, not part of the player:
`python -m tools.nk_core.prereq_fetcher` with `--bundle-dest` or `--source-archive` downloads
the same pinned archives and their source files to build a release
([`PREVIEW_RELEASE.md`](PREVIEW_RELEASE.md#recreate-the-package)). It uses the two hosts in
the table and, for the source releases of the libraries built into the CPython runtime, also
`github.com` (whose downloads redirect to `release-assets.githubusercontent.com` and
`codeload.github.com`), `www.sqlite.org`, `sourceware.org` and `www.bytereef.org`. The player
never passes those options.

### Build lifecycle and cleanup targets

The build system provides scoped and explicit cleanup targets:

- `mingw32-make clean`: removes all build outputs for the current target (`build/$(GAME_NAME)`).
- `mingw32-make clean-fixtures`: removes temporary smoke test, cosimulation, and oracle build artifacts (`build/production-smoke`, `build/cosim`, etc.).
- `mingw32-make tidy` (or `distclean`): removes intermediate object files (`.o`, `.d`, profile manifests) and ephemeral build logs while preserving linked binaries (`.exe`, `.pdb`) for debugging.
- `mingw32-make clean-all`: comprehensively removes all subdirectories under `build/` and ephemeral build logs under `logs/`.

These targets strictly operate within `build/` and transient log paths, never deleting protected directories (`place_game_here/`, `memstick/`, `keys/`, `oracle/`, `assets/`, `tests/fixtures/`, `docs/`, `src/`, `tools/`).

Two variables relocate everything these targets touch. `BUILD_ROOT` (default `build`) holds the
default per-title `BUILD_DIR` (`$(BUILD_ROOT)/$(GAME_NAME)`), the shared fixture trees
`clean-fixtures` removes, and the SDL3 discovery cache; `clean-all` empties it. `LOG_DIR` (default
`logs`) holds the ephemeral logs `distclean` and `clean-all` remove; every other log there is kept.
For example, `mingw32-make clean-all BUILD_ROOT=../scratch/build LOG_DIR=../scratch/logs` cleans
only that scratch tree beside the checkout, which is how the Python suite exercises these targets
without touching your real `build/` and `logs/`. An overridden `BUILD_ROOT` must be the checkout's
`build/` tree, a directory beneath it, or a directory outside the checkout; Make refuses the
repository root, an ancestor of it, or any other directory inside the checkout before anything is
created or deleted.

An overridden `BUILD_DIR` (`make clean BUILD_DIR=<x>` deletes `<x>`) gets the same scope rule: it
must be a directory beneath the checkout's `build/` tree or one outside the checkout. `BUILD_DIR=.`,
`BUILD_DIR=src` and the checkout root are refused while Make parses. `clean` and `clean-all` also
refuse a `BUILD_DIR` that is the whole build root, meaning `BUILD_ROOT` or the checkout's `build/` tree
in any spelling (`build`, `build/`, `./build`, an absolute path, backslashes, or another letter case
on Windows), because they delete it wholesale: name one title's directory beneath the root instead, or
run `clean-all` without `BUILD_DIR` to empty `BUILD_ROOT`. The default `BUILD_DIR` derives from
`BUILD_ROOT`, so it needs no separate check. Every Makefile output is derived from `BUILD_ROOT`
(the native test binaries, the player executables, the platform-ladder and display-smoke trees), so
one scratch `BUILD_ROOT` redirects every write a Make run makes.

### Checkout path and `BUILD_DIR`

The repository may be cloned under a path that contains spaces (`C:/path/with spaces/nakagawa`): a
build, `mingw32-make native-core-tests`, `mingw32-make contrib-check` and the Python tooling suite
all work from there, and `tools/test_relocated_clone.py` runs a bounded contributor command set from
a copy of the tracked tree placed under a spaced directory, so the property is exercised rather than
assumed.

Two limits are stated rather than implied, because that check does not reach them:

- **Non-ASCII characters in the checkout path are unverified.** No hosted gate runs from such a path.
  The places measured are the selftest respawn sites named here, and they were measured locally, not
  by a hosted gate.
- **The selftest respawn sites quote their command line only recently.** `tests/native/hle_thread_selftest.c`,
  `tests/native/cpu_lle_selftest.c` and `tests/native/dispatch_isolation_selftest.c` start a second copy of their
  own binary, and they used the CRT's `_spawnl`/`_spawnv`, which joins its arguments without quoting:
  a spaced self path was re-parsed as extra arguments, the mode flag never arrived, and the child ran
  the whole suite instead of the requested mode. The HLE exit-game site was corrected in PR #680 and
  the CPU-LLE and dispatch-isolation sites in PR #681, which also changed all three to decode
  `argv[0]` in the active code page rather than assuming UTF-8, since that is the encoding the CRT
  produced it in. Those fixes closed [issue #667](https://github.com/Jstar269/nakagawa-recomp/issues/667).

`BUILD_DIR` is a narrower contract. GNU Make splits a target or prerequisite name on whitespace, so a
`BUILD_DIR` containing a space cannot be named by Make at all: the name silently becomes several
targets, which previously let `make clean BUILD_DIR=...` remove the first fragment — a directory the
caller never named — and left a `spaces/` directory tree in the checkout root. The Makefile now
refuses such a `BUILD_DIR` before any recipe runs and names the boundary:

```text
BUILD_DIR 'C:/path/with spaces/build' contains a space, which GNU Make cannot represent
in a target or prerequisite name. Use a relative BUILD_DIR under the repository
root (the default `build/<game>`), or set NK_BUILD_ROOT to a folder without spaces
so tools/title_codegen_plan.py can pick a Make-safe build root.
```

The default relative `BUILD_DIR` carries no space, so an ordinary checkout is unaffected. Every route
that accepts an operator-chosen output directory already resolves a Make-safe root through
`NK_BUILD_ROOT` or the 8.3 short name before it reaches Make, or stops with `PACKAGE_UNSUPPORTED_PATH`.

## 5. Optional developer quality tools

The repository includes shared pre-commit/pre-push checks for text/structured-file hygiene,
baseline Ruff correctness, large files, secret detection, and the publication audit:

```powershell
python -m pip install pre-commit
python -m pre_commit install
python -m pre_commit run --all-files
```

`install` sets up both the commit and the push hook (`default_install_hook_types` in
`.pre-commit-config.yaml`). The publication audit runs at both stages on purpose: the push stage
audits the index, not each pushed commit, and a pushed branch is public, so only the commit stage
checks every commit before it can leave the machine.

Invoked as `python -m pre_commit` rather than the bare `pre-commit` console script: pip
installs that script into a user `Scripts/` directory that is frequently absent from `PATH` on
Windows, so the bare form fails with "command not found" immediately after a successful install.
The module form works regardless of `PATH`.

These hooks install their own pinned Ruff and Betterleaks environments. The
Betterleaks hook runs at the commit stage in directory mode over the staged files, which works the
same on Windows and Linux; it is not a pre-push hook, because pre-commit passes no file names at
that stage and directory mode would then scan the whole working tree, including the private input
directories (the pushed history is scanned in hosted CI); its upstream git mode sets `GIT_CONFIG_GLOBAL=NUL`,
which Git for Windows rejects, so that mode scanned nothing on Windows. C formatting is defined by
`.clang-format` but is not currently an automatic pre-commit hook. Mypy 2.4.1 is a shared check for
the issue #1391 import/VFPU scope. Before this work, the full `tools/` scan used
`python -m mypy --no-incremental --no-error-summary --no-pretty --python-version 3.15 --platform linux --explicit-package-bases tools`
and reported 353 errors in 120 modules. The full-tree per-module ratchet remains in
`tools/mypy_baseline.json`; its entries outside the clean scope are existing typing debt, not
suppressed errors. The current full-tree residual is 133 errors in 48 modules; their per-module
counts are listed in `tools/mypy_baseline.json` and remain guarded by that ratchet. The clean scope
directly checks `tools/verify_vfpu_provenance.py`,
`tools/hle_manifest.py`, `tools/psp_import_table.py`, `tools/import_audit_gate.py`,
`tools/vfpu_fuzz_gen.py`, `tools/test_import_audit.py`, `tools/test_hle_manifest.py`,
`tools/test_enhancement_package.py` and `tools/mypy_baseline.py`; mypy also checks their imports.
The historical `tools/test_pgd_malformed.py` path is not present in this checkout. Run the focused
shared check with `python tools/mypy_baseline.py --scope issue-1391`. The local hook installs
its own `mypy==2.4.1` into pre-commit's environment, so no separate install is needed; for the direct command above,
run `python -m pip install mypy==2.4.1` (the helper rejects any other version). These tools are not
core runtime dependencies.

### Optional LLVM backend

The opt-in LLVM IR backend (`CODEGEN_BACKEND=llvm`, [`LLVM_BACKEND.md`](LLVM_BACKEND.md))
compiles generated LLVM IR with Clang and links it with `llvm-link`; its tests also run `opt`.
Install them from MSYS2 UCRT64 once:

```powershell
pacman -S --needed mingw-w64-ucrt-x86_64-clang mingw-w64-ucrt-x86_64-llvm-tools
```

The default C backend needs none of this. `LLVM_CLANG` and `LLVM_LINK` name other copies.

### PSP analysis tools (optional, never runtime dependencies)

- [PSPSDK](https://github.com/pspdev/pspsdk) is the preferred open source reference for PSP
  headers, NID names, documented error constants, PRX structure, and PBP utilities. Keep a local
  checkout outside the repository or fetch a pinned revision in a reproducible tooling step.
- [PSPLink USB](https://github.com/pspdev/psplinkusb) is useful only when a development-capable
  physical PSP is available to collect independent behavior traces. It is not required to build
  or run the recompiler.
- [Ghidra](https://github.com/NationalSecurityAgency/ghidra) can independently inspect MIPS
  control flow and shared entries when the Python analyzer is ambiguous. Export only scripts,
  address notes, or other redistributable metadata—never a project database containing game
  bytes.

Do not add any of these large checkouts to this repository, and do not make the player
download them. They are development/oracle aids; the release path should remain the recompiler,
its redistributable host dependencies, and user-supplied game input.

### Optional local symbol reference (never track it)

`tools/nk_manager.ps1 -Action FindSymbol` can search an optional local `functions.csv` when a
contributor keeps that reference data locally. This is an optional reverse-engineering aid; it is
not required to build or run Nakagawa Recomp. The manager checks these locations in order:

1. `docs/symbol_ref/functions.csv`
2. `functions.csv`

Both locations are ignored by the repository. **Keep the CSV, any decompiler export, annotations, or game-derived material untracked.** Place or link an
authorized local `functions.csv` at either supported path: the first is preferable when only the
CSV is needed, the second supports a root drop point. Verify that Git
excludes the selected path before using it:

```bash
git check-ignore -v docs/symbol_ref/functions.csv
# or
git check-ignore -v functions.csv
```

Then run a lookup from the repository root:

```powershell
.\tools\nk_manager.ps1 -Action FindSymbol -FindName Camera_Update
.\tools\nk_manager.ps1 -Action FindSymbol -FindName 47054
```

The command performs a text search and prints at most 20 matching CSV rows. It does not download,
generate, or validate the reference data.

Use only reference material that you are authorized to possess. Do not copy a third-party
repository, raw decompiler output, proprietary game bytes, private symbols, or local-path-bearing
exports into Git history merely to enable the lookup command. Facts learned from a local symbol
reference may be documented when they are independently supportable and do not reproduce protected
implementation or private game data; keep the local CSV itself and raw reverse-engineering exports
outside the published repository.

## Troubleshooting

- **Preflight diagnostics:** run `.\nk.ps1 Doctor -TitleManifest C:\path\to\manifest.json -GameName game` (or `python tools/nk_doctor.py --title-manifest C:\path\to\manifest.json --game-name game`) to validate the toolchain, build dependencies, and the selected title's local inputs. Without a title selection, Doctor uses the public synthetic manifest.
- **Missing Vulkan headers:** pass the correct `-VulkanSdk` path or `VULKAN_SDK=...` Make variable.
- **`SDL3.dll` missing:** for the release package, keep `bin/SDL3.dll` beside `bin/nakagawa_player.exe`; for a source build, install the MSYS2 UCRT64 SDL3 package so `tools/copy_build_assets.ps1` stages it beside `build/nakagawa_player.exe`.
- **The player says its launcher cannot start (exit code 4):** the launcher needs `SDL3_ttf.dll` and its dependency closure beside `nakagawa_player.exe` (the player never searches for it by name, so a copy only on `PATH` is not used), and its UI fonts in `assets/fonts/ui`. Install the MSYS2 UCRT64 `sdl3-ttf` package and re-run `mingw32-make player`, which stages the whole closure, the fonts and their licence notices; the workspace doctor reports `RUNTIME_SDL3_TTF`/`RUNTIME_SDL3_TTF_CLOSURE`, and the player names the missing part in its message.
- **`PUBLIC_SAFE=1` active:** the runtime always compiles with `PUBLIC_SAFE=1`; the private-backend mode was retired on 2026-10-25. This mode links the public replacements — `iso_public.c` for ISO9660 lookups driven by `PSP_ISO`, `pgf_public.c` for fonts, and the SDL3 audio backend — plus `pgd_unavailable.c`. Disc routes keep working; PGD-protected data is refused in this mode, and the runtime refuses encrypted `~PSP` executables; the player decrypts them before a build (see [Encrypted executables](#encrypted-executables)).
- **Missing ISO or missing extracted assets:** `place_game_here/ISO/<game>.iso` must be present, and `place_game_here/EXTRACTED/PSP_GAME/USRDIR/xbdata_extracted` (or configured `SR_DATAROOT`) must be populated. `SR_DATAROOT` may instead hold the read-only `<archive>.xb` archives; the runtime mounts those directly ([#298](https://github.com/Jstar269/nakagawa-recomp/issues/298)).
- **No late PRX exports / asset lookups fail:** restore the required `place_game_here/EXTRACTED/` layout (decrypted `libfont.prx`, `scePsmf_library.prx`, `scePsmfP_library.prx`).
- **PSP font missing or text not rendering:** Run `python tools/nk_cli.py fonts status` to see each slot's source, and `python tools/nk_cli.py fonts import <folder>` for any slot that is missing. Your imported fonts are in `<user data>/fonts/v2/` with a `manifest.json`. See [System fonts](#system-fonts).
- **Clean build omits chunks:** use the unchanged two-process `all` target; do not rewrite it as `all: pipeline compile`.
- **Watchdog fires:** `SR_WATCHDOG_EXIT` counts vblanks since the last newly
  presented frame, not seconds or frame count. The no-frame watchdog is a
  NO-NEW-FLIP observation, not a hang verdict: a legitimately static scene
  (e.g. a save-confirmation modal waiting for input) also stops presenting.
  Classify a firing with the facts the watchdog prints -- `WATCHDOG_DISPLAY`
  outcome counters, the thread wait-state dump, and the `WATCHDOG_MPEG` /
  `WATCHDOG_PSMF` activity -- rather than from the threshold alone. If a title
  waits for a profile/save or cannot open a title-specific cache, inspect the
  local diagnostic log. Do not copy that input or its path into Git.

### Local problem report (`nk_cli.py report`)

`python tools/nk_cli.py report --title <DISC_ID>` writes one ZIP you can attach to an issue yourself. It
is made on your PC and **nothing is uploaded**: the code uses no network library, and a test fails if it
opens a connection. The ZIP goes to `<user data>\reports\<UTC time>-<title>.zip`; `--preview` lists the
files without writing it, `--json` prints the list for a tool, and `--no-doctor` leaves out the toolchain
check (the one slow step).

| File in the ZIP | What it is |
| --- | --- |
| `about.txt` | Nakagawa version and source commit, tools version, OS, CPU, GPU and driver, RAM, title id and region, package state, and every file here with its size |
| `logs/game-<DISC_ID>-last.txt` and `-prev.txt` | The title's last two runtime logs (without `--title`, the two newest of any title) |
| `logs/developer-log.txt`, `logs/player-output.txt` | The launcher's logs |
| `settings/*.json` | The `.json` files directly in the player's settings folder; a value under a key named like a token, secret or password is replaced |
| `flight-recorder-summary.json` | The newest `flight-recorder*.json`'s scalar fields and the size of each list; never its events or memory |
| `nk_doctor.txt` | `tools/nk_doctor.py --json` |

Only those files are ever opened, so nothing is "everything except a few exclusions". Each is also
refused by rule when it is a game file or disc image, a decrypted module, a save, a frame or memory dump,
a trace, a key or a console-bound value, or when it is not plain text, and long logs keep their last 2 MiB
from a whole line. Every line is redacted: your home folder (backslash or slash, the doubled slashes of
JSON, the `\\?\` and `file:///` prefixes, the `/mnt/c` mount, and the Windows, Linux and macOS profile
folders, found both by the account this PC names and by their shape) becomes `~`, your account name becomes `<user>` and this PC's name `<host>`. Read the ZIP before you
attach it.

In the launcher, **Settings > Problem report** does the same: it lists every file with its size, **Create the
report** writes the ZIP, and **Open folder** shows it. The launcher runs the command above with your
Python, so without a Python it shows the command to run yourself instead of failing quietly. Creating
the report runs the toolchain check, which can take a minute.

### Windows host pitfalls

| Symptom | Cause | Fix |
| --- | --- | --- |
| Make stops with `<target> needs a POSIX sh, and mingw32-make found no sh.exe on PATH` | `mingw32-make` found no POSIX `sh`, so it would run that recipe with `cmd.exe`, which misreads sh syntax; the Makefile refuses before any line of the recipe runs | Put Git for Windows' `usr\bin` ahead of `C:\msys64\usr\bin` on `PATH` (see [PATH order](#path-order)) |
| A Make recipe fails with a shell error, or runs words such as `status` as commands | `mingw32-make` found no POSIX `sh` and fell back to `cmd.exe` | Put Git for Windows' `usr\bin` ahead of `C:\msys64\usr\bin` on `PATH` (see [PATH order](#path-order)) |
| `cannot find -lvulkan` with `VULKAN_SDK=/ucrt64` | That is the CI's MSYS2 layout; a local MSYS2 may lack the import library | Leave `VULKAN_SDK` unset so the Makefile finds the installed Vulkan SDK |
| A Make-driven step fails on a `C:\path\to\dir` path | Backslashes in a Make variable are read as escapes | Pass Make paths with forward slashes (`C:/path/to/dir`) |
| The player refuses to build a package | Package outputs must live outside the repository | Point `BUILD_ROOT` and the user-data root outside the checkout |
| `git show origin/main:<path>` fails in Git Bash with a mangled `origin\main;...` argument | MSYS converts the argument as a path | Prefix the command with `MSYS_NO_PATHCONV=1` |
| `bash -lc` under MSYS2 runs in the home directory | A login shell resets the working directory | `cd` to the checkout inside the command |
| Files under WSL `/tmp` vanish between commands | WSL shuts its VM down when idle and clears `/tmp` | Keep WSL scratch work under `$HOME` |

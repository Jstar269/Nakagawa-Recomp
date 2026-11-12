# Nakagawa Recomp v0.0.2 Release Notes

Status: CURRENT. These are the notes of the v0.0.2 release, **v0.0.2 - Return Ace!**; its release page names the
exact commit.
Release scope: the [v0.0.2 milestone](https://github.com/Jstar269/nakagawa-recomp/milestone/2).
Audience: players and contributors updating from v0.0.1 or trying Nakagawa for the first time.

## What v0.0.2 is

Nakagawa Recomp is an experimental static recompiler. It takes the program code of a PlayStation Portable (PSP) game
you own, translates it into C, links it with a native C runtime, and runs the result as a Windows program through
SDL3 and Vulkan. The player decrypts the game's program files from your own disc image, on your PC, the way the
PPSSPP emulator does.

v0.0.2 is the second public build. It adds two play modes for every game, Authentic and Enhanced, a launcher that handles large libraries,
save backups, compressed disc images and portable mode, and it runs Hot Shots Tennis with no title-specific
compatibility patches. Most of the work is in accuracy: the kernel, file, save-data, graphics and sound behaviour
games rely on now follows measurements from a real PSP in many more places. As before, when a game needs something
Nakagawa doesn't support, it stops and says what is missing.

## What's new

### Play modes: Authentic and Enhanced

- **Two play modes per game** ([#2055](https://github.com/Jstar269/nakagawa-recomp/issues/2055), [#2074](https://github.com/Jstar269/nakagawa-recomp/pull/2074)). **Authentic** plays the game as released and is the default.
  **Enhanced** adds host-side improvements that work for any game. A third mode, Ace, exists only for a game that has
  an enhancement package, and no game in this release has one, so no game offers Ace. Settings has a library-wide play
  mode, and each game's page can choose its own. A badge on a game's card names the mode while Enhanced is in
  effect.
- **Enhanced parts** (Settings > Enhanced, or **ENHANCED OPTIONS** on a game's page). Each one can be turned off for
  the whole library or for a single game. In Authentic none of them runs.
  - **Texture filtering**, 2x to 16x, which keeps slanted surfaces sharp; **Crisp system text**, which draws the
    on-screen keyboard at your screen's resolution; a **Speed readout**; and a **Game badge** that names the mode for
    a few seconds when a game starts ([#1797](https://github.com/Jstar269/nakagawa-recomp/pull/1797)).
  - **Smooth motion** makes the frames in between from the two latest real frames when a game holds a steady 30
    frames per second. A game that does not hold 30 is shown as it is. The 54.8 measured on Hot Shots Tennis counts
    pictures presented a second; during a rally its in-betweens mostly repeat the earlier frame, so the motion itself
    stays near 30 ([Smooth motion](ENHANCEMENT_CONTRACT.md#106-smooth-motion) says what it follows and how the
    readout counts it). The game itself is untouched. It adds about 17 ms of delay ([#1939](https://github.com/Jstar269/nakagawa-recomp/pull/1939)).
  - **Widescreen** shows more of the 3D scene at the sides and keeps the HUD in shape. Some objects appear late at
    the edges, and Smooth motion turns it off ([#2067](https://github.com/Jstar269/nakagawa-recomp/pull/2067), [#2174](https://github.com/Jstar269/nakagawa-recomp/pull/2174)).
  - **Sharper enlarging** enlarges the picture with AMD FidelityFX Super Resolution 1 ([#2082](https://github.com/Jstar269/nakagawa-recomp/pull/2082), [#2175](https://github.com/Jstar269/nakagawa-recomp/pull/2175)). It does not enlarge movie frames the game draws through the GPU ([#2111](https://github.com/Jstar269/nakagawa-recomp/issues/2111)).
  - **Clean pitched sound** resamples pitched sound effects without aliasing. It changes the samples the game
    receives, so it is off by default ([#2078](https://github.com/Jstar269/nakagawa-recomp/pull/2078)).
- **Graphics presets** (Low, Medium, High, Ultra and Handheld) set the resolution, scaling, VSync and fullscreen
  rows (and, for Handheld, game fullscreen) in one choice for Enhanced sessions. Authentic sessions always use the rows as you set them
  ([#2180](https://github.com/Jstar269/nakagawa-recomp/pull/2180)).

### Player and launcher

- **Large libraries.** The library loads, pages and stays fast with thousands of games ([#1647](https://github.com/Jstar269/nakagawa-recomp/pull/1647)). Find and sort
  work in the library ([#1740](https://github.com/Jstar269/nakagawa-recomp/pull/1740), [#1743](https://github.com/Jstar269/nakagawa-recomp/pull/1743), [#1771](https://github.com/Jstar269/nakagawa-recomp/pull/1771)).
- **Compressed disc images.** Besides a plain `.iso`, the player takes a `.cso`, `.zso` or `.chd` image. It expands
  the image once, when you add it, into a plain `.iso` in its data folder ([#1634](https://github.com/Jstar269/nakagawa-recomp/pull/1634)).
- **Disc checks.** The player hashes each disc it adds. If you import a Redump PSP datafile
  (`python tools/nk_cli.py redump-import <file>`), a disc that matches is marked Verified retail. One that does not
  is marked Modified when a verified copy with the same disc ID is in your library, and Unknown otherwise. A fan
  translation no longer replaces the record of the retail disc it shares a disc ID with ([#1789](https://github.com/Jstar269/nakagawa-recomp/pull/1789)).
- **Homebrew.** An unencrypted homebrew `EBOOT.PBP` can be added the way a PSN package is. Kernel-mode homebrew,
  encrypted executables and packages that hold no program are refused by name ([#2170](https://github.com/Jstar269/nakagawa-recomp/pull/2170)).
- **Check for updates.** Settings > **CHECK FOR UPDATES** asks GitHub for this project's releases, only when you
  select it, or once at start if you turn on **Check at start**, which is off by default ([#1628](https://github.com/Jstar269/nakagawa-recomp/pull/1628)). When a newer
  version exists, this build shows it and links to its release page.
- **Saves.** A game's page can back up its saves, restore a backup, and add saves. Removing a game
  with its saves backs them up first ([#2098](https://github.com/Jstar269/nakagawa-recomp/pull/2098)).
- **Removing a game.** **Remove from library** can also delete the disc image Nakagawa converted or expanded and
  the decrypted files, when you tick them; it shows their size first ([#2035](https://github.com/Jstar269/nakagawa-recomp/pull/2035)). Logs are in one folder.
- **Portable mode.** A file named `portable.txt` beside `nakagawa_player.exe` keeps all user data in a `data`
  folder beside the program ([#1872](https://github.com/Jstar269/nakagawa-recomp/pull/1872)). `--user-data-root` keeps settings, cache, logs and saves together in the
  folder you give it ([#1648](https://github.com/Jstar269/nakagawa-recomp/pull/1648)).
- **Problem report.** Settings > **Problem report** lists the files a report would hold and writes them to one ZIP
  on your PC, for you to attach to an issue yourself. Nothing is uploaded ([#1884](https://github.com/Jstar269/nakagawa-recomp/pull/1884), [#1894](https://github.com/Jstar269/nakagawa-recomp/pull/1894)).
- **Controls.** Keyboard keys and controller buttons can be rebound in the launcher ([#1299](https://github.com/Jstar269/nakagawa-recomp/issues/1299), [#1779](https://github.com/Jstar269/nakagawa-recomp/pull/1779)), and the
  Controls page shows a button glyph beside each binding ([#1548](https://github.com/Jstar269/nakagawa-recomp/pull/1548)).
- **Pause menu.** `Esc`, `F1` or the controller's Guide button opens a menu over the paused game: Resume, Settings,
  Controls and Quit to library ([#1323](https://github.com/Jstar269/nakagawa-recomp/pull/1323), [#1779](https://github.com/Jstar269/nakagawa-recomp/pull/1779)). Its Settings page also sets fullscreen and frame pacing
  ([#2026](https://github.com/Jstar269/nakagawa-recomp/pull/2026)).
- **Accessibility.** Settings > Accessibility sets the interface size, the text size, high contrast and reduced
  motion; Automatic follows the matching Windows settings ([#1739](https://github.com/Jstar269/nakagawa-recomp/issues/1739)). Under reduced motion the launcher and the
  in-game menu cut their transitions ([#1773](https://github.com/Jstar269/nakagawa-recomp/pull/1773)).
- **Display.** Settings warns when your display's refresh rate is not a whole multiple of 60 Hz, which makes
  motion judder slightly ([#1957](https://github.com/Jstar269/nakagawa-recomp/pull/1957)).
- **Windows.** The launcher and the games it starts share one taskbar button, and starting the player a second
  time on the same data folder opens no second launcher ([#2036](https://github.com/Jstar269/nakagawa-recomp/pull/2036)). Task
  Manager names the program "Nakagawa Recomp" ([#2008](https://github.com/Jstar269/nakagawa-recomp/pull/2008)).
- **Clearer messages.** Your user folder no longer appears on screen ([#1630](https://github.com/Jstar269/nakagawa-recomp/pull/1630)). Error cards name the actual
  problem for Windows blocks, long data folders and compressed discs ([#1596](https://github.com/Jstar269/nakagawa-recomp/pull/1596)). Buttons act on release, file
  pickers remember where you went, and the game opens on the launcher's display ([#1639](https://github.com/Jstar269/nakagawa-recomp/pull/1639)).
- **Library safety.** Two Nakagawa windows can no longer overwrite each other's library changes ([#1768](https://github.com/Jstar269/nakagawa-recomp/issues/1768),
  [#1772](https://github.com/Jstar269/nakagawa-recomp/pull/1772)).
- **Quitting.** Closing a game shuts it down through the scheduler, so the game's own exit callback runs on every
  way of quitting ([#1377](https://github.com/Jstar269/nakagawa-recomp/issues/1377), [#1392](https://github.com/Jstar269/nakagawa-recomp/issues/1392), [#1783](https://github.com/Jstar269/nakagawa-recomp/pull/1783)).

### Hot Shots Tennis

- **No title-specific compatibility patches.** Hot Shots Tennis (UCUS98701) is still rated **Playable**, and
  `python tools/compat_overrides.py --debt-census` now lists no active patch for it; v0.0.1 had 11. General fixes
  replaced the display, sync and frame-latch assists, the scheduler stand-ins, the archive-reader patch and the
  hand-written stubs ([#1516](https://github.com/Jstar269/nakagawa-recomp/pull/1516), [#1511](https://github.com/Jstar269/nakagawa-recomp/pull/1511), [#1580](https://github.com/Jstar269/nakagawa-recomp/pull/1580), [#1903](https://github.com/Jstar269/nakagawa-recomp/pull/1903), [#2041](https://github.com/Jstar269/nakagawa-recomp/pull/2041), [#2093](https://github.com/Jstar269/nakagawa-recomp/pull/2093)).
- **Movies play.** Movie frames that pass through a VRAM mirror are drawn, so the opening movie in Cinema no longer shows
  black ([#1049](https://github.com/Jstar269/nakagawa-recomp/issues/1049), [#1602](https://github.com/Jstar269/nakagawa-recomp/pull/1602)).
- **Speed.** On the reference PC named below, with builds of main from 2026-10-27, a full match ran at about 29.5 of
  the game's own 30 frames per second and the menus at 29.2 to 29.6 ([#1296](https://github.com/Jstar269/nakagawa-recomp/issues/1296)).
- **End of a match.** A stop at the end of a match is fixed: a freed heap block keeps its bytes until it is handed
  out again, as on a PSP ([#2038](https://github.com/Jstar269/nakagawa-recomp/pull/2038)).

### Compatibility and accuracy

- **Starting a program as the PSP does.** An executable starts at its `module_start` export, the last one when it
  lists two; the thread that ran `module_start` is deleted when it returns; and `sceKernelQueryModuleInfo` sees the
  main module ([#1680](https://github.com/Jstar269/nakagawa-recomp/pull/1680), [#1693](https://github.com/Jstar269/nakagawa-recomp/pull/1693), [#1702](https://github.com/Jstar269/nakagawa-recomp/pull/1702), [#1708](https://github.com/Jstar269/nakagawa-recomp/pull/1708), [#1984](https://github.com/Jstar269/nakagawa-recomp/pull/1984)).
- **Code a game loads later.** Games that split their code across overlay modules, including ELF type `0xFFA8`
  overlays and CodeWarrior code overlays, are translated and bound by their code ([#1581](https://github.com/Jstar269/nakagawa-recomp/pull/1581), [#1937](https://github.com/Jstar269/nakagawa-recomp/pull/1937), [#1938](https://github.com/Jstar269/nakagawa-recomp/pull/1938)).
- **Calls and jumps.** A guest return past its caller finds the enclosing call, and a code pointer of 0 is refused
  as a null pointer in every tier instead of running ([#1660](https://github.com/Jstar269/nakagawa-recomp/pull/1660), [#1700](https://github.com/Jstar269/nakagawa-recomp/pull/1700), [#1809](https://github.com/Jstar269/nakagawa-recomp/pull/1809)).
- **Interpreter.** The fail-closed interpreter runs the remaining Allegrex integer and COP1 forms, so fewer games
  stop on a rejected instruction ([#1705](https://github.com/Jstar269/nakagawa-recomp/pull/1705), [#1709](https://github.com/Jstar269/nakagawa-recomp/pull/1709)). An interpreted `longjmp` stops or unwinds instead of looping
  ([#2016](https://github.com/Jstar269/nakagawa-recomp/issues/2016), [#2021](https://github.com/Jstar269/nakagawa-recomp/pull/2021)).
- **Files and discs.** Seeks follow the PSP before the start and past the end of a file, on UMD and on the Memory
  Stick ([#1735](https://github.com/Jstar269/nakagawa-recomp/pull/1735), [#1867](https://github.com/Jstar269/nakagawa-recomp/pull/1867), [#2042](https://github.com/Jstar269/nakagawa-recomp/pull/2042)). Asynchronous reads and seeks work on the raw UMD device ([#1687](https://github.com/Jstar269/nakagawa-recomp/pull/1687)). A
  write longer than 4096 bytes writes every byte ([#1906](https://github.com/Jstar269/nakagawa-recomp/pull/1906)), `PSP_O_TRUNC` follows the PSP ([#1907](https://github.com/Jstar269/nakagawa-recomp/pull/1907)),
  `sceIoAssign` and `sceIoUnassign` are implemented ([#2075](https://github.com/Jstar269/nakagawa-recomp/issues/2075), [#2076](https://github.com/Jstar269/nakagawa-recomp/pull/2076)), and `sceKernelLoadModule` opens its file
  through the device first ([#1934](https://github.com/Jstar269/nakagawa-recomp/pull/1934)).
- **Save data.** A save is written all or nothing; save list selection works and listings are deterministic; an encrypted
  save is refused by name ([#1387](https://github.com/Jstar269/nakagawa-recomp/issues/1387), [#1790](https://github.com/Jstar269/nakagawa-recomp/pull/1790)). The game is told the real free space and the shortfall, and
  `GETSIZE` on a missing save answers as a PSP does ([#1638](https://github.com/Jstar269/nakagawa-recomp/pull/1638), [#1898](https://github.com/Jstar269/nakagawa-recomp/pull/1898)). Older parameter versions and the status
  after shutdown follow the PSP ([#1662](https://github.com/Jstar269/nakagawa-recomp/pull/1662)).
- **Utility dialogs.** Parameter sizes are checked for the on-screen keyboard, network configuration, game-data install and game-sharing
  dialogs, and a dialog's status follows its completion
  ([#1610](https://github.com/Jstar269/nakagawa-recomp/pull/1610), [#1950](https://github.com/Jstar269/nakagawa-recomp/pull/1950), [#1959](https://github.com/Jstar269/nakagawa-recomp/pull/1959)).
- **Fonts.** A game that loads `libfont` from its own disc runs it as guest code ([#1645](https://github.com/Jstar269/nakagawa-recomp/pull/1645)). The project's open
  stand-in fonts cover the PSP's Latin set with one face per Latin style, Japanese CP932 with both JIS kanji levels,
  and Japanese and Korean at the PSP's 10.125 points ([#2088](https://github.com/Jstar269/nakagawa-recomp/pull/2088), [#2177](https://github.com/Jstar269/nakagawa-recomp/pull/2177)).
- **Kernel and HLE.** Kernel behaviour from PSP evidence for exclusive file creation, seek destinations, `sceFont`
  spans, controller sampling, the volatile memory lock, VBLANK sub-interrupts and the system profile ([#1784](https://github.com/Jstar269/nakagawa-recomp/pull/1784)); per-number VBLANK handlers
  and the FINISH id to finish callbacks ([#1637](https://github.com/Jstar269/nakagawa-recomp/pull/1637)); message-pipe waiters ([#1586](https://github.com/Jstar269/nakagawa-recomp/pull/1586)); memory pools of any size
  ([#1621](https://github.com/Jstar269/nakagawa-recomp/pull/1621)); DMA transfers across threads and their measured time ([#1492](https://github.com/Jstar269/nakagawa-recomp/pull/1492), [#1652](https://github.com/Jstar269/nakagawa-recomp/pull/1652)); memory blocks placed High
  with measured error codes ([#1694](https://github.com/Jstar269/nakagawa-recomp/pull/1694)); host resources cleaned up when a game exits ([#1609](https://github.com/Jstar269/nakagawa-recomp/pull/1609)). More imports are
  implemented ([#1686](https://github.com/Jstar269/nakagawa-recomp/pull/1686), [#1712](https://github.com/Jstar269/nakagawa-recomp/pull/1712), [#1908](https://github.com/Jstar269/nakagawa-recomp/pull/1908)), and many handlers now carry a status backed by recorded PSP tests
  ([#1658](https://github.com/Jstar269/nakagawa-recomp/pull/1658), [#1865](https://github.com/Jstar269/nakagawa-recomp/pull/1865), [#1895](https://github.com/Jstar269/nakagawa-recomp/pull/1895), [#1902](https://github.com/Jstar269/nakagawa-recomp/pull/1902), [#1961](https://github.com/Jstar269/nakagawa-recomp/pull/1961), [#2195](https://github.com/Jstar269/nakagawa-recomp/pull/2195)).
- **Scheduling.** A higher-priority thread whose wait times out preempts at the next yield point ([#1629](https://github.com/Jstar269/nakagawa-recomp/pull/1629)); a
  blocking pad read waits on the VBLANK source ([#1921](https://github.com/Jstar269/nakagawa-recomp/pull/1921)); a long display list gives way to a stronger thread that
  comes due during it ([#1924](https://github.com/Jstar269/nakagawa-recomp/pull/1924)); interrupt and timer behaviour is modelled from PSP measurements ([#1661](https://github.com/Jstar269/nakagawa-recomp/pull/1661)).
- **VFPU.** `vh2f`, `vuc2i`, `vsocp`, `vwbn`, `vrndf1`, `vrndf2`, `vfad`, `vavg` and `vsrt1` to `vsrt4`,
  implemented from PSP measurements ([#1605](https://github.com/Jstar269/nakagawa-recomp/pull/1605), [#1656](https://github.com/Jstar269/nakagawa-recomp/pull/1656), [#1922](https://github.com/Jstar269/nakagawa-recomp/pull/1922)), and an aliasing and edge-value sweep with a
  per-form PSP result ([#1964](https://github.com/Jstar269/nakagawa-recomp/pull/1964)).

### Graphics and sound

- **Graphics.** Textures are sampled at pixel centres as a PSP samples them ([#1631](https://github.com/Jstar269/nakagawa-recomp/pull/1631)). Mip levels, including
  automatic and slope levels, are drawn on the Vulkan renderer ([#1518](https://github.com/Jstar269/nakagawa-recomp/pull/1518), [#1819](https://github.com/Jstar269/nakagawa-recomp/pull/1819)). A real-PSP raster test set
  found and fixed software-renderer errors ([#1670](https://github.com/Jstar269/nakagawa-recomp/pull/1670)). Immediate-mode drawing works ([#1757](https://github.com/Jstar269/nakagawa-recomp/pull/1757)), and `SIGNAL`
  jumps, calls and returns move the display list ([#2095](https://github.com/Jstar269/nakagawa-recomp/pull/2095)). The display list queue follows the PSP's driver, and
  blocking syncs wait for a stalled list ([#1374](https://github.com/Jstar269/nakagawa-recomp/issues/1374), [#1787](https://github.com/Jstar269/nakagawa-recomp/pull/1787)).
- **GPU renderer.** CPU writes to video memory are held until the GPU next uses the target ([#1710](https://github.com/Jstar269/nakagawa-recomp/pull/1710)); stencil
  states are drawn on the GPU instead of falling back ([#1674](https://github.com/Jstar269/nakagawa-recomp/pull/1674), [#1940](https://github.com/Jstar269/nakagawa-recomp/pull/1940)), and newer depth is written back
  when a target is evicted ([#2097](https://github.com/Jstar269/nakagawa-recomp/pull/2097)); the Vulkan pipeline cache is kept on disk, so a later run doesn't compile the
  same pipelines again ([#2189](https://github.com/Jstar269/nakagawa-recomp/pull/2189)).
- **Speed.** At higher resolutions only the area that changed is copied ([#1641](https://github.com/Jstar269/nakagawa-recomp/pull/1641)); colour targets are drawn in
  place, about 4x faster per pixel ([#1623](https://github.com/Jstar269/nakagawa-recomp/pull/1623)); texture scans are skipped for memory no texture reads ([#1620](https://github.com/Jstar269/nakagawa-recomp/pull/1620)).
- **Sound.** SAS envelopes follow the game's rates and curves ([#2059](https://github.com/Jstar269/nakagawa-recomp/pull/2059)); SAS voice volumes accept signed values, voices start at full volume and mix gain follows PPSSPP's; at the Hot Shots
  Tennis title logo the level now lands within about 0.5 dB of a PSP-3000 capture, though not sample for sample ([#2158](https://github.com/Jstar269/nakagawa-recomp/pull/2158)); re-initialising SAS starts it fresh ([#1619](https://github.com/Jstar269/nakagawa-recomp/pull/1619)); ATRAC3 decode timing follows a PSP
  measurement ([#1600](https://github.com/Jstar269/nakagawa-recomp/pull/1600)); and output volumes up to `0xFFFFF` are accepted and clamped by the mixer, a limit the project chose and did not measure on a PSP ([#1993](https://github.com/Jstar269/nakagawa-recomp/pull/1993)).
- **Movies.** A title's own PSMF player keeps time, waits for the display, reloads and plays Vaudio
  ([#1930](https://github.com/Jstar269/nakagawa-recomp/pull/1930)); the YCbCr decoder and the stream ends follow the PSP more closely ([#2020](https://github.com/Jstar269/nakagawa-recomp/pull/2020), [#2039](https://github.com/Jstar269/nakagawa-recomp/pull/2039), [#2065](https://github.com/Jstar269/nakagawa-recomp/pull/2065)).

### Packaging and verification

- The packaging recipe is laid out so two runs from the same attested bundle, unsigned, give the same bytes, and
  `tools/package_repro_check.py` compares two packages and names every difference ([#1781](https://github.com/Jstar269/nakagawa-recomp/pull/1781)).
- The player and the title programs carry Windows version information ([#2008](https://github.com/Jstar269/nakagawa-recomp/pull/2008)).
- `BUILD PACKAGE` compiles generated code at one measured optimisation level in bounded compile units, so compiler memory stays
  bounded (the title whose compile reached 18.7 GB now stays under 500 MB) ([#1730](https://github.com/Jstar269/nakagawa-recomp/issues/1730), [#1786](https://github.com/Jstar269/nakagawa-recomp/pull/1786)).

### For developers

- Generated code also builds with Clang ([#1655](https://github.com/Jstar269/nakagawa-recomp/pull/1655)); there is also an opt-in LLVM IR backend ([#1663](https://github.com/Jstar269/nakagawa-recomp/pull/1663)).
- Opt-in guest-register promotion (`--promote-registers`, [#284](https://github.com/Jstar269/nakagawa-recomp/issues/284)) and an advisory analysis IR ([#305](https://github.com/Jstar269/nakagawa-recomp/issues/305)).
- Verification builds: one translated function run from a captured state against the reference interpreter,
  sampled in-play checking, and a sweep of every scalar instruction form ([#1852](https://github.com/Jstar269/nakagawa-recomp/pull/1852), [#1855](https://github.com/Jstar269/nakagawa-recomp/pull/1855), [#1860](https://github.com/Jstar269/nakagawa-recomp/pull/1860)).
- Real-PSP instruction-trace comparison ([#1653](https://github.com/Jstar269/nakagawa-recomp/pull/1653)), a one-command PSP probe matrix runner ([#1671](https://github.com/Jstar269/nakagawa-recomp/pull/1671)), a tool that
  finds the first draw where the Vulkan renderer departs from the software one ([#2169](https://github.com/Jstar269/nakagawa-recomp/pull/2169)), and scripted PPSSPP
  reference captures with a graded comparison ([#2176](https://github.com/Jstar269/nakagawa-recomp/pull/2176)).
- A deterministic guest clock for scripted runs ([#2089](https://github.com/Jstar269/nakagawa-recomp/pull/2089)), recording a run as video and audio on one guest
  timeline ([#2080](https://github.com/Jstar269/nakagawa-recomp/pull/2080)), the flight recorder's trigger modes, watch class and thread snapshot ([#1622](https://github.com/Jstar269/nakagawa-recomp/pull/1622), [#1664](https://github.com/Jstar269/nakagawa-recomp/pull/1664)),
  and one machine-readable failure-code list ([#1683](https://github.com/Jstar269/nakagawa-recomp/pull/1683)).
- `tools/library_build.py` builds a whole folder of discs into one player data folder ([#1649](https://github.com/Jstar269/nakagawa-recomp/pull/1649)).
- A code tour and plainer module overviews ([#1529](https://github.com/Jstar269/nakagawa-recomp/pull/1529)), the Makefile split by concern ([#1875](https://github.com/Jstar269/nakagawa-recomp/pull/1875)), and the HLE split
  into one file per subsystem ([#1807](https://github.com/Jstar269/nakagawa-recomp/pull/1807)).
- Contributing: a DCO sign-off is optional ([#1763](https://github.com/Jstar269/nakagawa-recomp/pull/1763)).

## Public verification you can reproduce

You can check these parts of v0.0.2 yourself, with no retail ISO, PRX, save, key or decrypted module:

- **The player:** `mingw32-make player` builds `build/nakagawa_player.exe` with SDL3.
- **Display smoke test:** `mingw32-make display-smoke` checks the two-phase pipeline, the loader, NID imports,
  VBLANK delivery, the display latch and the off-screen window check.
- **Production smoke test:** `mingw32-make production-smoke` makes a synthetic PSP PRX, recompiles it from MIPS to C
  to native code, then checks and runs the result.
- **Fail-closed dispatch:** `mingw32-make production-smoke-gap` leaves one synthetic function out of the translated
  code on purpose, and checks that the real dispatcher hands it to the fail-closed interpreter.
- **Differential cosimulation:** `mingw32-make cosim-selftest` runs synthetic cases both as translated code and in the
  interpreter, and compares their traces, writes, memory and CPU state.
- **Platform ladder:** `mingw32-make platform-ladder` exercises relocations, threads and scheduling, scalar floating
  point, and the filesystem over synthetic workloads.
- **Core runtime self-tests:** `selftest`, `sched-selftest`, `hle-thread-selftest` and `public-safe-verify`.
- **Showcase demos:** `mingw32-make showcase-smoke` runs both project-written demos without a window.

From a clean Windows checkout with the supported toolchain, the four headline commands are:

```powershell
mingw32-make --no-print-directory production-smoke
mingw32-make --no-print-directory production-smoke-gap
mingw32-make --no-print-directory player
mingw32-make --no-print-directory display-smoke
```

They create their synthetic inputs under the ignored `build/` folder.

## What v0.0.2 is not

- **No games included.** This repository contains no game programs, game assets, firmware modules, keys bound to a
  console or a user, or private test recordings. Its only key material is the public game-decryption tables PPSSPP
  also ships, in `src/core/nk_psp_builtin_keys.c`. Nakagawa never downloads games or game data. You supply your own
  disc image of a game you own, or the `EBOOT.PBP` of a PSN release of a UMD game you own.
- **Few network connections, all yours to choose.** The player connects only when you choose to: the build-tool
  download, which the ZIP doesn't need because the tools are in it, and Check for updates. Enhanced runs on
  your PC and makes no connection ([`SETUP.md`](SETUP.md#network-use)).
- **Not a finished emulator.** Hot Shots Tennis is the one retail game rated **Playable**;
  [`COMPATIBILITY.md`](COMPATIBILITY.md) lists how far each tested game gets. A valid PSP disc with no title file
  is imported as **Experimental**. PGD-protected content, PSP minis, games tied to the PSP that bought them and PSOne
  Classics are not supported.
- **No PSP fonts included.** The PSP's own fonts belong to Sony, so the project doesn't include or share them; you
  import them from your own PSP in **Settings > PSP FONTS**
  ([how to copy them](SETUP.md#copying-the-fonts-from-your-psp)). Without them a game shows the project's own open
  fonts, which look close to the PSP's but not the same.
- **Windows only for the download.** The release is one ZIP for Windows 11 x64. On Linux, the player and runtime
  build from source and use the system's own build tools ([#1644](https://github.com/Jstar269/nakagawa-recomp/pull/1644)); that is a development setup, not a release.
- **Sound needs an audio device.** With no audio device, a game runs silently after one message.

## Checking your download

v0.0.2 is not code-signed, so the first time you run `nakagawa_player.exe`, Windows SmartScreen may say
"Windows protected your PC", because the file has no publisher signature. That warning is about the missing
signature, not about anything found in the file. Don't turn SmartScreen or your antivirus off. Instead, before you
choose **More info** and then **Run anyway**, check that the download is the one this project built:

1. Before you extract it, in the folder where you saved the ZIP, compare its SHA-256 with
   `nakagawa-recomp-0.0.2-windows-x64.zip.sha256`, published next to it on the release page:

   ```powershell
   (Get-FileHash .\nakagawa-recomp-0.0.2-windows-x64.zip -Algorithm SHA256).Hash.ToLower()
   ```

2. After extracting, open PowerShell in the extracted folder (the one with `bin\` in it).
   `SHA256SUMS.txt` there lists the SHA-256 of every file in the package.
3. With the [GitHub CLI](https://cli.github.com/), check that each executable and DLL under `bin\` was
   built by this repository's release workflow. Each file carries a signed build attestation (SLSA Build
   Level 2). The GitHub CLI needs you to sign in once first (`gh auth login`). Set `$Commit` to the full commit
   the release page names (the commit the `v0.0.2` tag points at),
   then check one file:

   ```powershell
   $Commit = '<the 40-character commit from the release page>'
   gh attestation verify .\bin\nakagawa_player.exe --repo Jstar269/Nakagawa-Recomp `
       --signer-workflow Jstar269/Nakagawa-Recomp/.github/workflows/provenance-attestation.yml `
       --source-ref refs/heads/main --source-digest $Commit
   ```

   Or every file under `bin\` at once:

   ```powershell
   Get-ChildItem .\bin -Recurse -File | ForEach-Object {
       gh attestation verify $_.FullName --repo Jstar269/Nakagawa-Recomp `
           --signer-workflow Jstar269/Nakagawa-Recomp/.github/workflows/provenance-attestation.yml `
           --source-ref refs/heads/main --source-digest $Commit
   }
   ```

   The signer workflow and the commit together prove the file came from this release's build, not from
   some other workflow run in the repository.

If any check fails, don't run the file; report it as described under [How to report](#how-to-report).

### Source of the bundled build tools

The source of the build tools in `prereqs/` is published next to the ZIP as
`nakagawa-recomp-0.0.2-prerequisite-sources.zip`, with its own `.sha256`: the MSYS2 source package of each MSYS2 package, the CPython source release, and the source releases of
the libraries built into the CPython runtime (OpenSSL, libffi, SQLite and others). The Microsoft C runtime that comes
with CPython has no public source; its redistribution terms are in `prereqs/notices`. `prereqs/notices/SOURCES.txt`
in the ZIP names each file of the source archive with its SHA-256, and `prereqs/notices` holds every bundled
package's licence files. The bundled tools leave out their manuals (`share/doc`, `share/info`, `share/man`), which a
build never reads; the source archive has them.

## Requirements when running the packaged player

The package puts `bin/nakagawa_player.exe` next to the public source under `source/`, which includes
`tools/nk_cli.py` ([`PREVIEW_RELEASE.md`](PREVIEW_RELEASE.md) defines the layout). It includes the SDL3 and
SDL3_ttf DLLs and the DLLs they depend on, with generated third-party notices and the material you need to relink
them. The exact list of DLLs and their licences is in `THIRD_PARTY_NOTICES` and the SBOM. The Vulkan loader and a
Vulkan-capable graphics driver come from your own system.

- **Running the player:** start `nakagawa_player.exe` with the packaged `SDL3.dll` beside it and a Vulkan-capable
  GPU driver. The player's readable font loads from the packaged `SDL3_ttf.dll`, with no toolchain on `PATH`.
- **BUILD PACKAGE:** recompiling a game needs Python, GCC and GNU Make. The ZIP carries the pinned ones in
  `prereqs/`: CPython 3.15.2, and the MSYS2 UCRT64 GCC 16.2, GNU Make 4.4.1, SDL3 3.4.20 and their libraries
  ([#1285](https://github.com/Jstar269/nakagawa-recomp/issues/1285), [#2113](https://github.com/Jstar269/nakagawa-recomp/pull/2113)). Before each build the player checks them against `source/assets/prereq_manifest.json`, then
  uses them with no consent card and no download. If that folder is damaged, the player names the first missing or
  changed file (`PREREQUISITE_BUNDLE_INVALID`) instead of downloading a replacement: extract the ZIP again. Without
  `prereqs/`, as in a source checkout, the player offers one consent card for the same pinned tools.

**Hardware:** Windows 11 x64 with a Vulkan GPU, 8 GB RAM (16 GB recommended) and 5 GB of free disk space (10 GB on an
SSD recommended). Building a game is the heavy part. The measured numbers are in
[`SETUP.md`](SETUP.md#system-requirements).

**Reference PC:** the speeds in these notes were measured on Windows 11 with an AMD Ryzen 9 5900X and an NVIDIA
GeForce RTX 3080.

**Download size:** the build tools in the ZIP's `prereqs/` unpack to 7,305 files and 699 MB. Their source is a
separate 531 MB download next to the ZIP; see
[Source of the bundled build tools](#source-of-the-bundled-build-tools).

## Updating from v0.0.1

- Unpack v0.0.2 into a new folder. Your library, built games, saves and settings are in the per-user data folder, so
  v0.0.2 finds them, and the old folder can be deleted afterwards
  ([`SETUP.md`](SETUP.md#install-update-and-uninstall-windows)).
- Games built by v0.0.1 are marked as built by an older version of Nakagawa. They need building again to get this
  release's fixes, and the library offers **Rebuild all** ([#1720](https://github.com/Jstar269/nakagawa-recomp/pull/1720)).
- If you start the player with `--user-data-root`, v0.0.2 keeps settings, cache, logs and saves in that folder too
  ([#1648](https://github.com/Jstar269/nakagawa-recomp/pull/1648)). Settings and saves that v0.0.1 wrote with that flag are in the default folder and stay there.

## Known issues

- **Some sound effects drop out in Hot Shots Tennis while it loads** ([#1296](https://github.com/Jstar269/nakagawa-recomp/issues/1296)).
- **Hot Shots Tennis's title-logo sound effects match a PSP's level only approximately.** The level is within about
  0.5 dB of a PSP-3000 capture, but the waveform is not the same ([#2135](https://github.com/Jstar269/nakagawa-recomp/issues/2135)).
- **Hot Shots Tennis's in-game text uses the project's stand-in fonts unless you import your PSP's**, and the
  stand-ins don't look exactly like the PSP's. Text drawn from your own PSP's fonts has not been compared with a PSP
  ([#1300](https://github.com/Jstar269/nakagawa-recomp/issues/1300)).
- **The software renderer clips every draw to 480x272.** A PSP draws up to 512 pixels wide, so an effect a game
  draws into a wider off-screen image can come out with stale columns at the right when the software renderer is in
  use ([#2193](https://github.com/Jstar269/nakagawa-recomp/issues/2193)). The player doesn't say when drawing falls back to the software renderer ([#2151](https://github.com/Jstar269/nakagawa-recomp/issues/2151)).
- **Overlapping see-through layers can blend wrongly on the GPU.** When several overlapping translucent shapes are
  drawn in one batch, each blends with the picture as it was before the batch ([#2109](https://github.com/Jstar269/nakagawa-recomp/issues/2109)).
- **Effects a game samples as textures don't scale with the resolution setting.** Bloom, blur and similar effects a
  game draws into small off-screen images stay at the PSP's resolution, so they can look soft next to a sharp scene
  ([#2052](https://github.com/Jstar269/nakagawa-recomp/issues/2052)).
- **Movie timing.** The number of decoded movie pictures and the audio offset are not fully matched to a PSP
  ([#279](https://github.com/Jstar269/nakagawa-recomp/issues/279)).
- **Most other games stop early.** Each one names the missing piece; typical causes are unsupported system calls
  and interpreter rejections. A game that keeps drawing a black screen can be stopped with **STOP GAME**.
- **No online play.** `sceNetInit` and `sceNetTerm` succeed with no network ([#2106](https://github.com/Jstar269/nakagawa-recomp/pull/2106)); every other network call
  is refused, with no fake connections.
- **Not code-signed.** Windows SmartScreen warns on first run because the files have no publisher signature; see
  [Checking your download](#checking-your-download). The bundled third-party build tools keep their own upstream
  files and signatures.

## How to report

If something goes wrong in the player (`bin\nakagawa_player.exe`), note what you did and what the screen said.
Testers who want a full pass/fail run can follow [`SMOKE_TEST.md`](SMOKE_TEST.md).

Send smoke test results and bug reports with the
[Smoke Test Report issue template](https://github.com/Jstar269/nakagawa-recomp/issues/new?template=smoke_test_report.yml).
To tell us how far a game gets, post in
[Compatibility reports](https://github.com/Jstar269/nakagawa-recomp/discussions/categories/compatibility-reports), and
for questions, ask in [Help](https://github.com/Jstar269/nakagawa-recomp/discussions/categories/help).

When you report a problem:

1. Say which step in [`SMOKE_TEST.md`](SMOKE_TEST.md) failed and what the error card said. **Copy details** on
   the card puts a short report on the clipboard, including the error code (for example `ISO_CORRUPT`).
2. Settings > **Problem report** writes the logs and settings a report needs into one ZIP on your PC
   ([`SETUP.md`](SETUP.md#local-problem-report-nk_clipy-report)). The logs are in `%LOCALAPPDATA%\Nakagawa\logs` (the `logs` folder under `data` in portable mode).
3. Read every file before you attach it, to make sure it contains nothing private: no disc images, no game files,
   no decryption keys, and no personal paths you don't want to share.

## Thanks

Thank you to the people outside the maintainer team who contributed to this release:

- [@FractalEngineer](https://github.com/FractalEngineer): immediate-mode GE drawing and generic bring-up paths
  ([#1757](https://github.com/Jstar269/nakagawa-recomp/pull/1757)).

# Nakagawa Recomp v0.0.3 Release Notes

Status: CURRENT. These are the notes of the v0.0.3 release, **v0.0.3 - Service Ace!**; its release page names the
exact commit.
Release scope: what landed on `main` after v0.0.2, listed below.
Audience: players and contributors updating from v0.0.2 or v0.0.1, or trying Nakagawa for the first time.

## What v0.0.3 is

Nakagawa Recomp is an experimental static recompiler. It takes the program code of a PlayStation Portable (PSP) game
you own, translates it into C, links it with a native C runtime, and runs the result as a Windows program through
SDL3 and Vulkan. The player decrypts the game's program files from your own disc image, on your PC, the way the
PPSSPP emulator does.

v0.0.3 is the third public build, published as a pre-release: one portable ZIP for Windows with its checksum file,
and the source archive of the bundled build tools with its own. Its main addition is **Ace**, the play mode made for one game at a time: Hot Shots
Tennis can run its game logic at 60 updates a second instead of 30, and LittleBigPlanet has an Experimental package
that does the same. Enhanced mode gains parts for any game (supersampling, full 32-bit colour, texture packs and
more), and the player gains a build queue, a screenshot key, fast forward, Add to Steam and a disc check that works
without importing anything. Fixes follow three user reports (Sonic Rivals, PaRappa the Rapper and Warship Gunner 2
Portable), and the kernel, file, save-data, graphics and sound behaviour follows a real PSP in more places. As before, when a game needs
something Nakagawa doesn't support, it stops and says what is missing.

## What's new

### Ace mode: 60 FPS game logic

- **Hot Shots Tennis at 60 updates a second.** Hot Shots Tennis (UCUS98701, disc version 1.01) now has an
  enhancement package, so its page offers **Ace**. Ace is Enhanced plus that game's **60 FPS game logic**: the game
  itself updates 60 times a second instead of 30, with its own timing values rescaled so that it plays at the same
  speed. Authentic and Enhanced are unchanged, and the switch is off until you choose it. The package
  ships with Nakagawa ([#2326](https://github.com/Jstar269/Nakagawa-Recomp/pull/2326), [#2357](https://github.com/Jstar269/Nakagawa-Recomp/pull/2357)), is at version 15.0.0-pre1 ([#2362](https://github.com/Jstar269/Nakagawa-Recomp/pull/2362), [#2547](https://github.com/Jstar269/Nakagawa-Recomp/pull/2547)), and the maintainer released
  it from Experimental on 2026-11-12 ([#2521](https://github.com/Jstar269/Nakagawa-Recomp/pull/2521), [#2539](https://github.com/Jstar269/Nakagawa-Recomp/pull/2539)). It is short of a steady 60: on a private full-match route
  with an earlier version of the package (14.0.0-pre1), on a PC that was busy with other work, the game ran at 57.2
  updates a second in the menus, 59.9 on the title screen and 57.1 in a match, with short dips, and no compared
  game event (point winner, the ball's side, the ball's owner) differed from the authentic game. Version 15.0.0-pre1
  adds four sites to it and has not had that full comparison itself
  ([what was measured](ENHANCEMENT_CONTRACT.md#9101-hot-shots-tennis-ucus98701-hsttimingnative_60hz), [#1732](https://github.com/Jstar269/Nakagawa-Recomp/issues/1732)).
- **LittleBigPlanet at 60 updates a second (Experimental).** LittleBigPlanet (UCUS98744, disc version 2.00 only)
  ships with a 60 FPS game logic package, tagged EXPERIMENTAL on the game's page ([#2611](https://github.com/Jstar269/Nakagawa-Recomp/pull/2611), [#1931](https://github.com/Jstar269/Nakagawa-Recomp/issues/1931)). On a private
  route to the start of the first story level, the previous version of the package ran at a median of 60 updates a
  second on a PC that was 16 to 18 % busy with other work, with 1.2 to 1.8 % of level updates running long, in the
  same two scenes in every run, and the jump's height and air time equal to the authentic game. The version that
  ships adds twelve sites; its own run was on a much busier PC and does not show the rate. Gadgets and mechanisms
  other than the emitter are not surveyed (a rocket is expected to push twice as hard), and the camera follow and
  the music fade are not rescaled. LittleBigPlanet itself is rated Experimental: nothing past the start of the first level has been
  checked.
- **Installed copies find the packages they ship with.** A player started from an unpacked release did not see a
  package the release ships, and a game saved with 60 FPS game logic on refused to start or build
  (`ENHANCEMENT_UNAVAILABLE`). The player now reads the shipped packages once it knows its own folder ([#2610](https://github.com/Jstar269/Nakagawa-Recomp/pull/2610)).
- **Ace-only parts** (Settings > Enhanced, or **ENHANCED OPTIONS** on a game's page; all off by default):
  - **Late pad sampling** reads the controller later in each 60 FPS update, so a press reaches the screen sooner; it
    changes the game's timing ([#2369](https://github.com/Jstar269/Nakagawa-Recomp/pull/2369)).
  - **Stick dead zone** makes small pushes of the stick count in games that ignore part of its travel, as Hot Shots
    Tennis does; the feel changes ([#2370](https://github.com/Jstar269/Nakagawa-Recomp/pull/2370)).
  - **Short loads**, **Wider culling** and **Soft shadows** need data that a game's package declares
    ([#2368](https://github.com/Jstar269/Nakagawa-Recomp/pull/2368), [#2308](https://github.com/Jstar269/Nakagawa-Recomp/pull/2308), [#2407](https://github.com/Jstar269/Nakagawa-Recomp/pull/2407), [#2546](https://github.com/Jstar269/Nakagawa-Recomp/pull/2546), [#2371](https://github.com/Jstar269/Nakagawa-Recomp/pull/2371)). No package in this release declares any, so these three do
    nothing yet unless you have a package of your own.
- **Packages are site data, checked against your disc.** A published package holds addresses and replacement values
  for one disc version, never game code, and `BUILD PACKAGE` refuses it for any other executable ([#2323](https://github.com/Jstar269/Nakagawa-Recomp/pull/2323), [#2324](https://github.com/Jstar269/Nakagawa-Recomp/pull/2324)).

### Enhanced: more parts for any game

The parts below that are settings are on Settings > Enhanced for the whole library and on a game's page for that
game, and every new one is off by default. Three entries are not settings there: Dump textures is a switch in the
pause menu, fast forward is a key, and the movie upscaler is a developer switch. In Authentic none of this runs.

- **Supersampling** (2x or 4x) draws each frame larger and averages it down, which smooths jagged edges. 2x takes 4
  times the GPU work and 4x takes 16 times ([#2419](https://github.com/Jstar269/Nakagawa-Recomp/pull/2419)).
- **Full 32-bit colour** shows 16-bit games in full colour without dither, at the cost of GPU work and memory. The
  Ultra graphics preset turns it on ([#2269](https://github.com/Jstar269/Nakagawa-Recomp/pull/2269), [#2563](https://github.com/Jstar269/Nakagawa-Recomp/pull/2563)).
- **Texture packs.** A game's Enhanced options can point at a folder or `textures.zip` in the layout PSP emulators
  read (`textures.ini` and hash-named PNG files) and draw its textures in place of the game's. Nakagawa ships no
  packs ([#2358](https://github.com/Jstar269/Nakagawa-Recomp/pull/2358), [#2365](https://github.com/Jstar269/Nakagawa-Recomp/pull/2365)). To make one from your own game, the pause menu's Settings has a **Dump textures**
  switch that saves each texture the game draws as a PNG on your PC
  ([how](YOUR_OWN_GAMES.md#making-a-texture-pack-from-your-own-game), [#2364](https://github.com/Jstar269/Nakagawa-Recomp/pull/2364)).
- **Texture scaling** (2x to 6x) enlarges each texture with the xBRZ filter, and **Smart 2D filter** draws flat 2D
  pictures without blurring them. The owner admitted both for v0.0.3 while some of their comparison runs are still
  owed, so they stay off by default. The `SR_TEX_SCALE`, `SR_SMART_2D` and `SR_GLYPHS` environment variables no
  longer switch anything, and a game started from the player ignores them ([#2608](https://github.com/Jstar269/Nakagawa-Recomp/pull/2608)).
- **Finer patches** (2x to 4x) draws curved surfaces with more triangles ([#2272](https://github.com/Jstar269/Nakagawa-Recomp/pull/2272)).
- **Smooth far textures** reduces shimmer on distant surfaces at a high resolution. The measured gain is small when
  texture filtering is on: 0.1 % less shimmer on LittleBigPlanet's first level at 4x with 16x filtering, 2.2 %
  without it ([#2363](https://github.com/Jstar269/Nakagawa-Recomp/pull/2363)).
- **Unclipped sound.** Where a PSP clips a loud sound mix, this plays it through a limiter instead ([#2446](https://github.com/Jstar269/Nakagawa-Recomp/pull/2446),
  [#2609](https://github.com/Jstar269/Nakagawa-Recomp/pull/2609)).
- **Movies.** With the `SR_MOVIE_UPSCALE` developer switch, movie frames get their own upscaler (FSR 1 or
  Anime4K), now including movies a game draws through the GPU, such as Hot Shots Tennis's opening movie ([#2247](https://github.com/Jstar269/Nakagawa-Recomp/pull/2247),
  [#2569](https://github.com/Jstar269/Nakagawa-Recomp/pull/2569)). The player has no setting for it yet ([#2111](https://github.com/Jstar269/Nakagawa-Recomp/issues/2111)).
- **Smooth motion** keeps a figure whole when its pose changes too much between two frames, never loses a real
  frame to a busy display, and its readout now weighs each in-between frame by the motion it added ([#2312](https://github.com/Jstar269/Nakagawa-Recomp/pull/2312),
  [#2320](https://github.com/Jstar269/Nakagawa-Recomp/pull/2320), [#2351](https://github.com/Jstar269/Nakagawa-Recomp/pull/2351), [#2427](https://github.com/Jstar269/Nakagawa-Recomp/pull/2427)).
- **Graphics presets** now also set the Enhanced parts that suit them ([#2256](https://github.com/Jstar269/Nakagawa-Recomp/pull/2256)).
- **Fast forward.** In an Enhanced or Ace run, hold `Tab` or press `F9` to run the game at 3x; its sound is muted
  until it returns to normal speed. In Authentic the keys do nothing ([#2325](https://github.com/Jstar269/Nakagawa-Recomp/pull/2325)).

### Player and launcher

- **Build queue.** A game's page offers **Add to queue** beside BUILD PACKAGE, and a queue screen shows the game
  that is building and the ones that wait, each with **Up**, **Down** and **Remove** ([#2307](https://github.com/Jstar269/Nakagawa-Recomp/pull/2307)).
- **Build speed.** Settings > Build tools and cache has **Build speed**: Background, Balanced or Fastest, to trade
  build time for a usable PC ([#2295](https://github.com/Jstar269/Nakagawa-Recomp/pull/2295)). Builds also size their compile jobs from measured compiler memory, and say
  why when they run one job at a time ([#2286](https://github.com/Jstar269/Nakagawa-Recomp/pull/2286)).
- **Screenshots.** `F12` in the game window saves the next picture as a PNG in the `screenshots` folder of your
  user data ([#2321](https://github.com/Jstar269/Nakagawa-Recomp/pull/2321)).
- **Disc check without an import.** Nakagawa now carries Redump's list of PSP discs and checks each disc you add
  against it. Before, a disc read Not checked until you imported a list yourself ([#2530](https://github.com/Jstar269/Nakagawa-Recomp/pull/2530)).
- **Add to Steam.** A game's page can write a Steam shortcut that starts that game, after backing up Steam's
  shortcut file ([#2260](https://github.com/Jstar269/Nakagawa-Recomp/pull/2260)).
- **A notice when drawing falls back to software.** When the GPU renderer can't start, or keeps handing part of a
  scene to the software renderer, the game's page says so ([#2277](https://github.com/Jstar269/Nakagawa-Recomp/pull/2277), [#2526](https://github.com/Jstar269/Nakagawa-Recomp/pull/2526)). When a run starts on a slower path
  than it asked for (the GPU renderer or the Vulkan presenter did not start), a notice over the picture also says
  the game may run slower, until you hide it, in Authentic too ([#2607](https://github.com/Jstar269/Nakagawa-Recomp/pull/2607)).
- **Keyboard and mouse.** Mouse buttons can be bound like keys ([#2548](https://github.com/Jstar269/Nakagawa-Recomp/pull/2548)). With the keyboard shown, the Controls
  page has a **MOUSE AIM** switch with speed, smoothing and inversion, which drives the PSP's stick from mouse
  movement ([#2609](https://github.com/Jstar269/Nakagawa-Recomp/pull/2609)).
- **Nickname.** The player asks once for a nickname, which games read as the console's nickname. It is never taken
  from your Windows user name, and the default is `Player` ([#2267](https://github.com/Jstar269/Nakagawa-Recomp/pull/2267)).
- **Launcher music.** Settings > Launcher and sound has an optional music track for the launcher, off by default;
  it fades out when a game starts and is never mixed into a game's sound ([#2536](https://github.com/Jstar269/Nakagawa-Recomp/pull/2536)).
- **Discord status, built but not in this download.** The player can show the game you are playing through the
  Discord app on your PC, as an opt-in that is off by default ([#2261](https://github.com/Jstar269/Nakagawa-Recomp/pull/2261)). The switch appears only in a build that
  carries the project's Discord application ID, and this release's source carries none, so this download has no
  Discord status switch ([Network use](SETUP.md#network-use)).
- **Homebrew.** An `EBOOT.PBP` built with the PSPSDK imports ([#2287](https://github.com/Jstar269/Nakagawa-Recomp/pull/2287)) and runs from its own Memory Stick folder,
  so it finds the files beside it ([#2426](https://github.com/Jstar269/Nakagawa-Recomp/pull/2426)).
- **Pages that fit.** A page taller than the window scrolls, the pause menu fits the game window, and the game
  page's sentences wrap instead of ending in an ellipsis ([#2300](https://github.com/Jstar269/Nakagawa-Recomp/pull/2300), [#2277](https://github.com/Jstar269/Nakagawa-Recomp/pull/2277), [#2390](https://github.com/Jstar269/Nakagawa-Recomp/pull/2390)).
- **Library find box.** It uses the same on-screen keyboard as the nickname editor, with Shift, symbols and the
  same controller shortcuts ([#2389](https://github.com/Jstar269/Nakagawa-Recomp/pull/2389)).
- **Updates.** An update interrupted while unpacking no longer blocks the next try ([#2470](https://github.com/Jstar269/Nakagawa-Recomp/pull/2470)). This build carries no
  update-signing key, so Check for updates shows a newer version and links to its release page.
- **PSP fonts page.** It no longer says Cyrillic is missing ([#2438](https://github.com/Jstar269/Nakagawa-Recomp/pull/2438)).
- **Developer mode.** The pause menu's VRAM viewer can step between the captures a run wrote ([#2552](https://github.com/Jstar269/Nakagawa-Recomp/pull/2552)).

### Hot Shots Tennis

- **Still Playable, with fewer special cases.** The game now runs its own `sprintf` and declares no dispatch alias,
  and its loops and `abort()` translate as written; the native bridge and the temporary patch that stood in for
  them are retired ([#2264](https://github.com/Jstar269/Nakagawa-Recomp/pull/2264), [#2450](https://github.com/Jstar269/Nakagawa-Recomp/pull/2450), [#2453](https://github.com/Jstar269/Nakagawa-Recomp/pull/2453), [#363](https://github.com/Jstar269/Nakagawa-Recomp/issues/363)).
- **Loading.** A disc read now takes the drive's time in game time and no longer stops sound and picture while the
  PC reads ([#2440](https://github.com/Jstar269/Nakagawa-Recomp/pull/2440)); the sound device opens at start-up instead of at the game's first sound ([#2439](https://github.com/Jstar269/Nakagawa-Recomp/pull/2439)); and a save
  hands the processor over between writes ([#2233](https://github.com/Jstar269/Nakagawa-Recomp/pull/2233)). Sound effects and text clicks can still drop out while it
  loads ([#1296](https://github.com/Jstar269/Nakagawa-Recomp/issues/1296)).
- **Text.** The stand-in fonts have heavier horizontal strokes and corrected round letters, closer to the PSP's
  ([#2236](https://github.com/Jstar269/Nakagawa-Recomp/pull/2236), [#2301](https://github.com/Jstar269/Nakagawa-Recomp/pull/2301), [#1300](https://github.com/Jstar269/Nakagawa-Recomp/issues/1300)).

### Compatibility and accuracy

- **Sonic Rivals.** A reported black screen ([#2375](https://github.com/Jstar269/Nakagawa-Recomp/discussions/2375)) came from display-list finish callbacks running inside one
  another. They now run one at a time, in completion order, and one raised while interrupts are suspended runs when
  they resume ([#2486](https://github.com/Jstar269/Nakagawa-Recomp/pull/2486), [#2513](https://github.com/Jstar269/Nakagawa-Recomp/pull/2513)). On a private route the US release reaches its stage select; nothing past that has
  been checked.
- **PaRappa the Rapper.** A reported stop at start ([#2445](https://github.com/Jstar269/Nakagawa-Recomp/discussions/2445)) is fixed: a thread's stack can take memory the game
  has just freed ([#2469](https://github.com/Jstar269/Nakagawa-Recomp/pull/2469), [#2514](https://github.com/Jstar269/Nakagawa-Recomp/pull/2514)). In a run without a window the game then loads and keeps drawing frames; what
  it shows has not been checked.
- **Warship Gunner 2 Portable.** A v0.0.2 report of heavy stutter once sound streams led to two changes: the
  mixer keeps one more device pull of sound queued ([#2359](https://github.com/Jstar269/Nakagawa-Recomp/pull/2359)), and a slow disc read no longer holds up sound and
  picture ([#2440](https://github.com/Jstar269/Nakagawa-Recomp/pull/2440)). The issue stays open ([#2361](https://github.com/Jstar269/Nakagawa-Recomp/issues/2361)).
- **LittleBigPlanet.** Disc version 1.00 no longer has its semaphores, event flags and lightweight mutexes refused
  past 128 ([#2258](https://github.com/Jstar269/Nakagawa-Recomp/pull/2258)); with a save it made itself it plays its intro and reaches the first tutorial area
  ([#2306](https://github.com/Jstar269/Nakagawa-Recomp/pull/2306)).
- **A game's own movie library.** A game that ships `mpeg.prx` now runs it as guest code, with the host serving
  only the Media Engine boundary beneath it ([#2239](https://github.com/Jstar269/Nakagawa-Recomp/pull/2239), [#286](https://github.com/Jstar269/Nakagawa-Recomp/issues/286)). The values at that boundary come from JPCSP and are
  labelled synthetic until they are measured on a PSP.
- **Movies and sound decoding.** A PSMF picture carries the time of its own access unit when the decoder reorders
  ([#2232](https://github.com/Jstar269/Nakagawa-Recomp/pull/2232)); colour conversion refusals, 16-bit output and YCbCr mode 2 follow PSP measurements ([#2241](https://github.com/Jstar269/Nakagawa-Recomp/pull/2241)); the
  ATRAC3plus frame size is read correctly ([#2249](https://github.com/Jstar269/Nakagawa-Recomp/pull/2249), [#2388](https://github.com/Jstar269/Nakagawa-Recomp/pull/2388)); and `_sceAtracGetContextAddress` returns a context
  the game can read ([#2480](https://github.com/Jstar269/Nakagawa-Recomp/pull/2480)).
- **Save data.** A missing or damaged save is reported with the PSP's codes ([#2356](https://github.com/Jstar269/Nakagawa-Recomp/pull/2356)), and `GETSIZE` on a missing
  save leaves the fields a PSP-3000 leaves ([#2472](https://github.com/Jstar269/Nakagawa-Recomp/pull/2472)).
- **Files.** Creating and writing on a read-only Memory Stick answer as the PSP does ([#2409](https://github.com/Jstar269/Nakagawa-Recomp/pull/2409)), and a seek on a
  `flash0` file refuses a destination outside the file ([#2385](https://github.com/Jstar269/Nakagawa-Recomp/pull/2385)).
- **Kernel and HLE.** Fixed and variable pool limits are the ones a PSP-3000 measured ([#2410](https://github.com/Jstar269/Nakagawa-Recomp/pull/2410)); closing standard
  output and a module stopping itself answer as measured ([#2421](https://github.com/Jstar269/Nakagawa-Recomp/pull/2421)); `sceRtcTickAddMinutes`, `-Hours` and `-Weeks`
  add as the PSP does ([#2479](https://github.com/Jstar269/Nakagawa-Recomp/pull/2479)); every utility dialog refuses a shutdown outside its finished state ([#2533](https://github.com/Jstar269/Nakagawa-Recomp/pull/2533)); and
  `sceAudioGetChannelRestLen` answers ([#2430](https://github.com/Jstar269/Nakagawa-Recomp/pull/2430)).
- **Determinism.** Audio, the Memory Stick and the console's identity no longer read values from the host PC, so
  two runs see the same ones ([#2270](https://github.com/Jstar269/Nakagawa-Recomp/pull/2270)).
- **VFPU.** `vf2i`, `mtvc`, `vcmp`, `vadd`, `vsub`, `vmul`, `vscl`, `vdiv`, `vmin`, `vmax` and `vus2i`, and the
  dot, cross, quaternion and matrix forms, follow PSP-3000 measurements ([#2273](https://github.com/Jstar269/Nakagawa-Recomp/pull/2273), [#2290](https://github.com/Jstar269/Nakagawa-Recomp/pull/2290), [#2292](https://github.com/Jstar269/Nakagawa-Recomp/pull/2292), [#2293](https://github.com/Jstar269/Nakagawa-Recomp/pull/2293),
  [#2303](https://github.com/Jstar269/Nakagawa-Recomp/pull/2303)).
- **Translator.** A body that leaves its frame by loading the stack pointer releases the earlier restores
  ([#2238](https://github.com/Jstar269/Nakagawa-Recomp/pull/2238)); an untranslated instruction word is refused by name instead of crashing the build ([#2498](https://github.com/Jstar269/Nakagawa-Recomp/pull/2498)); and data
  mistaken for code is withdrawn in images without relocation evidence ([#2564](https://github.com/Jstar269/Nakagawa-Recomp/pull/2564)).

### Graphics and sound

- **Software renderer.** It draws to the target's real width, up to 512 pixels, and keeps colour and depth writes
  inside the buffer they belong to. This closes v0.0.2's known issue about draws clipped to 480x272 ([#2382](https://github.com/Jstar269/Nakagawa-Recomp/pull/2382),
  [#2424](https://github.com/Jstar269/Nakagawa-Recomp/pull/2424), [#2193](https://github.com/Jstar269/Nakagawa-Recomp/issues/2193)). It also caches transformed vertices per draw ([#2378](https://github.com/Jstar269/Nakagawa-Recomp/pull/2378)).
- **Effects that scale.** A render target the game samples as a texture is read at the render scale, at its own
  size, so bloom, blur and similar effects scale with the scene. This closes v0.0.2's known issue ([#2271](https://github.com/Jstar269/Nakagawa-Recomp/pull/2271),
  [#2441](https://github.com/Jstar269/Nakagawa-Recomp/pull/2441), [#2052](https://github.com/Jstar269/Nakagawa-Recomp/issues/2052)). The reads that still decode at the PSP's resolution are listed with their reasons ([#2534](https://github.com/Jstar269/Nakagawa-Recomp/pull/2534),
  [#2540](https://github.com/Jstar269/Nakagawa-Recomp/pull/2540), [#2279](https://github.com/Jstar269/Nakagawa-Recomp/issues/2279)).
- **More drawn on the GPU.** The colour test runs in the fragment shader and untextured lines are drawn on the GPU
  ([#2318](https://github.com/Jstar269/Nakagawa-Recomp/pull/2318)); stencil batches that write once draw without splits ([#2355](https://github.com/Jstar269/Nakagawa-Recomp/pull/2355), [#2523](https://github.com/Jstar269/Nakagawa-Recomp/pull/2523)); only the pixels a palette
  load or a CPU read overlaps are read back ([#2317](https://github.com/Jstar269/Nakagawa-Recomp/pull/2317)).
- **See-through layers.** Overlapping translucent shapes in one batch read the picture in order through a shader
  interlock, including 32-bit colour draws and depth writes under alpha, colour, range and stencil tests ([#2251](https://github.com/Jstar269/Nakagawa-Recomp/pull/2251),
  [#2383](https://github.com/Jstar269/Nakagawa-Recomp/pull/2383), [#2436](https://github.com/Jstar269/Nakagawa-Recomp/pull/2436)). The issue stays open for the cases left ([#2109](https://github.com/Jstar269/Nakagawa-Recomp/issues/2109)).
- **GE details.** Per-level palette offsets ([#2266](https://github.com/Jstar269/Nakagawa-Recomp/pull/2266)); a through-mode depth value saturates to 16 bits and a sprite
  at a NaN depth is drawn, as on a PSP-3000 ([#2334](https://github.com/Jstar269/Nakagawa-Recomp/pull/2334), [#2411](https://github.com/Jstar269/Nakagawa-Recomp/pull/2411)).
- **Presenting.** A game frame never waits for the display while the game is paced ([#2319](https://github.com/Jstar269/Nakagawa-Recomp/pull/2319)).
- **Sound.** Besides the loading fixes above, the SAS envelope and mix were measured on a PSP-3000 ([#2516](https://github.com/Jstar269/Nakagawa-Recomp/pull/2516),
  [#2560](https://github.com/Jstar269/Nakagawa-Recomp/pull/2560)); the title-logo sound still matches a PSP's level but not its waveform ([#2135](https://github.com/Jstar269/Nakagawa-Recomp/issues/2135)).

### Security hardening

These come from the maintainer's own source review after v0.0.2 ([#2603](https://github.com/Jstar269/Nakagawa-Recomp/pull/2603)). They are hardening: none can be reached
in v0.0.2 as shipped without something already planted on the PC.

- **Archive member names.** When the player unpacks its build tools, an archive member or link whose name would not
  stay inside the install folder on Windows is refused by name.
- **Memory Stick files.** A game's request to create or empty a file is checked to stay inside the game's Memory
  Stick folder before the file is touched, and links that lead out of the folder are refused. The save-data code
  follows the same order.
- **DLL search order.** The player and the games it starts load DLLs by name only from their own folder and the
  Windows system folder, not from the current folder or `PATH`.

### Packaging and verification

- **The download is the portable ZIP only.** The source can now build a per-user `Setup.exe` that carries the ZIP's
  exact files, with a Start menu entry and an uninstaller, reproducibly ([#2216](https://github.com/Jstar269/Nakagawa-Recomp/pull/2216), [#2208](https://github.com/Jstar269/Nakagawa-Recomp/issues/2208)). It is not part of
  v0.0.3: no installer is published with this release, and one is planned for v0.1.0.
- The portable ZIP names the commit it was built from, and a dry run compares two build records ([#2462](https://github.com/Jstar269/Nakagawa-Recomp/pull/2462)); the
  SBOMs carry the SHA-256 of every packaged binary ([#2454](https://github.com/Jstar269/Nakagawa-Recomp/pull/2454)).
- A package build puts the bundled UCRT64 toolchain first on its own `PATH` ([#2283](https://github.com/Jstar269/Nakagawa-Recomp/pull/2283)), and staged module paths
  stay usable when your user folder has a space in its name ([#2282](https://github.com/Jstar269/Nakagawa-Recomp/pull/2282)).
- A fresh Linux or macOS install keeps everything in one `Nakagawa` data folder; an existing install keeps its
  folders ([#2525](https://github.com/Jstar269/Nakagawa-Recomp/pull/2525)). There is still no Linux or macOS release.

### For developers

- **Repository layout.** The test fixtures moved from `fixtures/` to `tests/fixtures/`, and the manager script
  moved from the repository root into `tools/`: run it as `.\tools\nk_manager.ps1`. `.\nk.ps1` is unchanged.
  `REVIEW.md`, `SUPPORT.md` and
  `THIRD_PARTY_LICENSES/` left the repository root ([#2605](https://github.com/Jstar269/Nakagawa-Recomp/pull/2605), [#2606](https://github.com/Jstar269/Nakagawa-Recomp/pull/2606), [#2615](https://github.com/Jstar269/Nakagawa-Recomp/pull/2615)).
- **Resume at a PC.** An opt-in code generation mode lets a parked guest thread resume at a program counter by
  native re-entry, the groundwork for save states ([#2451](https://github.com/Jstar269/Nakagawa-Recomp/pull/2451), [#2172](https://github.com/Jstar269/Nakagawa-Recomp/issues/2172)).
- **Enhancement packages.** New site transforms for damped motion, gated stores and culling hooks ([#2240](https://github.com/Jstar269/Nakagawa-Recomp/pull/2240),
  [#2360](https://github.com/Jstar269/Nakagawa-Recomp/pull/2360), [#2366](https://github.com/Jstar269/Nakagawa-Recomp/pull/2366), [#2413](https://github.com/Jstar269/Nakagawa-Recomp/pull/2413), [#2308](https://github.com/Jstar269/Nakagawa-Recomp/pull/2308)), a code-hook package beside the timing package ([#2407](https://github.com/Jstar269/Nakagawa-Recomp/pull/2407)), and more room on
  the launch line ([#2567](https://github.com/Jstar269/Nakagawa-Recomp/pull/2567)). Sharp game text is the launch line's `glyphs=on` ([#2253](https://github.com/Jstar269/Nakagawa-Recomp/pull/2253), [#2524](https://github.com/Jstar269/Nakagawa-Recomp/pull/2524)), and `depth=32`
  asks for a 32-bit host depth buffer ([#2415](https://github.com/Jstar269/Nakagawa-Recomp/pull/2415)); neither has a player setting yet.
- **Diagnostics.** Replay stops and freezes the flight record at the first divergent checkpoint ([#2408](https://github.com/Jstar269/Nakagawa-Recomp/pull/2408));
  `SR_GESCENE` dumps one frame's scene record ([#2482](https://github.com/Jstar269/Nakagawa-Recomp/pull/2482)); the renderer counts why each draw stays on the CPU
  ([#2265](https://github.com/Jstar269/Nakagawa-Recomp/pull/2265)); `SR_PROFILE` prints on every hard exit ([#2322](https://github.com/Jstar269/Nakagawa-Recomp/pull/2322)).
- **Backends.** The analysis IR leaves generated C byte-identical ([#2380](https://github.com/Jstar269/Nakagawa-Recomp/pull/2380)); the LLVM backend lowers the stale-code
  and static-verify hooks and keys its packages on the lowering plan ([#2456](https://github.com/Jstar269/Nakagawa-Recomp/pull/2456), [#2459](https://github.com/Jstar269/Nakagawa-Recomp/pull/2459)).
- **PSP measurements.** New PSP-3000 reads for VFPU forms, kernel pools, file seeks and truncation, GE raster
  bounds and depth rounding, SAS and the utility dialogs ([#2293](https://github.com/Jstar269/Nakagawa-Recomp/pull/2293), [#2367](https://github.com/Jstar269/Nakagawa-Recomp/pull/2367), [#2416](https://github.com/Jstar269/Nakagawa-Recomp/pull/2416), [#2516](https://github.com/Jstar269/Nakagawa-Recomp/pull/2516), [#2557](https://github.com/Jstar269/Nakagawa-Recomp/pull/2557), [#2560](https://github.com/Jstar269/Nakagawa-Recomp/pull/2560),
  [#2593](https://github.com/Jstar269/Nakagawa-Recomp/pull/2593)).
- **Platforms, none released.** macOS link and coroutine pieces ([#2485](https://github.com/Jstar269/Nakagawa-Recomp/pull/2485)); an on-device C toolchain and test route
  for Android ([#2252](https://github.com/Jstar269/Nakagawa-Recomp/pull/2252), [#2316](https://github.com/Jstar269/Nakagawa-Recomp/pull/2316), [#2483](https://github.com/Jstar269/Nakagawa-Recomp/pull/2483)); an OpenXR module behind a fake-runtime self-test ([#2288](https://github.com/Jstar269/Nakagawa-Recomp/pull/2288)); and an ad
  hoc relay codec and transport seam, with no network connection made for a game ([#2549](https://github.com/Jstar269/Nakagawa-Recomp/pull/2549)).
- **Testing.** A public corpus of four pinned PSPSDK samples ([#2289](https://github.com/Jstar269/Nakagawa-Recomp/pull/2289)), homebrew import routes run end to end in CI
  ([#2537](https://github.com/Jstar269/Nakagawa-Recomp/pull/2537)), and a sealed project-owned disc that goes from ISO to Play through the player ([#2431](https://github.com/Jstar269/Nakagawa-Recomp/pull/2431)).
- **CI and tooling.** Quick checks on each pull request push and the full matrix per landing batch ([#2346](https://github.com/Jstar269/Nakagawa-Recomp/pull/2346));
  trusted workflows no longer depend on `pull_request_target` ([#2342](https://github.com/Jstar269/Nakagawa-Recomp/pull/2342)); the tools prefer PowerShell 7 ([#2296](https://github.com/Jstar269/Nakagawa-Recomp/pull/2296));
  the legacy launcher renderer is removed ([#2255](https://github.com/Jstar269/Nakagawa-Recomp/pull/2255)); and the recorded typing backlog in `tools/` went from 318 to
  172 errors ([#2234](https://github.com/Jstar269/Nakagawa-Recomp/pull/2234), [#2434](https://github.com/Jstar269/Nakagawa-Recomp/pull/2434), [#2435](https://github.com/Jstar269/Nakagawa-Recomp/pull/2435)).

## Public verification you can reproduce

You can check these parts of v0.0.3 yourself, with no retail ISO, PRX, save, key or decrypted module:

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

## What v0.0.3 is not

- **No games included.** This repository contains no game programs, game assets, firmware modules, keys bound to a
  console or a user, or private test recordings. Its only key material is the public game-decryption tables PPSSPP
  also ships, in `src/core/nk_psp_builtin_keys.c`. Nakagawa never downloads games or game data. You supply your own
  disc image of a game you own, or the `EBOOT.PBP` of a PSN release of a UMD game you own. An enhancement package
  holds addresses and replacement values for your copy of a game, not the game.
- **Few network connections, all yours to choose.** The player connects only when you choose to: the build-tool
  download, which the ZIP doesn't need because the tools are in it, and Check for updates. Discord status, when you
  turn it on, talks only to the Discord app on your PC. Enhanced and Ace run on your PC and make no connection
  ([`SETUP.md`](SETUP.md#network-use)).
- **Not a finished emulator.** Hot Shots Tennis is the one retail game rated **Playable**;
  [`COMPATIBILITY.md`](COMPATIBILITY.md) lists how far each tested game gets. A valid PSP disc with no title file
  is imported as **Experimental**. PGD-protected content, PSP minis, games tied to the PSP that bought them and PSOne
  Classics are not supported.
- **No PSP fonts included.** The PSP's own fonts belong to Sony, so the project doesn't include or share them; you
  import them from your own PSP in **Settings > PSP FONTS**
  ([how to copy them](SETUP.md#copying-the-fonts-from-your-psp)). Without them a game shows the project's own open
  fonts, which look close to the PSP's but not the same.
- **No texture packs or textures included.** A texture pack is yours to supply, and dumped textures stay on your PC.
- **Windows only for the download, and no installer.** The release is one portable ZIP for Windows 11 x64; a
  `Setup.exe` installer is planned for v0.1.0. On Linux, the player and runtime build from source and use the
  system's own build tools; that is a development setup, not a release.
- **Sound needs an audio device.** With no audio device, a game runs silently after one message.

## Checking your download

v0.0.3 is not code-signed, so the first time you run `nakagawa_player.exe`, Windows SmartScreen may say
"Windows protected your PC", because the file has no publisher signature. That warning is about the missing
signature, not about anything found in the file. Don't turn SmartScreen or your antivirus off. Instead, before you
choose **More info** and then **Run anyway**, check that the download is the one this project built:

1. Before you extract it, in the folder where you saved the ZIP, compare its SHA-256 with
   `nakagawa-recomp-0.0.3-windows-x64.zip.sha256`, published next to it on the release page:

   ```powershell
   (Get-FileHash .\nakagawa-recomp-0.0.3-windows-x64.zip -Algorithm SHA256).Hash.ToLower()
   ```

2. After extracting, open PowerShell in the extracted folder (the one with `bin\` in it).
   `SHA256SUMS.txt` there lists the SHA-256 of every file in the package.
3. With the [GitHub CLI](https://cli.github.com/), check that each executable and DLL under `bin\` was
   built by this repository's release workflow. Each file carries a signed build attestation (SLSA Build
   Level 2). The GitHub CLI needs you to sign in once first (`gh auth login`). Set `$Commit` to the full commit
   the release page names (the commit the `v0.0.3` tag points at),
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
`nakagawa-recomp-0.0.3-prerequisite-sources.zip`, with its own `.sha256`: the MSYS2 source package of each MSYS2 package, the CPython source release, and the source releases of
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
  `prereqs/`: CPython 3.15.2, and the MSYS2 UCRT64 GCC 16.2, GNU Make 4.4.1, SDL3 3.4.20 and their libraries.
  Before each build the player checks them against `source/assets/prereq_manifest.json`, then uses them with no
  consent card and no download. If that folder is damaged, the player names the first missing or changed file
  (`PREREQUISITE_BUNDLE_INVALID`) instead of downloading a replacement: extract the ZIP again. Without `prereqs/`,
  as in a source checkout, the player offers one consent card for the same pinned tools.

**Hardware:** Windows 11 x64 with a Vulkan GPU, 8 GB RAM (16 GB recommended) and 5 GB of free disk space (10 GB on an
SSD recommended). Building a game is the heavy part. Supersampling, Full 32-bit colour and texture scaling need more
GPU memory. The measured numbers are in [`SETUP.md`](SETUP.md#system-requirements).

**Reference PC:** the project's reference PC runs Windows 11 with an AMD Ryzen 9 5900X and an NVIDIA GeForce RTX
3080. The records behind the 60 FPS update rates in these notes say how busy the PC was, but do not name its
processor or graphics card.

**Download size:** the release page gives the size of the ZIP. The build tools in its `prereqs/` unpack to 7,305
files and 699 MB, and their source is a separate 531 MB download next to the ZIP; see
[Source of the bundled build tools](#source-of-the-bundled-build-tools). Those two figures were measured on a
rehearsal build on 2026-11-14, with the same pinned tools; the release page has the final sizes.

## Updating from v0.0.2

- Unpack v0.0.3 into a new folder. Your library, built games, saves and settings are in the per-user data folder, so
  v0.0.3 finds them, and the old folder can be deleted afterwards
  ([`SETUP.md`](SETUP.md#install-update-and-uninstall-windows)).
- Settings > **CHECK FOR UPDATES** in v0.0.2 shows v0.0.3 and links to its release page. v0.0.2 does not download
  it: neither build carries an update-signing key.
- Games built by v0.0.2 or v0.0.1 are marked as built by an older version of Nakagawa. They still start; building
  them again gets this release's fixes, and the library offers **Rebuild all**.
- To play Hot Shots Tennis (disc version 1.01) or LittleBigPlanet (disc version 2.00 only) with 60 FPS game logic,
  choose **Ace** on the game's page and build again: Ace uses a second package built for it, beside the authentic
  one. A package is refused for any other version of the game's program.
- The player asks for a nickname the first time v0.0.3 starts. Skipping keeps `Player`.
- Controller and key bindings carry over. The input profile moves to schema 6 for mouse buttons ([#2548](https://github.com/Jstar269/Nakagawa-Recomp/pull/2548)).
- If you built from source: the manager script is now `tools\nk_manager.ps1`, and a development build that called
  itself
  `0.1.0-dev` ranks above 0.0.3, so its Check for updates says it has the newest version. Build from this release's
  commit to get a player that calls itself 0.0.3.

## Known issues

- **Hot Shots Tennis at 60 FPS is short of a steady 60.** Measured at 57 to 60 updates a second on a busy PC, with
  short dips; a `SLOWED` line in the log names a stretch under 57 ([#1732](https://github.com/Jstar269/Nakagawa-Recomp/issues/1732)).
- **LittleBigPlanet's 60 FPS package is Experimental**, and for disc version 2.00 only. Two scenes run long,
  gadgets and mechanisms other than the emitter are not surveyed, and the camera follow and the music fade are not
  rescaled ([#1931](https://github.com/Jstar269/Nakagawa-Recomp/issues/1931)).
- **Some sound effects and text clicks drop out in Hot Shots Tennis while it loads** ([#1296](https://github.com/Jstar269/Nakagawa-Recomp/issues/1296)).
- **Hot Shots Tennis's title-logo sound effects match a PSP's level only approximately.** The level is within about
  0.5 dB of a PSP-3000 capture, but the waveform is not the same ([#2135](https://github.com/Jstar269/Nakagawa-Recomp/issues/2135)).
- **Hot Shots Tennis's in-game text uses the project's stand-in fonts unless you import your PSP's**, and the
  stand-ins don't look exactly like the PSP's. Text drawn from your own PSP's fonts has not been compared with a PSP
  ([#1300](https://github.com/Jstar269/Nakagawa-Recomp/issues/1300)).
- **Overlapping see-through layers can still blend wrongly on the GPU** in the cases the ordered path does not
  cover ([#2109](https://github.com/Jstar269/Nakagawa-Recomp/issues/2109)).
- **Some render-target reads stay at the PSP's resolution** above render scale 1; each case is listed with its
  reason ([#2279](https://github.com/Jstar269/Nakagawa-Recomp/issues/2279)).
- **Texture scaling, Smart 2D filter and several Ace parts change the pictures a game can read back**, and their
  comparison runs are not complete; they are off by default for that reason.
- **Movie timing.** The number of decoded movie pictures and the audio offset are not fully matched to a PSP
  ([#279](https://github.com/Jstar269/Nakagawa-Recomp/issues/279)).
- **Most other games stop early.** Each one names the missing piece; typical causes are unsupported system calls
  and interpreter rejections. A game that keeps drawing a black screen can be stopped with **STOP GAME**.
- **No online or ad hoc play.** No call a game makes opens a network connection. `sceNetInit` and `sceNetTerm`
  succeed with no network, the wireless switch and address answer from the console profile, and the ad hoc
  control library answers as a console with nobody in range ([#2549](https://github.com/Jstar269/Nakagawa-Recomp/pull/2549)). Its join, game-mode and name-lookup calls,
  and the ad hoc data and matching, access-point, HTTP and SSL calls that are registered, are refused with one
  error code. There are no fake connections.
- **Not code-signed.** Windows SmartScreen warns on first run because the files have no publisher signature; see
  [Checking your download](#checking-your-download). The bundled third-party build tools keep their own upstream
  files and signatures.

## How to report

If something goes wrong in the player (`bin\nakagawa_player.exe`), note what you did and what the screen said.
Testers who want a full pass/fail run can follow [`SMOKE_TEST.md`](SMOKE_TEST.md).

Send smoke test results and bug reports with the
[Smoke Test Report issue template](https://github.com/Jstar269/Nakagawa-Recomp/issues/new?template=smoke_test_report.yml).
To tell us how far a game gets, post in
[Compatibility reports](https://github.com/Jstar269/Nakagawa-Recomp/discussions/categories/compatibility-reports), and
for questions, ask in [Help](https://github.com/Jstar269/Nakagawa-Recomp/discussions/categories/help).

When you report a problem:

1. Say which step in [`SMOKE_TEST.md`](SMOKE_TEST.md) failed and what the error card said. **Copy details** on
   the card puts a short report on the clipboard, including the error code (for example `ISO_CORRUPT`).
2. Settings > **Problem report** writes the logs and settings a report needs into one ZIP on your PC
   ([`SETUP.md`](SETUP.md#local-problem-report-nk_clipy-report)). The logs are in `%LOCALAPPDATA%\Nakagawa\logs` (the `logs` folder under `data` in portable mode).
3. Read every file before you attach it, to make sure it contains nothing private: no disc images, no game files,
   no decryption keys, and no personal paths you don't want to share.

## Thanks

Thank you to the people whose reports led to fixes in this release:

- [@Fabchaotix](https://github.com/Fabchaotix): the Sonic Rivals report ([#2375](https://github.com/Jstar269/Nakagawa-Recomp/discussions/2375)).
- [@Superleopey](https://github.com/Superleopey): the PaRappa the Rapper report ([#2445](https://github.com/Jstar269/Nakagawa-Recomp/discussions/2445)).

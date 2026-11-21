# Online mode and developer mode (design)

Status: step 1 of the implementation order is built (the switches, the wizard step and the developer log panel).
The update check is built too, as its own opt-in feature outside online mode (see
[Update check](#update-check)), as are [Discord status](#discord-status) and
[RetroAchievements](#retroachievements), each with its own opt-in; news and reports are not. The player is offline unless the user asks
([`SETUP.md`](SETUP.md#network-use)). This page is the contract the implementation follows. The launcher gets an update check, a reporting system and a news feed, all behind an
**online mode** that the setup wizard asks about, and a **developer mode** that shows a developer log for
diagnosing new issues.

## Principles

- **Asked once, changeable anytime.** The setup wizard step offers the switches for online mode and developer
  mode, and each is a switch in Settings afterwards. Both start switched off. Until the online features exist
  the Online mode switch is not shown to anyone whose stored setting is Off (see the note under the table).
- **One host.** Online mode talks to `nakagawa.jaycast.net` over HTTPS and nothing else. The news site (The
  FreshWire) cross-posts to it, so the player's allowlist stays one host. (The update check, outside online mode,
  uses GitHub's hosts for the releases themselves; the opt-in cover-art download uses
  `thumbnails.libretro.com`.) `docs/SETUP.md` Network use lists every host and connection.
- **Nothing is sent on its own.** No telemetry, analytics, crash upload or identifier. The automatic traffic is
  fetching the news feed while online mode is on, the one update check at start when the user turned it on, and
  the cover-art download when **Download cover art** is on (Settings > Online and developer, off by default), once
  per verified Redump disc not yet cached. Requests carry no cookie and no ID; the
  update check's user agent is the fixed string `NakagawaRecomp/UpdateCheck`, with no version or system details.
- **Game data never leaves the PC.** No game code, game data, decrypted module, package, save, guest memory or
  capture is ever uploaded (`AGENTS.md`, section 5). A report sends only the fields the user sees in its preview.
- **Offline stays complete.** Every feature works without online mode except news and reports. A network failure is
  shown once, quietly, and never blocks the library or a game.
- **Games stay offline.** Online mode is a launcher feature. The PSP network services a game calls stay fail-closed
  whatever this setting says.

## Setup wizard step: "Online and developer options"

Shown right after Welcome, before the game step, so every first run sees it (it needs no game). The design is two
switches, each with one plain sentence (today the Online mode row is hidden, as the note below the table says):

| Switch | Default | Sentence |
| --- | --- | --- |
| Online mode | Off | "When on, Nakagawa checks nakagawa.jaycast.net for news and lets you send reports. Nothing is sent without you." |
| Developer mode | Off | "Show a developer log on screen to help track down problems. Stays on this PC." |

Until news and reports exist, the wizard step and Settings > Online and developer do not show the Online mode
switch or its two parts (news, reports): they say in one line that online features are not built yet. The
wizard step then has only the Developer mode switch; Settings > Online and developer has the Developer mode switch
and, in a build that can show it, the [Discord status](#discord-status) switch. A user whose stored `online_mode` is On still gets the Online mode
switch (with a sentence that it does nothing yet), so it can be turned off. The `online_mode`, `online_news` and `online_reports` settings are still
stored and still default to off, so a build that adds the features keeps what the user chose; with online mode
on, Settings will show its two parts again, each on by default and each switchable alone. The step is skippable;
skipping keeps developer mode off.

## Update check

Built for v0.0.2: a way for users to Check for Updates and have the newer version
automatically downloaded optionally. It replaces the signed `updates.json` design this section held before:
the update check is its own opt-in feature, not a part of online mode, and it uses GitHub, where the releases
already are.

- **Settings > Updates.** CHECK FOR UPDATES asks GitHub's public Releases API for this repository once
  (`src/player/update_check.c`), anonymously. The page shows this version, the newest one (pre-releases count,
  since every v0.0.x release is one), its date, the release page link and the first lines of its notes.
- **Check at start** is a switch on that page, off by default (`check_updates_at_start` in `settings.json`).
  When on, the player asks once at start. A newer version shows one quiet "Version x is out" button on the
  library; a failure is not shown, only logged in the developer log.
- **DOWNLOAD** and **INSTALL** are separate clicks. DOWNLOAD first checks the release's `.sha256` file
  against its minisign signature, `.sha256.minisig`, made offline by the owner with the update-signing key
  whose public half is built into the player ([`RELEASE_SIGNING.md`](RELEASE_SIGNING.md)). The Windows ZIP is
  fetched only after that, and kept only if its SHA-256 matches the signed checksum file and GitHub's own
  digest of the asset; Install unpacks it into a new folder and tells the user how to start it. Nothing is
  replaced and nothing runs on its own.
- A build without the key, a release without a signature, and a signature that does not verify all fail
  closed: no download, a plain card, and the release page link. The signed checksum names the ZIP, whose name
  carries the version, so an older signed checksum served for a newer release is refused. The ZIP itself carries no
  build-provenance attestation; the files in it do.

Hosts, the exact request and the failure cards are in [`SETUP.md`](SETUP.md#network-use).

## Discord status

Built for v0.0.3 (#2259; maintainer decision 2026-11-08): Discord Rich Presence, as PPSSPP offers it,
opt-in and off by default.

- **Settings > Online and developer** has a **Discord status** switch, off by default
  (`discord_presence` in `settings.json`; anything but `true` reads as off).
- **No network connection.** The player writes to the Discord app on the same PC through its local
  named pipe (`discord-ipc-0` to `discord-ipc-9`; `src/player/discord_presence.c`, over
  `nk_platform_local_pipe_*`). The protocol is written from Discord's public RPC documentation: a
  framed JSON handshake with the application ID, then `SET_ACTIVITY`. No Discord SDK is used or
  vendored.
- **What Discord receives:** the game's title from the disc's own metadata (the disc ID when it
  names no title), the play mode (Authentic, Enhanced or Ace) and the start time, plus the player's
  process ID, which the protocol requires. Never a path, save, user name or anything from the
  game's memory. Discord shows these on the user's profile; that is the disclosure the switch
  asks about.
- **Quiet failure.** If Discord isn't running, the player tries each pipe once, at most once a
  minute, and logs nothing. A write gives up after 250 ms and no read waits, so the launcher never
  stalls. The status clears when the game exits and when the player closes.
- **Application ID.** The project's Discord application ID is one constant,
  `NK_DISCORD_APPLICATION_ID` in `src/player/discord_presence.h`. It is public, not a secret. An
  empty or malformed value fails closed: the build cannot show Discord status, and the player never
  opens the pipe.
- **Which builds show it.** Windows builds with an application ID. Other builds (no application ID,
  or Linux and macOS, which have no local pipes here) have no Discord status row; one whose stored
  setting is On still shows the row, saying the build can't show it, so it can be turned off.
- **Tests** (`tests/native/test_discord_presence.c`) drive the client against a fake transport and,
  on Windows, a fake pipe server with a test-only name; they never reach Discord.

## RetroAchievements

Built for v0.1.0 (#1316): an opt-in client using the MIT-licensed rcheevos client, in Casual Mode only
(the mode RetroAchievements formerly called Softcore). Hardcore is not offered.

- **Settings > Online and developer options** has an **Achievements** switch, off by default. Turning it on shows a consent
  screen that names `retroachievements.org` and `media.retroachievements.org` and lists every data
  field. The player's requests go to `https://retroachievements.org/dorequest.php` over the existing
  player HTTPS transport; the achievement badge pictures come from `media.retroachievements.org`, and
  no other host is used. Turning the switch off stops the requests: one waiting its turn is answered
  as refused and never sent, nothing new is sent, and only the request already on its way finishes
  (a badge picture's answer is then thrown away).
- **Badge pictures.** Once the switch is on and you are signed in, the player asks for the badge of
  each achievement in the loaded set, one at a time, from the address rcheevos itself builds for it:
  `https://media.retroachievements.org/Badge/<badge>.png`, or `<badge>_lock.png` for the grey picture
  of a locked one. The request is a plain HTTPS GET with certificate validation, through the same
  transport, carrying only the user agent below: no user name, token, cookie or console ID. The
  badge name comes from the set the server sent and is used only when it is one to seven letters and
  digits; an address the server suggests for a picture is not used. Each badge is asked for at most
  once while its game's page lists it; a game opened later in the same session asks again only for
  pictures the folder below does not hold. A picture larger than 64 KB, wider or taller than 128
  pixels, or not a PNG is
  refused by name, and an answer that does not come is not an error: the row (or the in-game notice)
  shows the player's own drawn icon, a trophy for an unlocked achievement and a padlock for a locked
  one, and the log says once per run that a badge could not be shown. Pictures are decoded with the
  vendored stb_image, its size checked before any pixel memory is taken.
- **Badge cache.** Fetched pictures are kept in `achievements-badges` under the player data folder,
  at most 2,048 files, each named from the badge name only: `b`, the name with a mark before each
  capital letter, `-lock` for the grey picture, then `.png` (so no file takes a Windows device name
  such as `CON` or `COM1`, and names that differ only in case keep separate files). A picture is kept
  only once it decodes, written to a temporary `.part` file, put on the disk and then renamed; a kept
  picture is read from there instead of asked for again, and one that no longer decodes is deleted and
  asked for again. A `.part` file a crash left is deleted when the player next starts RetroAchievements.
  **Sign out** deletes the folder (the pictures say which games were played). Turning the switch off
  fetches nothing more and leaves the folder as it is: the next **Sign out** deletes it, a factory
  reset moves it aside with the rest of the data folder, and it can be deleted by hand at any time.
  The folder is not tied to an account: when the token in Windows Credential Manager is replaced or
  removed outside the player, without **Sign out**, the pictures stay until the next **Sign out**.
  They are RetroAchievements' public artwork, the same for every account, and hold no user data. The
  in-game notice reads the unlocked picture from that folder before the frame is drawn; the runtime
  itself opens no connection.
- **User agent.** Each request names the player in the form RetroAchievements documents for clients:
  `NakagawaRecomp/<numeric version> (<operating system and version>) rcheevos/<version>`, for example
  `NakagawaRecomp/0.1.0 (Windows 10.0.26100) rcheevos/12.5`. A version that is not numeric, or a
  system part that would break the form, means no request is sent.
- **Sign-in.** The player sends the RetroAchievements user name and password once to exchange them for a
  token, clears the password from its temporary buffers and from rcheevos' request buffer once the request
  is handed over, and stores only the token in Windows Credential
  Manager. Sign-out removes that credential. Other platforms without the Windows credential store refuse
  sign-in rather than saving a token in a plain file.
- **What is sent**, as form fields of RetroAchievements' `dorequest.php` API:
  - sign-in (`r=login2`): the user name with the password once, or with the stored token;
  - the game's achievement set (`r=achievementsets`): user name, token and the game hash;
  - session start (`r=startsession`): user name, token, the game's RetroAchievements ID, the game hash,
    `h=0` (Casual Mode) and the rcheevos version;
  - an unlock (`r=awardachievement`): user name, token, the achievement ID, `h=0`, the game hash, the
    seconds since a queued unlock happened and rcheevos' request signature;
  - Rich Presence (`r=ping`), every two minutes while a game runs: user name, token, the game ID, the
    game's Rich Presence text (what its achievement set says you are doing), `h=0` and the game hash;
    none is sent once the game has exited or its frames have stopped.

  Leaderboard entries are not sent: rcheevos submits them only in Hardcore, or in Casual Mode when an
  emulator allows it, which the bridge never does, and the bridge also refuses that request. The hash
  follows RetroAchievements' PSP method: raw bytes from `PSP_GAME/PARAM.SFO`, then
  `PSP_GAME/SYSDIR/EBOOT.BIN`, with no new decryption. No game file, save, package, guest memory,
  telemetry or capture is sent.
- **Offline unlocks** are stored as owner-only, token-free records in the player's data folder. After the
  next sign-in, the player submits each pending unlock through rcheevos' award request builder and the
  same transport. That includes an unlock the switch stopped from being sent (one being sent or
  waiting for another try when it went off, or earned in the frame it went off, since frames stop
  with it), which goes once after the next sign-in in the same run (rcheevos itself gives such a
  request up), and one left from an earlier run. The queue holds at most 4,096 records. With no
  definite answer (no connection, a busy server) a record waits 30 seconds; a record the server refuses
  by its own error leaves the queue and is reported; a refused account keeps every record until the next
  sign-in. Each unlock is sent once in a session: one the game earns again while its saved record is
  on its way, or after the server awarded that record, gets the server's answer to the record instead
  of a second request, and a saved record waits while the game's own request for it is on its way.
  The game's title page shows its core achievements under **RetroAchievements · Casual**:
  how many are unlocked and the points earned of the total, with a bar, then one row each, the
  unlocked first, each with its badge (or the drawn icon), title, description and its points as a
  number (up to 256; with more, the page says how many it shows and that all of them are on the
  game's page at retroachievements.org). When the game's set has loaded, a notice **Achievements
  active** shows the game's title and how many of its achievements are unlocked ("3 of 20 unlocked"),
  since RetroAchievements asks clients to say when achievements become active. Unlock notices appear
  in-game, one after another:
  **Achievement unlocked** over the achievement's title, its points ("25 points") and its badge. Two
  unlocks close together are two notices. When the last achievement of the set unlocks, a notice
  **Set completed** follows with the game's title and "Every achievement unlocked"; it is drawn on the
  PC and sends nothing. The mode, Casual, is said once, in that page heading and in Settings, not on
  every notice or row.
- **One request at a time.** The player carries one RetroAchievements request at a time. A request made
  while another is on its way (a game started while a saved unlock is being sent) waits its turn, in
  order, instead of failing. When the player closes, the request on its way is ended at once instead
  of waiting out the connection's timeouts; an unlock it carried stays in the queue for the next run.
- **After a factory reset** the player writes no unlock queue before it closes, so the moved-aside data
  folder is not made again. The token stays in Windows Credential Manager until **Sign out**.
- **The queue belongs to one account.** It names that account by a tag, the MD5 of a fixed label and the
  account name in lower case: the name itself and the token are not in the file. Only that account's
  session sends the records. **Sign out** discards what is still waiting, also when it comes while a
  sign-in is on its way or the switch is turned off before that sign-in answers; when the token cannot be
  removed from Credential Manager, the account is still the stored one and its records stay. Records
  another account left (a different account signs in, or the credential was changed outside the player)
  are discarded and reported, never awarded to the account now signed in.
- **The queue file** is `achievements/unlock-queue.bin` under the player's data folder, written through a
  private temporary file and a rename. Integers are little-endian: the 8 bytes `NKRAQ001`, a 32-bit
  format version (2), a 32-bit record count, the owner's 32-character tag (thirty-two `0` when the queue is
  empty), then 44-byte records: 32 characters of game hash, a 32-bit achievement ID and a 64-bit unlock
  time in seconds since 1970 UTC. A file the player cannot read (damaged, cut short, from another
  version) is renamed beside itself with the ending `.unreadable`, never loaded and never written over;
  when it cannot even be renamed, nothing is saved over it in that run, which the **RetroAchievements**
  account line says.
- **RetroAchievements' data.** The achievement set the server sends is RetroAchievements' data. Its
  titles, descriptions and badges are shown; its trigger logic (the memory addresses and conditions
  that decide each unlock) belongs to the RetroAchievements community, whose terms forbid reusing or
  redistributing it. The player keeps the set only in rcheevos' memory while the game runs, and wipes
  the server's answer once rcheevos has read it. The set is never written to a file, a log, the
  developer log, a problem report, a save backup, a package or anything the player exports. An
  achievement turned off because its memory cannot be read is reported in the player's own words,
  never with the address from the set. The only RetroAchievements files on the disk are the badge
  pictures and the queue file above (game hash, achievement ID and time), both in the player's data
  folder. Save backups and folder copies take a game's saves only, a report link carries no file or log,
  and packages are built from a game's own files, so none of them includes these.
- **Sign-in at start** uses the stored token. When the server gives no answer (no connection, a 5xx, 408 or
  429), the token was not refused: the player tries it again after 30 seconds, then after twice as long
  each time, up to ten minutes, with no password asked. A refusal by the server ends that, as do the
  switch turned off (also while a sign-in is on its way: the status stays "RetroAchievements is off.")
  and **Sign out**. A password is never kept, so a password sign-in is not retried.
- **Frames.** For a game started with achievements on, the runtime and the player share the guest memory
  through a local channel, and at each VBLANK the runtime waits while the achievements are evaluated once
  on that frame's memory. A thread of the player's own serves this, so a modal box or a window being
  dragged, which stop the player's event loop, do not stop the game. A runtime that is told to use a
  channel it cannot open ends at once with `GUEST_MEMORY_CHANNEL_UNAVAILABLE` and exit status 2. A
  game whose window is up without having opened the channel (a package built before it, or without it)
  never sends a frame: the player stops the channel and says the package must be rebuilt to track
  achievements. When the game's achievement set cannot be loaded, the player stops serving frames, so
  the runtime's next VBLANK stops waiting for it; the shared memory stays mapped until the game exits.
- **UI regression runs** refuse the switch, **Sign in** and **Sign out**, and say so: they stay offline,
  open no consent or credential box and read no credential.
- **Hardcore is unavailable.** The bridge turns rcheevos' hardcore mode off before every sign-in, game
  load and frame, and refuses to send any request that claims it (`h=` other than `0`, or a leaderboard
  entry); neither the player nor the runtime has a setting, option or variable that turns it on.
- **Off with cheats or a changed speed.** A game started with cheats on (saved, or `--cheats` for the
  session) or with a package that changes its game logic's speed (60 FPS game logic) loads no achievement
  set and evaluates no frame. The game's page says why before it starts, and Settings says so while cheats
  are on.
- **Frames read the game's own memory.** The achievements are evaluated once per guest VBLANK, on the
  game's clock, over rcheevos' PSP memory map: RetroAchievements addresses `0x00000000`-`0x03FFFFFF` are
  guest `0x08000000`-`0x0BFFFFFF` (kernel, user and extended RAM).
- **No telemetry.** Nothing else about the player, the PC or play is sent.
- **Tests** use rcheevos' server callback with canned responses and never touch the internet. The
  egress allowlist permits the player's existing transport as the caller, while the bridge and vendored
  rcheevos library hold no network implementation of their own.

## News

- The library shows a small news strip from `https://nakagawa.jaycast.net/news.json` (title, date, one-line summary,
  link), fetched at most every few hours and cached for offline display.
- Text only: no remote images, HTML or scripts are rendered. **Read** opens the post in the browser.

## Reports

Built for v0.0.2: browser links, with no network call from the player.

- **Report this game** is on the card of a game that stopped early, closed unexpectedly or did not start; each
  game's title page has a smaller **Report a problem**. Either opens the user's browser at a new discussion in
  the [Compatibility reports](https://github.com/Jstar269/nakagawa-recomp/discussions/categories/compatibility-reports)
  category, with the title ("game title (disc ID): boundary or crash code") and the body filled in: the disc ID
  and version, the Nakagawa version and commit, the system, the named boundary or crash code, and empty "What I
  did" and "What I saw" sections. The link carries no path, user name, log or setting, is percent-encoded and
  stays under 2000 characters (Windows' shell takes links up to 2083). Nothing is posted until the user posts it
  on GitHub.
- **See existing reports** opens the same category searched for the disc ID.
- **Known causes.** When the named boundary is in the player's table of known causes (`k_known_causes` in
  `src/player/player_state.c`), the card says "This is a known issue", links that issue, and shows Report as a
  smaller button. Only open issues are listed.
- **Copy details** puts a longer report on the clipboard: the boundary, the last boot events, the renderer and
  the package check. The profile folder is shown as `~` and the user name as `<user>`.
- If a `.github/DISCUSSION_TEMPLATE/compatibility-reports.yml` form is added later, its fields are filled by
  their ids instead of `body`.

The plan below is the later in-player report, once online mode ships.

- A **Report** action on a game's title page and on every error card composes a report inside the player: player
  version and build date, operating system, disc ID and title, the furthest stage reached and the named boundary
  (for example `unsupported-import-sceUmdReplaceProhibit`), and the user's own comment.
- The user sees the exact text before sending, can edit the comment and remove any field, and chooses **Send** or
  **Cancel**. With developer mode on, the user may also attach the developer log, shown in the same preview.
- Until the report endpoint exists, **Send** opens the browser on a pre-filled form (forum or issue tracker) instead
  of posting from the player. The endpoint, when built, rate-limits and stores reports without IP logging.
- Never included: game files, saves, memory, screenshots, file paths outside the redaction below.

## Developer mode

- Adds a **Developer log** panel (toggle with F12 or the controller's Select + R): the player's and the runtime's
  log lines, boot events, the named stop boundary and the first missing imports, with copy and save buttons.
- Local only: developer mode sends nothing. Saving or attaching the log replaces the user's profile path and user
  name with placeholders (`%USERPROFILE%`, `<user>`) before display.
- Release builds keep the panel; it adds no performance cost while hidden.

## Online play

Status: design approved by the owner on 2026-11-06
([#1139](https://github.com/Jstar269/nakagawa-recomp/issues/1139)). The relay's wire format is built
(`src/core/nk_adhoc_relay.c`, tested by `relay-codec-selftest`), and so are the transport seam and its in-process
medium (`src/core/nk_adhoc_net.c`, tested by `adhoc-net-selftest`). Nothing connects yet: the PSP ad hoc services
are still named refusals.

A separate switch, **Online play**, lets games use the PSP's ad hoc multiplayer over the internet. It is not part of
online mode: online mode lets the launcher talk to one host, while online play lets game code reach other consoles,
so it is its own explicit choice with its own sentence in Settings. It is off by default.

- **Ad hoc multiplayer through a relay.** Most PSP multiplayer is local wireless ad hoc play. Nakagawa carries it
  through an ad hoc relay server, the way PPSSPP does. Nakagawa speaks the protocol of
  [aemu_postoffice](https://github.com/Kethen/aemu_postoffice) at commit `02abf18f`, the version PPSSPP pins, so
  Nakagawa players can play with PPSSPP players, and with PSPs running the aemu plugin, on the same servers.
- **Servers.** The player ships one server, `socom.cc`, which is PPSSPP's default. The user may enter their own
  instead. A server the user enters is used only on their PC and never ships.
- **Two connections, both unencrypted.** The protocol is plain TCP. It has no encryption and no sign-in.
  - **Lobby** (port 27312): log in, find groups, join one, and learn who is in it.
  - **Data relay** (port 27313): one connection per ad hoc socket, carrying the game's own packets.

  Anyone on the network path can read this traffic. The consent card and `docs/SETUP.md` say so plainly.
- **What is sent, and nothing else:**
  - to the lobby: the disc ID (for example `ULUS10000`), a nickname (the console profile's own, which the user can
    change), the console profile's MAC address and the group name. The profile's MAC is a random address Nakagawa
    made for that profile, never the PC's.
  - to the data relay: the game's own ad hoc packets, addressed by that MAC and the game's ports.

  No game data, saves, packages, guest memory, telemetry or identifier of the PC is sent. The chat message the
  protocol allows is never sent.
- **Where it lives.**
  - **PSP services:** `sceNetAdhoc`, `sceNetAdhocctl` and `sceNetAdhocMatching` are firmware modules, so they are
    HLE (`AGENTS.md`, section 6). They become real state machines in `src/rt/hle_network.c`. Their PSP-visible
    results come from the public PSP recordings in pspautotests (`tests/net/adhoc`) and from measurements on the
    project's PSP. A call stays a named refusal until its sequence is measured.
  - **Transport:** the services talk to their peers through one small transport interface
    (`src/core/nk_adhoc_net.h`): the lobby and PDP sockets now, PTP streams later. Its first implementation is an
    in-process medium whose lobby is translated from PPSSPP's ad hoc server, which speaks the same lobby protocol.
    One console alone on it is a PSP with no peers in range, the default with Online play off. Two to four consoles
    on it are a simulation for tests, labelled as such. The relay is the second implementation.
  - **The relay client** is one module in the game process (`src/core/nk_adhoc_relay*`), because a game's
    datagrams cannot wait for a trip through the launcher. The player starts a game with the server only when the
    user turned Online play on. That module is the only runtime file the egress allowlist test lets use the network.
- **Where the code comes from** (`src/core/PROVENANCE.md` has the details).
  - The relay codec (`src/core/nk_adhoc_relay.c/.h`) encodes and decodes the protocol's packets; no
    aemu_postoffice code is copied. The packet layouts and limits come from aemu_postoffice's headers. Its login
    checks (PPSSPP's `login_user_data`) and port offset are translated from PPSSPP, so `nk_adhoc_relay.c` is
    labelled derived-translated.
  - The in-process medium's lobby (`src/core/nk_adhoc_net.c`) is translated from PPSSPP's ad hoc server
    (`Core/HLE/proAdhocServer.cpp`, GPL-2.0-or-later) and labelled derived-translated. Its PDP half is this
    project's own.
  - Importing aemu_postoffice's client code later under `AGENTS.md` section 4 is approved by the owner. That code is
    marked GPL-3.0-only, and its `PROVENANCE.md` entry would record that.
- **Infrastructure revivals, per title, later.** A game whose official servers are gone can be revived only by
  rebuilding that game's own server protocol, one title at a time, from interoperability study of the game itself.
- **Never:** anything that needs the console vendor's online-account sign-in or DNAS authentication. Those depend on
  the vendor's keys and certificates, which stay out of the project like firmware and decryption.
- **Still fail-closed where unbuilt.** A network service a title calls that Nakagawa does not implement stays a named,
  visible stop whatever this switch says. No setting makes a refusal permissive.
- **Before the first connection ships:**
  - a `docs/SETUP.md` "Network use" row names the server, both ports and what is sent;
  - the egress allowlist test in `tools/test_prereq_fetcher.py` gains the relay module and the shipped server;
  - the relay's tests use a local fake server only.
- **Hardware evidence:** ad hoc behaviour is measured on PSP hardware (two consoles where the test needs a peer)
  before it is claimed, as for every other runtime behaviour.

## Testing online features

- **The default test suite and hosted CI stay offline.** Online code is tested against a stub server on the loopback
  interface with fixed signed fixtures (a valid update notice, a tampered one, a malformed feed, a timeout). Ad hoc
  play is tested against an in-process fake relay and a two-console loopback inside one process, with no host
  socket.
- **One labelled online smoke check may use the internet,** opt-in only: it runs when the maintainer starts it or on a
  schedule the maintainer sets, talks only to the project's own host, checks the live update notice's signature,
  fetches the news feed and joins the relay. It is never part of the default suite; without network it reports
  `NOT_RUN`, not a failure.
- When the first online feature ships, the `AGENTS.md` section 5 sentence "Tests never touch the internet" becomes:
  "The default test suite and hosted CI never touch the internet; the labelled online smoke check may, opt-in, and
  only against the project's own host."

## Policy and code changes this needs

1. `AGENTS.md`, section 5: offline by default becomes "offline until the user turns on online mode; then only
   news and user-sent reports, only to `nakagawa.jaycast.net`". The update check already has its own entry there.
2. Until the first network feature ships, `network_mode` in `settings.json` stays `"offline"` (the only value
   `AGENTS.md` section 5 accepts) and the user's choice lives in a separate `online_mode` key, with the per-feature
   switches (`online_news`, `online_reports`) and `developer_mode` beside it. The update check has its own key,
   `check_updates_at_start`, outside online mode. `network_mode` gains
   `"online"` when step 2 ships, together with the section 5 change in item 1; any unknown value still reads as
   offline.
3. `docs/SETUP.md` "Network use" gains one row per feature when each ships; the egress allowlist test
   (`tools/test_prereq_fetcher.py`) gains the one host and the one module allowed to use it.
4. Tests never touch the internet: a local stub server and fixed signed fixtures cover the client.

## Maintainer tasks

- DNS and hosting for `nakagawa.jaycast.net`, serving `updates.json`, `news.json` and a privacy page.
- Generating and keeping the update-signing key offline and signing each release's checksum file; committing
  its public key as `assets/update_signing_key.pub` ([`RELEASE_SIGNING.md`](RELEASE_SIGNING.md)).

## Implementation order

1. Settings fields, the wizard step and the developer log panel (no network). **Built.**
   - The settings keys are as item 2 below describes.
   - The switches live on Settings > ONLINE & DEVELOPER and on the wizard step "Online and developer options"
     (right after Welcome), both saying that news and reports aren't built yet. CONTINUE is the step's
     default focus, so Enter never flips a switch; SKIP turns both off.
   - The developer log (F12, or Select + R on a controller, with developer mode on) holds the player's errors,
     launches, setting changes, the game's exit code and the runtime's boot events (phases, the named stop boundary,
     missing imports), redacted as they are added. COPY puts it on the clipboard; SAVE writes `developer-log.txt` in
     the logs folder. Tests: `tests/native/test_player_state.c` (redaction, the ring, the panel, the step) and
     `tests/native/test_player_ui.py`.
2. A small HTTPS client behind one host allowlist (WinHTTP on Windows, which the build-tool download already uses;
   a portable backend on Linux), with the stub-server tests.
3. Update check. **Built** as its own opt-in feature on GitHub's Releases API, with a SHA-256 check of the
   download ([Update check](#update-check)); tests: `tests/native/test_update_check.c` and
   `tests/native/test_player_ui.py`, on recorded and synthetic answers, never the internet.
   - **Built:** signature verification before any SHA-256 is trusted. Each release's checksum file is signed
     offline with stock `minisign` ([`RELEASE_SIGNING.md`](RELEASE_SIGNING.md)); the player checks the
     `.minisig` with `nk_minisign_verify` (`src/core/nk_sig.h`: minisign's prehashed `ED` and legacy `Ed`
     forms, the key ID and the trusted comment's global signature), on `nk_ed25519_verify` and the vendored
     Monocypher 4.0.3 Ed25519 and BLAKE2b code (`third_party/monocypher/`). The public key is read at build
     time from `assets/update_signing_key.pub`; until the owner commits that file, the build carries no key and
     offers only the release page. Tests: `ed25519-verify-selftest` (the RFC 8032 vectors and real minisign
     output, TEST-ONLY key) and the update tests above (TEST-ONLY keys derived at run time).
4. News strip.
5. Reports, browser hand-off first, then the endpoint.
6. Online play: ad-hoc relay first, then per-title infrastructure revivals.

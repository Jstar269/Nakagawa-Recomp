# Online mode and developer mode (design)

Status: step 1 of the implementation order is built (the switches, the wizard step and the developer log panel).
The update check is built too, as its own opt-in feature outside online mode (see
[Update check](#update-check)), and so is [Discord status](#discord-status), which makes no
network connection; news and reports are not. The player is offline unless the user asks
([`SETUP.md`](SETUP.md#network-use)). This page is the contract the implementation follows. The launcher gets an update check, a reporting system and a news feed, all behind an
**online mode** that the setup wizard asks about, and a **developer mode** that shows a developer log for
diagnosing new issues.

## Principles

- **Asked once, changeable anytime.** The setup wizard step offers the switches for online mode and developer
  mode, and each is a switch in Settings afterwards. Both start switched off. Until the online features exist
  the Online mode switch is not shown to anyone whose stored setting is Off (see the note under the table).
- **One host.** Online mode talks to `nakagawa.jaycast.net` over HTTPS and nothing else. The news site (The
  FreshWire) cross-posts to it, so the player's allowlist stays one host. (The update check, outside online mode,
  uses GitHub's hosts for the releases themselves.)
- **Nothing is sent on its own.** No telemetry, analytics, crash upload or identifier. The only automatic traffic is
  fetching the news feed while online mode is on, and the one update check at start when the user turned it on. Requests carry no cookie and no ID; the
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

Built for v0.1.0 (#2259; maintainer decision 2026-11-08): Discord Rich Presence, as PPSSPP offers it,
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
    checks and port offset are translated from PPSSPP, so `nk_adhoc_relay.c` is labelled derived-translated.
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

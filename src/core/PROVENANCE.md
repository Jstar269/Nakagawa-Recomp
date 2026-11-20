# PROVENANCE.md — `src/core/nk_psp_*` (built-in PSP game decryption) and `src/core/nk_adhoc_*` (ad hoc transport)

The decryption boundary that turns the encrypted executable and modules of a game the user owns into plain ELF files,
locally, from the user's own disc image (issue #295). It was added in PRs #548 and #550, removed as a temporary measure
in PR #1187, and restored with built-in key tables after the maintainer's decision of 2026-10-27 to decrypt the way
PPSSPP does. Firmware is never decrypted: updater PSAR archives, the IPL, the Media Engine image, system-menu files and
encrypted `flash0` kernel and VSH modules are refused by name before any decryption step runs (`AGENTS.md` section 5).

Every derived file below is labelled `derived-translated` in the trusted provenance record
(`docs/provenance/INDEPENDENCE_MODEL.md`); none is "independent". The project-authored files are listed last.

## Built-in key tables (`LOCAL DELTA`)

- Files: `nk_psp_builtin_keys.c`, `nk_psp_builtin_keys.h`.
- Upstream: PPSSPP, `https://github.com/hrydgard/ppsspp`, revision `b4386554795949491a26e51b7d74082bb9b2470f`.
  - `Core/ELF/PrxDecrypter.cpp` (SHA-256 `3ab45acda6b80752ce0c5aaef1abc3a80903beb2d9dfc59b477cfe8793660133`),
    Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later: the tag key arrays and the `g_tagInfo`/`g_tagInfo2` recipes.
  - `ext/libkirk/kirk_engine.c` (SHA-256 `cfd70093c3e6572b9862dc61cd99ebea117eb92feb3e19a9cd393f7a1af0598c`),
    KIRK-ENGINE by Draan and contributors, GPL-3.0-or-later: `kirk1_key`, the curve-1 ECDSA parameters and public
    point, and the keyvault rows the recipes use.
  - `ext/libkirk/amctrl.c` (SHA-256 `ec416616fcf8facf305e4969376ab15a933a4290915b5913d8ceef1cf0450c91`), amctrl.prx
    reverse engineering by tpu, no licence notice of its own (PPSSPP ships it under GPL-2.0-or-later beside
    KIRK-ENGINE): the `loc_1CD4`, `loc_1CE4` and `loc_1CF4` constants, as KeyStore values `amctrl.loc_1CD4/1CE4/1CF4`,
    and the keyvault rows 0x38, 0x39 and 0x63 (`kirk.keyvault.56/57/99`) the DRM BB MAC and cipher use (added
    2026-10-27 for PSN UMD images, phase 2).
- Licence: the file is GPL-3.0-or-later, which both upstream licences permit; both copyright notices are kept in its
  header.
- LOCAL DELTA: the values and array names are unchanged. The file is C data in one table; a tag that appears in both
  upstream tables (a 144-byte and a 16-byte recipe with the same code) is one entry; the 144-byte keys keep upstream's
  32-bit words and the KeyStore serialises them little-endian, as upstream's byte view does on the PSP. Left out:
  - every firmware container tag: `0x0B000000` (updater executable), `0x06000000` and `0x0E000000` (updater DATA.PSAR
    blocks), `0x0F000000` and `0x862648D1` (Media Engine image), `0x07000000` and the eighteen `sceResmgr_9DC14891`
    index.dat tags (`0x0B2B05F0` to `0x0B2B92F0`, `0x495BE403`). They are listed in `nk_psp_firmware_tags` only, so the
    boundary can refuse them by name; their keys are not carried. `0x06000000` (the 1.00 updater) and `0x0B2B10F0` (the
    3.60 index.dat) are the two tags PPSSPP added after the pin, in `fb371af6bb7d13406fdfe7f23b6bbad6c47629a0` and `6a9b84a714e2e03470dcfebb223c8c4688e61e99`; a review on 2026-11-05 of
    the five upstream commits to `Core/ELF/PrxDecrypter.*` and `ext/libkirk` between the pin and
    `5b3ddcc0628585b7b091c9a9904dba58bb4118c8` found no new or changed game key, tag recipe or cipher fix missing here;
  - every `flash0` firmware-module tag, removed on 2026-11-17: the kernel-module families `0x4C94xxF0` (with
    `key_2DA8`), `0xCFEFxxF0` and `0x16D59E03`, and the VSH-module families `0x457BxxF0`, `0x3802xxF0` (with
    `key_380210F0`, `key_380280F0` and `key_380283F0`), `0x76202403` and `0x0F037303`, with their key arrays and the
    keyvault row `0x62` that only they name. They are listed in `nk_psp_firmware_tags` with the firmware container
    tags, so an encrypted module copied from a console's `flash0` is refused by name before any key lookup;
  - the type-3 DRM BB patch tag `0x89742B04`, the SPRX decrypt-mode pad `xor_91E0A9AD` and the KIRK command-2 results,
    which need the installing console's context (PBP/NPDRM);
  - amctrl's fixed-key tables (`key_357C`, `key_363C`, `dnas_key1A90`, `dnas_key1AA0`), which only PGD data and
    `sceNpDrmGetFixedKey` use;
  - every KIRK value no game recipe uses: curve 2, the signing key `kirk16_key`, and the keyvault rows no included
    recipe names (among them `0x4E`, the updater's, and `0x52`, the Media Engine image's).
- Regenerating: the table is a mechanical extraction of the two pinned files under the rules above. A refresh pins a new
  revision here, records both SHA-256 values, and keeps the exclusions.

## Container and KIRK engine (`LOCAL DELTA`, restored from PR #548)

- Upstream: John-K/pspdecrypt, `https://github.com/John-K/pspdecrypt`, revision
  `c156627db7634d395c380c0a9589130f603307fc`, `LICENSE.TXT` GNU GPL version 3 (no later-version grant). Its
  `PrxDecrypter.cpp` descends from PPSSPP's (GPL-2.0-or-later).
- Files and their upstream paths:
  - `nk_psp_kirk.c/.h` from `libkirk/kirk_engine.c/.h` (KIRK-ENGINE, GPL-3.0-or-later per its own notice);
  - `nk_psp_aes.c/.h` from `libkirk/AES.c/.h` (public-domain Rijndael notice kept), GPL-3.0-only;
  - `nk_psp_sha1.c/.h` from `libkirk/SHA1.c/.h` (public-domain SHA-1 notice kept), GPL-3.0-only;
  - `nk_psp_kle.c/.h` from `kl4e.c/.h`, GPL-3.0-only;
  - `nk_psp_prx.c/.h` from `PrxDecrypter.cpp/.h`, GPL-3.0-only.
- LOCAL DELTA: portable C99; every embedded key, tag table and PSAR constant removed (recipes come from the KeyStore,
  which the built-in tables above fill); KIRK operations thread a context so a missing entry fails closed by name. Each
  file header lists its own changes. The 2026-10-27 restoration changed only `nk_psp_prx.c`'s missing-recipe message.
- LOCAL DELTA, 2026-11-18: `nk_psp_kle.c` takes its probability contexts from the output offset and the
  `copyCountBitsProbs` index instead of the low bits of host pointers, and drops a pointer comparison that could never
  be true; `nk_psp_aes.c`'s `AES_cbc_decrypt` returns at once for an empty run; `nk_psp_kirk.c`'s `kirk4`/`kirk7`
  refuse an empty pass. In `nk_psp_kirk.c/.h` the CMD1 wrapping key is read from the caller's context for each
  command instead of a process-wide AES schedule, and CMD1, CMD4, CMD7 and CMD11 refuse an output size smaller than
  what they write. Each file header records its change.
- Licence note: a GPL-3.0-only file can be combined with the project's GPL-3.0-or-later files; the player and
  `nk_decrypt`, which link them, are conveyed under the GNU GPL version 3 (`NOTICE.md`).

## PSN UMD images (NPUMDIMG, `LOCAL DELTA`, phase 2)

A PSN release of a UMD game is an `EBOOT.PBP` whose `DATA.PSAR` is an NPUMDIMG image: the game's ISO in blocks,
encrypted with the DRM BB cipher and compressed with LZRC. These files turn it back into that ISO the way PPSSPP reads
it; the phase-1 disc route then runs unchanged.

- Upstream: PPSSPP, `https://github.com/hrydgard/ppsspp`, revision `b4386554795949491a26e51b7d74082bb9b2470f`.
- `nk_psp_amctrl.c/.h` from `ext/libkirk/amctrl.c` (SHA-256
  `ec416616fcf8facf305e4969376ab15a933a4290915b5913d8ceef1cf0450c91`) and `ext/libkirk/amctrl.h` (SHA-256
  `c2f4d9f24d86f9a6f5157de03a01707efee20d29d89edd68dbd2edbea2886d90`), "written by tpu". The upstream files carry no
  licence notice; PPSSPP ships them under its GPL-2.0-or-later licence beside KIRK-ENGINE (GPL-3.0-or-later), and the
  port is GPL-3.0-or-later. LOCAL DELTA: the MAC (type 3) and cipher (type 1, decrypt) subset only
  (`sceDrmBBMacInit/Update/Final`, `bbmac_getkey`, `sceDrmBBCipherInit/Update/Final`); the KIRK passes go through
  the engine's `kirk4`/`kirk7` and the constants come from the KeyStore, so a missing one fails closed by name; the
  fuse-ID types (MAC type 2, cipher type 2, KIRK commands 5 and 8) fail `CONTEXT_REQUIRED` instead of running with a
  default fuse ID; local bounded buffers; a cipher run that is not a multiple of 16 derives its last keystream block in
  full; a MAC update that leaves only the previous full pad block chains it instead of dropping it; since 2026-11-18
  the cipher's counter base is derived once per `sceDrmBBCipherInit` instead of once per 2 KiB chunk. Not ported:
  `sceDrmBBMacFinal2`, `sceNpDrmGetFixedKey`, the PGD functions and the encrypt direction.
- `nk_psp_lzrc.c/.h` from `Core/FileSystems/tlzrc.cpp` (SHA-256
  `ccf084593f0de93154597d47a470a7b539d4ca0cfa4773fddfd91c9a9e8a026a`), "based on benhur's code, rewrite by tpu",
  Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later, kept GPL-2.0-or-later with its notice. LOCAL DELTA: C99 with
  `size_t` lengths bounded at 64 MiB; a match distance decoded in 64 bits; a match copy and a literal stop at the first
  byte that does not fit; a literal-context shift past the byte width selects context 0; a stored stream whose
  declared size passes the output or its input fails instead of being truncated (2026-11-18).
- `nk_psp_npumdimg.c/.h` from `Core/FileSystems/BlockDevices.cpp` (SHA-256
  `d29e6d39dce987f2f88183bb0fe15112b1cba4fc09507be7a822e7bb4fb01029`, `NPDRMDemoBlockDevice`) and the `table_info`
  layout in `BlockDevices.h` (SHA-256 `8253d1c6f18b17d9c6023e59067e9e5a22b997b7703b67deb401f785f4a00fe5`),
  GPL-2.0-or-later, conveyed as GPL-3.0-or-later. LOCAL DELTA: a read-at callback instead of the FileLoader; PPSSPP's
  bounds checks kept and tightened (the whole table and every block inside the file, a 4 GiB image limit, a block a
  packaging tool marked or a short last block fails instead of being taken as read); the whole image is written to a
  temporary file and renamed into place with progress and cancel. Project-authored in the same file: the PBP
  identification, the PSOne Classic (`PSONE_CLASSIC_UNSUPPORTED`) and firmware refusals before any key lookup, and the
  ISO9660/`PARAM.SFO` check of the result (ECMA-119 and the PBP/SFO layouts).

## Restoration source

Every file except the built-in tables comes from this repository's own public history: the tree before PR #1187, at
commit `8ca38e6399064e1d76f9670ed4722d521fd21a9a^`. No code, table or key value came from any other tool.

- Byte-identical to those bytes when restored: `nk_psp_aes.c/.h`, `nk_psp_sha1.c/.h`, `nk_psp_kirk.c/.h`,
  `nk_psp_kle.c/.h`, `nk_psp_ec.c/.h`, `nk_psp_inflate.c/.h` and `nk_psp_prx.h`. Later changes are dated in each file's
  header and in this manifest (above for the ports, below for the project-authored files). The AES and SHA-1 ports
  keep their upstream layout and stay excluded from the trailing-whitespace hook (`.pre-commit-config.yaml`), as
  before PR #1187.
- Changed on restoration (each file's header or this manifest says how): `nk_psp_prx.c`, `nk_psp_container.c/.h`,
  `nk_psp_crypto.c/.h`, `nk_psp_keystore.c/.h`, `nk_decrypt_main.c`, `tools/nk_core/decrypt_tool.py` and
  `tools/test_psp_decrypt.py`.

## Project-authored files

`nk_psp_container.c/.h` (framing, decompression dispatch, the firmware refusal), `nk_psp_keystore.c/.h` (built-in and
key-file loading, no PSAR entry name), `nk_psp_crypto.c/.h` (the error contract and result names), `nk_psp_ec.c/.h`,
`nk_psp_inflate.c/.h` and `nk_decrypt_main.c` (the `nk_decrypt` command line, with its `npumdimg` subcommand) are
project-authored from the format rules
and the standards their headers cite (FIPS 197, RFC 4493, FIPS 180, RFC 1950-1952, SEC 1). Since 2026-11-18,
`nk_psp_ec.c`'s `point_on_curve` refuses a coordinate that is not below the field prime (SEC 1 3.2.2.1), and in
`nk_psp_inflate.c` a failed raw retry of a zlib-looking stream reports into no buffer, so the zlib attempt's message
stays with the code it returns.

## Cheat files (`nk_cheats.*`, issue #2639)

The CWCheat reader, the load-time check and the applier. The CWCheat format has no specification; PPSSPP's
implementation is the public reference for it, and both files are translated from it.

- Upstream: PPSSPP, `https://github.com/hrydgard/ppsspp`, revision `c437463bc5cb78a0236312ad23f9c3b56fb9ae67`, GPL-2.0-or-later:
  `Core/CwCheat.cpp` (SHA-256 `9f542a111ade55dab0fd82f2a2db0646dbb23a53ebeb23e47b52651945cb65e3`, blob
  `0831a83f9fbabb3f900b546defe6589d7418b866`) and `Core/CwCheat.h` (SHA-256
  `a18d57f3d9fd5f0691be8a6f543e9217e43e7d24b04dbf26b7e87bab32231415`, blob
  `4d56915980a00590afb37e25c477949e49e680d7`). The symbols are `CheatFileParser::ParseLine` and
  `CheatFileParser::ParseDataLine` (the `_S`, `_G`, `_C` and `_L` lines), the `CheatOp` and `CheatOperation`
  types, `CWCheatEngine::InterpretNextCwCheat` (the fields of each code form), `CWCheatEngine::ExecuteOp` with
  `ApplyMemoryOperator`, `TestIf` and `TestIfAddr` (what a form does), and `CWCheatEngine::GetAddress` in
  `Core/CwCheat.h` (the `0x08800000` base and the `0x3FFFFFFF` mask).
- Notice: neither upstream file carries a copyright or licence line of its own at that revision. The two files
  here carry the notice PPSSPP's source files carry, `Copyright (c) 2012- PPSSPP Project.`, copied from the
  first line of `Core/Core.cpp` at the same revision (blob `fabfe45556560ec8e0b3c2c233629d249efdbbcc`); that
  file's notice and PPSSPP's `LICENSE.TXT` (blob `1c3210d9d1eaf47cc33a1e0c451f4704a765852d`) give the licence,
  GPL version 2.0 or later. Both files are conveyed as GPL-3.0-or-later.
- What follows PPSSPP and what does not. No line of PPSSPP's code was copied: the files are C99 over a host
  interface (`NkCheatHost`) where PPSSPP is C++ over its memory map, JIT cache and controller state. A reading
  of `nk_cheats.c` beside `Core/CwCheat.cpp` on 2026-11-16 found these parts following the upstream's
  structure closely, and the file header names them: the operation kinds of `OpKind`, in the order of PPSSPP's
  `CheatOp`; the operation record `CheatOp`, one per code as PPSSPP's `CheatOperation`, with a multi-write's
  `count`, `step` and `add` fields under the same names; `check_code`, whose switch over a code's top four bits
  and whose bit fields for each form are those of `InterpretNextCwCheat`; `run_cheat`, with one case per kind as
  `ExecuteOp` (the multi-write loop, the copy, the pointer walk and write, the tests that skip lines);
  `code_address` and `user_address` (`GetAddress`); and the line types and trailing-comment rule of
  `parse_text`. This project's own, with no counterpart in PPSSPP: the text reader's code, the load-time check
  that compiles a whole cheat to its operation list once and resolves each test's line count to the operation it
  continues at, the refusal of a write into translated code (PPSSPP writes and invalidates its JIT cache), the
  bound on what one frame can write, the whole-or-nothing apply with its undo journal, and every refusal by name
  where PPSSPP picks a meaning or skips.
- Evidence class: PPSSPP-derived, not PSP-measured (`AGENTS.md` section 7). Both files say so, and a run says
  so once, at the first VBLANK that applies cheats (`src/rt/cheats.c`).

| Path (under `src/`) | Lineage | Classification | PPSSPP pin | Local modifications | Notice lines in the file header | Commits here |
| --- | --- | --- | --- | --- | --- | --- |
| `core/nk_cheats.c` | PPSSPP `Core/CwCheat.cpp` (`CheatFileParser::ParseLine`, `CheatFileParser::ParseDataLine`, the `CheatOp` and `CheatOperation` types, `CWCheatEngine::InterpretNextCwCheat`, `CWCheatEngine::ExecuteOp` with `ApplyMemoryOperator`, `TestIf` and `TestIfAddr`) and `Core/CwCheat.h` (`CWCheatEngine::GetAddress`); the load-time check, the code-write refusal and the undo journal: this project | derived-translated (ledger: upstream_derived, record `nk_cheats_ppsspp_2639_20261116`); the load-time check, the code-write refusal and the whole-or-nothing apply are this project's own | `c437463bc5cb78a0236312ad23f9c3b56fb9ae67` | C99 over a host interface (`NkCheatHost`) instead of PPSSPP's memory map, JIT cache and controller state; a whole cheat is checked and compiled to an operation list when the file is read, where PPSSPP decodes each code every time it runs; a write into translated code is refused, at load and before every write, where PPSSPP writes and invalidates its JIT cache; a cheat applies whole on a frame or not at all (an undo journal, holds reported once per cause); comparisons are unsigned, and the 32-bit orderings of two addresses, which PPSSPP compares as signed, are refused; a section applies only when its `_S` disc ID is the run's; refused by name where PPSSPP picks a meaning or skips: a value wider than its form (PPSSPP widens a type 0x0 write), bits set where a form has zeros, a count of 0, an overlapping copy, a 0x6 base offset and its 0x1 and 0x9 lines, a test that guards no line, runs past the end or ends inside a multi-line code, an address outside user memory, types 0x9, 0xA, 0xB, 0xC and 0xF, and `_M` and `_N` lines (PPSSPP runs PSP Action Replay codes). The file header lists them. | SPDX GPL-3.0-or-later; PPSSPP copyright line (2012-, the form of `Core/Core.cpp`, since `Core/CwCheat.cpp` has none); Nakagawa Recomp authors copyright; derivation line naming the repository, revision, files and symbols; derived-translated label beside "PPSSPP-derived, not PSP-measured"; "conveyed as GPL-3.0-or-later" line; the note on where the PPSSPP line was taken from; modification list dated 2026-11-16, naming the parts that follow PPSSPP closely and the parts that are this project's own | 2 |
| `core/nk_cheats.h` | PPSSPP `Core/CwCheat.cpp` (`CheatFileParser::ParseLine`, `CWCheatEngine::InterpretNextCwCheat`, `CWCheatEngine::ExecuteOp`) and `Core/CwCheat.h` (`CWCheatEngine::GetAddress`) for the line types and code forms its comment lists and for `NK_CHEATS_ADDRESS_BASE`; the declarations: this project | derived-translated (ledger: upstream_derived, record `nk_cheats_ppsspp_2639_20261116`); the declarations are this project's own | `c437463bc5cb78a0236312ad23f9c3b56fb9ae67` | the interface of `nk_cheats.c` (`NkCheatHost`, `NkCheatConfig`, `NkCheatInfo`, `NkCheatSummary`, the `nk_cheats_*` functions and the limits), which has no counterpart in PPSSPP; the table of code forms is narrowed to the forms that run here, and the list of refusals is this project's | SPDX GPL-3.0-or-later; PPSSPP copyright line (2012-, the form of `Core/Core.cpp`); Nakagawa Recomp authors copyright; derivation line naming the repository, revision, files and symbols; derived-translated label beside "PPSSPP-derived, not PSP-measured"; "conveyed as GPL-3.0-or-later" line; the note on where the PPSSPP line was taken from; modification note dated 2026-11-16 | 2 |

The runtime's side, `src/rt/cheats.c` and `src/rt/cheats.h`, is project-authored (`src/rt/PROVENANCE.md`). The
tests are project-authored on synthetic guests, with made-up disc IDs and addresses: `tests/native/test_nk_cheats.c`,
a harness in `tests/native/test_fuzz_parsers.c` and cases in `tests/native/hle_thread_selftest.c`; what they
expect of each code form follows PPSSPP's reading of it.

## Ad hoc transport (`nk_adhoc_*`, issue #1139)

The transport seam of the PSP's ad hoc services, its in-process loopback medium, the relay wire
codec, and the opt-in plain-TCP relay client. Only `nk_adhoc_relay_client.c` opens host sockets
(and only when the user opts in via **Online play** / `SR_ADHOC_ENABLED=1`).

- Upstream for the medium's lobby and relay transport: PPSSPP, `https://github.com/hrydgard/ppsspp`, revision
  `3bc60270eae3069f13dfbfdb7292a7e1375de6f6` (the revision `src/rt/PROVENANCE.md` pins for the SAS envelope and
  CREDITS.md for texture replacement), `Core/HLE/proAdhocServer.cpp` (SHA-256
  `aa9f7546c881f5b7b47fafbfe2ffd14e5612d49bdc236617e24055ee29907525`, blob
  `bd32875a2fc00fde1529a91f7c9291439e8cf5a8`), Copyright (c) 2014- PPSSPP Project, GPL-2.0-or-later. That file is
  PPSSPP's port of Coldbird's aemu ad hoc server. Also used, at the same revision: `Core/HLE/sceNetAdhoc.cpp`
  (SHA-256 `36b4f0c4b9aaa1241ac10292eff4252e7f29413510ca4d50ea540a705f7594a3`, blob
  `4ec5b5bc43e9878d20326b853ae054fbc88fe606`, Copyright (c) 2013- PPSSPP Project) for the port offset,
  `Core/HLE/proAdhoc.h` (SHA-256 `58d08305844211df95c7fb99183d86c16b620d01d79bd540297b82af253523ea`, blob
  `26f574ef1b34c7787a56bbf129117e8d269291b2`) for the lobby packet layouts, and
  `Core/HLE/AdhocTransportRelay.cpp` (Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later) for
  `nk_adhoc_relay_client.c`'s listener backlog and connect state machine (`RelayBacklog`,
  `fillRelayBacklog`, `closeRelayBacklog`, `RelayTransport::PtpConnect`,
  `RelayTransport::PtpConnectPoll`).
- Protocol: Kethen/aemu_postoffice, `https://github.com/Kethen/aemu_postoffice`, commit
  `02abf18fd161a1639d9f93c48c4c46638c7c6d15`, the commit PPSSPP `3bc60270` pins. The relay codec takes from it facts
  only, as interoperation needs them: the packet layouts, opcode and init-type numbers and size limits in
  `aemu_postoffice_packets.h` (marked GPL-3.0-only) and `adhocctl/packets_v1.h` and `adhocctl/pspstructs.h` (PRO ONLINE
  notice, GPL version 3 or later), the two ports and the connection order in `design.md`, and the rule that a PDP
  or PTP length over its limit ends the session, which its client `client/postoffice.c` (marked GPL-3.0-only) also
  applies. No code is copied: a comparison on 2026-11-12 of the new files against every aemu_postoffice text file
  outside `ext/` and PPSSPP's ad hoc files found no copied line. The owner accepted aemu_postoffice's licence for
  imports on #1139 (2026-11-06); nothing is imported from it.
- Rewritten on 2026-11-12, in three rounds. The 2026-11-06 version followed the aemu_postoffice adhocctl server
  (`adhocctl/server_cpp/server.cpp` and `client.cpp`, marked GPL-3.0-only). The first round rewrote the lobby of
  `nk_adhoc_net.c` from PPSSPP, but two functions were carried over from the 2026-11-06 version with small edits
  rather than written again: `lo_login` (its duplicate-MAC loop and field copies) and `nk_relay_product_ok`, and
  with them two of aemu_postoffice's rules and terms: a nickname of 128 bytes refused, and "leader MAC" for the
  group's BSSID. An independent review of pull request #2549 found this. In the second round `lo_login`,
  `nk_relay_mac_ok`, `nk_relay_nickname_ok` and the checks in `nk_lobby_login` were deleted and written again from
  PPSSPP's `login_user_data` alone; the nickname rule is now PPSSPP's (refused only when its first byte is zero, so
  all 128 bytes of the field may be text, with no terminator), and the field is named the group's BSSID. That round
  left `nk_relay_product_ok`'s body as it was (only its comments changed), so an earlier head of the pull request
  still carried the 2026-11-06 body; a second review found this. In the third round that body was deleted and
  written again from `login_user_data`'s product code check (`Core/HLE/proAdhocServer.cpp:561-569` at `3bc60270`).
  All the login checks, the product-code check included, are now written from PPSSPP's `login_user_data`; nothing
  in them is from aemu_postoffice. The first round's author states that its session did not open aemu_postoffice's
  source; the second round's session opened neither aemu_postoffice's source nor the code it replaced. The third
  round's session opened no aemu_postoffice source; it had read the old product-check body earlier, while checking
  this record's wording, and wrote the new one from `login_user_data` without opening the old one again.

| Path (under `src/`) | Lineage | Classification | PPSSPP pin | Local modifications | Notice lines in the file header | Commits here |
| --- | --- | --- | --- | --- | --- | --- |
| `core/nk_adhoc_net.c` | lobby: PPSSPP `Core/HLE/proAdhocServer.cpp` (`login_user_data`, `logout_user`, `connect_user`, `disconnect_user`, `send_scan_results`, the opcode rules of `server_loop`); PDP half & loopback PTP streams: this project | derived-translated (ledger: upstream_derived, record `nk_adhoc_net_lobby_ppsspp_1139_20261112`); PDP and PTP loopback parts are this project's own | `3bc60270eae3069f13dfbfdb7292a7e1375de6f6` | C99 over fixed arrays (members newest first, groups by creation number, as PPSSPP's head-linked lists); a server send is an event on the receiver's queue, and a full queue drops and counts it as PPSSPP logs a failed send; peer id is the console index plus one instead of an IP; BSSIDs are numbered (`02:00` then the group count) instead of drawn at random; a MAC already logged in is refused; the login checks take C strings, so a product code must end after nine bytes and a nickname within 128 (inside the field the rules are PPSSPP's), and a refused login answers `NK_NET_BAD_ARG`; a login empties the event queue and sets both drop counts to zero; group numbers are 64-bit and a number whose BSSID (its low 32 bits) a live group holds is skipped; a request PPSSPP answers with a logout (a login while logged in included) logs the console out and answers `NK_NET_NOT_LOGGED_IN`; product-code overrides, chat, status log and timeouts left out; extended under #1139 with `nk_adhoc_pump`, `nk_adhoc_pdp_peek`, and loopback `nk_adhoc_ptp_*` stream callbacks. `nk_adhoc_drop_report` is this project's own. | SPDX GPL-3.0-or-later; PPSSPP copyright line (2014-); Nakagawa Recomp authors copyright; derivation line naming the file, revision and functions; derived-translated label; "conveyed as GPL-3.0-or-later" line; modification list dated 2026-11-12; the note that `lo_login` was rewritten after review | 7 |
| `core/nk_adhoc_net.h` | this project; its comment describes the lobby rules of PPSSPP's server | project-authored | N/A | the transport interface (`NkAdhocTransportOps`: lobby, PDP, PTP listeners/streams), result codes, the medium's test trace and the drop report | SPDX GPL-3.0-or-later; Nakagawa Recomp authors copyright | 7 |
| `core/nk_adhoc_relay.c` | this project, encoding the aemu_postoffice wire format above; `nk_relay_product_ok`, `nk_relay_mac_ok`, `nk_relay_nickname_ok` and the checks in `nk_lobby_login` from PPSSPP `proAdhocServer.cpp` `login_user_data` (all of them written from it on 2026-11-12, after review; nothing in them is from aemu_postoffice); `nk_relay_port_out`/`nk_relay_port_in` from PPSSPP `sceNetAdhoc.cpp` `offset_port_simple`/`reverse_port_simple`; the one shipped server preset, `socom.cc`, is PPSSPP's default server (`Core/Config.cpp`; first entry of `assets/adhoc-servers.json`) | derived-translated (ledger: upstream_derived, record `nk_adhoc_relay_codec_1139_20261112`); the rest of the codec, preset table, and `nk_adhoc_relay_parse_server` are this project's own | `3bc60270eae3069f13dfbfdb7292a7e1375de6f6` | C99; the checks take C strings, so a product code must end after nine bytes and a nickname within 128, the fields' sizes (inside the field the rules are PPSSPP's: nine characters A-Z or 0-9, a MAC neither all ones nor all zero, a nickname refused only when its first byte is zero); the offset passed in rather than read from settings, and a relay port of 0 coming back as 0; `nk_relay_nickname_field_ok` checks a raw 128-byte field; `nk_adhoc_server_preset*` and strict `nk_adhoc_relay_parse_server`. The file header lists them. | SPDX GPL-3.0-or-later; PPSSPP copyright lines (2013-, 2014-); Nakagawa Recomp authors copyright; derivation and "conveyed as GPL-3.0-or-later" lines; modification list dated 2026-11-12; the note that all the login checks were written again from `login_user_data` after review, `nk_relay_product_ok` last | 8 |
| `core/nk_adhoc_relay.h` | this project; its header lists what each part was written from | project-authored | N/A | wire structs/constants, `NK_ADHOC_DEFAULT_SERVER`, preset API and `nk_adhoc_relay_parse_server` | SPDX GPL-3.0-or-later; Nakagawa Recomp authors copyright | 6 |
| `core/nk_adhoc_relay_client.c` | C99 non-blocking transport over `nk_adhoc_relay.h` and `nk_adhoc_net.h`; eager listener backlog up to `max_backlog` (`RelayBacklog`, `fillRelayBacklog`, `closeRelayBacklog`) and three-way connect state (`RelayTransport::PtpConnect` and `RelayTransport::PtpConnectPoll`, with the refused result they report, `SCE_NET_ADHOC_ERROR_CONNECTION_REFUSED`, which this file keeps as its own `PTP_STATE_REFUSED`) adapted from PPSSPP `Core/HLE/AdhocTransportRelay.cpp`; zero `aemu_postoffice` source used | derived-translated (conveyed as GPL-3.0-or-later; pending maintainer ledger re-attestation from `adhoc_relay_client_1139_20261116` to `upstream_derived`) | `3bc60270eae3069f13dfbfdb7292a7e1375de6f6` (`Core/HLE/AdhocTransportRelay.cpp`) | C99 non-blocking Winsock2/`poll(2)` TCP lobby (`27312`) & data (`27313`) relay client implementing `NkAdhocTransportOps`, bounded 512 KiB TX/RX channel buffers, dead-lobby detection, single-lookup async DNS worker thread with `AI_NUMERICHOST` IP literal fast path, and PTP slot reservation before frame consume | SPDX GPL-3.0-or-later; PPSSPP copyright (2012-); Nakagawa Recomp authors copyright; derivation line naming PPSSPP `Core/HLE/AdhocTransportRelay.cpp`, revision and symbols; `derived-translated` and `conveyed as GPL-3.0-or-later` lines; modification list dated 2026-11-16 | 2 |
| `core/nk_adhoc_relay_client.h` | this project | project-authored (admitted under #1139) | N/A | `NkAdhocRelayClient`, `NkAdhocRelayConfig`, `nk_relay_client_*` declarations | SPDX GPL-3.0-or-later; Nakagawa Recomp authors copyright | 2 |

The tests `tests/native/test_nk_adhoc_net.c`, `tests/native/test_nk_adhoc_relay.c`, and `tests/native/test_nk_adhoc_relay_client.c` are project-authored with
synthetic MACs, names, product codes, and `127.0.0.1` loopback test sockets (never touching the internet); their expected lobby/PTP sequences follow the relay specification and PPSSPP's server (with `test_nk_adhoc_relay_client.c`'s backlog and refused-connect cases testing the `Core/HLE/AdhocTransportRelay.cpp` behavior adapted in `nk_adhoc_relay_client.c` at `3bc60270`).

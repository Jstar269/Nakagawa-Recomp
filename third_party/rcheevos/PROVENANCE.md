# PROVENANCE.md — `third_party/rcheevos/`

rcheevos, RetroAchievements' own C library, vendored for opt-in RetroAchievements support
([#1316](https://github.com/Jstar269/nakagawa-recomp/issues/1316)). The project wrapper is
`src/core/nk_achievements.c`, which hands rc_client a bounded read of guest memory through the PSP memory map and
passes its requests to a caller-supplied transport only for `https://retroachievements.org/`. The native selftest
`tests/native/test_nk_achievements.c` (`achievements-selftest`, also run by `native-core-tests`) tests it against an
in-process fake server. The vendored files contain no network code: rc_client builds each request and hands it to the
host's callback, so nothing here opens a connection.

## Upstream source

- Project: rcheevos
- Repository: `https://github.com/RetroAchievements/rcheevos`
- Pin: release tag `v12.5.0`, commit `1433173220a7eaede6a9ed7a18e94117be1821e0` (published 2026-09-14, the newest
  release at the time of import).
- License: MIT (`third_party/rcheevos/LICENSE`, "Copyright (c) 2018 RetroAchievements.org"), kept beside the sources unchanged. MIT is
  compatible with GPL-3.0-or-later. `src/rhash/md5.c` and `md5.h` are L. Peter Deutsch's MD5 ("Copyright (C)
  1999, 2000, 2002 Aladdin Enterprises"), under the zlib licence in their own headers, which are kept unchanged;
  rcheevos uses it to sign unlock requests. The zlib licence is compatible with GPL-3.0-or-later.
- Import method: each file was read from a clone of the repository with `git show <commit>:<path>`. Its Git blob ID
  matches the pinned commit's tree, and the SHA-256 values below were calculated from the local bytes. `src/rc_client.c`,
  five `src/rapi/` files, `src/rcheevos/alloc.c`, `src/rcheevos/richpresence.c` and `src/rcheevos/runtime_progress.c`
  have since been changed (Local deltas below); their rows keep the upstream blob ID.
- Only what rc_client needs to sign in, load a game's achievement set by its hash, evaluate it and submit unlocks is
  vendored, in the upstream layout so every `#include` resolves without an edit: the public headers, `rc_client`, the
  `rapi` request builders and parsers, the `rcheevos` runtime, and `rhash`'s MD5. Not vendored: the rest of `rhash`
  (disc hashing; the hash is computed elsewhere), the editor API (`rc_api_editor`), RAIntegration's sources and the
  external-client implementation, the libretro helpers, the tests and the build files.
- Four vendored files have no runtime role here (corrected 2026-11-17: this paragraph said the validator and the
  external-client shim were not vendored). `src/rcheevos/rc_validate.c` and `rc_validate.h` came with the `rcheevos`
  runtime folder; nothing in the vendored tree, `src/` or `tests/` calls an `rc_validate_*` function (`rc_internal.h`
  only declares `rc_validate_memrefs_for_console`), but the achievements selftest's
  `$(wildcard third_party/rcheevos/src/rcheevos/*.c)` (`mk/native_ui_tests.mk`) still compiles `rc_validate.c`.
  `src/rc_client_external.h` is included only under `#ifdef RC_CLIENT_SUPPORTS_EXTERNAL` (`rc_client_internal.h`),
  which no build defines, and `src/rc_client_external_versions.h` is included by no vendored file. They are kept so
  the upstream layout stays whole; removing them would be a maintainer admission removal.

## File manifest

| File | Upstream path | Kind | Upstream v12.5.0 blob SHA | SHA-256 |
| --- | --- | --- | --- | --- |
| `LICENSE` | `LICENSE` | imported byte-identical | `5f1faf3dc4e00f66418bd1f67fb5b55f8d932faa` | `bf820c034966a3b6be050bb85f94ec841a0b972bd5d477e362337d0574b5c219` |
| `include/rc_api_info.h` | `include/rc_api_info.h` | imported byte-identical | `1894cce1231b723185130fbb6cad311d0492014c` | `5c553b7dd109e854b3d3bfbecb9fb69b56cf922c25f88d04d9fe9bb162410f2a` |
| `include/rc_api_request.h` | `include/rc_api_request.h` | imported byte-identical | `ddccc7bc28194600405008979eb8ad6ea49ced86` | `2d27109deb502ed447cb821815a04c30bec37b90b1016f805960efc056a2d9bc` |
| `include/rc_api_runtime.h` | `include/rc_api_runtime.h` | imported byte-identical | `8a0de01b7f2a3016a49c3f6a9641527fedfe4419` | `32688333ba3923695fc568acf5580805158302de85da2fc078e590ed550409f8` |
| `include/rc_api_user.h` | `include/rc_api_user.h` | imported byte-identical | `f604188aa85568814fb5b39c4bb1bdb04352a19d` | `3268cdfded32a43e78844d9e07f79f3a8be3a97a9fdfdb6a17458a566df9761f` |
| `include/rc_client.h` | `include/rc_client.h` | imported byte-identical | `dd1538c0774826c50083c15d006ae0f21297f557` | `7176b338d8a80eee3be9a188887eaf9d375eb24b19f0efedfbe6b51e3504811a` |
| `include/rc_consoles.h` | `include/rc_consoles.h` | imported byte-identical | `1625cf2174b4eed75bf46403a958aac4d45dd780` | `73c1178e501692a11fb702eb1d90d25a7c47ed1d02678c619bc087c9755b132b` |
| `include/rc_error.h` | `include/rc_error.h` | imported byte-identical | `197bcea7023dfada97ae83eaa984830b7b43b797` | `b8d8214fe7ed4c0bb86420f1bfc4c4cb63548907b2c1001fea2083a55554763a` |
| `include/rc_export.h` | `include/rc_export.h` | imported byte-identical | `9471acd480b2fa61a5f97ee1d93741e5fce45603` | `9683f979bb152781677ffd952a68fdddcb5d951eb227e96a2d1f7fd0feb3be62` |
| `include/rc_hash.h` | `include/rc_hash.h` | imported byte-identical | `be1d37812394d8ae9449cf135d531e5807a04139` | `b2eeb7eb928ed07094463eefd8111ecc69a2f8aeb04e63b23172dcf3663a271b` |
| `include/rc_runtime.h` | `include/rc_runtime.h` | imported byte-identical | `88b12dea27d77c46e39931f7b0843c2c57ec5c61` | `a3c5e73681844036bc9d343e82fbc0d79c8bb6b6c584044b200515ac025a3063` |
| `include/rc_runtime_types.h` | `include/rc_runtime_types.h` | imported byte-identical | `e17797063c0de25515c072c74b2d18b21ddad146` | `f7d63f647773e4dc251ae8dc577d15a329093715c3c8b5172fb5b652d099c416` |
| `include/rc_util.h` | `include/rc_util.h` | imported byte-identical | `18d27e0fc789f19a19920061502e397151e87449` | `12840744dcca64ffd2e5f0a1c92fca2e235236edd5a66d8787d1d26189254344` |
| `include/rcheevos.h` | `include/rcheevos.h` | imported byte-identical | `0d96002148b69653ba6d49c1b66c503a985a2f43` | `0264f2a35a7f79f144263384d5378976c4c1062fa7477a281ed6fe2d64eba760` |
| `src/rapi/rc_api_common.c` | `src/rapi/rc_api_common.c` | imported + LOCAL DELTA | `3b3528605b225e3d1dc14515b2dbc4190d4507f5` | `5bbc766bd0954edb4391b8fa954873e10f83b352503c3263f110fa0e1674a32a` (this tree's bytes) |
| `src/rapi/rc_api_common.h` | `src/rapi/rc_api_common.h` | imported + LOCAL DELTA | `06a7485da35a1121c703d9f460564cd8391c6db6` | `4d94c16793a44b53efda51a270a6f89c8410d1eb8712181f115243c40a6afb7a` (this tree's bytes) |
| `src/rapi/rc_api_info.c` | `src/rapi/rc_api_info.c` | imported + LOCAL DELTA | `327a6dff3fc8f775f0bf369205c11482e43f9638` | `f036a31dc0d8c0db8cea3ee78a3746a218369e7f10c3c2183751a0c72cf78bfc` (this tree's bytes) |
| `src/rapi/rc_api_runtime.c` | `src/rapi/rc_api_runtime.c` | imported + LOCAL DELTA | `261741d55aeebf4323b126f993dbf53483529c83` | `2580e58f67b20b020be0d2d29d5cf0e93383b930fe5d2b2613b103be14f569c7` (this tree's bytes) |
| `src/rapi/rc_api_user.c` | `src/rapi/rc_api_user.c` | imported + LOCAL DELTA | `6943c98c443d647070e637758184fc4c09bec499` | `f8816573734c4b35f8ad9c15636ed6630dd4b6fc4d2199da57aa638947ae8260` (this tree's bytes) |
| `src/rc_client.c` | `src/rc_client.c` | imported + LOCAL DELTA | `7a7399580ebe48fd92b84d3a198f51b6675a23fd` | `093f7159a99354bf57628798cc2801051e3a15c321fed55ddfa0a15683e717f9` (this tree's bytes) |
| `src/rc_client_external.h` | `src/rc_client_external.h` | imported byte-identical | `b3c0e5eba9bfa12f32cc54bf6150bf67d67b6da9` | `dbca667b2ec79cc74347d234085b7c9fb3bd931e22bc2d598fb0af9931edc3ba` |
| `src/rc_client_external_versions.h` | `src/rc_client_external_versions.h` | imported byte-identical | `90f6f689604498d8d9ec61f6126c6c13f7f7328d` | `47c3a1975907b4d77ae7f427257f15cead2a9eef95c32b6034ebd3c048a330f1` |
| `src/rc_client_internal.h` | `src/rc_client_internal.h` | imported byte-identical | `fad288d62aee1717d4062bb2661ef5b5c9075a5a` | `a7fa88dbad2cb390cf84a5b2697284c928ef74fd4438afa81e6e633cbd8aacd2` |
| `src/rc_compat.c` | `src/rc_compat.c` | imported byte-identical | `4658c18d08e0f2fc8542074d81070bfae1a9939e` | `13869045a2fc3b33c0161dafc9e37d870be66b6642c5c0917b896c7b11a599fd` |
| `src/rc_compat.h` | `src/rc_compat.h` | imported byte-identical | `3b788e2e771763f20fc4dbdc3f2786bd80fd1687` | `4a97999341f386ef1a3b1271b1696e69dd476b0b073a752c7868c5420d9dabc5` |
| `src/rc_util.c` | `src/rc_util.c` | imported byte-identical | `3efdf04da480707da81c0a5c34f6045bbbd5a350` | `8e7a44443c9062054ee97bb9c60148ea7f14116a42d525d8abe3835a0a5c9666` |
| `src/rc_version.c` | `src/rc_version.c` | imported byte-identical | `a29d0a5f522f6b474063056cfd926a04dd49aef8` | `210091300446cf20bcbfc8f80c747c543a5a99e143887cf6aff16ab5f5a245a3` |
| `src/rc_version.h` | `src/rc_version.h` | imported byte-identical | `a2c0544d929e060daf2410a36efab82c653ffb0e` | `99db64703fd642b32812efe3b6fefb4843a2f1da6352aed3cb0d89cefcaafe0d` |
| `src/rcheevos/alloc.c` | `src/rcheevos/alloc.c` | imported + LOCAL DELTA | `96fb8e382511670b8bcccb3197caf9260927ce49` | `476da4dcbcc209aca2fd1b43bc9824c2d74b9dab88830447dd1aa18cbd2cbed1` (this tree's bytes) |
| `src/rcheevos/condition.c` | `src/rcheevos/condition.c` | imported byte-identical | `eb57a1cba3ea6add393b6b557775f0786b6a235d` | `bf21adfc2be390f9ba5a85c4039b73645a866e4100139368541efdd654bafe90` |
| `src/rcheevos/condset.c` | `src/rcheevos/condset.c` | imported byte-identical | `c9f984bfffd8d6bb4d248c819689165c25b020a5` | `e619f132a9a2bd62bc9d246a8e7598682806f8446eb4b197c09fb2929f65859b` |
| `src/rcheevos/consoleinfo.c` | `src/rcheevos/consoleinfo.c` | imported byte-identical | `2acedfdec644c855c99f763f129e2649ac745ce3` | `8e2949ee957152c1989c97472634d4a12859feaaa8c89c205775434a734d006d` |
| `src/rcheevos/format.c` | `src/rcheevos/format.c` | imported byte-identical | `bfeadf44cf5415044bc4d018cd19d20a0679ff10` | `1deaa114504049b9f303b4a01087f1639ff7939e6729671b3677106e33f8c59d` |
| `src/rcheevos/lboard.c` | `src/rcheevos/lboard.c` | imported byte-identical | `84ff5460af5a72888b0f16150b33b69969a214f0` | `14d94ba718d3e694e4874bbcead3c32fb0141728ef4fe011646d266b9261249e` |
| `src/rcheevos/memref.c` | `src/rcheevos/memref.c` | imported byte-identical | `577c7e483336a32a367a6b03db75edfe87e14967` | `4a1f0a044efc4a1eb2da11f484a630ef8169b86e9b53a193d9439d35f5d9c801` |
| `src/rcheevos/operand.c` | `src/rcheevos/operand.c` | imported byte-identical | `5d9c81a5cfd76ba151939bc83d0e798b84b5bb6a` | `b787371db68a52bb17fe6a0199630287cfd0314e51362c93afaaa89592d6b4c8` |
| `src/rcheevos/rc_internal.h` | `src/rcheevos/rc_internal.h` | imported byte-identical | `51e2e234c9095fe49be755a0243050592f24f0b0` | `bdabc324f20606e6377e86b606d587d0ebe061f7413c2afce0a850b7274d4b8b` |
| `src/rcheevos/rc_validate.c` | `src/rcheevos/rc_validate.c` | imported byte-identical | `843c2ad30cf77f4aecbc7651eb74ddaf2a19e007` | `b669159309b67eecbfd9ddd2332cdc083e025f75bb743f6e5c972f7ee5cdee0d` |
| `src/rcheevos/rc_validate.h` | `src/rcheevos/rc_validate.h` | imported byte-identical | `cfda94075d36295b3a6cb78e4ad9f239eb507774` | `d3da5219ebf38d2c34008417b3415658997cb3b63ee26b859d4036fd3fff3191` |
| `src/rcheevos/richpresence.c` | `src/rcheevos/richpresence.c` | imported + LOCAL DELTA | `c6a3393e2a22f29c9119e27c5725e7eafdf4dc60` | `a753aef5eacd1ace621580bf6cb6d0477e033f8bbd3bcfed950714a7472375c1` (this tree's bytes) |
| `src/rcheevos/runtime.c` | `src/rcheevos/runtime.c` | imported byte-identical | `c1100b9af9a9c33a50ba75420b51704d36d6336d` | `5dd68234cecb9111646431bc785a07eaca3234e8794a9c25d0622fe1607a360f` |
| `src/rcheevos/runtime_progress.c` | `src/rcheevos/runtime_progress.c` | imported + LOCAL DELTA | `c87721eb4fc04df5a110400a923bf7dec567681c` | `fd333fc6b6192638476e13711909b02483433a4d4dbdbc455ecc9d976872556c` (this tree's bytes) |
| `src/rcheevos/trigger.c` | `src/rcheevos/trigger.c` | imported byte-identical | `063a14f5c92c8e924c34a6325adf5d69a5340737` | `fe38429dd6c0e18d2c27c1eb797c963d61f63130c6546a20e1f8d27ed8271ece` |
| `src/rcheevos/value.c` | `src/rcheevos/value.c` | imported byte-identical | `9a451c63c88d74cb6e540bc286712394f31fe41f` | `126a5e14182fa551c03c758a2066feeda17c6e255a0dcb21fc6895ed5371413b` |
| `src/rhash/md5.c` | `src/rhash/md5.c` | imported byte-identical | `f3a52056695d3b582c3738f5e839a38db0547cbd` | `fd38ee6a43316007d4aa816f23c24a8355c58df106b4c54e43ec44afc55df521` |
| `src/rhash/md5.h` | `src/rhash/md5.h` | imported byte-identical | `698c995d8f4e9d98766d568fe88863381f94d16f` | `8341e2a307d7cfaeb816b261d92ce87a07be62e3eb58c98d2c769e75a6e2b959` |
| `PROVENANCE.md` | n/a | project-authored manifest | n/a | n/a |

## Local deltas

Nine files are modified (corrected 2026-11-18: this line said six). The `src/rapi/` changes are listed after the
`src/rc_client.c` ones, then the `src/rcheevos/` ones.

### `src/rc_client.c`

`src/rc_client.c` was modified by Nakagawa Recomp contributors on 2026-11-17 (audit 2026-11-17). Its
upstream v12.5.0 bytes have SHA-256 `f3fcc544f082acef62ca32fdd8d05f43fed5f77e14484c344e8dca9402e0024f`; the three
changes below give the SHA-256 in the table. Each is marked `LOCAL DELTA` in the file, and
`tests/native/test_nk_achievements.c` (`achievements-selftest`) fails on the upstream bytes for each of them, with a
transport that answers later, as `NkAchTransport` allows. The first and third are the changes upstream made after
the pin, in commit `db8ca2e622f05b5c870c43db9a1d796036aebbb8` ("add null checks to rc_client functions", #570, in
`src/client/rc_client_load.c` and `src/client/rc_client_do_frame.c`, where upstream has since split
`rc_client.c`); a later pin that includes that commit drops them.

- `rc_client_start_session_callback` tests for an aborted load (`outstanding_requests < 0`) before it reports a
  server error. Upstream at the pin tests the error first. When a set reply names a game but no set, the load fails
  while the session request is still out; if that session reply then carries a server error,
  `rc_client_end_load_state` frees the abandoned load, and the error was reported on the freed load: its callback
  ran a second time and the load was freed again (a use after free and a double free). The selftest checks that
  the load fails exactly once.
- `rc_client_award_achievement_callback`: when the server awards another achievement than the one asked for, the
  error log's second `%u` is the requested achievement's ID. Upstream passes `error_message`, a pointer that is
  NULL on this path, so the log read a pointer as an unsigned int and showed 0. The selftest checks the logged
  text. Upstream's current source still has this line.
- `rc_client_award_achievement_callback`: the mastery check after an award with no achievement left moves into
  `rc_client_award_achievement_check_mastery`, which holds the state mutex and does nothing when no game is
  loaded. Upstream at the pin walked `client->game->subsets` without either, so an award whose answer arrived
  after the game was unloaded (a new load unloads it at once) read through a NULL `client->game`. The selftest
  delivers the award's answer after the next load has started.

### `src/rapi/rc_api_common.c`, `rc_api_common.h`, `rc_api_runtime.c`, `rc_api_info.c`

Modified by Nakagawa Recomp contributors on 2026-11-17 (audit 2026-11-17). Their upstream v12.5.0 SHA-256 values are
`972ab73888c2bf37efde05e3daca487cdaf1e47735abe0ada3d48a4a9cb8c128` (`rc_api_common.c`),
`f6c49b5c6ff67e1b3c28d108799a6c975d2a7340b399e7a92cf0dbbaea60267b` (`rc_api_common.h`),
`5ce6ea89d8f4bc4cfbf492ef2553323f7f0f043dd2e5a0a45588ca0661f44ae2` (`rc_api_runtime.c`) and
`d9a3707339b36052672eca0a0195487666a0866ff1c428cec69508bb5cef5f5c` (`rc_api_info.c`); the changes below give the
SHA-256 in the table. Each is marked `LOCAL DELTA` in the file, and `achievements-selftest` fails on the upstream
bytes for the first three. The first two and the caller changes are upstream commit
`8409479c855aeee5eee7d2420eac2f68b6120077` ("don't accept null when looking for required string in JSON response",
#577, in `src/api/`, where upstream has since moved `src/rapi/`), applied to the vendored files only (its
`rc_api_editor.c` hunk is in a file not vendored); a later pin that includes that commit drops them.

- `rc_json_get_required_string` refuses `null`. Upstream at the pin took a `null` value as a present required
  string and returned a NULL pointer, so a set whose achievement had `"MemAddr": null` loaded with a NULL
  definition, and activating the set passed it to `strlen` (a NULL dereference). The load now fails with the
  missing-field error. The selftest checks that such a load fails and names `MemAddr`.
- `rc_json_parse_field` refuses arrays and objects nested deeper than `RC_JSON_MAX_DEPTH` (32) as invalid JSON,
  counting the depth in `rc_json_iterator_t.depth` (`rc_api_common.h`). Upstream at the pin recursed once per `[`
  or `{` with no limit, so a reply of a million `[` overflowed the native stack. The selftest sends a login reply
  nested 2^20 deep and expects the login to fail.
- `rc_json_parse_server_response` treats a body of length 0 as empty whatever its pointer points at. Upstream
  reads `body[0]` without checking `body_length`, a read past the given length; a 504 whose zero-length body
  pointed at a non-NUL byte was parsed as an HTML error ("Invalid JSON") instead of reporting "Request has timed
  out.". Upstream commit `c0a3ebdd95c51e53ac0b70a9c1728936df178f59` (#553, below) makes the same test, replacing
  the `*body` test where this tree keeps both. The selftest checks the timeout message.
- The callers where `null` is a real answer read the field as optional, as #577 does: an achievement's `Author`
  and a subset's `Title` (`rc_api_runtime.c`), a leaderboard's `LBAuthor` and a game title's `ImageIcon`
  (`rc_api_info.c`). The selftest loads a set whose achievement author is `null`, and its synthetic set has a
  `null` subset title, so it fails without the `rc_api_runtime.c` change.

### `src/rapi/rc_api_runtime.c`, `rc_api_user.c`, `rc_api_info.c`: arrays that hold a non-object

Modified by Nakagawa Recomp contributors on 2026-11-17 (audit 2026-11-17), on top of the changes above. The upstream
v12.5.0 SHA-256 of `rc_api_user.c` is `2430718f9fce10d4aa9f887b3cafdb817c37164291983fc345c19dac8c2ccfe2`; the
other two files' upstream values are in the section above, and the table gives this tree's bytes. The change is
upstream commit `c0a3ebdd95c51e53ac0b70a9c1728936df178f59` ("return RC_INVALID_VALUE if expecting array of objects
and got array of primitives", #553), applied to the three vendored files it touches (its `rc_api_editor.c` hunk is
in a file not vendored, and its `rc_api_common.c` hunk is the body-length test above); each site is marked
`LOCAL DELTA`, and a later pin that includes that commit drops it.

- The parsers count every element of an array (`rc_json_get_required_array` returns `array_size`), allocate that
  many entries without zeroing them, and fill only the objects up to the first element that is not one. Upstream at
  the pin then returned `RC_OK`, so a set reply such as `"Sets":[1]` or `"Achievements":[{...},1]` left entries
  uninitialized that `rc_client_copy_achievements` read as pointers (title, badge, definition). Each loop now
  returns `RC_INVALID_VALUE` with the count cut to the entries filled: achievements, leaderboards and subsets
  (`rc_api_process_fetch_game_sets_server_response`, also the game-data parser), leaderboard `TopEntries`, the
  start-session `Unlocks` and `HardcoreUnlocks`, followed users, and in `rc_api_info.c` the achievement info,
  leaderboard info, games list and game titles parsers. `achievements-selftest` feeds each parser such an array and
  loads such a set through the bridge; all twelve checks fail on the previous bytes.

### `src/rcheevos/alloc.c`: memref lists larger than 16 bits

`src/rcheevos/alloc.c` was modified by Nakagawa Recomp contributors on 2026-11-17 (audit 2026-11-17). Its upstream
v12.5.0 bytes have SHA-256 `aac9433815af40f2ffaba468f251304bb446e27cf54087982ec80ad358a12829`; the table gives this
tree's. The change is marked `LOCAL DELTA` in the file.

- A memref list's `capacity` is a `uint16_t`. `rc_preparse_reserve_memrefs` sized the one new list to the next power
  of two of the memrefs still needed, so an achievement set that needed 32769 to 65536 new memrefs stored 65536 as 0.
  The parse then placed every memref in the trigger's buffer, past the size the preparse had measured: a heap
  overflow (AddressSanitizer reports it in `rc_alloc_memref`; the Windows selftest crashes with an access
  violation). `rc_preparse_array_size` now never returns more than `0xFFFF`, and the reservation adds lists, each
  linked at the end of the chain, until they hold every memref; the modified-memref lists get the same change.
  `achievements-selftest` loads a set whose one achievement watches 40000 distinct bytes through the bridge and
  unlocks it; it fails on the upstream bytes. Upstream fixed the same truncation after the pin by refusing more
  than `0xFFFF` memrefs (commit `6d897b44fe8e141eac90f7c2a32f1a08a8110a48`, #568, in `src/runtime/rc_alloc.c`).
- Not changed: the reservation still does not check its `calloc` and `malloc` results. Upstream's handling
  (commit `e3e7e86bc8ab10295dace443addfb091d1095a0e`, #559) makes every later allocation in the parse stop after
  an error, across nine files; a later pin that includes it supersedes this delta.

### `src/rcheevos/richpresence.c`: lookups built in order, unknown-macro text cut to its buffer

`src/rcheevos/richpresence.c` was modified by Nakagawa Recomp contributors on 2026-11-18 (audit 2026-11-17). Its
upstream v12.5.0 bytes have SHA-256 `06c940a6159c9fb39785b29eed204865150cef89431bd923d4f11465665a1144`; the table
gives this tree's. Both changes are upstream commit `cab7f7ff938a1e68cb07a6072745bb31197723bd` ("keep lookup tree
semi-balanced while building", #569, in `src/runtime/rc_richpresence.c`, where upstream has since moved the file),
applied by hand to the pinned file; each is marked `LOCAL DELTA`, and a later pin that includes that commit drops
them. `achievements-selftest` fails on the upstream bytes for each.

- `rc_parse_richpresence_lookup` keeps a lookup's entries in a sorted array, scanned from its end, and builds the
  balanced tree from it once at the end of the lookup. Upstream at the pin inserted each entry into a plain binary
  tree from its root and rebalanced it afterwards, so entries listed in value order, the usual case, built a list:
  quadratic time to parse, then a count and a flatten that recursed once per entry. The selftest loads a lookup of
  65536 sorted entries through the bridge and reads three of them and the default; on the upstream bytes the
  Windows selftest dies with a stack overflow (`0xC00000FD`). One difference from #569: the array doubles when
  full instead of growing by 64 entries, so a long lookup is not copied once per 64 entries; the size arithmetic
  is checked. Entries listed out of order still cost a scan and a shift each, as in #569.
- `rc_evaluate_richpresence_display` cuts an unknown macro's text to the 255 characters its 256-byte buffer holds.
  `snprintf` returns the untruncated length, and upstream at the pin used it as the copy length, so a caller
  buffer larger than 256 bytes (such as one passed to `rc_client_get_rich_presence_message`) received the stack
  bytes past the buffer and a length longer than the text. The selftest gives a 1 KiB buffer a 624-character
  unknown macro and checks that 255 characters and nothing more are written.
- Not changed: the scratch allocator `rc_buffer_reserve` (`src/rc_util.c`) still walks every chunk from the first
  on each allocation, and each chunk holds a few entries, so parsing a very long lookup stays quadratic there.

### `src/rcheevos/runtime_progress.c`: progress reads bounded to each chunk

`src/rcheevos/runtime_progress.c` was modified by Nakagawa Recomp contributors on 2026-11-18 (audit 2026-11-17). Its
upstream v12.5.0 bytes have SHA-256 `24de7caf7edf30fc54a65314c5046389be138d11ff122d20d70845fb778439ce`; the table
gives this tree's. The change is upstream commit `e9a6d580f01cbd4e299af967390aac3115c47ed5` ("runtime: bound progress
deserialization to each chunk", #554), applied unchanged with `git apply`; it is marked `LOCAL DELTA` in the file,
and a later pin that includes that commit drops it.

- `rc_runtime_deserialize_progress_sized` checks each chunk's size against the bytes left before it forms the chunk
  end, and every read (`rc_runtime_progress_read_uint`, the MD5 match) checks the buffer and the current chunk.
  Upstream at the pin added the size to the offset in 32 bits unchecked and read with no bounds, so a chunk size
  near `0xFFFFFFFF` wrapped the end back inside the state and the readers ran past it. #554 also skips a memref
  chunk when no memref is waiting to be matched. The bridge does not restore progress itself, but its callers
  reach `rc_client_deserialize_progress_sized` through `nk_ach_rc_client()`. The selftest restores a 29-byte state
  whose DONE chunk size wraps and whose checksum lies in the 15 bytes past the stated size: the upstream bytes
  accept it (`RC_OK`) by reading those bytes, this tree refuses it with `RC_INSUFFICIENT_BUFFER`.

The bridge builds rc_client without `RC_CLIENT_SUPPORTS_HASH`,
`RC_CLIENT_SUPPORTS_EXTERNAL` and `RC_CLIENT_SUPPORTS_RAINTEGRATION`, rcheevos' own defaults, so the files those
switches would pull in are not needed.

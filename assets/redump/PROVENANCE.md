# PROVENANCE.md — `assets/redump/`

Redump's list of PSP disc dumps, bundled so the disc check works with no setup. The player and the tools compare a
whole-image SHA-1, CRC32 and size with it ([`docs/COMPATIBILITY.md`](../../docs/COMPATIBILITY.md#whole-library)); a list
the user imports with `python tools/nk_cli.py redump-import` takes precedence over it. Nothing downloads this list at
run time: `tools/stage_runtime_dlls.py` copies this folder beside the player, and the code reads the file from there
(`src/core/nk_disc_verify.h`, `tools/nk_core/disc_verify.py`).

## Upstream source

- Project: Redump (<https://redump.info/>), the official home of the Redump project since 2026-06-20.
- Download: `https://redump.info/datfile/PSP`, linked from <https://redump.info/downloads> ("Sony PlayStation
  Portable", "Datfile"). Fetched once, anonymously, on 2026-11-12 at 18:43:40 UTC. The server named the file
  `Sony - PlayStation Portable - Datfile (3546) (2026-11-12 16-02-53).zip` (`Content-Type: application/zip`).
- What it holds: one file, `Sony - PlayStation Portable - Datfile (3546) (2026-11-12 16-02-53).dat` (1,475,779
  bytes, SHA-256 `0a1d9a3f1d4b776a16491cfd3f390865d133d49e5b740e4e2fbbca990b858590`), Logiqx XML. Its header names
  `Sony - PlayStation Portable`, version and date `2026-11-12 16-02-53`, author `redump.info`. It lists 3,546 games,
  each with one `<rom>` (name, size, CRC32, MD5, SHA-1); 3,541 of them carry a `<serial>` (the disc IDs, 3,907
  distinct once spellings such as `ULJM 06340, ULJM 06341` are read) and 3,542 a `<version>`. It names no licence.
- Terms: Redump's About page (<https://redump.info/about>, read 2026-11-12) says of its data: "This information is
  considered public domain to be used however people see fit - whether you are a researcher, academic, organization,
  or simply an individual verifying the data for your own media." No page found states a licence for the datfiles.
- Import method: the downloaded bytes, unchanged and not repacked, saved under a stable name so the code needs no
  change when the list is refreshed. The upstream file name, date and hash stay recorded here.

## File manifest

| File | Kind | Upstream | Fetched (UTC) | Size (bytes) | SHA-256 |
| --- | --- | --- | --- | --- | --- |
| `redump-psp.zip` | imported byte-identical | `https://redump.info/datfile/PSP` | 2026-11-12T18:43:40Z | 346932 | `7e4afb96fe3a02b3e43e4138b1ebba911bdb1d96c73fa61f294166bb14fbebaf` |
| `PROVENANCE.md` | project-authored manifest | n/a | n/a | n/a | n/a |

The same SHA-256 is the `redump-psp-datfile` pin in [`assets/release_manifest.json`](../release_manifest.json), and
`tools/test_disc_verify.py` checks that the file, this row and that pin agree.

## Refreshing the list

Redump updates the list often; a bundled copy is a snapshot. A user who wants a newer one downloads it from
`https://redump.info/datfile/PSP` and imports it, the zip as served or the `.dat` inside it:

```text
python tools/nk_cli.py redump-import "Sony - PlayStation Portable - Datfile (....).zip"
```

The import is kept in the user's data folder and is used instead of this file from then on. To refresh the bundled
copy itself, the maintainer replaces `redump-psp.zip` with a new download, unchanged, and updates in one change: the
row above and the facts in "Upstream source", the pin in `assets/release_manifest.json`, the dated counts in
`tests/native/test_disc_verify.c` and `tools/test_disc_verify.py`, and the date in `CREDITS.md`. New bytes under a
byte-identical row need the maintainer's approval
([byte-identical rows](../../docs/PUBLICATION_READINESS.md#byte-identical-manifest-rows)).

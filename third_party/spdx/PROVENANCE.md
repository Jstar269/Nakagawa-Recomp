# PROVENANCE.md — `third_party/spdx/`

The official SPDX 2.3 JSON schema, vendored so the SBOM tests check every generated SPDX document against it on
every host and CI lane. `tools/test_sbom_binding.py` validates the documents `tools/generate_sbom.py` writes with
the project's JSON Schema subset validator (`_validate_schema_value` in `tools/nk_cli.py`), which refuses any schema
keyword it does not enforce, and checks that this schema uses only enforced keywords. The schema is test data: no
build, package or runtime reads it.

## Upstream source

- Project: the SPDX specification (Software Package Data Exchange), the Linux Foundation and its contributors.
- Repository: `https://github.com/spdx/spdx-spec`
- Pin: tag `v2.3`, commit `aadf3b0b8dbbabdb4d880b0fc714255fea436ff7` (committed 2022-08-11, "Merge 2.3
  development branch to main"). The newer tag `v2.3.1` carries a different schema (JSON Schema 2019-09, titled
  "SPDX 2.3.1"); this directory keeps the 2.3 schema the generator's `spdxVersion` (`SPDX-2.3`) names.
- License: CC-BY-3.0 (Creative Commons Attribution 3.0 Unported), the licence of the whole upstream repository
  (its `LICENSE`, kept here unchanged). The specification states "Copyright © 2010-2022 Linux Foundation and its
  Contributors" and its CC-BY-3.0 licence (`chapters/index.md` at the pinned commit). The schema file carries no
  notice of its own.
- Import method: both files were downloaded on 2026-11-21 from `raw.githubusercontent.com` at the pinned commit
  into an empty directory and copied here unchanged. Their Git blob IDs match the pinned commit's tree (GitHub API
  `git/trees/aadf3b0b8dbbabdb4d880b0fc714255fea436ff7`), and the SHA-256 values below were calculated from the local
  bytes. Neither file ends in a newline upstream; the top-level `third_party/` exclude in `.pre-commit-config.yaml`
  keeps the formatting hooks from adding one.
- Renamed: `schemas/spdx-schema.json` is stored as `spdx-schema-2.3.json`. The bytes are unchanged.

## File manifest

| File | Upstream path | Kind | Upstream v2.3 blob SHA | SHA-256 |
| --- | --- | --- | --- | --- |
| `spdx-schema-2.3.json` | `schemas/spdx-schema.json` | imported byte-identical | `ee61e6686e885f8139c132647fd0b4f483b8fb81` | `239208b7ac287b3cf5d9a9af23f9d69863971102a5e1587a27a398b43490b89b` |
| `LICENSE` | `LICENSE` | imported byte-identical | `44a22d370bba8d13c7dd7449d71b40ea8842788e` | `017e38491cccbd2bdb6da0a32a33db9ec245b5dab30fdcd09f2c742c975e5b35` |
| `PROVENANCE.md` | n/a | project-authored manifest | n/a | n/a |

## Updating

Pin a new upstream commit, download the files from it into an empty directory, check their Git blob IDs against
that commit's tree, and update the pin, the rows above and `CREDITS.md` together. A schema that uses a keyword the
validator does not enforce fails `tools/test_sbom_binding.py` by name; extend the validator first, never drop the
check.

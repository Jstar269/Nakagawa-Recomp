# PROVENANCE.md — `src/rt/xr/openxr/`

The Khronos OpenXR C headers, for the runtime's optional OpenXR (VR) module
`src/rt/xr/nk_xr.c`. The module loads the OpenXR loader at run time
(`LoadLibraryA` or `dlopen`) and never links it, so the headers supply only
types, enumerations and function-pointer typedefs. No loader binary is carried.
The headers are imported unmodified.

## Upstream source

- Project: OpenXR-SDK (Khronos Group)
- Repository: `https://github.com/KhronosGroup/OpenXR-SDK`
- Pin: tag `release-1.1.65`, commit `24a1f2b92d212501a2b1b8d30ffd53abf55fcfa8`.
  Blob SHAs below are `git rev-parse release-1.1.65:include/openxr/<file>`
  against that commit's tree (`release-1.1.65:LICENSES/<file>` for the two licence texts).
- Licence: `Apache-2.0 OR MIT`, Copyright The Khronos Group Inc. (the SPDX
  line and copyright notice at the top of each header are kept unchanged). Both
  licence texts are carried here, as upstream ships them in `LICENSES/`.
- Import method: `git cat-file blob <blob>` from a local clone of the upstream
  repository, written unchanged into this directory.

## File manifest

| File | Kind | Upstream blob SHA |
| --- | --- | --- |
| `openxr.h` | imported byte-identical | `da41ddd77db24cb0fafc0b2d9a2461fcc2e582c2` |
| `openxr_platform.h` | imported byte-identical | `9de27ad14a934fe7a751416d2312b84e9b163e7c` |
| `openxr_platform_defines.h` | imported byte-identical | `79c5b2cfc2ff09d8afec806965c84ca27c5f090f` |
| `Apache-2.0.txt` | imported byte-identical (upstream `LICENSES/Apache-2.0.txt`) | `527a83a230cd01d7ff033e7bcb2d19d66ab2c956` |
| `MIT.txt` | imported byte-identical (upstream `LICENSES/MIT.txt`) | `204b93da48d02900098ced21c54062ffbff36b9c` |

Upstream `openxr.h` ends one blank line (line 2663) in spaces, so the file is exempt from the
trailing-whitespace hook (`.pre-commit-config.yaml`) and from Git's whitespace checks
(`.gitattributes`); its bytes stay as imported.

There is no LOCAL DELTA. Upstream's loader sources, layers, samples, specification
sources and generators are not imported.

## Build configuration

`src/rt/xr/nk_xr.h` defines `XR_NO_PROTOTYPES` before including `openxr.h`, so
the headers declare no linkable `xr*` functions and the build has no link-time
dependency on OpenXR. `openxr_platform.h` is carried for the platform and graphics
binding structures a later slice uses; the module does not include it yet.

## Verification

```sh
git hash-object src/rt/xr/openxr/openxr.h                    # da41ddd77db24cb0fafc0b2d9a2461fcc2e582c2
git hash-object src/rt/xr/openxr/openxr_platform.h           # 9de27ad14a934fe7a751416d2312b84e9b163e7c
git hash-object src/rt/xr/openxr/openxr_platform_defines.h   # 79c5b2cfc2ff09d8afec806965c84ca27c5f090f
git hash-object src/rt/xr/openxr/Apache-2.0.txt              # 527a83a230cd01d7ff033e7bcb2d19d66ab2c956
git hash-object src/rt/xr/openxr/MIT.txt                     # 204b93da48d02900098ced21c54062ffbff36b9c
```

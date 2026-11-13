# PROVENANCE.md — `assets/music/launcher/`

The optional launcher music ([#2207](https://github.com/Jstar269/nakagawa-recomp/issues/2207)): one track, off by
default, that the player plays only while its launcher shows. It is not game data and comes from no game.

## Upstream source

- Work: "Chill lofi inspired", by omfgdude, posted on OpenGameArt on 2017-05-29.
- Page: `https://opengameart.org/content/chill-lofi-inspired` (fetched 2026-11-12). The page lists the author as
  omfgdude, the art type as Music and the licence as CC0, and describes the piece as "A short song inspired by lofi
  hip hop. Created and recorded in one take using the styles of a Yamaha CVP-701."
- File: `ChillLofi.ogg`, the page's Ogg Vorbis attachment
  (`https://opengameart.org/sites/default/files/ChillLofi.ogg`, 6,486,377 bytes). In a comment on the page the author
  says it is an export of the original WAV recording, without the low-pass filter of the page's MP3.
- Licence: CC0-1.0 (Creative Commons Zero, public-domain dedication), as the page shows. CC0 asks for no credit; the
  project credits the author in `CREDITS.md` and `NOTICE.md` anyway.
- Import method: downloaded from the page's attachment link on 2026-11-12. The SHA-256 of the download and of the
  file here are the same.

## File manifest

| File | Kind | SHA-256 |
| --- | --- | --- |
| `ChillLofi.ogg` | imported byte-identical | `7ea7fd1b7c95ed1ca5f6ebb796626570137bcb75c7f2ea87b6c61212c7a86694` |
| `PROVENANCE.md` | project-authored manifest | n/a |

The track is Ogg Vorbis, 44.1 kHz stereo, 2 minutes 5.15 seconds (5,519,068 frames), at an integrated loudness of
about -22 LUFS (measured with FFmpeg's `ebur128` filter), the level the issue asks for. No loop edit is shipped: the
file is the author's upload, unchanged.

## How the player uses it

`tools/stage_runtime_dlls.py` copies `ChillLofi.ogg` beside the player as `assets/music/launcher/ChillLofi.ogg`, with
the launcher's fonts. `src/player/ui/player_ui_music.c` reads it only when the user turns **Launcher music** on,
decodes it whole with the vendored stb_vorbis (`third_party/stb/`), and loops it from its first to its last frame that
is not digital silence. The recording opens with about 2.0 s and closes with about 2.2 s of digital silence; the loop
leaves both out, so it has no gap, and both of its ends sit next to that silence, so the seam has no click. Nothing is
downloaded: the track ships with the player.

# open1161HD — licence scope

open1161HD by nikoltu (https://github.com/nikoltu/open1161HD-android-native).

## Covered by the MIT licence in `LICENSE`

Code written for open1161HD:

- `android/` — the Android launcher (Java) and the native entry point, except generated SDL sources;
- `android/ai/` — the built-in AI;
- `ui/` — the presentation layer: renderer, touch HUD, keyboard and mouse, diagnostics, CASC reader,
  DXT / ANIM loaders, VP8 / VP9 / WebM decoders, YUV conversion — except the third-party files listed below;
- `tools/` — host harnesses;
- this project's changes to files that come from OpenBW.

## Not covered

- **OpenBW** — the simulation core and the files that come from it (`bwgame.h`, `actions.h`, `sync.h`,
  `replay.h`, `data_loading.h` and the other root headers, and OpenBW's parts of `ui/`). These remain under
  their authors' terms; OpenBW has not published a licence (https://github.com/OpenBW/openbw/issues/17).
- **Third-party components**, each under its own licence: SDL2 and SDL_mixer (zlib), Asio (Boost Software
  License 1.0, `deps/asio/LICENSE_1_0.txt`), stb_image (public domain / MIT), libsmacker (LGPL 2.1,
  `ui/libsmacker/COPYING`).
- **Game content.** StarCraft, Brood War and StarCraft: Remastered and all their data are the property of
  Blizzard Entertainment, Inc. None of it is included. This project is not affiliated with or endorsed by
  Blizzard Entertainment.

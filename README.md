<div align="center">

<img src="android/app/src/main/res/mipmap-xxxhdpi/ic_launcher_round.png" alt="open1161HD" width="150"/>

# open1161HD

**A StarCraft: Brood War engine for Android — the classic 1.16.1 game and its
Remastered HD presentation, in one app.**

![Platform](https://img.shields.io/badge/platform-Android%2024%2B%20(arm64%20%2B%20x86__64)-3ddc84)
![Engine](https://img.shields.io/badge/core-OpenBW%201.16.1-1f6feb)

</div>

Built on [OpenBW](https://github.com/OpenBW/openbw), **open1161HD** is an Android
front-end and HD presentation layer over the 1.16.1 simulation core. A single APK
(`org.openbw.android.hd`) plays the classic **SD** game and, when Remastered data is
present, switches on the **HD** tiers — HD sprites, unit portraits and cinematics.

> This repository contains engine code only. It ships **no game assets** — you bring
> your own legally obtained data files (see below).

## Features

- **SD and HD in one app** — the SD tier from classic 1.16.1 archives; HD sprite,
  portrait and cinematic tiers when a Remastered install is present.
- **Gameplay** — the full 1.16.1 simulation, trigger engine and native AI scripts.
- **Campaigns** — briefings, cinematics, score screens, save and load.
- **Multiplayer** — deterministic LAN games (FFA and teams) with optional house rules.
- **Replays** — classic `.rep` and Remastered replays, with a timeline and speed control.
- **Built-in AI** — one opponent AI with four difficulty levels.
- **Touch HUD** — designed for tablets: pinch-zoom, S-Pen, per-race themes.
- **Render options** — a GLES2 post-processing pipeline (sharpen, bloom, colour grade,
  shadows), all optional.

The simulation core (`bwgame.h`, `actions.h`, `sync.h`, `replay.h`) is kept
deterministic: games stay in lockstep and replays play back exactly. Everything in `ui/`
is presentation only.

## Layout

| Path | Contents |
|---|---|
| `*.h` (root) | OpenBW simulation core, data loading, replays, saves, LAN sync |
| `ui/` | SDL2 UI, HUD, HD renderer, CASC reader, VP8/VP9/WebM decoders, libsmacker |
| `deps/asio/` | Asio (networking) |
| `android/` | Gradle project: launcher (Java), native entry point |
| `android/ai/` | the built-in AI; `bash android/scripts/build-ai.sh` rebuilds it into the app's assets |

## Build

Requirements: Android SDK, NDK 26.3.11579264, CMake 3.22.1, JDK 21 (Android Studio's
JBR), Gradle 8.9.

```bash
bash android/scripts/fetch-sdl.sh
gradle -p android assembleDebug
```

The APK is written to `android/app/build/outputs/apk/debug/open1161HD-debug.apk`
(ABIs `arm64-v8a` and `x86_64`, `minSdk 24`).

## Game data

The app runs on data files from **your own legally obtained** copies, imported from the
launcher:

| Edition | You provide |
|---|---|
| Classic (SD) | the 1.16.1 data archives (`StarCraft.mpq`, `BroodWar.mpq`, …) |
| HD | additionally, a Remastered install's CASC data |

## License & attribution

Engine code is open source under the terms of its upstream components (OpenBW, SDL,
Asio, stb, libsmacker; see `deps/asio/LICENSE_1_0.txt` and `ui/libsmacker/COPYING`). This is an independent, non-commercial project, **not
affiliated with or endorsed by Blizzard Entertainment**, and includes no game assets.
StarCraft and Brood War are trademarks of Blizzard Entertainment, Inc.

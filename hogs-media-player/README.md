<p align="center"><img src="hogs_logo.png" width="180" alt="Hogs media player logo"></p>

# Hogs media player

A fast, standalone media player for Windows 10/11 (64-bit), written in C, with:

- **Plays practically everything**: FFmpeg inside, GPU (D3D11VA) decoding, 4K/6K and 120 fps video,
  HDR tone-mapping, correct DVD/anamorphic aspect, subtitles, multiple audio tracks, images.
- **AI enhancement** on the graphics card (Vulkan, via ncnn): Real-ESRGAN upscaling, RIFE frame
  generation, and **AI Archive**, which saves a quality x4 remaster of a video to a file.
- **MilkDrop music visualizations**: 8,600+ presets, including MilkDrop 2 shader presets, cycling with the beat.
- **A playlist built for big libraries**: add whole folders (sub-folders included), endless shuffle,
  instant search with phrase-based hide words (`-one piece`), drag to reorder, sorting, and it remembers
  everything between launches.
- **Explorer integration**: the hog icon, and real thumbnails for videos Windows can't preview
  (HEVC/x265, RMVB, ...). Registered per user, no admin needed.

## Download

Get the latest `Hogs-media-player-…-win64.zip` from the
**[Releases](https://github.com/hogofwar0/hogs-media-player/releases)** page, unzip it anywhere and run
`hogofwar.exe`. Nothing to install. See the README.txt inside for keys and tips.

> Windows may say **"Windows protected your PC"** the first time: the program isn't signed with a paid
> certificate. Click **More info → Run anyway**. The full source is right here if you want to check it
> or build it yourself.

## Support the project

Hogs media player is free and open source. If you enjoy it, Bitcoin donations are very welcome:

<p><img src="donate_qr.png" width="200" alt="Bitcoin donation QR code for bc1q5m6vvn7w2htcglefmvqq5aku64fhuvezf9sg29"></p>

```
bc1q5m6vvn7w2htcglefmvqq5aku64fhuvezf9sg29
```

In the player, press **K** (or F1) and click **Copy**, or press **Ctrl+B**, to copy the address.
Please double-check it in your wallet before sending, and only send Bitcoin (BTC) to it.

## Building from source

Everything builds with free tools on Windows:

| What | Where it goes |
|---|---|
| [w64devkit](https://github.com/skeeto/w64devkit) (MinGW-w64 GCC) | anywhere; set `W64DEVKIT` to its folder |
| [SDL2 2.32.10](https://github.com/libsdl-org/SDL/releases) MinGW development package | `deps\SDL2-2.32.10\` |
| [FFmpeg n7.1 win64 gpl-shared](https://github.com/BtbN/FFmpeg-Builds/releases) (BtbN) | `deps\ffmpeg-n7.1-win64-gpl-shared\` |
| *Enhancer only:* [ncnn](https://github.com/Tencent/ncnn) built with Vulkan | `deps\ncnn-install\` (`include\ncnn`, `lib\*.a`) |
| *Enhancer only:* [Vulkan SDK](https://vulkan.lunarg.com/) | anywhere; set `HOGS_VULKAN_SDK` to its folder |

```bat
build.bat                       :: hogofwar.exe + hogs_thumb.dll (+ copies the runtime DLLs)
sh enhancer_src/build_enh.sh    :: hogs_enhance.exe  (run from the w64devkit shell)
powershell -ExecutionPolicy Bypass -File make_release.ps1   :: the portable release ZIP
```

The AI models (`vulkan\models\`) and MilkDrop presets (`presets\`) are large data files and are not
kept in the repository: copy them from a release ZIP (their origins are listed in
`release_docs/THIRD-PARTY-NOTICES.txt`).

| File | What it is |
|---|---|
| `hogs_ultimate.c` | the player (SDL2 + OpenGL + FFmpeg) |
| `hogs_thumb.c` | Explorer thumbnail provider (runs `hogofwar.exe --thumb` in a child process) |
| `enhancer_src/` | `hogs_enhance.exe`: Real-ESRGAN + RIFE on ncnn/Vulkan, H.265 output |
| `tools/logo3d.py` | renders the 3D logo (numpy ray-marcher) used for `hogs.ico` |
| `release_docs/` | README, licence notices and this page, as shipped |

## Licence

Hogs media player is free software: you can redistribute it and/or modify it under the terms of the
**GNU General Public License, version 3** (see `LICENSE`). It bundles a GPL build of FFmpeg, plus SDL2
(zlib), ncnn (BSD-3), Real-ESRGAN models (BSD-3), RIFE (MIT) and community MilkDrop presets; see
[`release_docs/THIRD-PARTY-NOTICES.txt`](release_docs/THIRD-PARTY-NOTICES.txt).

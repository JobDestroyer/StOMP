# StOMP

SteamOs Music Player (StOMP) 0.2 is a CONTROLLER-FIRST music player intended for the living room or the Steam Deck.

Features: 
 - FAST!
 - Milkdrop-style effects thanks to ProjectM
 - A controller-first user interface with keyboard availability if you need it
 - Internet Radio built-in
 - Single executable for easy management
 - No tracking or weird network calls; it never connects to the internet unless you try to play internet radio
 - Written in C
 - Can manage large music libraries of over 10,000 music files without issue
 - Compatibility with MusicControlol Decky plugin; play music while playing games! 
 
## Screenshots

![StOMP interface](assets/StOMP_Interface.jpg)

![StOMP interface 2](assets/StOMP_Interface2.jpg)

## Install

On a Steam Machine or SteamOS desktop session (one line):

```bash
curl -fsSL https://raw.githubusercontent.com/JobDestroyer/StOMP/main/install.sh | bash
```

That copies StOMP to `~/.local/share/stomp/stomp`, adds a Steam library shortcut, and installs Big Picture / library art. Restart Steam (or return to Gaming Mode) once.

From a built source tree, with Steam closed or in Desktop Mode:

```bash
./install.sh
```

The installer uses `./build/stomp` if present, otherwise the latest GitHub Release. Override with `STOMP_BIN=/path/to/stomp ./install.sh`.

Manual: download `stomp` from [GitHub Releases](https://github.com/JobDestroyer/StOMP/releases), `chmod +x`, run it, or add it in Steam Desktop Mode.



## Build dependencies

The player is an x86_64 Linux binary aimed at **SteamOS / Steam Machine / Big
Picture**. Build on any glibc Linux with:

- GCC (C11/C17) and a C++14 compiler (C++ is only used to compile **libprojectM**)
- CMake ≥ 3.21, Ninja or Make, pkg-config
- SDL2
- FFmpeg 5+ (`libavformat`, `libavcodec`, `libswresample`, `libavutil`)
- FreeType 2
- OpenGL 3.3 core
- dbus-1 (MPRIS, used by MusicControlol)

SQLite 3.46.1 is vendored as the official amalgamation (`third_party/sqlite3.c`,
`sqlite3.h`, `sqlite3ext.h`) and compiled into the player. Distro sqlite `-dev`
packages are not required.

libprojectM 4.1.2 is built from a **pruned** `third_party/projectm` tree as a
**shared** library and linked dynamically (LGPL-2.1). Do not static-link it.
Upstream docs, screenshots, tests, and git history are not in this tree.

SteamOS / Arch (workstation or SDK):

```bash
sudo pacman -S --needed base-devel cmake ninja pkgconf sdl2 ffmpeg freetype2 mesa dbus
```

Debian/Ubuntu workstation (to cross-build the same binary):

```bash
sudo apt install build-essential cmake ninja-build pkg-config \
    libsdl2-dev libavformat-dev libavcodec-dev libswresample-dev \
    libavutil-dev libfreetype-dev libgl-dev libdbus-1-dev
```

## CMake

From this source directory (the one that contains `CMakeLists.txt`):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -G Ninja
cmake --build build --target stomp-pack
```

`build/stomp` is a **single file** (the packed StOMP executable). libprojectM,
presets, textures, and the HUD font are packed into that ELF (libprojectM stays
a shared library inside the payload, which is what LGPL requires). On first
launch it unpacks into `~/.cache/vibe/r/<id>/` and execs the real player from
there.

```bash
./build/stomp --windowed --music-dir "$HOME/Music"
./build/stomp /path/to/song.flac
```

Copy that one packed file onto a Steam Machine and run it. You can still
inspect the unpacked layout at `build/vibe-core` plus the `.so` files if you
are debugging.

Point the library at folders from **Library → Settings → Music folder**.
That list is saved in `~/.config/vibe/vibe.conf` and used on the next launch.
`--music-dir` still adds a root for the current run. Until you set a folder
in-app, StOMP also looks at `~/Music`, `/run/media/*` (microSD), `/media/*`,
and a `demo/` directory next to the source tree if one is present.

Music library and session: `~/.local/share/vibe/library.db`.
Visualization likes/dislikes: `~/.local/share/vibe/viz.db` (survives deleting `library.db`).
To copy ratings out of an old combined library: `tools/split-viz-ratings`.

**Library → Playlists** lists `.m3u`, `.m3u8`, `.pls`, `.pl`, `.xspf`, `.wpl`,
`.zpl`, `.asx`, `.wax`, `.wmx`, and `.cue` files under those folders. Each
entry is matched first by the stored path, then by filename anywhere in the
library, then by title (and artist when the playlist has one). HTTP streams
in a playlist file are skipped. Hold Y adds a playlist to the queue.

## Controls

Every gamepad action has a keyboard twin so Steam Input can bind buttons to keys.
The same list is in **Library → Settings → Controls**. The Steam/Guide button is
unbound (Big Picture / on-screen keyboard).

| Function | Gamepad | Keyboard |
|----------|---------|----------|
| Play / confirm | A | Enter, Space |
| Back / hide HUD / quit prompt | B | |
| Dislike visualization; hold to remove from queue | X | X |
| Like visualization; hold to add to queue | Y | Y |
| Library | Start | Tab, Esc |
| Stop | Select | End |
| Queue | R3 | Q |
| Previous / next preset | L1 / R1 | [ / ] |
| Previous / next track | L2 / R2 | P / N |
| Volume | (keyboard) | - / = |
| Move | D-pad or left stick | Arrows |
| Letter jump (albums, artists, folders, …) | D-pad / stick Left Right | Left / Right |
| Seek | Right stick left/right | , / . |
| Page | Right stick up/down | Page Up / Down |
| Search backspace | on-screen Bksp | Backspace |
| Search clear | on-screen Clear | Delete |

Preset lock, Shuffle, and Repeat sit on the now-playing extras row (Up from the
seek bar, then A). The current visualization name is on the right of that row;
A opens the full preset list. Search uses a QWERTY grid plus Clear/Backspace;
Steam’s keyboard also types into the field.

Hold A on an artist, album, playlist, or radio station to add or remove a favorite.
Hold Y on a song, album, artist, folder, or playlist to append it to the queue.
Hold X removes a song that is already in the queue, from the queue screen or from
the list you added it from. Search is Library → Search. Queue → Queue Options can
clear the queue or save it as an `.m3u` in your music folder.
B on Library goes back to now playing. On now playing, B hides the HUD; B
again asks whether to quit. Tab and Esc both open and close the Library.

No mouse is required. The SDL Game Controller API is required.

## Formats

FLAC, MP3, Ogg Vorbis, Opus, WAV, AAC/M4A. Output is stereo float 44.1 kHz.
FFmpeg resamples only when needed. The same decoded PCM is written to the
device and fed to ProjectM (target audio-to-visual latency under ~35 ms).
There is no PipeWire/Pulse loopback and no microphone path.

## License

Copyright (C) 2026 JobDestroyer.

StOMP is free software: you can redistribute it and/or modify it under the
terms of the **GNU Lesser General Public License, version 2.1**, the same
license as libprojectM. The full text is in `LICENSE`.

libprojectM is linked **dynamically** as a shared library. Do not static-link it.

Other bundled material keeps its own terms:

- libprojectM 4.1.2 — LGPL-2.1 (`third_party/projectm/LICENSE.txt`)
- SQLite 3.46.1 amalgamation — public domain (`third_party/sqlite3.c`)
- DejaVu Sans — Bitstream Vera / Arev (`fonts/LICENSE-DejaVu.txt`)
- Droid Sans Fallback — Apache 2.0 (`fonts/LICENSE-Apache-2.0.txt`)
- Cream of the Crop presets — see `presets/cream-of-the-crop/LICENSE.md`

SDL2, FFmpeg, FreeType, and OpenGL come from the system and keep their own
licenses. A typical distro FFmpeg is also LGPL; do not enable extra GPL codecs
in a custom FFmpeg build unless you want the whole combination under GPL.

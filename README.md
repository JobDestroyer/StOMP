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

In Desktop Mode open your Konsole and paste this: 

```bash
curl -fsSL https://raw.githubusercontent.com/JobDestroyer/StOMP/main/install.sh | bash
```

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

StOMP is not an official Valve product. It is not affiliated with, endorsed by,
or sponsored by Valve Corporation. Steam, SteamOS, Steam Deck, and Steam Machine
are trademarks of Valve Corporation.

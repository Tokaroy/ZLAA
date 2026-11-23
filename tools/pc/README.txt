ZLAA Builder @VERSION@
=====================

Builds ZLAA's game code on your PC instead of on your phone: a few minutes on most PCs, instead of about an hour
of Build Game on the phone. It needs the same thing Build Game does: your own extracted dump of The Legend of
Zelda: Link's Awakening (version 1.0.0, 1.0.1 or 1.1.0), its exefs folder in particular.

Use the Builder of the same version as your ZLAA app (@VERSION@). ZLAA won't take game code built by another one;
its start screen then says "Game code is from an older version".


Windows
-------
1. Unpack this whole folder somewhere (not inside the dump).
2. Double-click "Build Game.bat" and choose your dump's folder (the one with exefs and romfs).
   Or drop that folder onto "Build Game.bat".
3. Wait. It translates the game's code, compiles it and links it. If you stop it, running it again carries on.
   It needs about 2 GB of free space while it works.
4. The result is in the "output" folder: libgame-<build id>.so.

Linux (and SteamOS)
-------------------
1. Unpack this folder. You need python3 (3.10 or newer), which most systems have.
2. ./build-game.sh /path/to/your/dump
3. The result is in the "output" folder: libgame-<build id>.so.


Putting it on your phone
------------------------
Copy libgame-<build id>.so to your phone (by USB cable, for example), into ONE of:
  - the Download folder (ZLAA needs "All files access" for this: it asks for it when you choose your game files),
  - the folder that holds your game files' exefs and romfs folders,
  - Android/data/io.github.zlaa/files/game
Then start ZLAA. The start screen shows Ready, and ZLAA copies the file to its own storage at Play: you can delete
the copy you made afterwards.

With adb (Android's platform tools) and USB debugging on, "--push" copies it for you:
  Windows: in a command prompt, in this folder: "Build Game.bat" "C:\path\to\your\dump" --push
  Linux:   ./build-game.sh /path/to/your/dump --push


Good to know
------------
- Everything it makes comes from your own files and stays on your PC until you copy it. Don't share the result:
  it is made from Nintendo's game.
- Other options: --jobs n (compiles at a time; by default 3 of every 4 cores, as memory allows: fewer keep the
  PC quieter, more make it faster), --keep (keep the translated code, ~700 MB, in output/work), --out folder.
- Something went wrong? The logs are in output/work/logs.

Inside: Python (python.org, PSF license; Windows only), zig (ziglang.org, MIT), ZLAA's recompiler (MIT). Their
licenses are in the licenses folder.

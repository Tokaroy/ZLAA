ZLAA Builder @VERSION@
==================

Builds ZLAA's game code on your PC instead of on your phone: about 10-30 minutes depending on the PC, instead of about an hour of Build Game on the phone. It needs your own extracted dump of The Legend of Zelda: Link's Awakening (version 1.0.0, 1.0.1 or 1.1.0), with both the exefs and romfs folders.

If you are getting the message "Game code is from an older version", make sure you are using the Builder of the same version as your ZLAA app (@VERSION@).


Windows
-------
1. Unpack this whole folder somewhere (not inside the dump).
2. Double-click "Build Game.bat" and choose your dump's folder (the one with exefs and romfs). Or drop that folder onto "Build Game.bat".
3. Wait. It translates the game's code, compiles it and links it. You will need about 2GB of free space.
4. The result is in the "output" folder: libgame-<build id>.so.

Linux/SteamOS
-------------
1. Unpack this folder. You need python3 (3.10 or newer).
2. ./build-game.sh /path/to/your/dump
3. The result is in the "output" folder: libgame-<build id>.so.


Putting it on your phone
------------------------
Copy libgame-<build id>.so to your phone, into ONE of:
- the Download folder (ZLAA needs "All files access" for this, it asks for it when you choose your game files),
- the folder that holds your game files' exefs and romfs folders,
- Android/data/io.github.zlaa/files/game

Then start ZLAA. The start screen shows Ready, and ZLAA copies the file to its own storage, so you can now delete the copy.

With adb (Android's platform tools) and USB debugging on, "--push" copies it for you:
Windows: in a command prompt, in this folder: "Build Game.bat" "C:\path\to\your\dump" --push
Linux: ./build-game.sh /path/to/your/dump --push


Important
---------
- Everything it makes comes from your own files and stays on your PC until you copy it. Don't share the result, since it is made from Nintendo's game.

HOGS MEDIA PLAYER  V1.45  (Windows 10/11, 64-bit)
=================================================

A fast, standalone video / music / image player with AI upscaling, AI frame
generation, MilkDrop music visualizations and an endless shuffle playlist.
Nothing to install: unzip and run.


GETTING STARTED
---------------
1. Unzip this folder anywhere you like (Desktop, Documents, a games drive...).
   Keep all the files together - the player needs the DLLs, "presets" and
   "vulkan" folders that sit next to it.
2. Double-click  hogofwar.exe.
   If Windows says "Windows protected your PC" (the program isn't signed with
   a paid certificate), click "More info" and then "Run anyway".
3. Add media: drag files or whole folders onto the window, or use the
   playlist's Add buttons. Sub-folders are scanned too.
4. Press Space (or click Play).

To make it your default player: right-click any video -> Open with ->
Choose another app -> Hogs media player -> "Always".
Opening a file that way shows just that file; your saved playlist is kept for
the next normal launch.

The first launch also gives your Windows user the hog icon and picture
thumbnails for videos Windows can't preview itself (HEVC/x265, RMVB, ...).
No admin rights are needed and nothing system-wide is changed.


KEYS
----
  Space            play / pause            S        stop
  Left / Right     seek -10 s / +10 s      Up/Down  volume (up to 200%)
  N / P            next / previous         M        mute
  H                shuffle on/off          R        repeat on/off
  F                fullscreen              C        fit / crop to fill
  L                show/hide playlist      Ctrl+F   search the playlist
  Ctrl+S / Ctrl+O  save / open a playlist  Del      remove selected rows
  A                next audio track        B        subtitles on/off
  V                change visualizer       I        stats overlay
  Y                change colour theme     K or F1  show all keys
  U                AI upscale              G        AI frame generation
  E                AI upscale + frame-gen  T        AI anime upscale
  D                light enhancement       W        AI Archive (save a remaster)
  Esc              cancel an AI job / leave fullscreen
  Ctrl+B           copy the Bitcoin donation address (see "Support" below)

Playlist search: type words to show matching files. A minus hides things:
"-one piece" hides everything containing that phrase. Press Enter to keep a
hide-word as a chip; the search box itself is remembered between launches.


AI FEATURES
-----------
Upscaling, frame generation and Archive use your graphics card through Vulkan,
which comes with any up-to-date AMD, NVIDIA or Intel graphics driver.
They are heavy: Archive (quality x4) can take many hours for one film and uses
most of your graphics card while it runs. Finished results are saved in an
"enhcache" folder so replaying them is instant.


WHERE YOUR SETTINGS ARE KEPT
----------------------------
Next to hogofwar.exe (portable). If that folder isn't writable - for example
inside "Program Files" - they go to  %LOCALAPPDATA%\Hogs media player\  instead.
Files: hogs_session.m3u8 (playlist), hogs_excludes.txt (hide-words),
hogs_search.txt, hogs_theme.txt, and the enhcache folder.


UNINSTALLING
------------
1. Right-click unregister_thumbnails.ps1 -> Run with PowerShell
   (removes the icon/thumbnail registration for your Windows user).
2. Delete the folder (and %LOCALAPPDATA%\Hogs media player if it exists).


SUPPORT HOGS MEDIA PLAYER
-------------------------
Hogs media player is free. If you enjoy it, donations in Bitcoin are very
welcome and help keep it going:

    bc1q5m6vvn7w2htcglefmvqq5aku64fhuvezf9sg29

In the player: press K (or F1) to see the address and a QR code you can scan
with a phone wallet; click "Copy address" or press Ctrl+B to copy it. The QR
code is also included in this folder as donate_qr.png. Always double-check the address in your wallet
before sending - only send Bitcoin (BTC) to it.


LICENCE AND SOURCE CODE
-----------------------
Hogs media player is free software under the GNU General Public License v3
(see LICENSE.txt). It bundles FFmpeg, SDL2, ncnn, Real-ESRGAN and RIFE models
and MilkDrop presets - see THIRD-PARTY-NOTICES.txt for their licences and
where to get their source code. The player's own source code, new versions
and bug reports:  https://github.com/hogofwar0/hogs-media-player

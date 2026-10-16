Space Cadet Pinball - Windows build (x64)
=========================================

Thanks for playing.  This is the reverse-engineered "3D Pinball for Windows -
Space Cadet" with a modern renderer, so it runs on current Windows instead of
the original `pinball.exe`.

Keys
----
    F2   new game                  F6   music on/off
    F3   pause / resume            F8   player controls dialog
    F4   full screen               F9   show/hide the menu bar
    F5   sound effects on/off      ESC  menu

Requirements
------------
* 64-bit Windows 10 or 11.  Nothing else to install: the game and its DLLs use
  only system libraries, so no Visual C++ redistributable is needed.
* A GPU driver with Vulkan.  The standard Intel / AMD / NVIDIA drivers for
  Windows include it; if the game reports "Could not initialize renderer",
  install the Vulkan Runtime from https://vulkan.lunarg.com/sdk/home
* The game window must have focus - it mutes itself while another window is in
  front, which is normal.

No music?
---------
The game is a normal windowed program, so it has no console of its own.  To
read its startup log, run it from a Command Prompt with the output redirected:

    cd /d "<the folder the game is installed in>"
    SpaceCadetPinball.exe > log.txt 2>&1
    type log.txt

The log names the SoundFont the game loaded:

    [midi] Using SoundFont: <path>

If it says "No usable SoundFont found; music disabled", point the game at a
SoundFont explicitly:

    set SCP_SOUNDFONT=<folder with the .sf2 file>\FluidR3_GM.sf2
    SpaceCadetPinball.exe

Any General MIDI .sf2 or .sf3 works.  FluidR3_GM.sf2 is about 140 MB;
TimGM6mb.sf2 is about 6 MB with lower quality.

Where settings live
-------------------
Options, high scores and the window layout are stored per user in

    %APPDATA%\SpaceCadetPinball\

so the install folder stays clean, and uninstalling the game keeps your high
scores.

Credits and licensing
---------------------
Source code: https://github.com/k4zmu2a/SpaceCadetPinball (MIT license).
The game data (PINBALL.DAT, SOUND*.WAV, PINBALL*.MID) and the SoundFont are not
part of that license - they originate from "3D Pinball for Windows - Space
Cadet" and from the FluidR3_GM SoundFont project respectively.

<!-- markdownlint-disable-file MD033 -->

# SpaceCadetPinball

## Summary

Reverse engineering of `3D Pinball for Windows - Space Cadet`, a game bundled with Windows.

## How to play

Place compiled executable into a folder containing original game resources (not included).\
Supports data files from Windows and Full Tilt versions of the game.

## Known source ports

| Platform           | Author          | URL                                                                                                        |
| ------------------ | --------------- | ---------------------------------------------------------------------------------------------------------- |
| PS Vita            | Axiom           | <https://github.com/suicvne/SpaceCadetPinball_Vita>                                                        |
| Emscripten         | alula           | <https://github.com/alula/SpaceCadetPinball> <br> Play online: <https://alula.github.io/SpaceCadetPinball> |
| Nintendo Switch    | averne          | <https://github.com/averne/SpaceCadetPinball-NX>                                                           |
| webOS TV           | mariotaku       | <https://github.com/webosbrew/SpaceCadetPinball>                                                           |
| Android (WIP)      | Iscle           | https://github.com/Iscle/SpaceCadetPinball                                                                 |
| Nintendo Wii       | MaikelChan      | https://github.com/MaikelChan/SpaceCadetPinball                                                            |
| Nintendo 3DS       | MaikelChan      | https://github.com/MaikelChan/SpaceCadetPinball/tree/3ds                                                   |
| Nintendo DS        | Headshotnoby    | https://github.com/headshot2017/3dpinball-nds                                                              |
| Nintendo Wii U     | IntriguingTiles | https://github.com/IntriguingTiles/SpaceCadetPinball-WiiU                                                  |
| PlayStation 2      | Headshotnoby    | https://github.com/headshot2017/3dpinball-ps2                                                              |
| Sega Dreamcast     | Headshotnoby    | https://github.com/headshot2017/3dpinball-dc                                                               |
| MorphOS            | BeWorld         | https://www.morphos-storage.net/?id=1688897                                                                |
| AmigaOS 4          | rjd324          | http://aminet.net/package/game/actio/spacecadetpinball-aos4                                                |
| Android (WIP)      | fexed           | https://github.com/fexed/Pinball-on-Android                                                                |

Platforms covered by this project: desktop Windows, Linux and macOS.

<br>
<br>
<br>
<br>
<br>
<br>

## Source

* `pinball.exe` from `Windows XP` (SHA-1 `2A5B525E0F631BB6107639E2A69DF15986FB0D05`) and its public PDB
* `CADET.EXE` 32bit version from `Full Tilt! Pinball` (SHA-1 `3F7B5699074B83FD713657CD94671F2156DBEDC4`)

## Tools used

`Ghidra`, `Ida`, `Visual Studio`

## What was done

* All structures were populated, globals and locals named.
* All subs were decompiled, C pseudo code was converted to compilable C++. Loose (namespace?) subs were assigned to classes.

## Compiling

Project uses `C++11`. The window and input come from `GLFW`, rendering from `Vulkan`, audio from `miniaudio`, and MIDI music from `FluidSynth` (optional).

### On Windows

Install the [Vulkan SDK](https://vulkan.lunarg.com/sdk/home) (provides `vulkan-1.dll` and the loader).\
Dependencies (`glfw3`, and optionally `fluidsynth`) can be provided with [vcpkg](https://vcpkg.io); set `VCPKG_ROOT` and configure with `-DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake`.\
Compile with Visual Studio; tested with 2019. `BuildForWindows.ps1` automates the release build.

### On Linux

Install devel packages for `GLFW`, `Vulkan` and (optionally) `FluidSynth`.\
Compile with CMake; tested with GCC 10, Clang 11.\
To cross-compile for Windows, install a 64-bit mingw toolchain plus GLFW/Vulkan/FluidSynth, then use the `mingwcc.cmake` toolchain.

[![Packaging status](https://repology.org/badge/tiny-repos/spacecadetpinball.svg)](https://repology.org/project/spacecadetpinball/versions) 

Some distributions provide a package in their repository. You can use those for easier dependency management and updates.

This project is available as Flatpak on [Flathub](https://flathub.org/apps/details/com.github.k4zmu2a.spacecadetpinball).

### On macOS

Install XCode (or at least Xcode Command Line Tools with `xcode-select --install`) and CMake.

**HomeBrew**

You can easily install the build artifact by using `brew`.

```sh
brew tap draftbrew/tap
brew install --no-quarantine space-cadet-pinball
```

Be aware that the flag `--no-quarantime` will disable macOS's Gatekeeper during installation.

**Manual compilation:**

* **Homebrew**: Install the `glfw`, `vulkan-loader`, `molten-vk` and (optionally) `fluid-synth` homebrew packages.
* **MacPorts**: Install the `glfw`, `moltenvk` and (optionally) `fluid-synth` macports packages.

Compile with CMake. Ensure that `CMAKE_OSX_ARCHITECTURES` variable is set for either `x86_64` Apple Intel or `arm64` for Apple Silicon.

Tested with: macOS Big Sur (Intel) with Xcode 13 & macOS Montery Beta (Apple Silicon) with Xcode 13.

**Automated compilation:**

Run the `build-mac-app.sh` script from the root of the repository. The app will be available in a DMG file named `SpaceCadetPinball-<version>-mac.dmg`.

Tested with: macOS Ventura (Apple Silicon) with Xcode Command Line Tools 14 & macOS Big Sur on GitHub Runner (Intel) with XCode 13.

### Background music (optional)

MIDI music is rendered by `FluidSynth` and needs a General MIDI SoundFont at runtime.\
The game looks for a SoundFont in the usual system locations; set `SCP_SOUNDFONT=/path/to/soundfont.sf2` to override it.\
If FluidSynth or a SoundFont is missing, the game runs normally without music.

## Plans

* ~~Decompile original game~~
* ~~Resizable window, scaled graphics~~
* ~~Loader for high-res sprites from CADET.DAT~~
* ~~Cross-platform port using SDL2, SDL2_mixer, ImGui~~
* ~~Replace SDL2 with GLFW + Vulkan and SDL2_mixer with miniaudio~~
* Full Tilt Cadet features
* Localization support
* Maybe: Support for the other two tables - Dragon and Pirate
* Maybe: Game data editor

## On 64-bit bug that killed the game

I did not find it, decompiled game worked in x64 mode on the first try.\
It was either lost in decompilation or introduced in x64 port/not present in x86 build.\
Based on public description of the bug (no ball collision), I guess that the bug was in `TEdgeManager::TestGridBox`

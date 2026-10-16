# Space Cadet Pinball

A cross-platform rebuild of *3D Pinball for Windows – Space Cadet*, the pinball
table that shipped with Windows. The original `pinball.exe` was disassembled and
turned back into C++ source, which this project builds into a modern executable
for Windows, Linux and macOS.

| | |
| --- | --- |
| Rendering | Vulkan (MoltenVK on macOS) |
| Windowing, input, gamepads | GLFW |
| Audio | built-in miniaudio mixer |
| Music | FluidSynth with a General MIDI SoundFont (optional) |
| UI | Dear ImGui |
| Build | CMake, C++11 |

This repository is a fork of [k4zmu2a/SpaceCadetPinball](https://github.com/k4zmu2a/SpaceCadetPinball).
It adds the Windows packaging described below; see [Credits](#credits-and-license).

## Game data is not included

This is a game engine, not the game. The table graphics, sounds and music are
Microsoft's and are **not** part of this repository or its license. To play you
need data files from a copy you own:

* from Windows XP: the `3D Pinball` folder (`PINBALL.DAT`, `FONT.DAT`, `PINBALL.MID`, `PINBALL2.MID`, the `SOUND*.WAV` samples)
* from *Full Tilt! Pinball*: `CADET.EXE` and its data (the `CADET.DAT` data set)

Put those files next to the executable. Either the 3DPB or the Full Tilt data
set works, and the two are interchangeable; the game picks whichever it finds
(main menu has an option to prefer one over the other if both are present).

## Getting it running

### Windows

Download or build the release, then either:

* **Installer** – run `SpaceCadetPinball-Setup.exe`. It installs for the current
  user only (`%LOCALAPPDATA%\Programs\SpaceCadetPinball`), needs no
  administrator rights, adds a Start Menu entry and an optional desktop
  shortcut, and appears in *Settings → Apps* so you can uninstall it. For a
  scripted install:

  ```powershell
  SpaceCadetPinball-Setup.exe /S /D=C:\Games\SpaceCadetPinball
  ```

  `/D=` must come last and must not be quoted. See
  [Platform/Windows/BUILD.md](Platform/Windows/BUILD.md) to build the installer
  yourself.

* **Portable** – unpack the zip and run `SpaceCadetPinball.exe`. Nothing is
  installed and nothing outside that folder is touched. Keep the files together:
  the game loads its data, its sound samples and its SoundFont from the folder
  the executable is in.

### Linux

Install `glfw`, `vulkan` and (optionally) `fluidsynth` development packages and
build with CMake – see [Platform/Linux/BUILD.md](Platform/Linux/BUILD.md).
Distribution packages and a Flatpak of the upstream project exist as well, and
usually track this code.

The game runs natively on Wayland with GLFW 3.4 or newer; see
[Wayland](Platform/Linux/BUILD.md#wayland).

### macOS

Build the app bundle with `./build-mac-app.sh`; it produces
`SpaceCadetPinball-<version>-mac.dmg`. Requires Xcode command line tools and,
for dependencies, Homebrew (`glfw`, `vulkan-loader`, `molten-vk` and optionally
`fluid-synth`). Upstream also has a Homebrew formula.

## Controls

| Key | Action | Key | Action |
| --- | ------ | --- | ------ |
| `F2` | new game | `F6` | music on/off |
| `F3` | pause / resume | `F8` | player controls dialog |
| `F4` | full screen | `F9` | show/hide the menu bar |
| `F5` | sound effects on/off | `Esc` | menu |

Flipers and the plunger are keyboard, mouse and gamepad input; `F8` opens the
dialog where the bindings can be inspected and changed. The game pauses its
sound while its window is not focused, which is intentional.

## Background music

MIDI music is rendered by FluidSynth and needs a General MIDI SoundFont at
runtime. The game looks for one in this order:

1. the file named by `SCP_SOUNDFONT` (any `.sf2`/`.sf3`),
2. next to the executable: `soundfonts/FluidR3_GM.sf2`, `soundfonts/FluidR3_GS.sf2`,
   `soundfonts/default.sf2`, `soundfonts/default-GM.sf2`, `soundfonts/TimGM6mb.sf2`,
   `FluidR3_GM.sf2`,
3. system locations: `C:\soundfonts\` and `C:\Program Files\FluidSynth\soundfonts\`
   on Windows; `/usr/share/sounds/sf2/`, `/usr/share/soundfonts/` and friends on
   Linux, including a scan for any `.sf2`/`.sf3` in those directories.

The packaged Windows build puts `FluidR3_GM.sf2` in `soundfonts\` next to the
game, so music works out of the box. Any General MIDI SoundFont can be swapped
in; `TimGM6mb.sf2` is about 6 MB instead of 140 MB with lower quality.

If there is no FluidSynth or no SoundFont, the game runs normally without music.

## Building

### Windows (MSVC + vcpkg)

Install [vcpkg](https://vcpkg.io), set `VCPKG_ROOT`, then either configure
CMake yourself:

```powershell
cmake -S . -B build -A x64 -DCMAKE_WIN32_EXECUTABLE:BOOL=1 `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_MANIFEST_FEATURES=music
cmake --build build --config Release
```

or run `./BuildForWindows.ps1`, which builds and zips both x86 and x64 into
`bin\SpaceCadetPinball<arch>Win.zip` (the Vulkan loader comes from vcpkg, so the
Vulkan SDK is not required). `-DCMAKE_WIN32_EXECUTABLE:BOOL=1` makes a windowed
build - without it the game opens with a console window.

### Windows installer

[Platform/Windows/BUILD.md](Platform/Windows/BUILD.md) - NSIS script that packs
`bin\` into a single setup executable, with an optional SoundFont component. It
compiles on Windows or, through Wine, on Linux.

### Linux

[Platform/Linux/BUILD.md](Platform/Linux/BUILD.md) - development packages,
CMake, and the SoundFont path notes.

### Cross-compiling for Windows from Linux

A 64-bit mingw-w64 toolchain plus GLFW/Vulkan/FluidSynth for Windows is enough;
`mingwcc.cmake` is the toolchain file:

```bash
cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=mingwcc.cmake \
  -DCMAKE_WIN32_EXECUTABLE:BOOL=1 -DCMAKE_BUILD_TYPE=Release
cmake --build build-win
```

The resulting `bin/SpaceCadetPinball.exe` needs `libfluidsynth-3.dll` and
`sndfile.dll` next to it for music, and it imports only system libraries
otherwise - no Visual C++ redistributable is required.

## Troubleshooting

**"Could not initialize renderer"** – the game found no Vulkan device. Install
or update your GPU driver; on Windows, the Vulkan Runtime from
<https://vulkan.lunarg.com/sdk/home> provides the loader if the driver itself
has no Vulkan support. This is also what happens inside most virtual machines:
VirtualBox, VMware's guest driver and Hyper-V GPU-P do not expose Vulkan, so the
game cannot start there. GPU passthrough works. Software rendering (Mesa's
lavapipe) can be used as a slow fallback by pointing `VK_ICD_FILENAMES` at its
ICD.

**No music** – start the game with its output redirected and read the log; it
names the SoundFont it loaded (`[midi] Using SoundFont: ...`) or reports that
none was usable. Then set `SCP_SOUNDFONT` to an `.sf2`/`.sf3` file.

**Mouse clicks near the top of the table do nothing** – that was a bug fixed in
2.2.0; update.

**Windows warns about an unknown publisher** – release builds are not code
signed, so SmartScreen asks for confirmation (*More info → Run anyway*) when the
file carries a mark-of-the-web.

**Rebuilt or upgraded and the controls are wrong** – control bindings are stored
as library-specific codes, so they are reset once when upgrading from a build
that used SDL2 (2.1.0 and older). Bind them again in the `F8` dialog.

## Repository layout

| Path | Contents |
| ---- | -------- |
| `SpaceCadetPinball/` | the game source, plus vendored Dear ImGui and miniaudio |
| `Platform/Linux/`, `Platform/macOS/`, `Platform/Windows/` | packaging and build notes per platform |
| `BuildForWindows.ps1`, `build-mac-app.sh` | release packagers |
| `mingwcc.cmake` | mingw-w64 toolchain file for cross-compiling |
| `Doc/`, `Screenshots/` | decompilation notes and images |
| `bin/` | build output and your game data (ignored by git) |

`CHANGELOG.md` lists release notes.

## Provenance

The decompilation is based on `pinball.exe` from Windows XP
(SHA-1 `2A5B525E0F631BB6107639E2A69DF15986FB0D05`) and its public PDB, and on the
32-bit `CADET.EXE` from *Full Tilt! Pinball*
(SHA-1 `3F7B5699074B83FD713657CD94671F2156DBEDC4`), using Ghidra, IDA and Visual
Studio. All structures were populated, globals and locals named, and the
decompiled pseudo-code was turned into compilable C++.

The original game had a 64-bit bug that killed collision handling. It was not
reproduced here: the decompiled game worked in x64 mode on the first try, so the
bug was either lost in decompilation or never present in this code path.

## Credits and license

* Original reverse engineering and the cross-platform port:
  [k4zmu2a/SpaceCadetPinball](https://github.com/k4zmu2a/SpaceCadetPinball)
  (MIT). This repository is a fork of it.
* Third-party libraries: [GLFW](https://www.glfw.org),
  [Dear ImGui](https://github.com/ocornut/imgui),
  [miniaudio](https://github.com/mackron/miniaudio),
  [FluidSynth](https://www.fluidsynth.org), MoltenVK and the Vulkan loader.
* The project's source code is MIT licensed, see [LICENSE](LICENSE).
* The game data (`PINBALL.DAT`, `SOUND*.WAV`, `PINBALL*.MID`, `CADET.DAT`, ...)
  and any General MIDI SoundFont are **not** covered by that license. They stay
  the property of their respective owners and are not distributed here.

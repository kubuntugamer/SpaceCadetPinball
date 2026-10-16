# Building

Install devel packages for `GLFW`, `Vulkan` and (optionally) `FluidSynth`.\

Compile with CMake; tested with GCC 10, Clang 11.\
To cross-compile for Windows, install a 64-bit mingw toolchain plus the GLFW/Vulkan/FluidSynth devel packages, then use the `mingwcc.cmake` toolchain.

```bash
# On Debian and Ubuntu
sudo apt install cmake build-essential ninja-build libglfw3-dev libvulkan-dev libfluidsynth-dev

# On Fedora
sudo dnf install cmake ninja-build glfw-devel vulkan-loader-devel fluidsynth fluidsynth-devel g++

# Build
cmake -GNinja .
ninja
```

## Wayland

The game runs natively on Wayland. Windowing is done with GLFW, so this needs **GLFW 3.4 or newer**: it selects a backend at start-up and picks Wayland whenever the session offers it.

Older GLFW (3.3, which older Debian and Ubuntu still ship) cannot select at run time - its packages are built for one backend, and the default is X11, so those systems run the game through XWayland. There, install the Wayland flavour to get the Wayland backend:

```bash
sudo apt install libglfw3-dev libglfw3-wayland
```

Check your version with `pkg-config --modversion glfw3`.

**On Wayland an application cannot position its own windows**; the compositor decides placement. The game saves its windowed position when it goes full screen and restores it on the way back, so on Wayland that restore is a no-op and the window reappears wherever the compositor puts it. Full screen itself works normally.

## AppImage

`build-appimage.sh` packs the Linux build from `bin/` into a single file that
runs without installing anything:

```bash
./build-appimage.sh
```

It writes `bin/SpaceCadetPinball-<version>-x86_64.AppImage`, carrying the game,
the game data and the libraries the build needs. The display server, GPU driver
and audio stack come from the host, so a Vulkan-capable driver is still
required, and no SoundFont is bundled - music uses the system one or
`SCP_SOUNDFONT`. The first run needs `curl` and network access to fetch
`appimagetool` into `~/.cache`; set `APPIMAGETOOL` to use an existing copy.

**If you can't hear the background music**: FluidSynth needs a General MIDI SoundFont. The game auto-detects common system locations, but you can point it at a specific file:
`export SCP_SOUNDFONT=/path/to/your/soundfont`

FluidSynth packages ship a default soundfont in the following path:
On Fedora: `/usr/share/soundfonts/FluidR3_GM.sf2`
On Ubuntu/Debian: `/usr/share/sounds/sf2/FluidR3_GM.sf2`

## Native packages

`build-packages.sh` turns the Linux build into installable packages using CPack:

```bash
./build-packages.sh
```

It produces, in `build/` (and copies them to `bin/`):

- `spacecadetpinball_<version>_amd64.deb` - for Debian/Ubuntu
- `spacecadetpinball-<version>-1.x86_64.rpm` - for Fedora/openSUSE (needs `rpmbuild`)
- `SpaceCadetPinball-<version>-Linux.tar.gz` / `.zip` - generic

The packages install the engine, desktop entry, icons and AppStream metadata. The
game data (PINBALL.DAT, SOUND*.WAV, ...) is copyrighted by Microsoft and is
**not** included - drop it into `/usr/share/SpaceCadetPinball/` (or any of the
other search paths the game prints) afterwards. Music needs a SoundFont; the
package recommends `fluid-soundfont-gm` on Debian and the equivalent on RPM
distros.

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

**If you can't hear the background music**: FluidSynth needs a General MIDI SoundFont. The game auto-detects common system locations, but you can point it at a specific file:
`export SCP_SOUNDFONT=/path/to/your/soundfont`

FluidSynth packages ship a default soundfont in the following path:
On Fedora: `/usr/share/soundfonts/FluidR3_GM.sf2`
On Ubuntu/Debian: `/usr/share/sounds/sf2/FluidR3_GM.sf2`

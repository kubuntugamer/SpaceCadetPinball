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

**If you can't hear the background music**: FluidSynth needs a General MIDI SoundFont. The game auto-detects common system locations, but you can point it at a specific file:
`export SCP_SOUNDFONT=/path/to/your/soundfont`

FluidSynth packages ship a default soundfont in the following path:
On Fedora: `/usr/share/soundfonts/FluidR3_GM.sf2`
On Ubuntu/Debian: `/usr/share/sounds/sf2/FluidR3_GM.sf2`

# Changelog

All notable changes to this project are documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and
releases are tagged `Release_<version>`, matching the tags of the upstream
project this is forked from.

Game data and SoundFonts are never distributed with the source, so no entry
below concerns them.

## [Unreleased]

### Changed

* The flipper's collision shape — five control points rotated about the pivot, two lines rebuilt, two circles reset — and the scalars that decide how far a flipper turns per frame and how many substeps that spans are computed in the Rust geometry crate. `flipper_parity` runs the pre-port bodies beside them over 773 cases, bit for bit.

## [2.6.6] - 2026-10-31

### Changed

* Table walls hand their geometry to the collision world instead of answering a distance callback: a `TLine` segment its endpoints, a `TCircle` bumper its squared radius, filed into the boxes the engine's own `place_in_grid` walk chose. Casts are answered from Rust in every build; flippers keep the callback, since their segments are rebuilt as the bat turns. The contact point `TLine::EdgeCollision` reflects the ball from comes back through `scp_world_edge_hit_point`, which is what gated this step — and the world now applies the collision-group mask itself, where `TestGridBox` did: registering every edge as group `0xffffffff` meant nothing filtered it at all. `SCP_WORLD_CHECK` compares contact points as well as distances and prints a running cast total; 40,000 casts over a real session reported no disagreement.

* The collision world asks the engine whether an edge may be tested, instead of deciding from the component that owns it. Components flip a per-edge flag while the game runs, which the owner cannot see; this is what lets edge geometry move into the world without changing behaviour. No player-visible change.

## [2.6.5] - 2026-10-31

### Changed

* Which grid boxes a circle reaches — the last of the spatial bookkeeping in C++ — is decided in the Rust geometry crate. `circlegrid_parity` compares the box lists, in order, against the original over 25,932 cases.


## [2.6.4] - 2026-10-31

### Changed

* The Rust substep loop is now the default; `SCP_RUST_STEPPER=0` runs the original C++ loop instead, from the same binary.

## [2.6.3] - 2026-10-31

### Changed

* The ball's substep loop — the innermost collision iteration, with its ray windows, dedupe flag and break conditions — can run from `rust/geometry` with `SCP_RUST_STEPPER=1`. The C++ loop still ships as the default; `stepper_parity` compares the two, call for call, over 2,100 scripted scenarios.

## [2.6.2] - 2026-10-30

### Changed

* The per-frame ball integration — the frame slice clamp, field force, renormalised speed and substep count — runs in the Rust geometry crate. A new `ballstep_parity` test compares it with the C++ arithmetic it replaced over 24,032 cases, bit for bit, because `direction * speed + field * slice` is the expression a compiler is free to fuse.

## [2.6.1] - 2026-10-30

### Added

* `SCP_TRACE=<file>` writes one line of ball state per frame (position, direction, speed, activeness) so a recorded session can be replayed against a ported physics loop. Off unless the variable is set.

### Changed

* Ball-versus-ball collision selection runs in the Rust maths layer; ordering, tie-breaking and the 0.002 skin are unchanged.

## [2.6.0] - 2026-10-30

### Changed

* Collision box membership, the ray walk and the per-query visited marks now come from the Rust world in `rust/geometry` instead of `TEdgeManager`'s box lists and walked-edge array. Edge distances still run through the C++ edge classes, so gameplay is unchanged; `-DSCP_USE_RUST_WORLD=OFF` builds the previous grid walk.
* The debug overlay reads box contents from the world too, so the ball-edges view shows the same walls in both configurations.

### Added

* `SCP_WORLD_CHECK=1` runs the old C++ walk beside the world on every collision query and reports disagreements on stderr by DAT wall index, then prints a cast/mismatch total at exit. Diagnostic only; it doubles query cost.

## [2.5.0] - 2026-10-30

### Added

* `Options -> Fullscreen Display` picks the monitor used for fullscreen: `Automatic (window's display)` or a specific display by name. The choice is saved, re-applies immediately if fullscreen is already up, and falls back to automatic when the chosen display is no longer connected. This also gives a way out on Wayland, where the window's own display cannot be detected because GLFW reports every window at the origin.

## [2.4.2] - 2026-10-30

### Fixed

* Fullscreen opened on the wrong monitor in a multi-monitor setup. `glfwGetPrimaryMonitor()` reports the backend's primary output, not the display the window is on, so a second display (below or beside the first) received the fullscreen window. The monitor is now the one showing most of the window, with the primary kept only as the fallback for a window that is off every display. Leaving fullscreen still returns the window to where it was.
* On Wayland the game cannot read the window's position (GLFW reports every window at the origin), so there the choice resolves to the display containing the screen origin rather than the window's own display; Windows, macOS and X11 follow the window exactly.

## [2.4.1] - 2026-10-30

### Fixed

* The projection shim copied the 88-byte matrix and the projection parameters across the ABI on every call. It now refreshes a cached mirror when the projection changes (`init`/`recenter`), which a microbenchmark showed is the difference between being slower and being level with the C++ it replaced.

### Notes

* Measured on this machine (20M calls, release): the Rust maths and projection functions run at parity with their C++ originals, within a few nanoseconds per call. Porting so far is about safety and a testable boundary, not raw speed; anyone expecting a framerate win from rewriting the remaining layers should not count on one.

## [2.4.0] - 2026-10-30

### Changed

* The projection now runs in Rust (`rust/geometry`, `proj` module): matrix multiply, projected depth, the 3D-to-2D sprite transform, the reverse transform that turns a screen position back into table coordinates, and z-buffer depth normalization. The engine keeps its `proj` statics in C++ and passes them by value, so the boundary has no shared mutable state.
* Added `proj_parity`, comparing the Rust projection against the original `proj.cpp` bit for bit over 200,000 randomized matrices, parameters and points (1,000,000 comparisons).
* Depth normalization reproduces the original's out-of-range cast behaviour rather than saturating, so a depth the C++ wraps stays wrapped. Real sprite depths stay inside the 16-bit range, where the behaviour is defined.

## [2.3.0] - 2026-10-30

### Changed

* The table's collision-grid walk now runs in Rust (`rust/geometry`, `grid` module). One implementation replaces the two copies of the same Bresenham traversal in C++: `TLine::place_in_grid` files a wall segment into the boxes it spans, and `TEdgeManager::FindCollisionDistance` walks the boxes a ball ray crosses. Box visit order is unchanged, so tie-breaking between equidistant edges is identical.
* Added an `edgegrid_parity` differential test that compares the Rust walk against the pre-port C++ traversal box for box over 200,000 randomized grids and segments; `SCP_BUILD_MATHS_PARITY_TEST` became `SCP_BUILD_PARITY_TESTS` and builds both tests.
* Cargo now rebuilds when any crate source changes, not only `lib.rs`, so editing a module can no longer leave a stale static library in the build tree.
* The original C++ loop could spin forever once the walk overshot an endpoint box; the port stops there instead. No table geometry reaches that state, but a malformed table can no longer hang the physics step.

## [2.2.9] - 2026-10-30

### Changed

* The whole maths layer (`maths.cpp`) now runs in Rust when `SCP_USE_RUST_GEOMETRY=ON`, which release builds set: vector and rectangle operations, ray/circle and ray/line intersection, line construction, the ramp edge search, flipper collision probing and the ball rebound model. The C++ implementation stays as the reference and as the default for developer builds.
* Added a differential test (`-DSCP_BUILD_MATHS_PARITY_TEST=ON`, run by CI) that compiles the original C++ and the Rust port side by side and compares 20,000 randomized cases bit for bit. It caught a real divergence during the port: C++ resolves `sin(float)`/`cos(float)` to the `float` libm functions, while the first Rust version went through `f64` and differed by one ulp on about 5% of angles.

## [2.2.8] - 2026-10-30

### Changed

* Added an opt-in Rust port of rectangle merging and clipping, wired through a C ABI. Rust-backed geometry is enabled in the CI and release builds; ordinary developer builds retain the C++ implementation unless `SCP_USE_RUST_GEOMETRY=ON` is selected. The game engine remains predominantly C++; this is an incremental migration checkpoint, not a complete Rust rewrite.
* Windows and macOS release builds now compile the Rust geometry library for their target architectures, including both slices in the universal macOS build.

## [2.2.7] - 2026-10-17

### Fixed

* Self-update now properly restarts the game after an update is installed. The update check runs once per process launch via `CheckOnStartup()`, and the "Restart Now" button in the update popup correctly triggers `winmain::Restart()`, which sets `restart = true` to re-execute the game from the beginning. No manual quit and restart is required.

## [2.2.6] - 2026-10-17

### Changed

* The table's lights now glow and twinkle. The Vulkan renderer already carried
  a complete bloom and sparkle pipeline -- bright-pass, separable blur,
  additive composite and a luminance-masked twinkle -- built into pipelines and
  shipped in the binary, but nothing ever enabled it and both intensities sat
  at zero. They are now on at a fixed, tuned strength. There is deliberately no
  new preference for this; the look is part of the game's presentation rather
  than a setting, and turning it off means rebuilding.

## [2.2.5] - 2026-10-17

### Changed

* The table's lights now glow and twinkle. The Vulkan renderer already carried
  a complete bloom and sparkle pipeline -- bright-pass, separable blur,
  additive composite and a luminance-masked twinkle -- built into pipelines and
  shipped in the binary, but nothing ever enabled it and both intensities sat
  at zero. They are now on at a fixed, tuned strength. There is deliberately no
  new preference for this; the look is part of the game's presentation rather
  than a setting, and turning it off means rebuilding.

## [2.2.4] - 2026-10-17

### Fixed

* The startup update check described under 2.2.3 never ran: the option was
  read, but the check was not wired into the start-up path, so it reported
  "up to date" without ever contacting GitHub. It now runs once per launch,
  silently in the background, and the *Check for Updates on Startup* preference
  still turns it off.
* Package-manager installs could not actually install an update, so 2.2.3's
  self-update only worked for portable builds. The package type detection could
  not see `PATH`, the shell commands handed to `system()` were malformed, the
  install lock was held across the download and the polkit prompt, and the
  dialog had no *Installing* or *Installed* state. `.deb` and `.rpm` installs
  now download the new release and hand it to `pkexec apt-get install`, `dpkg`,
  `dnf` or `rpm`, with progress and a restart prompt.

### Changed

* Music is on by default. It is part of the original game's presentation, but a
  fresh profile started with it muted, so the published packages played no
  music until it was turned on by hand.
* The `.deb` and `.rpm` now **require** a General MIDI SoundFont
  (`fluid-soundfont-gm`) instead of merely recommending it, so an install with
  `--no-install-recommends` no longer loses music silently. The `.deb` also
  drops a redundant Recommends entry for `libfluidsynth3`, which
  `dpkg-shlibdeps` already records as a hard dependency from the binary.
* **Stereo Sound Effects** are on by default. Sound effects are panned by their
  position on the table and attenuated gently with distance; a centred source
  keeps the same level as it had with the previous default.

## [2.2.3] - 2026-10-17

### Added

* Self-update. The game checks the project's GitHub Releases at start-up and
  offers a newer version when there is one; **Help → Check for Updates...**
  runs the same check on demand. A *Check for Updates on Startup* preference
  (Options menu, on by default) turns the automatic check off.
* When the running binary is owned by a package manager (`.deb` or `.rpm`
  install), the update is downloaded from the release and handed to that package
  manager (`pkexec apt-get install`, `dpkg`, `dnf` or `rpm`) so it asks for
  administrator rights instead of being written over. Portable builds still
  download and launch the new version directly.
* A single version source: the new `VERSION` file feeds the game, the CPack
  packages, the NSIS installer, the DMG/AppImage/zip names and the release tag,
  and `scripts/bump-version.sh` cuts a patch release (bumps the version and
  starts the changelog and AppStream entries in one command).

### Fixed

* Bumpers, drop targets and holes made no sound when hit: their collision
  sound effect IDs were never initialised. (Flippers were unaffected.)
* The macOS release job now builds on `macos-14` as arm64 and installs its own
  Homebrew dependencies, so it runs again on current runners.
* The Windows installer and the vcpkg manifest reported the previous release's
  version; both are now derived from `VERSION`.

## [2.2.2] - 2026-10-16

### Added

* Native Linux packages: a `.deb` (Debian/Ubuntu) and a `.rpm` (Fedora/openSUSE),
  built with CPack, plus a `Platform/Linux/build-packages.sh` script to produce
  them locally. The release workflow now builds and publishes all formats.

## [2.2.1] - 2026-10-16

### Added

* An AppImage build (`Platform/Linux/build-appimage.sh`), which packages the
  game so Linux users can run it from a single file without installing
  anything.
* `Platform/Linux/BUILD.md` documents what running on Wayland needs: GLFW 3.4
  or newer selects a backend at start-up and picks Wayland, while the older 3.3
  packages are single-backend and fall back to XWayland.

### Fixed

* The in-game version string was hardcoded and had not been bumped since 2.1.1,
  so 2.2.0 builds reported the wrong version in the menu, in the stdout log and
  to the Vulkan driver. It now tracks the release version.

* MSVC builds could not compile: `<windows.h>` defines `min` and `max` as
  macros, which corrupted every `std::min`/`std::max` call in a translation
  unit that includes it (`error C2589: '(' illegal token on right side of '::'`).
  The precompiled header now defines `NOMINMAX`, so the MSVC release build and
  the Windows CI job work again. The mingw and GCC builds were unaffected.
* The Windows CI job asked for the `Visual Studio 17 2022` generator by name
  and failed with "could not find any instance of Visual Studio" once the
  runner image stopped shipping exactly that version. CMake now picks whichever
  Visual Studio is installed, as `BuildForWindows.ps1` already did.

## [2.2.0] - 2026-10-16

### Added

* Windows installer (`Platform/Windows/installer.nsi`): a single setup
  executable that installs per user - no administrator rights - with Start Menu
  and desktop shortcuts, an entry in Add/Remove Programs and an uninstaller.
  The SoundFont is an optional component, so the same script builds both a full
  and a music-less package. `Platform/Windows/build-installer.sh` compiles it
  with NSIS, natively or through Wine on Linux.
* Windows release packaging in CI: `.github/workflows/ReleaseBuilds.yml` now
  builds and uploads the x86 and x64 zips produced by `BuildForWindows.ps1`.
* The game looks for a SoundFont next to the executable
  (`soundfonts/FluidR3_GM.sf2`, plus a few other common names) before the
  system-wide locations, so a packaged or portable build plays music without
  setting `SCP_SOUNDFONT`.
* `.gitattributes` marking the vendored libraries (Dear ImGui, miniaudio) so the
  GitHub language breakdown reflects project code.

### Changed

* `BuildForWindows.ps1` fails loudly instead of producing an empty package: it
  accepts `VCPKG_INSTALLATION_ROOT` when `VCPKG_ROOT` is not set, checks the
  exit code of every `cmake` call and verifies that
  `SpaceCadetPinball.exe` was produced.
* CI runs on every branch and tag, not only on `master`/`main`.
* Version bumped to 2.2.0 in `CMakeLists.txt` (CPack), `vcpkg.json`,
  `build-mac-app.sh` and the Linux AppStream metadata.

### Fixed

* Control bindings are migrated once when upgrading from the SDL2-based build:
  key and button codes are stored as raw library-specific numbers, and GLFW
  numbers them differently than SDL2, so an old settings file silently
  scrambled every control. A `Control Bindings Version` marker makes the reset
  happen exactly once; high scores and all other settings are kept.
* The menu bar no longer swallows clicks meant for the flippers. The menu strip
  is an always-present transparent window, so hovering it made ImGui claim the
  mouse and clicks near the top of the table did nothing.
* The game no longer spins a CPU core while its window is unfocused: it waits
  for events with a growing timeout instead of polling in a tight loop.

## [2.1.1] - 2026-10-15

Platform modernization release.

### Changed

* Replaced SDL2 with GLFW for windowing and input.
* Replaced SDL2 rendering with Vulkan (MoltenVK on macOS).
* Replaced SDL2_mixer with a built-in miniaudio software mixer.
* MIDI music is rendered by FluidSynth, using a General MIDI SoundFont, and is
  available as the optional `music` vcpkg feature; the game runs normally
  without it.
* Added Windows and Linux CI workflows.
* Documentation: added the remaining console ports to the README and Homebrew
  installation commands; updated the macOS CI runner and extended macOS
  compatibility.

### Fixed

* Initial input focus for the hidden menu.
* Plunger pullback speed in 3DPB mode.
* The Exit menu item did nothing.

## [2.1.0] - 2023-10-16

Feature release focusing on Full Tilt! compatibility.

* FT: collision system changes, table control changes, multiball, three MIDI
  tracks.
* Work in progress: localization support. Depends on external font selection for
  most languages.
* Hardware accelerated ImGui renderer.
* Experimental stereo sound support.
* UX improvements and bug fixes.

## [2.0.1] - 2021-12-29

First bug fix release of the cross-platform port.

* Improved Linux support: install target, icons, desktop shortcut, alternative
  data paths.
* Improved game controller support.
* High precision sleep mode for ~0 frame time jitter.
* Integer image scaling mode.
* 3DPB <-> FT data switch option, for easier data set changes when both are
  available.
* FT demo data support and bug fixes.

## [2.0.0] - 2021-10-30

First release of the cross-platform port.

* Windows dependencies removed.
* Graphics and sound rendered with SDL, GUI in Dear ImGui.
* CMake build script, compiles on many platforms.
* All features of the 1.x line are present, except for text translations.

The 1.x tags are the Windows-only port of the decompiled code and predate the
cross-platform rewrite.

# Changelog

All notable changes to this project are documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and
releases are tagged `Release_<version>`, matching the tags of the upstream
project this is forked from.

Game data and SoundFonts are never distributed with the source, so no entry
below concerns them.

## [Unreleased]

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

# Changelog

All notable changes to this project are documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and
releases are tagged `Release_<version>`, matching the tags of the upstream
project this is forked from.

Game data and SoundFonts are never distributed with the source, so no entry
below concerns them.

## [Unreleased]

### Fixed

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

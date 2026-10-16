# Building the Windows installer

`installer.nsi` packs a folder of build output and game data into one setup
executable: the game, its runtime DLLs, the original game data and - when
present - a General MIDI SoundFont.  The result is a per-user install (no
administrator rights, no UAC prompt) with Start Menu and desktop shortcuts, an
entry in Add/Remove Programs and an uninstaller.

## 1. Prepare `bin\`

The installer reads from the repository's `bin\` folder, which must contain:

| File | Source |
| ---- | ------ |
| `SpaceCadetPinball.exe` | release build, configured with `-DCMAKE_WIN32_EXECUTABLE:BOOL=1` |
| `*.dll` | the runtime dependencies copied next to the executable, e.g. `libfluidsynth-3.dll` and `sndfile.dll` for music |
| `PINBALL.DAT`, `FONT.DAT`, `SOUND*.WAV`, `PINBALL*.MID` | original game data (`CADET.DAT` / `DEMO.DAT` for the Full Tilt data sets) |
| `soundfonts\*.sf2` | optional, a General MIDI SoundFont such as `FluidR3_GM.sf2` |

`Bin\Release` is what `BuildForWindows.ps1` fills in on the MSVC path, so
`bin\` can be prepared by copying that next to the game data.

The game data and the SoundFont are **not** part of this repository and are not
covered by its license.  The SoundFont section of the installer is optional -
without it the game runs fine, just without music.

## 2. Compile

On Windows, with [NSIS](https://nsis.sourceforge.io) installed:

```powershell
cd Platform\Windows
& "$env:ProgramFiles(x86)\NSIS\makensis.exe" installer.nsi
```

On Linux, through Wine (no NSIS package needed):

```bash
NSISDIR=~/nsis/nsis-3.10 ./build-installer.sh
```

Either way the installer lands in `bin\SpaceCadetPinball-Setup.exe`.

`makensis` options:

* `/DSRCDIR=<path>` - read the payload from somewhere else than the repository
  root (must be a Windows path when running through Wine).
* `/DDATASIZE=<KB>` - size shown in Add/Remove Programs; `build-installer.sh`
  computes it from `bin\`.

## 3. Install it

```powershell
SpaceCadetPinball-Setup.exe                      # interactive
SpaceCadetPinball-Setup.exe /S /D=C:\Games\SpaceCadetPinball   # silent, /D= last and unquoted
```

Uninstall with `Uninstall.exe` from the install folder, or from
*Settings > Apps > Installed apps*.  Options and high scores live in
`%APPDATA%\SpaceCadetPinball` and are kept.

## 4. Portable build

For users who would rather not install anything, `build-portable.sh` packs the
same `bin\` folder into a zip:

```bash
./build-portable.sh
```

It writes `bin\SpaceCadetPinball-<version>-win64-portable.zip`, holding the
game, its runtime DLLs, the game data and the SoundFont in one folder, with
`PLAYER-README.txt` as `README.txt`.  Unpack and run; nothing is installed and
nothing is written outside that folder.

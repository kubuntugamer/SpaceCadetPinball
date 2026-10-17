#!/usr/bin/env bash
#
# Builds bin\SpaceCadetPinball-<version>-win64-portable.zip from the contents of
# bin\ - the unpack-and-run Windows build: the game, its runtime DLLs, the game
# data and, when present, the SoundFont.  Nothing is installed and nothing is
# written outside the folder, so it needs no administrator rights.
#
#   ./build-portable.sh
#
# See BUILD.md for what has to be in bin\ first.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

# Single source of truth for the version: the VERSION file at the repo root.
VERSION="$(tr -d '[:space:]' < "$ROOT/VERSION")"
[ -n "$VERSION" ] || { echo "Could not read the version from VERSION" >&2; exit 1; }

BIN="$ROOT/bin"
EXENAME="SpaceCadetPinball.exe"
[ -f "$BIN/$EXENAME" ] || { echo "$BIN/$EXENAME is missing - see BUILD.md" >&2; exit 1; }

NAME="SpaceCadetPinball-$VERSION-win64-portable"
OUT="$BIN/$NAME.zip"
STAGE="$(mktemp -d)/$NAME"
trap 'rm -rf "$(dirname "$STAGE")"' EXIT
mkdir -p "$STAGE"

cp "$BIN/$EXENAME" "$STAGE/"
# Runtime dependencies of the build (FluidSynth and its codec library, ...):
# whatever the build copied next to the executable.
cp "$BIN"/*.dll "$STAGE/" 2>/dev/null || true
# Original game data, provided by the user - not part of this repository.
cp "$BIN"/*.DAT "$BIN"/*.MID "$BIN"/SOUND*.WAV "$STAGE/" 2>/dev/null || true
# Optional SoundFont, so the portable build has music too.
if [ -d "$BIN/soundfonts" ]; then
	mkdir -p "$STAGE/soundfonts"
	cp "$BIN"/soundfonts/*.sf2 "$BIN"/soundfonts/*.sf3 "$STAGE/soundfonts/" 2>/dev/null || true
fi
cp "$HERE/PLAYER-README.txt" "$STAGE/README.txt"

# Guard against packaging build leftovers: the Linux binary, the Wine launcher
# and the installer all live in bin\ as well, and none of them belong in here.
for forbidden in SpaceCadetPinball play-windows.sh SpaceCadetPinball-Setup.exe "$NAME.zip"; do
	if [ -e "$STAGE/$forbidden" ]; then
		echo "refusing to package: $forbidden ended up in the staging folder" >&2
		exit 1
	fi
done

rm -f "$OUT"
(cd "$(dirname "$STAGE")" && zip -qr "$OUT" "$NAME")

echo
ls -l "$OUT"
echo "files:  $(unzip -l "$OUT" | tail -1 | awk '{print $2}')"
echo "SHA256: $(sha256sum "$OUT" | cut -d' ' -f1)"

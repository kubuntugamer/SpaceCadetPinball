#!/usr/bin/env bash
#
# Builds bin\SpaceCadetPinball-Setup.exe from the contents of bin\ with NSIS.
#
# On Windows, or with the "nsis" package installed on Linux, a native
# makensis is used.  Otherwise the official NSIS Windows build is run through
# Wine, which needs no packages at all:
#
#   ./build-installer.sh                      # native makensis from PATH
#   NSISDIR=~/nsis/nsis-3.10 ./build-installer.sh   # run through Wine
#
# See BUILD.md for what has to be in bin\ first.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

if command -v makensis >/dev/null 2>&1; then
	makensis=(makensis)
	winsrc="$ROOT"
elif [ -n "${NSISDIR:-}" ] && [ -f "$NSISDIR/makensis.exe" ]; then
	command -v wine >/dev/null 2>&1 || {
		echo "NSISDIR is set but wine is not installed" >&2
		exit 1
	}
	makensis=(wine "$NSISDIR/makensis.exe")
	# NSIS is a Windows program under Wine, so it needs a Windows path.
	winsrc="$(winepath -w "$ROOT" | tr -d '\r')"
else
	echo "No makensis found. Install NSIS (https://nsis.sourceforge.io) or set" >&2
	echo "NSISDIR to an unpacked NSIS release and install wine." >&2
	exit 1
fi

[ -f "$ROOT/bin/SpaceCadetPinball.exe" ] || {
	echo "$ROOT/bin/SpaceCadetPinball.exe is missing - see BUILD.md" >&2
	exit 1
}

# Size (KB) shown in Add/Remove Programs.
datasize="$(du -sk "$ROOT/bin" | cut -f1)"

echo "makensis  : ${makensis[*]}"
echo "source    : $winsrc"
echo "size      : ${datasize} KB"

# makensis reads any argument starting with '/' as an option, so the script has
# to be named relative to the working directory.
cd "$HERE"
WINEDEBUG="${WINEDEBUG:-}" "${makensis[@]}" /V3 /DSRCDIR="$winsrc" /DDATASIZE="$datasize" installer.nsi

echo
ls -l "$ROOT/bin/SpaceCadetPinball-Setup.exe"

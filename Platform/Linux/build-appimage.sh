#!/usr/bin/env bash
#
# Builds bin/SpaceCadetPinball-<version>-x86_64.AppImage from the Linux build in
# bin/ - the single-file format that runs without installing anything.
#
# Needs appimagetool, downloaded on first use into ~/.cache (or point
# APPIMAGETOOL at an existing one). Everything else it needs it does itself.
#
#   ./build-appimage.sh
#
# The AppImage carries the game, its data and the libraries the build needs.
# A SoundFont is deliberately NOT bundled (it is 140 MB and most distributions
# ship one already); music uses the system SoundFont or SCP_SOUNDFONT.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
APPNAME="SpaceCadetPinball"

# Single source of truth for the version: the constant the game itself reports.
VERSION="$(sed -n 's/.*Version = "\([0-9.]*\)".*/\1/p' "$ROOT/SpaceCadetPinball/winmain.h" | head -1)"
[ -n "$VERSION" ] || { echo "Could not read the version from winmain.h" >&2; exit 1; }

BIN="$ROOT/bin"
[ -x "$BIN/$APPNAME" ] || { echo "$BIN/$APPNAME is missing - build it first (see BUILD.md)" >&2; exit 1; }

ARCH="${ARCH:-x86_64}"
OUT="$BIN/$APPNAME-$VERSION-$ARCH.AppImage"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
APPDIR="$WORK/$APPNAME.AppDir"

# Libraries that must come from the host: glibc and the C++/GCC runtime, which
# are tied to the loader, plus the display server, GPU driver and the libraries
# that talk to a system daemon, where a bundled copy has to match the machine.
# Matched by prefix, so libX11 covers libX11-xcb and so on.
#
# libpulse is deliberately NOT here: FluidSynth links libpulse-simple directly,
# so leaving it out makes the whole AppImage refuse to start on a machine
# without libpulse installed. Bundling the client libraries instead keeps the
# pulse client and its private libpulsecommon in step with each other.
host_libs="ld-linux libc.so libm.so libdl.so libpthread.so librt.so libstdc++ libgcc_s \
	libGL libEGL libGLX libOpenGL libGLdispatch libvulkan libdrm libgbm \
	libX11 libxcb libXext libXrandr libXi libXcursor libXfixes libXrender \
	libwayland libxkbcommon libasound libdbus libsystemd libapparmor \
	libudev libselinux libresolv"

is_host_lib() {
	for prefix in $host_libs; do
		case "$1" in "$prefix"*) return 0 ;; esac
	done
	return 1
}

# --- appimagetool -----------------------------------------------------------
TOOL="${APPIMAGETOOL:-$HOME/.cache/appimagetool-$ARCH.AppImage}"
if [ ! -x "$TOOL" ]; then
	echo "Downloading appimagetool to $TOOL"
	mkdir -p "$(dirname "$TOOL")"
	curl -fsSL -o "$TOOL" \
		"https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-$ARCH.AppImage"
	chmod +x "$TOOL"
fi

# --- AppDir layout ----------------------------------------------------------
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib" \
	"$APPDIR/usr/share/metainfo" "$APPDIR/usr/share/applications"
for s in 16 32 48 128 192; do
	mkdir -p "$APPDIR/usr/share/icons/hicolor/${s}x${s}/apps"
	cp "$ROOT/$APPNAME/Icon_${s}x${s}.png" "$APPDIR/usr/share/icons/hicolor/${s}x${s}/apps/$APPNAME.png"
done

cp "$BIN/$APPNAME" "$APPDIR/usr/bin/"
# The game looks for its data next to the executable, so keep it there.
cp "$BIN"/*.DAT "$BIN"/*.MID "$BIN"/SOUND*.WAV "$APPDIR/usr/bin/" 2>/dev/null || true
cp "$HERE/$APPNAME.desktop" "$APPDIR/usr/share/applications/"
cp "$HERE/$APPNAME.metainfo.xml" "$APPDIR/usr/share/metainfo/"

# appimagetool wants the .desktop and an icon at the AppDir root.
cp "$HERE/$APPNAME.desktop" "$APPDIR/"
cp "$ROOT/$APPNAME/Icon_128x128.png" "$APPDIR/$APPNAME.png"

cat > "$APPDIR/AppRun" <<'EOF'
#!/bin/sh
# Only the bundled libraries are exported, so the game keeps using the host's
# Vulkan driver, display server and audio.
HERE="$(dirname "$(readlink -f "$0")")"
export LD_LIBRARY_PATH="$HERE/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$HERE/usr/bin/SpaceCadetPinball" "$@"
EOF
chmod +x "$APPDIR/AppRun"

# --- bundle the libraries the build needs -----------------------------------
# Walk the dependency closure of the game, copying everything that is not a
# host library. The transitive part matters: FluidSynth pulls in a dozen
# libraries of its own that a machine without it installed will not have.
queue=("$APPDIR/usr/bin/$APPNAME")
declare -A seen=()
copied=0
for ((i = 0; i < ${#queue[@]}; i++)); do
	lib="${queue[i]}"
	[ -n "${seen[$lib]:-}" ] && continue
	seen[$lib]=1
	for dep in $(ldd "$lib" 2>/dev/null | awk '{print $3}' | grep '^/' | sort -u); do
		name="$(basename "$dep")"
		is_host_lib "$name" && continue
		[ -e "$APPDIR/usr/lib/$name" ] && continue
		cp -L "$dep" "$APPDIR/usr/lib/$name"
		copied=$((copied + 1))
		queue+=("$dep")
	done
done
echo "Bundled $copied libraries"

# --- build ------------------------------------------------------------------
# --no-appstream: the metainfo ships inside the AppImage but is not validated
# here, since that needs appstreamcli, which is not a build dependency.
ARCH="$ARCH" "$TOOL" --no-appstream "$APPDIR" "$OUT"

echo
ls -l "$OUT"
echo "SHA256: $(sha256sum "$OUT" | cut -d' ' -f1)"

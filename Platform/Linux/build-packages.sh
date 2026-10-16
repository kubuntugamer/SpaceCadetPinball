#!/usr/bin/env bash
#
# Builds native Linux packages from the build in bin/ using CPack:
#   - .deb (Debian/Ubuntu)
#   - .rpm (Fedora/openSUSE) when rpmbuild is available
#   - .tar.gz / .zip (generic)
#
# Build first:
#   cmake -GNinja . && ninja
# Then:
#   ./build-packages.sh
#
# Output lands in build/ (and is copied to bin/ for convenience).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"

[ -x "$BUILD/SpaceCadetPinball" -o -f "$BUILD/SpaceCadetPinball" ] || {
    # the binary lives in bin/, but CPack runs from the build dir
    [ -d "$BUILD" ] || { echo "No build/ directory - configure with cmake first" >&2; exit 1; }
}

cd "$BUILD"

# Generate every format CPack knows about (DEB, RPM, TGZ, ZIP). RPM is skipped
# gracefully when rpmbuild is not installed.
GENERATORS="DEB;TGZ;ZIP"
if command -v rpmbuild >/dev/null 2>&1; then
    GENERATORS="DEB;RPM;TGZ;ZIP"
fi

cpack -G "$GENERATORS"

# Copy the resulting packages next to the binary for convenience.
for f in *.deb *.rpm *.tar.gz *.zip; do
    [ -e "$f" ] && cp -f "$f" "$ROOT/bin/" 2>/dev/null || true
done

echo
echo "Packages:"
ls -l "$BUILD"/*.deb "$BUILD"/*.rpm "$BUILD"/*.tar.gz "$BUILD"/*.zip 2>/dev/null || true

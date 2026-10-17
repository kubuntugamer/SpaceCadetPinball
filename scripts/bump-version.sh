#!/usr/bin/env bash
#
# Bumps the release version for a patch/point (or minor/major) release.
#
# The VERSION file at the repo root is the single source of truth; this script
# updates it and the three places that carry a per-release value by hand:
# CHANGELOG.md, Platform/Linux/SpaceCadetPinball.metainfo.xml (AppStream) and
# vcpkg.json. Everything else (game, CPack packages, NSIS installer, DMG and
# AppImage names, the release tag) reads VERSION at build time, so it needs no
# edit.
#
#   ./scripts/bump-version.sh patch        # 2.2.2 -> 2.2.3
#   ./scripts/bump-version.sh minor        # 2.2.2 -> 2.3.0
#   ./scripts/bump-version.sh major        # 2.2.2 -> 3.0.0
#   ./scripts/bump-version.sh 2.3.1        # explicit version
#   ./scripts/bump-version.sh patch --dry-run
#   ./scripts/bump-version.sh --tag        # tag an already-committed VERSION
#
# The tag is created in a second step so it lands on the commit that contains
# the bump, not on the one before it.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION_FILE="$ROOT/VERSION"
CHANGELOG="$ROOT/CHANGELOG.md"
METAINFO="$ROOT/Platform/Linux/SpaceCadetPinball.metainfo.xml"

usage() {
	awk 'NR == 1 { next } /^#/ { sub(/^# ?/, ""); print; next } { exit }' "$0"
}

die() { echo "error: $*" >&2; exit 1; }

[ -f "$VERSION_FILE" ] || die "no VERSION file at $VERSION_FILE"

arg="${1:-}"
[ -n "$arg" ] || { usage; exit 1; }
shift || true

dry_run=0
want_tag=0
while [ $# -gt 0 ]; do
	case "$1" in
		--dry-run|-n) dry_run=1 ;;
		--tag|-t) want_tag=1 ;;
		-h|--help) usage; exit 0 ;;
		*) die "unknown option: $1" ;;
	esac
	shift
done

# `--tag` on its own is a separate mode: it tags the version already in VERSION,
# so it must run on a clean tree *after* the bump has been committed. Tagging
# from inside the bump would land on the previous commit (the bump is uncommitted
# at that point), or on a tree that still has unsaid edits in it.
if [ "$arg" = "--tag" ] || [ "$arg" = "-t" ]; then
	want_tag=1
fi

if [ "$want_tag" = 1 ] && [ "$arg" != "--tag" ] && [ "$arg" != "-t" ]; then
	die "commit the bump first, then run: $(basename "$0") --tag"
fi

if [ "$want_tag" = 1 ]; then
	[ -z "${2:-}" ] || die "unknown argument: $2"
	if ! git -C "$ROOT" diff --quiet HEAD 2>/dev/null ||
	   ! git -C "$ROOT" diff --cached --quiet HEAD 2>/dev/null; then
		die "--tag needs a clean tree: commit first, then run this"
	fi
	current="$(tr -d '[:space:]' < "$VERSION_FILE")"
	[[ "$current" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] ||
		die "VERSION must look like X.Y.Z, got '$current'"
	tag="Release_${current}"
	if git -C "$ROOT" rev-parse -q --verify "refs/tags/$tag" >/dev/null 2>&1; then
		die "tag $tag already exists"
	fi
	git -C "$ROOT" tag "$tag"
	echo "created git tag $tag -> $(git -C "$ROOT" rev-parse --short HEAD)"
	echo
	echo "Next steps:"
	echo "  git push origin master"
	echo "  git push origin $tag   # triggers the release build"
	exit 0
fi

old="$(tr -d '[:space:]' < "$VERSION_FILE")"
[[ "$old" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "VERSION must look like X.Y.Z, got '$old'"

IFS=. read -r major minor patch <<<"$old"

case "$arg" in
	patch) new="${major}.${minor}.$((patch + 1))" ;;
	minor) new="${major}.$((minor + 1)).0" ;;
	major) new="$((major + 1)).0.0" ;;
	*)
		new="$arg"
		[[ "$new" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] ||
			die "expected 'patch', 'minor', 'major' or an X.Y.Z version, got '$arg'"
		;;
esac

[ "$new" = "$old" ] && die "version is already $old"

if grep -qE "^## \[$new\]" "$CHANGELOG" 2>/dev/null; then
	die "CHANGELOG.md already has an entry for $new"
fi

today="$(date +%F)"
tag="Release_${new}"

echo "bump: $old -> $new  ($today, tag $tag)"

if [ "$dry_run" = 1 ]; then
	echo "dry run: nothing written"
	exit 0
fi

printf '%s\n' "$new" > "$VERSION_FILE"
echo "  updated VERSION"

# CHANGELOG.md: insert the new section right under the version header line.
python3 - "$CHANGELOG" "$new" "$today" <<'PY'
import sys, io
path, ver, date = sys.argv[1], sys.argv[2], sys.argv[3]
text = io.open(path, encoding="utf-8").read()
marker = "## ["
i = text.find(marker)
if i < 0:
    raise SystemExit("CHANGELOG.md: no '## [' heading found to insert before")
# A pending "## [Unreleased]" block stays at the very top, so a new release
# section belongs underneath it rather than above it.
if text.startswith("## [Unreleased]", i):
    j = text.find(marker, i + len(marker))
    if j > 0:
        i = j
stub = "## [%s] - %s\n\n<!-- Describe this release below, then remove this line. -->\n\n" % (ver, date)
text = text[:i] + stub + text[i:]
io.open(path, "w", encoding="utf-8").write(text)
print("  updated CHANGELOG.md")
PY

# AppStream metainfo: insert a <release> entry right after <releases>.
python3 - "$METAINFO" "$new" "$today" <<'PY'
import sys, io
path, ver, date = sys.argv[1], sys.argv[2], sys.argv[3]
text = io.open(path, encoding="utf-8").read()
open_tag = "<releases>"
i = text.find(open_tag)
if i < 0:
    raise SystemExit("metainfo: no <releases> element found to insert into")
entry = (
    '\n        <release version="%s" date="%s">\n'
    '            <description>\n'
    '                <p><!-- Fill in. --></p>\n'
    '            </description>\n'
    '        </release>'
) % (ver, date)
text = text[:i + len(open_tag)] + entry + text[i + len(open_tag):]
io.open(path, "w", encoding="utf-8").write(text)
print("  updated SpaceCadetPinball.metainfo.xml")
PY

# vcpkg manifest: keeps its own version field in step with the release.
python3 - "$ROOT/vcpkg.json" "$new" <<'PY'
import sys, io, json, collections
path, ver = sys.argv[1], sys.argv[2]
text = io.open(path, encoding="utf-8").read()
data = json.loads(text, object_pairs_hook=collections.OrderedDict)
for key in ("version-string", "version"):
    if key in data:
        data[key] = ver
        break
else:
    data["version-string"] = ver
io.open(path, "w", encoding="utf-8").write(
    json.dumps(data, indent=2, ensure_ascii=False) + "\n")
print("  updated vcpkg.json")
PY

echo
echo "Next steps:"
echo "  1. Fill in the CHANGELOG.md and metainfo entries for $new"
echo "  2. git commit -am 'Bump version to $new'"
echo "  3. $(basename "$0") --tag   # creates $tag on that commit"
echo "  4. git push origin master && git push origin $tag   # triggers the release"

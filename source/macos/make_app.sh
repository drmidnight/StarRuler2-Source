#!/bin/bash
#Packages the macOS build into an ad-hoc signed app bundle and a disk image.
#Usage, from the repository root after building:
#  bash source/macos/build_deps.sh            (once, for the static libraries)
#  make -f source/linux/Makefile compile
#  bash source/macos/make_app.sh
#Output: dist/Star Ruler 2.app and dist/StarRuler2-<version>-macos-arm64.dmg
set -euo pipefail

BIN=bin/osx64/StarRuler2.bin
[[ -x "$BIN" ]] || { echo "Missing $BIN, build the game first" >&2; exit 1; }

#Refuse binaries that would not run on other Macs
if otool -L "$BIN" | tail -n +2 | awk '{print $1}' | grep -v -e '^/System/Library/' -e '^/usr/lib/' ; then
	echo "The binary links libraries outside the system, run source/macos/build_deps.sh and rebuild" >&2
	exit 1
fi
MINOS="$(otool -l "$BIN" | grep -A4 LC_BUILD_VERSION | awk '/minos/ {print $2; exit}')"

VERSION="$(sed -n 's/.*GAME_VERSION = "v\{0,1\}\([^"]*\)".*/\1/p' scripts/definitions/version.as | head -1)"
BUILD="$(git rev-list --count HEAD 2>/dev/null || echo 1)"
COMMIT="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"

DIST=dist
APP="$DIST/Star Ruler 2.app"
DMG="$DIST/StarRuler2-$VERSION-macos-arm64.dmg"
#The disk image layout is staged outside the repository: its Applications link
#must not end up inside the game folder, which the game scans as the base mod
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

rm -rf "$DIST/dmg" "$APP" "$DMG"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

cp "$BIN" "$APP/Contents/MacOS/StarRuler2"
#The debug map is only needed to rebuild the dSYM; function names stay for crash traces
strip -S "$APP/Contents/MacOS/StarRuler2"
for dir in data scripts locales maps mods; do
	rsync -a --exclude '.DS_Store' "$dir" "$APP/Contents/Resources/"
done
cp COPYING credits.txt README.md "$APP/Contents/Resources/"
cp source/macos/StarRuler2.icns "$APP/Contents/Resources/"
sed -e "s/@VERSION@/$VERSION/" -e "s/@BUILD@/$BUILD/" -e "s/@MINOS@/$MINOS/" \
	source/macos/Info.plist.in > "$APP/Contents/Info.plist"
plutil -lint "$APP/Contents/Info.plist" >/dev/null
printf 'APPL????' > "$APP/Contents/PkgInfo"

#Ad-hoc signature: no developer identity, required for arm64 code to run at all
codesign --force --sign - --timestamp=none "$APP"
codesign --verify --strict "$APP"

ditto "$APP" "$STAGE/Star Ruler 2.app"
ln -s /Applications "$STAGE/Applications"
cp COPYING "$STAGE/License.txt"
cat > "$STAGE/Read Me.txt" <<TXT
Star Ruler 2 $VERSION for Apple Silicon Macs (macOS $MINOS or later), build $COMMIT.

Drag Star Ruler 2 into Applications.

This build is not notarized. The first time you open it macOS will refuse.
Open System Settings > Privacy & Security, scroll down and click "Open Anyway",
or run this once in Terminal:

    xattr -dr com.apple.quarantine "/Applications/Star Ruler 2.app"

Settings, saves and logs are kept in ~/.starruler2.
Music is not part of the open source release; copy data/music from a commercial
copy into Star Ruler 2.app/Contents/Resources/data/ to add it.
TXT

hdiutil create -quiet -volname "Star Ruler 2" -srcfolder "$STAGE" -fs HFS+ -format ULMO -ov "$DMG"
echo "Created $APP"
echo "Created $DMG ($(du -h "$DMG" | cut -f1))"

#Kept next to the disk image, not shipped: symbolicates crash logs players attach
if [[ -d "$BIN.dSYM" ]]; then
	DSYM="$DIST/StarRuler2-$VERSION-$COMMIT.dSYM"
	rm -rf "$DSYM"
	ditto "$BIN.dSYM" "$DSYM"
	echo "Created $DSYM"
fi

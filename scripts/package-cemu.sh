#!/usr/bin/env bash
# Package the PS4 Cemu build (after configure-cemu.sh + build-cemu.sh) as Crema, CREM00001
# (IV0000-CREM00001_00-CREMA00000000000.pkg; before v01.17 it was CEMU00001): eboot.bin, Cemu's
# data folders (resources, gameProfiles) and the launcher's assets in /app0.
#   CEMU_PS4_VERSION=01.00 package-cemu.sh [upload]
# upload: puts the pkg in the console's /data/pkg (install it with GoldHEN's Package Installer).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/ps4-env.sh"
SRC="${CEMU_SRC:-$PS4_BUILD_ROOT/src/Cemu}"
BUILD="$PS4_BUILD_ROOT/build/cemu"
EBOOT="$BUILD/src/CemuBin_eboot/eboot.bin"
VERSION="${CEMU_PS4_VERSION:-01.00}"
HOST="ftp://${PS4_HOST:-192.168.0.90}:${PS4_FTP_PORT:-2121}"

[ -f "$EBOOT" ] || { echo "missing $EBOOT: build first" >&2; exit 1; }
[[ "$VERSION" =~ ^[0-9][0-9]\.[0-9][0-9]$ ]] || { echo "version must look like 01.00" >&2; exit 1; }
STAGE="$PS4_BUILD_ROOT/build/cemu-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/sce_sys"
cp "$EBOOT" "$STAGE/eboot.bin"
cp "$HERE/../sce_sys/icon0.png" "$STAGE/sce_sys/"
cp -r "$SRC/bin/resources" "$SRC/bin/gameProfiles" "$STAGE/"
# the launcher's fonts, sounds, languages (xmb/, from DolphinPS4) and artwork (launcher/)
cp -r "$HERE/../launcher-assets/xmb" "$HERE/../launcher-assets/launcher" "$STAGE/"
echo "$VERSION" > "$STAGE/version.txt"
# PkgTool's pkg_build crashes ("Sequence contains no elements") on empty folders.
find "$STAGE" -type d -empty -delete
# Vulkan (RADV on GNM) runs under the plain homebrew identity (4.5 GiB of direct memory).
PKG="$(SFO_STYLE=plain NO_RIGHT_SPRX=1 "$HERE/make-pkg.sh" "$STAGE" CREM00001 "Crema" "$VERSION" CREMA "$PS4_BUILD_ROOT/out" | tail -1)"
ls -la "$PKG"
# a copy next to the repo (E:\PS4 Projects\CEMUPS4\out), easy to find from Windows; *.pkg is ignored by git
mkdir -p "$HERE/../out"
cp "$PKG" "$HERE/../out/Crema-v$VERSION.pkg"
# Keep the symbols of every packaged build: crash logs must be symbolized against the exact binary.
mkdir -p "$PS4_BUILD_ROOT/oelf"
cp "$BUILD/src/CemuBin_eboot/CemuBin.oelf" "$PS4_BUILD_ROOT/oelf/v$VERSION.oelf"
cp "$SRC/bin/Cemu_release" "$PS4_BUILD_ROOT/oelf/v$VERSION.elf"
if [ "${1:-}" = upload ]; then
    curl -sS -T "$PKG" "$HOST/data/pkg/"
    echo "uploaded $(basename "$PKG")"
    # the package from before the rename (CEMU00001), superseded: /data/pkg stays clean
    curl -sS -Q "DELE /data/pkg/IV0000-CEMU00001_00-CEMUPS4000000000.pkg" "$HOST/" -o /dev/null 2>/dev/null &&
        echo "removed the old IV0000-CEMU00001_00-CEMUPS4000000000.pkg from /data/pkg" || true
    # (The test homebrew in tests/out is uploaded by hand now: creating data_dir/games from here
    # before the app has run made it skip moving the old /data/CemuPS4 into /data/Crema.)
fi

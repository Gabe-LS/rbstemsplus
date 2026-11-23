#!/bin/sh
# Regenerate the checked-in Windows icon with ImageMagick (7.1.2-35).
# The Mac Icon Composer bundle's artwork is already a full-bleed square.
set -eu
root="$(cd "$(dirname "$0")/.." && pwd)"
shell="$root/win/shell"
tmp="$(mktemp -d "$shell/.icon.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT

magick "$root/app/icon/RBStemsPlus.icon/Assets/artwork.png" \
    -strip -alpha on -type TrueColorAlpha \
    -define icon:auto-resize=16,20,24,32,40,48,64,128,256 \
    -define icon:png-compression-size=256 "$tmp/RbStemsPlus.ico"
mv -f "$tmp/RbStemsPlus.ico" "$shell/RbStemsPlus.ico"

#!/bin/bash
# The minos gate: every slice of every Mach-O must ask for macOS 12.0 or older, so that macOS 12
# still opens the app. Run by scripts/build-app.sh on every build.
#
#   tests/mac/minos.sh [--max X.Y] PATH...
#
# PATH: an app bundle or any folder (every Mach-O file in it, links not followed), an app zip
# (unpacked with ditto into a scratch folder), or one Mach-O file (the Mac bridge, say). Prints one
# line per slice: "minos 12.0  arm64  Contents/MacOS/RB Stems Plus". The minimum comes from
# LC_BUILD_VERSION, or from LC_VERSION_MIN_MACOSX in older load commands. Fails when a slice asks
# for more than --max (default 12.0), when a slice has neither load command, or when a PATH holds
# no Mach-O at all.
set -euo pipefail
max="12.0"
die() { echo "minos.sh: $*" >&2; exit 2; }
usage() { echo "usage: $0 [--max X.Y] PATH..." >&2; exit 2; }
if [ "${1:-}" = "--max" ]; then
  [ $# -ge 2 ] || usage
  max="$2"; shift 2
fi
[ $# -ge 1 ] || usage
[[ "$max" =~ ^[0-9]+(\.[0-9]+){0,2}$ ]] || die "--max must be a version like 12.0, not '$max'"

# ver_gt A B: true when version A is newer than B (missing parts count as 0)
ver_gt() {
  local IFS=. i
  local -a a=($1) b=($2)
  for i in 0 1 2; do
    if [ "${a[i]:-0}" -gt "${b[i]:-0}" ]; then return 0; fi
    if [ "${a[i]:-0}" -lt "${b[i]:-0}" ]; then return 1; fi
  done
  return 1
}

# is_macho FILE: a Mach-O file or a universal one (by its first four bytes)
is_macho() {
  case "$(head -c 4 "$1" | od -An -tx1 | tr -d ' \n')" in
    feedfacf|cffaedfe|feedface|cefaedfe|cafebabe|bebafeca|cafebabf|bfbafeca) return 0 ;;
  esac
  return 1
}

# minos_of FILE ARCH: the slice's minimum macOS, or nothing
minos_of() {
  otool -l -arch "$2" "$1" | awk '
    $1 == "cmd" { cmd = $2; next }
    cmd == "LC_BUILD_VERSION" && $1 == "platform" { plat = $2; next }
    cmd == "LC_BUILD_VERSION" && $1 == "minos" { if (plat == 1 || plat == "MACOS" || plat == "macos") { print $2; exit } }
    cmd == "LC_VERSION_MIN_MACOSX" && $1 == "version" { print $2; exit }'
}

tmp=""
trap '[ -z "$tmp" ] || rm -rf "${tmp:?}"' EXIT
bad=0
for path in "$@"; do
  root="$path"
  if [ -f "$path" ] && [[ "$path" == *.zip ]]; then
    [ -n "$tmp" ] || tmp="$(mktemp -d "${TMPDIR:-/tmp}/rbsp-minos.XXXXXX")"
    root="$tmp/$(basename "$path").d"
    mkdir -p "$root"
    ditto -x -k "$path" "$root" || die "couldn't unpack $path"
  fi
  [ -e "$root" ] || die "no such file: $path"
  found=0
  while IFS= read -r -d '' f; do
    is_macho "$f" || continue
    found=$((found + 1))
    rel="${f#"$root"/}"; [ "$rel" != "$f" ] || rel="$(basename "$f")"
    archs="$(lipo -archs "$f" 2>/dev/null)" || { echo "FAIL  $rel: lipo can't read it"; bad=1; continue; }
    for arch in $archs; do
      v="$(minos_of "$f" "$arch")"
      if [ -z "$v" ]; then
        echo "FAIL  no minimum macOS  $arch  $rel"; bad=1
      elif ver_gt "$v" "$max"; then
        echo "FAIL  minos $v  $arch  $rel (more than $max)"; bad=1
      else
        echo "minos $v  $arch  $rel"
      fi
    done
  done < <(find "$root" -type f -print0 | LC_ALL=C sort -z)
  [ "$found" -gt 0 ] || { echo "FAIL  $path holds no Mach-O file"; bad=1; }
done
if [ "$bad" = 0 ]; then echo "minos: every slice asks for macOS $max or older"; else echo "minos: FAILED (the limit is macOS $max)"; fi
exit "$bad"

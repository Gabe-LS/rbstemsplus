#!/bin/bash
# Two of release.md §6.1's static gates (T25), on an app zip, run by scripts/build-app.sh on every
# build, TEST builds included (R4.I39: the TEST_BUILD apps of G1, G2, tart and P1-P16 too):
#   - no quarantine: the zip unpacked with `ditto -x -k` into a scratch folder has no
#     com.apple.quarantine on any file or folder (xattr);
#   - no sanitizer: no Mach-O slice has an ASan, UBSan or TSan symbol (___asan_*, ___ubsan_*,
#     ___tsan_*) or links a sanitizer runtime (libclang_rt.*san*).
# The minos and bundle-list gates are tests/mac/minos.sh and bundle-files.sh; the availability
# flag is checked on the core's compile line by build-app.sh.
#
#   tests/mac/static-gates.sh APP_ZIP
#
# Exit 0: both pass; 1: a gate fails (it says which file); 2: the check itself can't run.
set -uo pipefail
die() { echo "static-gates.sh: $*" >&2; exit 2; }
[ $# -eq 1 ] && [ -f "$1" ] || { echo "usage: $0 APP_ZIP" >&2; exit 2; }
tmp="$(mktemp -d "${TMPDIR:-/tmp}/rbsp-gates.XXXXXX")" || die "no scratch folder"
trap 'rm -rf "${tmp:?}"' EXIT
ditto -x -k "$1" "$tmp/u" || die "couldn't unpack $1"
bad=0
q="$(find "$tmp/u" \( -type f -o -type d \) -print0 | xargs -0 xattr 2>/dev/null | grep 'com.apple.quarantine$' || true)"
if [ -n "$q" ]; then echo "quarantine: ${q//$tmp\/u\//}"; bad=1; else echo "quarantine: none"; fi
machos=0
while IFS= read -r -d '' f; do
  archs="$(lipo -archs "$f" 2>/dev/null)" || continue
  [ -n "$archs" ] || continue
  machos=$((machos + 1))
  for a in $archs; do
    syms="$(nm -arch "$a" "$f" 2>/dev/null)" || die "nm can't read ${f#$tmp/u/} ($a)"
    libs="$(otool -arch "$a" -L "$f" 2>/dev/null)" || die "otool can't read ${f#$tmp/u/} ($a)"
    if grep -Eq ' ___(asan|ubsan|tsan)_' <<< "$syms" || grep -Eq 'libclang_rt\.[a-z_]*san' <<< "$libs"; then
      echo "sanitizer: ${f#$tmp/u/} ($a)"; bad=1
    fi
  done
done < <(find "$tmp/u" -type f -print0)
[ "$machos" -gt 0 ] || die "no Mach-O in $1"
[ "$bad" = 1 ] || echo "sanitizer: none in $machos Mach-O files"
exit "$bad"

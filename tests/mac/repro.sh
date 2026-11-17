#!/bin/bash
# Is the Mac app build byte-reproducible? Builds the app of one commit twice, each time from a
# clean copy of the commit's files (git archive, nothing else of this checkout), and compares the
# two unpacked apps file by file. With --against ZIP, builds once and compares with ZIP instead
# (a published release's app zip, say: the carry-forward's question).
#
#   tests/mac/repro.sh [--same-path] [--against ZIP] [--keep DIR] [COMMIT]
#
#   COMMIT        what to build (default HEAD)
#   --same-path   both builds in the same folder, one after the other (default: two folders with
#                 different paths, as a release's public clone and a later rebuild may be)
#   --against ZIP build once and compare with ZIP
#   --keep DIR    keep the builds and unpacked apps in DIR (default: a scratch folder, deleted)
#
# Only the download caches are reused: build/flac, build/sqlcipher and build/pyrekordbox of this
# checkout, copied in when present (each build checks their sha256s; missing ones are
# downloaded). Release builds (scripts/build-app.sh with the public keys of the commit), so no
# TEST_BUILD code. The compare is scripts/bundle-compare.sh (the carry-forward's): for each file
# that differs it says what differs: for a Mach-O, whether the code without its signature is the
# same and the signatures' settings too; for an asset catalog, whether assetutil describes the
# same content apart from the fields actool writes anew on every run; for a CodeResources seal,
# whether it differs only in the entries of those files. When every difference is of those
# kinds, it says so on its last line (the fallback rule then applies: compare the rest, require
# the asset catalog's inputs unchanged). Every entry is compared too (type, mode, path), and any
# link or special file fails; with --against, both apps must pass codesign --verify --deep
# --strict. Exit 0: identical apps. Needs Xcode's actool outside a sandbox.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
die() { echo "repro.sh: $*" >&2; exit 2; }
same=0; against=""; keep=""; commit=HEAD
while [ $# -gt 0 ]; do
  case "$1" in
    --same-path) same=1; shift ;;
    --against) [ $# -ge 2 ] || die "--against needs a zip"; against="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"; shift 2 ;;
    --keep) [ $# -ge 2 ] || die "--keep needs a folder"; keep="$2"; shift 2 ;;
    -*) die "unknown option $1" ;;
    *) commit="$1"; shift ;;
  esac
done
[ -z "$against" ] || [ -f "$against" ] || die "no zip at $against"
sha="$(git -C "$root" rev-parse --verify "$commit^{commit}")" || die "no commit $commit"
if [ -n "$keep" ]; then
  mkdir -p "$keep"; work="$(cd "$keep" && pwd)"
else
  work="$(mktemp -d "${TMPDIR:-/tmp}/rbsp-repro.XXXXXX")"
  trap 'rm -rf "${work:?}"' EXIT
fi

# build NAME DIR: the commit's files into DIR/src, the app built there, its zip unpacked into
# $work/NAME.app.d
build() {
  local name="$1" dir="$2" c
  rm -rf "$dir"; mkdir -p "$dir/src/build"
  git -C "$root" archive --format=tar "$sha" | tar -x -f - -C "$dir/src"
  for c in flac sqlcipher pyrekordbox; do
    [ ! -d "$root/build/$c" ] || cp -R "$root/build/$c" "$dir/src/build/$c"
  done
  echo "== build $name in $dir/src"
  ( cd "$dir/src" && env -u RBSTEMSPLUS_TEST_BASE_URL -u RBSTEMSPLUS_TEST_KEY -u RBSTEMSPLUS_SCREENSHOTS -u RBSTEMSPLUS_TEST_BROKEN_SELFTEST \
      scripts/build-app.sh "$dir/out" ) > "$work/$name.log" 2>&1 \
    || { tail -40 "$work/$name.log"; die "build $name failed (log: $work/$name.log)"; }
  rm -rf "$work/$name.app.d"; mkdir -p "$work/$name.app.d"
  ditto -x -k "$dir/out/rbstemsplus-app.zip" "$work/$name.app.d"
  cp "$dir/out/rbstemsplus-app.zip" "$work/$name.zip"
}

echo "commit $sha"
build A "$work/a"
if [ -n "$against" ]; then
  rm -rf "$work/B.app.d"; mkdir -p "$work/B.app.d"
  ditto -x -k "$against" "$work/B.app.d"
  cp "$against" "$work/B.zip"
  echo "== B is $against"
elif [ "$same" = 1 ]; then
  build B "$work/a"
else
  build B "$work/b-elsewhere"
fi

# the compare is scripts/bundle-compare.sh's, the carry-forward's own (release.md §1.3)
cd "$work"
if cmp -s A.zip B.zip; then echo "the zips are byte-identical"; else echo "the zips differ"; fi
rc=0
"$root/scripts/bundle-compare.sh" ${against:+--verify} A.app.d B.app.d > compare.log || rc=$?
grep -v '^bundle-compare: ' compare.log || true
case "$rc" in
  0) echo "repro: REPRODUCIBLE, $(sed -n 's/^bundle-compare: IDENTICAL: //p' compare.log)"; exit 0 ;;
  1) echo "repro: NOT reproducible: $(sed -n 's/^bundle-compare: SAME CODE: \([^;]*\);.*/\1/p' compare.log)"
     echo "repro: each difference is an asset catalog with the same content, a signature over the same code, or a seal differing only in those"
     exit 1 ;;
  2) echo "repro: NOT reproducible: $(sed -n 's/^bundle-compare: DIFFERENT: //p' compare.log)"; exit 1 ;;
  *) die "scripts/bundle-compare.sh failed ($rc)" ;;
esac

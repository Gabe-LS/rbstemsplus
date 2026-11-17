#!/bin/bash
# make mac-unchanged (mac-core M9b, §6.2): does a branch leave the Mac files alone? Builds the
# Mac app (with the prepare helper inside it) and the Mac bridge from the merge base and from
# HEAD, each from a clean copy of its commit's files (git archive), and compares them with the
# carry-forward's compare (scripts/bundle-compare.sh, release.md §1.3): the app must be the same
# files, or the same code with the set-aside asset catalog's inputs unchanged between the two
# commits (R10's rule; the lists are the HEAD build's); the helper and the bridge byte for byte.
# Then the per-OS rows and code check (core/tests/os-rows.sh: Windows-only rows and code leave
# the Mac objects identical). A required merge check for every Windows-lane commit that touches
# core/, common/, helper/ or bridge/.
#
#   tests/mac/unchanged.sh [BASE [HEAD]]   BASE: default the merge base of HEAD and main;
#                                          HEAD: default HEAD (committed files only)
#   KEEP=DIR                               keep the builds in DIR
#
# Release builds with each commit's public keys (no TEST_BUILD code). The download caches of this
# checkout (build/flac, build/sqlcipher, build/pyrekordbox) are copied in when present. Needs
# Xcode's actool, outside a sandbox. Exit 0: unchanged; 1: a Mac file changed; 2: an error.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
die() { echo "unchanged.sh: $*" >&2; exit 2; }
head_rev="${2:-HEAD}"
head_sha="$(git -C "$root" rev-parse --verify "$head_rev^{commit}")" || die "no commit $head_rev"
if [ -n "${1:-}" ]; then
  base_sha="$(git -C "$root" rev-parse --verify "$1^{commit}")" || die "no commit $1"
else
  base_sha="$(git -C "$root" merge-base "$head_sha" main)" || die "no merge base of $head_rev and main (give BASE)"
fi
[ -z "$(git -C "$root" status --porcelain)" ] || echo "NOTE: uncommitted changes aren't built: this compares the commits $base_sha and $head_sha"
if [ -n "${KEEP:-}" ]; then
  mkdir -p "$KEEP"; work="$(cd "$KEEP" && pwd)"
else
  work="$(mktemp -d "${TMPDIR:-/tmp}/rbsp-unchanged.XXXXXX")"
  trap 'rm -rf "${work:?}"' EXIT
fi
echo "base $base_sha"
echo "head $head_sha"

# build NAME SHA: the commit's files into $work/NAME/src; the app's zip, the helper and the bridge
build() {
  local name="$1" sha="$2" d="$work/$1" c
  rm -rf "$d"; mkdir -p "$d/src/build"
  git -C "$root" archive --format=tar "$sha" | tar -x -f - -C "$d/src"
  for c in flac sqlcipher pyrekordbox; do
    [ ! -d "$root/build/$c" ] || cp -R "$root/build/$c" "$d/src/build/$c"
  done
  echo "== build $name (log: $d/build.log)"
  ( cd "$d/src" && env -u RBSTEMSPLUS_TEST_BASE_URL -u RBSTEMSPLUS_TEST_KEY -u RBSTEMSPLUS_SCREENSHOTS \
      -u RBSTEMSPLUS_APP_VERSION -u RBSP_RESEARCH -u RBSTEMS_DUMP_DIR -u RBSTEMSPLUS_TEST_BROKEN_SELFTEST \
      scripts/build-app.sh "$d/out" && bridge/build.sh "$d/bridge" ) > "$d/build.log" 2>&1 \
    || { tail -30 "$d/build.log"; die "the $name build failed (log: $d/build.log)"; }
}
build base "$base_sha"
build head "$head_sha"

failed=0
echo "== the Mac app"
rc=0
"$root/scripts/bundle-compare.sh" "$work/base/out/rbstemsplus-app.zip" "$work/head/out/rbstemsplus-app.zip" > "$work/compare.log" 2>&1 || rc=$?
cat "$work/compare.log"
git -C "$root" diff -z --name-only --no-renames "$base_sha" "$head_sha" > "$work/changed"
case "$rc" in
  0) echo "the app: unchanged (the same files)" ;;
  1)
    : > "$work/setaside.changed"
    while IFS= read -r f; do
      list="$work/head/src/build/app/$(basename "$f").inputs"
      if [ ! -s "$list" ]; then echo "$f: no input list in the head build" >> "$work/setaside.changed"; continue; fi
      r=0; python3 -I "$root/scripts/mac-inputs.py" changed "$list" < "$work/changed" >> "$work/setaside.changed" || r=$?
      [ "$r" -le 1 ] || die "couldn't read $list"
    done < <(sed -n 's/^set-aside: //p' "$work/compare.log")
    if [ -s "$work/setaside.changed" ]; then
      echo "the app: CHANGED: the same code, but a set-aside file's inputs changed: $(tr '\n' ' ' < "$work/setaside.changed")"; failed=1
    else
      echo "the app: unchanged (the same code; the asset catalog's inputs unchanged)"
    fi ;;
  2) echo "the app: CHANGED (above)"; failed=1 ;;
  *) die "scripts/bundle-compare.sh failed" ;;
esac
# the helper is in the app (compared above); said apart, as it is built apart
if cmp -s "$work/base/src/build/app/helper/rbsp-prepare" "$work/head/src/build/app/helper/rbsp-prepare"; then echo "the helper: unchanged (byte-identical)"
else echo "the helper: CHANGED"; failed=1; fi
if cmp -s "$work/base/bridge/libonnxruntime.1.18.0.dylib" "$work/head/bridge/libonnxruntime.1.18.0.dylib"; then echo "the bridge: unchanged (byte-identical)"
else echo "the bridge: CHANGED"; failed=1; fi
# what changed for the Mac app on the head build's lists, for the reader
if [ -s "$work/head/src/build/app/rbstemsplus-app.zip.inputs" ]; then
  r=0; python3 -I "$root/scripts/mac-inputs.py" changed "$work/head/src/build/app/rbstemsplus-app.zip.inputs" \
    "$work/head/bridge/libonnxruntime.1.18.0.dylib.inputs" < "$work/changed" > "$work/inputs.changed" || r=$?
  [ "$r" -le 1 ] || die "couldn't read the head build's input lists"
  if [ -s "$work/inputs.changed" ]; then echo "Mac inputs changed between the commits: $(tr '\n' ' ' < "$work/inputs.changed")"
  else echo "no Mac input changed between the commits"; fi
fi

echo "== the per-OS rows and code (core/tests/os-rows.sh at head)"
if [ -f "$work/head/src/core/tests/os-rows.sh" ]; then
  (cd "$work/head/src" && sh core/tests/os-rows.sh) > "$work/os-rows.log" 2>&1 && tail -1 "$work/os-rows.log" \
    || { cat "$work/os-rows.log"; echo "the per-OS rows and code check FAILED"; failed=1; }
else
  echo "no core/tests/os-rows.sh at head"
fi
if [ "$failed" = 0 ]; then echo "mac-unchanged: PASS: the Mac app, helper and bridge of $head_sha are $base_sha's"; exit 0; fi
echo "mac-unchanged: FAIL: a Mac file changed (above)"
exit 1

#!/bin/bash
# Are two Mac app bundles the same app? The Mac carry-forward's compare (docs: release.md §1.3,
# R10's rule): scripts/release.sh asks it whether the app built now is the previous release's,
# tests/mac/unchanged.sh (make mac-unchanged) whether a branch leaves the Mac bytes alone, and
# tests/mac/repro.sh whether two builds of one commit agree.
#
#   scripts/bundle-compare.sh [--verify] A B
#
#   A, B       each an app zip (rbstemsplus-app.zip) or a folder holding the unpacked zip
#   --verify   both apps must also pass codesign --verify --deep --strict
#
# Every entry is compared (type, mode, path: find and stat), and any link or special file fails.
# Then file by file. A file that differs is set aside only when it is one of the kinds a rebuild
# of the same sources gives anew (two builds of one commit differ in exactly these, the T23b
# spike):
#   - an asset catalog (*.car) whose content (assetutil --info) is the same apart from the fields
#     actool writes anew on every run (Timestamp, RenditionName, SHA1Digest). It is not compared
#     further: its inputs must be unchanged instead (the caller checks them; "set-aside:" lines);
#   - a Mach-O whose slices are the same once their signatures are removed, and whose signatures
#     say the same apart from the hashes (identifier, flags, entitlements, requirements: codesign
#     -d), so only what the signature hashes differs;
#   - a CodeResources seal that is the same once the entries of those files are left out.
# Anything else that differs (an Info.plist, a resource, the code) is a difference.
#
# The last line says which: "bundle-compare: IDENTICAL", "bundle-compare: SAME CODE" (with a
# "set-aside: PATH" line per asset catalog before it) or "bundle-compare: DIFFERENT".
# Exit 0: identical; 1: the same code, every difference set aside; 2: different; 3: an error.
set -euo pipefail
die() { echo "bundle-compare.sh: $*" >&2; exit 3; }
verify=0
[ "${1:-}" != --verify ] || { verify=1; shift; }
[ $# -eq 2 ] || die "usage: $0 [--verify] A B (each an app zip or a folder)"
work="$(mktemp -d "${TMPDIR:-/tmp}/rbsp-bundle-compare.XXXXXX")"
trap 'rm -rf "${work:?}"' EXIT

# unpack X SRC: SRC (a zip, or a folder) as $work/X.d
unpack() {
  if [ -d "$2" ]; then
    ditto "$2" "$work/$1.d" || die "couldn't copy $2"
  elif [ -f "$2" ]; then
    mkdir -p "$work/$1.d" && ditto -x -k "$2" "$work/$1.d" || die "couldn't unpack $2"
  else
    die "no zip or folder at $2"
  fi
}
unpack A "$1"
unpack B "$2"

# what differs in a Mach-O, per slice: nothing, only the signature, or the code. Returns 1 when
# the code or the signature's own settings differ.
macho_why() {
  local a="$1" b="$2" arch t="$work/why" rc=0
  rm -rf "$t"; mkdir -p "$t"
  [ "$(lipo -archs "$a")" = "$(lipo -archs "$b")" ] || { echo "    the slices differ: $(lipo -archs "$a") / $(lipo -archs "$b")"; return 1; }
  for arch in $(lipo -archs "$a"); do
    lipo "$a" -thin "$arch" -output "$t/a" 2>/dev/null || cp "$a" "$t/a"
    lipo "$b" -thin "$arch" -output "$t/b" 2>/dev/null || cp "$b" "$t/b"
    if cmp -s "$t/a" "$t/b"; then echo "    $arch: identical slice"; continue; fi
    # the signature's settings, without the hashes (and the path codesign prints)
    sig_info "$a" "$arch" > "$t/sa"; sig_info "$b" "$arch" > "$t/sb"
    codesign --remove-signature "$t/a" 2>/dev/null || true
    codesign --remove-signature "$t/b" 2>/dev/null || true
    if ! cmp -s "$t/a" "$t/b"; then
      rc=1
      echo "    $arch: the code differs ($(cmp -l "$t/a" "$t/b" 2>/dev/null | wc -l | tr -d ' ') bytes, unsigned); UUIDs $(otool -l "$t/a" | awk '$1=="uuid"{print $2}') / $(otool -l "$t/b" | awk '$1=="uuid"{print $2}')"
    elif ! cmp -s "$t/sa" "$t/sb"; then
      rc=1
      echo "    $arch: the same code, but the signatures' settings differ:"; diff "$t/sa" "$t/sb" | head -10 | sed 's/^/      /' || true
    else
      echo "    $arch: only the signature differs (the same settings; only the hashes differ)"
    fi
  done
  return "$rc"
}
sig_info() {
  codesign -d -vvv --arch "$2" --entitlements - --requirements - "$1" 2>&1 \
    | grep -vE '^(Executable=|CandidateCDHash|CMSDigest=|CDHash=)' \
    | sed -E 's/H"[0-9a-f]+"/H"…"/g' || true
}
# an asset catalog's description without the fields actool writes anew on every run
car_info() { assetutil --info "$1" 2>/dev/null | grep -vE '"(Timestamp|RenditionName|SHA1Digest)"' || true; }
# seal_same A B BASE DIFF...: the CodeResources A and B are the same once the entries of the
# set-aside files (DIFF, paths from the top; BASE: the bundle's Contents/ path) are left out,
# and those of the nested code holding one
seal_same() {
  python3 -I - "$@" <<'PY'
import plistlib, sys
a, b, base, diffs = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
inner = [d[len(base):] for d in diffs if d.startswith(base)]
def load(p):
    with open(p, "rb") as f:
        d = plistlib.load(f)
    for k in ("files", "files2"):
        entries = d.get(k, {})
        for name in list(entries):
            if any(i == name or i.startswith(name + "/") for i in inner):
                del entries[name]
    return d
sys.exit(0 if load(a) == load(b) else 1)
PY
}

cd "$work"
(cd A.d && find . -type f | sed 's|^\./||' | LC_ALL=C sort) > list.A
(cd B.d && find . -type f | sed 's|^\./||' | LC_ALL=C sort) > list.B
same_files=0; diffs=(); setaside=(); setaside_sig=(); explained=1
if ! cmp -s list.A list.B; then
  echo "the file lists differ:"; diff list.A list.B | sed 's/^/  /' || true
  explained=0
fi
# every entry, not only the files: its type, its mode and its path
inventory() { (cd "$1" && find . -print0 | LC_ALL=C sort -z | xargs -0 stat -f '%HT %Sp %N'); }
inventory A.d > inv.A; inventory B.d > inv.B
for x in A B; do
  if grep -v -E '^(Regular File|Directory) ' "inv.$x" > "odd.$x"; then
    echo "a link or special file in $x:"; sed 's/^/  /' "odd.$x"; explained=0
  fi
done
if ! cmp -s inv.A inv.B; then
  echo "the entries differ (type, mode or path):"; diff inv.A inv.B | sed 's/^/  /' || true; explained=0
fi
if [ "$verify" = 1 ]; then
  for x in A B; do
    n=0
    for app in "$x.d"/*.app; do
      [ -d "$app" ] || continue
      n=$((n + 1))
      if codesign --verify --deep --strict "$app" > "verify.$x" 2>&1; then echo "$x: codesign --verify --deep --strict passes"
      else echo "$x: codesign --verify --deep --strict FAILS:"; sed 's/^/  /' "verify.$x"; explained=0; fi
    done
    [ "$n" -gt 0 ] || { echo "$x: no .app at its top"; explained=0; }
  done
fi
while IFS= read -r f; do
  [ -f "B.d/$f" ] || continue
  if cmp -s "A.d/$f" "B.d/$f"; then same_files=$((same_files + 1)); else diffs+=("$f"); fi
done < list.A
# first the files themselves; the seals after, knowing which files were set aside
seals=()
for f in ${diffs[@]+"${diffs[@]}"}; do
  if [[ "$f" == */_CodeSignature/CodeResources ]]; then seals+=("$f"); continue; fi
  echo "DIFFERS: $f ($(stat -f %z "A.d/$f") / $(stat -f %z "B.d/$f") bytes)"
  if lipo -archs "A.d/$f" > /dev/null 2>&1 && lipo -archs "B.d/$f" > /dev/null 2>&1; then
    if macho_why "A.d/$f" "B.d/$f"; then setaside_sig+=("$f"); else explained=0; fi
  elif [[ "$f" == *.car ]]; then
    car_info "A.d/$f" > car.A; car_info "B.d/$f" > car.B
    if [ -s car.A ] && cmp -s car.A car.B; then
      echo "    assetutil --info: the same content; only Timestamp, RenditionName and SHA1Digest differ"
      setaside+=("$f")
    else
      echo "    assetutil --info differs beyond those per-run fields:"; diff car.A car.B | head -20 | sed 's/^/      /' || true
      explained=0
    fi
  else
    cat -v "A.d/$f" > text.A; cat -v "B.d/$f" > text.B
    diff text.A text.B | head -10 | sed 's/^/    /' || true
    explained=0
  fi
done
for f in ${seals[@]+"${seals[@]}"}; do
  echo "DIFFERS: $f ($(stat -f %z "A.d/$f") / $(stat -f %z "B.d/$f") bytes)"
  if seal_same "A.d/$f" "B.d/$f" "${f%_CodeSignature/CodeResources}" ${setaside[@]+"${setaside[@]}"} ${setaside_sig[@]+"${setaside_sig[@]}"}; then
    echo "    the same seal apart from the entries of the files set aside"
  else
    echo "    the seal differs in other entries too"; explained=0
  fi
done
if cmp -s list.A list.B && [ "${#diffs[@]}" = 0 ] && [ "$explained" = 1 ]; then
  echo "bundle-compare: IDENTICAL: the $same_files files are byte-identical, with the same entries and modes"
  exit 0
fi
if [ "$explained" = 1 ]; then
  for f in ${setaside[@]+"${setaside[@]}"}; do echo "set-aside: $f"; done
  echo "bundle-compare: SAME CODE: ${#diffs[@]} file(s) differ, $same_files identical; each is an asset catalog with the same content (its inputs must be unchanged), a signature over the same code, or a seal differing only in those"
  exit 1
fi
echo "bundle-compare: DIFFERENT: ${#diffs[@]} file(s) differ, $same_files identical (above)"
exit 2

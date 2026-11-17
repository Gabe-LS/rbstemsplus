#!/bin/bash
# The Mac carry-forward with the real builds (release.md §1.3, T23b), from a clean copy of a
# commit (git archive, committed again in a scratch repository so scripts/build.sh has a clean
# checkout):
#   1. the input lists the builds derive (the depfiles of helper/build.sh and bridge/build.sh, and
#      what scripts/build-app.sh reads): what they hold, and what they never hold (VERSION, docs/,
#      tests/, win/);
#   2. on those lists (scripts/mac-inputs.py): a commit touching only VERSION, docs/, win/ and
#      tests/ changes no Mac input, nor one touching only the core's Windows code and its tests
#      (core/win.c, core/tests/: the core's share is its archive's depfiles, SO7); one changing
#      only FLAC_VERSION in bridge/build.sh, one changing only a bridge/ header the helper
#      includes, and one changing only a core source the archive compiles, each change a Mac app
#      input (R4.I91);
#   3. RBSTEMSPLUS_APP_VERSION: stamped with VERSION's own number, the release app is VERSION's
#      (scripts/bundle-compare.sh: the same code); stamped with another, it differs (its
#      Info.plists and the version compiled in);
#   4. scripts/build.sh --test URL --plan FILE in a carried shape: the previous release's TEST
#      build from a gate archive's tag-vX.Y.Z/ (checked against its SHA256SUMS) reused byte for
#      byte, its version and commits in payload.json, signed with the key pair archived with it
#      (build/test-keys/ neither made nor used); and its refusals.
#
#   tests/release/mac-build-test.sh [COMMIT]   (default HEAD) Run outside a sandbox (actool). Needs
#                                              the model (RBSTEMSPLUS_MODEL, or where build.sh looks)
set -uo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
sha="$(git -C "$root" rev-parse --verify "${1:-HEAD}^{commit}")" || { echo "mac-build-test: no commit ${1:-HEAD}" >&2; exit 2; }
# the model: RBSTEMSPLUS_MODEL, else where build.sh looks, next to this checkout or, from a git
# worktree, next to its main checkout; else a dist/ copy
main="$(cd "$(git -C "$root" rev-parse --path-format=absolute --git-common-dir)/.." && pwd)"
model="${RBSTEMSPLUS_MODEL:-}"
for m in "$root/../Library Organizer/experiments/rekordbox-stems/runs/htdemucs_rb_v4xt.onnx" \
         "$main/../Library Organizer/experiments/rekordbox-stems/runs/htdemucs_rb_v4xt.onnx" \
         "$root/dist/stemsplus-model.onnx" "$main/dist/stemsplus-model.onnx"; do
  [ -n "$model" ] || [ ! -f "$m" ] || model="$m"
done
[ -n "$model" ] && [ -f "$model" ] || { echo "mac-build-test: no model (set RBSTEMSPLUS_MODEL)" >&2; exit 2; }
export RBSTEMSPLUS_MODEL="$model"
unset RBSTEMSPLUS_TEST_BASE_URL RBSTEMSPLUS_TEST_KEY RBSTEMSPLUS_SCREENSHOTS RBSTEMSPLUS_APP_VERSION RBSP_RESEARCH RBSTEMS_DUMP_DIR
w="$(mktemp -d "${TMPDIR:-/tmp}/rbsp-mac-build-test.XXXXXX")"
w="$(cd "$w" && pwd -P)"
trap 'if [ "${KEEP:-0}" = 1 ]; then echo "kept $w"; else rm -rf "${w:?}"; fi' EXIT
pass=0; fail=0
ok() { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "FAIL: $*"; }
g() { git -c init.defaultBranch=main -c user.name="Test Dev" -c user.email=dev@example.invalid -C "$src" "$@"; }
src="$w/src"
mkdir -p "$src/build"
git -C "$root" archive --format=tar "$sha" | tar -x -f - -C "$src"
for c in flac sqlcipher pyrekordbox; do [ ! -d "$root/build/$c" ] || cp -R "$root/build/$c" "$src/build/$c"; done
g init -q && g add -A && g commit -q -m "the commit $sha" || { echo "mac-build-test: couldn't make the scratch repository" >&2; exit 2; }
base="$(g rev-parse HEAD)"
v="$(tr -d '[:space:]' < "$src/VERSION")"
url="http://127.0.0.1:8765/"
echo "commit $sha (VERSION $v), in $src"

# ---------------------------------------------------------------- 1. the lists, from a TEST build
echo "== build.sh --test (log: $w/test-build.log)"
(cd "$src" && scripts/build.sh --test "$url") > "$w/test-build.log" 2>&1 || { tail -30 "$w/test-build.log"; echo "mac-build-test: the test build failed"; exit 1; }
app_list="$src/build/app/rbstemsplus-app.zip.inputs"; car_list="$src/build/app/Assets.car.inputs"
bridge_list="$src/build/bridge/libonnxruntime.1.18.0.dylib.inputs"
for f in "$app_list" "$car_list" "$bridge_list"; do [ -s "$f" ] && ok || bad "no input list $f"; done
has() { grep -qxF "$2" "$1" && ok || bad "$(basename "$1") doesn't list $2"; }
for p in app/Sources/ app/Info.plist app/Watcher-Info.plist app/icon/RBStemsPlus.icon/ NOTICE LICENSE scripts/build-app.sh \
         helper/build.sh helper/prepare.c helper/rbsp.h helper/library.h bridge/build.sh bridge/onnxruntime_c_api.h; do
  has "$app_list" "$p"
done
for p in bridge/build.sh bridge/rbstems_bridge.c bridge/onnxruntime_c_api.h; do has "$bridge_list" "$p"; done
has "$car_list" app/icon/RBStemsPlus.icon/
grep -qE '^(VERSION|docs/|tests/|win/|build/)' "$app_list" "$bridge_list" "$car_list" && bad "a list holds VERSION, docs/, tests/, win/ or build/" || ok
grep -q '^helper/' "$bridge_list" && bad "the bridge's list holds the helper's files" || ok
# the core's share: its archive's sources and headers, core/build.sh, core.h, the ids' .def files;
# never all of core/, its Windows code or its tests
for p in core/build.sh core/core.h core/flows.c core/texts.def; do has "$app_list" "$p"; done
grep -qE '^(core/|core/win\.c|core/tests/)$' "$app_list" && bad "the app's list holds core/, core/win.c or core/tests/" || ok
# every listed path is the repository's (a file, or a folder ending in /)
while IFS= read -r p; do
  case "$p" in */) [ -d "$src/$p" ] ;; *) [ -f "$src/$p" ] ;; esac && ok || bad "listed, but not in the commit: $p"
done < <(cat "$app_list" "$bridge_list")

# ---------------------------------------------------------------- 2. the three commits
# changed NAME LIST WANT: the commit on the branch NAME changes the inputs WANT (a space-separated
# list; empty: none) of LIST, from the base
changed() {
  local got rc=0
  got="$(g diff -z --name-only --no-renames "$base" "$1" | python3 -I "$root/scripts/mac-inputs.py" changed "$2" | tr '\n' ' ')" || rc=$?
  [ "$rc" -le 1 ] || { bad "$1: mac-inputs.py failed"; return; }
  [ "${got% }" = "$3" ] && ok || bad "$1 on $(basename "$2"): changed inputs '${got% }', not '$3'"
}
g switch -q -c only-version-docs-win "$base"
echo 9.9.9 > "$src/VERSION"; mkdir -p "$src/docs" "$src/win" "$src/tests"
echo "a note" >> "$src/docs/a-note.md"; echo "Windows" >> "$src/win/a-file.txt"; echo "a test" >> "$src/tests/a-test.txt"
g add -A && g commit -q -m "VERSION, docs/, win/, tests/"
changed only-version-docs-win "$app_list" ""
changed only-version-docs-win "$bridge_list" ""
g switch -q -c only-core-win-tests "$base"
echo "/* Windows */" >> "$src/core/win.c"; echo "/* Windows */" >> "$src/core/flows_win.c"
mkdir -p "$src/core/tests"; echo "/* a test */" >> "$src/core/tests/flows_test.c"
g commit -q -am "core/win.c, core/flows_win.c and core/tests/ only"
changed only-core-win-tests "$app_list" ""
changed only-core-win-tests "$bridge_list" ""
g switch -q -c only-core-source "$base"
echo "/* a later flow */" >> "$src/core/flows.c"
g commit -q -am "a core source only"
changed only-core-source "$app_list" "core/flows.c"
changed only-core-source "$bridge_list" ""
g switch -q -c only-flac-version "$base"
sed -i '' 's/^FLAC_VERSION=.*/FLAC_VERSION=9.9.9/' "$src/bridge/build.sh"
[ "$(g diff --name-only)" = bridge/build.sh ] && [ "$(g diff | grep -c '^[-+]FLAC_VERSION=')" = 2 ] && ok || bad "the FLAC_VERSION edit isn't one line of bridge/build.sh"
g commit -q -am "FLAC_VERSION only"
changed only-flac-version "$app_list" "bridge/build.sh"
changed only-flac-version "$bridge_list" "bridge/build.sh"
g switch -q -c only-bridge-header "$base"
echo "/* a later ORT */" >> "$src/bridge/onnxruntime_c_api.h"
g commit -q -am "a bridge header only"
changed only-bridge-header "$app_list" "bridge/onnxruntime_c_api.h"
changed only-bridge-header "$bridge_list" "bridge/onnxruntime_c_api.h"
g switch -q main

# ---------------------------------------------------------------- 3. the stamped build
# release builds (the commit's public keys): plain, stamped with VERSION's number, stamped other
rel_build() {   # NAME [VERSION]
  (cd "$src" && RBSTEMSPLUS_APP_VERSION="${2:-}" scripts/build-app.sh "$w/$1") > "$w/$1.log" 2>&1 \
    || { tail -20 "$w/$1.log"; bad "the release build $1 failed"; return 1; }
  rm -rf "$w/$1.d"; mkdir -p "$w/$1.d"; ditto -x -k "$w/$1/rbstemsplus-app.zip" "$w/$1.d"
}
plist() { /usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" "$1" 2>/dev/null; }
echo "== the stamped builds (logs: $w/plain.log, $w/same.log, $w/other.log)"
if rel_build plain && rel_build same "$v" && rel_build other 1.2.3; then
  has "$src/build/app/rbstemsplus-app.zip.inputs" keys/release.pub.pem
  has "$src/build/app/rbstemsplus-app.zip.inputs" keys/backup.pub.pem
  [ "$(plist "$w/same.d/RB Stems Plus.app/Contents/Info.plist")" = "$v" ] && ok || bad "the stamp $v isn't in Info.plist"
  [ "$(plist "$w/other.d/RB Stems Plus.app/Contents/Info.plist")" = 1.2.3 ] && ok || bad "the stamp 1.2.3 isn't in Info.plist"
  [ "$(plist "$w/other.d/RB Stems Plus.app/Contents/Helpers/RB Stems Plus Watcher.app/Contents/Info.plist")" = 1.2.3 ] && ok || bad "the stamp 1.2.3 isn't in the watcher's Info.plist"
  rc=0; "$root/scripts/bundle-compare.sh" "$w/plain/rbstemsplus-app.zip" "$w/same/rbstemsplus-app.zip" > "$w/cmp-same.log" 2>&1 || rc=$?
  [ "$rc" -le 1 ] && ok || { bad "stamped with $v, the app isn't VERSION's (bundle-compare $rc)"; tail -5 "$w/cmp-same.log"; }
  rc=0; "$root/scripts/bundle-compare.sh" "$w/plain/rbstemsplus-app.zip" "$w/other/rbstemsplus-app.zip" > "$w/cmp-other.log" 2>&1 || rc=$?
  [ "$rc" = 2 ] && grep -q 'DIFFERS: RB Stems Plus.app/Contents/Info.plist' "$w/cmp-other.log" \
    && grep -q 'x86_64: the code differs' "$w/cmp-other.log" && ok || { bad "stamped with 1.2.3, the app isn't different where it should be"; tail -8 "$w/cmp-other.log"; }
fi
(cd "$src" && RBSTEMSPLUS_APP_VERSION=1.2 scripts/build-app.sh "$w/bad") > "$w/bad.log" 2>&1 && bad "RBSTEMSPLUS_APP_VERSION=1.2 built" \
  || { grep -q "RBSTEMSPLUS_APP_VERSION must be x.y.z" "$w/bad.log" && ok || bad "RBSTEMSPLUS_APP_VERSION=1.2: not refused as expected"; }
(cd "$src" && RBSTEMSPLUS_APP_VERSION=1.2.3 scripts/build.sh --test "$url") > "$w/bad2.log" 2>&1 && bad "build.sh took RBSTEMSPLUS_APP_VERSION" \
  || { grep -q "RBSTEMSPLUS_APP_VERSION is set" "$w/bad2.log" && ok || bad "build.sh with RBSTEMSPLUS_APP_VERSION: not refused as expected"; }

# ---------------------------------------------------------------- 4. build.sh --test --plan, carried
# the gate archive: the previous release's TEST build (this commit's, above) as tag-v$v, with the
# test key pair that signed it (docs/RELEASING.md's archiving step); build/test-keys then goes,
# as after make clean: a carried TEST build is signed with the archived key, never a new one
A="$w/archive"; prev="$A/tag-v$v"
mkdir -p "$prev/test-keys"
cp "$src/dist-test/payload.json" "$src/dist-test/payload.json.sig" "$src/dist-test/rbstemsplus-app.zip" "$src/dist-test/libonnxruntime.1.18.0.dylib" "$prev/"
cp "$src/build/test-keys/test-key.pem" "$src/build/test-keys/test.pub.pem" "$prev/test-keys/"
rm -rf "$src/build/test-keys"
(cd "$A" && find . -type f ! -name SHA256SUMS | LC_ALL=C sort | sed 's|^\./||' | tr '\n' '\0' | xargs -0 shasum -a 256 > SHA256SUMS)
fsha() { shasum -a 256 "$1" | cut -d' ' -f1; }
zip_sha="$(fsha "$prev/rbstemsplus-app.zip")"; br_sha="$(fsha "$prev/libonnxruntime.1.18.0.dylib")"
app_c=1111111111111111111111111111111111111111; br_c=2222222222222222222222222222222222222222
echo 9.9.9 > "$src/VERSION"; g commit -q -am "9.9.9"
head="$(g rev-parse HEAD)"
mkplan() {   # FILE COMMIT APP_VERSION
  printf '%s\n' "plan 1" "commit $2" "version 9.9.9" "previous v$v $(fsha "$prev/payload.json")" \
    "mac.app carry $3 $app_c 5555555555555555555555555555555555555555555555555555555555555555" \
    "mac.bridge carry $br_c $br_sha" "windows.app none" "windows.bridge none" > "$1"
}
mkplan "$w/plan" "$head" "$v"
echo "== build.sh --test --plan, both Mac files carried (log: $w/plan-build.log)"
if (cd "$src" && GATES_ARCHIVE="$A" scripts/build.sh --test "$url" --plan "$w/plan") > "$w/plan-build.log" 2>&1; then
  ok
  d="$src/dist-test"; pj() { plutil -extract "$1" raw -o - "$d/payload.json" 2>/dev/null || echo "(missing)"; }
  [ "$(pj payload_version)" = 9.9.9 ] && [ "$(pj commit)" = "$head" ] && ok || bad "the TEST payload isn't 9.9.9 of $head"
  [ "$(pj app.version)" = "$v" ] && [ "$(pj app.commit)" = "$app_c" ] && [ "$(pj app.sha256)" = "$zip_sha" ] && ok || bad "the TEST payload's app isn't the carried one"
  [ "$(pj bridge.commit)" = "$br_c" ] && [ "$(pj bridge.sha256)" = "$br_sha" ] && ok || bad "the TEST payload's bridge isn't the carried one"
  cmp -s "$d/rbstemsplus-app.zip" "$prev/rbstemsplus-app.zip" && cmp -s "$d/libonnxruntime.1.18.0.dylib" "$prev/libonnxruntime.1.18.0.dylib" \
    && ok || bad "the carried files aren't the archive's bytes"
  /usr/bin/openssl dgst -sha256 -verify "$prev/test-keys/test.pub.pem" -signature "$d/payload.json.sig" "$d/payload.json" > /dev/null \
    && ok || bad "the TEST payload's signature doesn't verify with the archived test key"
  [ ! -e "$src/build/test-keys" ] && ok || bad "a carried TEST build made build/test-keys"
  grep -qF "the test key of $prev" "$w/plan-build.log" && ok || bad "build.sh didn't say it signs with the archived key"
  (cd "$d" && shasum -a 256 -s -c SHA256SUMS) && ok || bad "dist-test/SHA256SUMS doesn't match"
  grep -q "the Mac app carry ($v, from $app_c), the Mac bridge carry (from $br_c)" "$w/plan-build.log" && ok || bad "build.sh didn't say the plan's shape"
else
  bad "build.sh --test --plan failed"; tail -10 "$w/plan-build.log" | sed 's/^/    /'
fi
# refusals: another commit's plan; an app version the archived build isn't; the archive changed
refuse() {   # NAME TEXT PLAN [ARCHIVE]
  if (cd "$src" && GATES_ARCHIVE="${4:-$A}" scripts/build.sh --test "$url" --plan "$3") > "$w/$1.log" 2>&1; then bad "$1: built"
  elif grep -qF "$2" "$w/$1.log"; then ok
  else bad "$1: not refused with '$2'"; tail -3 "$w/$1.log" | sed 's/^/    /'; fi
}
mkplan "$w/plan-other-commit" "$base" "$v"
refuse plan-other-commit "is for $base, not this commit" "$w/plan-other-commit"
mkplan "$w/plan-other-version" "$head" 1.0.0
refuse plan-other-version "app isn't version 1.0.0, the plan's" "$w/plan-other-version"
sed 's/^mac.bridge carry .*/mac.bridge carry '"$br_c"' 6666666666666666666666666666666666666666666666666666666666666666/' "$w/plan" > "$w/plan-other-bridge"
refuse plan-other-bridge "doesn't have the SHA-256 6666" "$w/plan-other-bridge"
cp -R "$A" "$w/archive-changed"; echo "changed" >> "$w/archive-changed/tag-v$v/payload.json"
refuse archive-changed "the gate archive doesn't match its SHA256SUMS" "$w/plan" "$w/archive-changed"
refuse archive-missing "no gate archive at" "$w/plan" "$w/no-archive"
cp -R "$A" "$w/archive-nokey"; rm -rf "$w/archive-nokey/tag-v$v/test-keys"
(cd "$w/archive-nokey" && find . -type f ! -name SHA256SUMS | LC_ALL=C sort | sed 's|^\./||' | tr '\n' '\0' | xargs -0 shasum -a 256 > SHA256SUMS)
refuse archive-nokey "no test key pair in" "$w/plan" "$w/archive-nokey"
[ ! -e "$src/build/test-keys" ] && ok || bad "a refused carried TEST build made build/test-keys"

# ---------------------------------------------------------------- 5. build.sh --test --plan, all new
# a TEST build that carries nothing (a previous release named) is signed with the gate archive's
# test key too: G3 updates the archived TEST builds to it, and they trust only that key
printf '%s\n' "plan 1" "commit $head" "version 9.9.9" "previous v$v $(fsha "$prev/payload.json")" \
  "mac.app new" "mac.bridge new" "windows.app none" "windows.bridge none" > "$w/plan-new"
echo "== build.sh --test --plan, both Mac files new (log: $w/plan-new.log)"
if (cd "$src" && GATES_ARCHIVE="$A" scripts/build.sh --test "$url" --plan "$w/plan-new") > "$w/plan-new.log" 2>&1; then
  ok
  d="$src/dist-test"
  [ "$(plutil -extract app.version raw -o - "$d/payload.json" 2>/dev/null)" = 9.9.9 ] && ok || bad "the all-new TEST payload's app isn't 9.9.9"
  /usr/bin/openssl dgst -sha256 -verify "$prev/test-keys/test.pub.pem" -signature "$d/payload.json.sig" "$d/payload.json" > /dev/null \
    && ok || bad "the all-new TEST payload's signature doesn't verify with the archived test key"
  [ ! -e "$src/build/test-keys" ] && ok || bad "an all-new TEST build in a plan's shape made build/test-keys"
  grep -qF "the test key of the gate archive's TEST builds ($prev/test-keys)" "$w/plan-new.log" && ok || bad "build.sh didn't say it signs with the archive's key"
else
  bad "build.sh --test --plan (all new) failed"; tail -10 "$w/plan-new.log" | sed 's/^/    /'
fi
refuse plan-new-noarchive "no gate archive at" "$w/plan-new" "$w/no-archive"
refuse plan-new-nokey "no test key pair in" "$w/plan-new" "$w/archive-nokey"
cp -R "$A" "$w/archive-twokeys"; mkdir -p "$w/archive-twokeys/tag-v0.9.0/test-keys"
/usr/bin/openssl ecparam -name prime256v1 -genkey -noout -out "$w/archive-twokeys/tag-v0.9.0/test-keys/test-key.pem"
/usr/bin/openssl ec -in "$w/archive-twokeys/tag-v0.9.0/test-keys/test-key.pem" -pubout -out "$w/archive-twokeys/tag-v0.9.0/test-keys/test.pub.pem" 2>/dev/null
(cd "$w/archive-twokeys" && find . -type f ! -name SHA256SUMS | LC_ALL=C sort | sed 's|^\./||' | tr '\n' '\0' | xargs -0 shasum -a 256 > SHA256SUMS)
refuse plan-new-twokeys "the gate archive's TEST builds don't trust one key" "$w/plan-new" "$w/archive-twokeys"
[ ! -e "$src/build/test-keys" ] && ok || bad "a refused all-new TEST build made build/test-keys"

echo "mac-build-test ($sha): $pass passed, $fail failed"
[ "$fail" = 0 ]

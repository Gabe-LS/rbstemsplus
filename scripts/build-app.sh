#!/bin/bash
# Builds "RB Stems Plus.app" (universal: arm64 + x86_64, macOS 12+) and zips it to
# dist/rbstemsplus-app.zip, the release asset the bootstrap downloads.
#
#   scripts/build-app.sh [OUT [--broken-selftest-test]]   OUT: the folder for the zip (default dist/)
#                                    --broken-selftest-test: lets RBSTEMSPLUS_TEST_BROKEN_SELFTEST=1
#                                    through (a TEST app whose core self-test fails, G1.4's test;
#                                    tests/mac/swap-selftest.sh only); without it the variable is refused
#
# Test build: with RBSTEMSPLUS_TEST_BASE_URL set (e.g. http://192.168.64.1:8765/), the app
# downloads its files from that server instead of the GitHub release, and its window title and
# log say "TEST BUILD". Compiled in (-D TEST_BUILD), never read at run time. scripts/build.sh
# --test URL sets it; a release build refuses an app with "TEST BUILD" in it.
#
# The app is the AppKit shell (app/Shell) over the shared C core (core/, mac-core §3-5; M7's
# switch-over, from 1.4.0): every build links the core's archive per architecture (a release
# libcore.a, with core/build.sh's release gate; a test build the test-hooks archive the G1 API
# spike needs) with core.h as the bridging header and the generated ids, and runs
# core_selftest() first in --swap. A test build adds the harness's probe (app/Probe) and the spike
# (app/Spike).
#
# Signing keys (app/Sources/Signing.swift): the public keys payload.json's signature is checked
# with are compiled in from build/app/Keys.swift, written here. A release: keys/release.pub.pem
# and keys/backup.pub.pem (scripts/make-signing-keys.sh), and it stops if either is missing, if
# they are the same, or if one is build.sh's test key. A test build: only the test key
# RBSTEMSPLUS_TEST_KEY names (build/test-keys/test.pub.pem, made by build.sh --test), never
# the release keys.
#
# The version: VERSION is compiled in too (build/app/Version.swift, compiledVersion), as the
# oldest payload.json the app accepts (app/Sources/Payload.swift). RBSTEMSPLUS_APP_VERSION=X.Y.Z
# stamps the app with that version instead (Info.plist and Version.swift), as scripts/release.sh
# does to rebuild the previous release's app and compare it (the Mac carry-forward, release.md
# §1.3): VERSION then isn't read.
#
# The input lists, for the carry-forward (scripts/mac-inputs.py): build/app/rbstemsplus-app.zip.inputs
# names every repository file and folder that reaches the app's bytes (the Swift sources, keys/,
# the Info.plists, the icon, NOTICE, LICENSE, this script, and the prepare helper's list from
# its depfiles); build/app/Assets.car.inputs those of the asset catalog alone (the icon
# document), which actool doesn't build byte for byte the same twice (its inputs must be
# unchanged instead).
#
# Steps: swiftc per architecture (-O, macOS 12 target) → lipo → the bundle from app/Info.plist
# with the version from ./VERSION → the icon (app/icon/RBStemsPlus.icon) compiled with actool
# into Assets.car (macOS 26) and RBStemsPlus.icns (macOS 12–15) → the watcher helper,
# Contents/Helpers/RB Stems Plus Watcher.app (make_helper) → the prepare helper,
# Contents/Helpers/rbsp-prepare (helper/build.sh: Apple silicon only, signed ad hoc WITHOUT the
# hardened runtime, as it loads Pioneer's ONNX Runtime and rekordbox's libmpg123; signed once,
# here: the outer signatures seal it) → ad-hoc signatures with the hardened runtime, the watcher
# first, then the app (sign_app; the bootstrap and the update sign these two again on the user's
# Mac, the same way) → zip. Needs Xcode (actool reads Icon Composer documents from
# Xcode 26 on). Intermediate files go to build/app/.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
if [ -n "${RBSTEMSPLUS_APP_VERSION:-}" ]; then version="$RBSTEMSPLUS_APP_VERSION"
else version="$(tr -d '[:space:]' < "$root/VERSION")"; fi
build="$root/build/app"
dist="${1:-$root/dist}"
test_base="${RBSTEMSPLUS_TEST_BASE_URL:-}"
test_key="${RBSTEMSPLUS_TEST_KEY:-}"
app="$build/RB Stems Plus.app"
exe="RB Stems Plus"
sdk="$(xcrun --sdk macosx --show-sdk-path)"

die() { echo "build-app.sh: $*" >&2; exit 1; }
[ $# -le 1 ] || { [ $# -eq 2 ] && [ "$2" = --broken-selftest-test ]; } || { echo "usage: $0 [OUT [--broken-selftest-test]]" >&2; exit 2; }
# review Q30: the broken self-test app only when asked for by name, never from a leaked variable
[ -z "${RBSTEMSPLUS_TEST_BROKEN_SELFTEST:-}" ] || [ "${2:-}" = --broken-selftest-test ] \
  || die "RBSTEMSPLUS_TEST_BROKEN_SELFTEST is set: unset it, or pass --broken-selftest-test (tests/mac/swap-selftest.sh only)"

# The watcher helper inside the app $1: Contents/Helpers/RB Stems Plus Watcher.app (Apple's place
# for helper apps; Contents/Library/LoginItems is for ServiceManagement's login items, which this
# isn't: a LaunchAgent starts it). Its own Info.plist (app/Watcher-Info.plist: the name "RB Stems
# Plus Watcher", the watcher's bundle ID, LSUIElement: no Dock icon) with the app's version and
# minimum macOS, a copy of the app's executable (a real file: the bootstrap and the update refuse
# links; the app runs as the watcher because of the bundle it runs from) and the icon. So macOS
# names the watcher apart: Login Items, the background notice, Activity Monitor, crash reports.
make_helper() {
  local app="$1" helper="$1/Contents/Helpers/RB Stems Plus Watcher.app" key value main
  main="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$app/Contents/Info.plist")"
  mkdir -p "$helper/Contents/MacOS" "$helper/Contents/Resources"
  cp "$root/app/Watcher-Info.plist" "$helper/Contents/Info.plist"
  for key in CFBundleShortVersionString CFBundleVersion LSMinimumSystemVersion; do
    value="$(/usr/libexec/PlistBuddy -c "Print :$key" "$app/Contents/Info.plist")"
    /usr/libexec/PlistBuddy -c "Set :$key $value" "$helper/Contents/Info.plist"
  done
  cp "$app/Contents/MacOS/$main" "$helper/Contents/MacOS/RB Stems Plus Watcher"
  printf 'APPL????' > "$helper/Contents/PkgInfo"
  if [ -f "$app/Contents/Resources/RBStemsPlus.icns" ]; then cp "$app/Contents/Resources/RBStemsPlus.icns" "$helper/Contents/Resources/"; fi
}

# Signs the app $1 ad hoc with the hardened runtime, inside out: the watcher helper first, then
# the app, whose signature seals the helper's. The bootstrap and the update sign the same way.
sign_app() {
  local helper="$1/Contents/Helpers/RB Stems Plus Watcher.app"
  if [ -d "$helper" ]; then codesign -f -s - --options runtime "$helper"; fi
  codesign -f -s - --options runtime "$1"
}
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "$([ -n "${RBSTEMSPLUS_APP_VERSION:-}" ] && echo RBSTEMSPLUS_APP_VERSION || echo VERSION) must be x.y.z, not '$version'"
# A public key file's DER in base64 (one line), or nothing unless it is a P-256 public key.
pubkey_b64() {
  local text
  text="$(/usr/bin/openssl ec -pubin -in "$1" -noout -text 2>/dev/null)" && grep -q 'ASN1 OID: prime256v1' <<< "$text" || return 1
  /usr/bin/openssl ec -pubin -in "$1" -outform DER 2>/dev/null | /usr/bin/base64 | tr -d '\n'
}

if [ -n "$test_base" ]; then
  [ -n "$test_key" ] || die "a test build needs RBSTEMSPLUS_TEST_KEY (use scripts/build.sh --test URL)"
  k="$(pubkey_b64 "$test_key")" || die "$test_key isn't a P-256 public key"
  # a screenshot build names the test key "release", so its log reads like a real install's
  if [ "${RBSTEMSPLUS_SCREENSHOTS:-}" = 1 ]; then keys=("release:$k"); else keys=("test:$k"); fi
else
  [ -z "$test_key" ] || die "RBSTEMSPLUS_TEST_KEY is set: unset it for a release"
  keys=()
  for name in release backup; do
    f="$root/keys/$name.pub.pem"
    [ -f "$f" ] || die "keys/$name.pub.pem is missing: a release needs both public keys (scripts/make-signing-keys.sh, docs/RELEASING.md)"
    k="$(pubkey_b64 "$f")" || die "keys/$name.pub.pem isn't a P-256 public key"
    if [ -f "$root/build/test-keys/test.pub.pem" ] && [ "$k" = "$(pubkey_b64 "$root/build/test-keys/test.pub.pem")" ]; then
      die "keys/$name.pub.pem is the test key: not building a release with it"
    fi
    keys+=("$name:$k")
  done
  [ "${keys[0]#*:}" != "${keys[1]#*:}" ] || die "keys/release.pub.pem and keys/backup.pub.pem are the same key"
fi

rm -rf "$build"
mkdir -p "$build" "$dist" "$app/Contents/MacOS" "$app/Contents/Resources"
{
  echo "// Written by scripts/build-app.sh: the public keys payload.json's signature is checked with."
  echo "let compiledKeys: [(name: String, der: String)] = ["
  for k in "${keys[@]}"; do echo "    (\"${k%%:*}\", \"${k#*:}\"),"; done
  echo "]"
} > "$build/Keys.swift"
printf '// Written by scripts/build-app.sh from VERSION.\nlet compiledVersion = "%s"\n' "$version" > "$build/Version.swift"

test_flags=()
if [ -n "$test_base" ]; then
  [[ "$test_base" =~ ^https?://[A-Za-z0-9.:_/-]+/$ ]] \
    || { echo "RBSTEMSPLUS_TEST_BASE_URL must be http(s)://host[:port]/path/ ending in /, not '$test_base'" >&2; exit 2; }
  printf '// Written by scripts/build-app.sh for a TEST BUILD only.\nlet testDownloadBase = "%s"\n' "$test_base" > "$build/TestBase.swift"
  # the harness's probe (app/Probe, the private Windows plan's mac-core §2.3): test builds only
  test_flags=(-D TEST_BUILD "$build/TestBase.swift" "$root"/app/Probe/*.swift "$root"/app/Spike/*.swift)
  echo "TEST BUILD: downloads from $test_base"
  # for the docs' screenshots: the window title without the test note (the log keeps it)
  if [ "${RBSTEMSPLUS_SCREENSHOTS:-}" = 1 ]; then test_flags+=(-D SCREENSHOT_BUILD); echo "SCREENSHOT BUILD: plain window title"; fi
elif [ -n "${RBSTEMSPLUS_SCREENSHOTS:-}" ]; then
  die "RBSTEMSPLUS_SCREENSHOTS is only for a test build"
fi
[ -n "$test_base" ] || [ -z "${RBSTEMSPLUS_TEST_BROKEN_SELFTEST:-}" ] || die "RBSTEMSPLUS_TEST_BROKEN_SELFTEST is only for a test build"

# the shared C core (mac-core §5, M7's switch-over): core/build.sh's archive per architecture, core.h
# as the bridging header, the generated ids, -D CORE_LINKED (--swap runs core_selftest() first) and
# the AppKit shell over it (app/Shell). A release links libcore.a (core/build.sh's release gate: no
# test hooks, nothing exported, no debug build); a test build the test-hooks archive the G1 API
# spike needs. RBSTEMSPLUS_TEST_BROKEN_SELFTEST=1 (with --broken-selftest-test) links the archive
# whose core_selftest() fails, for G1.4's refused-swap test (tests/mac/swap-selftest.sh).
core_lib=libcore core_opts=() core_flags=()
if [ -n "$test_base" ]; then
  core_lib=libcore-testhooks core_opts=(--test-hooks) core_flags=(-Xcc -DCORE_TEST_HOOKS)
  if [ "${RBSTEMSPLUS_TEST_BROKEN_SELFTEST:-}" = 1 ]; then
    core_lib=libcore-testhooks-broken core_opts+=(--broken-selftest); echo "TEST BUILD: the core's self-test fails (G1.4's test)"
  fi
fi
"$root/core/build.sh" lib "$build/core" ${core_opts[@]+"${core_opts[@]}"} > "$build/core.log" 2>&1 || { cat "$build/core.log"; die "the core didn't build"; }
# release.md §6.1's availability gate: the core's compile line has the flag
grep -q '^core cflags: .*-Werror=unguarded-availability-new' "$build/core.log" || die "the core's compile line lacks -Werror=unguarded-availability-new"
"$root/core/build.sh" ids "$build/core" > /dev/null || die "the core's ids weren't generated"
core_flags+=(-D CORE_LINKED -import-objc-header "$root/core/core.h" "$build/core/CoreIds.swift" "$root"/app/Shell/*.swift)
echo "The shared C core linked ($core_lib)"

echo "Compiling RB Stems Plus $version..."
for arch in arm64 x86_64; do
  xcrun swiftc -O -sdk "$sdk" -target "$arch-apple-macos12.0" \
    "$root"/app/Sources/*.swift "$build/Keys.swift" "$build/Version.swift" "${core_flags[@]}" ${test_flags[@]+"${test_flags[@]}"} \
    "$build/core/$arch/$core_lib.a" -o "$build/$exe-$arch"
done
# the input list (header): the folder the sources come from (a glob, so an added or removed file
# counts), the keys compiled in, then what is copied or compiled below
inputs=(app/Sources/ app/Shell/ scripts/build-app.sh)
# the core's share (SO7): the input lists core/build.sh wrote next to the archive it linked and the
# generated ids (each from its compiles' depfiles, plus core/build.sh), and core.h (the bridging
# header): never all of core/, so a Windows-only file (core/win.c, the tests) is no Mac input
for f in "$build/core/$core_lib.inputs" "$build/core/CoreIds.swift.inputs"; do
  [ -s "$f" ] || die "core/build.sh wrote no input list $f"
  while IFS= read -r d; do inputs+=("$d"); done < "$f"
done
inputs+=(core/core.h)
if [ -n "$test_base" ]; then inputs+=(app/Probe/ app/Spike/); else inputs+=(keys/release.pub.pem keys/backup.pub.pem); fi
lipo -create "$build/$exe-arm64" "$build/$exe-x86_64" -output "$app/Contents/MacOS/$exe"

echo "Assembling the bundle..."
cp "$root/app/Info.plist" "$app/Contents/Info.plist"
inputs+=(app/Info.plist app/Watcher-Info.plist NOTICE LICENSE app/icon/RBStemsPlus.icon/)
/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString $version" \
                        -c "Set :CFBundleVersion $version" "$app/Contents/Info.plist"
printf 'APPL????' > "$app/Contents/PkgInfo"
# the licences travel with the app (libFLAC's BSD licence asks for its notice with binaries)
cp "$root/NOTICE" "$root/LICENSE" "$app/Contents/Resources/"

echo "Compiling the icon..."
xcrun actool "$root/app/icon/RBStemsPlus.icon" --compile "$app/Contents/Resources" \
  --platform macosx --minimum-deployment-target 12.0 --app-icon RBStemsPlus \
  --output-partial-info-plist "$build/icon-partial.plist" \
  --output-format human-readable-text --errors --warnings > "$build/actool.log" 2>&1 \
  || { cat "$build/actool.log"; exit 1; }
for f in Assets.car RBStemsPlus.icns; do
  [ -f "$app/Contents/Resources/$f" ] || { cat "$build/actool.log"; echo "actool made no $f"; exit 1; }
done

echo "Adding the watcher helper..."
make_helper "$app"
helper="$app/Contents/Helpers/RB Stems Plus Watcher.app"

echo "Building the prepare helper..."
# never the research build (RBSP_RESEARCH, helper/build.sh), whatever the environment says
RBSP_RESEARCH= "$root/helper/build.sh" "$build/helper" || die "the prepare helper didn't build (see above)"
prep="$app/Contents/Helpers/rbsp-prepare"
cp "$build/helper/rbsp-prepare" "$prep"
if grep -aq "rbsp-timing" "$prep"; then die "the prepare helper has the performance timing lines (helper/build.sh --test-timing)"; fi
if grep -aq -e '--keys' "$prep"; then die "the prepare helper is a research build (RBSP_RESEARCH)"; fi
# the input lists (scripts/mac-inputs.py): the app's, with the helper's from its depfiles; and
# the asset catalog's alone, the icon document actool reads (release.md §1.3's app/icon/; the
# settings of its command line here show in what scripts/bundle-compare.sh compares instead:
# assetutil's platform, deployment target and renditions)
[ -s "$build/helper/rbsp-prepare.inputs" ] || die "helper/build.sh wrote no input list"
{ printf '%s\n' "${inputs[@]}"; cat "$build/helper/rbsp-prepare.inputs"; } | LC_ALL=C sort -u > "$build/rbstemsplus-app.zip.inputs"
printf '%s\n' app/icon/RBStemsPlus.icon/ > "$build/Assets.car.inputs"

# the hardened runtime: no DYLD_INSERT_LIBRARIES or other injected code in the app that asks
# for the password (it needs no entitlement: it only runs programs, reads files and uses AppKit)
echo "Signing (ad hoc, hardened runtime, the helper first)..."
sign_app "$app"
codesign --verify --deep --strict "$app"
for code in "$helper" "$app"; do
  signed="$(codesign -dv "$code" 2>&1)"
  grep -Eq '^CodeDirectory .*flags=0x[0-9a-f]+\([^)]*runtime' <<< "$signed" \
    || { echo "$code isn't signed with the hardened runtime"; exit 1; }
done
grep -qx 'Identifier=io.github.rbstemsplus.watcher' <<< "$(codesign -dv "$helper" 2>&1)" \
  || die "the watcher helper isn't signed as io.github.rbstemsplus.watcher"
signed="$(codesign -dv "$prep" 2>&1)"
grep -qx 'Identifier=io.github.rbstemsplus.prepare' <<< "$signed" || die "the prepare helper isn't signed as io.github.rbstemsplus.prepare"
! grep -Eq '^CodeDirectory .*flags=0x[0-9a-f]+\([^)]*runtime' <<< "$signed" \
  || die "the prepare helper has the hardened runtime: library validation would refuse Pioneer's ONNX Runtime"
[ "$(lipo -archs "$prep")" = arm64 ] || die "the prepare helper isn't arm64"

# no links or special files in the app (the bootstrap and the update refuse them)
odd="$(find "$app" ! -type f ! -type d -print -quit)"
[ -z "$odd" ] || die "the app contains a link or special file: $odd"

# the bundle gates: macOS 12 still opens every slice, and no file is added or removed (an old
# app's update replaces the bundle it knows)
"$root/tests/mac/minos.sh" "$app" > "$build/minos.log" || { cat "$build/minos.log"; die "a Mach-O in the app asks for a macOS newer than 12.0"; }
for plist in "$app/Contents/Info.plist" "$helper/Contents/Info.plist"; do
  v="$(/usr/libexec/PlistBuddy -c 'Print :LSMinimumSystemVersion' "$plist" 2>/dev/null)" || die "$plist has no LSMinimumSystemVersion"
  [[ "$v" =~ ^[0-9]+(\.[0-9]+){0,2}$ ]] \
    && awk -v v="$v" 'BEGIN { split(v, a, "."); exit !(a[1] < 12 || (a[1] == 12 && a[2] + 0 == 0 && a[3] + 0 == 0)) }' \
    || die "$plist: LSMinimumSystemVersion is $v, more than 12.0"
done
"$root/tests/mac/bundle-files.sh" "$app" || die "the app's files aren't tests/mac/bundle-files.txt's"

rm -f "$dist/rbstemsplus-app.zip"
ditto -c -k --keepParent "$app" "$dist/rbstemsplus-app.zip"
# release.md §6.1's static gates on the zip itself (TEST builds too): no quarantine once unpacked, no sanitizer
"$root/tests/mac/static-gates.sh" "$dist/rbstemsplus-app.zip" > "$build/static-gates.log" 2>&1 \
  || { cat "$build/static-gates.log"; rm -f "$dist/rbstemsplus-app.zip"; die "the app zip fails a static gate (quarantine or sanitizer)"; }
echo "Built $dist/rbstemsplus-app.zip ($(lipo -archs "$app/Contents/MacOS/$exe"), with $(basename "$helper") and rbsp-prepare, version $version, keys: ${keys[*]%%:*}${test_base:+, TEST BUILD})"

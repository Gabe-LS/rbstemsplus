#!/bin/bash
# Builds "RB Stems Plus.app" (universal: arm64 + x86_64, macOS 12+) and zips it to
# dist/rbstemsplus-app.zip, the release asset the bootstrap downloads.
#
#   scripts/build-app.sh [OUT]       OUT: the folder for the zip (default dist/)
#
# Test build: with RBSTEMSPLUS_TEST_BASE_URL set (e.g. http://192.168.64.1:8765/), the app
# downloads its files from that server instead of the GitHub release, and its window title and
# log say "TEST BUILD". Compiled in (-D TEST_BUILD), never read at run time. scripts/build.sh
# --test URL sets it; a release build refuses an app with "TEST BUILD" in it.
#
# Signing keys (app/Sources/Signing.swift): the public keys payload.json's signature is checked
# with are compiled in from build/app/Keys.swift, written here. A release: keys/release.pub.pem
# and keys/backup.pub.pem (scripts/make-signing-keys.sh), and it stops if either is missing, if
# they are the same, or if one is build.sh's test key. A test build: only the test key
# RBSTEMSPLUS_TEST_KEY names (build/test-keys/test.pub.pem, made by build.sh --test), never
# the release keys.
#
# The version: VERSION is compiled in too (build/app/Version.swift, compiledVersion), as the
# oldest payload.json the app accepts (app/Sources/Payload.swift).
#
# Steps: swiftc per architecture (-O, macOS 12 target) → lipo → the bundle from app/Info.plist
# with the version from ./VERSION → the icon (app/icon/RBStemsPlus.icon) compiled with actool
# into Assets.car (macOS 26) and RBStemsPlus.icns (macOS 12–15) → the watcher helper,
# Contents/Helpers/RB Stems Plus Watcher.app (make_helper) → ad-hoc signatures with the hardened
# runtime, the helper first, then the app (sign_app; the bootstrap and the update sign again on
# the user's Mac, the same way) → zip. Needs Xcode (actool reads Icon Composer documents from
# Xcode 26 on). Intermediate files go to build/app/.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
version="$(tr -d '[:space:]' < "$root/VERSION")"
build="$root/build/app"
dist="${1:-$root/dist}"
test_base="${RBSTEMSPLUS_TEST_BASE_URL:-}"
test_key="${RBSTEMSPLUS_TEST_KEY:-}"
app="$build/RB Stems Plus.app"
exe="RB Stems Plus"
sdk="$(xcrun --sdk macosx --show-sdk-path)"

die() { echo "build-app.sh: $*" >&2; exit 1; }

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
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "VERSION must be x.y.z, not '$version'"
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
  test_flags=(-D TEST_BUILD "$build/TestBase.swift")
  echo "TEST BUILD: downloads from $test_base"
  # for the docs' screenshots: the window title without the test note (the log keeps it)
  if [ "${RBSTEMSPLUS_SCREENSHOTS:-}" = 1 ]; then test_flags+=(-D SCREENSHOT_BUILD); echo "SCREENSHOT BUILD: plain window title"; fi
elif [ -n "${RBSTEMSPLUS_SCREENSHOTS:-}" ]; then
  die "RBSTEMSPLUS_SCREENSHOTS is only for a test build"
fi

echo "Compiling RB Stems Plus $version..."
for arch in arm64 x86_64; do
  xcrun swiftc -O -sdk "$sdk" -target "$arch-apple-macos12.0" \
    "$root"/app/Sources/*.swift "$build/Keys.swift" "$build/Version.swift" ${test_flags[@]+"${test_flags[@]}"} -o "$build/$exe-$arch"
done
lipo -create "$build/$exe-arm64" "$build/$exe-x86_64" -output "$app/Contents/MacOS/$exe"

echo "Assembling the bundle..."
cp "$root/app/Info.plist" "$app/Contents/Info.plist"
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

# no links or special files in the app (the bootstrap and the update refuse them)
odd="$(find "$app" ! -type f ! -type d -print -quit)"
[ -z "$odd" ] || die "the app contains a link or special file: $odd"

rm -f "$dist/rbstemsplus-app.zip"
ditto -c -k --keepParent "$app" "$dist/rbstemsplus-app.zip"
echo "Built $dist/rbstemsplus-app.zip ($(lipo -archs "$app/Contents/MacOS/$exe"), with $(basename "$helper"), version $version, keys: ${keys[*]%%:*}${test_base:+, TEST BUILD})"

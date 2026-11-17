#!/bin/bash
# Tests of the bundle gates: tests/mac/minos.sh on tiny programs built for macOS 12, 13 and
# 10.13 (the older LC_VERSION_MIN_MACOSX load command), one and two slices; tests/mac/bundle-files.sh
# on a fake bundle made from the list, with a file added, one removed, a link, and as a zip;
# tests/mac/static-gates.sh on a tiny app zip, quarantined, with ASan and with UBSan.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
tmp="$(mktemp -d "${TMPDIR:-/tmp}/rbsp-gates-test.XXXXXX")"
trap 'rm -rf "${tmp:?}"' EXIT
pass=0; fail=0
# expect RC NAME COMMAND...: the command exits with RC
expect() {
  local want="$1" name="$2"; shift 2
  "$@" > "$tmp/out" 2>&1; local rc=$?
  if [ "$rc" = "$want" ]; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "FAIL: $name (exit $rc, expected $want)"; sed 's/^/    /' "$tmp/out"; fi
}

echo 'int main(void) { return 0; }' > "$tmp/m.c"
build() { xcrun clang -arch "$1" "-mmacosx-version-min=$2" "$tmp/m.c" -o "$3" 2> /dev/null; }
build arm64 12.0 "$tmp/a12" && build arm64 13.0 "$tmp/a13" && build x86_64 12.0 "$tmp/x12" \
  && build x86_64 10.13 "$tmp/x1013" || { echo "gates-test: clang couldn't build the test programs"; exit 1; }
lipo -create "$tmp/a12" "$tmp/x12" -output "$tmp/u12"
lipo -create "$tmp/a13" "$tmp/x12" -output "$tmp/u13"
expect 0 "minos: arm64 12.0" "$here/minos.sh" "$tmp/a12"
expect 1 "minos: arm64 13.0 fails" "$here/minos.sh" "$tmp/a13"
expect 0 "minos: arm64 13.0 with --max 13.0" "$here/minos.sh" --max 13.0 "$tmp/a13"
expect 0 "minos: x86_64 10.13 (LC_VERSION_MIN_MACOSX)" "$here/minos.sh" "$tmp/x1013"
grep -q 'minos 10.13  x86_64' "$tmp/out" || { fail=$((fail + 1)); echo "FAIL: minos: 10.13 not read from LC_VERSION_MIN_MACOSX"; }
expect 0 "minos: universal 12.0" "$here/minos.sh" "$tmp/u12"
expect 1 "minos: universal with one 13.0 slice fails" "$here/minos.sh" "$tmp/u13"
mkdir -p "$tmp/dir/sub" && cp "$tmp/u12" "$tmp/dir/sub/" && echo text > "$tmp/dir/readme"
expect 0 "minos: a folder" "$here/minos.sh" "$tmp/dir"
cp "$tmp/a13" "$tmp/dir/sub/late"
expect 1 "minos: a folder with a 13.0 file fails" "$here/minos.sh" "$tmp/dir"
mkdir "$tmp/empty" && echo text > "$tmp/empty/readme"
expect 1 "minos: no Mach-O fails" "$here/minos.sh" "$tmp/empty"

mkdir "$tmp/b"
while IFS= read -r p; do mkdir -p "$tmp/b/$(dirname "$p")" && echo x > "$tmp/b/$p"; done < "$here/bundle-files.txt"
expect 0 "bundle: the listed files" "$here/bundle-files.sh" "$tmp/b"
expect 0 "bundle: the app folder" "$here/bundle-files.sh" "$tmp/b/RB Stems Plus.app"
(cd "$tmp/b" && ditto -c -k --keepParent "RB Stems Plus.app" "$tmp/app.zip")
expect 0 "bundle: the zip" "$here/bundle-files.sh" "$tmp/app.zip"
echo x > "$tmp/b/RB Stems Plus.app/Contents/Resources/extra"
expect 1 "bundle: an added file fails" "$here/bundle-files.sh" "$tmp/b"
rm "$tmp/b/RB Stems Plus.app/Contents/Resources/extra" "$tmp/b/RB Stems Plus.app/Contents/PkgInfo"
expect 1 "bundle: a removed file fails" "$here/bundle-files.sh" "$tmp/b"
echo x > "$tmp/b/RB Stems Plus.app/Contents/PkgInfo"
ln -s Info.plist "$tmp/b/RB Stems Plus.app/Contents/link"
expect 1 "bundle: a link fails" "$here/bundle-files.sh" "$tmp/b"

# static-gates.sh: no quarantine, no sanitizer (release.md §6.1, T25)
mkdir -p "$tmp/g/X.app/Contents/MacOS"
cp "$tmp/u12" "$tmp/g/X.app/Contents/MacOS/X" && echo text > "$tmp/g/X.app/Contents/readme"
(cd "$tmp/g" && ditto -c -k --keepParent X.app "$tmp/g.zip")
expect 0 "static gates: a plain app" "$here/static-gates.sh" "$tmp/g.zip"
xattr -w com.apple.quarantine '0083;00000000;test;' "$tmp/g/X.app/Contents/readme"
(cd "$tmp/g" && ditto -c -k --keepParent X.app "$tmp/gq.zip")
expect 1 "static gates: a quarantined file fails" "$here/static-gates.sh" "$tmp/gq.zip"
xattr -d com.apple.quarantine "$tmp/g/X.app/Contents/readme"
if xcrun clang -arch arm64 -mmacosx-version-min=12.0 -fsanitize=address "$tmp/m.c" -o "$tmp/g/X.app/Contents/MacOS/X" 2> /dev/null; then
  (cd "$tmp/g" && ditto -c -k --keepParent X.app "$tmp/gs.zip")
  expect 1 "static gates: an ASan executable fails" "$here/static-gates.sh" "$tmp/gs.zip"
else fail=$((fail + 1)); echo "FAIL: static gates: clang couldn't build the ASan test program"; fi
# a signed add, so the binary has UBSan handler symbols as well as the runtime (Apple's clang has no minimal runtime)
printf 'int add(int a, int b) { return a + b; }\nint main(int c, char **v) { (void)v; return add(c, 1); }\n' > "$tmp/u.c"
if xcrun clang -arch arm64 -mmacosx-version-min=12.0 -fsanitize=undefined "$tmp/u.c" -o "$tmp/g/X.app/Contents/MacOS/X" 2> /dev/null; then
  (cd "$tmp/g" && ditto -c -k --keepParent X.app "$tmp/gu.zip")
  expect 1 "static gates: a UBSan executable fails" "$here/static-gates.sh" "$tmp/gu.zip"
else fail=$((fail + 1)); echo "FAIL: static gates: clang couldn't build the UBSan test program"; fi
expect 2 "static gates: no zip" "$here/static-gates.sh" "$tmp/none.zip"

echo "gates-test: $pass passed, $fail failed"
[ "$fail" = 0 ]

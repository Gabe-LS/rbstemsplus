#!/bin/bash
# Tests of scripts/bundle-compare.sh on small real bundles: a tiny program signed ad hoc as an app,
# with an Info.plist, a resource and an asset catalog (two actool runs on the app's icon, a
# second apart: the same content, other bytes, as two builds give). Identical bundles; only the
# asset catalog new (the same code: the catalog set aside, the signature's hashes and the seal's
# entry differ only); and each kind of difference that must count: an Info.plist, a resource,
# the code, the signature's settings, a file mode, a link, a missing file, an asset catalog with
# other content. Run outside a sandbox (actool).
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
cmp_sh="$root/scripts/bundle-compare.sh"
tmp="$(mktemp -d "${TMPDIR:-/tmp}/rbsp-bundle-compare-test.XXXXXX")"
trap 'rm -rf "${tmp:?}"' EXIT
pass=0; fail=0
# expect RC NAME TEXT A B: the compare of A and B exits with RC, its output holding TEXT
expect() {
  local want="$1" name="$2" text="$3" rc=0
  "$cmp_sh" "$4" "$5" > "$tmp/out" 2>&1 || rc=$?
  if [ "$rc" = "$want" ] && grep -qF -- "$text" "$tmp/out"; then pass=$((pass + 1))
  else fail=$((fail + 1)); echo "FAIL: $name (exit $rc, expected $want with '$text')"; sed 's/^/    /' "$tmp/out"; fi
}

# two asset catalogs of the icon, and one with other content (a tiny catalog of one color)
for x in a b; do
  mkdir -p "$tmp/car-$x"
  xcrun actool "$root/app/icon/RBStemsPlus.icon" --compile "$tmp/car-$x" --platform macosx --minimum-deployment-target 12.0 \
    --app-icon RBStemsPlus --output-partial-info-plist "$tmp/car-$x.plist" > "$tmp/actool-$x.log" 2>&1 \
    || { echo "bundle-compare-test: actool failed (it runs outside a sandbox):"; tail -5 "$tmp/actool-$x.log"; exit 2; }
  [ "$x" = b ] || sleep 1
done
cmp -s "$tmp/car-a/Assets.car" "$tmp/car-b/Assets.car" && { echo "bundle-compare-test: two actool runs gave the same bytes: the set-aside case can't be made"; exit 2; }
mkdir -p "$tmp/other.xcassets/Tint.colorset" "$tmp/car-c"
printf '{"info":{"version":1,"author":"xcode"}}' > "$tmp/other.xcassets/Contents.json"
printf '{"colors":[{"idiom":"universal","color":{"color-space":"srgb","components":{"red":"1.000","green":"0.000","blue":"0.000","alpha":"1.000"}}}],"info":{"version":1,"author":"xcode"}}' \
  > "$tmp/other.xcassets/Tint.colorset/Contents.json"
xcrun actool "$tmp/other.xcassets" --compile "$tmp/car-c" --platform macosx --minimum-deployment-target 12.0 \
  --output-partial-info-plist "$tmp/car-c.plist" > "$tmp/actool-c.log" 2>&1 || { tail -5 "$tmp/actool-c.log"; exit 2; }

echo 'int main(void) { return 0; }' > "$tmp/m.c"
echo 'int main(void) { return 1; }' > "$tmp/m2.c"
xcrun clang -arch arm64 -arch x86_64 -mmacosx-version-min=12.0 "$tmp/m.c" -o "$tmp/m" 2> /dev/null \
  && xcrun clang -arch arm64 -arch x86_64 -mmacosx-version-min=12.0 "$tmp/m2.c" -o "$tmp/m2" 2> /dev/null \
  || { echo "bundle-compare-test: clang couldn't build the test programs"; exit 2; }
# bundle NAME CAR [EXE]: NAME/T.app, with that asset catalog and program (signed after, by sign)
bundle() {
  local d="$tmp/$1" car="$2" exe="${3:-$tmp/m}"
  local app="$d/T.app/Contents"
  rm -rf "$d"; mkdir -p "$app/MacOS" "$app/Resources"
  cp "$exe" "$app/MacOS/T"
  printf '<?xml version="1.0" encoding="UTF-8"?>\n<plist version="1.0"><dict><key>CFBundleExecutable</key><string>T</string><key>CFBundleIdentifier</key><string>io.example.t</string><key>CFBundleShortVersionString</key><string>1.0.0</string></dict></plist>\n' > "$app/Info.plist"
  echo "a resource" > "$app/Resources/r.txt"
  cp "$car" "$app/Resources/Assets.car"
}
sign() { codesign -f -s - "$@" > /dev/null 2>&1 || { echo "bundle-compare-test: codesign failed"; exit 2; }; }

bundle A "$tmp/car-a/Assets.car"; sign --options runtime "$tmp/A/T.app"
cp -R "$tmp/A" "$tmp/A2"
expect 0 identical "bundle-compare: IDENTICAL" "$tmp/A" "$tmp/A2"
(cd "$tmp/A" && ditto -c -k --keepParent T.app "$tmp/A.zip")
expect 0 "identical, as a zip" "bundle-compare: IDENTICAL" "$tmp/A.zip" "$tmp/A2"

bundle B "$tmp/car-b/Assets.car"; sign --options runtime "$tmp/B/T.app"
expect 1 "a new asset catalog: the same code" "set-aside: T.app/Contents/Resources/Assets.car" "$tmp/A" "$tmp/B"
expect 1 "a new asset catalog: the signature" "only the signature differs (the same settings" "$tmp/A" "$tmp/B"
expect 1 "a new asset catalog: the seal" "the same seal apart from the entries of the files set aside" "$tmp/A" "$tmp/B"

bundle C "$tmp/car-b/Assets.car"; echo "another resource" > "$tmp/C/T.app/Contents/Resources/r.txt"; sign --options runtime "$tmp/C/T.app"
expect 2 "a resource too" "bundle-compare: DIFFERENT" "$tmp/A" "$tmp/C"
bundle D "$tmp/car-a/Assets.car"; sed -i '' 's/1\.0\.0/1.0.1/' "$tmp/D/T.app/Contents/Info.plist"; sign --options runtime "$tmp/D/T.app"
expect 2 "an Info.plist" "DIFFERS: T.app/Contents/Info.plist" "$tmp/A" "$tmp/D"
bundle E "$tmp/car-a/Assets.car" "$tmp/m2"; sign --options runtime "$tmp/E/T.app"
expect 2 "the code" "the code differs" "$tmp/A" "$tmp/E"
bundle F "$tmp/car-a/Assets.car"; sign "$tmp/F/T.app"
expect 2 "the signature's settings (no hardened runtime)" "the signatures' settings differ" "$tmp/A" "$tmp/F"
bundle G "$tmp/car-c/Assets.car"; sign --options runtime "$tmp/G/T.app"
expect 2 "an asset catalog with other content" "assetutil --info differs" "$tmp/A" "$tmp/G"
cp -R "$tmp/A" "$tmp/H"; chmod 600 "$tmp/H/T.app/Contents/Resources/r.txt"
expect 2 "a file mode" "the entries differ" "$tmp/A" "$tmp/H"
cp -R "$tmp/A" "$tmp/I"; ln -s r.txt "$tmp/I/T.app/Contents/Resources/link"
expect 2 "a link" "a link or special file in B" "$tmp/A" "$tmp/I"
cp -R "$tmp/A" "$tmp/J"; rm "$tmp/J/T.app/Contents/Resources/r.txt"
expect 2 "a missing file" "the file lists differ" "$tmp/A" "$tmp/J"
expect 3 "no such bundle" "no zip or folder" "$tmp/A" "$tmp/nothing"

echo "bundle-compare-test: $pass passed, $fail failed"
[ "$fail" = 0 ]

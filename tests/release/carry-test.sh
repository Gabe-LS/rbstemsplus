#!/bin/bash
# Tests the Mac carry-forward of scripts/release.sh (release.md §1.3, T23a, T23b) in the sandbox
# of tests/release/sandbox.sh (fake repositories, a fake gh, a throwaway key, fake builds that
# write input lists; the real scripts/bundle-compare.sh and scripts/mac-inputs.py): --plan and
# its file, the real run's check against it, --mac=auto|keep|new, the decisions (the same
# files; the same code with the asset catalog's inputs unchanged; a toolchain's drift; an input
# changed: a VERSION/docs/win/tests-only commit, a FLAC_VERSION-only one, a bridge-header-only
# one, an added Swift source), app.commit and bridge.commit (a carried file keeps its own, and a
# payload without them falls back to its top-level commit), and the previous release's checks
# (its signature, its files' SHA-256s). The input lists the real builds derive from their
# depfiles: tests/release/mac-build-test.sh.
#
#   tests/release/carry-test.sh [RELEASE_SH]    run outside a sandbox: a pseudo-terminal, and
#                                               Xcode's actool for two asset catalogs
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
rel="$(cd "$(dirname "${1:-$root/scripts/release.sh}")" && pwd)/$(basename "${1:-$root/scripts/release.sh}")"
[ -f "$rel" ] || { echo "carry-test: no release.sh at $rel" >&2; exit 2; }
. "$here/sandbox.sh"

# two asset catalogs of one icon, a second apart: the same content, other bytes (as two builds)
export FAKE_CARS="$t/cars"
mkdir -p "$FAKE_CARS/a" "$FAKE_CARS/b"
for x in a b; do
  xcrun actool "$root/app/icon/RBStemsPlus.icon" --compile "$FAKE_CARS/$x" --platform macosx --minimum-deployment-target 12.0 \
    --app-icon RBStemsPlus --output-partial-info-plist "$FAKE_CARS/$x.plist" > "$FAKE_CARS/$x.log" 2>&1 \
    || { echo "carry-test: actool failed (Xcode's actool runs outside a sandbox):"; tail -5 "$FAKE_CARS/$x.log"; exit 2; }
  [ "$x" = b ] || sleep 1
done
cp "$FAKE_CARS/a/Assets.car" "$FAKE_CARS/A.car"; cp "$FAKE_CARS/b/Assets.car" "$FAKE_CARS/B.car"
cmp -s "$FAKE_CARS/A.car" "$FAKE_CARS/B.car" && { echo "carry-test: two actool runs gave the same Assets.car: the set-aside cases can't be made"; exit 2; }

push() { g -C "$dev" push -q origin main 2> /dev/null; }
commit_all() { g -C "$dev" add -A && g -C "$dev" commit -q -m "$1" && push; }
pub_head() { g -C "$t/public" rev-parse HEAD; }
undo() { { g -C "$dev" revert --no-edit HEAD > /dev/null && push; } || bad "couldn't revert $(g -C "$dev" log -1 --format=%s)"; }
rsha() { shasum -a 256 "$FAKE_GH/releases/$1/$2" | cut -d' ' -f1; }
# logged NAME TEXT: the run's output holds TEXT (fixed)
logged() { if grep -qF -- "$2" "$t/$1.log"; then ok; else bad "$1: no '$2' in its output"; tail -5 "$t/$1.log" | sed 's/^/    /'; fi; }
# payload TAG KEY VALUE: the release's payload.json says it
payload() { local got; got="$(pjson "$FAKE_GH/releases/$1/payload.json" "$2")"; [ "$got" = "$3" ] && ok || bad "$1: payload.json's $2 is $got, not $3"; }

# 1. the first release: no previous one, so every Mac file is new, built from its public commit
rsp c-130 v1.3.0 --key "$key"
released c-130 v1.3.0
planned c-130-plan "previous none"
planned c-130-plan "mac\.app new"
planned c-130-plan "mac\.bridge new"
planned c-130-plan "windows\.app none"
p130="$(pub_head)"; zip130="$(rsha v1.3.0 rbstemsplus-app.zip)"; br130="$(rsha v1.3.0 libonnxruntime.1.18.0.dylib)"
payload v1.3.0 app.version 1.3.0
payload v1.3.0 app.commit "$p130"
payload v1.3.0 bridge.commit "$p130"
publish v1.3.0

# 2. a commit touching only VERSION, docs/, win/ and tests/: the same files, both carried
echo "a note" > "$dev/docs/notes.md"; echo more >> "$dev/win/README.txt"; echo more >> "$dev/tests/a-test.txt"
echo 1.3.1 > "$dev/VERSION"; commit_all 1.3.1
rsp c-131 v1.3.1 --key "$key"
released c-131 v1.3.1
planned c-131-plan "previous v1\.3\.0 [0-9a-f]{64}"
planned c-131-plan "mac\.app carry 1\.3\.0 $p130 $zip130"
planned c-131-plan "mac\.bridge carry $p130 $br130"
logged c-131 "Mac app         carried: 1.3.0, built from $p130 (the same files as v1.3.0's zip)"
logged c-131 "Mac bridge      carried: built from $p130 (the same bytes as v1.3.0's)"
cmp -s "$FAKE_GH/releases/v1.3.0/rbstemsplus-app.zip" "$FAKE_GH/releases/v1.3.1/rbstemsplus-app.zip" && ok || bad "c-131: the zip isn't v1.3.0's"
cmp -s "$FAKE_GH/releases/v1.3.0/libonnxruntime.1.18.0.dylib" "$FAKE_GH/releases/v1.3.1/libonnxruntime.1.18.0.dylib" && ok || bad "c-131: the bridge isn't v1.3.0's"
payload v1.3.1 payload_version 1.3.1
payload v1.3.1 commit "$(pub_head)"
payload v1.3.1 app.version 1.3.0
payload v1.3.1 app.commit "$p130"
payload v1.3.1 bridge.commit "$p130"
publish v1.3.1

# 3. a toolchain's drift: other bytes, no Mac input changed since the commit that built them:
# carried, and why printed; a file carried twice keeps the commit that built it
echo 1.3.2 > "$dev/VERSION"; commit_all 1.3.2
export FAKE_DRIFT=xcode-27.3 FAKE_BRIDGE_DRIFT=clang-27.3
rsp c-132 v1.3.2 --key "$key"
unset FAKE_DRIFT FAKE_BRIDGE_DRIFT
released c-132 v1.3.2
planned c-132-plan "mac\.app carry 1\.3\.0 $p130 $zip130"
planned c-132-plan "mac\.bridge carry $p130 $br130"
logged c-132 "no Mac app input changed since $p130: the toolchain's drift only"
logged c-132 "no bridge input changed since $p130: the toolchain's drift only"
logged c-132 "DIFFERS: RB Stems Plus.app/Contents/MacOS/RB Stems Plus"
payload v1.3.2 app.commit "$p130"
payload v1.3.2 bridge.commit "$p130"
publish v1.3.2

# 4. R10's rule: a new asset catalog (the same content) and nothing else: carried while its
# inputs are unchanged; with its input changed, a new app
echo 1.3.3 > "$dev/VERSION"; commit_all 1.3.3
FAKE_CAR=B rs c-car v1.3.3 --plan
planned c-car "mac\.app carry 1\.3\.0 $p130 $zip130"
logged c-car "set-aside: RB Stems Plus.app/Contents/Resources/Assets.car"
logged c-car "the asset catalog's inputs unchanged since $p130"
echo '{"icon": 2}' > "$dev/app/icon.json"; commit_all "the icon"
FAKE_CAR=B rs c-car-icon v1.3.3 --plan
planned c-car-icon "mac\.app new"
logged c-car-icon "Mac app inputs changed since $p130: app/icon.json"
# the same Assets.car bytes after all: the files are the same, so carried whatever changed
rs c-car-same v1.3.3 --plan
planned c-car-same "mac\.app carry 1\.3\.0 $p130 $zip130"
undo

# 5. an input changed: FLAC_VERSION alone (bridge/build.sh, which the helper reads), a bridge
# header alone (which the helper includes), an added Swift source: the app new (and the bridge,
# for the first two); each commit reverted after its plan
sed -i '' 's/^FLAC_VERSION=1.5.0$/FLAC_VERSION=1.5.1/' "$dev/bridge/build.sh"; commit_all "libFLAC 1.5.1"
rs c-flac v1.3.3 --plan
planned c-flac "mac\.app new"
planned c-flac "mac\.bridge new"
logged c-flac "Mac app inputs changed since $p130: bridge/build.sh"
logged c-flac "bridge inputs changed since $p130: bridge/build.sh"
undo
echo "/* a new entry point */" >> "$dev/bridge/onnxruntime_c_api.h"; commit_all "the header"
rs c-header v1.3.3 --plan
planned c-header "mac\.app new"
planned c-header "mac\.bridge new"
logged c-header "Mac app inputs changed since $p130: bridge/onnxruntime_c_api.h"
undo
# a non-ASCII name and one with a quote, which git's line output quotes (core.quotepath=true set
# explicitly): an input changed, never the toolchain's drift (Q43)
export GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=core.quotepath GIT_CONFIG_VALUE_0=true
cafe="$(printf 'app/Sources/Caf\303\251.swift')"
echo "// é" > "$dev/$cafe"; commit_all "Café"
rs c-cafe v1.3.3 --plan
planned c-cafe "mac\.app new"
logged c-cafe "Mac app inputs changed since $p130: $cafe"
grep -q "toolchain's drift" "$t/c-cafe.log" && bad "c-cafe: called the toolchain's drift" || ok
undo
echo '// "' > "$dev/app/Sources/we\"ird.swift"; commit_all "a quote"
rs c-quote v1.3.3 --plan
planned c-quote "mac\.app new"
logged c-quote "Mac app inputs changed since $p130: app/Sources/we\"ird.swift"
grep -q "toolchain's drift" "$t/c-quote.log" && bad "c-quote: called the toolchain's drift" || ok
undo
unset GIT_CONFIG_COUNT GIT_CONFIG_KEY_0 GIT_CONFIG_VALUE_0
echo "// more" > "$dev/app/Sources/More.swift"; commit_all "a Swift source"
rs c-swift v1.3.3 --plan
planned c-swift "mac\.app new"
planned c-swift "mac\.bridge carry $p130 $br130"
logged c-swift "Mac app inputs changed since $p130: app/Sources/More.swift"

# 6. --mac=keep carries both though the app changed; --mac=new makes a new app (the bridge
# still decided: carried); a real run whose decisions aren't the plan's stops
rs c-keep v1.3.3 --mac=keep --plan
planned c-keep "mac\.app carry 1\.3\.0 $p130 $zip130"
planned c-keep "mac\.bridge carry $p130 $br130"
logged c-keep "Mac app         carried: 1.3.0, built from $p130 (--mac=keep"
rs c-keep-then-auto v1.3.3 --key "$key"
refused c-keep-then-auto "the carry decisions differ from the plan's"
rsp c-133 v1.3.3 --key "$key" --mac=new
released c-133 v1.3.3
planned c-133-plan "mac\.app new"
planned c-133-plan "mac\.bridge carry $p130 $br130"
p133="$(pub_head)"
payload v1.3.3 app.version 1.3.3
payload v1.3.3 app.commit "$p133"
payload v1.3.3 bridge.commit "$p130"
publish v1.3.3

# 7. a payload.json without app.commit and bridge.commit (every one before 1.4.0): each falls
# back to its top-level commit (re-signed here with the sandbox's key, as a 1.3.0 payload is)
r="$FAKE_GH/releases/v1.3.3"
cp "$r/payload.json" "$t/payload-133.json"; cp "$r/payload.json.sig" "$t/payload-133.sig"
python3 -I -c 'import json,sys; d=json.load(open(sys.argv[1])); [d[k].pop("commit") for k in ("app","bridge")]; json.dump(d,open(sys.argv[1],"w"))' "$r/payload.json"
/usr/bin/openssl dgst -sha256 -sign "$key" -out "$r/payload.json.sig" "$r/payload.json"
echo 1.3.4 > "$dev/VERSION"; commit_all 1.3.4
rs c-fallback v1.3.4 --plan
planned c-fallback "mac\.app carry 1\.3\.3 $p133 $(rsha v1.3.3 rbstemsplus-app.zip)"
planned c-fallback "mac\.bridge carry $p133 $br130"

# 8. the previous release's checks: a payload.json whose signature doesn't verify, and a zip
# that isn't the one it names, are refused
echo " " >> "$r/payload.json"
rs c-badsig v1.3.4 --plan
refused c-badsig "doesn't verify with this commit's keys"
cp "$t/payload-133.json" "$r/payload.json"; cp "$t/payload-133.sig" "$r/payload.json.sig"
cp "$r/rbstemsplus-app.zip" "$t/zip-133"; echo "other bytes" > "$r/rbstemsplus-app.zip"
rs c-badzip v1.3.4 --plan
refused c-badzip "v1.3.3's rbstemsplus-app.zip doesn't have the SHA-256 its payload.json names"
cp "$t/zip-133" "$r/rbstemsplus-app.zip"

# 9. a plan is its commit's: after a new commit, the real run needs a new plan
rs c-134-plan v1.3.4 --plan
echo "a later note" >> "$dev/docs/notes.md"; commit_all "a note"
rs c-134-stale v1.3.4 --key "$key"
refused c-134-stale "no plan for $(g -C "$dev" rev-parse HEAD)"
rsp c-134 v1.3.4 --key "$key"
released c-134 v1.3.4
payload v1.3.4 app.version 1.3.3
payload v1.3.4 app.commit "$p133"
payload v1.3.4 bridge.commit "$p130"

publish v1.3.4

# 10. the G2 fallback (RELEASING.md, Q5): a new app whose new bridge may not ship. With the bridge
# changed, the plan makes a new bridge; with bridge/ restored to v1.3.0's sources, the rebuild is
# v1.3.0's bytes and the plan carries it, beside the new app
echo "int bridge_v2;" > "$dev/bridge/rbstems_bridge.c"; echo "// 1.3.5" >> "$dev/app/Sources/main.swift"
echo 1.3.5 > "$dev/VERSION"; commit_all 1.3.5
rs c-135-newbridge v1.3.5 --mac=new --plan
planned c-135-newbridge "mac\.bridge new"
g -C "$dev" checkout v1.3.0 -- bridge/ && commit_all "bridge: back to v1.3.0's sources" || bad "couldn't restore bridge/"
rsp c-135 v1.3.5 --key "$key" --mac=new
released c-135 v1.3.5
planned c-135-plan "mac\.app new"
planned c-135-plan "mac\.bridge carry $p130 $br130"
payload v1.3.5 app.version 1.3.5
payload v1.3.5 bridge.commit "$p130"
cmp -s "$FAKE_GH/releases/v1.3.0/libonnxruntime.1.18.0.dylib" "$FAKE_GH/releases/v1.3.5/libonnxruntime.1.18.0.dylib" && ok || bad "c-135: the bridge isn't v1.3.0's"

echo "carry-test ($rel): $pass passed, $fail failed"
[ "$fail" = 0 ]

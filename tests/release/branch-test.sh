#!/bin/bash
# Tests scripts/release.sh end to end in a sandbox: its normal release from main, a maintenance
# release with --branch NAME, the order of versions, the "windows" block guard and main's pin
# after a release from a branch (--mac=keep, --mac=new). The sandbox is tests/release/sandbox.sh
# (fake repositories, a fake gh, a throwaway key, fake builds). Every real run is preceded by its
# plan (--plan); the checks of steps 2-4 are exercised through --plan runs, which make them too.
# The Mac carry-forward itself: tests/release/carry-test.sh.
#
#   tests/release/branch-test.sh [RELEASE_SH]    RELEASE_SH: the release.sh to test (default:
#                                                this checkout's scripts/release.sh)
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
rel="$(cd "$(dirname "${1:-$root/scripts/release.sh}")" && pwd)/$(basename "${1:-$root/scripts/release.sh}")"
[ -f "$rel" ] || { echo "branch-test: no release.sh at $rel" >&2; exit 2; }
. "$here/sandbox.sh"

# 1. an ordinary release from main, its plan first; a real run without a plan is refused
rs main-130-noplan v1.3.0 --key "$key"
refused main-130-noplan "no plan for"
# G3 (T26): a real run needs a passing tart record of this commit, TEST payload and plan
g3_refusals main-130 v1.3.0 --key "$key"
# review BM3: an app that trusts this checkout's test key isn't released (step 6; build.sh's own
# guard looks in the public clone, which has no build/test-keys/)
mkdir -p "$dev/build/test-keys"
/usr/bin/openssl ecparam -name prime256v1 -genkey -noout -out "$t/test-key.pem" 2> /dev/null
/usr/bin/openssl ec -in "$t/test-key.pem" -pubout -out "$dev/build/test-keys/test.pub.pem" 2> /dev/null
tk="$(/usr/bin/openssl ec -pubin -in "$dev/build/test-keys/test.pub.pem" -outform DER 2> /dev/null | /usr/bin/base64 | tr -d '\n')"
g3_record
FAKE_APP_KEY="$tk" rs main-130-testkey v1.3.0 --key "$key"
refused main-130-testkey "the app in dist/rbstemsplus-app.zip trusts the test key"
FAKE_APP_KEY="$(/usr/bin/openssl ec -pubin -in "$dev/build/test-keys/test.pub.pem" -conv_form compressed -outform DER 2> /dev/null | /usr/bin/base64 | tr -d '\n')" rs main-130-testkeyc v1.3.0 --key "$key"
refused main-130-testkeyc "the app in dist/rbstemsplus-app.zip trusts the test key"
mv "$dev/build/test-keys/test.pub.pem" "$t/tk.pub.pem"
/usr/bin/openssl ec -pubin -in "$dev/keys/backup.pub.pem" -conv_form compressed -pubout -out "$dev/build/test-keys/test.pub.pem" 2> /dev/null
rs main-130-testkeyk v1.3.0 --key "$key"
refused main-130-testkeyk "keys/backup.pub.pem is the test key"
rm "$dev/build/test-keys/test.pub.pem"; mkdir -p "$t/ga/tag-v0.9.0/test-keys"; cp "$t/tk.pub.pem" "$t/ga/tag-v0.9.0/test-keys/test.pub.pem"
GATES_ARCHIVE="$t/ga" FAKE_APP_KEY="$tk" rs main-130-testkeya v1.3.0 --key "$key"
refused main-130-testkeya "the app in dist/rbstemsplus-app.zip trusts the test key $t/ga/tag-v0.9.0"
# review TK5: a test key that isn't P-256 (an RSA public key; its private half never leaves the pipe),
# a bootstrap.sh that trusts the test key, an app zip with no executable or two
/usr/bin/openssl genrsa 2048 2> /dev/null | /usr/bin/openssl rsa -pubout -out "$dev/build/test-keys/test.pub.pem" 2> /dev/null
rs main-130-testkeyr v1.3.0 --key "$key"
refused main-130-testkeyr "test.pub.pem isn't a P-256 public key"
mv "$t/tk.pub.pem" "$dev/build/test-keys/test.pub.pem"
FAKE_BOOTSTRAP_KEY="$tk" rs main-130-testkeyb v1.3.0 --key "$key"
refused main-130-testkeyb "dist/bootstrap.sh trusts the test key"
for n in 0 2; do
  FAKE_APP_EXES=$n rs "main-130-exes$n" v1.3.0 --key "$key"
  refused "main-130-exes$n" "dist/rbstemsplus-app.zip has no single app executable"
done
rsp main-130 v1.3.0 --key "$key"
released main-130 v1.3.0
[ "$(g -C "$t/remote/dev.git" rev-parse 'v1.3.0^{commit}' 2> /dev/null)" = "$(g -C "$dev" rev-parse HEAD)" ] && ok || bad "main-130: the tag v1.3.0 isn't on origin"
[ "$(g -C "$t/remote/Gabe-LS/rbstemsplus.git" log -1 --format=%s main)" = "RB Stems Plus 1.3.0" ] && ok || bad "main-130: the public main isn't the 1.3.0 commit"
publish v1.3.0

# 2. the maintenance branch, cut at v1.3.0: a fix, first with the branch not on origin, then
# with the branch pushed but not the fix
g -C "$dev" switch -q -c maint/1.3 v1.3.0
echo fix >> "$dev/README.md"; commit_version 1.3.1
rs b-nobranch-on-origin v1.3.1 --branch maint/1.3 --plan
refused b-nobranch-on-origin "couldn't fetch origin maint/1.3"
g -C "$dev" push -q origin 'v1.3.0^{commit}:refs/heads/maint/1.3' 2> /dev/null
rs b-unpushed v1.3.1 --branch maint/1.3 --plan
refused b-unpushed "isn't on origin/maint/1.3: push it first"
g -C "$dev" push -q origin maint/1.3 2> /dev/null
rs b-nobranch v1.3.1 --plan
refused b-nobranch "isn't on origin/main: push it first (a maintenance release from a branch: --branch NAME)"
rs b-badname v1.3.1 --key "$key" --branch 'maint..1.3'
refused b-badname "isn't a branch name"
rs b-dry v1.3.1 --branch maint/1.3 --dry-run
grep -q "HEAD is on origin/maint/1.3 as last fetched" "$t/b-dry.log" && grep -q "Dry run: nothing was committed" "$t/b-dry.log" \
  && grep -q "NOTE: not checked in a dry run: main's pin and the latest release's \"windows\" block" "$t/b-dry.log" \
  && grep -q "Mac app         carried: 1.3.0" "$t/b-dry.log" \
  && ok || { bad "b-dry: the dry run didn't pass"; tail -5 "$t/b-dry.log" | sed 's/^/    /'; }
[ ! -e "$dev/build/gates/$(g -C "$dev" rev-parse HEAD)/plan" ] && ok || bad "b-dry: the dry run wrote a plan"

# 3. the "windows" guard: a latest release whose payload.json has a windows block
mkdir -p "$FAKE_GH/releases/vwin"
printf '{"payload_version": "1.3.0", "windows": {"app": {"version": "1.3.0"}}}\n' > "$FAKE_GH/releases/vwin/payload.json"
publish vwin
rs b-windows v1.3.1 --branch maint/1.3 --plan
refused b-windows 'has a "windows" block'
# a payload.json that isn't a JSON object can't be checked: refused too
mkdir -p "$FAKE_GH/releases/vbad"
printf '["payload_version", "windows"]\n' > "$FAKE_GH/releases/vbad/payload.json"
publish vbad
rs b-badjson v1.3.1 --branch maint/1.3 --plan
refused b-badjson "couldn't read the latest release's payload.json as a JSON object"
publish v1.3.0

# 4. the maintenance release (its G3 record checked as from main)
g3_refusals b-131 v1.3.1 --key "$key" --branch maint/1.3
rsp b-131 v1.3.1 --key "$key" --branch maint/1.3
released b-131 v1.3.1
grep -q "(on maint/1.3)" "$t/b-131.log" && grep -q "its G3 tart run (from 1.0.0, 1.3.0 and the 1.3.x) was checked above" "$t/b-131.log" && ok || bad "b-131: the branch isn't named in the summary"
grep -q "G3 tart run: passed for" "$t/b-131.log" && grep -q "G3 tart run: passed for" "$t/main-130.log" && ok || bad "the G3 check didn't pass in a released run"
[ "$(g -C "$t/remote/dev.git" rev-parse 'v1.3.1^{commit}' 2> /dev/null)" = "$(g -C "$dev" rev-parse HEAD)" ] && ok || bad "b-131: the tag v1.3.1 isn't the branch commit"
g -C "$t/remote/dev.git" merge-base --is-ancestor v1.3.1 main 2> /dev/null && bad "b-131: v1.3.1 is on main" || ok
publish v1.3.1

# 5. the order of versions: an older version from the branch
commit_version 1.2.9; g -C "$dev" push -q origin maint/1.3 2> /dev/null
rs b-older v1.2.9 --branch maint/1.3 --plan
refused b-older "1.2.9 isn't newer than v1.3.1"

# 6. main's pin: the newest release came from a branch
g -C "$dev" switch -q main
echo next >> "$dev/app/Sources/main.swift"; commit_version 1.4.0; g -C "$dev" push -q origin main 2> /dev/null
rs m-pinned v1.4.0 --plan
refused m-pinned "v1.3.1, came from a branch"
# merging the branch into main (never done: fixes are cherry-picked) doesn't lift the pin
g -C "$dev" merge -q --no-ff -X ours -m "merge maint/1.3" 'v1.3.1^{commit}' && g -C "$dev" push -q origin main 2> /dev/null
g -C "$t/remote/dev.git" merge-base --is-ancestor v1.3.1 main && ok || bad "m-merged: v1.3.1 isn't merged into main"
rs m-merged v1.4.0 --plan
refused m-merged "v1.3.1, came from a branch"
rs m-merged-auto v1.4.0 --mac=auto --plan
refused m-merged-auto "v1.3.1, came from a branch"
rs m-badmac v1.4.0 --mac=old --plan
refused m-badmac "--mac takes auto, keep or new"
# keep: the branch release's Mac app and bridge carried forward, though main's app changed (v1.3.1
# itself carried 1.3.0's app: only README.md and VERSION changed)
[ "$(pjson "$FAKE_GH/releases/v1.3.1/payload.json" app.version)" = 1.3.0 ] && ok || bad "b-131: the app wasn't carried"
rs m-keep v1.4.0 --mac=keep --plan
planned m-keep "mac\.app carry 1\.3\.0 [0-9a-f]{40} $(shasum -a 256 "$FAKE_GH/releases/v1.3.1/rbstemsplus-app.zip" | cut -d' ' -f1)"
planned m-keep "mac\.bridge carry [0-9a-f]{40} [0-9a-f]{64}"
# a plan made with keep isn't the release's when the run asks for a new Mac app
rs m-keep-then-new v1.4.0 --key "$key" --mac=new
refused m-keep-then-new "the carry decisions differ from the plan's"
rsp m-new v1.4.0 --key "$key" --mac=new
released m-new v1.4.0
[ "$(pjson "$FAKE_GH/releases/v1.4.0/payload.json" app.version)" = 1.4.0 ] && ok || bad "m-new: the app isn't new"
publish v1.4.0
commit_version 1.4.1; g -C "$dev" push -q origin main 2> /dev/null
rsp m-141 v1.4.1 --key "$key"
released m-141 v1.4.1
publish v1.4.1

# 7. after 1.4.x, the branch can't ship an older version
g -C "$dev" switch -q maint/1.3
commit_version 1.3.2; g -C "$dev" push -q origin maint/1.3 2> /dev/null
rs b-132 v1.3.2 --branch maint/1.3 --plan
refused b-132 "1.3.2 isn't newer than v1.4.1"

grep -q '^gh release create' "$FAKE_GH/calls" && ok || bad "the fake gh was never asked to create a release"
echo "branch-test ($rel): $pass passed, $fail failed"
[ "$fail" = 0 ]

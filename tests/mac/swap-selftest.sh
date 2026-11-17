#!/bin/bash
# G1.4 (mac-core §9): `--swap` runs core_selftest() first, and a new app whose self-test fails is
# refused, so the old app stays. On the host, in a scratch folder, with two core-linked TEST apps:
# GOOD (given, or built here) and BROKEN, built here with RBSTEMSPLUS_TEST_BROKEN_SELFTEST=1 and
# --broken-selftest-test (core_selftest() returns 9). This script is the only caller that passes
# that flag (review Q30): scripts/build.sh and build-app.sh refuse the variable without it.
#
#   tests/mac/swap-selftest.sh [GOOD.zip]
#
# The builds use scripts/build-app.sh (its build/app/ is replaced) with the test key
# build/test-keys/test.pub.pem (scripts/build.sh --test makes it). Run outside a sandbox
# (swiftc's module cache, actool).
#
# Runs each new app's own executable as updateScript does (`"$STAGE/Contents/MacOS/RB Stems Plus"
# --swap "$STAGE" "$APP"`), the old app at "$APP" (a copy of GOOD): the broken one exits 1, says
# why on stderr and leaves both folders where they were (updateScript then decides "failed swap"
# from what is at the app's place: app/Tests/main.swift's update cases); the good one swaps.
# Nothing but the scratch folder and build/app/ is touched; no window opens (--swap reads only the
# Info.plists).
set -uo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
[ $# -le 1 ] || { echo "usage: $0 [GOOD.zip]" >&2; exit 2; }
key="$root/build/test-keys/test.pub.pem"
[ -f "$key" ] || { echo "swap-selftest.sh: no $key (scripts/build.sh --test URL makes it)" >&2; exit 2; }
# a plain path (no "//": --swap refuses anything else), with the links resolved
t="${TMPDIR:-/tmp}"
tmp="$(mktemp -d "${t%/}/rbsp-swap-selftest.XXXXXX")" || { echo "swap-selftest.sh: no scratch folder" >&2; exit 2; }
tmp="$(cd "$tmp" && pwd -P)" && [ -n "$tmp" ] && [ -d "$tmp" ] || { echo "swap-selftest.sh: no scratch folder" >&2; exit 2; }
trap 'rm -rf "${tmp:?}"' EXIT
# build NAME [FLAG]: a core-linked TEST app's zip into $tmp/NAME/
build() {
  echo "== building the $1 TEST app"
  env -u RBSTEMSPLUS_SCREENSHOTS RBSTEMSPLUS_TEST_BASE_URL=http://192.168.64.1:8765/ RBSTEMSPLUS_TEST_KEY="$key" \
    ${2:+RBSTEMSPLUS_TEST_BROKEN_SELFTEST=1} "$root/scripts/build-app.sh" "$tmp/$1" ${2:+"$2"} > "$tmp/$1.log" 2>&1 \
    || { tail -20 "$tmp/$1.log"; echo "swap-selftest.sh: the $1 app didn't build" >&2; exit 2; }
}
good="${1:-}"
if [ -z "$good" ]; then build good; good="$tmp/good/rbstemsplus-app.zip"; fi
[ -f "$good" ] || { echo "swap-selftest.sh: no $good" >&2; exit 2; }
build broken --broken-selftest-test
broken="$tmp/broken/rbstemsplus-app.zip"
grep -q "the core's self-test fails" "$tmp/broken.log" || { echo "swap-selftest.sh: the broken build didn't link the broken core" >&2; exit 2; }

pass=0 fail=0
ok() { if eval "$1"; then pass=$((pass + 1)); echo "ok: $2"; else fail=$((fail + 1)); echo "FAIL: $2"; fi; }
unpack() { mkdir -p "$tmp/u.$2" && ditto -x -k "$1" "$tmp/u.$2" && ditto "$tmp/u.$2/RB Stems Plus.app" "$3"; }
ino() { stat -f '%d:%i' "$1"; }
exe="Contents/MacOS/RB Stems Plus"
d="$tmp/Applications"; mkdir -p "$d"
app="$d/RB Stems Plus.app" stage="$d/.RB Stems Plus.update.app"

for z in "$good" "$broken"; do
  u="$tmp/check"; rm -rf "$u"; mkdir -p "$u"; ditto -x -k "$z" "$u"
  # grep reads nm's whole output: with grep -q, nm can die of SIGPIPE on a large binary, and
  # pipefail then fails the check though the symbol is there
  ok "nm '$u/RB Stems Plus.app/$exe' | grep '_core_selftest' > /dev/null" "$z: the core is linked (core_selftest)"
  ok "grep -aq 'TEST BUILD' '$u/RB Stems Plus.app/$exe'" "$z: a TEST build"
done

# a broken new app: refused, the old app stays
unpack "$good" old "$app"; unpack "$broken" new "$stage"
before_app="$(ino "$app")" before_stage="$(ino "$stage")" sum="$(shasum -a 256 "$app/$exe" | cut -d' ' -f1)"
out="$("$stage/$exe" --swap "$stage" "$app" 2>&1)"; rc=$?
ok "[ $rc = 1 ]" "broken: --swap exits 1 (was $rc)"
ok "[[ \"\$out\" == *\"the new app's core failed its self-test (check 9)\"* ]]" "broken: it says why ($out)"
ok "[ \"$(ino "$app")\" = '$before_app' ] && [ \"$(ino "$stage")\" = '$before_stage' ]" "broken: nothing was exchanged: the old app is at the app's place"
ok "[ \"$(shasum -a 256 "$app/$exe" | cut -d' ' -f1)\" = '$sum' ]" "broken: the old app's executable unchanged"

# a good new app: swapped in one step
rm -rf "$stage"; unpack "$good" new2 "$stage"
before_stage="$(ino "$stage")"
out="$("$stage/$exe" --swap "$stage" "$app" 2>&1)"; rc=$?
ok "[ $rc = 0 ] && [ -z \"\$out\" ]" "good: --swap exits 0 silently (was $rc: $out)"
ok "[ \"$(ino "$app")\" = '$before_stage' ] && [ \"$(ino "$stage")\" = '$before_app' ]" "good: the new app is at the app's place, the old one at the staging name"

# the fence: without the flag both build scripts refuse the variable
v="$(RBSTEMSPLUS_TEST_BROKEN_SELFTEST=1 "$root/scripts/build-app.sh" "$tmp/refused" 2>&1)"; rc=$?
ok "[ $rc = 1 ] && [[ \"\$v\" == *'pass --broken-selftest-test'* ]] && [ ! -e '$tmp/refused' ]" "build-app.sh refuses the variable without the flag (exit $rc)"
v="$(RBSTEMSPLUS_TEST_BROKEN_SELFTEST=1 "$root/scripts/build.sh" --test http://192.168.64.1:8765/ 2>&1)"; rc=$?
ok "[ $rc = 1 ] && [[ \"\$v\" == *'pass --broken-selftest-test'* ]]" "build.sh --test refuses the variable without the flag (exit $rc)"
v="$(RBSTEMSPLUS_TEST_BROKEN_SELFTEST=1 "$root/scripts/build.sh" 2>&1)"; rc=$?
ok "[ $rc = 1 ] && [[ \"\$v\" == *'RBSTEMSPLUS_TEST_BROKEN_SELFTEST or RBSTEMSPLUS_TEST_WIN_ORT_SHA256 is set: unset it for a release'* ]]" "build.sh (a release) refuses the variable (exit $rc)"

echo "swap-selftest: $pass passed, $fail failed"
[ "$fail" = 0 ]

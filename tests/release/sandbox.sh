# The sandbox scripts/release.sh is tested in (sourced by tests/release/branch-test.sh and
# tests/release/carry-test.sh). Nothing real is touched: the development and public
# repositories are bare repositories in a scratch folder (the public one at a path ending in
# Gabe-LS/rbstemsplus.git, as release.sh requires), gh is a fake that keeps "releases" in a folder,
# the signing key is a throwaway one made here (unencrypted), HOME is a scratch folder, and the
# builds are fakes committed in the sandbox repository: scripts/build-app.sh and bridge/build.sh
# write files made from their inputs (and write input lists, as the real ones do), and
# scripts/build.sh assembles dist/ in a plan's shape (--plan FILE --carry DIR). The real
# scripts/bundle-compare.sh and scripts/mac-inputs.py are copied in. release.sh runs in a
# pseudo-terminal, so it has the Terminal it asks for, and answers "yes" at its prompt.
#
# Sourced with rel set (the release.sh to test) and root (this checkout). Leaves: t (the scratch
# folder, removed on exit unless KEEP=1), dev (the sandbox checkout), key (the throwaway key),
# FAKE_GH (the fake's folder), and the helpers below.

t="$(mktemp -d "${TMPDIR:-/tmp}/rbsp-release-test.XXXXXX")"
t="$(cd "$t" && pwd -P)"
trap 'if [ "${KEEP:-0}" = 1 ]; then echo "kept $t"; else rm -rf "${t:?}"; fi' EXIT
export HOME="$t/home" GIT_CONFIG_NOSYSTEM=1 FAKE_GH="$t/gh"
mkdir -p "$HOME" "$FAKE_GH/releases" "$t/bin"
pass=0; fail=0
ok() { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "FAIL: $*"; }

# ---------------------------------------------------------------- the fake gh
cat > "$t/bin/gh" <<'GH'
#!/bin/bash
# a fake gh for release.sh: releases are folders in $FAKE_GH/releases, "latest" names the newest
# published one (the test publishes by writing it)
s="$FAKE_GH"; echo "gh $*" >> "$s/calls"
[ "${1:-}" = auth ] && exit 0
[ "${1:-}" = release ] || { echo "fake gh: unexpected: $*" >&2; exit 1; }
sub="$2"; shift 2
tag=""; repo=""; dir="."; json=""; q=""; pats=(); files=()
while [ $# -gt 0 ]; do
  case "$1" in
    --repo) repo="$2"; shift 2 ;;
    --dir) dir="$2"; shift 2 ;;
    --json) json="$2"; shift 2 ;;
    -q) q="$2"; shift 2 ;;
    --pattern) pats+=("$2"); shift 2 ;;
    --title|--notes|--notes-file) shift 2 ;;
    --draft|--verify-tag) shift ;;
    -*) echo "fake gh: unknown option $1" >&2; exit 1 ;;
    *) if [ -z "$tag" ]; then tag="$1"; else files+=("$1"); fi; shift ;;
  esac
done
[ "$repo" = Gabe-LS/rbstemsplus ] || { echo "fake gh: repo $repo" >&2; exit 1; }
case "$sub" in
  view)
    r="$s/releases/$tag"; [ -d "$r" ] || { echo "release not found" >&2; exit 1; }
    case "$json" in
      isDraft) if [ -n "$q" ]; then echo true; else echo '{"isDraft":true}'; fi ;;
      assets) for f in "$r"/*; do echo "$(basename "$f") $(stat -f %z "$f")"; done ;;
      *) exit 1 ;;
    esac ;;
  download)
    [ -n "$tag" ] || tag="$(cat "$s/latest" 2>/dev/null)"
    r="$s/releases/$tag"; [ -n "$tag" ] && [ -d "$r" ] || { echo "release not found" >&2; exit 1; }
    mkdir -p "$dir"
    for p in "${pats[@]}"; do cp "$r/$p" "$dir/" || exit 1; done ;;
  create)
    r="$s/releases/$tag"; [ ! -d "$r" ] || { echo "a release $tag exists" >&2; exit 1; }
    mkdir -p "$r" && cp "${files[@]}" "$r/" ;;
  *) exit 1 ;;
esac
GH
chmod 755 "$t/bin/gh"
export PATH="$t/bin:$PATH"

# ---------------------------------------------------------------- the repositories
g() { git -c init.defaultBranch=main -c user.name="Test Dev" -c user.email=dev@example.invalid "$@"; }
mkdir -p "$t/remote/Gabe-LS"
g init -q --bare "$t/remote/dev.git"
g init -q --bare "$t/remote/Gabe-LS/rbstemsplus.git"
# the public repository's history so far: one release, 1.2.0, by Gabe-LS
g init -q "$t/seed" && echo old > "$t/seed/README.md" && g -C "$t/seed" add -A \
  && GIT_COMMITTER_NAME=Gabe-LS GIT_COMMITTER_EMAIL=1+Gabe-LS@users.noreply.github.com \
     g -C "$t/seed" commit -q --author "Gabe-LS <1+Gabe-LS@users.noreply.github.com>" -m "RB Stems Plus 1.2.0" \
  && g -C "$t/seed" tag v1.2.0 && g -C "$t/seed" push -q "$t/remote/Gabe-LS/rbstemsplus.git" main v1.2.0 \
  || { echo "release test: couldn't seed the public repository"; exit 1; }
g clone -q "$t/remote/Gabe-LS/rbstemsplus.git" "$t/public"
g clone -q "$t/remote/dev.git" "$t/dev" 2> /dev/null
dev="$t/dev"
g -C "$dev" config user.name "Test Dev"; g -C "$dev" config user.email dev@example.invalid
g -C "$dev" config rbstemsplus.publicEmail 12345678+Gabe-LS@users.noreply.github.com
g -C "$dev" config rbstemsplus.publicClone "$t/public"
g -C "$t/public" config user.name "Test Dev"; g -C "$t/public" config user.email dev@example.invalid

# the throwaway signing key (P-256, unencrypted: no passphrase prompt) and its public half
key="$t/key.pem"
(umask 077 && /usr/bin/openssl ecparam -name prime256v1 -genkey -noout -out "$key")
mkdir -p "$dev/keys" "$dev/scripts" "$dev/docs" "$dev/app/Sources" "$dev/bridge" "$dev/win" "$dev/tests"
/usr/bin/openssl ec -in "$key" -pubout -out "$dev/keys/release.pub.pem" 2> /dev/null
/usr/bin/openssl ecparam -name prime256v1 -genkey -noout | /usr/bin/openssl ec -pubout -out "$dev/keys/backup.pub.pem" 2> /dev/null
cp "$rel" "$dev/scripts/release.sh" && chmod 755 "$dev/scripts/release.sh"
cp "$root/scripts/unwrap-notes.py" "$root/scripts/bundle-compare.sh" "$root/scripts/mac-inputs.py" "$dev/scripts/"
# the real G3 check (release.sh runs tests/mac/g3.sh check before it builds)
mkdir -p "$dev/tests/mac" && cp "$root/tests/mac/g3.sh" "$dev/tests/mac/" && chmod 755 "$dev/tests/mac/g3.sh"
printf 'dist/\ndist-test/\nbuild/\n' > "$dev/.gitignore"
printf 'docs/internal-notes.md\n' > "$dev/.publicignore"
echo "kept here" > "$dev/docs/internal-notes.md"
echo "sandbox" > "$dev/README.md"
echo "print(\"the app\")" > "$dev/app/Sources/main.swift"
echo "int bridge;" > "$dev/bridge/rbstems_bridge.c"
echo "/* the ORT API */" > "$dev/bridge/onnxruntime_c_api.h"
echo "Windows" > "$dev/win/README.txt"
echo "a test" > "$dev/tests/a-test.txt"
echo '{"icon": 1}' > "$dev/app/icon.json"
# the fake Mac app build: a bundle made from its inputs (the Swift sources, the bridge header the
# helper includes, bridge/build.sh's FLAC_VERSION), stamped with RBSTEMSPLUS_APP_VERSION or
# VERSION; FAKE_DRIFT changes its bytes without an input change (a new toolchain). With
# FAKE_CARS set (a folder with A.car and B.car, two actool runs on one icon: the same content,
# other bytes), its Assets.car is FAKE_CAR's (default A), as a rebuild gives a new one. FAKE_APP_KEY:
# a key (base64) the app's executable holds, as a test build's trusts its test key.
cat > "$dev/scripts/build-app.sh" <<'BUILD'
#!/bin/bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
v="${RBSTEMSPLUS_APP_VERSION:-$(tr -d '[:space:]' < "$root/VERSION")}"
out="${1:-$root/dist}"; b="$root/build/app"; app="$b/RB Stems Plus.app"
rm -rf "$b"; mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources" "$out"
echo "CFBundleShortVersionString $v" > "$app/Contents/Info.plist"
{ cat "$root"/app/Sources/*.swift "$root/bridge/onnxruntime_c_api.h"; sed -n 's/^FLAC_VERSION=//p' "$root/bridge/build.sh"
  [ -z "${FAKE_DRIFT:-}" ] || echo "built by toolchain $FAKE_DRIFT"
  [ -z "${FAKE_APP_KEY:-}" ] || echo "trusted key $FAKE_APP_KEY"; } > "$app/Contents/MacOS/RB Stems Plus"
[ -z "${FAKE_CARS:-}" ] || cp "$FAKE_CARS/${FAKE_CAR:-A}.car" "$app/Contents/Resources/Assets.car"
printf '%s\n' app/Sources/ app/icon.json bridge/build.sh bridge/onnxruntime_c_api.h scripts/build-app.sh > "$b/rbstemsplus-app.zip.inputs"
echo app/icon.json > "$b/Assets.car.inputs"
rm -f "$out/rbstemsplus-app.zip"
ditto -c -k --keepParent "$app" "$out/rbstemsplus-app.zip"
BUILD
# the fake bridge build: its source and header, its FLAC_VERSION; FAKE_BRIDGE_DRIFT as FAKE_DRIFT
cat > "$dev/bridge/build.sh" <<'BUILD'
#!/bin/sh
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
FLAC_VERSION=1.5.0
out="${1:-$here/../build/bridge}"; mkdir -p "$out"
{ cat "$here/rbstems_bridge.c" "$here/onnxruntime_c_api.h"; echo "flac $FLAC_VERSION"
  [ -z "${FAKE_BRIDGE_DRIFT:-}" ] || echo "built by toolchain $FAKE_BRIDGE_DRIFT"; } > "$out/libonnxruntime.1.18.0.dylib"
printf '%s\n' bridge/build.sh bridge/onnxruntime_c_api.h bridge/rbstems_bridge.c > "$out/libonnxruntime.1.18.0.dylib.inputs"
BUILD
# the fake build: dist/ in the plan's shape (release.sh passes --plan FILE [--carry DIR]),
# payload.json naming the public commit and each Mac file's version and commit. FAKE_BOOTSTRAP_KEY:
# a key (base64) bootstrap.sh trusts, as a test build's does; FAKE_APP_EXES=0|2: the app zip with
# its executable left out, or with a second one (another .app at the same depth)
cat > "$dev/scripts/build.sh" <<'BUILD'
#!/bin/bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
v="$(tr -d '[:space:]' < "$root/VERSION")"
c="$(git -C "$root" rev-parse HEAD)"
plan=""; carry=""
while [ $# -gt 0 ]; do case "$1" in --plan) plan="$2"; shift 2 ;; --carry) carry="$2"; shift 2 ;; *) exit 2 ;; esac; done
d="$root/dist"; mkdir -p "$d"
read -r _ app_kind app_v app_c _ < <(grep '^mac\.app ' "$plan")
read -r _ br_kind br_c _ < <(grep '^mac\.bridge ' "$plan")
if [ "$app_kind" = carry ]; then cp "$carry/rbstemsplus-app.zip" "$d/"
else "$root/scripts/build-app.sh" "$d"; app_v="$v"; app_c="$c"; fi
if [ "$br_kind" = carry ]; then cp "$carry/libonnxruntime.1.18.0.dylib" "$d/"
else "$root/bridge/build.sh" "$root/build/bridge"; cp "$root/build/bridge/libonnxruntime.1.18.0.dylib" "$d/"; br_c="$c"; fi
for f in stemsplus-model.onnx bootstrap.sh NOTICE LICENSE; do echo "$f $v" > "$d/$f"; done
[ -z "${FAKE_BOOTSTRAP_KEY:-}" ] || echo "trusted key $FAKE_BOOTSTRAP_KEY" >> "$d/bootstrap.sh"
if [ -n "${FAKE_APP_EXES:-}" ]; then
  python3 -I - "$d/rbstemsplus-app.zip" "$FAKE_APP_EXES" <<'PY'
import sys, zipfile
p, n = sys.argv[1], int(sys.argv[2])
exe = "RB Stems Plus.app/Contents/MacOS/RB Stems Plus"
z = zipfile.ZipFile(p); items = [(i, z.read(i.filename)) for i in z.infolist()]; z.close()
with zipfile.ZipFile(p, "w") as o:
    for i, b in items:
        if n or i.filename != exe:
            o.writestr(i, b)
    if n == 2:
        o.writestr(zipfile.ZipInfo("RB Stems Plus copy.app/Contents/MacOS/RB Stems Plus", (1980, 1, 1, 0, 0, 0)), b"another app")
PY
fi
s() { shasum -a 256 "$d/$1" | cut -d' ' -f1; }
cat > "$d/payload.json" <<EOF
{"payload_version": "$v", "commit": "$c",
 "app": {"version": "$app_v", "commit": "$app_c", "zip": "rbstemsplus-app.zip", "sha256": "$(s rbstemsplus-app.zip)"},
 "bridge": {"file": "libonnxruntime.1.18.0.dylib", "commit": "$br_c", "sha256": "$(s libonnxruntime.1.18.0.dylib)"}}
EOF
(cd "$d" && shasum -a 256 rbstemsplus-app.zip libonnxruntime.1.18.0.dylib stemsplus-model.onnx bootstrap.sh NOTICE LICENSE payload.json > SHA256SUMS)
BUILD
chmod 755 "$dev/scripts/build.sh" "$dev/scripts/build-app.sh" "$dev/bridge/build.sh"
echo 1.3.0 > "$dev/VERSION"
g -C "$dev" add -A && g -C "$dev" commit -q -m "1.3.0" && g -C "$dev" push -q origin main 2> /dev/null

# ---------------------------------------------------------------- running release.sh
# a pseudo-terminal for release.sh (it wants Terminal), which types "yes" at its prompt
cat > "$t/bin/pty-run.py" <<'PY'
import os, pty, select, sys
log = open(sys.argv[1], "wb")
pid, fd = pty.fork()
if pid == 0:
    os.execvp(sys.argv[2], sys.argv[2:])
seen, answered = b"", False
while True:
    try:
        if not select.select([fd], [], [], 300)[0]:
            break
        data = os.read(fd, 4096)
    except OSError:
        break
    if not data:
        break
    log.write(data)
    seen = (seen + data)[-200:]
    if not answered and b"Type yes to go on: " in seen:
        os.write(fd, b"yes\n")
        answered = True
sys.exit(os.waitstatus_to_exitcode(os.waitpid(pid, 0)[1]))
PY
# rs NAME ARGS...: release.sh in the sandbox checkout, in a pseudo-terminal, "yes" typed
rs() {
  local name="$1"; shift
  (cd "$dev" && python3 -I "$t/bin/pty-run.py" "$t/$name.out" scripts/release.sh "$@" < /dev/null > /dev/null 2>&1)
  tr -d '\r' < "$t/$name.out" > "$t/$name.log"
}
# g3_record [KIND]: a G3 record of HEAD (build/gates/<HEAD>/g3-mac.ok), as tests/mac/g3.sh run
# writes a passing one for HEAD's plan and a fake TEST payload (dist-test/payload.json), with every
# required scenario (g3.sh _t required); KIND: pass (default), partial (a --from leaving out
# v1.0.0), stub (a selftest's), failed (g3-mac.failed only), stale (the TEST payload changed since)
g3_record() {
  local c r
  c="$(g -C "$dev" rev-parse HEAD)"; r="$dev/build/gates/$c/g3-mac.ok"
  mkdir -p "$dev/dist-test" "${r%/*}"; echo "{\"commit\": \"$c\"}" > "$dev/dist-test/payload.json"
  rm -f "$r" "${r%.ok}.failed"
  { echo "g3 1"; echo "commit $c"; echo "payload_sha256 $(shasum -a 256 "$dev/dist-test/payload.json" | cut -c1-64)"
    echo "plan_sha256 $(shasum -a 256 "${r%/*}/plan" | cut -c1-64)"; echo "guests rb26-template"; echo "guest_versions rb26-template=26.6.2"
    (cd "$dev" && GATES_ARCHIVE="$t/no-archive" tests/mac/g3.sh _t required "$c" "rb26-template") \
      | while read -r gg nn; do echo "scenario $gg $nn pass 1min clicks=none"; done
    echo "coverage ok"; echo "result pass"; } > "$r"
  case "${1:-pass}" in
    partial) sed -i '' -e '/^scenario rb26-template update-v1.0.0 /d' -e 's/^coverage ok$/coverage partial (--from omits v1.0.0)/' -e 's/^result pass$/result FAIL (0 scenarios failed; coverage partial (--from omits v1.0.0))/' "$r" ;;
    stub) echo "stub (selftest)" >> "$r" ;;
    failed) sed -i '' 's/^result pass$/result FAIL (1 scenarios failed)/' "$r"; mv "$r" "${r%.ok}.failed" ;;
    stale) echo '{"changed": 1}' > "$dev/dist-test/payload.json" ;;
  esac
}
# rsp NAME ARGS...: the plan (NAME-plan, with --plan), a passing G3 record, then the real run (NAME)
rsp() {
  local name="$1"; shift
  rs "$name-plan" "$@" --plan
  g3_record
  rs "$name" "$@"
}
# g3_refusals NAME ARGS...: after NAME's plan, a real run refused for each bad G3 record (none,
# stale, partial, a stub's, failed)
g3_refusals() {
  local name="$1" k; shift
  rs "$name-plan" "$@" --plan
  rm -rf "$dev/build/gates/$(g -C "$dev" rev-parse HEAD)/g3-mac."*
  rs "$name-nog3" "$@"; refused "$name-nog3" "no passing G3 tart run for"
  grep -q "no G3 record" "$t/$name-nog3.log" && ok || bad "$name-nog3: g3.sh check's reason not shown"
  for k in stale partial stub failed; do
    g3_record "$k"; rs "$name-g3$k" "$@"; refused "$name-g3$k" "no passing G3 tart run for"
  done
  grep -q "another TEST payload" "$t/$name-g3stale.log" && grep -q "not every guest and scenario" "$t/$name-g3partial.log" \
    && grep -q "a selftest's record" "$t/$name-g3stub.log" && grep -q "no G3 record" "$t/$name-g3failed.log" && ok \
    || bad "$name: a bad G3 record's reason not shown"
}
# released NAME TAG: the run made the draft TAG
released() {
  if grep -q "Draft $2 is on Gabe-LS/rbstemsplus" "$t/$1.log" && [ -f "$FAKE_GH/releases/$2/payload.json.sig" ]; then ok
  else bad "$1: no draft $2"; tail -5 "$t/$1.log" | sed 's/^/    /'; fi
}
# refused NAME TEXT: the run stopped with a message holding TEXT, and made no draft
refused() {
  if grep -q "release.sh: .*$2" "$t/$1.log" && ! grep -q "is on Gabe-LS/rbstemsplus" "$t/$1.log"; then ok
  else bad "$1: not refused with '$2'"; tail -5 "$t/$1.log" | sed 's/^/    /'; fi
}
# planned NAME LINE: the plan run NAME wrote its plan, with LINE (a regular expression) in it
planned() {
  local f
  f="$(sed -n 's/^Plan written: //p' "$t/$1.log")"
  if [ -n "$f" ] && [ -f "$f" ] && grep -qE "^$2\$" "$f"; then ok
  else bad "$1: no plan line '$2'"; { [ -n "$f" ] && [ -f "$f" ] && sed 's/^/    plan: /' "$f"; } || tail -5 "$t/$1.log" | sed 's/^/    /'; fi
}
publish() { echo "$1" > "$FAKE_GH/latest"; }
commit_version() { echo "$1" > "$dev/VERSION" && g -C "$dev" commit -q -am "$1"; }
# pjson FILE KEY: a payload.json's value
pjson() { plutil -extract "$2" raw -o - "$1" 2>/dev/null || echo "(missing)"; }

#!/bin/bash
# Tests scripts/windows-assets.sh, the Windows part of scripts/build.sh in a plan's shape
# (release.md §1.3 "As built (T13)", T12), with a fake scripts/build-windows.sh that takes T13's
# interface (--stamp, --carry-file ENTRY=PATH; --test URL --test-key PEM) and records each call:
# no plan and a first release's plan (everything new), a carried zip and bridge (nothing built),
# a new zip with a carried x64 stub and worker (through --carry-file), a new bridge, a TEST build's
# carry from an archived TEST build (its zip and worker its own, its stubs and bridge the plan's),
# the release worker's hash from a TEST build (windows-release.sha256), bootstrap.ps1's keys,
# version and ASCII, NOTICE-windows.txt, windows.env's hashes of the very bytes shipped, and a
# refusal for every die that guards a carried or shipped file (a plan's SHA-256 that isn't the
# previous file's, a carried zip without its stubs, a missing line, a previous zip or entry that
# isn't what its payload names, a version mismatch, a carried bridge changed or not the plan's,
# a TEST archive whose stubs or bridge aren't the plan's, case-colliding names, a build that
# ignores --carry-file or drops a stub, a non-ASCII bootstrap.ps1, no release worker hash, no
# --carry). Disabling any die the BP4 ruling names, or either check on windows-release.sha256,
# fails this suite; the other dies (windows-assets.sh 110, 111, 113, 142, 166, 167, 193, 196) fail
# closed through a later guard.
#
#   tests/release/windows-assets-test.sh
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
t="$(mktemp -d "${TMPDIR:-/tmp}/rbsp-winassets-test.XXXXXX")"
trap 'if [ "${KEEP:-0}" = 1 ]; then echo "kept $t"; else rm -rf "${t:?}"; fi' EXIT
pass=0; fail=0
ok() { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "FAIL: $*"; }
check() { if eval "$1"; then ok; else bad "$2"; fi; }

# the sandbox tree: the real helper, win-carry.py, bootstrap.ps1 and win/NOTICE.txt; a fake build
r="$t/repo"
mkdir -p "$r/scripts" "$r/win" "$r/build"
cp "$root/scripts/windows-assets.sh" "$root/scripts/win-carry.py" "$root/scripts/bootstrap.ps1" "$r/scripts/"
cp "$root/win/NOTICE.txt" "$r/win/"
echo 1.4.0 > "$r/VERSION"
cat > "$r/scripts/build-windows.sh" <<'FAKE'
#!/bin/bash
# fake: a zip whose app files carry the stamp (and FAKE_DRIFT), the stubs and the worker the
# "code" (FAKE_CODE), each --carry-file entry PATH's bytes (not with FAKE_IGNORE_CARRY=1), without
# stub/FAKE_DROP; a bridge of FAKE_BRIDGE; a TEST build's windows-release.sha256, the release
# worker's ("admin " + code) hash (not with FAKE_NO_RELREF=1)
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
echo "build-windows.sh $*" >> "$root/calls"
stamp="$(cat "$root/VERSION")"; carries=(); test=""
while [ $# -gt 1 ]; do case "$1" in --stamp) stamp="$2"; shift 2 ;; --carry-file) carries+=("$2"); shift 2 ;;
  --test) test="$2"; shift 2 ;; --test-key) shift 2 ;; *) exit 2 ;; esac; done
out="$1"; mkdir -p "$out"
python3 -I - "$out" "$stamp" "${FAKE_DRIFT:-}" "${FAKE_CODE:-code1}" "$test" "${FAKE_IGNORE_CARRY:-}" "${FAKE_DROP:-}" "${FAKE_NO_RELREF:-}" ${carries[@]+"${carries[@]}"} <<'PY'
import hashlib, sys, zipfile
out, stamp, drift, code, test, ignore, drop, norel = sys.argv[1:9]
files = {"RB Stems Plus.exe": ("app " + stamp + drift + test).encode(), "x64/rbsp-core.dll": ("core " + stamp).encode(),
         "stub/x64/RB Stems Plus.exe": ("stub x64 " + code).encode(), "stub/arm64/RB Stems Plus.exe": ("stub arm64 " + code).encode(),
         "admin/RB Stems Plus.exe": ("admin " + code + test).encode()}
for c in sys.argv[9:]:
    n, p = c.split("=", 1)
    if not ignore:
        files[n] = open(p, "rb").read()
if drop:
    del files["stub/" + drop + "/RB Stems Plus.exe"]
with zipfile.ZipFile(out + "/rbstemsplus-windows.zip", "w") as z:
    for n in sorted(files):
        z.writestr(zipfile.ZipInfo(n, (1980, 1, 1, 0, 0, 0)), files[n])
if test and not norel:
    open(out + "/windows-release.sha256", "w").write("admin " + hashlib.sha256(("admin " + code).encode()).hexdigest() + "\n")
PY
printf 'bridge %s\n' "${FAKE_BRIDGE:-b1}" > "$out/rbstemsplus-bridge-win-x64.dll"
[ -z "${FAKE_RELREF_EXTRA:-}" ] || [ -z "$test" ] || echo "admin $(printf '%064d' 0)" >> "$out/windows-release.sha256"
FAKE
chmod 755 "$r/scripts/build-windows.sh" "$r/scripts/windows-assets.sh"
wa="$r/scripts/windows-assets.sh"
K="QUJD REVG"   # two "keys" (base64)
h() { shasum -a 256 "$1" | cut -d' ' -f1; }
esha() { python3 -I -c 'import hashlib, sys, zipfile; print(hashlib.sha256(zipfile.ZipFile(sys.argv[1]).read(sys.argv[2])).hexdigest())' "$1" "$2"; }
envv() { sed -n "s/^$2=//p" "$1/windows.env"; }
calls() { [ -f "$r/calls" ] && wc -l < "$r/calls" | tr -d ' ' || echo 0; }
X64="stub/x64/RB Stems Plus.exe" ARM="stub/arm64/RB Stems Plus.exe" ADM="admin/RB Stems Plus.exe"

# a previous release (1.3.0): the fake's files and a payload.json naming them
mkprev() {   # DIR VERSION [TEST]
  local d="$1"; mkdir -p "$d"
  (cd "$r" && FAKE_CODE="${FAKE_CODE:-code1}" scripts/build-windows.sh --stamp "$2" ${3:+--test https://h:1 --test-key k} "$d" > /dev/null)
  rm -f "$d/windows-release.sha256"
  python3 -I - "$d" "$2" <<'PY'
import hashlib, json, sys, zipfile
d, v = sys.argv[1:3]
z = zipfile.ZipFile(d + "/rbstemsplus-windows.zip"); h = lambda b: hashlib.sha256(b).hexdigest()
json.dump({"payload_version": v, "windows": {"app": {"version": v, "zip": "rbstemsplus-windows.zip",
  "sha256": h(open(d + "/rbstemsplus-windows.zip", "rb").read()), "admin_sha256": h(z.read("admin/RB Stems Plus.exe")),
  "stub_sha256": {"x64": h(z.read("stub/x64/RB Stems Plus.exe")), "arm64": h(z.read("stub/arm64/RB Stems Plus.exe"))}},
  "bridge": {"file": "rbstemsplus-bridge-win-x64.dll", "sha256": h(open(d + "/rbstemsplus-bridge-win-x64.dll", "rb").read())}}},
  open(d + "/payload.json", "w"))
PY
}
mkprev "$t/prev" 1.3.0
# a payload.json anew for DIR's files (VERSION), after a change to its zip
reseal() {
  python3 -I - "$1" "$2" <<'PY'
import hashlib, json, sys, zipfile
d, v = sys.argv[1:3]
z = zipfile.ZipFile(d + "/rbstemsplus-windows.zip"); h = lambda b: hashlib.sha256(b).hexdigest()
json.dump({"payload_version": v, "windows": {"app": {"version": v, "zip": "rbstemsplus-windows.zip",
  "sha256": h(open(d + "/rbstemsplus-windows.zip", "rb").read()), "admin_sha256": h(z.read("admin/RB Stems Plus.exe")),
  "stub_sha256": {"x64": h(z.read("stub/x64/RB Stems Plus.exe")), "arm64": h(z.read("stub/arm64/RB Stems Plus.exe"))}},
  "bridge": {"file": "rbstemsplus-bridge-win-x64.dll", "sha256": h(open(d + "/rbstemsplus-bridge-win-x64.dll", "rb").read())}}},
  open(d + "/payload.json", "w"))
PY
}
P="$t/prev/rbstemsplus-windows.zip"
plan() {   # FILE LINES...: a plan file with the release's other lines and these Windows ones
  local f="$1"; shift
  { echo "plan 1"; echo "version 1.4.0"; printf '%s\n' "$@"; } > "$f"
}
allnew=("windows.app new" "windows.stub.x64 new" "windows.stub.arm64 new" "windows.admin new" "windows.bridge new")

# 1. no plan (build.sh --windows): everything new, bootstrap.ps1 and NOTICE-windows.txt written
rm -f "$r/calls"; d="$t/d1"
"$wa" --version 1.4.0 --keys "$K" "$d" > "$t/1.log" 2>&1; check '[ $? = 0 ]' "1: no plan: failed ($(tail -1 "$t/1.log"))"
check '[ "$(calls)" = 1 ] && ! grep -q -- --carry-file "$r/calls"' "1: one build, no --carry-file"
check '[ "$(envv "$d" win_app_version)" = 1.4.0 ] && [ "$(envv "$d" win_zip_sha256)" = "$(h "$d/rbstemsplus-windows.zip")" ]' "1: windows.env's version and zip SHA-256"
check '[ "$(envv "$d" win_stub_x64_sha256)" = "$(esha "$d/rbstemsplus-windows.zip" "$X64")" ] && [ "$(envv "$d" win_admin_sha256)" = "$(esha "$d/rbstemsplus-windows.zip" "$ADM")" ]' "1: windows.env's stub and worker are the shipped entries'"
check '[ "$(envv "$d" win_bridge_sha256)" = "$(h "$d/rbstemsplus-bridge-win-x64.dll")" ]' "1: windows.env's bridge"
check "grep -qxF \"\\\$SigningKeys = '$K'\" \"\$d/bootstrap.ps1\" && grep -qxF \"\\\$PayloadMinimum = '1.4.0'\" \"\$d/bootstrap.ps1\"" "1: bootstrap.ps1's keys and version"
check 'python3 -I -c "import sys; b=open(sys.argv[1],\"rb\").read(); sys.exit(0 if b.isascii() else 1)" "$d/bootstrap.ps1"' "1: bootstrap.ps1 ASCII"
check 'cmp -s "$d/NOTICE-windows.txt" "$root/win/NOTICE.txt"' "1: NOTICE-windows.txt"
check '[ ! -e "$d/windows-release.sha256" ]' "1: a release writes no windows-release.sha256"

# 2. a first Windows release's plan (efdc278's lines, all new): the same
rm -f "$r/calls"; d="$t/d2"; plan "$t/p2" "${allnew[@]}"
"$wa" --version 1.4.0 --keys "$K" --plan "$t/p2" "$d" > "$t/2.log" 2>&1; check '[ $? = 0 ]' "2: all new: failed ($(tail -1 "$t/2.log"))"
check '[ "$(calls)" = 1 ] && [ "$(envv "$d" win_app_version)" = 1.4.0 ]' "2: built, version 1.4.0"

# 3. the zip and the bridge carried: nothing built, the previous bytes and hashes
rm -f "$r/calls"; d="$t/d3"
plan "$t/p3" "windows.app carry 1.3.0 $(h "$P")" "windows.stub.x64 carry $(esha "$P" "$X64")" "windows.stub.arm64 carry $(esha "$P" "$ARM")" \
  "windows.admin carry $(esha "$P" "$ADM")" "windows.bridge carry $(h "$t/prev/rbstemsplus-bridge-win-x64.dll")"
"$wa" --version 1.4.0 --keys "$K" --plan "$t/p3" --carry "$t/prev" "$d" > "$t/3.log" 2>&1; check '[ $? = 0 ]' "3: carry: failed ($(tail -1 "$t/3.log"))"
check '[ "$(calls)" = 0 ]' "3: nothing built"
check 'cmp -s "$d/rbstemsplus-windows.zip" "$P" && cmp -s "$d/rbstemsplus-bridge-win-x64.dll" "$t/prev/rbstemsplus-bridge-win-x64.dll"' "3: the previous zip and bridge byte for byte"
check '[ "$(envv "$d" win_app_version)" = 1.3.0 ] && [ "$(envv "$d" win_admin_sha256)" = "$(esha "$P" "$ADM")" ]' "3: windows.env names the carried version and worker"
check "grep -qxF \"\\\$PayloadMinimum = '1.4.0'\" \"\$d/bootstrap.ps1\"" "3: bootstrap.ps1 is this release's"

# 4. a new zip (other app code) with the x64 stub and the worker carried, the arm64 stub new, the
# bridge new (other bytes)
rm -f "$r/calls"; d="$t/d4"
plan "$t/p4" "windows.app new" "windows.stub.x64 carry $(esha "$P" "$X64")" "windows.stub.arm64 new" "windows.admin carry $(esha "$P" "$ADM")" "windows.bridge new"
FAKE_CODE=code2 FAKE_BRIDGE=b2 "$wa" --version 1.4.0 --keys "$K" --plan "$t/p4" --carry "$t/prev" "$d" > "$t/4.log" 2>&1
check '[ $? = 0 ]' "4: mixed: failed ($(tail -1 "$t/4.log"))"
check '[ "$(calls)" = 1 ] && [ "$(grep -o -- "--carry-file" "$r/calls" | wc -l | tr -d " ")" = 2 ]' "4: one build with two --carry-file"
check '[ "$(esha "$d/rbstemsplus-windows.zip" "$X64")" = "$(esha "$P" "$X64")" ] && [ "$(esha "$d/rbstemsplus-windows.zip" "$ADM")" = "$(esha "$P" "$ADM")" ]' "4: the carried stub and worker are the previous entries"
check '[ "$(esha "$d/rbstemsplus-windows.zip" "$ARM")" != "$(esha "$P" "$ARM")" ] && [ "$(envv "$d" win_stub_arm64_sha256)" = "$(esha "$d/rbstemsplus-windows.zip" "$ARM")" ]' "4: the new arm64 stub, named in windows.env"
check '[ "$(envv "$d" win_app_version)" = 1.4.0 ] && ! cmp -s "$d/rbstemsplus-bridge-win-x64.dll" "$t/prev/rbstemsplus-bridge-win-x64.dll"' "4: version 1.4.0, the new bridge"

# 5. a TEST build carrying from the archived TEST build: the plan names the release's hashes, the
# archive's payload its own; the archive's are checked
mkprev "$t/prevtest" 1.3.0 test
rm -f "$r/calls"; d="$t/d5"
plan "$t/p5" "windows.app carry 1.3.0 $(h "$P")" "windows.stub.x64 carry $(esha "$P" "$X64")" "windows.stub.arm64 carry $(esha "$P" "$ARM")" \
  "windows.admin carry $(esha "$P" "$ADM")" "windows.bridge carry $(h "$t/prev/rbstemsplus-bridge-win-x64.dll")"
"$wa" --version 1.4.0 --keys "$K" --plan "$t/p5" --carry "$t/prevtest" --test https://h:1 --test-key k "$d" > "$t/5.log" 2>&1
check '[ $? = 0 ] && cmp -s "$d/rbstemsplus-windows.zip" "$t/prevtest/rbstemsplus-windows.zip"' "5: a TEST build carries the archived TEST zip ($(tail -1 "$t/5.log"))"
check '[ ! -e "$d/windows-release.sha256" ]' "5: a carried worker: no windows-release.sha256"
# and a TEST build with a new zip passes --test on
rm -f "$r/calls"; "$wa" --version 1.4.0 --keys "$K" --test https://h:1 --test-key k "$t/d5b" > "$t/5b.log" 2>&1
check 'grep -q -- "--test https://h:1 --test-key k" "$r/calls"' "5: the TEST build's server and key passed on"
check '[ "$(cat "$t/d5b/windows-release.sha256")" = "admin $(esha "$t/d1/rbstemsplus-windows.zip" "$ADM")" ]' "5: a TEST build with a new worker names the release worker's hash"
check '[ "$(esha "$t/d5b/rbstemsplus-windows.zip" "$ADM")" != "$(esha "$t/d1/rbstemsplus-windows.zip" "$ADM")" ]' "5: (the TEST zip's worker is the TEST one)"
# a TEST build with a new zip and the archive's TEST worker carried: no windows-release.sha256
rm -f "$r/calls"; d="$t/d5c"
FAKE_CODE=code2 "$wa" --version 1.4.0 --keys "$K" --plan "$t/p4" --carry "$t/prevtest" --test https://h:1 --test-key k "$d" > "$t/5c.log" 2>&1
check '[ $? = 0 ] && [ ! -e "$d/windows-release.sha256" ]' "5: a new zip with a carried worker: no windows-release.sha256 ($(tail -1 "$t/5c.log"))"
check '[ "$(esha "$d/rbstemsplus-windows.zip" "$ADM")" = "$(esha "$t/prevtest/rbstemsplus-windows.zip" "$ADM")" ]' "5: the archive's TEST worker carried"

# 6. refusals, each before anything is built
refused() {   # NAME TEXT CMD...: fails, says TEXT, builds nothing
  local n="$1" want="$2"; shift 2
  rm -f "$r/calls"
  if "$@" > "$t/$n.log" 2>&1; then bad "$n: not refused"; return; fi
  grep -qF -- "$want" "$t/$n.log" && ok || { bad "$n: no '$want' in its output"; tail -2 "$t/$n.log" | sed 's/^/    /'; }
  [ "$(calls)" = 0 ] && ok || bad "$n: built something"
}
plan "$t/p6" "windows.app carry 1.3.0 $(printf '%064d' 0)" "windows.stub.x64 carry $(esha "$P" "$X64")" "windows.stub.arm64 carry $(esha "$P" "$ARM")" \
  "windows.admin carry $(esha "$P" "$ADM")" "windows.bridge new"
refused 6a "isn't the plan's" "$wa" --version 1.4.0 --keys "$K" --plan "$t/p6" --carry "$t/prev" "$t/d6"
plan "$t/p7" "windows.app carry 1.3.0 $(h "$P")" "windows.stub.x64 new" "windows.stub.arm64 carry $(esha "$P" "$ARM")" "windows.admin carry $(esha "$P" "$ADM")" "windows.bridge new"
refused 6b "a carried zip carries its stubs and worker" "$wa" --version 1.4.0 --keys "$K" --plan "$t/p7" --carry "$t/prev" "$t/d6"
plan "$t/p8" "windows.app new" "windows.stub.x64 new" "windows.admin new" "windows.bridge new"
refused 6c "has no windows.stub.arm64 line" "$wa" --version 1.4.0 --keys "$K" --plan "$t/p8" "$t/d6"
plan "$t/p9" "windows.app new" "windows.stub.x64 carry $(printf '%064d' 1)" "windows.stub.arm64 new" "windows.admin new" "windows.bridge new"
refused 6d "isn't the plan's" "$wa" --version 1.4.0 --keys "$K" --plan "$t/p9" --carry "$t/prev" "$t/d6"
refused 6e "--carry DIR" "$wa" --version 1.4.0 --keys "$K" --plan "$t/p4" "$t/d6"
cp -R "$t/prev" "$t/prevbad"; echo x >> "$t/prevbad/rbstemsplus-bridge-win-x64.dll"
refused 6f "doesn't have the SHA-256" "$wa" --version 1.4.0 --keys "$K" --plan "$t/p3" --carry "$t/prevbad" "$t/d6"
refused 6g "--keys" "$wa" --version 1.4.0 --keys "not base64!" "$t/d6"
# a changed previous zip (not what its payload names)
cp -R "$t/prev" "$t/prevzip"; printf 'x' >> "$t/prevzip/rbstemsplus-windows.zip"
refused 6h "rbstemsplus-windows.zip isn't the SHA-256 its payload.json names" "$wa" --version 1.4.0 --keys "$K" --plan "$t/p3" --carry "$t/prevzip" "$t/d6"
# a payload whose worker hash isn't the zip's entry
cp -R "$t/prev" "$t/prevent"
python3 -I -c 'import json, sys; p = sys.argv[1]; j = json.load(open(p)); j["windows"]["app"]["admin_sha256"] = "0" * 64; json.dump(j, open(p, "w"))' "$t/prevent/payload.json"
refused 6i "'s admin/RB Stems Plus.exe isn't the SHA-256 its payload.json names" "$wa" --version 1.4.0 --keys "$K" --plan "$t/p3" --carry "$t/prevent" "$t/d6"
# the plan's app version isn't the carried payload's: a release, and a TEST build's archive
plan "$t/p11" "windows.app carry 1.2.0 $(h "$P")" "windows.stub.x64 carry $(esha "$P" "$X64")" "windows.stub.arm64 carry $(esha "$P" "$ARM")" \
  "windows.admin carry $(esha "$P" "$ADM")" "windows.bridge new"
refused 6j "Windows app is 1.3.0, the plan's 1.2.0" "$wa" --version 1.4.0 --keys "$K" --plan "$t/p11" --carry "$t/prev" "$t/d6"
refused 6k "Windows app is 1.3.0, the plan's 1.2.0" "$wa" --version 1.4.0 --keys "$K" --plan "$t/p11" --carry "$t/prevtest" --test https://h:1 --test-key k "$t/d6"
# a plan's bridge that isn't the carried payload's
plan "$t/p12" "windows.app new" "windows.stub.x64 new" "windows.stub.arm64 new" "windows.admin new" "windows.bridge carry $(printf '%064d' 3)"
refused 6l "Windows bridge isn't the plan's" "$wa" --version 1.4.0 --keys "$K" --plan "$t/p12" --carry "$t/prev" "$t/d6"
# BP2: a TEST archive whose stubs and bridge aren't the plan's (the release's): everything carried
# (T1), a carried bridge alone, a new zip with the x64 stub carried (T2)
FAKE_CODE=code9 FAKE_BRIDGE=b9 mkprev "$t/prevtest9" 1.3.0 test
refused 6m "stub/x64/RB Stems Plus.exe isn't the plan's" "$wa" --version 1.4.0 --keys "$K" --plan "$t/p5" --carry "$t/prevtest9" --test https://h:1 --test-key k "$t/d6"
FAKE_BRIDGE=b9 mkprev "$t/prevtestb" 1.3.0 test
refused 6n "Windows bridge isn't the plan's" "$wa" --version 1.4.0 --keys "$K" --plan "$t/p5" --carry "$t/prevtestb" --test https://h:1 --test-key k "$t/d6"
plan "$t/p13" "windows.app new" "windows.stub.x64 carry $(esha "$P" "$X64")" "windows.stub.arm64 new" "windows.admin new" "windows.bridge new"
refused 6o "stub/x64/RB Stems Plus.exe isn't the plan's" "$wa" --version 1.4.0 --keys "$K" --plan "$t/p13" --carry "$t/prevtest9" --test https://h:1 --test-key k "$t/d6"
# BP3: a previous zip with a name that collides with an entry but for case
cp -R "$t/prev" "$t/prevcase"
python3 -I -c 'import sys, zipfile; z = zipfile.ZipFile(sys.argv[1], "a"); z.writestr("Admin/RB Stems Plus.exe", b"other"); z.close()' "$t/prevcase/rbstemsplus-windows.zip"
reseal "$t/prevcase" 1.3.0
plan "$t/p14" "windows.app carry 1.3.0 $(h "$t/prevcase/rbstemsplus-windows.zip")" "windows.stub.x64 carry $(esha "$P" "$X64")" "windows.stub.arm64 carry $(esha "$P" "$ARM")" \
  "windows.admin carry $(esha "$P" "$ADM")" "windows.bridge new"
refused 6p "duplicate or case-colliding entry names" "$wa" --version 1.4.0 --keys "$K" --plan "$t/p14" --carry "$t/prevcase" "$t/d6"
for v in "q:Admin/RB Stems Plus.exe/" "r:ADMIN"; do
  rm -rf "$t/prevcase"; cp -R "$t/prev" "$t/prevcase"
  python3 -I -c 'import sys, zipfile; z = zipfile.ZipFile(sys.argv[1], "a"); z.writestr(sys.argv[2], b""); z.close()' "$t/prevcase/rbstemsplus-windows.zip" "${v#*:}"
  reseal "$t/prevcase" 1.3.0
  plan "$t/p14" "windows.app carry 1.3.0 $(h "$t/prevcase/rbstemsplus-windows.zip")" "windows.stub.x64 carry $(esha "$P" "$X64")" "windows.stub.arm64 carry $(esha "$P" "$ARM")" \
    "windows.admin carry $(esha "$P" "$ADM")" "windows.bridge new"
  refused "6${v%%:*}" "duplicate or case-colliding entry names" "$wa" --version 1.4.0 --keys "$K" --plan "$t/p14" --carry "$t/prevcase" "$t/d6"
done

# 7. refusals after the build: each fails and says so
failed() {   # NAME TEXT CMD...: fails and says TEXT
  local n="$1" want="$2"; shift 2
  rm -f "$r/calls"
  if "$@" > "$t/$n.log" 2>&1; then bad "$n: not refused"; return; fi
  grep -qF -- "$want" "$t/$n.log" && ok || { bad "$n: no '$want' in its output"; tail -2 "$t/$n.log" | sed 's/^/    /'; }
}
FAKE_CODE=code2 FAKE_IGNORE_CARRY=1 failed 7a "isn't the carried one" "$wa" --version 1.4.0 --keys "$K" --plan "$t/p4" --carry "$t/prev" "$t/d7"
FAKE_DROP=arm64 failed 7b "the shipped files have no win_stub_arm64_sha256" "$wa" --version 1.4.0 --keys "$K" "$t/d7"
FAKE_NO_RELREF=1 failed 7c "made no windows-release.sha256" "$wa" --version 1.4.0 --keys "$K" --test https://h:1 --test-key k "$t/d7"
FAKE_RELREF_EXTRA=1 failed 7e "made no windows-release.sha256" "$wa" --version 1.4.0 --keys "$K" --test https://h:1 --test-key k "$t/d7"
cp "$r/scripts/bootstrap.ps1" "$t/bootstrap.ps1.orig"; printf '# caf\xc3\xa9\n' >> "$r/scripts/bootstrap.ps1"
failed 7d "isn't pure ASCII" "$wa" --version 1.4.0 --keys "$K" "$t/d7"
cp "$t/bootstrap.ps1.orig" "$r/scripts/bootstrap.ps1"

echo "windows-assets: $pass passed, $fail failed"
[ "$fail" = 0 ]

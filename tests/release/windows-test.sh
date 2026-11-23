#!/bin/bash
# Tests scripts/release.sh's Windows side (release.md T13; §1.2, §1.3, §6) in the sandbox of
# tests/release/sandbox.sh (fake repositories, a fake gh, a throwaway key, fake builds; nothing
# real is signed or uploaded, no network): a Mac-only release has no Windows asset and no windows
# block; --windows makes the first release with them (the four Windows assets, the windows
# block); from then on a release has them by itself, and refuses without scripts/build-windows.sh;
# the Windows carry-forward by SHA-256 (the zip built again stamped with the previous
# windows.app.version and compared file by file; the stubs and the worker carried one by one
# into a new zip, R4.I83; the bridge by its bytes); step 4b's VM record (vm-win.ok) when a
# Windows file is new or the TEST payload's lists changed (R4.I94), refused without it; step 6's
# checks of the windows block; a previous zip that isn't what its payload names; the plan's
# decisions against the real run's.
#
# Fakes added here: scripts/build-windows.sh (a deterministic zip from win/'s sources, --stamp,
# --carry-file), a scripts/build.sh that also assembles the Windows files in a plan's shape and
# makes the TEST build (--test URL), with FAKE_BUILD_FAULT's wrong builds for step 6 to refuse
# (review TT3: nocarryfile, stubsha, emptysha, noentry, oldstubs, lists, winblock-when-none,
# stale-ps1; review TR: retest, retest-ws, dupentry, oldstubs-arm64, spoofname; review BR1:
# casename, dirslash, fileasfolder; review RV1: vmfail, the VM records failed during the real
# build; review BS3: vmchange, the row's record another passing run's after the real build).
# FAKE_PS1_KEY is a key bootstrap.ps1 trusts (review TK5). Also faked: tests/win/vm-run.sh's check in
# VR1's layout (review TT6: every row of
# tests/win/suites/release.list as committed in COMMIT has exactly one
# build/gates/COMMIT/vm-win/SUITE@ID-ARCH.ok for COMMIT and the TEST payload, no .failed, the
# records in the real one's lines; FAKE_VM_OK: an exit 0 with a vm-win.ok that isn't this
# commit's or payload's, or none; the real one is tested in tests/win/vm-run-test.sh). Case 13:
# a row whose suite the commit doesn't have fails 4b in the fake too. Case 14 runs step 4b on
# the real check with synthetic VM, PC and VR2 records, and exercises step 6's second check.
#
#   tests/release/windows-test.sh [RELEASE_SH]    run outside a sandbox: a pseudo-terminal
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
rel="$(cd "$(dirname "${1:-$root/scripts/release.sh}")" && pwd)/$(basename "${1:-$root/scripts/release.sh}")"
[ -f "$rel" ] || { echo "windows-test: no release.sh at $rel" >&2; exit 2; }
. "$here/sandbox.sh"

push() { g -C "$dev" push -q origin main 2> /dev/null; }
commit_all() { g -C "$dev" add -A && g -C "$dev" commit -q -m "$1" && push; }
logged() { if grep -qF -- "$2" "$t/$1.log"; then ok; else bad "$1: no '$2' in its output"; tail -5 "$t/$1.log" | sed 's/^/    /'; fi; }
payload() { local got; got="$(pjson "$FAKE_GH/releases/$1/payload.json" "$2")"; [ "$got" = "$3" ] && ok || bad "$1: payload.json's $2 is $got, not $3"; }
rsha() { shasum -a 256 "$FAKE_GH/releases/$1/$2" | cut -d' ' -f1; }
# the SHA-256 of an entry of a release's Windows zip
esha() { python3 -I -c 'import hashlib, sys, zipfile; print(hashlib.sha256(zipfile.ZipFile(sys.argv[1]).read(sys.argv[2])).hexdigest())' "$FAKE_GH/releases/$1/rbstemsplus-windows.zip" "$2"; }

# ---------------------------------------------------------------- the Windows fakes
# the extended fake build: the sandbox's, plus the Windows files in the plan's shape and --test
cat > "$dev/scripts/build.sh" <<'BUILD'
#!/bin/bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
v="$(tr -d '[:space:]' < "$root/VERSION")"
c="$(git -C "$root" rev-parse HEAD)"
plan=""; carry=""; d="$root/dist"
while [ $# -gt 0 ]; do case "$1" in --plan) plan="$2"; shift 2 ;; --carry) carry="$2"; shift 2 ;; --test) d="$root/dist-test"; shift 2 ;; *) exit 2 ;; esac; done
mkdir -p "$d"
prev="$(sed -n 's/^previous \(v[^ ]*\) .*/\1/p' "$plan")"
[ -n "$carry" ] || carry="$FAKE_GH/releases/$prev"   # a TEST build: the previous release's files
read -r _ app_kind app_v app_c _ < <(grep '^mac\.app ' "$plan")
read -r _ br_kind br_c _ < <(grep '^mac\.bridge ' "$plan")
if [ "$app_kind" = carry ]; then cp "$carry/rbstemsplus-app.zip" "$d/"
else "$root/scripts/build-app.sh" "$d"; app_v="$v"; app_c="$c"; fi
if [ "$br_kind" = carry ]; then cp "$carry/libonnxruntime.1.18.0.dylib" "$d/"
else "$root/bridge/build.sh" "$root/build/bridge"; cp "$root/build/bridge/libonnxruntime.1.18.0.dylib" "$d/"; br_c="$c"; fi
for f in stemsplus-model.onnx bootstrap.sh NOTICE LICENSE; do echo "$f $v" > "$d/$f"; done
rm -f "$d/rbstemsplus-windows.zip" "$d/rbstemsplus-bridge-win-x64.dll" "$d/bootstrap.ps1" "$d/NOTICE-windows.txt" "$d/windows-release.sha256"
win=""; extra=""; fault="${FAKE_BUILD_FAULT:-}"
read -r _ w_kind w_v _ < <(grep '^windows\.app ' "$plan")
if [ "$w_kind" = none ]; then
  # wrong builds: a windows block, or a Windows asset left from an earlier build
  [ "$fault" != winblock-when-none ] || win=', "windows": {"app": {"version": "0.0.0"}}'
  [ "$fault" != stale-ps1 ] || echo "bootstrap.ps1 old" > "$d/bootstrap.ps1"
else
  b="$root/build/win-out"; rm -rf "$b"; mkdir -p "$b/carried"
  args=()
  for k in stub.x64:"stub/x64/RB Stems Plus.exe" stub.arm64:"stub/arm64/RB Stems Plus.exe" admin:"admin/RB Stems Plus.exe"; do
    how="$(sed -n "s/^windows\.${k%%:*} \([a-z]*\).*/\1/p" "$plan")"
    # oldstubs: the previous stubs under a new decision; nocarryfile: --carry-file left out
    if [ "$fault" = oldstubs ] && [ "${k%%:*}" != admin ]; then how=carry; fi
    if [ "$fault" = oldstubs-arm64 ] && [ "${k%%:*}" = stub.arm64 ]; then how=carry; fi
    if [ "$how" = carry ] && [ "$w_kind" = new ] && [ "$fault" != nocarryfile ]; then
      python3 -I -c 'import sys, zipfile; open(sys.argv[3], "wb").write(zipfile.ZipFile(sys.argv[1]).read(sys.argv[2]))' \
        "$carry/rbstemsplus-windows.zip" "${k#*:}" "$b/carried/${k%%:*}"
      args+=(--carry-file "${k#*:}=$b/carried/${k%%:*}")
    fi
  done
  # nocarryfile: the stubs built again by a toolchain that gives other bytes
  # admindrift: the release build's worker other bytes than the TEST build's reference
  FAKE_ADMIN_DRIFT="$([ "$fault" != admindrift ] || [ "$d" != "$root/dist" ] || echo rebuilt)" \
  FAKE_STUB_DRIFT="$([ "$fault" != nocarryfile ] || echo rebuilt)" "$root/scripts/build-windows.sh" ${args[@]+"${args[@]}"} "$b"
  # a TEST build with a new worker: the release worker's hash, as windows-assets.sh writes it
  if [ "$d" = "$root/dist-test" ] && grep -q '^windows\.admin new' "$plan"; then
    echo "admin $(python3 -I -c 'import hashlib, sys, zipfile; print(hashlib.sha256(zipfile.ZipFile(sys.argv[1]).read("admin/RB Stems Plus.exe")).hexdigest())' "$b/rbstemsplus-windows.zip")" > "$d/windows-release.sha256"
  fi
  # relchange: the reference rewritten during the real build
  if [ "$d" = "$root/dist" ] && [ "$fault" = relchange ]; then echo "admin $(printf '%064d' 0)" > "$FAKE_DEV/dist-test/windows-release.sha256"; fi
  # relsymlink: the reference replaced during the real build by a symlink to the same bytes
  if [ "$d" = "$root/dist" ] && [ "$fault" = relsymlink ]; then mv "$FAKE_DEV/dist-test/windows-release.sha256" "$FAKE_DEV/dist-test/rel.saved"; ln -s rel.saved "$FAKE_DEV/dist-test/windows-release.sha256"; fi
  # vmfail: a --record rerun that fails during the real build turns the passing records into .failed
  if [ "$d" = "$root/dist" ] && [ "$fault" = vmfail ]; then
    for f in "$FAKE_DEV/build/gates/$(git -C "$FAKE_DEV" rev-parse HEAD)/vm-win/"*.ok; do mv "$f" "${f%.ok}.failed"; done
  fi
  # vmchange: a passing --record rerun during the real build: the row's record is now another run's
  if [ "$d" = "$root/dist" ] && [ "$fault" = vmchange ]; then
    g="$FAKE_DEV/build/gates/$(git -C "$FAKE_DEV" rev-parse HEAD)/vm-win"; mv "$g/one@1111-aaaa-arm64.ok" "$g/one@2222-bbbb-arm64.ok"
  fi
  if [ "$d" = "$root/dist" ]; then
    g="$FAKE_DEV/build/gates/$(git -C "$FAKE_DEV" rev-parse HEAD)"
    case "$fault" in
      record-must-stop) exit 37 ;;
      pcchange) sed -i '' 's/^date .*/date 2026-11-13T00:00:01Z/' "$g/pc-win.ok" ;;
      handchange) sed -i '' 's/^date .*/date 2026-11-13T00:00:01Z/' "$g/win-by-hand.ok" ;;
    esac
  fi
  if [ "$w_kind" = carry ]; then cp "$carry/rbstemsplus-windows.zip" "$d/"; else cp "$b/rbstemsplus-windows.zip" "$d/"; w_v="$v"; fi
  if [ "$fault" = noentry ]; then   # the arm64 stub left out of the zip
    python3 -I -c 'import sys, zipfile
z = zipfile.ZipFile(sys.argv[1]); keep = [(i, z.read(i.filename)) for i in z.infolist() if i.filename != "stub/arm64/RB Stems Plus.exe"]; z.close()
with zipfile.ZipFile(sys.argv[1], "w") as o:
    for i, b in keep: o.writestr(i, b)' "$d/rbstemsplus-windows.zip"
  fi
  if [ "$fault" = dupentry ] || [ "$fault" = spoofname ] || [ "$fault" = casename ] || [ "$fault" = dirslash ] || [ "$fault" = fileasfolder ]; then
    # dupentry: a second x64 stub of other bytes before the real one (the last copy and the payload
    # still agree); spoofname: the arm64 entry renamed so its name holds a forged row; casename,
    # dirslash, fileasfolder: one more entry the updater takes for one of ours (BR1: Stub/X64/...
    # in other case, admin/RB Stems Plus.exe/ as a folder, a file ADMIN that is admin/'s folder)
    python3 -I - "$d/rbstemsplus-windows.zip" "$fault" <<'PY'
import hashlib, sys, warnings, zipfile
warnings.simplefilter("ignore")
p, fault = sys.argv[1:3]
z = zipfile.ZipFile(p); items = [(i, z.read(i.filename)) for i in z.infolist()]; z.close()
with zipfile.ZipFile(p, "w") as o:
    for i, b in items:
        if fault == "dupentry" and i.filename == "stub/x64/RB Stems Plus.exe":
            o.writestr(zipfile.ZipInfo(i.filename, (1980, 1, 1, 0, 0, 0)), b"another x64 stub")
        if fault == "spoofname" and i.filename == "stub/arm64/RB Stems Plus.exe":
            i = zipfile.ZipInfo("spoof\n" + hashlib.sha256(b).hexdigest() + " stub/arm64/RB Stems Plus.exe", (1980, 1, 1, 0, 0, 0))
        o.writestr(i, b)
    extra = {"casename": "Stub/X64/RB Stems Plus.exe", "dirslash": "admin/RB Stems Plus.exe/", "fileasfolder": "ADMIN"}.get(fault)
    if extra:
        o.writestr(zipfile.ZipInfo(extra, (1980, 1, 1, 0, 0, 0)), b"" if extra.endswith("/") else b"other")
PY
  fi
  if grep -q '^windows\.bridge carry' "$plan"; then cp "$carry/rbstemsplus-bridge-win-x64.dll" "$d/"; else cp "$b/rbstemsplus-bridge-win-x64.dll" "$d/"; fi
  for f in bootstrap.ps1 NOTICE-windows.txt; do echo "$f $v" > "$d/$f"; done
  [ -z "${FAKE_PS1_KEY:-}" ] || echo "trusted key $FAKE_PS1_KEY" >> "$d/bootstrap.ps1"   # as a test build's
  win="$(python3 -I - "$d" "$w_v" "$root/win/compat.txt" "$fault" <<'PY'
import hashlib, json, sys, zipfile
d, v, compat, fault = sys.argv[1:5]
z = zipfile.ZipFile(d + "/rbstemsplus-windows.zip")
h = lambda b: hashlib.sha256(b).hexdigest()
eh = lambda n: h(z.read(n)) if n in z.namelist() else ""
x64 = {"stubsha": h(b"another stub"), "emptysha": ""}.get(fault, eh("stub/x64/RB Stems Plus.exe"))
arm = eh("stub/arm64/RB Stems Plus.exe")
if fault == "spoofname":   # the hash in the forged row, as an old reader would have taken it
    arm = next(n.split()[-4] for n in z.namelist() if n.startswith("spoof"))
rb = open(compat).read().split() + (["7.2.99"] if fault in ("lists", "retest") else [])
print(json.dumps({"app": {"version": v, "zip": "rbstemsplus-windows.zip", "sha256": h(open(d + "/rbstemsplus-windows.zip", "rb").read()),
                          "admin_sha256": eh("admin/RB Stems Plus.exe"),
                          "stub_sha256": {"x64": x64, "arm64": arm}},
                  "bridge": {"file": "rbstemsplus-bridge-win-x64.dll", "sha256": h(open(d + "/rbstemsplus-bridge-win-x64.dll", "rb").read())},
                  "compatible": {"rekordbox": rb, "ort_sha256": [], "stems_engine": ["0002"]},
                  "resampler_proved": []}))
PY
)"
  win=", \"windows\": $win"
  extra="rbstemsplus-windows.zip rbstemsplus-bridge-win-x64.dll bootstrap.ps1 NOTICE-windows.txt"
fi
s() { shasum -a 256 "$d/$1" | cut -d' ' -f1; }
# retest / retest-ws: during the real build, dist-test/payload.json changes after step 4b read it
# (7.2.99 in its lists too, or the same JSON rewritten with other whitespace)
if [ "$d" = "$root/dist" ] && { [ "$fault" = retest ] || [ "$fault" = retest-ws ]; }; then
  python3 -I - "$FAKE_DEV/dist-test/payload.json" "$fault" <<'PY'
import json, sys
p, fault = sys.argv[1:3]
j = json.load(open(p))
if fault == "retest":
    j["windows"]["compatible"]["rekordbox"].append("7.2.99")
open(p, "w").write(json.dumps(j, indent=None if fault == "retest" else 2))
PY
fi
cat > "$d/payload.json" <<EOF
{"payload_version": "$v", "commit": "$c",
 "app": {"version": "$app_v", "commit": "$app_c", "zip": "rbstemsplus-app.zip", "sha256": "$(s rbstemsplus-app.zip)"},
 "bridge": {"file": "libonnxruntime.1.18.0.dylib", "commit": "$br_c", "sha256": "$(s libonnxruntime.1.18.0.dylib)"}$win}
EOF
# shellcheck disable=SC2086
(cd "$d" && shasum -a 256 rbstemsplus-app.zip libonnxruntime.1.18.0.dylib stemsplus-model.onnx bootstrap.sh NOTICE LICENSE payload.json $extra > SHA256SUMS)
BUILD
# the fake Windows build: a deterministic zip of win/'s sources (the app stamped --stamp or
# VERSION; FAKE_WIN_DRIFT changes its bytes as a new toolchain would), the stubs and the worker
# (each replaced by --carry-file NAME=PATH), and the bridge (FAKE_WBRIDGE_DRIFT). It takes the
# real one's options (--test URL --test-key PEM, --check-repro) and uses its download caches as
# the real builds do: each checked against its pin, "downloaded" when missing or not the pinned
# one, each use logged to FAKE_WIN_CACHE_LOG ("NAME cached|downloaded ROOT")
cat > "$dev/scripts/build-windows.sh" <<'BUILD'
#!/bin/bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
stamp="$(tr -d '[:space:]' < "$root/VERSION")"; carries=(); turl=""; tkey=""
while [ $# -gt 1 ]; do case "$1" in --stamp) stamp="$2"; shift 2 ;; --carry-file) carries+=("$2"); shift 2 ;;
  --test) turl="$2"; shift 2 ;; --test-key) tkey="$2"; shift 2 ;; --check-repro) shift ;; *) exit 2 ;; esac; done
[ -z "$turl$tkey" ] || { [ -n "$turl" ] && [ -f "$tkey" ]; } || exit 2
out="$1"; mkdir -p "$out"
for c in dl nuget pocketfft blocksrt flac; do
  if [ "$(cat "$root/build/$c/pin" 2>/dev/null)" = "pinned $c" ]; then how=cached
  else how=downloaded; echo "downloading $c" >&2; mkdir -p "$root/build/$c"; echo "pinned $c" > "$root/build/$c/pin"; fi
  [ -z "${FAKE_WIN_CACHE_LOG:-}" ] || echo "$c $how $root" >> "$FAKE_WIN_CACHE_LOG"
done
python3 -I - "$root" "$stamp" "$out" "${FAKE_WIN_DRIFT:-}" "${FAKE_STUB_DRIFT:-}" "${FAKE_ADMIN_DRIFT:-}" ${carries[@]+"${carries[@]}"} <<'PY'
import glob, sys, zipfile
root, stamp, out, drift, sdrift, adrift = sys.argv[1:7]
cat = lambda pat: b"".join(open(f, "rb").read() for f in sorted(glob.glob(root + "/" + pat)))
files = {"RB Stems Plus.exe": cat("win/shell/*") + b"version " + stamp.encode() + drift.encode(),
         "stub/x64/RB Stems Plus.exe": cat("win/c/stub*") + b"x64" + sdrift.encode(),
         "stub/arm64/RB Stems Plus.exe": cat("win/c/stub*") + b"arm64" + sdrift.encode(),
         "admin/RB Stems Plus.exe": cat("win/admin/*") + adrift.encode(), "NOTICE-windows.txt": cat("win/NOTICE.txt"),
         "app/rbsp-prepare.exe": b"the Prepare helper"}
for c in sys.argv[7:]:
    name, path = c.split("=", 1)
    files[name] = open(path, "rb").read()
with zipfile.ZipFile(out + "/rbstemsplus-windows.zip", "w") as z:
    for n in sorted(files):
        i = zipfile.ZipInfo(n, (1980, 1, 1, 0, 0, 0)); i.external_attr = 0o644 << 16
        z.writestr(i, files[n])
PY
{ cat "$root"/win/bridge/*; echo "${FAKE_WBRIDGE_DRIFT:-}"; } > "$out/rbstemsplus-bridge-win-x64.dll"
BUILD
# the VM record's check, as tests/win/vm-run.sh check does it (VR1): each row of release.list as
# committed in COMMIT ("SUITE ARCH {ID}") needs exactly one passing record for COMMIT and the TEST
# payload, and no failed one (an interrupted run's "result incomplete" .failed too), and a row
# whose suite COMMIT doesn't have fails (the real check: no record, or COMMIT has no suite); a record
# passes when its suite, arch and stage_commit are the row's and COMMIT's, update's candidate is
# served as COMMIT's release ("served dist-test/new/payload.json SHA COMMIT", its need candidate)
# and platform's vm/ was built from COMMIT (vm_commit, its need vm), it holds exactly one result
# line, and its file lines are exactly the suite's set (VT2); then vm-win.ok is written.
# FAKE_VM_OK=commit|payload|none: the check still exits 0, but leaves a vm-win.ok for another
# commit or payload, or none. A live pid in the share's lock (build/win/.vm-run.lock/pid): exit 2
mkdir -p "$dev/tests/win/suites"
printf '%s\n' '# fake release.list' 'bootstrap  arm64  {1111-aaaa}' 'uninstall  arm64  {1111-aaaa}' 'update     arm64  {1111-aaaa}' \
  'platform   arm64  {1111-aaaa}' > "$dev/tests/win/suites/release.list"
for s in bootstrap uninstall update platform; do echo "sys  60  $s  s/$s.ps1" > "$dev/tests/win/suites/$s.suite"; done
cat > "$dev/tests/win/vm-run.sh" <<'VM'
#!/bin/bash
[ "${1:-}" = check ] || exit 2
root="$(cd "$(dirname "$0")/../.." && pwd)"; c="$2"; dir="${3:-$root/dist-test}"; d="$root/build/gates/$c/vm-win"
# the share's lock (VT1): held by a live pid, the check doesn't run, exit 2
p="$(cat "${VMRUN_SHARE:-$root/build/win}/.vm-run.lock/pid" 2> /dev/null)"
if [[ "$p" =~ ^[1-9][0-9]*$ ]] && perl -e 'exit(kill(0, $ARGV[0]) || $!{EPERM} ? 0 : 1)' "$p"; then echo "vm-run.sh: another vm-run.sh (pid $p) drives the VM" >&2; exit 2; fi
rm -f "$root/build/gates/$c/vm-win.ok"
ps="$(shasum -a 256 "$dir/payload.json" | cut -c1-64)"; n=0; bad=0
list="$(git -C "$root" show "$c:tests/win/suites/release.list")" || { echo "vm-run.sh check: $c has no release.list"; exit 1; }
while read -r suite arch snap; do
  case "$suite" in ''|'#'*) continue ;; esac
  n=$((n + 1)); id="${snap#\{}"; id="${id%\}}"
  git -C "$root" cat-file -e "$c:tests/win/suites/$suite.suite" 2> /dev/null \
    || { echo "vm-run.sh check: $c has no tests/win/suites/$suite.suite (release.list's row $suite)"; bad=1; continue; }
  [ ! -e "$d/$suite@$id-$arch.failed" ] || { echo "vm-run.sh check: $suite on \"$snap\" ($arch) failed: $suite@$id-$arch.failed"; bad=1; continue; }
  r="$d/$suite@$id-$arch.ok"
  [ -f "$r" ] || { echo "vm-run.sh check: no record of $suite on \"$snap\" ($arch) in $d"; bad=1; continue; }
  grep -qx "commit $c" "$r" && grep -qx "suite $suite" "$r" && grep -qx "arch $arch" "$r" && grep -qx "payload_sha256 $ps" "$r" \
    && grep -qx "stage_commit $c" "$r" && [ "$(grep -c '^result ' "$r")" = 1 ] && grep -qx "result pass" "$r" \
    || { echo "vm-run.sh check: $(basename "$r") isn't one pass of $suite on $arch for $c and this TEST payload"; bad=1; continue; }
  # VT2: its files are exactly what COMMIT's suite runs (the suite, its includes, each step's
  # script) and vm-run.sh, each named once
  want="$({ echo "tests/win/suites/$suite.suite"; echo tests/win/vm-run.sh
            git -C "$root" show "$c:tests/win/suites/$suite.suite" | sed 's/#.*//' \
              | awk '$1 == "include" && NF == 2 { print "tests/win/suites/" $2 } $1 ~ /^(sys|user|user-high|user-signed)$/ && NF == 4 { print "tests/win/suites/" $4 }'; } \
          | LC_ALL=C sort -u)"
  got="$(sed -n 's/^file \([^ ]*\) .*/\1/p' "$r" | LC_ALL=C sort)"
  [ "$got" = "$want" ] || { echo "vm-run.sh check: $(basename "$r"): its files aren't exactly $c's suite's, each once"; bad=1; continue; }
  [ "$suite" != update ] || grep -qE "^served dist-test/new/payload.json [0-9a-f]{64} $c\$" "$r" \
    || { echo "vm-run.sh check: $(basename "$r"): its dist-test/new/payload.json isn't $c's release"; bad=1; continue; }
  [ "$suite" != platform ] || grep -qx "vm_commit $c" "$r" \
    || { echo "vm-run.sh check: $(basename "$r"): its vm/ wasn't built from $c"; bad=1; continue; }
done <<< "$list"
[ "$n" -gt 0 ] && [ "$bad" = 0 ] || exit 1
o="$root/build/gates/$c/vm-win.ok"
case "${FAKE_VM_OK:-}" in
  commit) { echo "commit 0000000000000000000000000000000000000000"; echo "payload_sha256 $ps"; echo "result pass"; } > "$o" ;;
  payload) { echo "commit $c"; echo "payload_sha256 $(printf '0%.0s' $(seq 64))"; echo "result pass"; } > "$o" ;;
  none) ;;
  *) { echo "commit $c"; echo "payload_sha256 $ps"; echo "result pass"; } > "$o" ;;
esac
echo "vm-run.sh check: $c passed ($n rows of release.list)"
VM
chmod 755 "$dev/scripts/build.sh" "$dev/scripts/build-windows.sh" "$dev/tests/win/vm-run.sh"
cp "$root/tests/win/gate-record.py" "$dev/tests/win/"
mkdir -p "$dev/win/shell" "$dev/win/c" "$dev/win/admin" "$dev/win/bridge"
echo "class App {}" > "$dev/win/shell/App.cs"
echo "int stub;" > "$dev/win/c/stub.c"
echo "int admin;" > "$dev/win/admin/admin.c"
echo "int bridge_win;" > "$dev/win/bridge/bridge_win.c"
echo "the Windows libraries" > "$dev/win/NOTICE.txt"
echo "7.2.19" > "$dev/win/compat.txt"
commit_all "the Windows fakes"
export FAKE_DEV="$dev"

# gates NAME [vm]: after NAME's plan, the TEST build in its shape, a passing G3 record and passing
# performance records for it and, with vm, passing VM and VR2 records
gates() {
  local c r
  c="$(g -C "$dev" rev-parse HEAD)"; r="$dev/build/gates/$c"
  (cd "$dev" && scripts/build.sh --test http://127.0.0.1:1/ --plan "$r/plan") > "$t/$1-test.log" 2>&1 \
    || { bad "$1: the TEST build failed"; tail -5 "$t/$1-test.log" | sed 's/^/    /'; }
  local ps; ps="$(shasum -a 256 "$dev/dist-test/payload.json" | cut -c1-64)"
  rm -rf "$r/g3-mac."* "$r/vm-win."* "$r/vm-win"
  { echo "g3 1"; echo "commit $c"; echo "payload_sha256 $ps"
    echo "plan_sha256 $(shasum -a 256 "$r/plan" | cut -c1-64)"; echo "guests rb26-template"; echo "guest_versions rb26-template=26.6.2"
    (cd "$dev" && tests/mac/g3.sh _t required "$c" "rb26-template") \
      | while read -r gg nn; do echo "scenario $gg $nn pass 1min clicks=none"; done
    echo "coverage ok"; echo "result pass"; } > "$r/g3-mac.ok"
  test_signed; perf_records win
  if [ "${2:-}" = vm ]; then   # one passing record per row of release.list (VR1's layout)
    hand_record "$c"
    mkdir -p "$r/vm-win"
    for s in bootstrap uninstall update platform; do
      { rec_head "$s" "$c" "$ps"; echo "step user install PASS 0 10s"; echo "result pass"; } > "$r/vm-win/$s@1111-aaaa-arm64.ok"
    done
  fi
}
hand_record() {
  python3 -I "$dev/tests/win/gate-record.py" write-by-hand "$1" "$dev/dist-test" --pass > /dev/null \
    || bad "could not write the VR2 fixture for $1"
}
# rec_head SUITE COMMIT PAYLOAD-SHA: a record's first lines, as tests/win/vm-run.sh's rec_head
# writes them (update's served candidate, platform's vm/ built from COMMIT)
rec_head() {
  echo "commit $2"; echo "suite $1"; echo "snapshot {1111-aaaa} clean Windows"; echo "vm_uuid 202a80c8-4bc4-4e95-890e-a31d6bfd5b70"; echo "arch arm64"
  echo "payload_sha256 $3"; echo "stage_commit $2"; echo "vm_commit $2"
  echo "repo_sha256 $(printf '0%.0s' $(seq 64))"; echo "run r1"; echo "date 2026-11-13T00:00:00Z"
  # the suite, its step's script and vm-run.sh, each once (VT2; the fake suites are one sys step)
  echo "file tests/win/suites/$1.suite $(printf '1%.0s' $(seq 64))"
  echo "file tests/win/suites/s/$1.ps1 $(printf '3%.0s' $(seq 64))"
  echo "file tests/win/vm-run.sh $(printf '4%.0s' $(seq 64))"
  [ "$1" != update ] || echo "served dist-test/new/payload.json $(printf '2%.0s' $(seq 64)) $2"
}
# wrun NAME [vm] ARGS...: the plan, the gates (with the VM record when vm), the real run
wrun() {
  local name="$1" vm=""; shift
  [ "${1:-}" != vm ] || { vm=vm; shift; }
  rs "$name-plan" "$@" --plan
  gates "$name" "$vm"
  rs "$name" "$@"
}
assets() { (cd "$FAKE_GH/releases/$1" && ls | LC_ALL=C sort | tr '\n' ' '); }

# 1. a Mac-only release: no Windows asset, no windows block, the plan says none
rs w-130-plan v1.3.0 --key "$key" --plan
gates w-130
# step 6 with no Windows files (review TT3): a build that writes a windows block, or leaves a
# Windows asset, is refused
FAKE_BUILD_FAULT=winblock-when-none rs w-130-winblock v1.3.0 --key "$key"
refused w-130-winblock "dist/payload.json has a windows block, which the plan doesn't"
FAKE_BUILD_FAULT=stale-ps1 rs w-130-ps1 v1.3.0 --key "$key"
refused w-130-ps1 "dist/bootstrap.ps1 is there, but this release has no Windows files"
rs w-130 v1.3.0 --key "$key"
released w-130 v1.3.0
planned w-130-plan "windows\.app none"
planned w-130-plan "windows\.bridge none"
logged w-130 "Windows app     none (no Windows files: no --windows, and none in the previous release)"
[ "$(assets v1.3.0)" = "LICENSE NOTICE SHA256SUMS bootstrap.sh libonnxruntime.1.18.0.dylib payload.json payload.json.sig rbstemsplus-app.zip stemsplus-model.onnx " ] \
  && ok || bad "w-130: the assets are $(assets v1.3.0)"
payload v1.3.0 windows "(missing)"
publish v1.3.0

# 2. the first release with Windows files (--windows): every Windows file new, so the VM record
# is needed: refused without it, released with it; the thirteen assets and the windows block
commit_version 1.4.0; push
for c in dl nuget pocketfft blocksrt flac; do mkdir -p "$dev/build/$c"; echo "pinned $c" > "$dev/build/$c/pin"; done
export FAKE_WIN_CACHE_LOG="$t/caches.log"; : > "$FAKE_WIN_CACHE_LOG"
rs w-140-novm-plan v1.4.0 --key "$key" --windows --plan
gates w-140-novm
public_state > "$t/public.before"
rs w-140-novm v1.4.0 --key "$key" --windows
refused w-140-novm "no passing VM run for"
public_unchanged w-140-novm
logged w-140-novm "the VM record (tests/win/vm-run.sh check): the Windows app is new, the Windows x64 stub is new"
planned w-140-novm-plan "windows\.app new"
planned w-140-novm-plan "windows\.stub\.x64 new"
planned w-140-novm-plan "windows\.admin new"
planned w-140-novm-plan "windows\.bridge new"
logged w-140-novm-plan "The VM record is needed (step 4b)"
logged w-140-novm-plan "Run each suite in tests/win/suites/release.list with --record"
logged w-140-novm-plan "scripts/release.sh v1.4.0 --windows"
# VR1's layout (review TT6): a row of release.list without its record, or with a failed one
rs w-140-plan v1.4.0 --key "$key" --windows --plan
gates w-140 vm
r140="$dev/build/gates/$(g -C "$dev" rev-parse HEAD)/vm-win"
mv "$r140/uninstall@1111-aaaa-arm64.ok" "$t/uninstall.ok.saved"
rs w-140-row v1.4.0 --key "$key" --windows
refused w-140-row "no passing VM run for"
logged w-140-row "no record of uninstall"
mv "$t/uninstall.ok.saved" "$r140/uninstall@1111-aaaa-arm64.ok"
echo "result fail" > "$r140/bootstrap@1111-aaaa-arm64.failed"
rs w-140-failed v1.4.0 --key "$key" --windows
refused w-140-failed "no passing VM run for"
rm -f "$r140/bootstrap@1111-aaaa-arm64.failed"
# a run interrupted after the VM's arch was known: no .ok, an incomplete .failed in its place
mv "$r140/update@1111-aaaa-arm64.ok" "$t/update.ok.saved"
{ rec_head update "$(g -C "$dev" rev-parse HEAD)" "$(shasum -a 256 "$dev/dist-test/payload.json" | cut -c1-64)"; echo "result incomplete"; } \
  > "$r140/update@1111-aaaa-arm64.failed"
rs w-140-incomplete v1.4.0 --key "$key" --windows
refused w-140-incomplete "no passing VM run for"
logged w-140-incomplete "update on \"{1111-aaaa}\" (arm64) failed: update@1111-aaaa-arm64.failed"
rm -f "$r140/update@1111-aaaa-arm64.failed"; mv "$t/update.ok.saved" "$r140/update@1111-aaaa-arm64.ok"
# the check's exit 0 alone isn't enough: its vm-win.ok must be this commit's and this TEST payload's
for f in commit payload none; do
  FAKE_VM_OK=$f rs "w-140-ok$f" v1.4.0 --key "$key" --windows
  refused "w-140-ok$f" "tests/win/vm-run.sh check passed, but"
done
# a VM run holds the share's lock: the check exits 2 without checking, and release.sh says so
mkdir -p "$dev/build/win/.vm-run.lock"; echo $$ > "$dev/build/win/.vm-run.lock/pid"
rs w-140-locked v1.4.0 --key "$key" --windows
refused w-140-locked "a VM run holds the share's lock"
logged w-140-locked "another vm-run.sh (pid $$) drives the VM"
rm -rf "$dev/build/win/.vm-run.lock"
# TT1's worker (BM1): no reference, a reference changed during the build, a drifted worker
mv "$dev/dist-test/windows-release.sha256" "$t/rel140"
rs w-140-norel v1.4.0 --key "$key" --windows
refused w-140-norel "dist-test/windows-release.sha256 (admin SHA256: the release worker the TEST build made) is missing"
{ cat "$t/rel140"; cat "$t/rel140"; } > "$dev/dist-test/windows-release.sha256"
rs w-140-tworel v1.4.0 --key "$key" --windows
refused w-140-tworel "is missing or not one such line"
rm -f "$dev/dist-test/windows-release.sha256"; ln -s "$t/rel140" "$dev/dist-test/windows-release.sha256"
rs w-140-symrel v1.4.0 --key "$key" --windows
refused w-140-symrel "is missing or not one such line"
rm -f "$dev/dist-test/windows-release.sha256"
cp "$t/rel140" "$dev/dist-test/windows-release.sha256"
FAKE_BUILD_FAULT=relchange rs w-140-relchange v1.4.0 --key "$key" --windows
refused w-140-relchange "dist-test/windows-release.sha256 changed after step 4b read it"
cp "$t/rel140" "$dev/dist-test/windows-release.sha256"
FAKE_BUILD_FAULT=relsymlink rs w-140-relsymlink v1.4.0 --key "$key" --windows
refused w-140-relsymlink "dist-test/windows-release.sha256 changed after step 4b read it"
rm -f "$dev/dist-test/windows-release.sha256" "$dev/dist-test/rel.saved"
cp "$t/rel140" "$dev/dist-test/windows-release.sha256"
FAKE_BUILD_FAULT=admindrift rs w-140-admindrift v1.4.0 --key "$key" --windows
refused w-140-admindrift "dist/payload.json's windows.app.admin_sha256 isn't the release worker the TEST build made"
# review TK5: a bootstrap.ps1 that trusts this checkout's test key isn't released (step 6)
mkdir -p "$dev/build/test-keys"
/usr/bin/openssl ecparam -name prime256v1 -genkey -noout 2> /dev/null | /usr/bin/openssl ec -pubout -out "$dev/build/test-keys/test.pub.pem" 2> /dev/null
FAKE_PS1_KEY="$(/usr/bin/openssl ec -pubin -in "$dev/build/test-keys/test.pub.pem" -outform DER 2> /dev/null | /usr/bin/base64 | tr -d '\n')" \
  rs w-140-testkey v1.4.0 --key "$key" --windows
refused w-140-testkey "dist/bootstrap.ps1 trusts the test key"
rm -rf "$dev/build/test-keys"
# T27: the Windows performance record is waived below 1.5.1, including this first draft.
rm "$dev/build/gates/$(g -C "$dev" rev-parse HEAD)/perf-win.tsv"
# If step 10 lists the live zip, show it a copy without Prepare, then restore the checked zip.
real_python="$(command -v python3)"
python3 -I -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1], "w").close()' "$t/no-prepare.zip"
cat > "$t/bin/python3" <<'PYTHON'
#!/bin/bash
if [ "$#" = 3 ] && [ "$1" = -I ] && [ "$2" = - ] && [ "$3" = "$FAKE_DEV/dist-test/rbstemsplus-windows.zip" ]; then
  mv "$3" "$3.saved" && cp "$ZIP_WITHOUT_PREPARE" "$3" || exit 1
  trap 'mv "$3.saved" "$3"' EXIT
fi
"$REAL_PYTHON" "$@"
PYTHON
chmod 755 "$t/bin/python3"
REAL_PYTHON="$real_python" ZIP_WITHOUT_PREPARE="$t/no-prepare.zip" rs w-140 v1.4.0 --key "$key" --windows
rm "$t/bin/python3"
released w-140 v1.4.0
cmp -s "$GATES_ARCHIVE/WP/rbstemsplus-windows.zip" "$dev/dist-test/rbstemsplus-windows.zip" \
  && grep -q ' WP/rbstemsplus-windows.zip$' "$GATES_ARCHIVE/SHA256SUMS" \
  && ok || bad "w-140-wp: release.sh did not archive and seal WP from the Windows TEST build"
# T27: the first release with Windows files archives WB and the worker's W0 (J14), sealed
cmp -s "$GATES_ARCHIVE/WB/rbstemsplus-windows.zip" "$dev/dist-test/rbstemsplus-windows.zip" \
  && [ "$(python3 -I -c 'import sys, zipfile; sys.stdout.buffer.write(zipfile.ZipFile(sys.argv[1]).read("admin/RB Stems Plus.exe"))' "$dev/dist-test/rbstemsplus-windows.zip" | shasum -a 256)" \
       = "$(shasum -a 256 < "$GATES_ARCHIVE/W0/worker/RB Stems Plus.exe")" ] \
  && [ -f "$GATES_ARCHIVE/tag-v1.4.0/rbstemsplus-windows.zip" ] \
  && (cd "$GATES_ARCHIVE" && shasum -a 256 -c --quiet SHA256SUMS) && ok || bad "w-140: WB, W0's worker or tag-v1.4.0 not archived"
# step 5's build in the public clone found every cache this checkout has (review TT9): copied in
# (never llvm-mingw/ or dotnet/), nothing downloaded there
grep ' /public$\| .*/public$' "$FAKE_WIN_CACHE_LOG" > "$t/pub-caches" || true
[ -s "$t/pub-caches" ] && ! grep -q ' downloaded ' "$t/pub-caches" && [ "$(grep -c ' cached ' "$t/pub-caches")" -ge 5 ] \
  && [ "$(cat "$t/public/build/nuget/pin" 2> /dev/null)" = "pinned nuget" ] && [ ! -e "$t/public/build/llvm-mingw" ] \
  && ok || bad "w-140: step 5's build in the public clone: $(tr '\n' ';' < "$t/pub-caches")"

[ -f "$r140/../vm-win.ok" ] && ok || bad "w-140: the check wrote no vm-win.ok"
[ "$(assets v1.4.0)" = "LICENSE NOTICE NOTICE-windows.txt SHA256SUMS bootstrap.ps1 bootstrap.sh libonnxruntime.1.18.0.dylib payload.json payload.json.sig rbstemsplus-app.zip rbstemsplus-bridge-win-x64.dll rbstemsplus-windows.zip stemsplus-model.onnx " ] \
  && ok || bad "w-140: the assets are $(assets v1.4.0)"
payload v1.4.0 windows.app.version 1.4.0
payload v1.4.0 windows.app.sha256 "$(rsha v1.4.0 rbstemsplus-windows.zip)"
payload v1.4.0 windows.app.stub_sha256.x64 "$(esha v1.4.0 'stub/x64/RB Stems Plus.exe')"
payload v1.4.0 windows.app.admin_sha256 "$(esha v1.4.0 'admin/RB Stems Plus.exe')"
payload v1.4.0 windows.bridge.sha256 "$(rsha v1.4.0 rbstemsplus-bridge-win-x64.dll)"
zip140="$(rsha v1.4.0 rbstemsplus-windows.zip)"; wb140="$(rsha v1.4.0 rbstemsplus-bridge-win-x64.dll)"
x64140="$(esha v1.4.0 'stub/x64/RB Stems Plus.exe')"; arm140="$(esha v1.4.0 'stub/arm64/RB Stems Plus.exe')"; adm140="$(esha v1.4.0 'admin/RB Stems Plus.exe')"
publish v1.4.0

# 3. a Mac-only change, no --windows: Windows files by themselves (the previous release has
# them), all carried byte for byte; no VM record needed. The checkout's download caches are
# copied into the carry decision's build: nothing downloaded there, the checkout's own untouched
echo "a Mac fix" >> "$dev/README.md"; commit_version 1.4.1; push
: > "$FAKE_WIN_CACHE_LOG"
rs w-141-plan v1.4.1 --key "$key" --plan
gates w-141
FAKE_BUILD_FAULT=lists rs w-141-lists v1.4.1 --key "$key"
refused w-141-lists "dist/payload.json's lists (windows compatible or resampler_proved, the model, previous_models) aren't the TEST build's"
# review TR1/TR4: dist-test/payload.json changed during the build, with no VM record needed
cp "$dev/dist-test/payload.json" "$t/test-payload.saved"
FAKE_BUILD_FAULT=retest rs w-141-retest v1.4.1 --key "$key"
refused w-141-retest "dist-test/payload.json changed after step 4b read it"
cp "$t/test-payload.saved" "$dev/dist-test/payload.json"
: > "$FAKE_WIN_CACHE_LOG"
rs w-141 v1.4.1 --key "$key"
released w-141 v1.4.1
cmp -s "$GATES_ARCHIVE/WB/payload.json" "$GATES_ARCHIVE/tag-v1.4.0/payload.json" && [ -d "$GATES_ARCHIVE/tag-v1.4.1" ] \
  && ok || bad "w-141: WB isn't v1.4.0's any more, or tag-v1.4.1 not archived"
[ "$(grep -c ' cached .*/decide-win/src$' "$FAKE_WIN_CACHE_LOG")" = 5 ] && ! grep -q ' downloaded ' "$FAKE_WIN_CACHE_LOG" \
  && ok || bad "w-141: the carry decision's build didn't find every cache: $(tr '\n' ';' < "$FAKE_WIN_CACHE_LOG")"
[ "$(cat "$dev/build/nuget/pin")" = "pinned nuget" ] && ! grep -q "not in this checkout" "$t/w-141.log" && ok || bad "w-141: the caches"
planned w-141-plan "windows\.app carry 1\.4\.0 $zip140"
planned w-141-plan "windows\.stub\.x64 carry $x64140"
planned w-141-plan "windows\.bridge carry $wb140"
logged w-141 "Windows app     carried: 1.4.0 (the same files as v1.4.0's zip)"
logged w-141 "the VM record: not needed (every Windows file carried, the lists unchanged)"
cmp -s "$FAKE_GH/releases/v1.4.0/rbstemsplus-windows.zip" "$FAKE_GH/releases/v1.4.1/rbstemsplus-windows.zip" && ok || bad "w-141: the zip isn't v1.4.0's"
cmp -s "$FAKE_GH/releases/v1.4.0/rbstemsplus-bridge-win-x64.dll" "$FAKE_GH/releases/v1.4.1/rbstemsplus-bridge-win-x64.dll" && ok || bad "w-141: the bridge isn't v1.4.0's"
payload v1.4.1 windows.app.version 1.4.0
publish v1.4.1

# 4. the Windows app changed, not its stubs, worker or bridge: a new zip holding the previous
# stubs and worker (R4.I83); the VM record needed
echo "class App2 {}" >> "$dev/win/shell/App.cs"; commit_version 1.4.2; push
wrun w-142-novm v1.4.2 --key "$key"
refused w-142-novm "no passing VM run for"
rs w-142-plan v1.4.2 --key "$key" --plan
gates w-142 vm
FAKE_BUILD_FAULT=nocarryfile rs w-142-nocarry v1.4.2 --key "$key"
refused w-142-nocarry "dist/rbstemsplus-windows.zip's stub/x64/RB Stems Plus.exe isn't v1.4.1's, which the plan carries"
FAKE_BUILD_FAULT=stubsha rs w-142-stubsha v1.4.2 --key "$key"
refused w-142-stubsha "dist/payload.json's SHA-256 of stub/x64/RB Stems Plus.exe isn't the zip's"
cp "$dev/dist-test/payload.json" "$t/test-payload.saved"
FAKE_BUILD_FAULT=retest-ws rs w-142-retest v1.4.2 --key "$key"
refused w-142-retest "dist-test/payload.json changed after step 4b read it"
cp "$t/test-payload.saved" "$dev/dist-test/payload.json"
rs w-142 v1.4.2 --key "$key"
released w-142 v1.4.2
planned w-142-plan "windows\.app new"
planned w-142-plan "windows\.stub\.x64 carry $x64140"
planned w-142-plan "windows\.stub\.arm64 carry $arm140"
planned w-142-plan "windows\.admin carry $adm140"
planned w-142-plan "windows\.bridge carry $wb140"
logged w-142 "files differ from v1.4.1's zip: RB Stems Plus.exe"
payload v1.4.2 windows.app.version 1.4.2
payload v1.4.2 windows.app.stub_sha256.x64 "$x64140"
payload v1.4.2 windows.app.admin_sha256 "$adm140"
[ "$(rsha v1.4.2 rbstemsplus-windows.zip)" != "$zip140" ] && ok || bad "w-142: the zip is v1.4.0's"
publish v1.4.2

# 5. a stub's code changed: both stubs new, the worker still carried. A cache this checkout
# doesn't have: said, and the build downloads it (its pin checked), as the Mac builds do
echo "int stub2;" >> "$dev/win/c/stub.c"; commit_version 1.4.3; push
mv "$dev/build/nuget" "$t/nuget.saved"; : > "$FAKE_WIN_CACHE_LOG"
rs w-143-plan v1.4.3 --plan
logged w-143-plan "== not in this checkout: build/nuget: scripts/build-windows.sh downloads them"
grep -q '^nuget downloaded .*/decide-win/src$' "$FAKE_WIN_CACHE_LOG" && [ "$(grep -c ' cached .*/decide-win/src$' "$FAKE_WIN_CACHE_LOG")" = 4 ] \
  && ok || bad "w-143: the missing cache: $(tr '\n' ';' < "$FAKE_WIN_CACHE_LOG")"
[ ! -e "$dev/build/nuget" ] && ok || bad "w-143: the download went into the checkout's build/"
mv "$t/nuget.saved" "$dev/build/nuget"
planned w-143-plan "windows\.app new"
planned w-143-plan "windows\.stub\.x64 new"
planned w-143-plan "windows\.stub\.arm64 new"
planned w-143-plan "windows\.admin carry $adm140"
gates w-143 vm
FAKE_BUILD_FAULT=noentry rs w-143-noentry v1.4.3 --key "$key"
refused w-143-noentry "dist/rbstemsplus-windows.zip doesn't hold exactly one stub/arm64/RB Stems Plus.exe"
FAKE_BUILD_FAULT=emptysha rs w-143-emptysha v1.4.3 --key "$key"
refused w-143-emptysha "dist/payload.json's SHA-256 of stub/x64/RB Stems Plus.exe isn't 64 hex digits"
FAKE_BUILD_FAULT=oldstubs rs w-143-oldstubs v1.4.3 --key "$key"
refused w-143-oldstubs "dist/payload.json's windows.app.stub_sha256.x64 isn't the TEST build's, which the VM ran"
FAKE_BUILD_FAULT=oldstubs-arm64 rs w-143-oldarm v1.4.3 --key "$key"
refused w-143-oldarm "dist/payload.json's windows.app.stub_sha256.arm64 isn't the TEST build's"
FAKE_BUILD_FAULT=dupentry rs w-143-dup v1.4.3 --key "$key"
refused w-143-dup "dist/rbstemsplus-windows.zip doesn't hold exactly one stub/x64/RB Stems Plus.exe"
FAKE_BUILD_FAULT=spoofname rs w-143-spoof v1.4.3 --key "$key"
refused w-143-spoof "dist/rbstemsplus-windows.zip can't be read as a zip"
for f in casename dirslash fileasfolder; do
  FAKE_BUILD_FAULT=$f rs "w-143-$f" v1.4.3 --key "$key"
  refused "w-143-$f" "dist/rbstemsplus-windows.zip has entry names that collide as the updater compares them"
done
rs w-143 v1.4.3 --key "$key"
released w-143 v1.4.3
payload v1.4.3 windows.app.version 1.4.3
payload v1.4.3 windows.app.admin_sha256 "$adm140"
[ "$(esha v1.4.3 'stub/x64/RB Stems Plus.exe')" != "$x64140" ] && [ "$(esha v1.4.3 'admin/RB Stems Plus.exe')" = "$adm140" ] \
  && ok || bad "w-143: the stubs aren't new or the worker isn't v1.4.0's"
publish v1.4.3

# 6. only the lists changed (R4.I94): every file carried, the VM record needed all the same
echo "7.2.19 7.2.20" > "$dev/win/compat.txt"; commit_version 1.4.4; push
wrun w-144-novm v1.4.4 --key "$key"
refused w-144-novm "no passing VM run for"
logged w-144-novm "the TEST payload's lists (windows compatible or resampler_proved, the model, previous_models) differ from v1.4.3's"
planned w-144-novm-plan "windows\.app carry 1\.4\.3 $(rsha v1.4.3 rbstemsplus-windows.zip)"
wrun w-144 vm v1.4.4 --key "$key"
released w-144 v1.4.4
payload v1.4.4 windows.compatible.rekordbox.1 7.2.20

# 7. the plan's decisions against the run's: a Windows toolchain's drift after the plan
echo more >> "$dev/README.md"; commit_version 1.4.5; push
rs w-145-plan v1.4.5 --plan
gates w-145
FAKE_WIN_DRIFT=msvc-2 rs w-145 v1.4.5 --key "$key"
refused w-145 "the carry decisions differ from the plan's"

# 8. a previous zip that isn't what its payload names: refused before any decision
publish v1.4.4
cp "$FAKE_GH/releases/v1.4.4/rbstemsplus-windows.zip" "$t/zip.saved"
printf 'X' >> "$FAKE_GH/releases/v1.4.4/rbstemsplus-windows.zip"
rs w-tamper v1.4.5 --plan
refused w-tamper "v1.4.4's rbstemsplus-windows.zip doesn't have the SHA-256 its payload.json names"
cp "$t/zip.saved" "$FAKE_GH/releases/v1.4.4/rbstemsplus-windows.zip"

# 9. after a release with Windows files, a commit without scripts/build-windows.sh: refused
g -C "$dev" rm -q scripts/build-windows.sh && g -C "$dev" commit -q -m "no Windows build" && push
rs w-nobuild v1.4.5 --plan
refused w-nobuild 'has a "windows" block, and'
g -C "$dev" revert --no-edit HEAD > /dev/null && push

# the fake takes the real build-windows.sh's other options (T12): a TEST build, --check-repro
(cd "$dev" && scripts/build-windows.sh --test https://127.0.0.1:1/ --test-key "$key" --check-repro "$t/bw-test") > /dev/null 2>&1 \
  && [ -f "$t/bw-test/rbstemsplus-windows.zip" ] && ok || bad "the fake build-windows.sh refused --test URL --test-key PEM --check-repro"

# 10. the dry run shows the Windows decisions and assets
rs w-dry v1.4.5 --dry-run
logged w-dry "Windows assets  rbstemsplus-windows.zip rbstemsplus-bridge-win-x64.dll bootstrap.ps1 NOTICE-windows.txt"
logged w-dry "Windows app     carried: 1.4.3"

# 11. only the Windows bridge changed (review TT4): the zip carried, the bridge new, so the VM
# record is needed; a bridge other than the TEST build's, which the VM ran, is refused (TT1)
echo "int bridge_win2;" >> "$dev/win/bridge/bridge_win.c"; commit_version 1.4.6; push
wrun w-146-novm v1.4.6 --key "$key"
refused w-146-novm "no passing VM run for"
logged w-146-novm "the VM record (tests/win/vm-run.sh check): the Windows bridge is new"
planned w-146-novm-plan "windows\.app carry 1\.4\.3 $(rsha v1.4.4 rbstemsplus-windows.zip)"
planned w-146-novm-plan "windows\.stub\.x64 carry $(esha v1.4.4 'stub/x64/RB Stems Plus.exe')"
planned w-146-novm-plan "windows\.bridge new"
gates w-146 vm
FAKE_WBRIDGE_DRIFT=relinked rs w-146-drift v1.4.6 --key "$key"
refused w-146-drift "dist/payload.json's windows.bridge.sha256 isn't the TEST build's, which the VM ran"
rs w-146 v1.4.6 --key "$key"
released w-146 v1.4.6
payload v1.4.6 windows.app.version 1.4.3
cmp -s "$FAKE_GH/releases/v1.4.4/rbstemsplus-windows.zip" "$FAKE_GH/releases/v1.4.6/rbstemsplus-windows.zip" && ok || bad "w-146: the zip isn't v1.4.4's"
[ "$(rsha v1.4.6 rbstemsplus-bridge-win-x64.dll)" != "$(rsha v1.4.4 rbstemsplus-bridge-win-x64.dll)" ] && ok || bad "w-146: the bridge isn't new"

# 13. a row of release.list whose suite the commit doesn't have (the real check's contract, in the
# fake): step 4b fails on it, even with a record of it that looks like a pass
publish v1.4.6
echo "int bridge_win3;" >> "$dev/win/bridge/bridge_win.c"
printf '%s\n' 'ghost      arm64  {1111-aaaa}' >> "$dev/tests/win/suites/release.list"
commit_version 1.4.7; push
rs w-147-plan v1.4.7 --key "$key" --plan
gates w-147 vm
c147="$(g -C "$dev" rev-parse HEAD)"
{ rec_head ghost "$c147" "$(shasum -a 256 "$dev/dist-test/payload.json" | cut -c1-64)"; echo "step user install PASS 0 10s"; echo "result pass"; } \
  > "$dev/build/gates/$c147/vm-win/ghost@1111-aaaa-arm64.ok"
rs w-147-nosuite v1.4.7 --key "$key"
refused w-147-nosuite "no passing VM run for"
logged w-147-nosuite "$c147 has no tests/win/suites/ghost.suite (release.list's row ghost)"

# 14. the real tests/win/vm-run.sh check with a VM row and release.list's worker row
cp "$root/tests/win/vm-run.sh" "$dev/tests/win/vm-run.sh"
mkdir -p "$dev/tests/win/suites/s"; echo '# the one step' > "$dev/tests/win/suites/s/one.ps1"
echo 'sys  60  one  s/one.ps1' | tee "$dev/tests/win/suites/one.suite" > "$dev/tests/win/suites/worker.suite"
worker_row="$(sed -n '/^worker[[:space:]]/p' "$root/tests/win/suites/release.list")"
printf '%s\n' 'one  arm64  {1111-aaaa}' "$worker_row" > "$dev/tests/win/suites/release.list"
commit_all "the real vm-run.sh and two suites"
# (case 13's runs stopped after the public commit: drop it, as release.sh asks the owner to)
g -C "$t/public" reset -q --hard origin/main
rs w-147r-plan v1.4.7 --key "$key" --plan
logged w-147r-plan "Plan written: "
gates w-147r
c14="$(g -C "$dev" rev-parse HEAD)"; d14="$dev/build/gates/$c14/vm-win"; mkdir -p "$d14"
ps14="$(shasum -a 256 "$dev/dist-test/payload.json" | cut -c1-64)"
logged w-147r-plan "build/gates/$c14/pc-win.ok for worker:"
logged w-147r-plan "build/gates/$c14/win-by-hand.ok for VR2:"
fsha() { g -C "$dev" show "$c14:$1" | shasum -a 256 | cut -c1-64; }
synth() {   # STEP-LINE [SUITE SNAPSHOT]: record at c14 on the TEST build, as --record writes it
  local suite="${2:-one}" snapshot="${3:-clean Windows}"
  { echo "commit $c14"; echo "suite $suite"; echo "snapshot {1111-aaaa} $snapshot"; echo "vm_uuid 202a80c8-4bc4-4e95-890e-a31d6bfd5b70"; echo "arch arm64"
    echo "payload_sha256 $ps14"; echo "stage_commit $c14"; echo "vm_commit -"
    echo "run r1"; echo "file tests/win/suites/$suite.suite $(fsha "tests/win/suites/$suite.suite")"
    echo "file tests/win/suites/s/one.ps1 $(fsha tests/win/suites/s/one.ps1)"; echo "file tests/win/vm-run.sh $(fsha tests/win/vm-run.sh)"
    echo "$1"; echo "result pass"; } > "$d14/$suite@1111-aaaa-arm64.ok"
}
pc="$d14/../pc-win.ok"
pc_fixture() {   # COMMIT PAYLOAD-SHA [HITS-EXIT [DIRECTML-EXIT [CPU-LINE]]], as pc-session.sh auto record writes it
  { printf 'commit %s\npayload_sha256 %s\n' "$1" "$2"
    echo "round worker (release.list's worker row, on the real PC: OD2)"
    echo 'date 2026-11-13T00:00:00Z'
    printf 'step %s 0\n' session stage preflight cache-install T2
    echo "step hits ${3:-0}"
    echo "step directml ${4:-0}"
    printf 'step %s 0\n' cache-uninstall collect cleanup
    [ -z "${5:-}" ] || echo "$5"
    printf 'log 20261113-000000-auto-record.log\nresult pass\n'; } > "$pc"
}
pc_check() {
  VMRUN_SHARE="$t/vm-share" "$dev/tests/win/vm-run.sh" check "$c14" "$dev/dist-test" > "$t/pc-check.log" 2>&1
}
pc_refused() {
  local status=0
  pc_check || status=$?
  [ "$status" = 1 ] && [ ! -e "$d14/../vm-win.ok" ] \
    && grep -qF "pc-win.ok: $1" "$t/pc-check.log" \
    && ok || bad "PC record ($1): exit $status, $(tr '\n' ';' < "$t/pc-check.log")"
}
synth "step sys one PASS 0 2s"
pc_fixture "$c14" "$ps14"
mv "$pc" "$t/pc.saved"
synth "step sys one PASS 0 2s" worker "rekordbox 7.2.19 installed"
pc_refused "missing record"  # a passing worker VM record cannot substitute for the PC
mv "$d14/worker@1111-aaaa-arm64.ok" "$d14/worker@1111-aaaa-arm64.failed"
pc_fixture "$(printf '%040d' 0)" "$ps14"; pc_refused "record commit does not match"
pc_fixture "$c14" "$(printf '%064d' 0)"; pc_refused "record TEST payload SHA-256 does not match"
pc_fixture "$c14" "$ps14" 3; pc_refused "required check hits did not pass"
pc_fixture "$c14" "$ps14" 0 3; pc_refused "required check directml did not pass"
cpu_line="directml cpu, accepted by --cpu-stems-ok"
pc_fixture "$c14" "$ps14" 0 0 "$cpu_line"; pc_refused "required check directml did not pass"
pc_fixture "$c14" "$ps14" 3 3 "$cpu_line"; pc_refused "required check hits did not pass"
pc_fixture "$c14" "$ps14" 0 3 "directml cpu"; pc_refused "invalid record format"
pc_fixture "$c14" "$ps14" 0 3 "$cpu_line"
pc_check && grep -qx "row worker arm64 rekordbox 7.2.19 installed: pc-win.ok $(shasum -a 256 "$pc" | cut -c1-64)" "$d14/../vm-win.ok" \
  && ok || bad "PC record (CPU stems on the owner's ruling): $(tr '\n' ';' < "$t/pc-check.log")"
rm -f "$d14/../vm-win.ok"
cp "$t/pc.saved" "$pc"; printf 'result pass\n' >> "$pc"; pc_refused "invalid record format"
cp "$t/pc.saved" "$pc"
pc_check && grep -qx "row worker arm64 rekordbox 7.2.19 installed: pc-win.ok $(shasum -a 256 "$pc" | cut -c1-64)" "$d14/../vm-win.ok" \
  && ok || bad "PC record (valid): $(tr '\n' ';' < "$t/pc-check.log")"

hand="$d14/../win-by-hand.ok"
vr2_refused() {
  local name="$1" why="$2"
  VMRUN_SHARE="$t/vm-share" FAKE_BUILD_FAULT=record-must-stop rs "$name" v1.4.7 --key "$key"
  refused "$name" "no passing VR2 by-hand record"
  logged "$name" "win-by-hand.ok: $why"
}
vr2_refused w-147r-hand-missing "missing record"
hand_record "$c14"; cp "$hand" "$t/hand.saved"
sed -i '' "s/^commit .*/commit $(printf '%040d' 0)/" "$hand"
vr2_refused w-147r-hand-commit "record commit does not match"
cp "$t/hand.saved" "$hand"
sed -i '' "s/^payload_sha256 .*/payload_sha256 $(printf '%064d' 0)/" "$hand"
vr2_refused w-147r-hand-payload "record TEST payload SHA-256 does not match"
cp "$t/hand.saved" "$hand"

synth "step sys one FAIL 1 2s"
VMRUN_SHARE="$t/vm-share" rs w-147r-fail v1.4.7 --key "$key"
refused w-147r-fail "no passing VM run for"
logged w-147r-fail "one@1111-aaaa-arm64.ok: a step in it didn't pass"
# VT2's rules: exactly the suite's files (each once) and one result line, else refused
synth "step sys one PASS 0 2s"; sed -i '' '/^file tests\/win\/vm-run.sh /d' "$d14/one@1111-aaaa-arm64.ok"
VMRUN_SHARE="$t/vm-share" rs w-147r-nofile v1.4.7 --key "$key"
refused w-147r-nofile "no passing VM run for"
logged w-147r-nofile "one@1111-aaaa-arm64.ok: its files aren't exactly"
synth "step sys one PASS 0 2s"; echo "file tests/win/suites/s/one.ps1 $(fsha tests/win/suites/s/one.ps1)" >> "$d14/one@1111-aaaa-arm64.ok"
VMRUN_SHARE="$t/vm-share" rs w-147r-twice v1.4.7 --key "$key"
refused w-147r-twice "no passing VM run for"
logged w-147r-twice "one@1111-aaaa-arm64.ok: its files aren't exactly"
synth "step sys one PASS 0 2s"; echo "result fail" >> "$d14/one@1111-aaaa-arm64.ok"
VMRUN_SHARE="$t/vm-share" rs w-147r-results v1.4.7 --key "$key"
refused w-147r-results "no passing VM run for"
logged w-147r-results "one@1111-aaaa-arm64.ok: it isn't one pass"
# VT1: a run holding the share's lock: the real check exits 2, release.sh says so
synth "step sys one PASS 0 2s"
mkdir -p "$t/vm-share/.vm-run.lock"; echo $$ > "$t/vm-share/.vm-run.lock/pid"
VMRUN_SHARE="$t/vm-share" rs w-147r-locked v1.4.7 --key "$key"
refused w-147r-locked "a VM run holds the share's lock"
logged w-147r-locked "another vm-run.sh (pid $$) drives the VM"
rm -rf "$t/vm-share/.vm-run.lock"
# RV2: a gates folder of its own makes the check read and write records release.sh doesn't read
synth "step sys one PASS 0 2s"
VMRUN_GATES="$t/other-gates" VMRUN_SHARE="$t/vm-share" rs w-147r-gates v1.4.7 --key "$key"
refused w-147r-gates "VMRUN_ROOT or VMRUN_GATES is set (a test variable)"
# RV1: step 6 checks the VM record again before signing; a --record rerun that failed during the
# build (the record now a .failed) is refused
VMRUN_SHARE="$t/vm-share" FAKE_BUILD_FAULT=vmfail rs w-147r-vmfail v1.4.7 --key "$key"
refused w-147r-vmfail "the VM record changed during the build"
logged w-147r-vmfail "== VMRUN_SHARE is set: the lock checked is $t/vm-share's"
# BS3: a --record rerun that passed during the build (exit 0, another run's record) is refused by the cmp
rm -f "$d14/one@1111-aaaa-arm64.failed"; synth "step sys one PASS 0 2s"
VMRUN_SHARE="$t/vm-share" FAKE_BUILD_FAULT=vmchange rs w-147r-vmchange v1.4.7 --key "$key"
refused w-147r-vmchange "the VM record changed during the build"
rm -f "$d14/one@2222-bbbb-arm64.ok"
rm -f "$d14/one@1111-aaaa-arm64.failed"; synth "step sys one PASS 0 2s"
VMRUN_SHARE="$t/vm-share" FAKE_BUILD_FAULT=pcchange rs w-147r-pcchange v1.4.7 --key "$key"
refused w-147r-pcchange "the VM record changed during the build"
cp "$t/pc.saved" "$pc"
VMRUN_SHARE="$t/vm-share" FAKE_BUILD_FAULT=handchange rs w-147r-handchange v1.4.7 --key "$key"
refused w-147r-handchange "win-by-hand.ok changed during the build"
cp "$t/hand.saved" "$hand"
VMRUN_SHARE="$t/vm-share" rs w-147 v1.4.7 --key "$key"
released w-147 v1.4.7
logged w-147 "VR2 by-hand record passed:"
grep -qx "row one arm64 {1111-aaaa}: one@1111-aaaa-arm64.ok" "$dev/build/gates/$c14/vm-win.ok" \
  && ok || bad "w-147: the real check's vm-win.ok: $(tr '\n' ';' < "$dev/build/gates/$c14/vm-win.ok" 2> /dev/null)"

# 15. a replaced TEST Windows zip stops step 10 after the draft, with the archive command
publish v1.4.7
commit_version 1.4.8; push
rs w-148-plan v1.4.8 --key "$key" --plan
gates w-148
printf 'not a zip\n' > "$dev/dist-test/rbstemsplus-windows.zip"
rs w-148-badzip v1.4.8 --key "$key"
[ "$rs_rc" != 0 ] && ok || bad "w-148-badzip: release.sh exited 0"
logged w-148-badzip "tag-v1.4.8/rbstemsplus-windows.zip isn't the one dist-test/SHA256SUMS lists"
logged w-148-badzip "the draft v1.4.8 is made, but the TEST build wasn't archived"
logged w-148-badzip "run tests/perf/archive-release.sh v1.4.8 --payload-sha $(shasum -a 256 "$dev/dist-test/payload.json" | cut -c1-64)"
[ -f "$FAKE_GH/releases/v1.4.8/payload.json.sig" ] && [ ! -e "$GATES_ARCHIVE/tag-v1.4.8" ] \
  && ok || bad "w-148-badzip: missing draft or a tag folder was archived"

echo "windows-test: $pass passed, $fail failed"
[ "$fail" = 0 ]

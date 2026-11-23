#!/bin/bash
# G3's tart run (release.md §6.1, mac-core.md §9 and §7; task T26): the in-app update from the
# tag TEST builds (v1.0.0, v1.3.0, any shipped 1.3.x; gates-archive/tag-v*) to the TEST build of
# the release commit, in fresh clones of the macOS 26 template (macOS 26 only is tested: owner,
# 2026-11-12; macOS 12 to 15 should work, untested), one VM at a time; the record
# build/gates/<commit>/g3-mac.ok, which scripts/release.sh checks (g3.sh check) before it builds,
# --branch included. Intel Macs aren't tested (owner, 2026-11-12): no Rosetta scenario; the app
# still ships universal. Lane L1 (an agent) runs it, outside the sandbox (tart, ssh). tart is
# ~/Applications/tart.app/Contents/MacOS/tart (not on PATH).
#
#   tests/mac/g3.sh plan [OPTIONS]      the dry run: checks every input (the TEST build, its plan,
#                                       the gate archive's checksums, the starting points, the
#                                       templates) and prints each scenario's steps, which ones are
#                                       scripted, emulated or may need the owner; no VM is touched.
#                                       Exit 0: a run can start; 1: something is missing (said)
#   tests/mac/g3.sh run [OPTIONS]       the run; build/gates/<commit>/g3-mac.ok when every scenario
#                                       passed (else g3-mac.failed); the scenarios' results in
#                                       build/gates/<commit>/g3/. Exit 0 pass, 1 fail, 2 can't start,
#                                       4 stopped: rekordbox showed a licence, sign-in or activation
#                                       text (nothing pressed; that clone kept running, the owner told)
#   tests/mac/g3.sh check COMMIT [DIR]  the check scripts/release.sh runs before it builds (main
#                                       and --branch): g3-mac.ok is this commit's, for the TEST
#                                       payload in DIR (default dist-test/) and the plan as it is now,
#                                       every required scenario passed (recomputed from the plan and
#                                       the archive), both guests covered, no stub
#   tests/mac/g3.sh post-publish V      after publishing V (a fresh clone each): the real published
#                                       1.3.0 updates in-app from the real releases/latest to V's
#                                       Mac app (its payload.json's app.version), and the real
#                                       one-liner installs it; build/gates/post-V.txt. The
#                                       guest downloads from GitHub: run it while the owner is there
#                                       (Little Snitch may ask)
#   tests/mac/g3.sh smoke [TEMPLATE]    one fresh clone of TEMPLATE (default rb26-template), headless
#                                       as every clone: the guest's facts (macOS, SIP), the G3
#                                       Driver pushed and its Accessibility grant tried, a dialog
#                                       of macOS's own answered OK by the driver (g3-guest.sh
#                                       dialog: the Accessibility press proven); the clone deleted
#   tests/mac/g3.sh build               the G3 Driver (tests/mac/g3ax.swift) into build/harness/vm/
#   tests/mac/g3.sh selftest            tests/mac/g3-test.sh (no VM)
# OPTIONS
#   --build DIR          the TEST build (default dist-test/: scripts/build.sh --test
#                        http://192.168.64.1:8765/ --plan build/gates/<commit>/plan)
#   --plan FILE          its plan (default build/gates/<commit>/plan, scripts/release.sh --plan)
#   --guests "T…"        the templates (default "rb26-template"): each must be macOS 26 or later
#   --from "vX.Y.Z…"     the starting points (default v1.0.0, v1.3.0, every archived v1.3.x, and the
#                        plan's previous release when archived)
#   --only GLOB          only the scenarios whose name matches (the record then can't pass)
#   --maint              a maintenance branch's TEST build (maint/1.3: its release.sh has no
#                        --plan): its plan is written here, the app and the bridge new, the
#                        previous release the newest tag below it; its release.sh doesn't
#                        check the record yet: it will, with main's g3.sh, once w6/g3-maint is
#                        merged after the first real G3 round (owner, 2026-11-12)
# Scenarios, each in a fresh clone (release.md §6.1):
#   Mac app new: "update FROM" per starting point and guest: FROM installed by its bootstrap,
#     Stems Cache installed with it, the new build served, the old app's notice, Update; the new
#     app is the version under test, no quarantine, its watcher registered again (1.0's --watch
#     LaunchAgent replaced), the Stems Cache offer only when the bridge changed; Reinstall and
#     Install; the complete uninstall and the trace audit.
#   Mac app carried: "carried PREVIOUS" (no notice, the version in About, the report and the log,
#     no Stems Cache offer unless the bridge changed) and "carried-update v1.0.0" (a notice, and the
#     update installs the carried app).
#   Always: "bootstrap" per guest: the new build's bootstrap.sh installs on a clean clone, no
#     quarantine, and the app starts.
# Clicks (R4.I98): the new app is driven through its probe (no click); App Management's Allow and
# the password are emulated (DJ's TCC row, sudo without a password: recorded as emulated); the old
# apps' buttons (no probe) are pressed through Accessibility by the G3 Driver when the clone allows
# it, else the owner clicks in the VM's screen when asked (each such step waits at most 10 minutes
# and is recorded as an owner click). Every wait is at most 10 minutes.
# Screens: every G3 and post-publish clone starts with tart's VNC server (vm.sh up --vnc), which
# opens no window on the owner's Mac; nothing restarts. Where the owner must look or click, the
# notice (G3_NOTIFY) names its vnc://HOST:PORT to open in Screen Sharing, its password only in
# build/harness/vnc-url (0600): the core's update scenario (the watcher's notification prompt:
# Allow), the old apps' clicks when the G3 Driver has no Accessibility, and a clone kept running
# after a licence stop.
# Smoke, 2026-11-12 (rb26-template, macOS 26.6.2): SIP is on and root can't write the system's
# TCC.db ("readonly database"), so the driver's Accessibility can't be scripted: the old apps' two
# clicks per update scenario (Install Stems Cache, Update) are the owner's, unless the owner allows
# "G3 Driver" once in the template (the grant is for its ad-hoc code hash: a build from a changed
# g3ax.swift or with another Swift toolchain, e.g. after an Xcode update, voids the grant).
# Screenshots (2026-11-17): macOS's screencapture is denied in the guest, headless or not, over SSH
# (as root, admin or DJ) and from a LaunchAgent in DJ's session alike: no Screen Recording grant
# in the template (tccd: "kTCCServiceScreenCapture does not allow prompting; returning denied").
set -uo pipefail
# here: this checkout, the tools': vm.sh and its state (build/harness: the clone pointer, the
# shared folder), the guest steps, the gate archive (one, in the tools checkout: release.md §6.2)
here="$(cd "$(dirname "$0")/../.." && pwd)"
# root: the checkout whose dist-test/, build/gates and tags a run or a check reads; G3_ROOT:
# another one (G3_ROOT=<maint> g3.sh run --maint; the maintenance branch's release.sh will run
# main's g3.sh check from a temporary copy (w6/g3-maint, after the first real G3 round), with no
# archive there: the tags alone then decide)
root="${G3_ROOT:-$here}"
TART="${G3_TART:-$HOME/Applications/tart.app/Contents/MacOS/tart}"
VM="${G3_VM:-$here/tests/harness/vm.sh}"                 # G3_VM, G3_TART, G3_SERVE_STUB: the selftest's
ARCHIVE="${GATES_ARCHIVE:-$here/gates-archive}"
GATES="${G3_GATES:-$root/build/gates}"
STATE="${G3_STATE:-$here/build/harness}"            # vm.sh's own state (tests/harness/vm.sh)
XFER="$STATE/xfer"; VX=/Users/admin/xfer                 # vm.sh's shared folder, host and guest
H=/Users/Shared/rbsp-harness
URL=http://192.168.64.1:8765/
LIMIT="${G3_LIMIT:-600}"                                 # every wait, in seconds: at most 10 minutes
[ "$LIMIT" -le 600 ] 2>/dev/null || { echo "g3.sh: G3_LIMIT must be at most 600 seconds" >&2; exit 2; }
GITHUB=https://github.com/Gabe-LS/rbstemsplus/releases
usage() { sed -n '2,81p' "$0"; exit 2; }
stop() { echo "g3.sh: $*" >&2; exit 2; }
say() { echo "[$(date +%H:%M:%S)] $*"; }
pj() { python3 -I -c 'import json, sys
d = json.load(open(sys.argv[1]))
for k in sys.argv[2].split("."): d = d[k]
print(d)' "$1" "$2"; }
sha() { shasum -a 256 "$1" | cut -c1-64; }
# vms: tart's list into VMS, and the running VMs' names into RUNNING; stops when tart can't list
# (the VM state unknown: never read as "none running")
# (tart list is bounded: at most 60 s, or LIMIT when that is shorter)
TLIST=$(( LIMIT < 60 ? LIMIT : 60 ))
vms() {
  VMS="$(bounded "$TLIST" "$TART" list 2>/dev/null)" || stop "tart list failed or took over $TLIST s: the VM state is unknown"
  RUNNING="$(awk 'NR > 1 && $NF == "running" {print $2}' <<< "$VMS" | tr '\n' ' ' | sed 's/ *$//')"
}
vm_names() { awk 'NR > 1 {print $2}' <<< "$VMS"; }

# ---- bounded commands and the guest ----------------------------------------------------------

# bounded SECS CMD…: CMD in its own process group, stopped after SECS (exit 124 then): the whole
# group gets TERM, and KILL 5 s later. CMD's output goes to a file, printed once its leader has
# returned or been stopped, so a child left in the group (vm.sh up's tart run) never keeps the
# caller's $(…) open. Each running group and its watcher are listed in ACTIVE (a file each: a
# global set inside $(…) never reaches this shell), with the timeout markers, so an interrupt
# stops them all (kill_active). A TERM sent to this shell alone while it waits in $(gst …) is
# acted on once that command returns (up to LIMIT+5 s); Ctrl-C reaches the $(…) at once.
ACTIVE="$STATE/g3-active.$$"
trap 'rm -rf "$ACTIVE"' EXIT
bounded() {
  local s="$1"; shift
  mkdir -p "$ACTIVE" || return 1
  local o; o="$(mktemp "$ACTIVE/out.XXXXXX")" || return 1
  perl -e 'setpgrp(0, 0); exec @ARGV or exit 127' "$@" < /dev/null > "$o" & local p=$!
  ( i=0; while [ $i -lt "$s" ]; do sleep 1; kill -0 $p 2>/dev/null || exit 0; i=$((i + 1)); done
    touch "$ACTIVE/timeout.$p"; kill -TERM -- -$p 2>/dev/null
    i=0; while [ $i -lt 5 ]; do sleep 1; kill -0 -- -$p 2>/dev/null || exit 0; i=$((i + 1)); done
    kill -KILL -- -$p 2>/dev/null ) > /dev/null 2>&1 & local w=$!
  echo "$p $w" > "$ACTIVE/$p"
  wait $p; local rc=$?
  if [ -e "$ACTIVE/timeout.$p" ]; then
    wait $w 2>/dev/null                    # the group's grace period, then KILL: at most 5 s more
    rm -f "$ACTIVE/timeout.$p"; rc=124
  else
    kill $w 2>/dev/null; wait $w 2>/dev/null
  fi
  rm -f "$ACTIVE/$p"; cat "$o" 2>/dev/null; rm -f "$o"
  return $rc
}
# kill_active: every running bounded group (and its watcher) gets TERM, then KILL after at most 5 s
kill_active() {
  local f p w i any
  for f in "$ACTIVE"/[0-9]*; do [ -f "$f" ] || continue; read -r p w < "$f"; kill -TERM -- -"$p" 2>/dev/null; kill "$w" 2>/dev/null; done
  for i in 1 2 3 4 5; do
    any=0
    for f in "$ACTIVE"/[0-9]*; do [ -f "$f" ] || continue; read -r p w < "$f"; kill -0 -- -"$p" 2>/dev/null && any=1; done
    [ $any = 0 ] && break; sleep 1
  done
  for f in "$ACTIVE"/[0-9]*; do [ -f "$f" ] || continue; read -r p w < "$f"; kill -KILL -- -"$p" 2>/dev/null; done
  rm -rf "$ACTIVE"
}
# gst STEP ARGS…: one step of tests/mac/g3-guest.sh in the VM, as root, at most LIMIT seconds
gst() {
  ours || return 1
  local c="sudo bash $H/g3-guest.sh $(printf '%q ' "$1" "$GRUN")" a
  shift
  for a in "$@"; do c+="$(printf '%q ' "$a")"; done
  bounded "$LIMIT" "$VM" sh "$c"; local rc=$?
  [ $rc = 124 ] && echo "FAIL timed out after $LIMIT s: $c" >> "$RUNH/result"
  [ $rc = 4 ] && licence_stop
  return $rc
}
# licence_stop: the guest saw a licence, sign-in or activation text in rekordbox (g3-guest.sh's
# rb_guard, exit 4; nothing pressed there): G3 stops at once, the clone is kept running as it is
# (no more steps, no logs taken, never deleted), the owner told (G3_NOTIFY)
KEEP_CLONE=0
licence_stop() {
  local why; why="$(sed -n 's/^STOP //p' "$RUNH/result" | tail -1)"
  [ -n "$why" ] || why="rekordbox showed a licence text"
  KEEP_CLONE=1; ABORT=1
  [ -n "${rec:-}" ] && [ -f "$rec" ] && echo "stopped: $clone kept running: $why" >> "$rec"
  notify_owner "G3 STOPPED in clone $clone (kept running, nothing clicked; open $VNC_URL in Screen Sharing, password: $STATE/vnc-url): $why"
  exit 4
}
# wait_cond SECS NAME ARGS…: polls the guest's condition every 5 s, at most SECS (≤ LIMIT): each
# poll and each pause capped at the time left, a success counted only within it
wait_cond() {
  local s="$1"; shift
  [ "$s" -le "$LIMIT" ] || s="$LIMIT"
  local end=$((SECONDS + s)) left
  while :; do
    ours || return 1                       # every poll: the pointer still names this run's clone
    left=$((end - SECONDS)); [ $left -gt 0 ] || return 1
    [ $left -le 60 ] || left=60
    if bounded "$left" "$VM" sh "sudo bash $H/g3-guest.sh cond $(printf '%q ' "$GRUN" "$@")" > /dev/null 2>&1; then
      [ $SECONDS -le $end ] && return 0
      return 1
    fi
    left=$((end - SECONDS)); [ $left -gt 0 ] || return 1
    sleep $(( left < 5 ? left : 5 ))
  done
}
# ours: vm.sh's pointer (build/harness/clone: vm.sh sh and delete act on it) still names this run's
# clone; else another lane started a VM meanwhile: FAIL, the run stops, nothing is deleted
ABORT=0
ours() {
  local p; p="$(cat "$STATE/clone" 2>/dev/null)"
  [ -n "$clone" ] && [ "$p" = "$clone" ] && return 0
  echo "FAIL the VM pointer $STATE/clone names ${p:-nothing}, not this run's clone ${clone:-(none)}: another lane's VM? The run stops; nothing is deleted" >> "${RUNH:-$STATE}/result"
  ABORT=1; return 1
}
serve() {
  if [ -n "${G3_SERVE_STUB:-}" ]; then "$G3_SERVE_STUB" "$1"; return; fi
  serve_stop
  if curl -s -o /dev/null --max-time 2 "$URL"; then stop "something already serves $URL (another lane's server?)"; fi
  nohup python3 -I "$here/tests/harness/serve.py" "$1" > "$STATE/g3-server.log" 2>&1 & echo $! > "$STATE/g3-server.pid"
  local i; for i in $(seq 1 50); do curl -s -o "$STATE/g3-served.json" --max-time 2 "${URL}payload.json" && break; sleep 0.1; done
  cmp -s "$STATE/g3-served.json" "$1/payload.json" || stop "the server on 8765 doesn't serve $1"
}
# the server stopped: its pid killed only while it is still tests/harness/serve.py (a stale file
# after a reboot may name another process)
serve_stop() {
  local p; p="$(cat "$STATE/g3-server.pid" 2>/dev/null)"
  if [ -n "$p" ] && ps -p "$p" -o command= 2>/dev/null | grep -q "tests/harness/serve.py"; then kill "$p" 2>/dev/null; fi
  rm -f "$STATE/g3-server.pid"; true
}

# ---- the G3 Driver (Accessibility) ------------------------------------------------------------

DRIVER="$STATE/vm/G3 Driver.app"
build_driver() {
  local exe="$DRIVER/Contents/MacOS/g3ax"
  [ -x "$exe" ] && [ "$exe" -nt "$here/tests/mac/g3ax.swift" ] && return 0
  mkdir -p "$DRIVER/Contents/MacOS" "$STATE/swift-cache"
  cat > "$DRIVER/Contents/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>io.github.rbstemsplus.g3driver</string>
<key>CFBundleExecutable</key><string>g3ax</string>
<key>CFBundleName</key><string>G3 Driver</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>LSMinimumSystemVersion</key><string>12.0</string>
<key>LSUIElement</key><true/>
</dict></plist>
EOF
  xcrun swiftc -O -module-cache-path "$STATE/swift-cache" -sdk "$(xcrun --sdk macosx --show-sdk-path)" -target arm64-apple-macos12.0 \
    "$here/tests/mac/g3ax.swift" -o "$exe" || stop "the G3 Driver doesn't build"
  codesign -f -s - --identifier io.github.rbstemsplus.g3driver "$DRIVER" || stop "the G3 Driver couldn't be signed"
}

# ---- inputs -------------------------------------------------------------------------------------

build="$root/dist-test"; planf=""; guests="rb26-template"; froms=""; only=""; maint=""
parse() {
  while [ $# -gt 0 ]; do
    case "$1" in
      --build) build="$2"; shift 2 ;;
      --plan) planf="$2"; shift 2 ;;
      --guests) guests="$2"; shift 2 ;;
      --from) froms="$2"; shift 2 ;;
      --only) only="$2"; shift 2 ;;
      --maint) maint=1; shift ;;
      *) usage ;;
    esac
  done
}
missing=()
resolve() {
  build="$(cd "$build" 2>/dev/null && pwd)" || stop "no TEST build folder"
  local f
  for f in rbstemsplus-app.zip payload.json payload.json.sig bootstrap.sh libonnxruntime.1.18.0.dylib stemsplus-model.onnx; do
    [ -e "$build/$f" ] || stop "no $build/$f: the TEST build of the release commit first (scripts/build.sh --test $URL --plan …)"
  done
  commit="$(pj "$build/payload.json" commit)"
  [[ "$commit" =~ ^[0-9a-f]{40}$ ]] || stop "payload.json's commit is \"$commit\": the TEST build of a committed tree (no -dirty)"
  # relv: the release (payload_version: About, the report, the log's started line); newv: the Mac
  # app's own version (app.version: the notice, the update, Info.plist); they differ when the app
  # is carried (release.md T24)
  relv="$(pj "$build/payload.json" payload_version)"
  newv="$(pj "$build/payload.json" app.version)"; new_bridge="$(pj "$build/payload.json" bridge.sha256)"
  new_model="$(pj "$build/payload.json" model.sha256)"; payload_sha="$(sha "$build/payload.json")"
  [[ "$newv" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || stop "the app's version $newv isn't x.y.z"
  [[ "$relv" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || stop "payload_version $relv isn't x.y.z"
  unzip -p "$build/rbstemsplus-app.zip" "RB Stems Plus.app/Contents/MacOS/RB Stems Plus" 2>/dev/null > "$STATE/g3-exe.bin"
  grep -aq "TEST BUILD" "$STATE/g3-exe.bin" && grep -aq "rbstemsplus-probe" "$STATE/g3-exe.bin" \
    || stop "$build/rbstemsplus-app.zip isn't a TEST build with the probe"
  rm -f "$STATE/g3-exe.bin"
  planf="${planf:-$GATES/$commit/plan}"
  if [ -n "$maint" ] && [ ! -f "$planf" ]; then
    # the maintenance branch's release.sh has no --plan and never carries: its plan says so, the
    # previous release being the newest tag below this one
    local pv
    pv="$(git -C "$root" tag -l 'v*' 2>/dev/null | python3 -I -c 'import re, sys
v = tuple(map(int, sys.argv[1].split(".")))
t = [tuple(map(int, m.groups())) for m in (re.fullmatch(r"v(\d+)\.(\d+)\.(\d+)", l.strip()) for l in sys.stdin) if m]
t = [x for x in t if x < v]
print("v%d.%d.%d" % max(t) if t else "none")' "$relv")"
    mkdir -p "$(dirname "$planf")"
    printf '# tests/mac/g3.sh --maint: a maintenance branch TEST build (its release.sh has no --plan; it never carries)\nplan 1\ncommit %s\nversion %s\nprevious %s\nmac.app new\nmac.bridge new\n' \
      "$commit" "$relv" "$pv" > "$planf"
    echo "the maintenance plan written: $planf (previous $pv)"
  fi
  [ -f "$planf" ] || stop "no plan $planf: scripts/release.sh vX.Y.Z --plan first, then the TEST build in its shape (a maintenance branch's TEST build: --maint)"
  [ "$(awk '$1 == "commit" {print $2}' "$planf")" = "$commit" ] || stop "$planf isn't the plan of $commit"
  plan_sha="$(sha "$planf")"
  app_dec="$(awk '$1 == "mac.app" {print $2}' "$planf")"; bridge_dec="$(awk '$1 == "mac.bridge" {print $2}' "$planf")"
  carried_v="$(awk '$1 == "mac.app" && $2 == "carry" {print $3}' "$planf")"
  prev="$(awk '$1 == "previous" {print $2}' "$planf")"
  case "$app_dec" in new|carry) ;; *) stop "$planf has no mac.app decision" ;; esac
  [ "$app_dec" = new ] || [ "$carried_v" = "$newv" ] || stop "the plan carries app $carried_v, but the TEST build's app is $newv"
  [ -f "$ARCHIVE/SHA256SUMS" ] || stop "no gate archive at $ARCHIVE (SHA256SUMS)"
  (A="$ARCHIVE"; . "$here/tests/perf/archive.sh" && verify) || stop "the gate archive doesn't match its SHA256SUMS: stop (never rebuild what it keeps)"
  [ "$app_dec" = new ] || { [ -n "$prev" ] && [ "$prev" != none ]; } || stop "the plan carries the app but names no previous release"
  required="$(required_froms)"
  [ -n "$froms" ] || froms="$required"
  # every required starting point must be archived (a shipped 1.3.x tag included: G47)
  for f in $required; do [ -d "$ARCHIVE/tag-$f" ] || stop "the starting point $f (required) isn't in the gate archive ($ARCHIVE/tag-$f): archive its TEST build first"; done
  for f in $froms; do
    local d="$ARCHIVE/tag-$f" x
    for x in rbstemsplus-app.zip payload.json payload.json.sig bootstrap.sh libonnxruntime.1.18.0.dylib; do
      [ -f "$d/$x" ] || stop "no $d/$x: the starting point $f isn't in the gate archive"
    done
    [ "v$(rel_of "$f")" = "$f" ] || stop "$d holds release $(rel_of "$f"), not $f"
    [ "$(pj "$d/payload.json" model.sha256)" = "$new_model" ] || stop "$f's payload names another model than the TEST build's: serve its own model (not handled)"
  done
  vms
  missing=()
  for f in $guests; do vm_names | grep -qx "$f" || missing+=("$f"); done
}
# vlt A B: version A is below version B (x.y.z, a leading v allowed), compared as numbers
vlt() {
  local a="${1#v}" b="${2#v}" i x y
  [[ "$a" =~ ^[0-9]+(\.[0-9]+)*$ && "$b" =~ ^[0-9]+(\.[0-9]+)*$ ]] || return 1
  local IFS=.; local -a A=($a) B=($b)
  for i in 0 1 2 3; do
    x="${A[i]:-0}"; y="${B[i]:-0}"
    [ "$((10#$x))" -lt "$((10#$y))" ] && return 0
    [ "$((10#$x))" -gt "$((10#$y))" ] && return 1
  done
  return 1
}
# required_froms: the starting points a passing record covers (release.md §6.1), from the plan
# (app_dec, prev), the release's version (relv: run's payload_version, the plan's version line in
# check) and the archive: the app new, v1.0.0, v1.3.0, every archived or tagged 1.3.x below the
# release, and the previous release when archived; the app carried, the previous release and v1.0.0
required_froms() {
  if [ "$app_dec" != new ]; then echo "$prev v1.0.0"; return; fi
  local r="v1.0.0 v1.3.0" f t
  for f in "$ARCHIVE"/tag-v1.3.*; do [ -d "$f" ] && t="${f##*/tag-}" && vlt "$t" "$relv" && [[ " $r " != *" $t "* ]] && r+=" $t"; done
  local tags                                        # G3_TAGS: the selftest's
  if [ -n "${G3_TAGS+x}" ]; then tags="$G3_TAGS"; else tags="$(git -C "$root" tag -l "v1.3.*" 2>/dev/null)"; fi
  for t in $tags; do vlt "$t" "$relv" && [[ " $r " != *" $t "* ]] && r+=" $t"; done
  [ -n "$prev" ] && [ "$prev" != none ] && [ -d "$ARCHIVE/tag-$prev" ] && [[ " $r " != *" $prev "* ]] && r+=" $prev"
  echo "$r"
}
# matrix GUESTS FROMS: every scenario, one per line: GUEST NAME [FROM]
matrix() {
  local g f
  for g in $1; do
    echo "$g bootstrap"
    if [ "$app_dec" = new ]; then
      for f in $2; do echo "$g update $f"; done
    else
      echo "$g carried $prev"; echo "$g carried-update v1.0.0"
    fi
  done
}
# required_scenarios GUESTS: the record's required lines, "GUEST NAME" (no --from, no --only)
required_scenarios() { matrix "$1" "$(required_froms)" | while read -r g n f; do echo "$g $n${f:+-$f}"; done; }
# has_scenario RECORD GUEST NAME [RESULT]: the record has that scenario's line (exact fields)
has_scenario() { awk -v g="$2" -v n="$3" -v r="${4:-}" '$1 == "scenario" && $2 == g && $3 == n && (r == "" || $4 == r) {f = 1} END {exit !f}' "$1"; }
# the scenarios to run, one per line: GUEST NAME [FROM]
scenarios() {
  matrix "$guests" "$froms" | while read -r g n f; do
    [ -z "$only" ] || [[ "$n${f:+-$f}" == $only ]] && echo "$g $n${f:+ $f}"
  done
}
from_dir() { echo "$STATE/g3-serve/$1"; }
# a starting point's release (payload_version; app.version in tags whose payload has none) and its
# Mac app's own version (app.version: what its bootstrap installs)
rel_of() { python3 -I -c 'import json, sys; d = json.load(open(sys.argv[1])); print(d.get("payload_version") or d["app"]["version"])' "$ARCHIVE/tag-$1/payload.json"; }
app_of() { pj "$ARCHIVE/tag-$1/payload.json" app.version; }
# a starting point's folder to serve: its archived TEST build, and the model (the same SHA-256)
stage_from() {
  local d; d="$(from_dir "$1")"; rm -rf "$d"; mkdir -p "$d"
  local x; for x in rbstemsplus-app.zip payload.json payload.json.sig bootstrap.sh libonnxruntime.1.18.0.dylib; do ln -s "$ARCHIVE/tag-$1/$x" "$d/$x"; done
  ln -s "$build/stemsplus-model.onnx" "$d/stemsplus-model.onnx"
}
changed() { [ "$(pj "$ARCHIVE/tag-$1/payload.json" bridge.sha256)" = "$new_bridge" ] && echo 0 || echo 1; }
is10() { [[ "$(app_of "$1" 2>/dev/null || echo "${1#v}")" == 1.0.* ]]; }

# ---- clicks: the G3 Driver, else the owner ---------------------------------------------------------

AX=no; clicks=()
# click_step NAME "what the owner does" "COND…" AXFUNC [ARGS…]
click_step() {
  local name="$1" what="$2" cond="$3"; shift 3
  if [ "$AX" = yes ] && [ -z "${G3_NO_AX:-}" ]; then
    AX_PENDING=""; "$@"; local rc=$?
    if [ $rc = 0 ] || [ $rc = 4 ]; then
      # exit 4: the last press reported a failure; its effect is this step's condition
      # shellcheck disable=SC2086
      if wait_cond "$LIMIT" $cond; then
        if [ $rc = 4 ]; then
          ax_log "\"$AX_PENDING\": press confirmed by its effect ($cond)"
          echo "ok $name (Accessibility; \"$AX_PENDING\": press confirmed by its effect)" >> "$RUNH/result"
        else echo "ok $name (Accessibility)" >> "$RUNH/result"; fi
        return 0
      fi
      if [ $rc = 4 ]; then
        ax_log "\"$AX_PENDING\" didn't take the press, and its effect ($cond) didn't show within $LIMIT s"
        echo "FAIL $name: the G3 Driver couldn't press it (\"$AX_PENDING\" didn't take the press, $cond didn't hold within $LIMIT s; ax.log)" >> "$RUNH/result"
      else echo "FAIL $name: pressed, but $cond didn't hold within $LIMIT s" >> "$RUNH/result"; fi
      return 1
    fi
    [ $rc = 3 ] || { echo "FAIL $name: the G3 Driver couldn't press it (ax.log)" >> "$RUNH/result"; return 1; }
  fi
  say "OWNER, in the VM's screen ($clone${VNC_URL:+, $VNC_URL in Screen Sharing, password: $STATE/vnc-url}): $what. Waiting up to $((LIMIT / 60)) minutes."
  # shellcheck disable=SC2086
  if wait_cond "$LIMIT" $cond; then echo "ok $name (the owner's click)" >> "$RUNH/result"; clicks+=("$name"); return 0; fi
  echo "FAIL $name: not done within $LIMIT s (owner click)" >> "$RUNH/result"; return 1
}
# A press whose action opens a modal dialog (the old apps' confirmations: NSApp.runModal, an alert,
# a modal sheet) is answered only when the dialog closes, so the G3 Driver reports "didn't take the
# press" (the Accessibility messaging timeout, kAXErrorCannotComplete) though the press worked. Such
# a press counts as taken when its effect shows, else it fails as before: the effect of a press
# followed by another is that next button found (in a new driver call: the dialog opened); of the
# last press, the click step's condition (exit 4, its title in AX_PENDING: click_step waits for it).
# "press confirmed by its effect" goes to ax.log and the result. (The driver itself is unchanged:
# a rebuilt g3ax would void the template's Accessibility grant.)
# ax_untaken LINE: the driver's first line says a press (a button's, a menu item's) didn't take
ax_untaken() { [[ "$1" == "FAIL \""*"\" didn't take the press" || "$1" == "FAIL the menu item didn't take the press" ]]; }
ax_log() { echo "ax (g3.sh): $*" >> "$RUNH/ax.log"; }
# ax_press [--after WHAT] BUNDLEID MS TITLE…: the driver's press of TITLE… in order (g3ax.swift's
# press verb), a press that didn't take counted by its effect (above); --after: WHAT's press (a
# menu item) didn't take, and its effect is TITLE…'s first button found
ax_press() {
  local pending="" out rc first n
  [ "$1" = --after ] && { pending="$2"; shift 2; }
  local bid="$1" ms="$2"; shift 2
  while :; do
    out="$(gst ax press "$bid" "$ms" "$@")"; rc=$?
    printf '%s\n' "$out"
    first="$(head -1 <<< "$out")"
    if [ -n "$pending" ]; then
      # the earlier press's effect: this call found a button (pressed it, or reached one whose press didn't take)
      if grep -q '^pressed: ' <<< "$out" || ax_untaken "$first"; then
        ax_log "\"$pending\": press confirmed by its effect (the next button found)"
        echo "note \"$pending\": press confirmed by its effect (the next button found)" >> "$RUNH/result"
      else
        ax_log "\"$pending\" didn't take the press, and its effect didn't show ($first)"; return 1
      fi
    fi
    { [ $rc = 1 ] && ax_untaken "$first"; } || return $rc
    # the titles the driver went through before the one that didn't take: its "pressed:" and "not there:" lines
    n="$(grep -cE '^(pressed|not there): ' <<< "$out")"
    [ "$n" -lt $# ] || return 1
    shift "$n"; pending="${1%\?}"; shift
    [ $# -gt 0 ] || { AX_PENDING="$pending"; return 4; }
  done
}
ax_install_cache() {
  if is10 "$1"; then ax_press io.github.rbstemsplus.app 30000 "Install Stems Plus + Stems Cache" Install
  else ax_press io.github.rbstemsplus.app 30000 "Manage?" "Install Stems Cache" Install; fi
}
ax_update() {
  local out rc after=()
  out="$(gst ax menu io.github.rbstemsplus.app 60000 "Update RB Stems Plus…" "Update RB Stems Plus")"; rc=$?
  printf '%s\n' "$out"
  if [ $rc != 0 ]; then
    { [ $rc = 1 ] && ax_untaken "$(head -1 <<< "$out")"; } || return $rc
    after=(--after "the menu item Update RB Stems Plus")
  fi
  ax_press "${after[@]+"${after[@]}"}" io.github.rbstemsplus.app 30000 Update
}
install_what() { if is10 "$1"; then echo "click Install Stems Plus + Stems Cache, then Install (no password is asked)"; else echo "on the Manage tab, click Install Stems Cache, then Install (no password is asked)"; fi; }
update_what() { if is10 "$1"; then echo "in the RB Stems Plus menu choose Update RB Stems Plus, then click Update"; else echo "in the RB Stems Plus menu choose Update RB Stems Plus…, then click Update"; fi; }

# notify_owner TEXT: said, and sent with G3_NOTIFY (a command taking the text, e.g. a push
# notifier) when set; its failure never stops the run
notify_owner() { say "OWNER: $1"; [ -z "${G3_NOTIFY:-}" ] || "$G3_NOTIFY" "$1" > /dev/null 2>&1 || say "(G3_NOTIFY failed)"; }
# keep_fail_logs: a failed scenario's guest evidence (g3-guest.sh faillogs: the logs, background
# items, the LaunchAgent, the processes) into GATES/<commit>/g3-fail-<clone>/ before the clone goes
keep_fail_logs() {
  [ -n "$clone" ] && [ "$ABORT" = 0 ] || return 0
  local c="$clone"
  gst faillogs > /dev/null 2>&1
  [ -d "$RUNH/fail-logs" ] || { say "   the guest's logs couldn't be kept"; return 0; }
  rm -rf "${GATES:?}/$commit/g3-fail-$c"; cp -R "$RUNH/fail-logs" "$GATES/$commit/g3-fail-$c"
  say "   the guest's logs kept: $GATES/$commit/g3-fail-$c"
}

# ---- clones -------------------------------------------------------------------------------------

clone=""; facts=""; VNC_URL=""
# clone_up TEMPLATE [clicks]: one VM at a time, a name never used before, with tart's VNC server
# (no window; its vnc://HOST:PORT in VNC_URL); clicks: the old apps' buttons follow, so without the
# G3 Driver's Accessibility the owner is sent the address to click there
clone_up() {
  vms
  if [ -n "$RUNNING" ]; then echo "FAIL a VM is running ($RUNNING): one VM at a time; the run stops" >> "$RUNH/result"; ABORT=1; return 1; fi
  n_clone=$((${n_clone:-0} + 1)); local name="${G3_TEST_CLONE:-g3-$(date +%Y%m%d%H%M%S)-$$-$n_clone}"   # G3_TEST_CLONE: the selftest's
  if vm_names | grep -qx "$name"; then echo "FAIL a VM named $name exists already: the run stops" >> "$RUNH/result"; ABORT=1; return 1; fi
  clone="$name"; VNC_URL=""
  vm_up "$1" || return 1
  local p; p="$XFER/g3push-$(date +%s)$RANDOM"; rm -rf "$XFER"/g3push-*; mkdir -p "$p/bin"
  cp "$here/tests/harness/lib.sh" "$here/tests/harness/run.sh" "$here/tests/mac/g3-guest.sh" "$p/"
  [ -x "$STATE/vm/vmtool" ] && cp "$STATE/vm/vmtool" "$p/bin/"
  cp -R "$DRIVER" "$p/"
  bounded 300 "$VM" sh "sudo mkdir -p $H && sudo ditto $VX/${p##*/} $H && sudo chmod -R a+rX $H" > /dev/null || { echo "FAIL the harness couldn't be pushed" >> "$RUNH/result"; return 1; }
  facts="$(gst facts)"
  if ! grep -qx "accessibility yes" <<< "$facts"; then
    echo "note Accessibility for the G3 Driver: $(gst ax-grant | tail -1)" >> "$RUNH/result"
    facts="$(gst facts)"
  fi
  grep -qx "accessibility yes" <<< "$facts" && AX=yes || AX=no
  printf '%s\n' "$facts" > "$RUNH/facts.txt"
  echo "note guest $1: $(tr '\n' ' ' <<< "$facts")" >> "$RUNH/result"
  if [ "${2:-}" = clicks ] && { [ $AX = no ] || [ -n "${G3_NO_AX:-}" ]; }; then
    notify_owner "G3: the old app's clicks are yours in clone $clone (the G3 Driver has no Accessibility): open $VNC_URL in Screen Sharing (password: $STATE/vnc-url)"
  fi
}
# vm_up TEMPLATE: vm.sh up --vnc of the clone; VNC_URL its vnc://HOST:PORT, from vm.sh's
# build/harness/vnc-url (the password left out)
vm_up() {
  # vm.sh up's exit 3: the template's MAC address (its one rekordbox seat) couldn't be held: the run stops
  TEMPLATE="$1" bounded "$LIMIT" "$VM" up "$clone" --vnc > "$RUNH/vm-up.log" 2>&1; local urc=$?
  if [ $urc = 3 ]; then echo "FAIL the clone $clone of $1 didn't come up with $1's MAC address ($(tail -1 "$RUNH/vm-up.log")): the run stops" >> "$RUNH/result"; ABORT=1; return 1; fi
  [ $urc = 0 ] || { echo "FAIL the clone $clone of $1 didn't come up (vm-up.log)" >> "$RUNH/result"; return 1; }
  ours || return 1
  VNC_URL="$(sed -n 's|^vnc://[^@/]*@|vnc://|p' "$STATE/vnc-url" 2>/dev/null | head -1)"
  [ -n "$VNC_URL" ] || { echo "FAIL the clone $clone of $1 came up without its VNC URL ($STATE/vnc-url)" >> "$RUNH/result"; return 1; }
}
# clone_down: the clone deleted, and gone from tart's list; else FAIL cleanup, its name into
# $STATE/g3-leftover, and the run stops (exit 1). Never deletes a VM the pointer doesn't say is ours.
clone_down() {
  serve_stop
  [ -n "$clone" ] || return 0
  local c="$clone" r="${RUNH:-$STATE}/result" rc=0 l
  if [ "$KEEP_CLONE" = 1 ]; then echo "note clone $c kept running (rekordbox's licence text): not deleted" >> "$r"; clone=""; return 0; fi
  if [ "$(cat "$STATE/clone" 2>/dev/null)" != "$c" ]; then
    echo "FAIL cleanup: clone $c not deleted (the VM pointer names another VM: nothing deleted)" >> "$r"; rc=1
  else
    bounded 300 "$VM" delete > /dev/null 2>&1 || rc=1
    if l="$(bounded "$TLIST" "$TART" list 2>/dev/null)"; then awk 'NR > 1 {print $2}' <<< "$l" | grep -qx "$c" && rc=1; else rc=1; fi
    [ $rc = 0 ] || echo "FAIL cleanup: clone $c not deleted" >> "$r"
  fi
  clone=""
  [ $rc = 0 ] && return 0
  echo "$c" >> "$STATE/g3-leftover"; ABORT=1; return 1
}
vm_session() { rm -rf "$ACTIVE"; trap 'clone_down; rm -rf "$ACTIVE"' EXIT; trap 'kill_active; exit 130' INT TERM; }

# ---- the scenarios --------------------------------------------------------------------------------

sc_bootstrap() {   # sc_bootstrap TEMPLATE
  clone_up "$1" || return 1
  gst prepare && serve "$build" && gst bootstrap "$URL" "$newv" && gst quit && gst new-start
}
sc_update() {   # sc_update TEMPLATE FROM
  local g="$1" from="$2" fv; fv="$(app_of "$2")"
  # the core's paths ask for the owner's one click (the watcher's notifications: Allow), in kept 1:
  # its VNC address sent to the owner (with the old app's clicks first when there's no Accessibility)
  if [ "$from" = "$(core_from)" ]; then
    clone_up "$g" || return 1
    local ax_note=""; { [ "$AX" = no ] || [ -n "${G3_NO_AX:-}" ]; } && ax_note="first the old app's clicks (no Accessibility): Install Stems Cache, then Update; then "
    notify_owner "G3: open $VNC_URL in Screen Sharing now (password: $STATE/vnc-url); ${ax_note}click Allow on RB Stems Plus Watcher when macOS asks (update-$from clone)"
  else clone_up "$g" clicks || return 1; fi
  stage_from "$from"
  gst prepare && serve "$(from_dir "$from")" && gst bootstrap "$URL" "$fv" && gst emulate || return 1
  click_step "$from: Stems Cache installed by the old app" "$(install_what "$from")" "cache-on" ax_install_cache "$from" || return 1
  gst before-update && serve "$build" || return 1
  gst switch || return 1
  wait_cond 120 notice "$newv" || { echo "FAIL $from: no update notice for $newv within 120 s (the old app's log)" >> "$RUNH/result"; return 1; }
  click_step "$from: Update RB Stems Plus in the old app" "$(update_what "$from")" "updated $newv" ax_update || return 1
  gst after-update "$newv" && gst offer "$(changed "$from")" || return 1
  gst manage cache && gst manage plus || return 1
  # the switch-over's core paths, once per guest (the newest starting point's scenario): the
  # report's lights, the prepared playlists' check over two rekordbox sessions (each ended by
  # rekordbox's Quit, as a user ends it: a kill leaves rekordboxAgent running), a queued action
  if [ "$from" = "$(core_from)" ]; then gst lights && gst kept 1 && gst kept 2 && gst queued || return 1; fi
  gst uninstall
}
# core_from: the starting point whose update scenario also runs the core's paths (the last one)
core_from() { local f l=""; for f in $froms; do l="$f"; done; echo "$l"; }
sc_carried() {   # sc_carried TEMPLATE PREVIOUS: the new payload proven read, then its display
  local g="$1" from="$2"
  clone_up "$g" clicks || return 1
  stage_from "$from"
  gst prepare && serve "$(from_dir "$from")" && gst bootstrap "$URL" "$(app_of "$from")" && gst emulate || return 1
  click_step "$from: Stems Cache installed" "$(install_what "$from")" "cache-on" ax_install_cache "$from" || return 1
  serve "$build" && gst switch && gst payload "$payload_sha" && gst carried "$relv" "$carried_v" "$(changed "$from")"
}
sc_carried_update() {   # sc_carried_update TEMPLATE v1.0.0: its notice, and the update installs the carried app
  local g="$1" from="$2"
  clone_up "$g" clicks || return 1
  stage_from "$from"
  gst prepare && serve "$(from_dir "$from")" && gst bootstrap "$URL" "$(app_of "$from")" && gst emulate || return 1
  click_step "$from: Stems Cache installed" "$(install_what "$from")" "cache-on" ax_install_cache "$from" || return 1
  gst before-update && serve "$build" && gst switch || return 1
  wait_cond 120 notice "$carried_v" || { echo "FAIL $from: no update notice for $carried_v within 120 s" >> "$RUNH/result"; return 1; }
  click_step "$from: Update RB Stems Plus in the old app" "$(update_what "$from")" "updated $carried_v" ax_update || return 1
  gst after-update "$carried_v" && gst offer "$(changed "$from")" || return 1
  # the same display checks as "carried", on a log segment after the update (the offer, if any,
  # was answered by the offer step: none expected again)
  gst payload "$payload_sha" && gst carried "$relv" "$carried_v" 0
}
describe() {   # what a scenario does, step by step (the dry run)
  local n="$2" f="${3:-}"
  case "$n" in
    bootstrap) echo "  fresh clone; the new build's bootstrap.sh as DJ: $newv installed, no quarantine, the app opens; its first window (probe)" ;;
    update) echo "  fresh clone; $f by its bootstrap; Stems Cache installed by $f [owner or Accessibility; emulated: App Management, sudo]"
      echo "  $newv served; $f's notice (its log); Update [owner or Accessibility]; the new app: $newv, updated, no quarantine, watcher registered again, Stems Cache offer: $( [ "$(changed "$f")" = 1 ] && echo "expected (the bridge changed)" || echo "none (the same bridge)")"
      echo "  Reinstall Stems Cache, Install Demucs v4 (probe); Uninstall RB Stems Plus…; the trace audit (rekordbox byte for byte, nothing left)"
      [ "$f" = "$(core_from)" ] && echo "  the core's paths: the report's lights are the window's (green, green); one kept set not ready over two rekordbox sessions, a step each (checked, kept.noticed, one notification, then unchanged and silent; the counts only when the watcher may notify); a queued install: Action interrupted (Continue), rekordbox open (Try Again), then it runs" ;;
    carried) echo "  fresh clone; $f by its bootstrap, Stems Cache [owner or Accessibility]; $relv served and its payload.json proven read: app $carried_v, no notice; relaunched: \"started: RB Stems Plus $relv (Mac app $carried_v\", About \"Version $relv\", the report's summary; offer: $( [ "$(changed "$f")" = 1 ] && echo expected || echo none)" ;;
    carried-update) echo "  fresh clone; $f by its bootstrap, Stems Cache; its notice for $carried_v; Update [owner or Accessibility]; the carried app $carried_v installed and checked; then the same display checks as carried (release $relv)" ;;
  esac
}

# ---- the record -----------------------------------------------------------------------------------

# guest_problems RECORD: what is wrong with its guests, nothing when they hold (macOS 26 only is
# tested: owner, 2026-11-12): at least one guest; guest_versions NAME=VER pairs naming exactly
# the guests, every version numeric and macOS 26 or later
guest_problems() {
  local -a gs gv; local pv nm ver mj okv=1 names=""
  read -ra gs <<< "$(sed -n 's/^guests //p' "$1")"
  read -ra gv <<< "$(sed -n 's/^guest_versions//p' "$1")"
  [ ${#gs[@]} -gt 0 ] || { echo " no guests;"; return; }
  for pv in ${gv[@]+"${gv[@]}"}; do
    nm="${pv%%=*}"; ver="${pv#*=}"
    if [[ "$pv" != *=* || ! "$ver" =~ ^[0-9]+(\.[0-9]+)*$ ]]; then okv=0; names+="$nm"$'\n'; continue; fi
    mj="$((10#${ver%%.*}))"; [ "$mj" -ge 26 ] || okv=0
    names+="$nm"$'\n'
  done
  [ "$(printf '%s' "$names" | sort)" = "$(printf '%s\n' "${gs[@]}" | sort -u)" ] || echo " guest_versions doesn't name exactly the guests;"
  [ $okv = 1 ] || echo " a guest's version isn't macOS 26 or later;"
}
check_record() {   # check_record COMMIT DIR
  local c="$1" d="${2:-$root/dist-test}" r="$GATES/$1/g3-mac.ok" p="$GATES/$1/plan"
  [ -f "$r" ] || { echo "g3.sh check: no G3 record $r: run tests/mac/g3.sh run on the TEST build of $c"; return 1; }
  local bad=""
  grep -qx "g3 1" "$r" || bad+=" format;"
  grep -qx "commit $c" "$r" || bad+=" another commit;"
  [ -f "$d/payload.json" ] && grep -qx "payload_sha256 $(sha "$d/payload.json")" "$r" || bad+=" another TEST payload than $d/payload.json;"
  [ -f "$p" ] && grep -qx "plan_sha256 $(sha "$p")" "$r" || bad+=" the plan changed since the run;"
  grep -qx "coverage ok" "$r" || bad+=" not every guest and scenario;"
  # every required scenario passed, recomputed from the plan and the archive (on the record's guests)
  if [ -f "$p" ]; then
    app_dec="$(awk '$1 == "mac.app" {print $2}' "$p")"; prev="$(awk '$1 == "previous" {print $2}' "$p")"
    relv="$(awk '$1 == "version" {print $2}' "$p")"
    local gl l miss=""; gl="$(sed -n 's/^guests //p' "$r")"
    while read -r l; do
      [ -n "$l" ] || continue
      set -- $l; has_scenario "$r" "$1" "$2" pass || miss+=" $l,"
    done <<< "$(required_scenarios "$gl")"
    [ -z "$miss" ] || bad+=" required scenarios not passed:${miss%,};"
  fi
  bad+="$(guest_problems "$r" | tr -d '\n')"
  grep -q "^stub" "$r" && bad+=" a selftest's record;"
  grep -q "^scenario .* FAIL" "$r" && bad+=" a scenario failed;"
  grep -qx "result pass" "$r" || bad+=" not passed;"
  [ -z "$bad" ] || { echo "g3.sh check: $r doesn't count:$bad"; return 1; }
  echo "G3 tart run: passed for $c ($(grep -c '^scenario ' "$r") scenarios; $(sed -n 's/^owner_clicks //p' "$r"))"
}

cmd="${1:-}"; [ $# -gt 0 ] && shift
case "$cmd" in
plan|run)
  parse "$@"; mkdir -p "$STATE"; resolve
  echo "G3 for $commit: release $relv, app $newv ($app_dec), bridge $bridge_dec, previous ${prev:-none}; TEST payload $payload_sha"
  echo "starting points: $froms; guests: $guests"
  list="$(scenarios)"
  [ -n "$list" ] || stop "no scenario matches --only $only"
  while read -r g n f; do echo "- $g: $n${f:+ from $f}"; describe "$g" "$n" "$f"; done <<< "$list"
  echo "about $(( $(wc -l <<< "$list") * 15 )) minutes, one VM at a time"
  if [ ${#missing[@]} -gt 0 ]; then
    echo "MISSING templates: ${missing[*]}: rb26-template is the macOS 26 template (DJ, rekordbox, STEMS, auto-login); never download an image without asking the owner"
    [ "$cmd" = plan ] && exit 1 || stop "templates missing"
  fi
  [ "$cmd" = plan ] && { echo "dry run: no VM touched"; exit 0; }
  vms; [ -z "$RUNNING" ] || stop "a VM is running ($RUNNING): one VM at a time; wait for that lane"
  build_driver
  [ -x "$STATE/vm/vmtool" ] || "$VM" build > /dev/null || stop "vm.sh build failed"
  stamp="$(date +%Y%m%d-%H%M%S)"; out="$GATES/$commit/g3"
  # an earlier record never outlives a new run, nor its interruption
  rm -f "$GATES/$commit/g3-mac.ok" "$GATES/$commit/g3-mac.failed"
  rm -rf "$out"; mkdir -p "$out"
  rec="$GATES/$commit/g3-mac.partial"
  failed_rec() { [ -f "$rec" ] && { echo "result FAIL ($1)" >> "$rec"; mv -f "$rec" "$GATES/$commit/g3-mac.failed"; }; true; }
  rm -rf "$ACTIVE"
  trap 'clone_down; failed_rec stopped; rm -rf "$ACTIVE"' EXIT
  trap 'kill_active; failed_rec interrupted; exit 130' INT TERM
  {
    echo "# tests/mac/g3.sh run, $(date '+%F %T') (release.md §6.1)"
    echo "g3 1"; echo "commit $commit"; echo "payload_sha256 $payload_sha"; echo "app_sha256 $(sha "$build/rbstemsplus-app.zip")"
    echo "plan_sha256 $plan_sha"; echo "plan mac.app $app_dec mac.bridge $bridge_dec previous ${prev:-none}"
    echo "version $relv (Mac app $newv)"; echo "from $froms"; echo "guests $guests"
    [ -n "${G3_VM:-}${G3_SERVE_STUB:-}${G3_TART:-}" ] && echo "stub (selftest)"
  } > "$rec"
  fails=0; all_clicks=(); majors=""; n=0
  while read -r g n_ f; do
    n=$((n + 1)); name="$n_${f:+-$f}"; RUNH="$XFER/g3-$stamp/$n-$g-$name"; GRUN="$VX/g3-$stamp/$n-$g-$name"
    mkdir -p "$RUNH"; : > "$RUNH/result"; clicks=(); t0=$SECONDS
    say "== $g: $name"
    case "$n_" in
      bootstrap) sc_bootstrap "$g" ;;
      update) sc_update "$g" "$f" ;;
      carried) sc_carried "$g" "$f" ;;
      carried-update) sc_carried_update "$g" "$f" ;;
    esac
    rc=$?
    m="$(sed -n 's/^macos //p' "$RUNH/facts.txt" 2>/dev/null)"; majors+=" $g=${m:-?}"
    { [ $rc != 0 ] || grep -q '^FAIL' "$RUNH/result"; } && keep_fail_logs
    clone_down || rc=1
    v=pass; { [ $rc != 0 ] || grep -q '^FAIL' "$RUNH/result"; } && { v=FAIL; fails=$((fails + 1)); }
    [ $rc != 0 ] && ! grep -q '^FAIL' "$RUNH/result" && echo "FAIL the scenario stopped (exit $rc)" >> "$RUNH/result"
    rm -rf "${out:?}/$n-$g-$name"; cp -R "$RUNH" "$out/"
    c="none"; [ ${#clicks[@]} -gt 0 ] && c="owner:$(IFS=,; echo "${clicks[*]}")"; all_clicks+=(${clicks[@]+"${clicks[@]}"})
    echo "scenario $g $name $v $(( (SECONDS - t0 + 59) / 60 ))min clicks=$c" >> "$rec"
    say "   $v ($(grep -c '^ok' "$RUNH/result") ok, $(grep -c '^FAIL' "$RUNH/result") FAIL)$( [ $v = FAIL ] && grep '^FAIL' "$RUNH/result" | sed 's/^/     /')"
    if [ $ABORT != 0 ]; then
      echo "stopped after $g $name: no other VM started (a VM running, a clone not deleted, or another lane's pointer)" >> "$rec"
      say "the run stops after $g $name (see its result)"; break
    fi
  done <<< "$list"
  # coverage: every required scenario (no --only, no --from leaving one out, none skipped by a stop),
  # every guest on macOS 26 or later (guest_problems)
  echo "guest_versions$(tr ' ' '\n' <<< "$majors" | sort -u | tr '\n' ' ' | sed 's/ *$//')" >> "$rec"
  cov=ok; notrun=""; omitted=""
  while read -r rg rn; do
    has_scenario "$rec" "$rg" "$rn" || notrun+=" $rg $rn,"
  done <<< "$(required_scenarios "$guests")"
  for f in $required; do [[ " $froms " == *" $f "* ]] || omitted+=" $f"; done
  if [ -n "$only" ]; then cov="partial (--only)"
  elif [ -n "$omitted" ]; then cov="partial (--from omits$omitted)"
  elif [ -n "$notrun" ]; then cov="partial (not run:${notrun%,})"; fi
  gp="$(guest_problems "$rec" | tr -d '\n')"; [ -z "$gp" ] || cov="the guests:$gp"
  {
    echo "owner_clicks ${#all_clicks[@]}$( [ ${#all_clicks[@]} -gt 0 ] && printf ' (%s)' "$(IFS=';'; echo "${all_clicks[*]}")")"
    echo "emulated App Management's Allow (DJ's TCC row), the password (sudo without one), the welcome sheet (seen)"
    echo "coverage $cov"
    if [ $fails = 0 ] && [ "$cov" = ok ]; then echo "result pass"; else echo "result FAIL ($fails scenarios failed; coverage $cov)"; fi
  } >> "$rec"
  if grep -qx "result pass" "$rec"; then mv "$rec" "$GATES/$commit/g3-mac.ok"; say "G3: passed: $GATES/$commit/g3-mac.ok"; exit 0; fi
  mv "$rec" "$GATES/$commit/g3-mac.failed"; say "G3: FAILED: $GATES/$commit/g3-mac.failed ($out)"; exit 1 ;;
check)
  [ $# -ge 1 ] && [[ "$1" =~ ^[0-9a-f]{40}$ ]] || usage
  check_record "$1" "${2:-}" ;;
smoke)
  g="${1:-rb26-template}"; mkdir -p "$STATE"
  vms; [ -z "$RUNNING" ] || stop "a VM is running ($RUNNING): one VM at a time"
  vm_session; build_driver
  RUNH="$XFER/g3-smoke-$(date +%s)"; GRUN="$VX/${RUNH##*/}"; mkdir -p "$RUNH"; : > "$RUNH/result"
  clone_up "$g" && if [ "$AX" = yes ]; then gst dialog; else echo "note dialog skipped: no Accessibility for the G3 Driver (the old apps' clicks are the owner's)" >> "$RUNH/result"; fi; rc=$?
  clone_down || rc=1
  cat "$RUNH/result"
  cp "$RUNH/result" "$STATE/g3-smoke.txt"
  [ $rc = 0 ] && grep -q "^macos " "$RUNH/facts.txt" ;;
post-publish)
  v="${1:-}"; [[ "$v" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || usage
  g="${2:-rb26-template}"; mkdir -p "$STATE"
  vms; [ -z "$RUNNING" ] || stop "a VM is running ($RUNNING): one VM at a time"
  vm_session; build_driver
  # the published payload.json, as the guest fetches it: its app.version is what the notice names,
  # the update installs and the one-liner installs (a carried app's, when V carries one)
  if [ -n "${G3_POST_PAYLOAD:-}" ]; then cp "$G3_POST_PAYLOAD" "$STATE/g3-post-payload.json"      # the selftest's
  else curl -fsSL --max-time 60 -o "$STATE/g3-post-payload.json" "$GITHUB/download/v$v/payload.json" || stop "couldn't read the published v$v payload.json"; fi
  [ "$(pj "$STATE/g3-post-payload.json" payload_version)" = "$v" ] || stop "the published v$v payload.json says release $(pj "$STATE/g3-post-payload.json" payload_version)"
  pubapp="$(pj "$STATE/g3-post-payload.json" app.version)"
  rec="$GATES/post-$v.txt"; stamp="$(date +%Y%m%d-%H%M%S)"; : > "$rec.partial"; rc=0
  # 1. the real published 1.3.0 (its own bootstrap and zip from the v1.3.0 release) updates in-app
  # from the real releases/latest (a release build: no probe; App Management and sudo emulated)
  RUNH="$XFER/g3-$stamp/post-1.3.0"; GRUN="$VX/g3-$stamp/post-1.3.0"; mkdir -p "$RUNH"; : > "$RUNH/result"
  { clone_up "$g" clicks && gst prepare && gst bootstrap "$GITHUB/download/v1.3.0/" 1.3.0 && gst emulate \
    && gst switch && wait_cond 120 notice "$pubapp" \
    && click_step "the published 1.3.0: Update" "$(update_what v1.3.0)" "version $pubapp" ax_update; } || rc=1
  clone_down || rc=1
  # 2. the real one-liner (no override) on a clean clone
  RUNH="$XFER/g3-$stamp/post-one-liner"; GRUN="$VX/g3-$stamp/post-one-liner"; mkdir -p "$RUNH"; : > "$RUNH/result"
  if [ $ABORT = 0 ]; then { clone_up "$g" && gst prepare && gst bootstrap real "$pubapp"; } || rc=1; clone_down || rc=1; fi
  cat "$XFER/g3-$stamp"/post-*/result > "$rec.partial"
  grep -q '^FAIL' "$rec.partial" && rc=1
  echo "result $( [ $rc = 0 ] && echo pass || echo FAIL)" >> "$rec.partial"; mv "$rec.partial" "$rec"
  say "post-publish checks of $v: $( [ $rc = 0 ] && echo passed || echo FAILED): $rec"; exit $rc ;;
build) mkdir -p "$STATE"; build_driver; echo "built: $DRIVER" ;;
selftest) exec "$here/tests/mac/g3-test.sh" ;;
_t)   # the selftests' hook: _t bounded SECS CMD… (its output captured, as gst's is) | _t wait_cond
  # SECS NAME… | _t required COMMIT GUESTS (the required scenario lines of COMMIT's plan: the
  # release tests write records with it)
  if [ "${1:-}" = required ]; then
    [ -f "$GATES/${2:-}/plan" ] || stop "no plan for ${2:-}"
    app_dec="$(awk '$1 == "mac.app" {print $2}' "$GATES/$2/plan")"; prev="$(awk '$1 == "previous" {print $2}' "$GATES/$2/plan")"
    relv="$(awk '$1 == "version" {print $2}' "$GATES/$2/plan")"
    required_scenarios "${3:-}"; exit 0
  fi
  mkdir -p "$STATE"; GRUN=/Users/admin/xfer/selftest; RUNH="$STATE/selftest-run"; mkdir -p "$RUNH"
  clone=g3-selftest; echo "$clone" > "$STATE/clone"
  case "${1:-}" in
    bounded) shift; o="$(bounded "$@")"; rc=$?; echo "rc=$rc out=$o" ;;
    wait_cond) shift; wait_cond "$@"; echo "rc=$? abort=$ABORT" ;;
    *) usage ;;
  esac ;;
*) usage ;;
esac

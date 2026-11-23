#!/bin/bash
# The bridge's performance numbers P-B1-P-B4 and P-B6-P-B8 (bridge.md §0b), and the gate GP: the
# harness's bench group on Pioneer's real ONNX Runtime 1.18, built at rekordbox's chunk size
# (-DL=485100), against bridges built with the timing lines (bridge/build.sh --test-timing).
# Built on fs/perfgate.sh: the same dedicated-drive rules and its cold method (the drive
# unmounted and mounted again), plus purge. P-B5 (rekordbox's stems load time) runs in tart.
#
#   bridge/tests/perf.sh build SIDE
#       a "side": the working tree's bridge with the timing lines, built into the folder SIDE
#       (SIDE/bridge/, SIDE/ort/ a read-only copy of Pioneer's library that the bridge loads by
#       that absolute path: a side runs only where it was built; SIDE/IDENTITY)
#   bridge/tests/perf.sh record SIDE OUT [--volume VOL] [--set NAME] [--quick] [--image] [--no-purge]
#       one run set of SIDE into OUT: samples.tsv (appended), meta-NAME.txt, logs-NAME/
#   bridge/tests/perf.sh gate A B OUT [--volume VOL] [--idle-wait SECS] [--results FILE] [--dry-run]
#                                [--quick] [--image] [--no-purge] [--no-preflight]
#       GP in one short exclusive session (bridge.md §0b; owner decided, 2026-11-12): two
#       alternating pairs (A B, then B A), one run set per side each, then A against B over every
#       set (tests/perf/perfstats.py gate, tests/perf/thresholds.txt): a slowdown counts only when
#       it is over both 10% and 20 ms (20 MB for memory); every other number is information; a
#       borderline row is printed for a human to judge. The idle guard (tests/perf/idle.sh): the
#       first set waits until the Mac has had no input for --idle-wait SECS (600; 0: off, the
#       default with --quick); while input continues, the next set waits for 5 idle minutes. A
#       set during which input came runs again once per session (disturbed_retried=); a second
#       one (disturbed=) or a set that came out short ends the session: the measurement failed
#       (3); an idle time that can't be read stops it (2). OUT/session.txt
#       holds every step (idle_at=, dur=, done=, verdict=, finished=). --dry-run prints the plan
#       and runs nothing. --results FILE appends the report to FILE.
#       Exit 0 pass, 1 a regression, 3 the measurement failed, 2 stopped. A gate whose A isn't
#       the gate archive's B0 (GATES_ARCHIVE) is information only: never a pass (3). With
#       --volume, before the idle wait, the drive's pre-flight (below) with the session's cycles
#       (4 sets x P-B1.cold's count: 60, about 4 min; --no-preflight when it was just run on its
#       own). The rehearsal: --quick --idle-wait 0 without --volume runs the whole session (both
#       sides, four sets, the verdict) with short counts, the drive's metrics left out of the
#       required set; never a pass (3, "tooling check only").
#   bridge/tests/perf.sh preflight --volume VOL [--cycles N] [--quick] [--image] [--no-purge]
#       the drive's pre-flight: N cycles (default a gate's: 60, --quick 8) of what a cold sample
#       does to it (unmount, mount again, purge), each writing a file on VOL and reading it back
#       after the next remount: a drive that drops out fails here, not mid-session. Exit 0 or 2
#   bridge/tests/perf.sh selftest
#       tiny runs of build, record and gate in a scratch folder (no drive): the tooling works
#   BRIDGE_PERF_TEST=1 (tests/harness/selftest.sh only): BRIDGE_PERF_BENCH replaces a run set
#   (called with SIDE_DIR SIDE ROUND SAMPLES META), PERF_IDLE_CMD the idle reader; a real gate
#   refuses them.
#
# VOL is the dedicated drive's mount point (never the owner's music drive, never this Mac's
# disk), with an empty file RBSP-PERF-SCRATCH at its root, as fs/perfgate.sh asks. Without
# --volume, P-B1's cold and drive numbers are left out (and the gate says so: a missing metric is
# a measurement failure). --image accepts a disk image as VOL: for testing this script only; its
# "cold" reads stay in the host's page cache, so the gate refuses such samples.
# Cold samples need purge, which needs root: a sudoers line for /usr/sbin/purge (NOPASSWD), or
# --no-purge (smoke runs only: the record says so). Rules while measuring (bridge.md §0b): AC
# power, Low Power Mode off, nothing else running. Outside a sandbox (it remounts the drive).
#
# What each metric is (samples in ns unless said):
#   (near hits off, as installed, except for P-B2; max_gb and max_days at their highest: no eviction)
#   P-B1.warm         100 hits, this Mac's folder, warm (the bridge's own time in Run)
#   P-B1.warm-drive   100 hits on the drive (a chosen location), warm
#   P-B1.cold         15 hits on the drive, each in a new process after a remount and purge
#   P-B2.near2k/20k   50 near hits each, an index of 2,000 and of 20,000 summaries
#   P-B3.miss         50 misses, our kind of model: Run's time minus the real Run's (each miss
#                     after the writer stored the one before: none dropped by its 4-job queue)
#   P-B3.miss-rebuilt 50 misses, a model shaped like rekordbox's own (stems rebuilt), paced alike
#   P-B4.store        each miss's store_entry (the writer queue); P-B4.index20k: load_index
#                     of 20,000 summaries, 3 runs; P-B4.prune10k: the limits' walk of 10,000
#                     entries, 3 runs
#   P-B6.start        10 starts: OrtGetApiBase's first call, its dlopen of the real library left out
#                     (P-B6.start-outer: the harness's own clock, OrtGetApiBase + GetApi; for information)
#   P-B7.rss1000      the harness's peak memory over 1,000 hits (bytes); P-B7.size: the dylib (bytes)
#   P-B8.memo/nomemo  hook_CreateSession's own time (the real CreateSession subtracted), 10
#                     sessions with the model's checksum memo, 10 without; the model is Pioneer's
#                     (~/Library/Application Support/rbstemsplus/originals/<sha>.onnx, or
#                     RBSP_PERF_B8_MODEL), else the tiny test model (the record says which). In the
#                     memo run every session after the first must say memo=memo (no rehash).
set -u
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
LIB=libonnxruntime.1.18.0.dylib
TEAM=6BRHGXQ6VU
ID=0F8C2D3A-1B2C-4D5E-8F90-1A2B3C4D5E6F
# the gate's set: every one must be measured on both sides (P-B5 runs in tart, separately)
REQUIRED=P-B1.warm,P-B1.warm-drive,P-B1.cold,P-B2.near2k,P-B2.near20k,P-B3.miss,P-B3.miss-rebuilt,P-B4.store,P-B4.index20k,P-B4.prune10k,P-B6.start,P-B7.rss1000,P-B7.size,P-B8.memo,P-B8.nomemo
die() { echo "perf.sh: $*" >&2; exit 2; }
say() { echo "[$(date +%H:%M:%S)] $*"; }
# shellcheck source=../../tests/perf/idle.sh disable=SC1091
. "$root/tests/perf/idle.sh"
idle=""

# ---------------------------------------------------------------- build

real_ort() {   # Pioneer's library, found as run.sh finds it
  local c
  local cands=("${RBSTEMS_REAL_ORT:-}" "/Library/Application Support/StemsPlus/ort/$LIB" "/Library/Application Support/rbstemsplus/ort/$LIB")
  for c in "$HOME"/Library/Pioneer/rekordbox-stems-backups/*/rekordbox.app/Contents/Frameworks/$LIB; do cands+=("$c"); done
  for c in "${cands[@]}"; do
    [ -n "$c" ] && [ -f "$c" ] || continue
    grep -aq "rbstems bridge: " "$c" && continue
    codesign -dv "$c" 2>&1 | grep -q "TeamIdentifier=$TEAM" || continue
    echo "$c"; return 0
  done
  return 1
}

cmd_build() {
  [ $# -eq 1 ] || die "usage: perf.sh build SIDE"
  mkdir -p "$1" || die "can't make $1"
  local side src; side="$(cd "$1" && pwd -P)"
  case "$side" in *'"'*|*'\'*) die "SIDE can't contain \" or \\" ;; esac
  src="$(real_ort)" || die "no copy of Pioneer's ONNX Runtime 1.18 (install Stems Cache, or set RBSTEMS_REAL_ORT)"
  rm -rf "$side/bridge" "$side/ort"; mkdir -p "$side/ort"
  cp "$src" "$side/ort/$LIB" && chmod 444 "$side/ort/$LIB" && chmod 755 "$side" "$side/ort" || die "copying $src"
  # RBSP_PERF_SRC: another source tree to build (tests/perf/b0.sh: B0's), RBSP_PERF_SRC_DESC says which
  "${RBSP_PERF_SRC:-$root}/bridge/build.sh" --test-timing "$side/ort/$LIB" "$side/bridge" > "$side/build.log" 2>&1 || { tail "$side/build.log"; die "the timing bridge didn't build"; }
  {
    echo "built: $(date -u +%Y-%m-%dT%H:%M:%SZ) on $(sysctl -n hw.model), macOS $(sw_vers -productVersion) ($(sw_vers -buildVersion))"
    echo "source: ${RBSP_PERF_SRC_DESC:-$(git -C "$root" rev-parse HEAD 2>/dev/null || echo "?")$(git -C "$root" status --porcelain -- bridge/rbstems_bridge.c bridge/build.sh bridge/onnxruntime_c_api.h common 2>/dev/null | grep -q . && echo " (the bridge's source has uncommitted changes)")}"
    echo "toolchain: $(clang --version | head -1); SDK $(xcrun --show-sdk-version 2>/dev/null)"
    echo "real library: a copy of $src"
    (cd "$side" && shasum -a 256 "bridge/$LIB" "ort/$LIB")
  } > "$side/IDENTITY"
  say "built $side ($(sed -n 2p "$side/IDENTITY"))"
}

# ---------------------------------------------------------------- the session (models, harnesses)

S= VOL= QUICK= IMAGE= NOPURGE= OUT= SAMPLES= dev= SHORT= SET= META= LOGS=
RESULTS= IDLE_WAIT= DRY= OPTS_GIVEN= NOPREFLIGHT= PF_AT=
setup_session() {
  S="$(cd "$(mktemp -d "${TMPDIR:-/tmp}/rbsp-bperf.XXXXXX")" && pwd -P)"
  trap 'rm -rf "$S"; vol_cleanup' EXIT     # dev is empty until check_volume: no drive, nothing to do
  mkdir -p "$S/model" "$S/pmodel" "$S/b8"
  python3 -I "$here/make_model.py" "$S/model/hdemucs.onnx" || die "make_model.py"
  python3 -I "$here/make_model.py" --pioneer 0.64 -0.64 0.32 -0.32 "$S/pmodel/hdemucs.onnx" || die "make_model.py --pioneer"
  local orig="${RBSP_PERF_B8_MODEL:-$HOME/Library/Application Support/rbstemsplus/originals/435c987855f7ea74ff5f20090541748cbbce5a2e8e501d07b10f4e703453215a.onnx}"
  B8KIND="Pioneer's model ($(basename "$orig"))"
  if [ -z "$QUICK" ] && [ -f "$orig" ]; then cp -c "$orig" "$S/b8/hdemucs.onnx" 2> /dev/null || cp "$orig" "$S/b8/hdemucs.onnx" || die "copying $orig"
  else cp "$S/model/hdemucs.onnx" "$S/b8/hdemucs.onnx"; B8KIND="the tiny test model"; fi
  if [ "$QUICK" ]; then
    # more misses than the writer queue's 4 jobs, two index loads and prunes: the full form's paths
    N_WARM=5 N_COLD=2 N_NEAR=4 N_MISS=6 N_IDX=2 N_PRUNE=2 N_START=3 N_RSS=10 N_SESS=2 IDX_A=50 IDX_B=200 PRUNE_N=100
  else
    # half the former counts (owner decided, 2026-11-12: four sets in about 20-30 min)
    N_WARM=100 N_COLD=15 N_NEAR=50 N_MISS=50 N_IDX=3 N_PRUNE=3 N_START=10 N_RSS=1000 N_SESS=10 IDX_A=2000 IDX_B=20000 PRUNE_N=10000
  fi
}

harness_for() {   # SIDE NAME: the harness linked against SIDE's bridge, at rekordbox's chunk size
  clang -arch arm64 -mmacosx-version-min=12.0 -O1 -Wall -Wextra -DL=485100 -I"$root/bridge" "$here/harness.c" \
    "$1/bridge/$LIB" -Wl,-rpath,"$1/bridge" -o "$S/harness-$2" || die "the harness didn't build"
}

check_side() {
  [ -f "$1/bridge/$LIB" ] && [ -f "$1/ort/$LIB" ] || die "$1 isn't a side (perf.sh build $1)"
  strings "$1/bridge/$LIB" | grep -q "timing run=" || die "$1/bridge has no timing lines"
  (cd "$1" && shasum -a 256 -c --quiet <(sed -n '/  bridge\//p; /  ort\//p' IDENTITY)) || die "$1 doesn't match its IDENTITY"
}

# ---------------------------------------------------------------- the dedicated drive (fs/perfgate.sh's rules)

check_volume() {
  [ -d "$VOL" ] || die "no volume $VOL"
  VOL="$(cd "$VOL" && pwd -P)"
  [ "$(stat -f %d "$VOL")" != "$(stat -f %d /)" ] && [ "$(stat -f %d "$VOL")" != "$(stat -f %d "$HOME")" ] || die "$VOL is on this Mac's own disk"
  [ "$(basename "$VOL")" != GLS ] || die "never on GLS (the owner's music drive)"
  [ -f "$VOL/RBSP-PERF-SCRATCH" ] || die "no $VOL/RBSP-PERF-SCRATCH: create it to show this drive may be used"
  dev="$(diskutil info "$VOL" | awk -F': *' '/Device Identifier/ {print $2; exit}')"
  [ -n "$dev" ] || die "diskutil doesn't know $VOL"
  if diskutil info "$VOL" | grep -q 'Protocol: *Disk Image'; then
    [ "$IMAGE" ] || die "$VOL is a disk image: its cold reads stay in the page cache (--image only to test this script)"
  fi
}

remount() {   # fs/perfgate.sh's
  { diskutil unmount "$VOL" > /dev/null 2>&1 || diskutil unmount force "$VOL" > /dev/null; } || die "couldn't unmount $VOL${PF_AT:+ (pre-flight, $PF_AT)}"
  local i
  for i in 1 2 3 4 5; do
    sleep 3
    diskutil mount "$dev" > /dev/null 2>&1 && [ -d "$VOL" ] && return 0
  done
  die "couldn't mount $VOL again${PF_AT:+ (pre-flight, $PF_AT)} (diskutil list: is the drive still there?)"
}

vol_mounted() { [ -n "$dev" ] && [ "$(diskutil info "$dev" 2> /dev/null | awk -F': *' '/Mount Point/ {print $2; exit}')" = "$VOL" ]; }
vol_cleanup() {   # EXIT: a remount cut short is completed, then the scratch folder removed
  [ -n "$VOL" ] && [ -n "$dev" ] || return 0
  vol_mounted || { diskutil mount "$dev" > /dev/null 2>&1; sleep 1; }
  if vol_mounted; then rm -rf "$VOL/rbsp-perf"
  else echo "perf.sh: $VOL is still unmounted (interrupted mid-remount): diskutil mount $dev" >&2; fi
}

PURGE=
check_purge() {
  if [ "$NOPURGE" ]; then PURGE="none (--no-purge: not a valid cold number)"; return; fi
  if sudo -n /usr/sbin/purge 2> /dev/null; then PURGE="sudo -n /usr/sbin/purge"; return; fi
  die "purge needs root without a password prompt: add \"$(id -un) ALL=(root) NOPASSWD: /usr/sbin/purge\" with sudo visudo -f /etc/sudoers.d/rbsp-purge (or --no-purge, smoke runs only)"
}
do_purge() { [ "$NOPURGE" ] || sudo -n /usr/sbin/purge || die "purge failed"; }

# the drive's pre-flight: CYCLES remounts and purges, a file written before each and read back
# after it (remount and do_purge die with their own message, said with the cycle reached)
vol_preflight() {   # CYCLES
  local i f="$VOL/rbsp-perf/preflight" want got
  mkdir -p "$VOL/rbsp-perf" || die "pre-flight: can't write on $VOL"
  say "pre-flight: $1 remount and purge cycles of $VOL (about $(( $1 * 4 / 60 + 1 )) min)"
  for i in $(seq "$1"); do
    want="rbsp pre-flight $$ $i"; printf '%s\n' "$want" > "$f" || die "pre-flight: can't write on $VOL (cycle $i of $1)"
    PF_AT="cycle $i of $1"; remount; do_purge; PF_AT=
    got="$(cat "$f" 2> /dev/null)"; [ "$got" = "$want" ] || die "pre-flight: $VOL lost its file over a remount (cycle $i of $1)"
    [ $((i % 10)) != 0 ] || say "pre-flight: $i of $1 cycles"
  done
  rm -f "$f"; rmdir "$VOL/rbsp-perf" 2> /dev/null
  say "pre-flight: $VOL came back $1 times out of $1"
}

cmd_preflight() {
  local cycles=
  while [ $# -gt 0 ]; do
    case "$1" in
      --cycles) cycles="${2:-}"; shift ;;
      --volume) VOL="${2:-}"; shift ;;
      --quick) QUICK=1 ;;
      --image) IMAGE=1 ;;
      --no-purge) NOPURGE=1 ;;
      *) die "unknown option $1" ;;
    esac
    shift
  done
  [ -n "$VOL" ] || die "usage: perf.sh preflight --volume VOL [--cycles N] [--quick] [--image] [--no-purge]"
  [ -n "$cycles" ] || cycles=$((4 * $([ -n "$QUICK" ] && echo 2 || echo 15)))   # a gate's: 4 sets x N_COLD
  case "$cycles" in ''|*[!0-9]*|0) die "--cycles N: a number over 0" ;; esac
  check_volume; check_purge
  trap vol_cleanup EXIT; trap 'exit 2' INT TERM HUP
  vol_preflight "$cycles"
}

preflight() {
  local batt lpm
  batt="$(pmset -g batt | head -1)"
  lpm="$(pmset -g | awk '$1 == "lowpowermode" || $1 == "powermode" {print $2; exit}')"   # 1: Low Power Mode
  if [ -z "$QUICK" ]; then
    [[ "$batt" == *"AC Power"* ]] || die "not on AC power ($batt)"
    [ "${lpm:-0}" != 1 ] || die "Low Power Mode is on"
    [ "$(df -k "$S" | awk 'NR==2 {print $4}')" -gt $((10 * 1024 * 1024)) ] || die "under 10 GB free for the scratch folder"
  fi
  POWER="$batt; powermode=${lpm:-?} (1: Low Power Mode)"
}

# ---------------------------------------------------------------- one side, one round

emit() {
  case "$2" in ''|*[!0-9]*) echo "  WARNING $1: no number (\"$2\")" | tee -a "$META"; return ;; esac
  printf '%s\t%s\t%s\t%s\n' "$1" "$SIDE_NAME" "$ROUND" "$2" >> "$SAMPLES"
}
# the timing lines of one kind from a log: field own_ns (or total_ns), one per line
field_of() { grep -F -- "$2" "$1" | sed -E "s/.* $3=([0-9]+).*/\1/"; }
emit_all() { local v n=0; while read -r v; do [ -n "$v" ] && { emit "$1" "$v"; n=$((n + 1)); }; done; echo "$n"; }
# a short workload is a measurement failure for the gate (SHORT, and the WARNING in meta)
want() { [ "$1" -ge "$2" ] || { SHORT=1; echo "  WARNING $3: $1 samples, expected $2" | tee -a "$META"; }; }

bench_round() {   # SIDE SIDE_NAME ROUND
  local side="$1" t="$S/r-$2-$3" n
  SIDE_NAME="$2" ROUND="$3"
  local h="$t/home" hn hp hd
  local hx=(env HOME="$h" RBSTEMS_TEST_HOME="$h")
  mkdir -p "$h/Library/Application Support/rbstemsplus" "$h/Library/Logs/rbstemsplus"
  # near hits off, as installed (the default): P-B2 turns them on in its own homes
  printf 'rekordbox_model=1\nmax_gb=100000\nmax_days=36500\n' > "$h/Library/Application Support/rbstemsplus/config.ini"
  local logf="$h/Library/Logs/rbstemsplus/bridge.log" H="$S/harness-$2" M="$S/model/hdemucs.onnx" PM="$S/pmodel/hdemucs.onnx"
  local lg="$LOGS/$2-$3"; mkdir -p "$lg"
  step() { : > "$logf"; }
  keep() { cp "$logf" "$lg/$1.log"; }
  say "$2 $3: start x$N_START"
  step; : > "$t.start"
  for n in $(seq "$N_START"); do "${hx[@]}" "$H" bench start >> "$t.start" || die "bench start failed: $(tail -1 "$t.start")"; done
  keep start
  n=$(field_of "$logf" "timing start " own_ns | emit_all P-B6.start); want "$n" "$N_START" P-B6.start
  sed -E 's/.*outer_ns=([0-9]+).*/\1/' "$t.start" | emit_all P-B6.start-outer > /dev/null

  say "$2 $3: warm hits x$N_WARM (this Mac's folder)"
  step; "${hx[@]}" "$H" bench run "$M" a wait "a*$N_WARM" > "$t.out" 2>&1; keep warm
  grep -q "stored 1" "$t.out" || die "$2 $3: the first entry wasn't stored: $(cat "$t.out")"
  n=$(field_of "$logf" "timing run=hit " own_ns | emit_all P-B1.warm); want "$n" "$N_WARM" P-B1.warm

  say "$2 $3: peak memory over $N_RSS hits"
  step; /usr/bin/time -l "${hx[@]}" "$H" bench run "$M" "a*$N_RSS" > "$t.out" 2> "$t.rss"
  emit P-B7.rss1000 "$(awk '/maximum resident set size/ {print $1}' "$t.rss")"
  emit P-B7.size "$(stat -f %z "$side/bridge/$LIB")"

  # each miss waits for the writer to store the one before ("m*1 drain"): the writer queue holds
  # 4 jobs and drops the rest (both sides), so back to back about half would go unstored and
  # skip the store's copy, as many as the writer's speed decides; paced, every miss does the same
  # work on both sides, as in rekordbox, whose own Run takes far longer than a store
  local paced=() k
  for k in $(seq "$N_MISS"); do paced+=("m*1" drain); done
  # a paced run that failed (a miss not stored in time: FAIL drain) makes the set short
  paced_fail() { SHORT=1; echo "  WARNING $2 ($1): the paced misses failed: $(grep -m1 '^FAIL' "$t.out" || tail -1 "$t.out")" | tee -a "$META"; }
  say "$2 $3: misses x$N_MISS (our kind of model), then rebuilt"
  step; "${hx[@]}" "$H" bench run "$M" mseed=1000 "${paced[@]}" wait sleep sleep > "$t.out" 2>&1 || paced_fail "$2 $3" P-B3.miss; keep miss
  n=$(field_of "$logf" "timing run=miss " own_ns | emit_all P-B3.miss); want "$n" "$N_MISS" P-B3.miss
  n=$(field_of "$logf" "timing store pcm=1 " total_ns | emit_all P-B4.store); want "$n" "$N_MISS" P-B4.store
  step; "${hx[@]}" RBSTEMS_TEST_PIONEER_MODEL="$(shasum -a 256 "$PM" | cut -d' ' -f1)" "$H" bench prun "$PM" w=0.64,-0.64,0.32,-0.32 mseed=2000 "${paced[@]}" wait > "$t.out" 2>&1 || paced_fail "$2 $3" P-B3.miss-rebuilt; keep miss-rebuilt
  n=$(field_of "$logf" "timing run=miss " own_ns | emit_all P-B3.miss-rebuilt); want "$n" "$N_MISS" P-B3.miss-rebuilt

  # near hits, the index loaded and the first prune done before the first one
  local msha size label ka f
  msha="$(shasum -a 256 "$M" | cut -d' ' -f1)"
  ka="$(grep -o 'hit  [0-9a-f]*' "$lg/warm.log" | head -1 | cut -c6-)"     # a's entry (12 digits: a prefix)
  [ -n "$ka" ] || die "$2 $3: no hit in the warm run"
  for size in "$IDX_A" "$IDX_B"; do
    label=$([ "$size" = "$IDX_A" ] && echo near2k || echo near20k)
    hn="$t/home-$label"; mkdir -p "$hn/Library/Logs/rbstemsplus"
    mkdir -p "$hn/Library/Application Support/rbstemsplus"
    printf 'near_hits=1\nmax_gb=100000\nmax_days=36500\n' > "$hn/Library/Application Support/rbstemsplus/config.ini"
    mkdir -p "$hn/Library/Caches/rbstemsplus/$msha"
    cp -cR "$S/distract-$size/" "$hn/Library/Caches/rbstemsplus/$msha/" || die "cloning the summaries"
    for f in "$h/Library/Caches/rbstemsplus/$msha"/${ka:0:2}/${ka}*; do
      mkdir -p "$hn/Library/Caches/rbstemsplus/$msha/${ka:0:2}"; cp "$f" "$hn/Library/Caches/rbstemsplus/$msha/${ka:0:2}/"
    done
    say "$2 $3: near hits x$N_NEAR, $size summaries"
    : > "$hn/Library/Logs/rbstemsplus/bridge.log"
    env HOME="$hn" RBSTEMS_TEST_HOME="$hn" RBSTEMS_HARNESS_WAIT_FOR="limits:" RBSTEMS_HARNESS_READY=600 "$H" bench run "$M" "b*$N_NEAR" > "$t.out" 2>&1 \
      || die "$2 $3: the near hits ($label) failed: $(tail -2 "$t.out")"
    cp "$hn/Library/Logs/rbstemsplus/bridge.log" "$lg/$label.log"
    n=$(field_of "$lg/$label.log" "timing run=near " own_ns | emit_all "P-B2.$label"); want "$n" "$N_NEAR" "P-B2.$label"
    if [ "$size" = "$IDX_B" ]; then
      say "$2 $3: load_index of $size summaries x$N_IDX"
      # a fresh log for each process: the harness waits for "limits:" anywhere in the log, so
      # in a shared one the later runs would see the first one's and quit before their own load
      : > "$lg/index.log"
      for n in $(seq "$N_IDX"); do
        : > "$hn/Library/Logs/rbstemsplus/bridge.log"
        env HOME="$hn" RBSTEMS_TEST_HOME="$hn" RBSTEMS_HARNESS_WAIT_FOR="limits:" RBSTEMS_HARNESS_READY=600 "$H" bench run "$M" > "$t.out" 2>&1 \
          || die "$2 $3: the index load failed: $(tail -2 "$t.out")"
        cat "$hn/Library/Logs/rbstemsplus/bridge.log" >> "$lg/index.log"
      done
      n=$(field_of "$lg/index.log" "timing index " total_ns | emit_all P-B4.index20k); want "$n" "$N_IDX" P-B4.index20k
    fi
    rm -rf "$hn"
  done

  say "$2 $3: prune walk of $PRUNE_N entries x$N_PRUNE"
  hp="$t/home-prune"; mkdir -p "$hp/Library/Logs/rbstemsplus" "$hp/Library/Caches/rbstemsplus/$msha"
  printf 'near_hits=0\nmax_gb=100000\nmax_days=36500\n' > "$t.cfg"; mkdir -p "$hp/Library/Application Support/rbstemsplus"; cp "$t.cfg" "$hp/Library/Application Support/rbstemsplus/config.ini"
  cp -cR "$S/distract-$PRUNE_N/" "$hp/Library/Caches/rbstemsplus/$msha/" || die "cloning the entries"
  # a fresh log for each process (as the index's), each waiting for its own prune's timing line
  # (logged just after "limits:")
  : > "$lg/prune.log"
  for n in $(seq "$N_PRUNE"); do
    : > "$hp/Library/Logs/rbstemsplus/bridge.log"
    env HOME="$hp" RBSTEMS_TEST_HOME="$hp" RBSTEMS_HARNESS_WAIT_FOR="timing prune complete=" RBSTEMS_HARNESS_READY=600 "$H" bench run "$M" > "$t.out" 2>&1 \
      || die "$2 $3: the prune run failed: $(tail -2 "$t.out")"
    cat "$hp/Library/Logs/rbstemsplus/bridge.log" >> "$lg/prune.log"
  done
  n=$(field_of "$lg/prune.log" "timing prune complete=1 " total_ns | emit_all P-B4.prune10k); want "$n" "$N_PRUNE" P-B4.prune10k
  rm -rf "$hp"

  say "$2 $3: sessions x$N_SESS with the checksum memo, x$N_SESS without ($B8KIND)"
  hd="$t/home-b8"; mkdir -p "$hd/Library/Logs/rbstemsplus" "$hd/Library/Application Support/rbstemsplus"
  cp "$t.cfg" "$hd/Library/Application Support/rbstemsplus/config.ini"
  local hb=(env HOME="$hd" RBSTEMS_TEST_HOME="$hd")
  "${hb[@]}" "$H" bench sessions "$S/b8/hdemucs.onnx" 1 > /dev/null 2>&1            # the memo written
  : > "$hd/Library/Logs/rbstemsplus/bridge.log"
  "${hb[@]}" "$H" bench sessions "$S/b8/hdemucs.onnx" "$N_SESS" > "$t.out" 2>&1 || die "sessions: $(tail -2 "$t.out")"
  cp "$hd/Library/Logs/rbstemsplus/bridge.log" "$lg/sessions-memo.log"
  [ "$(grep -c 'timing session memo=hashed' "$lg/sessions-memo.log")" = 0 ] || die "$2 $3: a session hashed the model again with its memo present (P-B8's no-rehash rule)"
  n=$(field_of "$lg/sessions-memo.log" "timing session memo=memo " own_ns | emit_all P-B8.memo); want "$n" "$N_SESS" P-B8.memo
  : > "$hd/Library/Logs/rbstemsplus/bridge.log"
  "${hb[@]}" "$H" bench sessions "$S/b8/hdemucs.onnx" "$N_SESS" nomemo > "$t.out" 2>&1 || die "sessions nomemo: $(tail -2 "$t.out")"
  cp "$hd/Library/Logs/rbstemsplus/bridge.log" "$lg/sessions-nomemo.log"
  n=$(field_of "$lg/sessions-nomemo.log" "timing session memo=hashed " own_ns | emit_all P-B8.nomemo); want "$n" "$N_SESS" P-B8.nomemo
  rm -rf "$hd"

  if [ -n "$VOL" ]; then
    local hc="$t/home-drive" loc="$VOL/rbsp-perf/$2-$3/RB Stems Plus Cache.noindex"
    mkdir -p "$hc/Library/Logs/rbstemsplus" "$hc/Library/Application Support/rbstemsplus" "$loc"
    printf 'id=%s\nformat=rbstems-v1\n' "$ID" > "$loc/.rbstemsplus-cache"
    printf 'cache_dir="%s"\ncache_id=%s\nmax_gb=100000\nmax_days=36500\n' "$loc" "$ID" > "$hc/Library/Application Support/rbstemsplus/config.ini"
    local hcx=(env HOME="$hc" RBSTEMS_TEST_HOME="$hc" RBSTEMS_HARNESS_CACHE="$loc" RBSTEMS_HARNESS_WAIT_FOR="cache: using" RBSTEMS_HARNESS_READY=600)
    local dl="$hc/Library/Logs/rbstemsplus/bridge.log"
    "${hcx[@]}" "$H" bench run "$M" a wait > "$t.out" 2>&1
    grep -q "stored 1" "$t.out" || die "$2 $3: the entry wasn't stored on $VOL: $(cat "$t.out")"
    say "$2 $3: cold hits x$N_COLD on $VOL (remount, purge, a new process each)"
    : > "$lg/cold.log"
    for n in $(seq "$N_COLD"); do                         # a fresh log each: it waits for its own "cache: using"
      : > "$dl"; remount; do_purge
      "${hcx[@]}" "$H" bench run "$M" a > "$t.out" 2>&1
      cat "$dl" >> "$lg/cold.log"
    done
    n=$(field_of "$lg/cold.log" "timing run=hit " own_ns | emit_all P-B1.cold); want "$n" "$N_COLD" P-B1.cold
    say "$2 $3: warm hits x$N_WARM on $VOL"
    : > "$dl"
    "${hcx[@]}" "$H" bench run "$M" "a*$N_WARM" > "$t.out" 2>&1
    cp "$dl" "$lg/warm-drive.log"
    n=$(field_of "$lg/warm-drive.log" "timing run=hit " own_ns | emit_all P-B1.warm-drive); want "$n" "$N_WARM" P-B1.warm-drive
    rm -rf "$VOL/rbsp-perf/$2-$3"
  fi
  rm -rf "$t"
}

# the other entries an index holds: summaries far from any test chunk (never near-hit
# candidates), each with a small stand-in .flac (the walks only stat it), like an entry
distractors() {   # N
  local d="$S/distract-$1"
  [ -d "$d" ] && return 0
  python3 -I - "$d" "$1" << 'EOF' || die "making the summaries"
import hashlib, os, random, struct, sys
d, n = sys.argv[1], int(sys.argv[2])
rnd = random.Random(20261109)
frames = 110                                   # 485,100 samples / 4410
flac = b"fLaC" + bytes(1020)
for i in range(n - 1):                         # the test's own entry makes n
    k = hashlib.sha256(b"perf-distractor-%d" % i).hexdigest()
    p = os.path.join(d, k[:2]); os.makedirs(p, exist_ok=True)
    dv = [rnd.uniform(1.0, 3.0) for _ in range(2 * frames)]           # log10 rms: far above any chunk's
    rf = [rnd.uniform(-40.0, -20.0) for _ in range(2 * frames)]
    b = b"RBD2" + struct.pack("<iff", frames, 0.99, 1.0) + struct.pack("<%df" % (2 * frames), *dv) \
        + struct.pack("<2f", -30.0, -30.0) + struct.pack("<%df" % (2 * frames), *rf)
    with open(os.path.join(p, k + ".desc"), "wb") as o: o.write(b)
    with open(os.path.join(p, k + ".flac"), "wb") as o: o.write(flac)
EOF
}

# ---------------------------------------------------------------- record

parse_opts() {
  while [ $# -gt 0 ]; do
    case "$1" in
      --volume) VOL="${2:-}"; shift ;;
      --set) SET="${2:-}"; shift ;;
      --idle-wait) IDLE_WAIT="${2:-}"; OPTS_GIVEN=1; shift ;;
      --dry-run) DRY=1 ;;
      --quick) QUICK=1 ;;
      --image) IMAGE=1 ;;
      --no-purge) NOPURGE=1 ;;
      --results) RESULTS="${2:-}"; [ -n "$RESULTS" ] || die "--results FILE"; shift ;;
      --no-preflight) NOPREFLIGHT=1; OPTS_GIVEN=1 ;;
      *) die "unknown option $1" ;;
    esac
    shift
  done
}

begin() {   # OUT: the session, the checks, meta
  OUT="$1"; mkdir -p "$OUT" || die "can't make $OUT"; OUT="$(cd "$OUT" && pwd -P)"
  SAMPLES="$OUT/samples.tsv"; LOGS="$OUT/logs-$SET"; mkdir -p "$LOGS"
  META="$OUT/meta-$SET.txt"
  [ -n "$VOL" ] && check_volume
  setup_session
  preflight
  [ -n "$VOL" ] && check_purge
  distractors "$IDX_A"; distractors "$IDX_B"; distractors "$PRUNE_N"
  {
    echo "set: $SET  started $(date '+%Y-%m-%d %H:%M:%S %Z')  form: $([ -n "$QUICK" ] && echo "QUICK (smoke, not a measurement)" || echo full)"
    echo "host: $(sysctl -n hw.model), $(sysctl -n machdep.cpu.brand_string), $(sysctl -n hw.ncpu) cores, macOS $(sw_vers -productVersion) ($(sw_vers -buildVersion))"
    echo "power: $POWER"
    echo "load: $(sysctl -n vm.loadavg)"
    echo "top: $(ps -Ao pcpu=,comm= -r | head -5 | awk '{printf "%s %s; ", $1, $NF}')"
    echo "volume: ${VOL:-none (no cold or drive numbers)}${VOL:+ ($(diskutil info "$VOL" | awk -F': *' '/File System Personality|Protocol|Solid State|Device Location/ {printf "%s; ", $2}'))}${IMAGE:+ DISK IMAGE: not a cold measurement}"
    echo "purge: ${PURGE:-not needed}"
    echo "counts: start $N_START, warm $N_WARM, cold $N_COLD, near $N_NEAR ($IDX_A, $IDX_B), miss $N_MISS, index $N_IDX, prune $N_PRUNE ($PRUNE_N), rss $N_RSS, sessions $N_SESS"
    echo "models: test $(shasum -a 256 "$S/model/hdemucs.onnx" | cut -c1-12), rebuilt-shaped $(shasum -a 256 "$S/pmodel/hdemucs.onnx" | cut -c1-12), P-B8: $B8KIND"
  } >> "$META"
  if [ -z "$QUICK" ] && [ -z "$VOL" ]; then echo "WARNING: no --volume: P-B1's cold and drive numbers are left out" | tee -a "$META"; fi
}

side_meta() { echo "side $1: $2"; sed 's/^/  /' "$2/IDENTITY"; }

cmd_record() {
  [ $# -ge 2 ] || die "usage: perf.sh record SIDE OUT [options]"
  local side; side="$(cd "$1" && pwd -P)" || die "no side $1"; local out="$2"; shift 2
  SET="set-$(date +%Y%m%d-%H%M)"; parse_opts "$@"
  [ -z "${OPTS_GIVEN:-}${DRY:-}${RESULTS:-}" ] || die "--idle-wait, --dry-run, --results and --no-preflight are the gate's"
  check_side "$side"
  begin "$out"
  side_meta "$(basename "$side")" "$side" >> "$META"
  harness_for "$side" "$(basename "$side")"
  bench_round "$side" "$(basename "$side")" "$SET"
  echo "ended $(date '+%Y-%m-%d %H:%M:%S')" >> "$META"
  python3 -I "$root/tests/perf/perfstats.py" summary "$SAMPLES" | tee "$OUT/summary.txt"
  say "recorded $SET into $OUT"
}

# ---------------------------------------------------------------- the gate (GP), one session

GA="${GATES_ARCHIVE:-$root/gates-archive}"
TESTMODE=; SS=; ALL=; SIDE_A=; SIDE_B=

ss() { echo "$*" >> "$SS"; }

stop() {    # stop RC WHY: the session ends here (3: the measurement failed, 2: a stop)
  say "GP stopped: $2"
  ss "stopped=$(date -u +%FT%TZ) rc=$1 $2"; ss "finished=$(date -u +%FT%TZ) rc=$1"
  exit "$1"
}

RETRIED=        # one disturbed set per session runs again (the cause is outside the build)
run_set() {   # SIDE K: one run set of SIDE as round pK; short, or disturbed a second time in the
  # session: the measurement failed
  local side="$1" n="$1-p$2" dir t0 dur w0
  if [ "$side" = A ]; then dir="$SIDE_A"; else dir="$SIDE_B"; fi
  while :; do
    if [ "$IDLE_WAIT" -gt 0 ]; then
      idle_try || stop 2 "HIDIdleTime unreadable (ioreg) before $n: '$idle'"
      if [ "$idle" -lt 60 ]; then idle_wait 300 "input continues" "$SS" || stop 2 "HIDIdleTime unreadable (ioreg) before $n"; fi
      ss "idle_at=$n $idle"
    fi
    SAMPLES="$OUT/sets/$n.tsv"; : > "$SAMPLES"; SHORT=
    w0="$(grep -c WARNING "$META")"
    t0="$(date +%s)"
    if [ -n "$TESTMODE" ]; then "$BRIDGE_PERF_BENCH" "$dir" "$side" "p$2" "$SAMPLES" "$META" || die "the bench stub failed on $n"
    else bench_round "$dir" "$side" "p$2"; fi
    dur=$(( $(date +%s) - t0 )); ss "dur=$dur $n"
    if [ -n "$SHORT" ] || [ "$(grep -c WARNING "$META")" != "$w0" ] || [ ! -s "$SAMPLES" ]; then
      ss "short=$n"; stop 3 "$n came out short (WARNING in $META): the measurement failed"
    fi
    if [ "$IDLE_WAIT" -gt 0 ]; then
      idle_try || stop 2 "HIDIdleTime unreadable (ioreg) after $n: '$idle'"
      if [ "$idle" -lt "$dur" ]; then
        [ -z "$RETRIED" ] || { ss "disturbed=$n (input during the set: idle $idle s, the set $dur s; the session's one retry is spent)"; stop 3 "$n disturbed by input: the measurement failed"; }
        # the first disturbance: the same set again, its samples replaced, after 5 idle minutes
        RETRIED=1; ss "disturbed_retried=$n (input during the set: idle $idle s, the set $dur s)"
        idle_wait 300 "a disturbed set runs again" "$SS" || stop 2 "HIDIdleTime unreadable (ioreg) before $n's retry"
        continue
      fi
    fi
    cat "$SAMPLES" >> "$ALL"; ss "done=$n"; return 0
  done
}

cmd_gate() {
  [ $# -ge 3 ] || die "usage: perf.sh gate A B OUT [options]"
  local a="$1" b="$2" out="$3" rc k s l notb0=; shift 3
  RESULTS= IDLE_WAIT= DRY= SET=gp
  parse_opts "$@"
  # the selftest's stubs (BRIDGE_PERF_TEST=1 only): the bench, the idle reader
  if [ "${BRIDGE_PERF_TEST:-}" = 1 ]; then
    [ -n "${BRIDGE_PERF_BENCH:-}" ] || die "BRIDGE_PERF_TEST needs BRIDGE_PERF_BENCH (a stub)"
    TESTMODE=1
  else
    [ -z "${BRIDGE_PERF_BENCH:-}${PERF_IDLE_CMD:-}" ] || die "BRIDGE_PERF_BENCH and PERF_IDLE_CMD are the selftest's (BRIDGE_PERF_TEST=1)"
  fi
  SIDE_A="$(cd "$a" 2> /dev/null && pwd -P)" || die "no side $a"; SIDE_B="$(cd "$b" 2> /dev/null && pwd -P)" || die "no side $b"
  [ -n "$IDLE_WAIT" ] || IDLE_WAIT=$([ -n "$QUICK$TESTMODE" ] && echo 0 || echo 600)
  case "$IDLE_WAIT" in ''|*[!0-9]*|???????*) die "--idle-wait SECS: a number up to 999999 (0: off)" ;; esac
  IDLE_WAIT=$((10#$IDLE_WAIT))
  [ -z "$IMAGE" ] || [ -n "$QUICK" ] || die "--image is for --quick smoke runs only: a disk image is never a cold number"
  if [ -n "$TESTMODE" ]; then
    [ -f "$SIDE_A/IDENTITY" ] && [ -f "$SIDE_B/IDENTITY" ] || die "a side without IDENTITY"
  else
    check_side "$SIDE_A"; check_side "$SIDE_B"
  fi
  if [ -n "$DRY" ]; then
    echo "GP, the bridge (bridge.md §0b): dry run, nothing run"
    echo "  A: $SIDE_A; B: $SIDE_B; volume: ${VOL:-none (no cold or drive numbers: the measurement fails)}"
    echo "  4 run sets, A B then B A, about 5-8 min each: about 20-30 min, plus the idle wait ($IDLE_WAIT s) and the setup"
    [ -z "$VOL" ] || echo "  the drive's pre-flight first: $([ -n "$NOPREFLIGHT" ] && echo "skipped (--no-preflight)" || echo "$((4 * $([ -n "$QUICK" ] && echo 2 || echo 15))) remount and purge cycles")"
    return 0
  fi
  if [ -e "$out" ] || [ -L "$out" ]; then
    [ -d "$out" ] && l="$(ls -A "$out" 2> /dev/null)" && [ -z "$l" ] || die "$out exists: a new OUT for each session"
  fi
  mkdir -p "$out/sets" || die "can't make $out"; OUT="$(cd "$out" && pwd -P)"; SS="$OUT/session.txt"; ALL="$OUT/samples.tsv"; : > "$ALL"
  {
    echo "gate started=$(date -u +%FT%TZ)"
    echo "A=$SIDE_A identity_sha256=$(shasum -a 256 "$SIDE_A/IDENTITY" | cut -d' ' -f1)"
    echo "B=$SIDE_B identity_sha256=$(shasum -a 256 "$SIDE_B/IDENTITY" | cut -d' ' -f1)"
    echo "volume=${VOL:-none}"
    echo "options=quick=${QUICK:-0} image=${IMAGE:-0} nopurge=${NOPURGE:-0} nopreflight=${NOPREFLIGHT:-0} test=${TESTMODE:-0}"
    echo "idle_wait=$IDLE_WAIT"
  } > "$SS"
  # an interrupt from here on: stopped= and finished= in session.txt, then the EXIT trap remounts
  trap 'stop 2 "interrupted"' INT TERM HUP
  if [ -n "$TESTMODE" ]; then
    LOGS="$OUT/logs-$SET"; META="$OUT/meta-$SET.txt"; mkdir -p "$LOGS"
    echo "set: $SET (the selftest's stubs) started $(date '+%Y-%m-%d %H:%M:%S')" >> "$META"
  else
    begin "$OUT"
    { side_meta A "$SIDE_A"; side_meta B "$SIDE_B"; } >> "$META"
    harness_for "$SIDE_A" A; harness_for "$SIDE_B" B
    caffeinate -ims -w $$ &          # the system stays awake; the displays sleep on their own timer
    if [ -n "$VOL" ] && [ -z "$NOPREFLIGHT" ]; then
      ( vol_preflight $((4 * N_COLD)) ) || stop 2 "the drive's pre-flight failed: no set was run"
      ss "preflight=$((4 * N_COLD)) cycles ok"
    fi
  fi
  if [ "$IDLE_WAIT" -gt 0 ]; then
    idle_wait "$IDLE_WAIT" "before the first set" "$SS" || stop 2 "HIDIdleTime unreadable (ioreg)"
    idle_display start "$SS"
  fi
  # two pairs, the sides alternating (A B, then B A): a drift falls on both alike
  for k in 1 2; do
    for s in $([ "$k" = 1 ] && echo "A B" || echo "B A"); do run_set "$s" "$k"; done
  done
  # the rehearsal (--quick, no drive): the drive's two metrics can't be there; a real gate needs them
  local req="$REQUIRED" nodrive=
  if [ -n "$QUICK" ] && [ -z "$VOL" ]; then nodrive=1; req="$(echo "$REQUIRED" | tr , '\n' | grep -vx 'P-B1.cold\|P-B1.warm-drive' | paste -sd, -)"; fi
  python3 -I "$root/tests/perf/perfstats.py" gate "$ALL" --a A --b B --prefix P-B --require "$req" \
    --rules "$root/tests/perf/thresholds.txt" --out "$OUT/gate.tsv" > "$OUT/gate.txt" 2>&1; rc=$?
  ss "verdict=$rc"      # the gate's own verdict, before a smoke run's or the stubs' downgrade
  [ -n "$TESTMODE" ] || python3 -I "$root/tests/perf/perfstats.py" summary "$ALL" > "$OUT/summary.txt"
  # the rows are a budget against B0 (bridge.md §0b): another A informs, never passes
  if ! { [ -d "$GA/B0" ] && [ "$SIDE_A" = "$(cd "$GA/B0" && pwd -P)" ]; }; then
    notb0=1
    sed 's/^GP: pass/GP: information only (A is not B0): pass/' "$OUT/gate.txt" > "$OUT/gate.txt.new" && mv "$OUT/gate.txt.new" "$OUT/gate.txt"
    echo "GP: information only (A is not B0, $GA/B0): never a pass" >> "$OUT/gate.txt"
    echo "# compare: information only (A is not B0)" >> "$OUT/gate.tsv"    # record.py refuses it
    [ "$rc" = 0 ] && rc=3
  fi
  if [ -n "$IMAGE$NOPURGE$QUICK$TESTMODE" ]; then
    echo "GP: not a valid gate (${QUICK:+quick }${IMAGE:+disk image }${NOPURGE:+no purge }${TESTMODE:+stubs}${nodrive:+no drive (P-B1.cold and P-B1.warm-drive not required)}): tooling check only" >> "$OUT/gate.txt"
    echo "# compare: information only (${QUICK:+quick }${IMAGE:+disk image }${NOPURGE:+no purge }${TESTMODE:+stubs}: not a valid gate)" >> "$OUT/gate.tsv"
    [ "$rc" = 0 ] && rc=3
  fi
  [ "$IDLE_WAIT" -gt 0 ] && idle_display end "$SS"
  ss "finished=$(date -u +%FT%TZ) rc=$rc"
  cat "$OUT/gate.txt"
  if [ -n "$RESULTS" ]; then
    {
      printf '\n## GP, the bridge (%s)%s\n\nbridge/tests/perf.sh gate: two alternating pairs (A B, B A). A: `%s`; B: `%s`. Volume: %s.\n\n```\n' \
        "$(date +%Y-%m-%d)" "${notb0:+: information only (A is not B0)}" "$(sed -n 2p "$SIDE_A/IDENTITY")" "$(sed -n 2p "$SIDE_B/IDENTITY")" "${VOL:-none}"
      cat "$OUT/gate.txt"; printf '```\n'
    } >> "$RESULTS"
  fi
  return "$rc"
}

cmd_selftest() {
  local t; t="$(cd "$(mktemp -d "${TMPDIR:-/tmp}/rbsp-bperf-self.XXXXXX")" && pwd -P)"
  say "selftest in $t"
  cmd_build "$t/A" || return 1
  cp -R "$t/A" "$t/B"                      # B runs A's bytes from its own folder: it loads A's ort/ path
  ( cmd_record "$t/A" "$t/rec" --quick ) || { echo "selftest: record failed"; return 1; }
  grep -q '^P-B8.memo' "$t/rec/samples.tsv" && grep -q '^P-B1.warm	' "$t/rec/samples.tsv" || { echo "selftest: samples missing"; return 1; }
  ( cmd_gate "$t/A" "$t/A" "$t/gate" --quick ); local rc=$?
  [ "$rc" = 3 ] && grep -q "tooling check only" "$t/gate/gate.txt" || { echo "selftest: gate rc $rc"; return 1; }
  grep -qx "done=B-p2" "$t/gate/session.txt" && grep -q "^verdict=" "$t/gate/session.txt" || { echo "selftest: the gate's session is incomplete"; return 1; }
  rm -rf "$t"
  say "selftest: ok"
}

case "${1:-}" in
  build) shift; cmd_build "$@" ;;
  record) shift; cmd_record "$@" ;;
  gate) shift; cmd_gate "$@" ;;
  selftest) cmd_selftest ;;
  preflight) shift; cmd_preflight "$@" ;;
  *) sed -n '2,/^set -u/p' "$0" | sed '$d'; exit 2 ;;
esac

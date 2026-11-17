#!/bin/bash
# The helper's performance numbers P-H1-P-H5 (helper.md §2a) and its GP gate, with helpers built
# with the timing lines (helper/build.sh --test-timing). Same method and rules as the bridge's
# bridge/tests/perf.sh: AC power, nothing else running, alternating pairs, one rule,
# tests/perf/thresholds.txt.
#
#   helper/tests/perf.sh build SIDE
#       the working tree's helper with the timing lines into SIDE/helper/ (SIDE/IDENTITY-helper)
#   helper/tests/perf.sh corpus DIR [--quick]
#       the corpus (tests/perf/corpus-v1.txt) made into DIR, with DIR/SHA256SUMS and DIR/TOOLS;
#       an existing DIR is checked against its SHA256SUMS instead (never made again in place)
#   helper/tests/perf.sh record SIDE OUT --corpus DIR [--form full|short] [--set NAME] [--quick]
#       one run set of SIDE into OUT: samples.tsv (appended), meta-NAME.txt, logs-NAME/
#   helper/tests/perf.sh gate A B OUT --corpus DIR [--form short|full] [--rounds N] [--idle-wait SECS] [--results FILE]
#       GP: N alternating pairs (default 2: A B, then B A), then tests/perf/perfstats.py gate (the
#       owner's rule of 2026-11-12). The bridge's idle guard (tests/perf/idle.sh): the first set
#       waits until the Mac has had no input for SECS (600; 0: off, the default with --quick); a
#       set during which input came runs again once per session (disturbed_retried=), a second
#       one (disturbed=) or a short set fails the measurement (3); an unreadable idle time stops
#       it (2). OUT/session.txt holds every step. Exit 0 pass, 1 a regression, 3 the measurement
#       failed. HELPER_PERF_TEST=1 (tests/harness/selftest.sh only): HELPER_PERF_BENCH replaces a
#       run set, PERF_IDLE_CMD the idle reader; a real gate refuses them.
#   helper/tests/perf.sh selftest
#       tiny runs of everything in a scratch folder: the tooling works
#
# The run set (full form; the short form at GP and G3: our model only, the corpus's 8 "short"
# tracks; both sides always run the same form):
#   P-H1  per model (demucs4: our Demucs v4; rb0002: rekordbox's own, rebuilt), 1 run of --list
#         on the corpus into a fresh --cache and --kept-dir, --threads 4 (RBSP_PERF_THREADS):
#         P-H1.run.<model> the run's wall time over its tracks; P-H1.track.<model> each track's
#         time from the timing lines (start of decoding to its last write)
#   P-H2  P-H2.<model>.<type> each chunk's own time: everything but ONNX Runtime's Run (taking the
#         chunk, its key, the model's tensors and spectrogram, the stems' rebuild, the write, and
#         its share of the track's decoding and resampling); every file type of the form's corpus
#         must have samples
#   P-H3  --estimate on the 5,000-path list (clones of the corpus's files, mostly MP3 and
#         AAC/ALAC), 1 run: P-H3.first and P-H3.last, from the start to the first and the last line
#   P-H4  P-H4.rss the first demucs4 run's peak memory; P-H4.size the binary
#   P-H5  --playlists, and --tracks of the library's largest plain playlist, 5 runs each, on a
#         copy of rekordbox's library made at the start (RBSP_PERF_LIBRARY, default
#         ~/Library/Pioneer/rekordbox/master.db; skipped with --quick). Nothing the library holds
#         is printed or kept: only the playlist's ID, in the meta file.
# Needs: our model (RBSP_PERF_MODEL_OURS, default dist-test/stemsplus-model.onnx here or in the
# main checkout), rekordbox's (RBSP_PERF_MODEL_RB, default the copy the app keeps in
# ~/Library/Application Support/rbstemsplus/originals), Pioneer's ONNX Runtime (RBSP_ORT, default
# the app's), rekordbox 7 (its libmpg123), ffmpeg (with libmp3lame and AudioToolbox's aac_at) for the corpus only.
set -u
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
die() { echo "perf.sh: $*" >&2; exit 2; }
say() { echo "[$(date +%H:%M:%S)] $*"; }
# shellcheck source=../../tests/perf/idle.sh disable=SC1091
. "$root/tests/perf/idle.sh"
idle=""
RB_SHA=435c987855f7ea74ff5f20090541748cbbce5a2e8e501d07b10f4e703453215a
ORT="${RBSP_ORT:-/Library/Application Support/rbstemsplus/ort/libonnxruntime.1.18.0.dylib}"
RBAPP="${RBSP_REKORDBOX:-/Applications/rekordbox 7/rekordbox.app}"
THREADS="${RBSP_PERF_THREADS:-4}"
REQUIRED_FULL=P-H1.run.demucs4,P-H1.track.demucs4,P-H1.run.rb0002,P-H1.track.rb0002,P-H3.first,P-H3.last,P-H4.rss,P-H4.size,P-H5.playlists,P-H5.tracks
REQUIRED_SHORT=P-H1.run.demucs4,P-H1.track.demucs4,P-H3.first,P-H3.last,P-H4.rss,P-H4.size,P-H5.playlists,P-H5.tracks

model_ours() {
  local c common
  common="$(git -C "$root" rev-parse --path-format=absolute --git-common-dir 2>/dev/null)"
  for c in "${RBSP_PERF_MODEL_OURS:-}" "$root/dist-test/stemsplus-model.onnx" "${common:+${common%/.git}/dist-test/stemsplus-model.onnx}"; do
    [ -n "$c" ] && [ -f "$c" ] && { echo "$c"; return 0; }
  done
  return 1
}
model_rb() { local c="${RBSP_PERF_MODEL_RB:-$HOME/Library/Application Support/rbstemsplus/originals/$RB_SHA.onnx}"; [ -f "$c" ] && echo "$c"; }

# ---------------------------------------------------------------- build

cmd_build() {
  [ $# -eq 1 ] || die "usage: perf.sh build SIDE"
  mkdir -p "$1" || die "can't make $1"
  local side; side="$(cd "$1" && pwd -P)"
  rm -rf "$side/helper"
  # RBSP_PERF_SRC: another source tree to build (tests/perf/b0.sh: B0's), RBSP_PERF_SRC_DESC says which
  "${RBSP_PERF_SRC:-$root}/helper/build.sh" --test-timing "$side/helper" > "$side/helper-build.log" 2>&1 || { tail "$side/helper-build.log"; die "the timing helper didn't build"; }
  {
    echo "built: $(date -u +%Y-%m-%dT%H:%M:%SZ) on $(sysctl -n hw.model), macOS $(sw_vers -productVersion) ($(sw_vers -buildVersion))"
    echo "source: ${RBSP_PERF_SRC_DESC:-$(git -C "$root" rev-parse HEAD 2>/dev/null || echo "?")$(git -C "$root" status --porcelain -- 'helper/*.c' 'helper/*.h' helper/build.sh 2>/dev/null | grep -q . && echo " (the helper's source has uncommitted changes)")}"
    echo "toolchain: $(clang --version | head -1); SDK $(xcrun --show-sdk-version 2>/dev/null)"
    (cd "$side" && shasum -a 256 helper/rbsp-prepare)
  } > "$side/IDENTITY-helper"
  say "built $side/helper ($(sed -n 2p "$side/IDENTITY-helper"))"
}

check_side() {
  [ -x "$1/helper/rbsp-prepare" ] || die "$1 has no helper (perf.sh build $1)"
  strings "$1/helper/rbsp-prepare" | grep -q "rbsp-timing" || die "$1/helper has no timing lines"
  (cd "$1" && shasum -a 256 -c --quiet <(sed -n '/  helper\//p' IDENTITY-helper)) || die "$1/helper doesn't match its IDENTITY-helper"
}

# ---------------------------------------------------------------- the corpus

QUICK_CORPUS='q1-mp3  mp3-cbr  44100  192k  12  220  277  short
q2-wav-48k  wav  48000  s16  12  247  311  short'

gen_track() {   # DIR name type rate fmt secs f1 f2
  local d="$1" name="$2" type="$3" rate="$4" fmt="$5" secs="$6" f1="$7" f2="$8" src="$1/.src-$2.wav"
  local ff=(ffmpeg -nostdin -hide_banner -loglevel error -y)
  local bx=(-fflags +bitexact -flags:a +bitexact -map_metadata -1)
  local beat="0.35*sin(2*PI*50*t)*exp(-6*mod(t,0.5))"
  local L="0.25*sin(2*PI*$f1*t)*(0.6+0.4*sin(2*PI*0.5*t))+$beat+0.12*(random(0)-0.5)"
  local R="0.25*sin(2*PI*$f2*t)*(0.6+0.4*sin(2*PI*0.3*t))+$beat+0.12*(random(1)-0.5)"
  "${ff[@]}" -f lavfi -i "aevalsrc=exprs='$L|$R':s=$rate:d=$secs" -c:a pcm_f32le "${bx[@]}" "$src" || return 1
  local sf pcm al
  case "$fmt" in
    s16) sf=(-sample_fmt s16); pcm=pcm_s16; al=(-sample_fmt s16p) ;;
    s24) sf=(-sample_fmt s32 -bits_per_raw_sample 24); pcm=pcm_s24; al=(-sample_fmt s32p -bits_per_raw_sample 24) ;;
    *) sf=(); pcm=pcm_f32; al=() ;;
  esac
  case "$type" in
    mp3-cbr) "${ff[@]}" -i "$src" -c:a libmp3lame -b:a "$fmt" "${bx[@]}" -write_xing 1 "$d/$name.mp3" ;;
    mp3-vbr) "${ff[@]}" -i "$src" -c:a libmp3lame -q:a "$fmt" "${bx[@]}" "$d/$name.mp3" ;;
    aac) "${ff[@]}" -i "$src" -c:a aac_at -b:a "$fmt" "${bx[@]}" "$d/$name.m4a" ;;
    alac) "${ff[@]}" -i "$src" -c:a alac "${al[@]}" "${bx[@]}" "$d/$name.m4a" ;;
    flac) "${ff[@]}" -i "$src" -c:a flac "${sf[@]}" "${bx[@]}" "$d/$name.flac" ;;
    wav) "${ff[@]}" -i "$src" -c:a "${pcm}le" "${bx[@]}" "$d/$name.wav" ;;
    aiff) "${ff[@]}" -i "$src" -c:a "${pcm}be" "${bx[@]}" -f aiff "$d/$name.aif" ;;
    *) echo "unknown type $type"; return 1 ;;
  esac || return 1
  rm -f "$src"
}

cmd_corpus() {
  [ $# -ge 1 ] || die "usage: perf.sh corpus DIR [--quick]"
  local d="$1" quick="${2:-}" spec
  if [ -f "$d/SHA256SUMS" ]; then
    (cd "$d" && shasum -a 256 -c --quiet SHA256SUMS) || die "$d doesn't match its SHA256SUMS: a damaged corpus (never remade in place)"
    return 0
  fi
  [ ! -e "$d" ] || [ -z "$(ls -A "$d")" ] || die "$d exists without SHA256SUMS"
  command -v ffmpeg > /dev/null || die "the corpus needs ffmpeg (FOSS; e.g. Homebrew's)"
  ffmpeg -hide_banner -encoders 2> /dev/null | grep -q libmp3lame || die "this ffmpeg has no libmp3lame (MP3)"
  ffmpeg -hide_banner -encoders 2> /dev/null | grep -q aac_at || die "this ffmpeg has no aac_at (macOS's AAC encoder)"
  mkdir -p "$d"
  if [ "$quick" = --quick ]; then spec="$QUICK_CORPUS"; else spec="$(grep -v '^#' "$root/tests/perf/corpus-v1.txt" | grep .)"; fi
  local n=0
  while read -r name type rate fmt secs f1 f2 short; do
    [ -n "$name" ] || continue
    gen_track "$d" "$name" "$type" "$rate" "$fmt" "$secs" "$f1" "$f2" || die "making $name"
    n=$((n + 1))
  done <<< "$spec"
  printf '%s\n' "$spec" | awk '{print $1, $8}' > "$d/FORMS"
  {
    echo "made: $(date -u +%Y-%m-%dT%H:%M:%SZ), macOS $(sw_vers -productVersion) ($(sw_vers -buildVersion))"
    echo "spec: $([ "$quick" = --quick ] && echo "QUICK (2 tracks)" || echo "tests/perf/corpus-v1.txt $(shasum -a 256 "$root/tests/perf/corpus-v1.txt" | cut -c1-16)")"
    echo "ffmpeg: $(ffmpeg -hide_banner -version | head -1)"
  } > "$d/TOOLS"
  (cd "$d" && shasum -a 256 $(ls | grep -v '^SHA256SUMS$') > SHA256SUMS)
  if [ "$quick" != --quick ]; then   # the same tools make the same bytes; other tools: a corpus of their own, said so
    if diff -q <(grep -v ' TOOLS$' "$d/SHA256SUMS") "$root/tests/perf/corpus-v1.sha256" > /dev/null; then echo "reference: the bytes of tests/perf/corpus-v1.sha256" >> "$d/TOOLS"
    else echo "reference: DIFFERS from tests/perf/corpus-v1.sha256 (other tools): this corpus is its own baseline" | tee -a "$d/TOOLS"; fi
    (cd "$d" && shasum -a 256 $(ls | grep -v '^SHA256SUMS$') > SHA256SUMS)
  fi
  say "made $n tracks into $d"
}

corpus_files() {   # DIR FORM: the files, full paths, the corpus's order
  local d="$1" form="$2" name s f
  while read -r name s; do
    [ "$form" = short ] && [ "$s" != short ] && continue
    for f in "$d/$name".*; do echo "$f"; done
  done < "$d/FORMS"
}

# ---------------------------------------------------------------- the session

S= QUICK= OUT= SAMPLES= CORPUS= FORM=full SET= LIBCOPY= PLAYLIST=
parse_opts() {
  while [ $# -gt 0 ]; do
    case "$1" in
      --corpus) CORPUS="${2:-}"; shift ;;
      --form) FORM="${2:-}"; shift ;;
      --set) SET="${2:-}"; shift ;;
      --rounds) ROUNDS="${2:-}"; shift ;;
      --idle-wait) IDLE_WAIT="${2:-}"; shift ;;
      --quick) QUICK=1 ;;
      --results) RESULTS="${2:-}"; [ -n "$RESULTS" ] || die "--results FILE"; shift ;;
      *) die "unknown option $1" ;;
    esac
    shift
  done
  case "$FORM" in full|short) ;; *) die "--form full or short" ;; esac
  [ -n "$CORPUS" ] || die "--corpus DIR (perf.sh corpus DIR makes it)"
  [ -f "$CORPUS/SHA256SUMS" ] || die "$CORPUS isn't a corpus (perf.sh corpus $CORPUS)"
  CORPUS="$(cd "$CORPUS" && pwd -P)"
}

begin() {
  OUT="$1"; mkdir -p "$OUT" || die "can't make $OUT"; OUT="$(cd "$OUT" && pwd -P)"
  SAMPLES="$OUT/samples.tsv"; LOGS="$OUT/logs-$SET"; META="$OUT/meta-$SET.txt"; mkdir -p "$LOGS"
  S="$(cd "$(mktemp -d "${TMPDIR:-/tmp}/rbsp-hperf.XXXXXX")" && pwd -P)"
  trap 'rm -rf "$S"' EXIT
  (cd "$CORPUS" && shasum -a 256 -c --quiet SHA256SUMS) || die "$CORPUS doesn't match its SHA256SUMS"
  MOURS="$(model_ours)" || die "no copy of our model (RBSP_PERF_MODEL_OURS)"
  MRB="$(model_rb)"
  [ -n "$MRB" ] || [ "$FORM" = short ] || [ -n "$QUICK" ] || die "no copy of rekordbox's model (RBSP_PERF_MODEL_RB), needed in the full form"
  [ -f "$ORT" ] && ! grep -aq "rbstems bridge: " "$ORT" || die "no Pioneer's ONNX Runtime at $ORT (RBSP_ORT)"
  [ -d "$RBAPP" ] || die "no rekordbox at $RBAPP (RBSP_REKORDBOX): its libmpg123 decodes MP3"
  local batt lpm
  batt="$(pmset -g batt | head -1)"
  lpm="$(pmset -g | awk '$1 == "lowpowermode" || $1 == "powermode" {print $2; exit}')"
  if [ -z "$QUICK" ]; then
    [[ "$batt" == *"AC Power"* ]] || die "not on AC power ($batt)"
    [ "${lpm:-0}" != 1 ] || die "Low Power Mode is on"
  fi
  # the 5,000-path list: clones of the corpus's files (no space taken), mostly MP3 and AAC/ALAC
  local n=5000; [ -n "$QUICK" ] && n=50
  python3 -I - "$CORPUS" "$S/est" "$n" << 'EOF' || die "the 5,000-path list (the corpus and the scratch folder must be on one APFS volume: clones)"
import os, subprocess, sys
src, d, n = sys.argv[1], sys.argv[2], int(sys.argv[3])
files = sorted(f for f in os.listdir(src) if f.endswith((".mp3", ".m4a", ".flac", ".wav", ".aif")))
mp3 = [f for f in files if f.endswith(".mp3")]; m4a = [f for f in files if f.endswith(".m4a")]
rest = [f for f in files if f not in mp3 and f not in m4a]
order = (mp3 * 4)[:4] + (m4a * 3)[:3] + (rest * 3)[:3]      # 40% MP3, 30% AAC/ALAC, 30% the others
os.makedirs(d)
with open(os.path.join(d, "list.txt"), "w") as o:
    for i in range(n):
        f = order[i % len(order)]
        dst = os.path.join(d, "%04d-%s" % (i, f))
        subprocess.run(["cp", "-c", os.path.join(src, f), dst], check=True)
        o.write(dst + "\n")
EOF
  if [ -z "$QUICK" ]; then
    local lib="${RBSP_PERF_LIBRARY:-$HOME/Library/Pioneer/rekordbox/master.db}" x
    [ -f "$lib" ] || die "no rekordbox library at $lib (RBSP_PERF_LIBRARY)"
    mkdir -p "$S/lib"
    for x in "" -wal -shm; do [ -f "$lib$x" ] && cp "$lib$x" "$S/lib/master.db$x"; done
    LIBCOPY="$S/lib/master.db"
  fi
  {
    echo "set: $SET  started $(date '+%Y-%m-%d %H:%M:%S %Z')  form: $FORM$([ -n "$QUICK" ] && echo ", QUICK (smoke, not a measurement)")"
    echo "host: $(sysctl -n hw.model), $(sysctl -n machdep.cpu.brand_string), $(sysctl -n hw.ncpu) cores, macOS $(sw_vers -productVersion) ($(sw_vers -buildVersion))"
    echo "power: $batt; powermode=${lpm:-?} (1: Low Power Mode)"
    echo "load: $(sysctl -n vm.loadavg)"
    echo "top: $(ps -Ao pcpu=,comm= -r | head -5 | awk '{printf "%s %s; ", $1, $NF}')"
    echo "corpus: $CORPUS ($(shasum -a 256 "$CORPUS/SHA256SUMS" | cut -c1-16)), $(corpus_files "$CORPUS" "$FORM" | wc -l | tr -d ' ') tracks"
    echo "models: demucs4 $(shasum -a 256 "$MOURS" | cut -c1-12)${MRB:+, rb0002 $(basename "$MRB" | cut -c1-12)}; threads $THREADS"
    echo "ORT: $ORT ($(shasum -a 256 "$ORT" | cut -c1-12)); rekordbox: $(defaults read "$RBAPP/Contents/Info" CFBundleShortVersionString 2>/dev/null)"
    echo "library: ${LIBCOPY:+a copy made $(date +%H:%M), $(stat -f %z "$LIBCOPY") bytes}${LIBCOPY:-none (--quick)}"
  } > "$META"
}

emit() {
  case "$2" in ''|*[!0-9]*) echo "  WARNING $1: no number (\"$2\")" | tee -a "$META"; return ;; esac
  printf '%s\t%s\t%s\t%s\n' "$1" "$SIDE_NAME" "$ROUND" "$2" >> "$SAMPLES"
}
# runs a command, its stdout into $1, prints the wall time in ns; the exit status is the command's
timed() { python3 -I -c 'import subprocess, sys, time
t = time.monotonic_ns(); r = subprocess.run(sys.argv[2:], stdout=open(sys.argv[1], "wb")).returncode
print(time.monotonic_ns() - t); sys.exit(r)' "$@"; }

# the timing lines of one run: per-track times and per-chunk own times by file type
parse_run() {   # STDERR MODEL: prints "metric value" lines
  python3 -I - "$1" "$2" << 'EOF'
import re, sys
tracks, chunks = {}, {}
for line in open(sys.argv[1], errors="replace"):
    if not line.startswith("rbsp-timing "):
        continue
    f = dict(kv.split("=", 1) for kv in line.split()[2:] if "=" in kv)
    if line.split()[1] == "track":
        tracks[f["at"]] = f
    else:
        chunks.setdefault(f["at"], []).append(f)
for at, t in tracks.items():
    print("P-H1.track.%s %d" % (sys.argv[2], int(t["end_ns"]) - int(t["start_ns"])))
    cs = [c for c in chunks.get(at, []) if c["kind"] in ("written", "refused")]
    share = (int(t["decode_ns"]) + int(t["resample_ns"])) // max(1, len(cs))
    for c in cs:
        # the whole chunk but ONNX Runtime's Run (the tensors and the spectrogram are ours)
        own = int(c["chunk_ns"]) - int(c["ort_ns"]) + share
        print("P-H2.%s.%s %d" % (sys.argv[2], t["ext"], own))
EOF
}

round_side() {   # BIN SIDE_NAME ROUND
  local bin="$1" t="$S/r-$2-$3" lg="$LOGS/$2-$3" m model path i v k first=1
  SIDE_NAME="$2" ROUND="$3"
  mkdir -p "$t" "$lg"
  corpus_files "$CORPUS" "$FORM" > "$t/list.txt"
  local ntr; ntr="$(wc -l < "$t/list.txt" | tr -d ' ')"
  local models=("demucs4 $MOURS"); [ "$FORM" = full ] && [ -n "$MRB" ] && models+=("rb0002 $MRB")
  local runs=1     # one run per set (the helper lane's follow-up, owner decided 2026-11-12: short sessions)
  for m in "${models[@]}"; do
    read -r model path <<< "$m"
    for i in $(seq "$runs"); do
      say "$2 $3: P-H1 $model run $i, $ntr tracks"
      rm -rf "$t/cache" "$t/kept"; mkdir -p "$t/cache" "$t/kept"
      local pre=(); [ "$first" = 1 ] && pre=(/usr/bin/time -l)
      v="$(timed "$t/out.jsonl" ${pre[@]+"${pre[@]}"} "$bin" --model "$path" --ort "$ORT" --rekordbox "$RBAPP" --cache "$t/cache" --kept-dir "$t/kept" \
           --threads "$THREADS" --list "$t/list.txt" 2> "$t/err.txt")" || die "$2 $3: the helper failed: $(tail -3 "$t/err.txt")"
      cp "$t/err.txt" "$lg/$model-$i.err"; tail -1 "$t/out.jsonl" > "$lg/$model-$i.done"
      # an incomplete run is a measurement failure, never a sample
      grep -q '"done":'"$ntr"'[,}]' "$t/out.jsonl" || die "$2 $3: $model run $i: not every track done: $(tail -1 "$t/out.jsonl")"
      ! grep -q '"result":"skipped"' "$t/out.jsonl" || die "$2 $3: $model run $i: tracks skipped: $(grep '"result":"skipped"' "$t/out.jsonl" | head -3)"
      emit "P-H1.run.$model" $((v / ntr))
      parse_run "$t/err.txt" "$model" | while read -r k v; do emit "$k" "$v"; done
      if [ "$first" = 1 ]; then emit P-H4.rss "$(awk '/maximum resident set size/ {print $1}' "$t/err.txt")"; first=; fi
    done
  done
  emit P-H4.size "$(stat -f %z "$bin")"
  for i in $(seq "$runs"); do
    say "$2 $3: P-H3 --estimate of $(wc -l < "$S/est/list.txt" | tr -d ' ') paths, run $i"
    rm -rf "$t/cache"; mkdir -p "$t/cache"
    python3 -I - "$t/est.txt" "$bin" --estimate --model "$MOURS" --ort "$ORT" --rekordbox "$RBAPP" --cache "$t/cache" --kept-dir "$t/kept" \
      --list "$S/est/list.txt" << 'EOF' > "$t/est.times" || die "$2 $3: the estimate failed"
import subprocess, sys, time
t0 = time.monotonic_ns()
p = subprocess.Popen(sys.argv[2:], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
first = last = None
with open(sys.argv[1], "wb") as o:
    for line in p.stdout:
        now = time.monotonic_ns() - t0
        first = first if first is not None else now
        last = now
        o.write(line)
sys.exit(p.wait() or print(first, last))
EOF
    tail -1 "$t/est.txt" | grep -q '"estimate":' || die "$2 $3: no estimate line: $(tail -1 "$t/est.txt")"
    read -r v k < "$t/est.times"
    emit P-H3.first "$v"; emit P-H3.last "$k"
  done
  if [ -n "$LIBCOPY" ]; then
    if [ -z "$PLAYLIST" ]; then        # the largest plain playlist: its ID only, nothing else is kept
      "$bin" --playlists --library "$LIBCOPY" > "$S/playlists.jsonl" 2> /dev/null || die "--playlists failed"
      PLAYLIST="$(python3 -I -c 'import json, sys
best = max((json.loads(l) for l in open(sys.argv[1]) if l.strip()), key=lambda p: (p.get("kind") == "playlist", p.get("tracks", 0)), default={})
print(best.get("playlist", "") if best.get("kind") == "playlist" else "")' "$S/playlists.jsonl")"
      rm -f "$S/playlists.jsonl"
      [ -n "$PLAYLIST" ] || die "the library has no plain playlist for --tracks"
      echo "P-H5: --tracks on playlist ID $PLAYLIST" >> "$META"
    fi
    for i in 1 2 3 4 5; do
      v="$(timed /dev/null "$bin" --playlists --library "$LIBCOPY")" || die "--playlists failed"; emit P-H5.playlists "$v"
      v="$(timed /dev/null "$bin" --tracks "$PLAYLIST" --library "$LIBCOPY")" || die "--tracks failed"; emit P-H5.tracks "$v"
    done
  fi
  rm -rf "$t"
}

cmd_record() {
  [ $# -ge 2 ] || die "usage: perf.sh record SIDE OUT --corpus DIR [options]"
  local side; side="$(cd "$1" && pwd -P)" || die "no side $1"; local out="$2"; shift 2
  SET="set-$(date +%Y%m%d-%H%M)"; parse_opts "$@"
  check_side "$side"
  begin "$out"
  { echo "side $(basename "$side"): $side"; sed 's/^/  /' "$side/IDENTITY-helper"; } >> "$META"
  round_side "$side/helper/rbsp-prepare" "$(basename "$side")" "$SET"
  echo "ended $(date '+%Y-%m-%d %H:%M:%S')" >> "$META"
  python3 -I "$root/tests/perf/perfstats.py" summary "$SAMPLES" | tee "$OUT/summary.txt"
  say "recorded $SET into $OUT"
}

cmd_gate() {
  [ $# -ge 3 ] || die "usage: perf.sh gate A B OUT --corpus DIR [options]"
  local a b; a="$(cd "$1" && pwd -P)" || die "no side $1"; b="$(cd "$2" && pwd -P)" || die "no side $2"
  local out="$3" testmode=; shift 3
  FORM=short ROUNDS=2 RESULTS= IDLE_WAIT= SET="gp-$(date +%Y%m%d-%H%M)"; parse_opts "$@"
  # the selftest's stubs (HELPER_PERF_TEST=1 only): HELPER_PERF_BENCH replaces a run set (called
  # with SIDE_DIR SIDE ROUND SAMPLES META), PERF_IDLE_CMD the idle reader; a real gate refuses them
  if [ "${HELPER_PERF_TEST:-}" = 1 ]; then
    [ -n "${HELPER_PERF_BENCH:-}" ] || die "HELPER_PERF_TEST needs HELPER_PERF_BENCH (a stub)"; testmode=1
  else
    [ -z "${HELPER_PERF_BENCH:-}${PERF_IDLE_CMD:-}" ] || die "HELPER_PERF_BENCH and PERF_IDLE_CMD are the selftest's (HELPER_PERF_TEST=1)"
  fi
  [ -n "$IDLE_WAIT" ] || IDLE_WAIT=$([ -n "$QUICK$testmode" ] && echo 0 || echo 600)
  case "$IDLE_WAIT" in ''|*[!0-9]*|???????*) die "--idle-wait SECS: a number up to 999999 (0: off)" ;; esac
  IDLE_WAIT=$((10#$IDLE_WAIT))
  if [ -n "$testmode" ]; then
    OUT="$out"; mkdir -p "$OUT" && OUT="$(cd "$OUT" && pwd -P)" || die "can't make $out"
    LOGS="$OUT/logs-$SET"; META="$OUT/meta-$SET.txt"; mkdir -p "$LOGS"; echo "set: $SET (the selftest's stubs)" >> "$META"
  else
    check_side "$a"; check_side "$b"
    begin "$out"
    { echo "side A: $a"; sed 's/^/  /' "$a/IDENTITY-helper"; echo "side B: $b"; sed 's/^/  /' "$b/IDENTITY-helper"; } >> "$META"
    caffeinate -ims -w $$ &          # the system stays awake; the displays sleep on their own timer
  fi
  local all="$OUT/samples.tsv" ss="$OUT/session.txt" i s retried= n t0 dur w0 bin
  : > "$all"; mkdir -p "$OUT/sets"
  echo "gate started=$(date -u +%FT%TZ) idle_wait=$IDLE_WAIT" > "$ss"
  stop() { echo "stopped=$(date -u +%FT%TZ) rc=$1 $2" >> "$ss"; say "GP stopped: $2"; exit "$1"; }
  # the idle guard (tests/perf/idle.sh, the bridge's): the Mac left alone; one disturbed set per
  # session runs again, a second one fails the measurement
  if [ "$IDLE_WAIT" -gt 0 ]; then
    idle_wait "$IDLE_WAIT" "before the first set" "$ss" || stop 2 "HIDIdleTime unreadable (ioreg)"
    idle_display start "$ss"
  fi
  # alternating pairs (A B, then B A): a drift falls on both alike
  for i in $(seq "$ROUNDS"); do
    for s in $([ $((i % 2)) = 1 ] && echo "A B" || echo "B A"); do
      n="$s-r$i"; if [ "$s" = A ]; then bin="$a"; else bin="$b"; fi
      while :; do
        if [ "$IDLE_WAIT" -gt 0 ]; then
          idle_try || stop 2 "HIDIdleTime unreadable (ioreg) before $n: '$idle'"
          if [ "$idle" -lt 60 ]; then idle_wait 300 "input continues" "$ss" || stop 2 "HIDIdleTime unreadable (ioreg) before $n"; fi
          echo "idle_at=$n $idle" >> "$ss"
        fi
        SAMPLES="$OUT/sets/$n.tsv"; : > "$SAMPLES"; w0="$(grep -c WARNING "$META")"; t0="$(date +%s)"
        if [ -n "$testmode" ]; then "$HELPER_PERF_BENCH" "$bin" "$s" "r$i" "$SAMPLES" "$META" || die "the bench stub failed on $n"
        else round_side "$bin/helper/rbsp-prepare" "$s" "r$i"; fi
        dur=$(( $(date +%s) - t0 )); echo "dur=$dur $n" >> "$ss"
        [ "$(grep -c WARNING "$META")" = "$w0" ] && [ -s "$SAMPLES" ] || { echo "short=$n" >> "$ss"; stop 3 "$n came out short (WARNING in $META): the measurement failed"; }
        if [ "$IDLE_WAIT" -gt 0 ]; then
          idle_try || stop 2 "HIDIdleTime unreadable (ioreg) after $n: '$idle'"
          if [ "$idle" -lt "$dur" ]; then
            [ -z "$retried" ] || { echo "disturbed=$n (input during the set: idle $idle s; the session's one retry is spent)" >> "$ss"; stop 3 "$n disturbed by input: the measurement failed"; }
            retried=1; echo "disturbed_retried=$n (input during the set: idle $idle s)" >> "$ss"
            idle_wait 300 "a disturbed set runs again" "$ss" || stop 2 "HIDIdleTime unreadable (ioreg) before $n's retry"
            continue
          fi
        fi
        cat "$SAMPLES" >> "$all"; echo "done=$n" >> "$ss"; break
      done
    done
  done
  SAMPLES="$all"
  echo "ended $(date '+%Y-%m-%d %H:%M:%S')" >> "$META"
  python3 -I "$root/tests/perf/perfstats.py" summary "$SAMPLES" > "$OUT/summary.txt"
  local req="$REQUIRED_FULL" rc m x; [ "$FORM" = short ] && req="$REQUIRED_SHORT"; [ -n "$QUICK" ] && req="${req//,P-H5.playlists,P-H5.tracks/}"
  # P-H2 for every model run and every file type of the form's corpus (the stubs: their own metrics)
  [ -n "$testmode" ] && req=P-H1.run.demucs4
  for m in $([ -z "$testmode" ] && echo demucs4) $([ -z "$testmode" ] && [ "$FORM" = full ] && [ -n "$MRB" ] && echo rb0002); do
    for x in $(corpus_files "$CORPUS" "$FORM" | sed 's/.*\.//' | tr 'A-Z' 'a-z' | sort -u); do req="$req,P-H2.$m.$x"; done
  done
  python3 -I "$root/tests/perf/perfstats.py" gate "$SAMPLES" --a A --b B --prefix P-H --require "$req" \
    --rules "$root/tests/perf/thresholds.txt" --out "$OUT/gate.tsv" > "$OUT/gate.txt"
  rc=$?
  echo "verdict=$rc" >> "$ss"
  cat "$OUT/gate.txt"
  if [ -n "$QUICK$testmode" ]; then echo "GP: not a valid gate (${QUICK:+quick}${testmode:+stubs}): tooling check only" | tee -a "$OUT/gate.txt"; [ "$rc" = 0 ] && rc=3; fi
  [ "$IDLE_WAIT" -gt 0 ] && idle_display end "$ss"
  echo "finished=$(date -u +%FT%TZ) rc=$rc" >> "$ss"
  if [ -n "$RESULTS" ]; then
    {
      printf '\n## GP, the helper (%s)\n\nhelper/tests/perf.sh gate, form %s, %s alternating pairs. A: `%s`; B: `%s`.\n\n```\n' \
        "$(date +%Y-%m-%d)" "$FORM" "$ROUNDS" "$(sed -n 2p "$a/IDENTITY-helper")" "$(sed -n 2p "$b/IDENTITY-helper")"
      cat "$OUT/gate.txt"; printf '```\n'
    } >> "$RESULTS"
  fi
  return "$rc"
}

cmd_selftest() {
  local t; t="$(cd "$(mktemp -d "${TMPDIR:-/tmp}/rbsp-hperf-self.XXXXXX")" && pwd -P)"
  say "selftest in $t"
  cmd_build "$t/A" || return 1
  ( cmd_corpus "$t/corpus" --quick ) || { echo "selftest: corpus failed"; return 1; }
  ( cmd_corpus "$t/corpus" ) || { echo "selftest: the corpus doesn't check against its sums"; return 1; }
  ( cmd_record "$t/A" "$t/rec" --corpus "$t/corpus" --quick ) || { echo "selftest: record failed"; return 1; }
  local m
  for m in P-H1.run.demucs4 P-H1.track.demucs4 P-H2.demucs4.mp3 P-H2.demucs4.wav P-H3.first P-H3.last P-H4.rss P-H4.size; do
    grep -q "^$m	" "$t/rec/samples.tsv" || { echo "selftest: no $m"; return 1; }
  done
  ( cmd_gate "$t/A" "$t/A" "$t/gate" --corpus "$t/corpus" --quick --rounds 1 ); local rc=$?
  [ "$rc" = 3 ] && grep -q "tooling check only" "$t/gate/gate.txt" || { echo "selftest: gate rc $rc"; return 1; }
  rm -rf "$t"
  say "selftest: ok"
}

case "${1:-}" in
  build) shift; cmd_build "$@" ;;
  corpus) shift; cmd_corpus "$@" ;;
  record) shift; cmd_record "$@" ;;
  gate) shift; cmd_gate "$@" ;;
  selftest) cmd_selftest ;;
  *) sed -n '2,/^set -u/p' "$0" | sed '$d'; exit 2 ;;
esac

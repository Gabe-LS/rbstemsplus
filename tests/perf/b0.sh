#!/bin/bash
# B0, the frozen baseline of the Mac bridge and helper (the private Windows plan §8.2, bridge.md §0b, helper.md
# §2a): the bridge and the helper at 3bf5a9f plus the timing commits, built ONCE into the gate
# archive and never rebuilt; then recorded once, on its own (bridge.md §0b: exclusive, nothing
# else running, the Mac left alone). B0's recorded numbers are a record for information: every
# gate runs B0's archived build again, interleaved with the candidate (owner decided, 2026-11-12).
#
#   tests/perf/b0.sh build [--base COMMIT]   B0 into gates-archive/B0 (refuses if it exists) and
#                                   the corpus into gates-archive/corpus-v1, then the checksums
#   tests/perf/b0.sh record --volume VOL     the run set: the bridge's (bridge/tests/perf.sh
#                                   record, the dedicated drive VOL for the cold part), then the
#                                   helper's (helper/tests/perf.sh record, full form), into
#                                   gates-archive/B0-records/<set>/; the checksums updated
#   tests/perf/b0.sh status                  the sets recorded, their days, what is missing
#   tests/perf/b0.sh verify                  gates-archive/SHA256SUMS checked
#   tests/perf/b0.sh copy DEST               the second copy (on another disk): DEST/gates-archive,
#                                   checked against the manifest
#
# The timing commits are this checkout's "bridge: test-only timing lines" and "helper: test-only
# timing lines" commits (found by subject; --bridge-timing/--helper-timing HASH to name them),
# applied as patches onto `git archive BASE`. gates-archive/ is at the top of this checkout
# (GATES_ARCHIVE to choose), git-ignored, outside make clean's reach. Run outside a sandbox, on AC
# power, with the other agents paused; the record keeps the Mac awake (caffeinate) while it runs.
set -u
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
A="${GATES_ARCHIVE:-$root/gates-archive}"
die() { echo "b0.sh: $*" >&2; exit 2; }
say() { echo "[$(date +%H:%M:%S)] $*"; }

# the archive's seal and verify, shared with mac-a.sh (they print their own message and return 2)
# shellcheck source=archive.sh
. "$root/tests/perf/archive.sh"

cmd_build() {
  local base=3bf5a9f bt= ht=
  while [ $# -gt 0 ]; do
    case "$1" in
      --base) base="$2"; shift ;;
      --bridge-timing) bt="$2"; shift ;;
      --helper-timing) ht="$2"; shift ;;
      *) die "unknown option $1" ;;
    esac
    shift
  done
  [ ! -e "$A/B0" ] || die "$A/B0 exists: B0 is built once and never rebuilt"
  [ -n "$bt" ] || bt="$(git -C "$root" log --format=%H -1 --grep='^bridge: test-only timing lines')"
  [ -n "$ht" ] || ht="$(git -C "$root" log --format=%H -1 --grep='^helper: test-only timing lines')"
  [ -n "$bt" ] && [ -n "$ht" ] || die "the timing commits aren't in this checkout"
  base="$(git -C "$root" rev-parse --verify "$base^{commit}")" || die "no commit $base"
  s="$(mktemp -d "${TMPDIR:-/tmp}/rbsp-b0.XXXXXX")" || { echo "b0.sh: no scratch folder" >&2; exit 2; }
  s="$(cd "$s" && pwd -P)" && [ -n "$s" ] && [ -d "$s" ] || { echo "b0.sh: no scratch folder" >&2; exit 2; }
  trap 'rm -rf "$s"' EXIT
  say "B0's source: $base + $bt (bridge) + $ht (helper)"
  mkdir -p "$s/src"
  git -C "$root" archive "$base" bridge helper | tar -x -C "$s/src" || die "git archive $base"
  git -C "$root" diff "$bt^" "$bt" -- bridge > "$s/bridge-timing.patch"
  git -C "$root" diff "$ht^" "$ht" -- helper > "$s/helper-timing.patch"
  (cd "$s/src" && patch -s -p1 < "$s/bridge-timing.patch" && patch -s -p1 < "$s/helper-timing.patch") || die "the timing commits don't apply to $base"
  # the pinned downloads this checkout already has (each build checks their sha256 again)
  local d
  for d in flac sqlcipher pyrekordbox; do [ -d "$root/build/$d" ] && mkdir -p "$s/src/build" && cp -R "$root/build/$d" "$s/src/build/"; done
  rm -rf "$s/src/build/flac/1.5.0-"* 2> /dev/null
  mkdir -p "$A/B0"
  local desc="$base (git archive) + bridge timing $bt + helper timing $ht"
  RBSP_PERF_SRC="$s/src" RBSP_PERF_SRC_DESC="$desc" "$root/bridge/tests/perf.sh" build "$A/B0" || { rm -rf "$A/B0"; die "the bridge didn't build"; }
  RBSP_PERF_SRC="$s/src" RBSP_PERF_SRC_DESC="$desc" "$root/helper/tests/perf.sh" build "$A/B0" || { rm -rf "$A/B0"; die "the helper didn't build"; }
  cp "$s/bridge-timing.patch" "$s/helper-timing.patch" "$A/B0/"
  {
    echo "B0: the Mac bridge and helper, the anchor of P-B and P-H (the private Windows plan §8.2). Never rebuilt."
    echo "base: $base"
    echo "bridge timing commit: $bt"
    echo "helper timing commit: $ht"
    echo "the bridge loads Pioneer's library from $A/B0/ort: it runs only at this path"
  } > "$A/B0/SOURCE"
  "$root/helper/tests/perf.sh" corpus "$A/corpus-v1" || die "the corpus"
  mkdir -p "$A/B0-records"
  seal || exit 2
  say "B0 built into $A/B0; corpus in $A/corpus-v1; $A/SHA256SUMS written"
}

cmd_record() {
  local vol= set
  while [ $# -gt 0 ]; do
    case "$1" in
      --volume) vol="$2"; shift ;;
      *) die "unknown option $1" ;;
    esac
    shift
  done
  [ -n "$vol" ] || die "--volume VOL: the dedicated drive (with RBSP-PERF-SCRATCH at its root)"
  # B0_TEST=1 (tests/harness/selftest.sh only): B0_BRIDGE_PERF and B0_HELPER_PERF replace the two
  # perf.sh scripts; a real record refuses them
  local bperf="$root/bridge/tests/perf.sh" hperf="$root/helper/tests/perf.sh"
  if [ "${B0_TEST:-}" = 1 ]; then
    bperf="${B0_BRIDGE_PERF:?B0_TEST needs B0_BRIDGE_PERF}"; hperf="${B0_HELPER_PERF:?B0_TEST needs B0_HELPER_PERF}"
  else
    [ -z "${B0_BRIDGE_PERF:-}${B0_HELPER_PERF:-}" ] || die "B0_BRIDGE_PERF and B0_HELPER_PERF are the selftest's (B0_TEST=1)"
  fi
  verify || exit 2
  set="$(date +%Y%m%d-%H%M)"
  # RBSP_B0_SMOKE=1: this script's own test (a disk image, tiny counts): never a B0 record
  local xb=() xh=()
  if [ "${RBSP_B0_SMOKE:-}" = 1 ]; then xb=(--quick --image --no-purge); xh=(--quick); set="smoke-$set"; fi
  # recorded into <set>.partial: it becomes <set> (and counts) only once both workloads succeeded
  # with no WARNING in their meta files; a failed set stays .partial, outside the manifest
  local out="$A/B0-records/$set" part="$A/B0-records/$set.partial"
  [ ! -e "$out" ] && [ ! -e "$part" ] || die "$out exists already"
  mkdir -p "$part" || die "can't make $part"
  caffeinate -dims -w $$ &
  say "B0 run set $set: the bridge"
  "$bperf" record "$A/B0" "$part/bridge" --volume "$vol" --set "$set" ${xb[@]+"${xb[@]}"} || die "the bridge's run set failed (see $part/bridge)"
  say "B0 run set $set: the helper"
  "$hperf" record "$A/B0" "$part/helper" --corpus "$A/corpus-v1" --form full --set "$set" ${xh[@]+"${xh[@]}"} || die "the helper's run set failed (see $part/helper)"
  [ -s "$part/bridge/samples.tsv" ] && [ -s "$part/helper/samples.tsv" ] || die "a workload left no samples (see $part): not a run set"
  if grep -l WARNING "$part"/bridge/meta-*.txt "$part"/helper/meta-*.txt 2> /dev/null; then
    die "WARNING in the meta file(s) above: a short workload, not a run set (kept in $part)"
  fi
  mv "$part" "$out" || die "moving $part to $out"
  seal || exit 2
  say "B0 run set $set recorded into $out"
  cmd_status
}

cmd_status() {
  [ -d "$A/B0-records" ] || die "no B0 records yet ($A/B0-records)"
  # a set counts when it is complete: <YYYYMMDD-HHMM>, no .partial, both samples files there
  local sets=0 days d done=()
  for d in $(ls "$A/B0-records" | grep '^[0-9]\{8\}-[0-9]\{4\}$'); do
    [ -s "$A/B0-records/$d/bridge/samples.tsv" ] && [ -s "$A/B0-records/$d/helper/samples.tsv" ] && done+=("$d")
  done
  sets=${#done[@]}
  days="$(printf '%s\n' ${done[@]+"${done[@]}"} | cut -c1-8 | grep . | sort -u | wc -l | tr -d ' ')"
  echo "B0 run sets: $sets (on $days dates); the plan asks for one, a record for information"
  ls "$A/B0-records" | while read -r d; do
    case " ${done[*]:-} " in *" $d "*) echo "  $d" ;; *) echo "  $d (not counted: incomplete, failed or a smoke test)" ;; esac
  done
  local all; all="$(mktemp "${TMPDIR:-/tmp}/rbsp-b0-all.XXXXXX")"
  for d in ${done[@]+"${done[@]}"}; do cat "$A/B0-records/$d/bridge/samples.tsv" "$A/B0-records/$d/helper/samples.tsv"; done | grep -v '^#' > "$all"
  [ -s "$all" ] && python3 -I "$root/tests/perf/perfstats.py" summary "$all"
  rm -f "$all"
  [ "$sets" -ge 1 ] && echo "B0: complete" || echo "B0: incomplete"
}

cmd_copy() {
  [ $# -eq 1 ] || die "usage: b0.sh copy DEST"
  verify || exit 2
  [ "$(stat -f %d "$1")" != "$(stat -f %d "$A")" ] || die "$1 is on the same disk as $A: the second copy goes on another disk"
  mkdir -p "$1/gates-archive" && rsync -a --delete "$A/" "$1/gates-archive/" || die "copying to $1"
  (cd "$1/gates-archive" && shasum -a 256 -c --quiet SHA256SUMS) || die "the copy in $1 doesn't match"
  say "copied and checked: $1/gates-archive"
}

case "${1:-}" in
  build) shift; cmd_build "$@" ;;
  record) shift; cmd_record "$@" ;;
  status) cmd_status ;;
  verify) verify || exit 2; echo "gate archive: checksums match" ;;
  copy) shift; cmd_copy "$@" ;;
  *) sed -n '2,/^set -u/p' "$0" | sed '$d'; exit 2 ;;
esac

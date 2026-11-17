#!/bin/bash
# Records A's traces, the oracle (mac-core.md §2.3-2.4): each scenario run twice in the tart VM;
# kept only when the two runs give the same frame sequences (or, for "compare: properties", the
# same properties) and the invariants hold. Run outside the sandbox (tart, ssh).
#
#   tests/harness/record.sh SCENARIO…      (perf-* scenarios are left out: they aren't traces)
# Into tests/harness/traces/A/<scenario>[@verb].jsonl, with the build's sha256 in
# tests/harness/traces/A/BUILD; the runs stay in build/harness/results/record-<date>/.
set -uo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
vm="$root/tests/harness/vm.sh"; tr="$root/tests/harness/trace.py"
label="record-$(date +%Y%m%d-%H%M%S)"
dest="$root/tests/harness/traces/A"; mkdir -p "$dest"
res="$root/build/harness/results/$label"
# --accept LABEL SCENARIO…: the checks again on runs already made (after a scenario's header changed)
accept=""
if [ "${1:-}" = --accept ]; then accept=1; res="$root/build/harness/results/$2"; shift 2; fi
# a run's traces in the order their app copies started (run.sh's traces.txt; else by pid)
started() {
  if [ -s "$1/traces.txt" ]; then sed "s|^|$1/|" "$1/traces.txt"
  else ls "$1"/app-*.jsonl 2>/dev/null | sed 's/.*app-\([0-9]*\)\.jsonl$/\1 &/' | sort -n | cut -d' ' -f2-; fi
}
rc=0
shas=""          # the app each recorded trace was made with (its runs' build.txt)
for s in "$@"; do
  case "$s" in perf-*|g1-*) echo "skip $s (a perf scenario or a G1 check: not a trace)"; continue ;; esac
  [ -n "$accept" ] || "$vm" run "$label" "$s" -n 2 || { echo "RECORD $s: a run failed (see $res/$s*/*/result)"; rc=1; continue; }
  for d in "$res/$s" "$res/$s"@*; do
    [ -d "$d" ] || continue
    name="$(basename "$d")"
    # every copy of the app a run started (a second copy, the new app of an update, a second
    # launch), by pid: the first launched is the scenario's trace, the k-th <name>.p<k>.jsonl
    ta=(); tb=()
    while read -r x; do [ -n "$x" ] && ta+=("$x"); done <<< "$(started "$d/1")"
    while read -r x; do [ -n "$x" ] && tb+=("$x"); done <<< "$(started "$d/2")"
    if [ ${#ta[@]} = 0 ] || [ ${#ta[@]} != ${#tb[@]} ]; then echo "RECORD $name: no trace, or its two runs started different numbers of app copies (${#ta[@]}, ${#tb[@]})"; rc=1; continue; fi
    a="${ta[0]}"; b="${tb[0]}"
    # an app that was never active (a locked screen) shows nothing a user would see
    if ! grep -q '"app.active":"1"' "$a" || ! grep -q '"app.active":"1"' "$b"; then echo "RECORD $name: the app never became active (screen locked?)"; rc=1; continue; fi
    # the invariants, the order pairs and the must-show states hold in every run, not only the kept one
    bad=""
    for k in "${!ta[@]}"; do
      "$tr" check "${ta[k]}" --scenario "$s" --part $((k + 1)) > "$d/check-$k.txt" && "$tr" check "${tb[k]}" --scenario "$s" --part $((k + 1)) > "$d/check2-$k.txt" || bad=1
    done
    if [ -n "$bad" ]; then echo "RECORD $name: invariants fail (see $d/check*.txt)"; rc=1; continue; fi
    # run 2 against run 1 as B against A; else run 1 against run 2: an allowed field may have lost a
    # transient state in either run, and the run that kept more is kept as A (B is held to A's).
    # One run is kept for all of its traces.
    keep=1
    for k in "${!ta[@]}"; do "$tr" compare "${ta[k]}" "${tb[k]}" --scenario "$s" --part $((k + 1)) > "$d/compare-$k.txt" || keep=""; done
    if [ -z "$keep" ]; then
      keep=2
      for k in "${!ta[@]}"; do "$tr" compare "${tb[k]}" "${ta[k]}" --scenario "$s" --part $((k + 1)) > "$d/compare-reverse-$k.txt" || keep=""; done
    fi
    if [ -z "$keep" ]; then echo "RECORD $name: the two runs differ (see $d/compare-*.txt)"; rc=1; continue; fi
    sa="$(sed -n 's/^app_sha256=//p' "$d/1/build.txt" 2>/dev/null)"; sb="$(sed -n 's/^app_sha256=//p' "$d/2/build.txt" 2>/dev/null)"
    if [ -z "$sa" ] || [ "$sa" != "$sb" ]; then echo "RECORD $name: its two runs don't name one app build (build.txt)"; rc=1; continue; fi
    shas="$shas $sa"
    rm -f "$dest/$name".p*.jsonl "$dest/$name".p*.dialogs.json
    for k in "${!ta[@]}"; do
      src="${ta[k]}"; [ "$keep" = 2 ] && src="${tb[k]}"
      suf=""; [ "$k" -gt 0 ] && suf=".p$((k + 1))"
      cp "$src" "$dest/$name$suf.jsonl"
      "$tr" dialogs "$src" --scenario "$s" > "$dest/$name$suf.dialogs.json"
    done
    [ -f "$d/1/fields.tsv" ] && cp "$d/1/fields.tsv" "$root/tests/harness/fields.tsv"
    echo "RECORDED $name (${#ta[@]} trace$([ ${#ta[@]} -gt 1 ] && echo s), run $keep kept)"
  done
done
# BUILD names the app the traces were recorded with: only when something was recorded, and only
# when every recorded trace names the same build (traces of two builds must not share one BUILD)
if [ -n "$shas" ]; then
  one="$(printf '%s\n' $shas | sort -u)"
  if [ "$(printf '%s\n' "$one" | wc -l | tr -d ' ')" = 1 ]; then printf '%s\n' "$one" > "$dest/BUILD"
  else echo "RECORD: the traces name different app builds: BUILD not written"; rc=1; fi
fi
exit $rc

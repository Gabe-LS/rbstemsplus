#!/bin/bash
# The core's Mac input lists (review SO7; make core-test): core/build.sh lib and ids write, from
# their compiles' clang depfiles, the repository files that reach the archives' and CoreIds.swift's
# bytes. The Mac app's input list takes these in place of all of core/, so a commit that touches
# only Windows-only or test-only files of core/ changes no Mac input. Checked here:
#   - both lists exist, every line is a repository file, and they hold what the Mac build reads
#     (core.h, a Mac source, a header, the .def tables, gen_ids.c, core/build.sh);
#   - none holds a Windows-only file (win.c, native.c, flows_win.c, core_win.h, win/), a test
#     (core/tests/) or a tool the Mac build doesn't run (gen_mask_tables.c);
#   - through scripts/mac-inputs.py changed: a commit touching only core/win.c, core/flows_win.c and
#     core/tests/ changes no Mac input; one touching core/flows.c or a .def table does.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
t="${TMPDIR:-/tmp}"
tmp="$(mktemp -d "${t%/}/rbsp-core-inputs.XXXXXX")" || { echo "mac-inputs.sh: no scratch folder" >&2; exit 2; }
tmp="$(cd "$tmp" && pwd -P)" && [ -n "$tmp" ] && [ -d "$tmp" ] || { echo "mac-inputs.sh: no scratch folder" >&2; exit 2; }
trap 'rm -rf "${tmp:?}"' EXIT
fails=0 passes=0
ok() { passes=$((passes + 1)); }
bad() { fails=$((fails + 1)); echo "FAIL: $*"; }

"$root/core/build.sh" lib "$tmp/lib" > /dev/null
"$root/core/build.sh" ids "$tmp/ids" > /dev/null
lib="$tmp/lib/libcore.inputs" ids="$tmp/ids/CoreIds.swift.inputs"
for l in "$lib" "$ids"; do
  if [ -s "$l" ]; then ok; else bad "$(basename "$l") is missing or empty"; continue; fi
  while IFS= read -r p; do
    if [ -f "$root/$p" ]; then ok; else bad "$(basename "$l"): $p isn't a repository file"; fi
  done < "$l"
  for p in core/win.c core/native.c core/flows_win.c core/core_win.h core/tools/gen_mask_tables.c; do
    if grep -qxF "$p" "$l"; then bad "$(basename "$l") lists $p (not a Mac input)"; else ok; fi
  done
  if grep -qE '^(core/tests/|win/)' "$l"; then bad "$(basename "$l") lists a test or a win/ file: $(grep -E '^(core/tests/|win/)' "$l" | head -3 | tr '\n' ' ')"; else ok; fi
  if grep -qxF core/build.sh "$l"; then ok; else bad "$(basename "$l") doesn't list core/build.sh"; fi
done
for p in core/core.h core/flows.c core/internal.h core/texts.def core/schema.def; do
  if grep -qxF "$p" "$lib"; then ok; else bad "libcore.inputs doesn't list $p"; fi
done
for p in core/tools/gen_ids.c core/core.h core/schema.def core/texts.def core/menus.def core/facts.def; do
  if grep -qxF "$p" "$ids"; then ok; else bad "CoreIds.swift.inputs doesn't list $p"; fi
done

# a commit's changed paths against both lists (mac-inputs.py changed: 0 none on a list, 1 some)
changed() {   # expected-exit path...
  local want="$1" rc=0
  shift
  printf '%s\0' "$@" | python3 -I "$root/scripts/mac-inputs.py" changed "$lib" "$ids" > /dev/null || rc=$?
  if [ "$rc" -eq "$want" ]; then ok; else bad "mac-inputs.py changed $* gave $rc, not $want"; fi
}
changed 0 core/win.c core/flows_win.c core/native.c core/tests/unit.c core/tests/flows_win_test.c core/API-V1-NOTES.md
changed 1 core/flows.c
changed 1 core/menus.def

# review CF3: a depfile the parser refuses (an OUT with a newline in its name) fails ids, and the
# list from before stays (not replaced by a list of core/build.sh alone)
nl="$tmp/ids
nl"
mkdir -p "$nl"
printf 'before\n' > "$nl/CoreIds.swift.inputs"
if "$root/core/build.sh" ids "$nl" > /dev/null 2>&1; then bad "ids into an OUT with a newline in its name succeeded"; else ok; fi
if [ "$(cat "$nl/CoreIds.swift.inputs")" = before ]; then ok; else bad "ids replaced the list it couldn't make"; fi
# review OM4: the list can't be written (its .tmp a folder): ids fails, and the list from before stays
wr="$tmp/ids-unwritable"
mkdir -p "$wr/CoreIds.swift.inputs.tmp"
printf 'before\n' > "$wr/CoreIds.swift.inputs"
if "$root/core/build.sh" ids "$wr" > /dev/null 2>&1; then bad "ids succeeded when its list couldn't be written"; else ok; fi
if [ "$(cat "$wr/CoreIds.swift.inputs")" = before ]; then ok; else bad "ids replaced the list it couldn't write"; fi

echo "mac-inputs: $passes passed, $fails failed ($(wc -l < "$lib" | tr -d ' ') files for libcore.a, $(wc -l < "$ids" | tr -d ' ') for CoreIds.swift)"
[ "$fails" -eq 0 ]

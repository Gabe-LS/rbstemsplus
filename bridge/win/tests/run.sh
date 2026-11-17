#!/bin/sh
# The Windows bridge's platform-neutral tests, on the Mac (bridge.md §4 "On the Mac"): the refusal
# stub's completeness (stub.c: every OrtApi slot typed, filled and called through the typed struct,
# none allocating; J3) and ort_win.h's checks (ort.c), each plain and with ASan and UBSan; and the
# PE table parsers of build.sh's export and import gates (pe.sh) on captured tool text (pe/).
#
#   bridge/win/tests/run.sh [OUT]     default build/bridge-win-tests; exits 1 on a failure
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
win="$(cd "$here/.." && pwd)"
root="$(cd "$win/../.." && pwd)"
out="${1:-$root/build/bridge-win-tests}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
WARN="-std=c11 -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Werror -mmacosx-version-min=12.0"
SAN="-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined"
fails=0
for t in stub ort; do
  # shellcheck disable=SC2086
  (cd "$here" && clang $WARN -O2 -I.. -I../.. "$t.c" -o "$out/$t" && clang $WARN $SAN -I.. -I../.. "$t.c" -o "$out/$t-asan")
  if "$out/$t" > "$out/$t.log" 2>&1 && "$out/$t-asan" >> "$out/$t.log" 2>&1; then echo "ok   $(head -1 "$out/$t.log")"
  else cat "$out/$t.log"; echo "FAIL $t"; fails=$((fails + 1)); fi
done

# build.sh's export and import gates (bridge/win/pe.sh), on the pinned tools' text (tests/pe/):
# the release DLL, the 1.17.0 fake and harness.exe as built; the fake linked with a 5th export
# whose name has a digit, a NONAME one and a forwarded one; a harness linked with a by-name import
# next to the ordinals (lld folds ONNXRUNTIME.DLL into the onnxruntime.dll descriptor); and, edited,
# a second descriptor named ONNXRUNTIME.DLL, "onnxruntime" or "onnxruntime.dll.", and the ordinals
# under onnxruntime_dll
. "$win/pe.sh"
pe="$here/pe"
checks=0 pefails=0
expect() {   # WHAT WANT GOT
  checks=$((checks + 1))
  [ "$3" = "$2" ] || { echo "FAIL pe gates: $1: got '$3', want '$2'"; pefails=$((pefails + 1)); }
}
fake="1:OrtGetApiBase 2:OrtSessionOptionsAppendExecutionProviderEx_DML 3:OrtSessionOptionsAppendExecutionProvider_CPU 4:OrtSessionOptionsAppendExecutionProvider_DML"
expect "release exports" "$fake 5:rbstems_marker" "$(pe_exports "$pe/exports-release.txt")"
expect "fake exports" "$fake" "$(pe_exports "$pe/exports-fake.txt")"
expect "a name with a digit" "$fake 5:fake_ort_marker2" "$(pe_exports "$pe/exports-digit.txt")"
expect "NONAME" "$fake 5:-" "$(pe_exports "$pe/exports-noname.txt")"
expect "stdin" "$fake 5:-" "$(pe_exports < "$pe/exports-noname.txt")"
got="$(pe_exports "$pe/exports-forward.txt" 2> "$out/pe-forward.err")" && st=0 || st=$?
expect "a forwarder: refused" "1 ''" "$st '$got'"
expect "a forwarder: says why" "1" "$(grep -c 'forwarded or unknown Export Table row: .*forwarded to KERNEL32.GetTickCount' "$out/pe-forward.err")"
expect "harness imports" "(1) (2) (3) (4)" "$(pe_imports onnxruntime.dll "$pe/imports-harness.txt")"
expect "stdin" "(1) (2) (3) (4)" "$(pe_imports onnxruntime.dll < "$pe/imports-harness.txt")"
expect "a by-name import" "(1) OrtGetApiBaseByName (7) (2) (3) (4)" "$(pe_imports onnxruntime.dll "$pe/imports-byname.txt")"
expect "a second descriptor, upper case" "(1) (2) (3) (4) OrtGetApiBase (0)" "$(pe_imports onnxruntime.dll "$pe/imports-case.txt")"
expect "a literal dot" "" "$(pe_imports onnxruntime.dll "$pe/imports-dot.txt")"
# BX8: the loader's reading of a name: ".dll" appended with no dot, trailing dots stripped; both
# fail build.sh's ordinal-only check ("(1) (2) (3) (4)" alone)
expect "a name with no extension" "(1) (2) (3) (4) OrtGetApiBase (0)" "$(pe_imports onnxruntime.dll "$pe/imports-noext.txt")"
expect "a trailing dot" "(1) (2) (3) (4) OrtGetApiBase (0)" "$(pe_imports onnxruntime.dll "$pe/imports-trailingdot.txt")"
if [ "$pefails" = 0 ]; then echo "ok   pe gates: $checks checks, 0 failed"; else fails=$((fails + 1)); fi
[ "$fails" = 0 ] || exit 1

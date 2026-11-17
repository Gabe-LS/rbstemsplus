#!/bin/bash
# Tests of scripts/mac-inputs.py (the Mac input lists, release.md §1.3): deps on depfiles written
# by clang itself (absolute paths with spaces, a folder outside the repository, build/), its
# refusal of a relative path unless --cwd names the compile's folder; and changed on a list.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
mi="$root/scripts/mac-inputs.py"
tmp="$(mktemp -d "${TMPDIR:-/tmp}/rbsp-mac-inputs-test.XXXXXX")"
tmp="$(cd "$tmp" && pwd -P)"
trap 'rm -rf "${tmp:?}"' EXIT
pass=0; fail=0
ok() { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "FAIL: $*"; }
# expect NAME RC WANT CMD...: CMD exits with RC and prints WANT (its stdout, lines joined by spaces)
expect() {
  local name="$1" want_rc="$2" want="$3" rc=0 got; shift 3
  got="$("$@" 2> "$tmp/err" | tr '\n' ' ')" || rc=$?
  got="${got% }"
  if [ "$rc" = "$want_rc" ] && [ "$got" = "$want" ]; then ok
  else bad "$name: exit $rc, '$got' (expected $want_rc, '$want'); $(tr '\n' ' ' < "$tmp/err")"; fi
}

# a repository with a space in its path, a header in a sibling folder, a generated header in build/
r="$tmp/a repo"
mkdir -p "$r/helper" "$r/bridge" "$r/build/gen" "$tmp/outside"
echo '#include "x.h"' > "$r/helper/a.c"
printf '#include "onnx.h"\n#include "gen.h"\n#include "out.h"\n' > "$r/helper/x.h"
echo '/* api */' > "$r/bridge/onnx.h"
echo '/* generated */' > "$r/build/gen/gen.h"
echo '/* elsewhere */' > "$r/outside.h"; mv "$r/outside.h" "$tmp/outside/out.h"
# absolute: what helper/build.sh and bridge/build.sh do
xcrun clang -c -I"$r/bridge" -I"$r/build/gen" -I"$tmp/outside" -MD -MF "$tmp/abs.d" "$r/helper/a.c" -o "$tmp/abs.o" 2> "$tmp/cc.log" \
  || { cat "$tmp/cc.log"; echo "mac-inputs-test: clang failed"; exit 2; }
expect "deps, absolute paths" 0 "bridge/onnx.h helper/a.c helper/x.h" python3 -I "$mi" deps "$r" "$tmp/abs.d"
# relative: compiled from inside helper/ (as core/build.sh compiles)
(cd "$r/helper" && xcrun clang -c -I../bridge -I../build/gen -I"$tmp/outside" -MD -MF "$tmp/rel.d" a.c -o "$tmp/rel.o") 2> "$tmp/cc.log" \
  || { cat "$tmp/cc.log"; echo "mac-inputs-test: clang failed"; exit 2; }
expect "deps, relative paths: refused" 2 "" python3 -I "$mi" deps "$r" "$tmp/rel.d"
grep -q "a relative path" "$tmp/err" && ok || bad "deps, relative paths: no reason given"
expect "deps, relative paths with --cwd" 0 "bridge/onnx.h helper/a.c helper/x.h" python3 -I "$mi" deps --cwd "$r/helper" "$r" "$tmp/rel.d"
expect "deps, --cwd not a folder" 2 "" python3 -I "$mi" deps --cwd "$tmp/nothing" "$r" "$tmp/rel.d"

# changed: exact entries and folders (a trailing /)
printf '%s\n' app/Sources/ bridge/build.sh helper/a.c > "$tmp/list"
printf '%s\0' VERSION docs/x.md app/Sources/New.swift bridge/build.sh > "$tmp/names"
expect "changed: the listed ones" 1 "app/Sources/New.swift bridge/build.sh" sh -c 'python3 -I "$1" changed "$2" < "$3"' _ "$mi" "$tmp/list" "$tmp/names"
printf '%s\0' VERSION docs/x.md win/a tests/b app/SourcesX.swift > "$tmp/names"
expect "changed: none listed" 0 "" sh -c 'python3 -I "$1" changed "$2" < "$3"' _ "$mi" "$tmp/list" "$tmp/names"
: > "$tmp/empty"
expect "changed: an empty list is an error" 2 "" sh -c 'python3 -I "$1" changed "$2" < "$3"' _ "$mi" "$tmp/empty" "$tmp/names"

# deps: a depfile line that isn't a rule (a name cut by a newline) and a name with a newline in
# it are refused: a list holds one path per line
printf '%s: %s/helper/new\n' "$tmp/x.o" "$(echo "$r" | sed 's/ /\\ /g')" > "$tmp/nl.d"; printf 'line.c\n' >> "$tmp/nl.d"
expect "deps, a name cut by a newline: refused" 2 "" python3 -I "$mi" deps "$r" "$tmp/nl.d"
grep -q "isn't a rule" "$tmp/err" && ok || bad "deps, a name cut by a newline: no reason given"

# git's quoted names (Q43): with core.quotepath=true, as git's default line output does, a
# non-ASCII name or one with a quote is written "app/Sources/Caf\303\251.swift"; -z isn't quoted
gq() { git -c core.quotepath=true -c init.defaultBranch=main -c user.name=T -c user.email=t@example.invalid -C "$q" "$@"; }
q="$tmp/q"
mkdir -p "$q/app/Sources" "$q/helper"
echo "// the app" > "$q/app/Sources/main.swift"; echo 'int a;' > "$q/helper/a.c"
gq init -q && gq add -A && gq commit -q -m base || { echo "mac-inputs-test: git failed"; exit 2; }
qbase="$(gq rev-parse HEAD)"
printf '%s\n' app/Sources/ helper/a.c > "$tmp/qlist"
cafe="$(printf 'app/Sources/Caf\303\251.swift')"          # NFC é, as typed
echo "// é" > "$q/$cafe"; gq add -A && gq commit -q -m cafe; c1="$(gq rev-parse HEAD)"
echo '// "' > "$q/app/Sources/we\"ird.swift"; gq add -A && gq commit -q -m weird; c2="$(gq rev-parse HEAD)"
nlname="$(printf 'app/Sources/new\nline.swift')"
echo '// nl' > "$q/$nlname"; gq add -A && gq commit -q -m newline; c3="$(gq rev-parse HEAD)"
gq diff --name-only "$qbase" "$c1" | grep -qF '"app/Sources/Caf\303\251.swift"' && ok || bad "git didn't quote the name (the case this guards against)"
qchanged() { gq diff -z --name-only --no-renames "$1" "$2" | python3 -I "$mi" changed "$tmp/qlist"; }
expect "changed: a non-ASCII name (-z)" 1 "$cafe" qchanged "$qbase" "$c1"
expect "changed: a name with a quote (-z)" 1 'app/Sources/we"ird.swift' qchanged "$c1" "$c2"
expect "changed: a name with a newline is still a hit, escaped" 1 "'app/Sources/new\\nline.swift'" qchanged "$c2" "$c3"
# and what the quoted line output gave before: no hit at all
got_rc=0; gq diff --name-only --no-renames "$qbase" "$c1" | tr '\n' '\0' | python3 -I "$mi" changed "$tmp/qlist" > /dev/null || got_rc=$?
[ "$got_rc" = 0 ] && ok || bad "the quoted line output matched (the test no longer shows the old failure)"
# a non-ASCII name from a real depfile (a git archive copy, compiled) is what git diff -z names
gq rm -q "$nlname" && gq commit -q -m "no newline"
cafec="$(printf 'helper/Caf\303\251.c')"
echo 'int cafe;' > "$q/$cafec"; gq add -A && gq commit -q -m "a C file"; c4="$(gq rev-parse HEAD)"
mkdir -p "$tmp/qa"; gq archive --format=tar "$c4" | tar -x -f - -C "$tmp/qa"
xcrun clang -c -MD -MF "$tmp/qa.d" "$tmp/qa/$cafec" -o "$tmp/qa.o" 2> "$tmp/cc.log" || { cat "$tmp/cc.log"; echo "mac-inputs-test: clang failed"; exit 2; }
python3 -I "$mi" deps "$tmp/qa" "$tmp/qa.d" > "$tmp/qa.list"
gq diff -z --name-only "$c4^" "$c4" | tr '\0' '\n' | grep -v '^$' > "$tmp/qa.diff"
[ -s "$tmp/qa.list" ] && cmp -s "$tmp/qa.list" "$tmp/qa.diff" && ok || bad "deps' non-ASCII name isn't git's: $(od -c "$tmp/qa.list" | head -2) / $(od -c "$tmp/qa.diff" | head -2)"
expect "changed: the depfile's non-ASCII name matches its git diff" 1 "$cafec" sh -c 'git -c core.quotepath=true -C "$1" diff -z --name-only "$2" "$3" | python3 -I "$4" changed "$5"' _ "$q" "$c4^" "$c4" "$mi" "$tmp/qa.list"

echo "mac-inputs-test: $pass passed, $fail failed"
[ "$fail" = 0 ]

#!/bin/bash
# helper/build.sh's and bridge/build.sh's input lists (scripts/mac-inputs.py deps), as
# core/tests/mac-inputs.sh checks core/build.sh's: when the parser fails, or gives no list, the
# build fails and the list from before stays (not replaced by an empty or partial one). A python3
# first on PATH stands in for the parser only (every other python3 call goes to the real one):
# "fail" exits 2, "empty" prints nothing, "pass" is the real parser with the list's .tmp made a
# folder (its write fails). The helper is checked all three ways, the bridge (which builds libFLAC
# every time) once; about a minute in all.
set -uo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
tmp="$(mktemp -d "${TMPDIR:-/tmp}/rbsp-build-lists.XXXXXX")" || { echo "build-lists-test: no scratch folder" >&2; exit 2; }
tmp="$(cd "$tmp" && pwd -P)" && [ -n "$tmp" ] && [ -d "$tmp" ] || { echo "build-lists-test: no scratch folder" >&2; exit 2; }
trap 'rm -rf "${tmp:?}"' EXIT
pass=0 fail=0
ok() { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "FAIL: $*"; }

real="$(command -v python3)" || { echo "build-lists-test: no python3"; exit 2; }
mkdir -p "$tmp/bin"
cat > "$tmp/bin/python3" <<EOF
#!/bin/bash
[ "\${BUILD_LISTS_PARSER:-}" = pass ] && exec "$real" "\$@"
for a in "\$@"; do
  case "\$a" in
    */scripts/mac-inputs.py)
      [ "\$BUILD_LISTS_PARSER" = empty ] && exit 0
      echo "build-lists-test: the parser fails" >&2; exit 2 ;;
  esac
done
exec "$real" "\$@"
EOF
chmod +x "$tmp/bin/python3"

# check WHAT LIST MODE WANT: WHAT/build.sh with the parser MODE fails with WANT in its log, LIST kept
check() {
  local what="$1" list="$2" mode="$3" want="$4" out="$tmp/$1-$3"
  mkdir -p "$out"
  printf 'before\n' > "$out/$list"
  [ "$mode" = pass ] && mkdir "$out/$list.tmp"
  if BUILD_LISTS_PARSER="$mode" PATH="$tmp/bin:$PATH" "$root/$what/build.sh" "$out" > "$tmp/$what-$mode.log" 2>&1; then
    bad "$what/build.sh succeeded with the parser $mode"
  else ok; fi
  if grep -q "$want" "$tmp/$what-$mode.log"; then ok
  else bad "$what/build.sh ($mode) didn't stop at its input list: $(tail -3 "$tmp/$what-$mode.log" | tr '\n' ' ')"; fi
  if [ "$(cat "$out/$list")" = before ]; then ok; else bad "$what/build.sh ($mode) replaced $list when it couldn't make it"; fi
}
check helper rbsp-prepare.inputs fail "the depfiles of rbsp-prepare couldn't be read"
check helper rbsp-prepare.inputs empty "no input list for rbsp-prepare"
check helper rbsp-prepare.inputs pass "Is a directory"   # the parser works, the list can't be written (its .tmp a folder)
check bridge libonnxruntime.1.18.0.dylib.inputs fail "the depfiles of libonnxruntime.1.18.0.dylib couldn't be read"
echo "build-lists: $pass passed, $fail failed"
[ "$fail" -eq 0 ]

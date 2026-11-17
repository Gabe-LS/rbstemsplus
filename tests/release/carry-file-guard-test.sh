#!/bin/bash
# Tests scripts/build-windows.sh's --carry-file guard (TB5, BR2): a carried file that resolves into a
# folder the build empties (here the work folder, RBSP_WIN_WORK) is refused before anything is
# built or deleted, whether the path names it directly, through a symlink in its last part, or
# through a symlinked parent; a file elsewhere passes the guard. The guard runs before any
# compiler, so no toolchain is needed (the passing case stops at a work folder that can't be made).
#
#   tests/release/carry-file-guard-test.sh
set -uo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
t="$(cd "$(mktemp -d "${TMPDIR:-/tmp}/rbsp-carry-guard.XXXXXX")" && pwd -P)"
trap 'rm -rf "${t:?}"' EXIT
pass=0; fail=0
E="admin/RB Stems Plus.exe"
mkdir -p "$t/work/admin" "$t/elsewhere" "$t/out"
echo carried > "$t/work/admin/f"
ln -s "$t/work/admin/f" "$t/elsewhere/link"           # a symlink in the last part
ln -s "$t/work/admin" "$t/linkdir"                     # a symlinked parent
for c in "direct:$t/work/admin/f" "last part:$t/elsewhere/link" "parent:$t/linkdir/f"; do
  name="${c%%:*}" p="${c#*:}"
  out="$(RBSP_WIN_WORK="$t/work" RBSP_WIN_SUMS="$t/sums" "$root/scripts/build-windows.sh" --carry-file "$E=$p" "$t/out" 2>&1)"; rc=$?
  if [ $rc -ne 0 ] && grep -q "lies in a folder the build empties" <<< "$out" && [ "$(cat "$t/work/admin/f" 2>/dev/null)" = carried ]; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1)); echo "FAIL $name: rc=$rc, the file $( [ -f "$t/work/admin/f" ] && echo kept || echo gone): $(head -2 <<< "$out")"
  fi
done
# a file outside the build's folders passes the guard (the run then stops: the work folder's parent is a file)
echo x > "$t/elsewhere/ok"; echo file > "$t/notadir"
out="$(RBSP_WIN_WORK="$t/notadir/work" RBSP_WIN_SUMS="$t/sums" "$root/scripts/build-windows.sh" --carry-file "$E=$t/elsewhere/ok" "$t/out" 2>&1)"; rc=$?
if [ $rc -ne 0 ] && ! grep -q "lies in a folder" <<< "$out"; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "FAIL elsewhere: rc=$rc: $(head -2 <<< "$out")"; fi
echo "carry-file-guard: $pass passed, $fail failed"
[ "$fail" = 0 ]

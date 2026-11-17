#!/bin/bash
# The real Windows build's decodes against the Mac's (T16/T17): rbsp-decode-dump.exe's output from
# the VM (WIN: its win.txt and .f32 files) compared with decode_dump-mac's on the same fixtures
# (MAC: fix/expect.txt from win-decode-fixtures.py, mac.txt, mac/*.f32), by expect.txt's fourth
# column:
#  - same: the same line and the same samples, bit for bit (or both refuse it);
#  - within-1: the same line, every sample within one 16-bit step (1/32768) of the Mac's (one MP3:
#    rekordbox's Windows libmpg123 isn't the Mac's build; win-decode-fixtures.py says why);
#  - refuses: the Windows build skips it; decodes: it decodes it (the Mac refuses it).
#
#   helper/tests/win-decode-vm.sh MAC WIN     exits 1 on a failure
set -u
mac="$1" win="$2"
ok=0 bad=0
for f in "$mac/fix/expect.txt" "$mac/mac.txt" "$win/win.txt"; do
  [ -s "$f" ] || { echo "FAIL $f is missing or empty"; exit 1; }
done
line() { grep -m1 "^$1	" "$2" | cut -f2- | tr -d '\r' || true; }
while IFS= read -r row; do
  row="${row%$'\r'}"
  n="$(printf '%s' "$row" | cut -f1)" want="$(printf '%s' "$row" | cut -f4)"   # the why column may be empty
  m="$(line "$n" "$mac/mac.txt")" w="$(line "$n" "$win/win.txt")"
  case "$want" in
    same | within-1)
      if [ "${m%%:*}" = skipped ] && [ "${w%%:*}" = skipped ]; then ok=$((ok + 1)); continue; fi
      if [ -z "$m" ] || [ "$m" != "$w" ]; then bad=$((bad + 1)); echo "FAIL $n: mac '$m', windows '$w'"; continue; fi
      k="$(echo "$m" | cut -f1 | cut -d' ' -f1)" fine=1
      for ((i = 0; i < k; i++)); do
        a="$mac/mac/$n.$i.f32" b="$win/$n.$i.f32"
        if [ "$want" = same ]; then cmp -s "$a" "$b" || fine=0
        else
          r="$(python3 -I - "$a" "$b" << 'PY'
import math
import sys
from array import array
def f32(p):
    v = array("f")
    with open(p, "rb") as f:
        v.frombytes(f.read())
    if sys.byteorder != "little":
        v.byteswap()
    return v
a, b = f32(sys.argv[1]), f32(sys.argv[2])
if len(a) != len(b):
    print("lengths %d and %d" % (len(a), len(b)))
    sys.exit(1)
step = 1.0 / 32768
if not all(math.isfinite(x) and math.isfinite(y) for x, y in zip(a, b)):
    print("a sample isn't a finite number")
    sys.exit(1)
off = sum(1 for x, y in zip(a, b) if x != y)
worst = max((abs(x - y) for x, y in zip(a, b)), default=0.0)
print("%d of %d samples off, at most %g step(s)" % (off, len(a), worst / step))
sys.exit(0 if worst <= step else 1)
PY
)" || fine=0
          echo "     $n.$i: $r"
        fi
      done
      if [ "$fine" = 1 ]; then ok=$((ok + 1)); else bad=$((bad + 1)); echo "FAIL $n: the samples differ ($want)"; fi ;;
    refuses) if [ "${w%%:*}" = skipped ]; then ok=$((ok + 1)); else bad=$((bad + 1)); echo "FAIL $n should be refused: '$w'"; fi ;;
    decodes) if [ -n "$w" ] && [ "${w%%:*}" != skipped ]; then ok=$((ok + 1)); else bad=$((bad + 1)); echo "FAIL $n should decode: '$w'"; fi ;;
    *) bad=$((bad + 1)); echo "FAIL $n: unknown expectation '$want'" ;;
  esac
done < "$mac/fix/expect.txt"
echo "decode on Windows vs the Mac: $ok ok, $bad failed"
[ "$bad" = 0 ] && [ "$ok" -gt 0 ]

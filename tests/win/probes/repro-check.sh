#!/bin/sh
# T1 check: the pinned toolchain and win-cc.sh's flags give byte-identical PEs from two copies of
# the sources at different paths (one with a space) into different output folders.
#   tests/win/probes/repro-check.sh
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
"$root/scripts/win-cc.sh" --fetch
work="$root/build/repro"
rm -rf "$work"
r2="$work/other root"
mkdir -p "$r2/build" "$r2/tests/win" "$r2/scripts"
cp -R "$root/bridge" "$r2/bridge"
cp -R "$here" "$r2/tests/win/probes"
cp "$root/scripts/win-cc.sh" "$r2/scripts/"
ln -s "$root/build/llvm-mingw" "$r2/build/llvm-mingw"
SOURCE_DATE_EPOCH="$(git -C "$root" log -1 --format=%ct)"
export SOURCE_DATE_EPOCH
"$here/build.sh" "$work/a" > /dev/null
"$r2/tests/win/probes/build.sh" "$work/b" > /dev/null
bad=0
for f in $(cd "$work/a" && find . -type f \( -name '*.dll' -o -name '*.exe' \) | sort); do
  if cmp -s "$work/a/$f" "$work/b/$f"; then echo "same $f"; else echo "DIFF $f"; bad=1; fi
done
# neither source root may appear in a binary: grep's 0 is a leak, 1 is clean, 2 or more an error
rc=0
find "$work/a" -type f \( -name '*.dll' -o -name '*.exe' \) -exec grep -l -a -F -e "$root" -e "$r2" {} + || rc=$?
if [ $rc -eq 0 ]; then
  echo "a source path is in the files above"; bad=1
elif [ $rc -ge 2 ]; then
  echo "the path check failed (grep status $rc)"; bad=1
fi
[ $bad = 0 ] && echo "reproducible" || { echo "NOT reproducible"; exit 1; }

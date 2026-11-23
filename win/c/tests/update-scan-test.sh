#!/bin/sh
# Compile the actual scan loops with fakes for process enumeration, opens and path queries.
set -eu
out="$1"
src="${2:-win/c/update_win.c}"
mkdir -p "$out"
sed -n -e '/^static int scan_opened_unknown(/,/^}/p' \
       -e '/^static int win_stop_watchers(/,/^}/p' \
       -e '/^static int win_watchers_in(/,/^}/p' \
       -e '/^static int win_running_under(/,/^}/p' "$src" > "$out/update_scan.inc"
clang -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Werror \
  -Icore -Icommon -I"$out" -Wl,-dead_strip win/c/update_rules.c win/c/tests/update_scan_test.c -o "$out/update-scan-test"
"$out/update-scan-test"

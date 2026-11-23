#!/bin/sh
# Exercise the Windows admin client against deterministic overlapped-I/O fakes on the host.
set -eu
out="$1"
src="${2:-win/c/admin_win.c}"
mkdir -p "$out"
sed -n '/^int admin_win_elevate(/,/^}/p' "$src" > "$out/admin_elevate.inc"
clang -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Werror \
  -Icore -Icommon -I"$out" win/c/tests/admin_connect_test.c -o "$out/admin-connect-test"
"$out/admin-connect-test"

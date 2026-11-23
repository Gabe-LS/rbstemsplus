#!/bin/sh
# Build the actual watcher adapters with fakes for the Windows calls and the real core.
set -eu
out="$1"
mkdir -p "$out"
src=win/c/core_native_steps.c
sed -n -e '/^static int ul_run_remove(/,/^\/\* SELF_DELETE /{ /^\/\* SELF_DELETE /d; p; }' \
       -e '/^\/\* LAUNCH_AGENT_LOAD, WATCHER_CHECK, WATCHER_STATE /,/^\/\* ----.* where the stems are/{ /^\/\* ----.* where the stems are/d; p; }' \
       "$src" > "$out/watcher_steps.inc"
core_srcs="$(core/build.sh srcs)"
# shellcheck disable=SC2086
clang -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Werror -g \
  -DCORE_WIN_POLICY -Icore -Icommon -I"$out" $(printf 'core/%s ' $core_srcs) \
  win/c/native_steps.c common/plat_mac.c win/c/tests/watcher_step_test.c \
  -Wl,-dead_strip -framework CoreFoundation -framework Accelerate -o "$out/watcher-step-test"
"$out/watcher-step-test"

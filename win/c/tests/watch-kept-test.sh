#!/bin/sh
# Run the watcher's kept-set adapter on the host without the Windows process/UI calls.
set -eu
out="$1"
mkdir -p "$out"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/rbsp-watch-kept.XXXXXX")"
trap 'rm -rf "${scratch:?}"' EXIT
core_srcs="$(core/build.sh srcs)"
# shellcheck disable=SC2086
clang -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wformat=2 -Werror \
  -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -DCORE_WIN_POLICY -Icore -Icommon \
  $(printf 'core/%s ' $core_srcs) win/c/watch_win.c win/c/host_facts.c win/c/inventory.c win/c/receipt.c win/c/pe_code.c win/c/helper_proc.c win/c/native_facts.c \
  win/c/payload.c win/c/payload_store.c win/c/compat.c win/c/update.c win/c/update_rules.c win/c/native_location.c \
  win/c/native_config.c win/c/fs_write.c win/c/sig.c win/c/tests/sig_mac.c common/plat_mac.c common/cfg_ini.c common/path_rules.c \
  win/c/tests/watch_kept_test.c -Wl,-dead_strip -framework Security -framework CoreFoundation -framework Accelerate \
  -o "$out/watch-kept-test"
"$out/watch-kept-test" "$scratch"

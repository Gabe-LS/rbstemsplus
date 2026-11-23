#!/bin/sh
# Compile the DLL's Prepare argument builder and worker snapshot code with host-side fakes.
set -eu
out="$1"
mkdir -p "$out"
src="${2:-win/c/core_native.c}"
steps="${3:-win/c/core_native_steps.c}"
update="${4:-win/c/update_win.c}"
sed -n '/^static int nw_prepare_args(/,/^}/p' "$src" > "$out/prepare_args.inc"
clang -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Werror -ffunction-sections \
  -Icommon -Icore -Iwin/c -I"$out" win/c/tests/prepare_args_test.c win/c/helper_proc.c win/c/compat.c \
  -Wl,-dead_strip -o "$out/prepare-args-test"
"$out/prepare-args-test"
sed -n -e '/^struct nw_steps {/,/^static nw_steps \*steps_list;/p' \
       -e '/^static int e_saved(/,/^}/p' \
       -e '/^static int read_installed_list(/,/^int nw_steps_rekordbox_folder(/{ /^int nw_steps_rekordbox_folder(/!p; }' \
       -e '/^int nw_steps_rekordbox_current(/,/^}/p' "$steps" > "$out/rekordbox_snapshot.inc"
sed -n '/^    case CORE_STEP_PAYLOAD: {/,/^    }/p' "$steps" > "$out/rekordbox_payload_step.inc"
sed -n '/^    case CORE_STEP_REQUEST: {/,/^    }/p' "$steps" > "$out/rekordbox_request_step.inc"
sed -n '/^CORE_API .* core_win_update_after_render(/,/^}/p' "$update" > "$out/rekordbox_after_render.inc"
core_srcs="$(core/build.sh srcs)"
# shellcheck disable=SC2086
clang -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Werror -ffunction-sections \
  -DCORE_WIN_POLICY $(printf 'core/%s ' $core_srcs) common/plat_mac.c \
  -Icommon -Icore -Iwin/c -I"$out" win/c/tests/rekordbox_snapshot_test.c win/c/helper_proc.c win/c/compat.c \
  -Wl,-dead_strip -framework CoreFoundation -framework Accelerate -o "$out/rekordbox-snapshot-test"
"$out/rekordbox-snapshot-test"
sed -n -e '/^struct nw_steps {/,/^static nw_steps \*steps_list;/p' \
       -e '/^static host_facts \*read_rb_coverage(/,/^}/p' \
       -e '/^static void free_rb_coverage(/,/^}/p' \
       -e '/^static int32_t r1_dismiss(/,/^}/p' \
       -e '/^\/\* BLOCKED.s facts/,/^static int e_blocked(/{ /^static int e_blocked(/!p; }' \
       -e '/^static int scope_version(/,/^}/p' \
       -e '/^static int win_scope(/,/^}/p' "$steps" > "$out/win_scope.inc"
# shellcheck disable=SC2086
clang -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Werror -ffunction-sections \
  -DCORE_WIN_POLICY $(printf 'core/%s ' $core_srcs) common/plat_mac.c \
  -Icommon -Icore -Iwin/c -I"$out" win/c/tests/win_scope_test.c win/c/inventory.c win/c/admin_proto.c win/c/dismissed.c win/c/payload.c win/c/sig.c win/c/tests/sig_mac.c \
  -Wl,-dead_strip -framework Security -framework CoreFoundation -framework Accelerate -o "$out/win-scope-test"
"$out/win-scope-test"

# BLOCKED's Cache facts at the same scope should line up.
awk '
  /case CORE_STEP_BLOCKED:/ { blocked = 1 }
  blocked && /core_fact_str\(out, FACT_REKORDBOX_VERSION,/ { version_indent = match($0, /[^ ]/) }
  blocked && /core_fact_flag\(out, FACT_REKORDBOX_SUPPORTED,/ {
    aligned = (match($0, /[^ ]/) == version_indent)
    exit
  }
  END { if (!aligned) { print "FAIL BLOCKED Cache facts do not align"; exit 1 } }
' "$steps"

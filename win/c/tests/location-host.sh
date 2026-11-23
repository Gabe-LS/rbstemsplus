#!/bin/sh
# The location checks on the Mac, with and without the Windows fixture's volume override.
set -eu
cd "$(dirname "$0")/../../.."
scratch=$(mktemp -d "${TMPDIR:-/tmp}/rbsp-location-host.XXXXXX")
trap 'chmod -R u+w "$scratch"; rm -rf "$scratch"' EXIT

cat > "$scratch/main.c" <<'EOF'
#include "check.h"

int checks, failures;

int main(int argc, char **argv) {
    if (argc != 4) return 2;
    test_native_location(argv[1]);
    test_location_choice(argv[2]);
    test_native_steps(argv[3]);
    printf("%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
EOF

for policy in release internal-ok; do
    set --
    if [ "$policy" = internal-ok ]; then set -- -DVOL_TEST_INTERNAL_OK; fi
    clang -std=c11 -O2 -mmacosx-version-min=14.0 -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes \
        -Wmissing-prototypes -Wformat=2 -Wformat-nonliteral -Werror -DNATIVE_LOCATION_TEST_HOOKS -DNATIVE_STEPS_TEST_HOOKS -DRBSTEMS_WIN_CACHE_TEST \
        "$@" -Icore -Icommon -Iwin/c -Iwin/c/tests -Wl,-dead_strip \
        "$scratch/main.c" win/c/tests/native_location_test.c win/c/tests/location_choice_test.c win/c/tests/native_steps_test.c \
        win/c/native_location.c win/c/native_steps.c win/c/native_facts.c win/c/native_config.c win/c/volumes.c win/c/fs_write.c \
        win/c/compat.c win/c/models.c win/c/shell.c win/c/payload.c common/cfg_ini.c common/path_rules.c common/marker.c common/plat_mac.c \
        core/arena.c core/json.c core/state.c core/text.c core/helper_lines.c core/words.c \
        -framework Accelerate -framework CoreFoundation -o "$scratch/test"
    mkdir -p "$scratch/$policy/location" "$scratch/$policy/choice" "$scratch/$policy/steps"
    "$scratch/test" "$scratch/$policy/location" "$scratch/$policy/choice" "$scratch/$policy/steps" > "$scratch/$policy.log" 2>&1 || {
        cat "$scratch/$policy.log"
        exit 1
    }
    printf 'location host (%s): %s\n' "$policy" "$(tail -1 "$scratch/$policy.log")"
done

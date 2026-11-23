#!/bin/sh
# Extract the bridge's parser for tests, as bridge/tests/fuzz.sh does for its parser checks.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
sed -n \
  -e '/^#define KEPT_FILE_MAX /p' \
  -e '/^#define KEPT_LINE /p' \
  -e '/^typedef struct { unsigned char model\[32\], key\[32\]; } kept_entry;$/p' \
  -e '/^static void unhex(.*);$/p' \
  -e '/^static int parse_kept(/,/^}$/p' \
  -e '/^static void unhex(.* {/,/^}$/p' \
  "$here/../../bridge/rbstems_bridge.c"

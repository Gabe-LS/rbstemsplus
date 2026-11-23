#!/bin/sh
# Exercise the PE digest and Windows release preflight using synthetic fixtures only.
set -eu
root="$(cd "$(dirname "$0")/../../.." && pwd)"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/rbsp-pe-code.XXXXXX")"
trap 'rm -rf "${scratch:?}"' EXIT
mkdir "$scratch/fixtures"
cd "$root"

clang -std=c11 -O2 -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes \
  -Wformat=2 -Wformat-nonliteral -Werror -mmacosx-version-min=14.0 -Icore -Icommon -Iwin/c \
  win/c/tests/pe_code_test.c win/c/pe_code.c -o "$scratch/pe-code-test"
python3 -I win/c/tests/pe_code_test.py "$scratch/pe-code-test" "$scratch/fixtures"

#!/bin/sh
# Integrity check of the VM library copies that vm/collect.ps1 left in the share
# (build/win/inv/LABEL/db/). Builds folderpath (the helper's SQLCipher amalgamation) into
# build/ptmp if needed; the key goes from libkey.py to folderpath through a pipe, never printed.
#   tests/win/probes/dbcheck.sh LABEL...   (checks every label; exits 1 if any failed)
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
main="$(cd "$(git -C "$here" rev-parse --path-format=absolute --git-common-dir)/.." && pwd)"
sqlc="$main/build/helper/sqlcipher-4.19.0"
pyrb="$main/build/pyrekordbox/2dcb57f2ea907c51fa15850f5433ec09ff3223ff"
tool="$root/build/ptmp/folderpath"
mkdir -p "$root/build/ptmp"
[ "$tool" -nt "$here/folderpath.c" ] || clang -O1 -w -DSQLITE_HAS_CODEC -DSQLCIPHER_CRYPTO_CC -DSQLITE_TEMP_STORE=2 -DSQLITE_THREADSAFE=1 \
  -DSQLITE_EXTRA_INIT=sqlcipher_extra_init -DSQLITE_EXTRA_SHUTDOWN=sqlcipher_extra_shutdown \
  -I"$sqlc" "$here/folderpath.c" "$sqlc/sqlite3.c" -framework Security -framework CoreFoundation -o "$tool"
failed=0
for label in "$@"; do
  work="$root/build/ptmp/db-$label"
  rm -rf "$work"; mkdir -p "$work"
  if cp "$main/build/win/inv/$label/db/"master.db* "$work/"; then
    printf '%s: ' "$label"
    # the pipeline's status is folderpath's: 0 only for a single "ok" row
    if ! python3 -I "$here/libkey.py" "$pyrb/database.py" "$pyrb/utils.py" | "$tool" --check "$work/master.db"; then
      echo "$label: FAILED"; failed=1
    fi
  else
    echo "$label: no library copy"; failed=1
  fi
  rm -rf "$work"
done
exit $failed

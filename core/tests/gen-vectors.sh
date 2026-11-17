#!/bin/sh
# The core's vectors from the frozen Swift (test only): builds core/tests/swift/vectors.swift with
# the Swift app's sources as they were when the core took over its decisions (core/tests/swift/oracle:
# app/Sources at the M7 switch-over's base, d005363 = 56d7c11, plus the freeze-time fix 07db3b2;
# not the v1.3.0 tag, it has 1.4.0's code too; all but main.swift, as `make test` built them) and writes what those functions return into OUT/vectors_swift.h, which the
# core's unit tests include and compare against (core/tests/vectors.c). The Swift UI's decisions
# are gone from app/Sources since the switch-over (mac-core §8), so the oracle is that frozen copy:
# never edited, never shipped. core/build.sh test runs this when the header is missing or older.
#
#   core/tests/gen-vectors.sh OUT
set -eu
[ $# -eq 1 ] || { echo "usage: $0 OUT" >&2; exit 2; }
mkdir -p "$1"
dest="$(cd "$1" && pwd)"
root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$root"
out="$dest/vectors-build"
rm -rf "$out"
mkdir -p "$out/main"
printf '%s\n' 'let compiledKeys: [(name: String, der: String)] = []' > "$out/Keys.swift"
printf 'let compiledVersion = "%s"\n' "$(tr -d '[:space:]' < VERSION)" > "$out/Version.swift"
cp core/tests/swift/vectors.swift "$out/main/main.swift"   # top-level code beside other files: main.swift
set --
for f in core/tests/swift/oracle/*.swift; do [ "$f" = core/tests/swift/oracle/main.swift ] || set -- "$@" "$f"; done
xcrun swiftc -sdk "$(xcrun --sdk macosx --show-sdk-path)" -module-cache-path "$out/mc" \
  "$@" "$out/Keys.swift" "$out/Version.swift" "$out/main/main.swift" -o "$out/gen"
"$out/gen" > "$out/vectors_swift.h"
mv "$out/vectors_swift.h" "$dest/vectors_swift.h"
echo "wrote $dest/vectors_swift.h ($(wc -c < "$dest/vectors_swift.h" | tr -d ' ') bytes)"

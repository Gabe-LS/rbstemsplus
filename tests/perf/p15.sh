#!/bin/bash
# P15 on the host: build tests/perf/p15-bench.swift with the app's sources (-O, as the app) and
# run it; prints "swift <ns per line>" (stats.py reads it from a run's p15.txt).
#   tests/perf/p15.sh [OUT]          OUT default build/perf/p15.txt
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
b="$root/build/perf"; mkdir -p "$b"
out="${1:-$b/p15.txt}"
printf 'let compiledKeys: [(name: String, der: String)] = []\n' > "$b/Keys.swift"
printf 'let compiledVersion = "%s"\n' "$(tr -d '[:space:]' < "$root/VERSION")" > "$b/Version.swift"
srcs=()
for f in "$root"/app/Sources/*.swift; do [ "$(basename "$f")" = main.swift ] || srcs+=("$f"); done
# the bench's own file must be main.swift for top-level code
cp "$root/tests/perf/p15-bench.swift" "$b/main.swift"
xcrun swiftc -O -sdk "$(xcrun --sdk macosx --show-sdk-path)" "${srcs[@]}" "$b/Keys.swift" "$b/Version.swift" "$b/main.swift" -o "$b/p15-bench"
"$b/p15-bench" | tee "$out"

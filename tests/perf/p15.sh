#!/bin/bash
# P15 on the host: the helper-line parse, ns per line, both parsers on the same lines:
#   swift  tests/perf/p15-bench.swift with the frozen Swift app's sources (core/tests/swift/oracle,
#          whose parseRunLine is 1.3.0's; app/Sources has none since the switch-over), -O as the app
#   c      tests/perf/p15-bench.c with the C core's parser (core/helper_lines.c), -O2 as the core
# prints "swift <ns per line>" and "c <ns per line>" (stats.py --p15 reads them: swift A's, c B's).
#   tests/perf/p15.sh [OUT]          OUT default build/perf/p15.txt
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
b="$root/build/perf"; mkdir -p "$b"
out="${1:-$b/p15.txt}"
printf 'let compiledKeys: [(name: String, der: String)] = []\n' > "$b/Keys.swift"
printf 'let compiledVersion = "%s"\n' "$(tr -d '[:space:]' < "$root/VERSION")" > "$b/Version.swift"
srcs=()
for f in "$root"/core/tests/swift/oracle/*.swift; do [ "$(basename "$f")" = main.swift ] || srcs+=("$f"); done
# the bench's own file must be main.swift for top-level code
cp "$root/tests/perf/p15-bench.swift" "$b/main.swift"
xcrun swiftc -O -sdk "$(xcrun --sdk macosx --show-sdk-path)" "${srcs[@]}" "$b/Keys.swift" "$b/Version.swift" "$b/main.swift" -o "$b/p15-bench"
xcrun clang -std=c11 -O2 -DNDEBUG -I"$root/core" "$root/tests/perf/p15-bench.c" \
  "$root/core/helper_lines.c" "$root/core/arena.c" "$root/core/json.c" -o "$b/p15-bench-c"
{ "$b/p15-bench"; "$b/p15-bench-c"; } | tee "$out"

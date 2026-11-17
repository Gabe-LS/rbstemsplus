#!/bin/sh
# The shared payload and signature fixtures from today's Swift (test only): builds
# win/c/tests/fixtures.swift with the app's sources (all but main.swift, as `make test` builds
# them) and writes what Signing.swift and Payload.swift decide into OUT/fixtures.txt, which
# win/c/tests/run.sh's tests read. Made afresh from app/Sources each time, as the core's vectors
# (core/tests/gen-vectors.sh): a Swift change the Windows copy doesn't follow fails its tests.
#
#   win/c/tests/gen-fixtures.sh OUT
set -eu
[ $# -eq 1 ] || { echo "usage: $0 OUT" >&2; exit 2; }
mkdir -p "$1"
dest="$(cd "$1" && pwd)"
root="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$root"
out="$dest/fixtures-build"
rm -rf "$out"
mkdir -p "$out/main"
printf '%s\n' 'let compiledKeys: [(name: String, der: String)] = []' > "$out/Keys.swift"
printf 'let compiledVersion = "%s"\n' "$(tr -d '[:space:]' < VERSION)" > "$out/Version.swift"
cp win/c/tests/fixtures.swift "$out/main/main.swift"
set --
for f in app/Sources/*.swift; do [ "$f" = app/Sources/main.swift ] || set -- "$@" "$f"; done
xcrun swiftc -sdk "$(xcrun --sdk macosx --show-sdk-path)" -module-cache-path "$out/mc" \
  "$@" "$out/Keys.swift" "$out/Version.swift" "$out/main/main.swift" -o "$out/gen"
"$out/gen" "$out/scratch" > "$out/fixtures.txt"
mv "$out/fixtures.txt" "$dest/fixtures.txt"
rm -rf "$out/scratch"
# config.ini writes Swift couldn't make here (Foundation's atomic write goes through a folder a
# sandbox refuses): the fixtures are partial, and run.sh makes them again next time (review S44)
unwritten="$(sed -n 's/^# config writes not made: //p' "$dest/fixtures.txt")"
if [ "${unwritten:-0}" -gt 0 ]; then : > "$dest/fixtures.partial"; else rm -f "$dest/fixtures.partial"; fi
echo "wrote $dest/fixtures.txt ($(grep -vc '^#' "$dest/fixtures.txt") records${unwritten:+, $unwritten config writes not made})"

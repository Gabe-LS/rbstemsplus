#!/bin/bash
# The AppKit shell's host tests (tests/mac/shell-test.swift): app/Shell, app/Sources (without
# main.swift) and the probe, built as a TEST build with the core's test-hooks archive and core.h as
# the bridging header, run on the host with no window shown and nothing activated. Its steps,
# admin runs and dialogs are the test's stand-ins: nothing on this Mac is changed.
# Run outside a sandbox (swiftc writes its module cache in the user's folder; the run talks to
# the window server for its invisible panels).
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
t="${TMPDIR:-/tmp}"
tmp="$(mktemp -d "${t%/}/rbsp-shell-test.XXXXXX")" || { echo "shell-test.sh: no scratch folder" >&2; exit 2; }
tmp="$(cd "$tmp" && pwd -P)" && [ -n "$tmp" ] && [ -d "$tmp" ] || { echo "shell-test.sh: no scratch folder" >&2; exit 2; }
trap 'rm -rf "${tmp:?}"' EXIT
arch="$(uname -m)"
"$root/core/build.sh" lib "$tmp/core" --test-hooks > /dev/null
"$root/core/build.sh" ids "$tmp/core" > /dev/null
printf '%s\n' 'let compiledKeys: [(name: String, der: String)] = []' > "$tmp/Keys.swift"
printf 'let compiledVersion = "%s"\n' "$(tr -d '[:space:]' < "$root/VERSION")" > "$tmp/Version.swift"
printf '%s\n' 'let testDownloadBase = "http://127.0.0.1:9/"' > "$tmp/TestBase.swift"
# top-level code beside other files: it must be called main.swift
mkdir -p "$tmp/test" && cp "$root/tests/mac/shell-test.swift" "$tmp/test/main.swift"
sources=()
for f in "$root"/app/Sources/*.swift; do [ "$(basename "$f")" = main.swift ] || sources+=("$f"); done
# main.swift's declarations (AppDelegate), without its top-level start-up code: everything above
# the --swap role's comment
awk '/^\/\/ "--swap <staged app> <app>"/ { exit } { print }' "$root/app/Sources/main.swift" > "$tmp/MainDecls.swift"
grep -q '^final class AppDelegate' "$tmp/MainDecls.swift" || { echo "shell-test.sh: main.swift's AppDelegate not found above its start-up code" >&2; exit 1; }
sources+=("$tmp/MainDecls.swift")
# the release configuration first (scripts/build-app.sh without a test base: no TEST_BUILD, no
# probe, no test hooks), typechecked only: a release-only compile error (a TEST_BUILD name used
# outside its #if) fails here, before the test build and long before carry-test's release build
xcrun swiftc -typecheck -sdk "$(xcrun --sdk macosx --show-sdk-path)" -target "$arch-apple-macos12.0" \
  -D CORE_LINKED -import-objc-header "$root/core/core.h" \
  "$root"/app/Sources/*.swift "$root"/app/Shell/*.swift "$tmp/Keys.swift" "$tmp/Version.swift" "$tmp/core/CoreIds.swift" \
  || { echo "shell-test.sh: the release configuration (no TEST_BUILD) doesn't compile" >&2; exit 1; }
echo "shell-test: the release configuration typechecks"
xcrun swiftc -O -sdk "$(xcrun --sdk macosx --show-sdk-path)" -target "$arch-apple-macos12.0" \
  -D TEST_BUILD -D CORE_LINKED -import-objc-header "$root/core/core.h" -Xcc -DCORE_TEST_HOOKS \
  "${sources[@]}" "$root"/app/Probe/*.swift "$root"/app/Shell/*.swift \
  "$tmp/Keys.swift" "$tmp/Version.swift" "$tmp/TestBase.swift" "$tmp/core/CoreIds.swift" "$root/tests/mac/shell-oracle.swift" "$tmp/test/main.swift" \
  "$tmp/core/$arch/libcore-testhooks.a" -o "$tmp/shell-test"
"$tmp/shell-test" "$root"            # the repository: the readbacks' tables read today's sources there

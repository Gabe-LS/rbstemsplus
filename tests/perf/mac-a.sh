#!/bin/bash
# mac-A, the performance anchor (the private Windows plan §8.2, mac-core.md §2): the app at the G0 commit
# (v1.3.0's source plus the probe and the timing commits) built ONCE as a TEST build and kept as
# binaries in the gate archive. Never rebuilt: this script refuses when gates-archive/mac-A exists.
#
#   tests/perf/mac-a.sh [COMMIT]     default HEAD; the checkout must be clean and at COMMIT
#
# What it checks first:
#   - app/Sources at COMMIT differs from v1.3.0's only inside "#if TEST_BUILD" blocks: a
#     release-mode executable of each (arm64) has the same code (__TEXT sections; the UUID and
#     the linker's temporary names differ between any two swiftc runs, also of the same source)
#   - the TEST build has the probe, the release build of COMMIT has none (check-release.sh)
# What it keeps, in $GATES_ARCHIVE/mac-A/ (default: gates-archive/ in this checkout): the app
# zip, its bridge (fact_cache_on), the test key pair that signs its payload.json (a test key,
# never a release key; for P14's local update server), the build log, the toolchain's identity,
# COMMIT, and its own SHA256SUMS; then the archive's SHA256SUMS is sealed (tests/perf/archive.sh).
# Then copy the archive to the second disk (release.md §6.1: the archive's second copy) by hand.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$root"
commit="$(git rev-parse "${1:-HEAD}")"
A="${GATES_ARCHIVE:-$root/gates-archive}"
arch_dir="$A/mac-A"
# shellcheck source=archive.sh
. "$root/tests/perf/archive.sh"
die() { echo "mac-a.sh: $*" >&2; exit 1; }
[ ! -e "$arch_dir" ] || die "$arch_dir exists: mac-A is never rebuilt"
[ "$(git rev-parse HEAD)" = "$commit" ] || die "check out $commit first"
[ -z "$(git status --porcelain)" ] || die "uncommitted or untracked files: mac-A is built only from a commit"
model="${RBSTEMSPLUS_MODEL:-}"
[ -f "$model" ] || die "RBSTEMSPLUS_MODEL must name the model export (scripts/build.sh needs it)"

# 1. app/Sources: the release code of COMMIT is v1.3.0's
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
sdk="$(xcrun --sdk macosx --show-sdk-path)"
printf 'let compiledKeys: [(name: String, der: String)] = []\n' > "$tmp/Keys.swift"
printf 'let compiledVersion = "1.3.0"\n' > "$tmp/Version.swift"
for v in v1.3.0 "$commit"; do
  mkdir -p "$tmp/$v" && git archive "$v" app/Sources | tar -x -C "$tmp/$v"
  (cd "$tmp/$v" && xcrun swiftc -O -sdk "$sdk" -target arm64-apple-macos12.0 app/Sources/*.swift "$tmp/Keys.swift" "$tmp/Version.swift" -o "$tmp/exe-$(echo "$v" | tr / _)" 2>/dev/null)
done
code() { for s in __text __cstring __const __swift5_types; do otool -s __TEXT "$s" "$1" | tail -n +3; done | shasum -a 256 | cut -d' ' -f1; }
[ "$(code "$tmp/exe-v1.3.0")" = "$(code "$tmp/exe-$commit")" ] || die "the release code at $commit isn't v1.3.0's: only TEST_BUILD hunks may differ"
echo "release code at ${commit:0:12} = v1.3.0's"
tests/harness/check-release.sh "$tmp/exe-$commit" || die "the release-mode executable at $commit has the probe (or the check failed)"

# 2. the TEST build
RBSTEMSPLUS_MODEL="$model" scripts/build.sh --test "${TEST_URL:-http://192.168.64.1:8765/}" > "$tmp/build.log" 2>&1 || { tail -20 "$tmp/build.log"; die "the TEST build failed"; }
tests/harness/check-release.sh "build/app/RB Stems Plus.app" --expect-probe

# 3. the archive
mkdir -p "$arch_dir"
cp dist-test/rbstemsplus-app.zip dist-test/libonnxruntime.1.18.0.dylib dist-test/payload.json dist-test/payload.json.sig "$arch_dir/"
cp -R build/test-keys "$arch_dir/test-keys"
cp "$tmp/build.log" "$arch_dir/build.log"
{
  echo "commit=$commit"
  echo "built=$(date -u +%FT%TZ)"
  echo "test_url=${TEST_URL:-http://192.168.64.1:8765/}"
  echo "swiftc=$(xcrun swiftc --version 2>&1 | head -1)"
  echo "clang=$(clang --version | head -1)"
  echo "xcode=$(xcodebuild -version 2>/dev/null | tr '\n' ' ')"
  echo "sdk=$(xcrun --sdk macosx --show-sdk-version)"
  echo "macos=$(sw_vers -productVersion)"
} > "$arch_dir/toolchain.txt"
(cd "$arch_dir" && find . -type f ! -name SHA256SUMS | sort | xargs shasum -a 256 > SHA256SUMS)
seal mac-A || die "the gate archive's SHA256SUMS couldn't be written"
echo "mac-A archived in $arch_dir, $A/SHA256SUMS sealed (copy the archive to the second disk)"

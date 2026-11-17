#!/bin/bash
# The G1 API spike's host smoke test: app/Spike/CoreSpike.swift built with the core's test-hooks
# archive, core.h as its bridging header and a stand-in for the probe (tests/mac/spike-smoke.swift),
# its window never shown, fed by the harness's fake helper (tests/harness/fake-prepare.c) with the
# perf scenarios' lines: 5,000 rows at full speed and at 1 line/ms, 20,000 at full speed. A check
# of the mechanics, not a measurement (that is tart's: tests/harness/scenarios/perf-spike*.sh).
# Run outside a sandbox (swiftc writes its module cache in the user's folder).
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
t="${TMPDIR:-/tmp}"   # tmp: a plain path, pgrep matches it
tmp="$(mktemp -d "${t%/}/rbsp-spike-smoke.XXXXXX")" || { echo "spike-smoke.sh: no scratch folder" >&2; exit 2; }
tmp="$(cd "$tmp" && pwd -P)" && [ -n "$tmp" ] && [ -d "$tmp" ] || { echo "spike-smoke.sh: no scratch folder" >&2; exit 2; }
trap 'rm -rf "${tmp:?}"' EXIT
arch="$(uname -m)"
"$root/core/build.sh" lib "$tmp/core" --test-hooks > /dev/null
"$root/core/build.sh" ids "$tmp/core" > /dev/null
mkdir -p "$tmp/fake/script" "$tmp/fake/log"
clang -O2 -o "$tmp/fake/fake-prepare" "$root/tests/harness/fake-prepare.c"
est='{"file":"/Users/dj/Music/Harness/big-{n}.mp3","result":"prepare","seconds":240,"entries":22,"gb":0.06,"missing":0}'
end='{"estimate":5000,"ready":0,"prepare":5000,"skipped":0,"entries":110000,"runs":110000,"time_s":275000,"need_gb":300,"kept_gb":0,"share_gb":150,"max_gb":200,"cache_gb":1,"free_gb":30,"fits":false,"fit_files":2500,"max_gb_needed":400,"disk_ok":false,"asking":0,"folders":[]}'
# 52: a list with no estimate script (the fake prints nothing): its rows stay Checking…
echo "@repeat 5000 {\"file\":\"/Users/dj/Music/Harness/big-{n}.mp3\"}" > "$tmp/fake/script/tracks-52.jsonl"
for p in 50:5000 51:5000 150:20000; do
  id="${p%%:*}" n="${p#*:}"
  echo "@repeat $n {\"file\":\"/Users/dj/Music/Harness/big-{n}.mp3\"}" > "$tmp/fake/script/tracks-$id.jsonl"
  pace=""; e="$est"; [ "$id" = 51 ] && { pace='@pace 1'; e="${est/\"prepare\"/\"ready\"}"; }   # 51: "Ready", told apart from 50's rows
  printf '%s\n@repeat %s %s\n%s\n' "$pace" "$n" "$e" "$end" > "$tmp/fake/script/estimate-$id.jsonl"
done
# top-level code beside other files: it must be called main.swift
mkdir -p "$tmp/smoke" && cp "$root/tests/mac/spike-smoke.swift" "$tmp/smoke/main.swift"
xcrun swiftc -O -sdk "$(xcrun --sdk macosx --show-sdk-path)" -target "$arch-apple-macos12.0" \
  -D TEST_BUILD -D CORE_LINKED -D SPIKE_HOST_TEST -import-objc-header "$root/core/core.h" -Xcc -DCORE_TEST_HOOKS \
  "$root/app/Spike/CoreSpike.swift" "$tmp/core/CoreIds.swift" "$tmp/smoke/main.swift" \
  "$tmp/core/$arch/libcore-testhooks.a" -o "$tmp/spike-smoke"
"$tmp/spike-smoke" "$tmp/fake"

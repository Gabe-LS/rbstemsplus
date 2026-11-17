#!/bin/bash
# Proves a build has none of the harness's probe (app/Probe/Probe.swift): no probe symbol, no probe
# string, in every architecture of the app's executable (and the watcher helper's copy).
#
#   tests/harness/check-release.sh APP_OR_EXECUTABLE [--expect-probe]
# Exit 0 when a release build is clean (or, with --expect-probe, when a TEST build has the probe:
# the check's own control, so a check that can't see the probe can't pass a release); 1 when the
# probe is there (or, with --expect-probe, isn't); 2 when the check itself can't run (no file, no
# architecture, strings or nm failing): it never passes then.
set -uo pipefail
target="${1:-}"; expect="${2:-}"
fail2() { echo "check-release.sh: $*" >&2; exit 2; }
[ -n "$target" ] || fail2 "usage: check-release.sh APP_OR_EXECUTABLE [--expect-probe]"
if [ -d "$target" ]; then
  main="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$target/Contents/Info.plist" 2>/dev/null)" || fail2 "$target has no Info.plist executable"
  exes=("$target/Contents/MacOS/$main")
  helper="$target/Contents/Helpers/RB Stems Plus Watcher.app/Contents/MacOS/RB Stems Plus Watcher"
  [ -f "$helper" ] && exes+=("$helper")
else
  exes=("$target")
fi
# what only the probe has: its configuration folder, its entry points, its class, its records' words
strings_re='rbstemsplus-probe|probe\.conf|workQueueFree|dialogEnd|windowDrawn|refresh\.begin|trigger\.activate|trigger\.actionEnd|tabShown|estimate\.lastline'
# (the class Probe of module main is "main5ProbeC"; LocationProbe, a release type, is "…13LocationProbe…")
symbols_re='probeStart|probeMark|probeFakeHelper|main5ProbeC'
found=0
for exe in "${exes[@]}"; do
  [ -f "$exe" ] || fail2 "no executable $exe"
  archs="$(lipo -archs "$exe")" || fail2 "lipo can't read $exe"
  [ -n "$archs" ] || fail2 "no architecture in $exe"
  for arch in $archs; do
    # the tools' output first, their status tested: a failing strings or nm never reads as clean
    s_out="$(strings -a -arch "$arch" "$exe")" || fail2 "strings failed on $exe ($arch)"
    n_out="$(nm -arch "$arch" "$exe")" || fail2 "nm failed on $exe ($arch)"
    [ -n "$s_out" ] && [ -n "$n_out" ] || fail2 "strings or nm read nothing from $exe ($arch)"
    s="$(printf '%s\n' "$s_out" | grep -E -c "$strings_re")"
    y="$(printf '%s\n' "$n_out" | grep -E -c "$symbols_re")"
    echo "$(basename "$exe") $arch: $s probe strings, $y probe symbols"
    found=$((found + s + y))
  done
done
if [ "$expect" = --expect-probe ]; then
  [ "$found" -gt 0 ] || { echo "check-release.sh: the TEST build shows no probe: the check can't see it"; exit 1; }
  echo "the probe is there (TEST build)"
else
  [ "$found" = 0 ] || { echo "check-release.sh: the probe is in this build"; exit 1; }
  echo "no probe in this build"
fi

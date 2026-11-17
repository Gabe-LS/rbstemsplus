#!/bin/sh
# Stages the Windows bridge's harness run (T7) into the VM's share, bridge-win/ (\\Mac\win\bridge-win\):
# the TEST bridge (onnxruntime.dll), harness.exe, fake/onnxruntime.dll (bridge/win/build.sh --test's
# OUT and OUT/tests/), model/hdemucs.onnx (bridge/tests/make_model.py's test model, for run.ps1's
# gpu group), and bridge/win/tests/run.ps1, which tests/win/suites/bridge.suite's steps
# dot-source; bridge-win/shas.txt lists them. bridge-win/.commit names the HEAD they were built
# from, only for a --build from a clean HEAD that didn't move while it ran; else it says unbuilt
# (review TR72). Replaced whole.
#
#   bridge/win/tests/stage.sh [--build] [SHARE]
#
# --build: bridge/win/build.sh --test first, into build/bridge-win-test (its real library comes
# from RBSP_TEST_PROGRAMS's tree in the VM, run.ps1's C:\rbtest-pf). SHARE: by default the main
# checkout's build/win, the folder the VM shares (as tests/win/vm-run.sh finds it, also from a
# worktree). Refuses a bridge that isn't a TEST build.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
fail() { echo "stage.sh: $*" >&2; exit 1; }
build=
[ "${1:-}" != --build ] || { build=1; shift; }
case "${1:-}" in -*) fail "usage: bridge/win/tests/stage.sh [--build] [SHARE]" ;; esac
if [ -n "${1:-}" ]; then share="$1"
else share="$(cd "$(git -C "$root" rev-parse --path-format=absolute --git-common-dir)/.." && pwd)/build/win"; fi
out="$root/build/bridge-win-test"
built=
if [ -n "$build" ]; then
  built="$(git -C "$root" rev-parse HEAD)"
  [ -z "$(git -C "$root" status --porcelain)" ] || built=
  mkdir -p "$root/build" || fail "can't make $root/build"   # a fresh worktree has none
  "$root/bridge/win/build.sh" --test 'C:\rbtest\ort\onnxruntime.dll' "$out" > "$out.log" 2>&1 \
    || { tail -20 "$out.log" >&2; fail "bridge/win/build.sh --test failed ($out.log)"; }
  tail -1 "$out.log"
  [ "$(git -C "$root" rev-parse HEAD)" = "$built" ] && [ -z "$(git -C "$root" status --porcelain)" ] || built=
fi
for f in onnxruntime.dll tests/harness.exe tests/fake/onnxruntime.dll; do
  [ -f "$out/$f" ] || fail "no $out/$f: bridge/win/build.sh --test first (or --build)"
done
# a TEST build reads RBSP_TEST_PROGRAMS (a release build refuses to hold it, bridge/win/build.sh)
python3 -I -c 'import sys; sys.exit(0 if "RBSP_TEST_PROGRAMS".encode("utf-16-le") in open(sys.argv[1], "rb").read() else 1)' "$out/onnxruntime.dll" \
  || fail "$out/onnxruntime.dll isn't a TEST build (no RBSP_TEST_PROGRAMS)"
[ -d "$share" ] || mkdir -p "$share" || fail "no share $share"
dst="$share/bridge-win"
rm -rf "$dst" || fail "can't clear $dst (files the VM wrote are root's: delete them from the VM)"
mkdir -p "$dst/fake" "$dst/model"
cp "$out/onnxruntime.dll" "$out/tests/harness.exe" "$here/run.ps1" "$dst/"
cp "$out/tests/fake/onnxruntime.dll" "$dst/fake/"
python3 -I "$root/bridge/tests/make_model.py" "$dst/model/hdemucs.onnx" > /dev/null || fail "bridge/tests/make_model.py failed"
(cd "$dst" && shasum -a 256 onnxruntime.dll harness.exe fake/onnxruntime.dll model/hdemucs.onnx run.ps1 > shas.txt)
if [ -n "$built" ]; then
  echo "$built" > "$dst/.commit"
  echo "staged $dst from $(git -C "$root" rev-parse --short "$built"):"
else
  echo unbuilt > "$dst/.commit"
  echo "staged $dst (not built from a clean HEAD here: no commit)"
fi
sed 's/^/  /' "$dst/shas.txt"

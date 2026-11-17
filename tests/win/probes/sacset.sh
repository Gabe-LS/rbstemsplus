#!/bin/sh
# Makes a set of never-seen unsigned stand-ins for the Smart App Control test in the VM's share
# (build/win/p0/NAME): each probe rebuilt with a random string linked into its image, so its
# image (and any hash of it) is new to Windows and the cloud. vm/install.ps1 ($which = NAME) and
# vm/sac-setup.ps1 ($set = NAME) use it.
#   tests/win/probes/sacset.sh NAME
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
main="$(cd "$(git -C "$here" rev-parse --path-format=absolute --git-common-dir)/.." && pwd)"
cc="$root/scripts/win-cc.sh"
out="$main/build/win/p0/$1"
gen="$root/build/probes/gen"
[ -f "$gen/stub_api.inc" ] || "$here/build.sh" > /dev/null
rm -rf "$out"
mkdir -p "$out/stub"
printf '__attribute__((used)) const char rbsp_sac_nonce[] = "%s";\n' "$(od -An -tx1 -N16 /dev/urandom | tr -d ' \n')" > "$gen/nonce.c"
n="$gen/nonce.c"
"$cc" x64 "$here/cpuprobe.c" "$n" -o "$out/app.exe"
"$cc" x64 -I"$root/bridge" "$here/llcheck.c" "$n" -o "$out/corehost.exe"
"$cc" x64 -shared -DCALLER -I"$root/bridge" "$here/llcheck.c" "$n" -o "$out/rbsp-core.dll"
"$cc" x64 -shared -I"$root/bridge" -I"$gen" -I"$here" "$here/stubprobe.c" "$n" "$here/ort.def" -o "$out/stub/onnxruntime.dll"
"$cc" x64 -mwindows -municode "$here/workerspike.c" "$gen/worker-res.o" "$n" -o "$out/worker.exe"
"$cc" x64 -shared -Wno-cast-function-type -I"$root/bridge" -I"$here" "$here/ortprobe.c" "$n" "$here/ort.def" -o "$out/onnxruntime.dll"
echo "never-seen set in $out"

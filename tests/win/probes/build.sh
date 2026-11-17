#!/bin/sh
# Builds the Phase 0 VM probes with the pinned toolchain (scripts/win-cc.sh) into OUT
# (default build/probes). Copy OUT into build/win/p0/ in the main checkout for the VM (\\Mac\win).
#   tests/win/probes/build.sh [OUT]
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
cc="$root/scripts/win-cc.sh"
out="${1:-$root/build/probes}"
mkdir -p "$out/stub" "$out/ort" "$out/bin" "$out/planted" "$out/gen"
W="-Wall -Wextra -Werror"

# the stub OrtApi, generated from clang's AST of the vendored 1.18 header
printf '#include "onnxruntime_c_api.h"\n' > "$out/gen/h.c"
"$root/build/llvm-mingw/bin/x86_64-w64-mingw32-clang" -I"$root/bridge" -fsyntax-only \
  -Xclang -ast-dump=json -Xclang -ast-dump-filter=OrtApi "$out/gen/h.c" > "$out/gen/ast.json"
python3 -I "$here/gen_stub.py" "$out/gen/ast.json" > "$out/gen/stub_api.inc"

"$cc" x64 -shared $W -I"$root/bridge" -I"$out/gen" -I"$here" "$here/stubprobe.c" "$here/ort.def" -o "$out/stub/onnxruntime.dll"
"$cc" x64 -shared $W -Wno-cast-function-type -I"$root/bridge" -I"$here" "$here/ortprobe.c" "$here/ort.def" -o "$out/ort/onnxruntime.dll"
"$cc" x64 $W -I"$root/bridge" "$here/llcheck.c" -o "$out/bin/llcheck.exe"
"$cc" x64 -shared $W -DCALLER -I"$root/bridge" "$here/llcheck.c" -o "$out/bin/llcaller.dll"
"$cc" x64 $W "$here/cpuprobe.c" -o "$out/bin/cpuprobe-x64.exe"
"$cc" x64 $W -municode "$here/dumpnow.c" -ldbghelp -o "$out/bin/dumpnow.exe"
"$cc" arm64 $W "$here/cpuprobe.c" -o "$out/bin/cpuprobe-arm64.exe"
for a in x64 arm64; do
  "$cc" $a $W -municode "$here/mfcheck.c" -lmfplat -lmfreadwrite -lmfuuid -lole32 -o "$out/bin/mfcheck-$a.exe"
done

# the worker spike: built as the worker will be (release.md §3.4)
"$cc" --tool x64 windres "$here/worker.rc" -O coff -o "$out/gen/worker-res.o"
"$cc" x64 $W -mwindows -municode -ffunction-sections -fdata-sections -Wl,--gc-sections \
  "$here/workerspike.c" "$out/gen/worker-res.o" -o "$out/bin/workerspike.exe"

# planted DLLs: no CRT, kernel32 only
"$cc" x64 -shared $W -nostdlib -Wl,--entry,DllMain "$here/planted.c" -lkernel32 -o "$out/planted/planted.dll"
for n in ucrtbase bcrypt wintrust crypt32 version api-ms-win-crt-runtime-l1-1-0 api-ms-win-crt-stdio-l1-1-0 vcruntime140; do
  cp "$out/planted/planted.dll" "$out/planted/$n.dll"
done
rm "$out/planted/planted.dll"
echo "built into $out"

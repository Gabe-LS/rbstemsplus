#!/bin/sh
# model.c's Windows logic on the Mac (helper.md T12): helper/tests/model-win-host.c on model.c with
# RBSP_TEST_WIN_MODEL (the thread clamp, the DirectML/CPU choice, the timing lines' fields, the
# pocketfft spectrogram against vDSP's), then each check against a copy of model.c with its code
# broken: every mutant must fail the test.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
mkdir -p "$root/build"
scratch="$(mktemp -d "$root/build/model-win-host.XXXXXX")"
trap 'rm -rf "${scratch:?}"' EXIT
pf="$("$root/scripts/win-deps.sh" pocketfft)"
flac="$(brew --prefix flac)"
cd "$root"
clang -O2 -w -I"$pf" -c "$pf/pocketfft.c" -o "$scratch/pocketfft.o"
clang -O2 -Wno-deprecated-declarations -Icommon -I"$flac/include" -c helper/entry.c -o "$scratch/entry.o"
clang -O2 -Wno-deprecated-declarations -c common/plat_mac.c -o "$scratch/plat_mac.o"
clang -O2 -c common/plat_helper.c -o "$scratch/plat_helper.o"
build() {   # OUT [MODEL_C]: a mutant (MODEL_C) may leave a parameter unused, so its warnings aren't errors
  set -- "$1" ${2:+"-DMODEL_C=\"$2\"" -Wno-error}
  o="$1"; shift
  clang -O2 -Wall -Wextra -Werror -DRBSP_TEST_WIN_MODEL -DRBSP_TEST_DML_SESSION "$@" -Ihelper -Icommon -Ibridge -I"$pf" \
    helper/tests/model-win-host.c "$scratch/pocketfft.o" "$scratch/entry.o" "$scratch/plat_mac.o" "$scratch/plat_helper.o" \
    -L"$flac/lib" -lFLAC -framework CoreFoundation -framework Accelerate -framework Security -o "$o"
}
run() { "$1" "$scratch/model.onnx" "$scratch/replacement.onnx"; }
build "$scratch/model-win-host"
run "$scratch/model-win-host"

# the mutants: SED-EXPRESSION|what it breaks|optional exact assertion that must fail
caught=0 missed=0
while IFS='|' read -r expr what expected; do
  [ -n "$expr" ] || continue
  sed "$expr" helper/model.c > "$scratch/model.c"
  if cmp -s helper/model.c "$scratch/model.c"; then echo "FAIL mutant didn't apply: $what"; missed=$((missed + 1)); continue; fi
  if build "$scratch/mutant" "$scratch/model.c" 2> "$scratch/cc.log" && ! run "$scratch/mutant" > "$scratch/out" 2>&1 &&
     { [ -z "$expected" ] || grep -Fxq "FAIL $expected" "$scratch/out"; }; then
    echo "ok   mutant caught: $what"
    caught=$((caught + 1))
  else
    echo "FAIL mutant not caught: $what"
    missed=$((missed + 1))
  fi
done << 'EOF'
s/t > 64 ? 64/t > 128 ? 128/|the clamp at 64
s/t < 1 ? 1 :/t < 0 ? 1 :/|a zero count gives 1
s/asked \&\& directml_loaded \&\& append_found/asked \&\& directml_loaded/|DirectML without ORT's ordinal 4
s/ac_line_status == 1 ? "ac"/ac_line_status ? "ac"/|power=ac only for ACLineStatus 1
s/ep == MODEL_EP_DML ? "dml" : "cpu"/"cpu"/|the ep field
s/return ep == MODEL_EP_DML; }/return 0; }/|a failed DirectML run not run again on the CPU
s/(float)(fr\[2 \* k\] \* scale)/(float)(-fr[2 * k] * scale)/|the spectrogram's imaginary sign
s/const double scale = 1.0 \/ sqrt/const double scale = 0.5 \/ sqrt/|the spectrogram's scale
s/R_\[f\] = (float)(fr\[0\] \* scale);/R_[f] = (float)(fr[NFFT - 1] * scale);/|the DC bin, not Nyquist
/if (cpu_fallback(m.*return MODEL_RUN_BATCH_ERROR/s/return MODEL_RUN_BATCH_ERROR/return 1/|a failed CPU session reported as a chunk failure
/if (!m .* !m->s)/d|passing a missing session to ORT
s/plat_id_same(current.id, info->id))/1)/|omitting the file ID comparison|opening rejects a replacement with the same size and mtime
s/ && current.mtime_nsec == info->mtime_nsec//|omitting the subsecond mtime comparison|opening detects a subsecond mtime change
s/ && current.size == info->size//|omitting the size comparison|opening detects a size change with the same mtime and file ID
/if (!model_unchanged(m->path/d|omitting the comparison before CPU fallback|a model replaced before CPU fallback never reaches CreateSession
/if (!model_unchanged(path, info/d|omitting the post-CreateSession comparison|a model replaced during CPU fallback never reaches inference
EOF
echo "model-win-host mutants: $caught caught, $missed missed"
test "$missed" = 0

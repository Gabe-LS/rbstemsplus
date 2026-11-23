#!/bin/sh
# Deterministic decoded-audio comparisons, with no PC or VM. Optional case name
# and DUMP_COMPARE override let a single assertion run against a mutant.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
compare=${DUMP_COMPARE:-"$here/dump-compare.py"}
work=$(mktemp -d "$here/.dump-compare-test.XXXXXX")
trap 'rm -rf "$work"' EXIT

python3 -I - "$work" <<'PY'
from pathlib import Path
import sys

import numpy as np

work = Path(sys.argv[1])
rng = np.random.default_rng(239)
rate = 44100
n = 98317
base = rng.uniform(-0.7, 0.7, (n, 2)).astype("<f4")
# Exact half-LSBs of both signs, even and odd, plus clipping at both rails.
base[100:106] = np.array([0.5, -0.5, 1.5, -1.5, 2.5, -2.5])[:, None] / 32768
base[106:108] = [[1.25, -1.25], [-1.25, 1.25]]


def dump(name, samples, planar=False):
    (samples.T if planar else samples).astype("<f4").tofile(work / name)


dump("a", base)
dump("b", base, planar=True)
shifted = np.pad(base, ((5186, 233), (0, 0)))
dump("shift-a", shifted)
dump("shift-b", shifted, planar=True)
dump("gain", base.astype(float) * 10 ** (0.5 / 20))
for bits in (16, 24):
    scale = 1 << (bits - 1)
    source = base.copy()
    source[100:106] = np.array([0.5, -0.5, 1.5, -1.5, 2.5, -2.5])[:, None] / scale
    dump(f"pcm{bits}-b", source, planar=True)
    x = source.astype(float) * scale
    y = np.rint(x) if bits == 16 else np.copysign(np.floor(abs(x) + 0.5), x)
    dump(f"pcm{bits}-a", np.clip(y, -scale, scale - 1) / scale)

t = np.arange(-160, 161)
fir = 2 * 8000 / rate * np.sinc(2 * 8000 / rate * t) * np.kaiser(len(t), 8)
fir /= fir.sum()
dump("filter", np.column_stack([np.convolve(x, fir, mode="same") for x in base.T]))
np.savetxt(work / "fir", fir)
np.savetxt(work / "other-fir", [0.25, 0.5, 0.25])

edge = base.copy()
edge[17:33, 0] += 0.01
edge[-9:, 0] -= 0.02
dump("edge", edge, planar=True)
dump("noise", base + rng.normal(0, 3e-6, base.shape))
PY

run() {
    a=$1 b=$2
    shift 2
    python3 -I "$compare" "$work/$a" "$work/$b" "$@" > "$work/report"
}

expect() {
    if ! grep -Eq -- "$1" "$work/report"; then
        echo "FAIL $case_name (expected $1)" >&2
        cat "$work/report" >&2
        exit 1
    fi
}

for case_name in ${1:-exact shift gain quantization filter edge broadband}; do
    case "$case_name" in
        exact)
            run a b
            expect '^Lengths A=98317 frames \(interleaved\), B=98317 frames \(planar\), B-A=\+0,'
            expect '^Offset B relative to A=\+0 frames,'
            expect '^L error max=0 mean-abs=0 mean-signed=0 RMS=0$'
            expect '^R error max=0 mean-abs=0 mean-signed=0 RMS=0$'
            expect '^Verdict: exact$'
            ;;
        shift)
            run a shift-b
            expect '^Offset B relative to A=\+5186 frames,'
            expect '^Overlap=98317 frames, excluded head/tail A=0/0, B=5186/233$'
            expect '^Verdict: shifted, exact over overlap$'
            run shift-a b
            expect '^Offset B relative to A=-5186 frames,'
            expect '^Overlap=98317 frames, excluded head/tail A=5186/233, B=0/0$'
            expect '^Verdict: shifted, exact over overlap$'
            ;;
        gain)
            run gain b
            expect 'gain A/B=1\.059253[0-9]+ \(\+0\.500000 dB\)'
            expect '^Verdict: constant gain$'
            ;;
        quantization)
            run pcm16-a pcm16-b
            expect 'int16=nearest-even, int24=no$'
            expect '^Verdict: integer quantization$'
            run pcm24-a pcm24-b
            expect 'int16=no, int24=nearest-away$'
            expect '^Verdict: integer quantization$'
            ;;
        filter)
            run filter b --kernel kaiser="$work/fir" --kernel other="$work/other-fir" --plot-free "$work/spectra.csv"
            expect '^Verdict: low-pass filter difference$'
            expect '^  kernel kaiser magnitude match,'
            expect '^  kernel other different magnitude,'
            python3 -I - "$work" <<'PY'
import csv
from pathlib import Path
import re
import sys

import numpy as np

work = Path(sys.argv[1])
fir = np.loadtxt(work / "fir")
response = 20 * np.log10(np.maximum(abs(np.fft.rfft(fir, 65536)), 1e-15))
hz = np.fft.rfftfreq(65536, 1 / 44100)


def crossing(db):
    i = np.flatnonzero(response <= -db)[0]
    return np.interp(-db, response[i-1:i+1][::-1], hz[i-1:i+1][::-1])


report = (work / "report").read_text()
measurements = re.findall(
    r"cutoff=([\d.]+) Hz .*transition=([\d.]+) Hz .*passband ripple=([\d.]+) dB", report
)
assert len(measurements) == 2, report
for cutoff, width, ripple in measurements:
    assert abs(float(cutoff) - crossing(3)) < 30, cutoff
    assert abs(float(width) - (crossing(20) - crossing(1))) < 35, width
    assert 0.5 < float(ripple) < 1.1, ripple
with (work / "spectra.csv").open(newline="") as file:
    rows = list(csv.DictReader(file))
    assert len(rows) == 8194
    for start, channel in ((0, "L"), (4097, "R")):
        assert rows[start]["channel"] == channel
        assert float(rows[start]["frequency_hz"]) == 0
        assert float(rows[start+4096]["frequency_hz"]) == 22050
        assert float(rows[start+100]["a_over_b_db"]) > -0.1
        assert float(rows[start+2000]["a_over_b_db"]) < -40
        assert float(rows[start+2000]["error_power_per_hz"]) > 0
PY
            ;;
        edge)
            run edge a --a-layout planar --b-layout interleaved
            expect 'differing float32 frames=25/98317, A 17\.\.98316, B 17\.\.98316'
            expect 'error RMS first 2205/middle/last 2205 frames: 0\.000[0-9]+/0/0\.001[0-9]+'
            expect '^Verdict: L edge-localized error, R exact aligned samples$'
            ;;
        broadband)
            run noise b
            expect 'error spectrum top quarter of Nyquist=2[34-6]\.[0-9]+%, flatness=0\.9[0-9]+'
            expect '^Verdict: small broadband error \(consistent with precision, rounding or dither\)$'
            ;;
        *)
            echo "unknown case $case_name" >&2
            exit 2
            ;;
    esac
    echo "dump-compare $case_name PASS"
done

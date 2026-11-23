#!/usr/bin/env python3
"""Compare raw little-endian stereo float32 track dumps (A=rekordbox, B=helper).

B defaults to the headerless planar layout of helper/tests/decode_dump.c.
A defaults to headerless interleaved audio. Select planar explicitly for planar
captures. Supply one continuous track per file. The probe's overlapping model
chunks need to be assembled upstream before treating them as a track.

Positive offset means B is delayed (compare A[i] with B[i + offset]).
Frequencies are at the shared dump rate, usually 44100 Hz. The transfer estimate
is the effective A/B response over the aligned overlap, including any processing
already present in B. For a known effective FIR at that rate, --kernel NAME=FILE
compares its magnitude shape after removing overall gain. Ripple is measured
below the inferred -1 dB passband edge.
"""

import argparse
import csv
from dataclasses import dataclass
import math
from pathlib import Path

import numpy as np


STATS_BLOCK = 65536
FFT_SIZE = 8192


def read_dump(path, layout):
    size = path.stat().st_size
    if not size or size % 8:
        raise ValueError(f"{path}: expected a nonempty raw stereo dump in whole 8-byte frames")
    raw = np.memmap(path, dtype="<f4", mode="r")
    audio = raw.reshape(-1, 2) if layout == "interleaved" else raw.reshape(2, -1).T
    for start in range(0, len(audio), STATS_BLOCK):
        if not np.isfinite(audio[start:start + STATS_BLOCK]).all():
            raise ValueError(f"{path}: non-finite sample near frame {start}")
    return audio


def prefix(values):
    return np.concatenate(([0.0], np.cumsum(values, dtype=np.float64)))


def best_offset(a, b, limit):
    """Sum mean-removed, overlap-normalized stereo correlations at three positions."""
    ref, candidate, sign = (a, b, 1) if len(a) <= len(b) else (b, a, -1)
    width = min(len(ref), max(STATS_BLOCK, 2 * limit))
    minimum = (width + 1) // 2
    lags = np.arange(-min(limit, len(ref) - minimum), min(limit, len(candidate) - minimum) + 1)
    cross, ref_energy, cand_energy = np.zeros((3, len(lags)))
    last = len(ref) - width
    for start in sorted({0, last // 2, last}):
        lo, hi = max(0, start - limit), min(len(candidate), start + width + limit)
        shift = start + lags - lo
        first = np.maximum(0, -shift)
        stop = np.minimum(width, hi - lo - shift)
        usable = stop - first >= minimum
        first, stop, shift = first[usable], stop[usable], shift[usable]
        n = stop - first
        fft_size = 1 << (width + hi - lo - 2).bit_length()
        for channel in range(2):
            x = ref[start:start + width, channel].astype(np.float64)
            y = candidate[lo:hi, channel].astype(np.float64)
            x -= x.mean()
            y -= y.mean()
            xy = np.fft.irfft(np.fft.rfft(y, fft_size) * np.fft.rfft(x, fft_size).conj(), fft_size)
            sx, sy = prefix(x), prefix(y)
            mx = sx[stop] - sx[first]
            my = sy[stop + shift] - sy[first + shift]
            xx, yy = prefix(x * x), prefix(y * y)
            cross[usable] += xy[shift] - mx * my / n
            ref_energy[usable] += np.maximum(0, xx[stop] - xx[first] - mx * mx / n)
            cand_energy[usable] += np.maximum(0, yy[stop + shift] - yy[first + shift] - my * my / n)

    norm = np.sqrt(ref_energy * cand_energy)
    score = np.clip(np.divide(cross, norm, out=np.zeros_like(cross), where=norm > 0), -1, 1)
    quality = abs(score)
    tied = np.flatnonzero(quality >= quality.max() - 1e-10)
    best = tied[np.argmin(abs(lags[tied]))]
    other = quality[abs(lags - lags[best]) > 1]
    alternate = float(other.max()) if len(other) else None
    return int(sign * lags[best]), float(score[best]), alternate


def rms(power, count):
    return math.sqrt(power / count) if count else 0.0


def error_stats(a, b, rate):
    """Accumulate in float64 and retain bit-level locations, including signed zero."""
    edge = min(rate // 20, len(a) // 10)
    peak = absolute = bias = bb = eb = aa = 0.0
    energy = [0.0, 0.0, 0.0]
    first = last = None
    changed = 0
    bounds = (0, edge, len(a) - edge, len(a))
    for region, (lo, hi) in enumerate(zip(bounds, bounds[1:])):
        for start in range(lo, hi, STATS_BLOCK):
            stop = min(hi, start + STATS_BLOCK)
            ar, br = a[start:stop], b[start:stop]
            where = np.flatnonzero(ar.view("<u4") != br.view("<u4"))
            if where.size:
                if first is None:
                    first = start + int(where[0])
                last = start + int(where[-1])
                changed += where.size
            x, y = ar.astype(np.float64), br.astype(np.float64)
            error = x - y
            peak = max(peak, float(abs(error).max()))
            absolute += abs(error).sum()
            bias += error.sum()
            energy[region] += (error * error).sum()
            bb += (y * y).sum()
            eb += (error * y).sum()
            aa += (x * x).sum()
    power = sum(energy)
    gain = 1 + eb / bb if bb else None
    residual = max(0, power - eb * (eb / bb)) if bb else power
    return (peak, absolute / len(a), bias / len(a), rms(power, len(a)),
            changed, first, last, edge,
            [rms(e, hi - lo) for e, lo, hi in zip(energy, bounds, bounds[1:])],
            power > 0 and energy[1] <= 0.01 * power,
            gain, rms(residual, len(a)), rms(aa, len(a)))


def pcm_matches(a, b, bits):
    """Test exact sample equality after signed PCM rounding, clipping and scaling."""
    scale = 1 << (bits - 1)
    rules = {
        "nearest-even": np.rint,
        "nearest-away": lambda x: np.copysign(np.floor(abs(x) + 0.5), x),
        "nearest-up": lambda x: np.floor(x + 0.5),
        "nearest-down": lambda x: np.ceil(x - 0.5),
        "truncate": np.trunc,
        "floor": np.floor,
        "ceil": np.ceil,
    }
    for start in range(0, len(a), STATS_BLOCK):
        target = a[start:start + STATS_BLOCK].astype(np.float64) * scale
        x = b[start:start + STATS_BLOCK].astype(np.float64) * scale
        rules = {name: round_pcm for name, round_pcm in rules.items()
                 if np.array_equal(target, np.clip(round_pcm(x), -scale, scale - 1))}
        if not rules:
            break
    return list(rules)


@dataclass
class Spectrum:
    hz: np.ndarray
    a: np.ndarray
    b: np.ndarray
    error: np.ndarray
    transfer: np.ndarray
    coherence: np.ndarray


def spectra(a, b, rate):
    """Welch periodograms and H1 spectral division, summed over the entire overlap."""
    n = min(len(a), FFT_SIZE)
    hop = max(1, n // 2)
    # A half-sample-centered Hann gives nonzero weight to the endpoints, even for short dumps.
    window = np.sin(np.pi * (np.arange(n) + 0.5) / n) ** 2
    last = len(a) - n
    pa, pb, pe = np.zeros((3, n // 2 + 1))
    cross = np.zeros_like(pa, dtype=np.complex128)
    blocks = 0
    for start in range(0, last + hop, hop):
        start = min(start, last)
        x = np.fft.rfft(a[start:start + n] * window)
        y = np.fft.rfft(b[start:start + n] * window)
        pa += abs(x) ** 2
        pb += abs(y) ** 2
        pe += abs(x - y) ** 2
        cross += x * y.conj()
        blocks += 1
    h = np.divide(cross, pb, out=np.full_like(cross, np.nan), where=pb > 0)
    denom = pa * pb
    coherence = np.divide(abs(cross) ** 2, denom, out=np.full_like(pa, np.nan), where=denom > 0)
    factor = np.full(len(pa), 2 / (blocks * rate * np.sum(window ** 2)))
    factor[0] /= 2
    if n % 2 == 0:
        factor[-1] /= 2
    return Spectrum(np.fft.rfftfreq(n, 1 / rate), pa * factor, pb * factor, pe * factor,
                    h, np.clip(coherence, 0, 1))


def magnitude_db(h):
    return 20 * np.log10(np.maximum(abs(h), 1e-15))


def filter_shape(s):
    """Estimate a low-pass relative to its low-band gain from excited bins."""
    n = len(s.hz)
    if n < 129 or not s.b.max():
        return None
    excited = s.b >= s.b.max() * 1e-6
    coherent = excited & (s.coherence >= 0.8)
    # Use about 512 frequency bands for stable crossings. Keep raw bins for ripple.
    groups = np.arange(n) // max(1, n // 512)
    count = np.bincount(groups)
    supported = np.bincount(groups, weights=excited) >= 0.8 * count
    reliable = np.bincount(groups, weights=coherent) >= 0.8 * count
    hz = np.bincount(groups, weights=s.hz) / count
    # Cross-power integration can cancel phase near Nyquist. Average squared
    # magnitudes instead, weighting each excited bin by its B power.
    db = magnitude_db(s.transfer)
    weights = np.where(excited, s.b, 0)
    mag2 = np.bincount(groups, weights=np.where(excited, abs(s.transfer) ** 2, 0) * weights)
    power = np.bincount(groups, weights=weights)
    curve = magnitude_db(np.sqrt(np.divide(mag2, power, out=np.full_like(power, np.nan), where=power > 0)))
    low = (hz > 0) & (hz <= s.hz[-1] * 0.1)
    if np.mean(reliable[low]) < 0.8:
        return None
    reference = np.median(curve[low & reliable])
    relative = curve - reference

    def crossing(level, valid):
        # A crossing needs four supported bins on its low side and a supported
        # bracketing pair. This ignores isolated dips and unsupported gaps.
        below = valid & (relative <= -level)
        sustained = np.flatnonzero(np.convolve(below, np.ones(4, dtype=int), "valid") == 4)
        for index in sustained:
            if index and valid[index - 1] and relative[index - 1] > -level:
                f = (-level - relative[index - 1]) / (relative[index] - relative[index - 1])
                return hz[index - 1] + f * (hz[index] - hz[index - 1])
        return None

    cutoff = crossing(3, reliable)
    upper = supported & (hz > cutoff) if cutoff is not None else supported
    if cutoff is None or not np.any(upper) or np.median(relative[upper]) > -3:
        return None
    if np.mean(reliable[hz <= cutoff]) < 0.8:
        return None
    start = crossing(1, reliable)
    stop = crossing(20, supported)
    passband = coherent & (s.hz > 0) & (s.hz < (cutoff if start is None else start))
    ripple = float(np.ptp(db[passband])) if np.any(passband) else None
    width = stop - start if start is not None and stop is not None and stop > start else None
    return reference, ripple, cutoff, width


def kernel_matches(s, kernels, fft_size, shape):
    """Report magnitude fit of supplied effective FIRs where response is observable."""
    for name, taps in kernels:
        factor = (len(taps) + fft_size - 1) // fft_size
        expected = magnitude_db(np.fft.rfft(taps, factor * fft_size)[::factor])
        measured = magnitude_db(s.transfer)
        trusted = (s.b >= s.b.max() * 1e-6) & (s.coherence >= 0.8) & (s.b > 0)
        low = trusted & (s.hz > 0) & (s.hz <= s.hz[-1] * 0.1)
        if not np.any(low):
            print(f"  kernel {name} fit unavailable (insufficient low-band excitation)")
            continue
        expected -= np.median(expected[low])
        measured -= np.median(measured[low])
        trusted &= (expected > -40) | (measured > -40)
        error = measured[trusted] - expected[trusted]
        if not len(error):
            print(f"  kernel {name} fit unavailable (insufficient excitation)")
            continue
        fit = rms(float(np.sum(error ** 2)), len(error))
        # Require measurements through the transition before calling a low-pass a match.
        covers_transition = shape is not None and np.count_nonzero(trusted & (measured < -6)) >= 8
        verdict = "magnitude match" if fit <= 0.5 else "different magnitude"
        if not covers_transition:
            verdict = "limited coverage"
        print(f"  kernel {name} {verdict}, {fit:.4g} dB RMS over {len(error)} bins")


def describe_channel(name, a, b, start_a, start_b, rate, kernels):
    peak, mae, bias, error_rms, changed, first, last, edge, regions, edge_only, gain, residual, signal = error_stats(a, b, rate)
    print(f"{name} error max={peak:.9g} mean-abs={mae:.9g} mean-signed={bias:.9g} RMS={error_rms:.9g}")
    print(f"  error RMS first {edge}/middle/last {edge} frames: " + "/".join(f"{x:.6g}" for x in regions))
    if changed:
        print(f"  differing float32 frames={changed}/{len(a)}, A {start_a + first}..{start_a + last}, "
              f"B {start_b + first}..{start_b + last} (zero-based inclusive)")
    if gain is not None:
        db = f"{20 * math.log10(abs(gain)):+.6f} dB" if gain else "-inf dB"
        print(f"  gain A/B={gain:.9g} ({db}), gain-fit residual RMS={residual:.9g}")

    s = spectra(a, b, rate)
    power = s.error.sum()
    high = s.error[s.hz >= rate * 0.375].sum() / power if power else 0.0
    flatness = 0.0
    if power and len(s.error) >= 129:
        normalized = s.error / s.error.mean()
        flatness = math.exp(float(np.log(np.maximum(normalized, 1e-15)).mean()))
    print(f"  error spectrum top quarter of Nyquist={high:.2%}, flatness={flatness:.3f}")
    shape = filter_shape(s)
    if shape is not None:
        reference, ripple, cutoff, width = shape
        ripple_text = f"{ripple:.4g} dB p-p" if ripple is not None else "unavailable"
        width_text = f"{width:.2f} Hz" if width is not None else "unavailable"
        print(f"  relative low-pass cutoff={cutoff:.2f} Hz (-3 dB), transition={width_text} (-1 to -20 dB), "
              f"passband ripple={ripple_text}, low-band gain={reference:+.4g} dB")
    kernel_matches(s, kernels, min(len(a), FFT_SIZE), shape)

    matches = {bits: pcm_matches(a, b, bits) for bits in (16, 24)}
    print("  exact B quantization to A (signed, saturated) " +
          ", ".join(f"int{bits}={','.join(rules) or 'no'}" for bits, rules in matches.items()))
    if not changed:
        verdict = "exact aligned samples"
    elif error_rms == 0:
        verdict = "signed-zero differences"
    elif matches[16] or matches[24]:
        verdict = "integer quantization"
    elif gain is not None and gain != 0 and abs(gain - 1) > 2e-6 and residual <= signal * 2e-7:
        verdict = "constant gain"
    elif edge_only:
        verdict = "edge-localized error"
    elif shape is not None:
        verdict = "low-pass filter difference"
    elif high >= 0.9:
        verdict = "high-frequency error (consistent with a filter difference)"
    elif error_rms <= 1e-4 and flatness >= 0.5 and min(regions) >= max(regions) / 3:
        verdict = "small broadband error (consistent with precision, rounding or dither)"
    else:
        verdict = "sample mismatch"
    return verdict, s


def write_csv(path, channels):
    with path.open("x", newline="") as output:
        writer = csv.writer(output)
        writer.writerow(["channel", "frequency_hz", "a_power_per_hz", "b_power_per_hz",
                         "error_power_per_hz", "a_over_b_db", "a_over_b_phase_deg", "coherence"])
        for name, s in channels:
            writer.writerows((name, *row) for row in zip(
                s.hz, s.a, s.b, s.error, magnitude_db(s.transfer), np.angle(s.transfer, deg=True), s.coherence))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("a", type=Path, help="rekordbox dump")
    parser.add_argument("b", type=Path, help="helper dump")
    parser.add_argument("--a-layout", choices=("interleaved", "planar"), default="interleaved")
    parser.add_argument("--b-layout", choices=("interleaved", "planar"), default="planar")
    parser.add_argument("--rate", type=int, default=44100, help="shared sample rate in Hz (default 44100)")
    parser.add_argument("--max-offset", type=int, default=16384, help="search radius in frames (default 16384)")
    parser.add_argument("--plot-free", type=Path, metavar="CSV", help="write spectra to a new CSV file")
    parser.add_argument("--kernel", action="append", default=[], metavar="NAME=FIR",
                        help="compare an effective FIR (whitespace-separated coefficients, repeatable)")
    args = parser.parse_args()
    if args.rate <= 0 or args.max_offset < 0:
        parser.error("rate must be positive and max-offset must be nonnegative")
    try:
        kernels = []
        for spec in args.kernel:
            name, sep, path = spec.partition("=")
            if not sep or not name or not path:
                raise ValueError("--kernel needs NAME=FIR")
            taps = np.loadtxt(path, ndmin=1)
            if taps.ndim != 1 or not np.isfinite(taps).all() or not np.any(taps):
                raise ValueError(f"{path}: expected nonzero finite FIR coefficients")
            kernels.append((name, taps))

        a, b = read_dump(args.a, args.a_layout), read_dump(args.b, args.b_layout)
        offset, score, alternate = best_offset(a, b, args.max_offset)
        start_a, start_b = max(0, -offset), max(0, offset)
        length = min(len(a) - start_a, len(b) - start_b)
        print(f"Lengths A={len(a)} frames ({args.a_layout}), B={len(b)} frames ({args.b_layout}), "
              f"B-A={len(b) - len(a):+d}, rate={args.rate} Hz")
        alternate_text = f"{alternate:.8f}" if alternate is not None else "n/a"
        print(f"Offset B relative to A={offset:+d} frames, limit=±{args.max_offset}, "
              f"correlation={score:.8f}, alternate |correlation|={alternate_text}")
        if abs(score) < 0.2 or (alternate is not None and abs(score) - alternate < 1e-4):
            print("Alignment confidence low (weak or ambiguous correlation)")
        print(f"Overlap={length} frames, excluded head/tail A={start_a}/{len(a) - start_a - length}, "
              f"B={start_b}/{len(b) - start_b - length}")
        verdicts, channels = [], []
        for index, name in enumerate(("L", "R")):
            verdict, spectrum = describe_channel(name, a[start_a:start_a + length, index],
                                                 b[start_b:start_b + length, index],
                                                 start_a, start_b, args.rate, kernels)
            verdicts.append(verdict)
            channels.append((name, spectrum))
        verdict = verdicts[0] if verdicts[0] == verdicts[1] else f"L {verdicts[0]}, R {verdicts[1]}"
        if verdict == "exact aligned samples":
            if offset:
                verdict = "shifted, exact over overlap"
            elif len(a) != len(b):
                verdict = "length mismatch, exact over overlap"
            else:
                verdict = "exact"
        print(f"Verdict: {verdict}")
        if args.plot_free:
            write_csv(args.plot_free, channels)
    except (OSError, ValueError) as exc:
        parser.error(str(exc))


if __name__ == "__main__":
    main()

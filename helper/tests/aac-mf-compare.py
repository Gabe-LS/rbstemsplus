#!/usr/bin/env python3
"""Q3 (helper.md §2, T10): Media Foundation's AAC output (tests/win/suites/helper/aac.ps1's
results: run1.txt and <name>.s16) against each fixture's source and its MP4 tables
(helper/tests/aac-mf-fixtures.sh's DIR: <name>.wav, <name>.m4a|.mp4). Per file: the sound track's
sample table (stts total and last packet, in its timescale), edit list, iTunSMPB; MF's frames,
MF_PD_DURATION and first sample time; the lag of MF's output behind the source (the priming MF
keeps) and what is left past the source's end; the SNR once aligned.

A gate as well: it exits 1 when a file wasn't decoded, when its lag isn't one of the measured
primings (1024, 2112, 5186: helper.md §2 Q3), or when its length isn't every packet in full (the
stts total rounded up to whole packets of 1024 samples at the core's rate).

  python3 -I helper/tests/aac-mf-compare.py FIXTURES RESULTS
"""
import os
import struct
import sys
import wave

import numpy as np


def boxes(b, off, end):
    while off + 8 <= end:
        size, typ = struct.unpack(">I4s", b[off:off + 8])
        hdr = 8
        if size == 1:
            size = struct.unpack(">Q", b[off + 8:off + 16])[0]
            hdr = 16
        elif size == 0:
            size = end - off
        if size < hdr:
            return
        yield typ.decode("latin-1"), off + hdr, off + size
        off += size


def find(b, off, end, path):
    for t, s, e in boxes(b, off, end):
        if t == path[0]:
            if len(path) == 1:
                yield s, e
            else:
                skip = 4 if t == "meta" else 0
                yield from find(b, s + skip, e, path[1:])


def mp4_facts(p):
    b = open(p, "rb").read()
    f = {}
    for ms, me in find(b, 0, len(b), ["moov"]):
        for ts, te in find(b, ms, me, ["trak"]):
            hd = [b[s + 8:s + 12].decode("latin-1") for s, e in find(b, ts, te, ["mdia", "hdlr"])]
            if hd != ["soun"]:
                continue
            for s, e in find(b, ts, te, ["mdia", "mdhd"]):
                v = b[s]
                f["timescale"] = struct.unpack(">I", b[s + 12:s + 16] if v == 0 else b[s + 20:s + 24])[0]
            for s, e in find(b, ts, te, ["mdia", "minf", "stbl", "stts"]):
                n = struct.unpack(">I", b[s + 4:s + 8])[0]
                tot, last = 0, 0
                for i in range(n):
                    c, d = struct.unpack(">II", b[s + 8 + 8 * i:s + 16 + 8 * i])
                    tot += c * d
                    last = d
                f["stts"], f["last"] = tot, last
            for s, e in find(b, ts, te, ["edts", "elst"]):
                v = b[s]
                n = struct.unpack(">I", b[s + 4:s + 8])[0]
                ents = []
                o = s + 8
                for i in range(n):
                    if v == 1:
                        d, m = struct.unpack(">Qq", b[o:o + 16]); o += 20
                    else:
                        d, m = struct.unpack(">Ii", b[o:o + 8]); o += 12
                    ents.append((d, m))
                f["elst"] = ents
        k = b.find(b"iTunSMPB")
        if k >= 0:
            d = b.find(b"data", k)
            txt = b[d + 12:d + 12 + 120].split(b"\0")[0].decode("latin-1").split()
            f["smpb"] = (int(txt[1], 16), int(txt[2], 16), int(txt[3], 16))   # priming, padding, valid
    return f


def wav(p):
    w = wave.open(p)
    x = np.frombuffer(w.readframes(w.getnframes()), "<i2").reshape(-1, w.getnchannels()).astype(np.float64)
    return x, w.getframerate()


def lag(src, out, rate):
    """the lag of out behind src (samples), by cross-correlation of their first 4 s, mono"""
    n = min(len(src), len(out), 4 * rate)
    a = src[:n].mean(axis=1)
    b = out[:n + 8192].mean(axis=1) if len(out) > n else out.mean(axis=1)
    m = 1
    while m < len(a) + len(b):
        m *= 2
    c = np.fft.irfft(np.fft.rfft(b, m) * np.conj(np.fft.rfft(a, m)), m)
    k = int(np.argmax(c[:8192]))
    return k


def main():
    fix, res = sys.argv[1], sys.argv[2]
    bad = []
    rows = [l.rstrip("\n").split("\t") for l in open(os.path.join(res, "run1.txt"), encoding="utf-8") if l.strip()]
    print("name\tstts(ts)\tlast\telst(dur,media)\tsmpb(prime,pad,valid)\tsrc\tMF frames@rate ch\tMF-stts\tMF_PD_DURATION*rate\tfirst_ts\tlag\tMF-lag-src\tSNR dB")
    for r in rows:
        name = r[0]
        if len(r) < 18:
            print(name + "\t" + "\t".join(r[1:]))
            bad.append("%s: not decoded" % name)
            continue
        frames, rate, ch = int(r[1]), int(r[2]), int(r[3])
        dur, first, core = int(r[8]), int(r[9]), int(r[6])
        f = mp4_facts(os.path.join(fix, name))
        src, srate = wav(os.path.join(fix, os.path.splitext(name)[0] + ".wav"))
        p = os.path.join(res, name + ".s16")
        sz = os.path.getsize(p)
        if sz != frames * ch * 2:
            bad.append("%s: %d bytes of samples, want %d" % (name, sz, frames*ch*2))
            continue
        out = np.fromfile(p, "<i2").reshape(-1, ch).astype(np.float64)
        if out.shape[1] != src.shape[1]:
            out = out.mean(axis=1, keepdims=True) if src.shape[1] == 1 else np.repeat(out, src.shape[1], axis=1)
        stts = f.get("stts", 0)
        ts = f.get("timescale", rate)
        stts_r = stts * rate // ts if ts else stts
        if srate != rate:
            k = None
            snr = float("nan")
        else:
            k = lag(src, out, rate)
            al = out[k:k + len(src)]
            n = min(len(al), len(src))
            e = al[:n] - src[:n]
            snr = 10 * np.log10((src[:n] ** 2).sum() / max((e ** 2).sum(), 1e-9))
        print("%s\t%d(%d)\t%d\t%s\t%s\t%d\t%d@%d %d\t%+d\t%d\t%d\t%s\t%s\t%.1f" % (
            name, stts, ts, f.get("last", 0), f.get("elst", "-"), f.get("smpb", "-"), len(src), frames, rate, ch,
            frames - stts_r, round(dur * rate / 1e7), first, k if k is not None else "-",
            "%+d" % (frames - k - len(src)) if k is not None else "-", snr))
        if name == "ff-lc-22k.m4a":   # a measurement case; its output rate is still open
            continue
        if k not in (1024, 2112, 5186):
            bad.append("%s: lag %s, not a measured priming (1024, 2112, 5186)" % (name, k))
        per = 1024 * rate // core if core else 1024   # one packet at the output's rate (x2: SBR)
        full = -(-stts_r // per) * per
        if frames != full:
            bad.append("%s: %d frames, not every packet in full (%d)" % (name, frames, full))
    for b in bad:
        print("FAIL " + b, file=sys.stderr)
    if not rows:
        print("FAIL no rows in run1.txt", file=sys.stderr)
    return 1 if bad or not rows else 0


sys.exit(main())

"""win-alac.sh's fixtures, made in OUT (a scratch folder, never committed): music-like WAVs,
ALAC and AAC files from ffmpeg's and afconvert's encoders (each skipped with a note on stderr
when it doesn't work here), and broken copies. Prints win-alac.c's manifest on stdout: KIND TAB
PATH [TAB REF].

    python3 -I win-alac-fixtures.py OUT
"""
import base64
import os
import random
import struct
import subprocess
import sys
import wave
import zlib

import numpy as np

OUT = sys.argv[1]
manifest = []
made = {}


# review I6's fixture: a 7 KB random-stretch mutation of an ffmpeg ALAC file (synthetic music)
# whose packets reach dp_dec.c's 1 << (denshift - 1) with denshift 0; zlib, then base64
DENSHIFT0 = (
    "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"
)


def note(msg):
    print("note: " + msg, file=sys.stderr)


def path(name):
    return os.path.join(OUT, name)


def music(n, sr, ch, seed):
    """notes with harmonics and decays, drum hits, a stretch of full-scale noise (ALAC's escape
    frames), a stretch of digital silence, different on each channel"""
    rng = np.random.default_rng(seed)
    out = np.zeros((n, ch))
    beat = max(1, int(sr * 0.25))
    for start in range(0, n, beat):
        f0 = 55.0 * 2 ** (rng.integers(0, 36) / 12)
        ln = min(n - start, beat * int(rng.integers(1, 4)))
        tt = np.arange(ln) / sr
        env = np.exp(-tt * rng.uniform(2, 8))
        tone = sum((0.6 ** h) * np.sin(2 * np.pi * f0 * (h + 1) * tt + rng.uniform(0, 6.28)) for h in range(6))
        out[start:start + ln] += (env * tone)[:, None] * rng.uniform(0.1, 1, ch) * 0.25
        if rng.random() < 0.5:
            dl = min(n - start, int(sr * 0.05))
            out[start:start + dl] += (rng.standard_normal(dl) * np.exp(-np.arange(dl) / sr * 60))[:, None] * 0.3
    out += rng.standard_normal((n, ch)) * 1e-3
    out = out / max(1e-9, np.abs(out).max()) * 0.8
    if n > sr * 4:
        a = int(sr * 1.5)
        out[a:a + sr // 2] = rng.uniform(-1, 1, (sr // 2, ch))     # incompressible: escape frames
        out[a + sr:a + sr + sr // 2] = 0                            # silence
    return out


def write_wav(name, x, sr, bits):
    """integer PCM WAV, TPDF-dithered, full use of the low bits"""
    rng = np.random.default_rng(len(name))
    full = 2 ** (bits - 1) - 1
    q = np.round(x * full + (rng.random(x.shape) - rng.random(x.shape)))
    q = np.clip(q, -full - 1, full).astype(np.int64)
    if bits == 16:
        raw = q.astype("<i2").tobytes()
    elif bits == 24:
        b = q.astype("<i4").view(np.uint8).reshape(-1, 4)[:, :3]
        raw = b.tobytes()
    else:
        raw = q.astype("<i4").tobytes()
    w = wave.open(path(name), "wb")
    w.setnchannels(x.shape[1])
    w.setsampwidth(bits // 8)
    w.setframerate(sr)
    w.writeframes(raw)
    w.close()
    return path(name)


def run(name, cmd):
    """an encoder run: True when it made the file"""
    try:
        r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=600)
    except (OSError, subprocess.TimeoutExpired) as e:
        note("%s: %s" % (name, e))
        return False
    if r.returncode or not os.path.exists(path(name)) or os.path.getsize(path(name)) == 0:
        err = (r.stderr or r.stdout).decode("utf-8", "replace").strip().splitlines()
        note("%s: encoder failed (%s)" % (name, err[-1] if err else r.returncode))
        return False
    made[name] = path(name)
    return True


FF = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y"]


def ff(name, inputs, args, kind="alac"):
    cmd = list(FF)
    for i in inputs:
        cmd += ["-i", i]
    if run(name, cmd + args + [path(name)]):
        manifest.append((kind, path(name)))


def af(name, src, args, kind="alac"):
    if run(name, ["afconvert"] + args + [src, path(name)]):
        manifest.append((kind, path(name)))


# ---- sources
srcs = {}
def src(name, seconds_or_n, sr, ch, bits, seed):
    n = seconds_or_n if isinstance(seconds_or_n, int) else int(seconds_or_n * sr)
    srcs[name] = write_wav(name + ".wav", music(n, sr, ch, seed), sr, bits)
    return srcs[name]

s16 = src("s16", 20.0, 44100, 2, 16, 1)
s24 = src("s24", 20.0, 48000, 2, 24, 2)
s32 = src("s32", 10.0, 44100, 2, 32, 3)
smono = src("smono", 15.0, 44100, 1, 16, 4)
s96 = src("s96", 10.0, 96000, 2, 24, 5)
s51 = src("s51", 10.0, 48000, 6, 16, 6)
s51_24 = src("s51-24", 8.0, 48000, 6, 24, 7)
sshort = src("sshort", 1000, 44100, 2, 16, 8)
sexact = src("sexact", 4096 * 10, 44100, 2, 16, 9)
sexact1 = src("sexact1", 4096 * 10 + 1, 44100, 2, 16, 10)
slong = src("slong", 360.0, 44100, 2, 16, 11)
cover = path("cover.png")
subprocess.run(FF + ["-f", "lavfi", "-i", "testsrc=s=320x320", "-frames:v", "1", cover], check=False)
meta = ["-metadata", "title=Win ALAC fixture", "-metadata", "artist=RB Stems Plus tests", "-metadata", "album=Fixtures"]

# ---- ALAC: ffmpeg's encoder
ff("ff16.m4a", [s16], ["-c:a", "alac"])
ff("ff24.m4a", [s24], ["-c:a", "alac", "-sample_fmt", "s32p"])
ff("ffmono.m4a", [smono], ["-c:a", "alac"])
ff("ff96.m4a", [s96], ["-c:a", "alac", "-sample_fmt", "s32p"])
ff("ff51.m4a", [s51], ["-c:a", "alac"])
ff("ff51-24.m4a", [s51_24], ["-c:a", "alac", "-sample_fmt", "s32p"])
ff("ffshort.m4a", [sshort], ["-c:a", "alac"])
ff("ffexact.m4a", [sexact], ["-c:a", "alac"])
ff("ffexact1.m4a", [sexact1], ["-c:a", "alac"])
ff("fflong.m4a", [slong], ["-c:a", "alac"])
ff("ffcover-fast.m4a", [s16, cover], ["-map", "0", "-map", "1", "-c:a", "alac", "-c:v", "png", "-disposition:v", "attached_pic"] + meta + ["-movflags", "+faststart"])
ff("ffcover.m4a", [s16, cover], ["-map", "0", "-map", "1", "-c:a", "alac", "-c:v", "png", "-disposition:v", "attached_pic"] + meta)
ff("ffvideo-alac.mp4", [], ["-loop", "1", "-i", cover, "-i", s16, "-map", "0:v", "-map", "1:a", "-c:v", "mpeg4", "-r", "5", "-c:a", "alac", "-shortest"])
ff("ffat16.m4a", [s16], ["-c:a", "alac_at"])            # AudioToolbox's encoder through ffmpeg

# ---- ALAC: afconvert (AudioToolbox's encoder; the source bit depth in the format flags: 1 16-bit,
# 2 20-bit, 3 24-bit, 4 32-bit)
af("af16.m4a", s16, ["-f", "m4af", "-d", "alac"])
af("af24.m4a", s24, ["-f", "m4af", "-d", "alac/3"])
af("af20.m4a", s24, ["-f", "m4af", "-d", "alac/2"])
af("af32.m4a", s32, ["-f", "m4af", "-d", "alac/4"])
af("afmono.m4a", smono, ["-f", "m4af", "-d", "alac"])
af("af96.m4a", s96, ["-f", "m4af", "-d", "alac/3"])
af("af51.m4a", s51, ["-f", "m4af", "-d", "alac", "-l", "MPEG_5_1_D"])
af("afshort.m4a", sshort, ["-f", "m4af", "-d", "alac"])
af("afexact.m4a", sexact, ["-f", "m4af", "-d", "alac"])
af("afexact1.m4a", sexact1, ["-f", "m4af", "-d", "alac"])
af("aflong.m4a", slong, ["-f", "m4af", "-d", "alac"])

# ---- AAC (the probe's classification only)
ff("ffaac.m4a", [s16], ["-c:a", "aac", "-b:a", "192k"], "aac")
ff("ff-hop-boundary.m4a", [src("aac-hop-boundary", 10.9, 44100, 2, 16, 12)], ["-c:a", "aac", "-b:a", "128k"], "aac")
ff("ffaac-mono48.m4a", [path("s24.wav")], ["-ac", "1", "-c:a", "aac"], "aac")
ff("ffaac-fast.mp4", [s16], ["-c:a", "aac", "-movflags", "+faststart"], "aac")
ff("ffaac51.m4a", [s51], ["-c:a", "aac"], "aac")
ff("ffaac-cover.m4a", [s16, cover], ["-map", "0", "-map", "1", "-c:a", "aac", "-c:v", "png", "-disposition:v", "attached_pic"] + meta, "aac")
ff("ffaac-video.mp4", [], ["-loop", "1", "-i", cover, "-i", s16, "-map", "0:v", "-map", "1:a", "-c:v", "libx264", "-r", "5", "-c:a", "aac", "-shortest"], "aac")
ff("atlc.m4a", [s16], ["-c:a", "aac_at"], "aac")
ff("athe.m4a", [s16], ["-c:a", "aac_at", "-profile:a", "aac_he"], "aac")
ff("athev2.m4a", [s16], ["-c:a", "aac_at", "-profile:a", "aac_he_v2"], "aac")
af("afaac.m4a", s16, ["-f", "m4af", "-d", "aac"], "aac")
af("afhe.m4a", s16, ["-f", "m4af", "-d", "aach"], "aac")
af("afhev2.m4a", s16, ["-f", "m4af", "-d", "aacp"], "aac")
af("afld.m4a", s16, ["-f", "m4af", "-d", "aacl"], "aac")
af("afeld.m4a", s16, ["-f", "m4af", "-d", "aace"], "aac")

# ---- not AAC or ALAC, or no sample table: refused
ff("ffmp3.mp4", [s16], ["-c:a", "libmp3lame", "-f", "mp4"], "refuse")
ff("ffac3.mp4", [s16], ["-c:a", "ac3", "-f", "mp4"], "refuse")
ff("fffrag-aac.mp4", [s16], ["-c:a", "aac", "-movflags", "frag_keyframe+empty_moov"], "refuse")
# fragmented ALAC: AudioToolbox reads it, this helper doesn't (an accepted difference, helper.md;
# rekordbox for Windows checked in T2)
ff("fffrag-alac.m4a", [s16], ["-c:a", "alac", "-movflags", "frag_keyframe+empty_moov"], "mac-accepts")
ff("ffvideo-only.mp4", [], ["-loop", "1", "-i", cover, "-t", "2", "-c:v", "mpeg4", "-r", "5"], "refuse")
with open(path("wav-named.m4a"), "wb") as f:
    f.write(open(s16, "rb").read())
manifest.append(("refuse", path("wav-named.m4a")))

# ---- broken and rewritten copies. The tree edits work on a file whose moov is last (mdat's
# offsets don't move when moov changes size).
CONT = {b"moov", b"trak", b"mdia", b"minf", b"stbl", b"udta", b"edts", b"dinf", b"mvex"}


def parse(buf):
    boxes, i = [], 0
    while i + 8 <= len(buf):
        size, typ = struct.unpack(">I4s", buf[i:i + 8])
        hdr = 8
        if size == 1:
            size = struct.unpack(">Q", buf[i + 8:i + 16])[0]
            hdr = 16
        elif size == 0:
            size = len(buf) - i
        body = buf[i + hdr:i + size]
        boxes.append([typ, parse(body) if typ in CONT else bytearray(body)])
        i += size
    return boxes


def ser(boxes):
    out = b""
    for typ, body in boxes:
        b = ser(body) if isinstance(body, list) else bytes(body)
        out += struct.pack(">I4s", 8 + len(b), typ) + b
    return out


def top(buf):
    """top-level boxes: (type, offset, header, size)"""
    out, i = [], 0
    while i + 8 <= len(buf):
        size, typ = struct.unpack(">I4s", buf[i:i + 8])
        hdr = 8
        if size == 1:
            size = struct.unpack(">Q", buf[i + 8:i + 16])[0]
            hdr = 16
        elif size == 0:
            size = len(buf) - i
        out.append((typ, i, hdr, size))
        i += size
    return out


def get(boxes, *types):
    for typ, body in boxes:
        if typ == types[0]:
            return [typ, body] if len(types) == 1 else get(body, *types[1:])
    return None


def stbl(moov, t):
    return get(moov, b"trak", b"mdia", b"minf", b"stbl", t)


def moov_last(name):
    buf = open(made[name], "rb").read()
    boxes = top(buf)
    typ, off, hdr, size = boxes[-1]
    assert typ == b"moov", name + ": moov isn't last"
    return buf[:off], parse(buf[off + hdr:off + size])


def emit(name, kind, data, ref=None):
    with open(path(name), "wb") as f:
        f.write(data)
    manifest.append((kind, path(name)) + ((ref,) if ref else ()))


def edited(name, base, kind, fn):
    if base not in made:
        return
    prefix, moov = moov_last(base)
    fn(moov)
    emit(name, kind, prefix + struct.pack(">I4s", 8 + len(ser(moov)), b"moov") + ser(moov), made[base] if kind == "same" else None)


def cookie(moov):
    """the 'alac' child of the 'alac' sample entry: its body (version, flags, config)"""
    stsd = stbl(moov, b"stsd")[1]
    entry = stsd[8:]
    a = entry.find(b"alac", 8 + 28)
    return stsd, 8 + a + 4                          # the cookie's body offset in stsd


def set_cookie(off_in_cfg, fmt, value):
    def fn(moov):
        stsd, at = cookie(moov)
        struct.pack_into(fmt, stsd, at + 4 + off_in_cfg, value)
    return fn


def poke(t, at, fmt, value):
    def fn(moov):
        struct.pack_into(fmt, stbl(moov, t)[1], at, value)
    return fn


def to_co64(moov):
    s = stbl(moov, b"stco")
    n = struct.unpack(">I", s[1][4:8])[0]
    offs = struct.unpack(">%dI" % n, s[1][8:8 + 4 * n])
    s[0] = b"co64"
    s[1] = bytearray(s[1][:8] + struct.pack(">%dQ" % n, *offs))


def to_stz2(moov):
    s = stbl(moov, b"stsz")
    fixed, n = struct.unpack(">II", s[1][4:12])
    sizes = [fixed] * n if fixed else list(struct.unpack(">%dI" % n, s[1][12:12 + 4 * n]))
    assert max(sizes) < 65536
    s[0] = b"stz2"
    s[1] = bytearray(b"\0\0\0\0" + b"\0\0\0\x10" + struct.pack(">I", n) + struct.pack(">%dH" % n, *sizes))


def one_per_chunk(moov):
    """every packet its own chunk (stsc one run of 1; stco an offset each)"""
    sz = stbl(moov, b"stsz")[1]
    fixed, n = struct.unpack(">II", sz[4:12])
    sizes = [fixed] * n if fixed else list(struct.unpack(">%dI" % n, sz[12:12 + 4 * n]))
    sc = stbl(moov, b"stsc")[1]
    runs = struct.unpack(">I", sc[4:8])[0]
    ent = [struct.unpack(">III", sc[8 + 12 * i:20 + 12 * i]) for i in range(runs)]
    co = stbl(moov, b"stco")
    nch = struct.unpack(">I", co[1][4:8])[0]
    offs = struct.unpack(">%dI" % nch, co[1][8:8 + 4 * nch])
    flat, s = [], 0
    for c in range(nch):
        per = [e for e in ent if e[0] <= c + 1][-1][1]
        o = offs[c]
        for _ in range(per):
            flat.append(o)
            o += sizes[s]
            s += 1
    co[1] = bytearray(co[1][:4] + struct.pack(">I", len(flat)) + struct.pack(">%dI" % len(flat), *flat))
    stbl(moov, b"stsc")[1] = bytearray(b"\0\0\0\0" + struct.pack(">IIII", 1, 1, 1, 1))


def soun_to_vide(moov):
    struct.pack_into(">4s", get(moov, b"trak", b"mdia", b"hdlr")[1], 8, b"vide")


if "ff16.m4a" in made:
    base = "ff16.m4a"
    prefix, moov = moov_last(base)
    body = ser(moov)
    # the same file another way: moov with a 64-bit size, moov with size 0 (to the end), co64, stz2,
    # a chunk per packet
    emit("same-moov64.m4a", "same", prefix + struct.pack(">I4sQ", 1, b"moov", 16 + len(body)) + body, made[base])
    emit("same-moov0.m4a", "same", prefix + struct.pack(">I4s", 0, b"moov") + body, made[base])
    edited("same-co64.m4a", base, "same", to_co64)
    edited("same-stz2.m4a", base, "same", to_stz2)
    edited("same-chunks.m4a", base, "same", one_per_chunk)
    # broken headers ("alac": refused where AudioToolbox refuses them, else decoded as it does;
    # "refuse": refused here though AudioToolbox decodes them, kb 0 being a shift by 32 in Apple's
    # C, a short chunk count a table this helper doesn't guess at)
    emit("bad-moov-huge.m4a", "alac", prefix + struct.pack(">I4s", 0x7FFFFFF0, b"moov") + body)
    emit("bad-moov64-huge.m4a", "alac", prefix + struct.pack(">I4sQ", 1, b"moov", 1 << 62) + body)
    for cut in (12, len(body) // 3, len(body) - 3):
        emit("bad-moov-cut%d.m4a" % cut, "alac", prefix + struct.pack(">I4s", 8 + len(body), b"moov") + body[:cut])
    emit("bad-no-moov.m4a", "alac", prefix)
    edited("bad-cookie-bits17.m4a", base, "alac", set_cookie(5, ">B", 17))
    edited("bad-cookie-ch0.m4a", base, "alac", set_cookie(9, ">B", 0))
    edited("bad-cookie-ch9.m4a", base, "alac", set_cookie(9, ">B", 9))
    edited("bad-cookie-frames0.m4a", base, "alac", set_cookie(0, ">I", 0))
    edited("bad-cookie-frames-huge.m4a", base, "alac", set_cookie(0, ">I", 0x7FFFFFFF))
    edited("bad-cookie-version1.m4a", base, "alac", set_cookie(4, ">B", 1))
    edited("bad-cookie-kb0.m4a", base, "refuse", set_cookie(8, ">B", 0))
    edited("bad-stsz-count-huge.m4a", base, "alac", poke(b"stsz", 8, ">I", 0xFFFFFFFF))
    edited("bad-stsz-first-huge.m4a", base, "alac", poke(b"stsz", 12, ">I", 0xFFFFFFF0))
    edited("bad-stco-past-end.m4a", base, "alac", poke(b"stco", 8, ">I", 0xFFFFFF00))
    edited("bad-stco-count-huge.m4a", base, "alac", poke(b"stco", 4, ">I", 0x40000000))
    edited("bad-stsc-first2.m4a", base, "alac", poke(b"stsc", 8, ">I", 2))
    edited("bad-stsc-count-huge.m4a", base, "alac", poke(b"stsc", 4, ">I", 0x20000000))
    edited("bad-stts-count-huge.m4a", base, "alac", poke(b"stts", 4, ">I", 0x20000000))
    edited("bad-stsd-count0.m4a", base, "alac", poke(b"stsd", 4, ">I", 0))
    edited("bad-hdlr-vide.m4a", base, "alac", soun_to_vide)
    edited("bad-fewer-chunks.m4a", base, "refuse", poke(b"stco", 4, ">I", 1))
    # the first packet's first element a CCE (unsupported): a corrupt packet
    buf = bytearray(open(made[base], "rb").read())
    co = stbl(moov, b"stco")[1]
    first = struct.unpack(">I", co[8:12])[0]
    buf[first] = 0x40
    emit("bad-packet0-cce.m4a", "alac", bytes(buf))
    # (UB in Apple's dp_dec.c reached with denShift 0, review I6: deterministic, the same samples
    # as AudioToolbox; a 7 KB mutation of an ffmpeg ALAC file, byte-compared with AudioToolbox)
    emit("bad-denshift0.m4a", "alac", zlib.decompress(base64.b64decode(DENSHIFT0)))

# A packet whose elements' sample counts differ (review H5, I10): on a noise file, so the first
# packet is a full uncompressed one with room for the crafted packet. Two single-channel
# uncompressed elements of 10 and 4096 samples (the last equal to the frame length): only the
# count check refuses it, with the reason win-alac.c matches (run under ASan by win-alac.sh).
rng_n = np.random.default_rng(42)
srcs["snoise"] = write_wav("snoise.wav", rng_n.uniform(-0.999, 0.999, (44100, 2)), 44100, 16)
if run("ffnoise.m4a", FF + ["-i", srcs["snoise"], "-c:a", "alac", path("ffnoise.m4a")]):
    nprefix, nmoov = moov_last("ffnoise.m4a")
    ncookie_first = struct.unpack(">I", stbl(nmoov, b"stco")[1][8:12])[0]
    nsizes = stbl(nmoov, b"stsz")[1]
    nsize0 = struct.unpack(">I", nsizes[12:16])[0] if struct.unpack(">I", nsizes[4:8])[0] == 0 else struct.unpack(">I", nsizes[4:8])[0]
    bits = []
    def put(v, n):
        bits.extend((v >> (n - 1 - i)) & 1 for i in range(n))
    for count in (10, 4096):
        put(0, 3); put(0, 4); put(0, 12); put(0b1001, 4); put(count, 32)   # SCE, partial, escape
        for i in range(count):
            put((i * 997) & 0xFFFF, 16)
    put(7, 3)                                                              # END
    while len(bits) % 8:
        bits.append(0)
    pk = bytes(int("".join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits), 8))
    assert len(pk) <= nsize0, "ffnoise.m4a's first packet (%d bytes) can't hold the crafted one (%d)" % (nsize0, len(pk))
    buf = bytearray(open(made["ffnoise.m4a"], "rb").read())
    buf[ncookie_first:ncookie_first + len(pk)] = pk
    emit("bad-packet0-counts.m4a", "refuse", bytes(buf), "corrupt ALAC packet 0")

# A truncated packet (GPT robustness review, High 1): mono 16-bit, 4096 frames a packet; packet 0
# is the six bytes 00 00 12 00 00 20, its stsz entry 6. Apple's reader ends its header past the
# packet's end: refused as corrupt, never decoded from the bytes past it (run under ASan too).
if "ffmono.m4a" in made:
    tprefix, tmoov = moov_last("ffmono.m4a")
    tfirst = struct.unpack(">I", stbl(tmoov, b"stco")[1][8:12])[0]
    poke(b"stsz", 12, ">I", 6)(tmoov)
    tbuf = bytearray(tprefix + struct.pack(">I4s", 8 + len(ser(tmoov)), b"moov") + ser(tmoov))
    tbuf[tfirst:tfirst + 6] = bytes([0x00, 0x00, 0x12, 0x00, 0x00, 0x20])
    emit("bad-packet0-trunc.m4a", "refuse", bytes(tbuf), "corrupt ALAC packet 0")

# AudioToolbox's length for ALAC (m4a.c's alac_length), each compared with it: iTunSMPB's fields
# changed in place, and without it the sample table's durations, mdhd and the edit list changed
def inplace(name, base, fn):
    if base in made:
        b = bytearray(open(made[base], "rb").read())
        fn(b)
        emit(name, "alac", bytes(b))


def smpb(prim=None, rem=None, valid=None, gone=False):
    def fn(b):
        i = b.find(b"iTunSMPB")
        d = b.find(b"data", i) + 12                 # after data's type, kind and locale
        parts = b[d:d + 116].decode().split(" ")    # "", reserved, priming, remainder, valid, ...
        if prim is not None: parts[2] = "%08X" % prim
        if rem is not None: parts[3] = "%08X" % rem
        if valid is not None: parts[4] = "%016X" % valid
        b[d:d + 116] = " ".join(parts).encode()
        if gone:
            b[i:i + 8] = b"iTunXXXX"
    return fn


def full(b, t, at, fmt, value):
    struct.pack_into(fmt, b, b.find(t) + 4 + at, value)


def stts_last(delta):
    def fn(b):
        i = b.find(b"stts") + 4
        n = struct.unpack(">I", b[i + 4:i + 8])[0]
        struct.pack_into(">I", b, i + 8 + 8 * (n - 1) + 4, delta)
    return fn


if "af16.m4a" in made:
    # the valid frames: after name's "iTunSMPB", data's size, type, kind and locale, then the text
    T = int(open(made["af16.m4a"], "rb").read().split(b"iTunSMPB")[1][16:116].decode().split(" ")[4], 16)
    inplace("len-smpb-valid-less.m4a", "af16.m4a", smpb(valid=T - 1000))
    inplace("len-smpb-valid-more.m4a", "af16.m4a", smpb(valid=T + 1000))       # AudioToolbox refuses it
    # a forged length (GPT robustness review, High 2): far more frames than the packets hold
    inplace("len-smpb-valid-huge.m4a", "af16.m4a", smpb(valid=0xFFFFFFFFF))
    inplace("len-smpb-prim100.m4a", "af16.m4a", smpb(prim=100, valid=T - 100))
    inplace("len-smpb-prim5000.m4a", "af16.m4a", smpb(prim=5000, valid=T - 5000))
    inplace("len-smpb-rem-big.m4a", "af16.m4a", smpb(rem=5000))
    inplace("len-smpb-valid0.m4a", "af16.m4a", smpb(prim=5000, valid=0))
    inplace("len-smpb-gone.m4a", "af16.m4a", smpb(gone=True))
    inplace("len-af-stts-less.m4a", "af16.m4a", stts_last(1000))
inplace("len-exact-smpb-gone.m4a", "afexact.m4a", smpb(gone=True))            # AudioToolbox refuses it
inplace("len-stts-less.m4a", "ff16.m4a", stts_last(1000))
inplace("len-stts-more.m4a", "ff16.m4a", stts_last(2000))
inplace("len-mdhd-less.m4a", "ff16.m4a", lambda b: full(b, b"mdhd", 16, ">I", 882000 - 1000))
inplace("len-mdhd-0.m4a", "ff16.m4a", lambda b: full(b, b"mdhd", 16, ">I", 0))
inplace("len-elst-less.m4a", "ff16.m4a", lambda b: full(b, b"elst", 8, ">I", 882000 - 1000))
inplace("len-elst-mt100.m4a", "ff16.m4a", lambda b: full(b, b"elst", 12, ">I", 100))

if "ffcover-fast.m4a" in made:
    buf = open(made["ffcover-fast.m4a"], "rb").read()
    emit("bad-mdat-cut-half.m4a", "alac", buf[:len(buf) // 2])
    emit("bad-mdat-cut-1.m4a", "alac", buf[:-1])

# fuzz: random bytes changed in moov and in the packets, random cuts (seeded: the same files each run)
rng = random.Random(1234)
for base in ("ffexact1.m4a", "af24.m4a", "ff51-24.m4a", "af20.m4a", "af32.m4a"):
    if base not in made:
        continue
    buf = open(made[base], "rb").read()
    boxes = {t: (o, h, s) for t, o, h, s in top(buf)}
    mo, mh, ms = boxes[b"moov"]
    do, dh, ds = boxes[b"mdat"]
    stem = base.split(".")[0]
    for i in range(60):
        b = bytearray(buf)
        lo, hi = (mo, mo + ms) if i % 2 == 0 else (do + dh, do + ds)
        for _ in range(rng.randint(1, 8)):
            at = rng.randrange(lo, hi)
            b[at] = rng.randrange(256) if rng.random() < 0.7 else b[at] ^ (1 << rng.randrange(8))
        emit("fz-%s-%02d.m4a" % (stem, i), "fuzz", bytes(b))
    for i in range(10):
        emit("fz-%s-cut%02d.m4a" % (stem, i), "fuzz", buf[:rng.randrange(len(buf))])

for row in manifest:
    print("\t".join(row))

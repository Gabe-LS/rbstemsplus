"""win-mp3.sh's fixtures: MP3 files made here (never committed) from music-like signals (numpy),
with lame and ffmpeg, plus the tags and damage the probe must handle, built byte by byte.

    python3 -I win-mp3-fixtures.py OUT [--long]

OUT/*.mp3, one per case; --long adds 6-minute CBR and headerless VBR files (the probe's cost)."""
import os
import struct
import subprocess
import sys
import wave

import numpy as np

out = sys.argv[1]
long_ = "--long" in sys.argv
os.makedirs(out, exist_ok=True)
tmp = os.path.join(out, "_wav")
os.makedirs(tmp, exist_ok=True)
rng = np.random.default_rng(7)


def music(rate, seconds, ch=2, seed=1):
    """chords, a kick, hats and a bass line, 120 bpm, panned"""
    r = np.random.default_rng(seed)
    n = int(rate * seconds)
    t = np.arange(n) / rate
    beat = rate // 2
    left = np.zeros(n)
    right = np.zeros(n)
    roots = [220.0, 174.6, 196.0, 261.6]
    for b in range(0, n, beat):
        m = min(beat, n - b)
        tt = np.arange(m) / rate
        env = np.exp(-tt * 3.0)
        root = roots[(b // (beat * 4)) % 4]
        chord = sum(np.sin(2 * np.pi * root * k * tt + r.uniform(0, 6)) / (i + 1)
                    for i, k in enumerate((1, 1.25, 1.5, 2, 3)))
        kick = np.sin(2 * np.pi * (50 + 120 * np.exp(-tt * 30)) * tt) * np.exp(-tt * 8)
        hat = r.standard_normal(m) * np.exp(-tt * 60) * 0.3
        bass = np.sin(2 * np.pi * root / 2 * tt) * 0.5
        left[b:b + m] += 0.25 * chord * env + 0.6 * kick + hat * 0.7 + bass * 0.4
        right[b:b + m] += 0.2 * chord * env + 0.6 * kick + hat * 1.2 + bass * 0.4
    left += 0.02 * r.standard_normal(n)
    right += 0.02 * r.standard_normal(n)
    x = np.stack([left, right], 1) if ch == 2 else ((left + right) / 2)[:, None]
    x = x / np.max(np.abs(x)) * 0.8
    return (x * 32767).astype("<i2")


def wav(name, rate, seconds, ch=2, seed=1):
    p = os.path.join(tmp, name)
    if not os.path.exists(p):
        with wave.open(p, "wb") as w:
            w.setnchannels(ch)
            w.setsampwidth(2)
            w.setframerate(rate)
            w.writeframes(music(rate, seconds, ch, seed).tobytes())
    return p


def run(*a):
    subprocess.run(a, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def lame(name, src, *opts):
    run("lame", "--quiet", "--noreplaygain", *opts, src, os.path.join(out, name))


def put(name, data):
    with open(os.path.join(out, name), "wb") as f:
        f.write(data)


def get(name):
    with open(os.path.join(out, name), "rb") as f:
        return f.read()


def syncsafe(n):
    return bytes([(n >> 21) & 0x7F, (n >> 14) & 0x7F, (n >> 7) & 0x7F, n & 0x7F])


def id3v2(payload, major=3):
    """an ID3v2.<major> tag with one APIC-like frame of payload bytes and some padding"""
    body = b"TIT2" + struct.pack(">I", 6) + b"\0\0" + b"\0title"
    if major == 4:
        body += b"APIC" + syncsafe(len(payload) + 14) + b"\0\0" + b"\0image/jpeg\0\3\0" + payload
    else:
        body += b"APIC" + struct.pack(">I", len(payload) + 14) + b"\0\0" + b"\0image/jpeg\0\3\0" + payload
    body += b"\0" * 1024
    return b"ID3" + bytes([major, 0, 0]) + syncsafe(len(body)) + body


def id3v1():
    return b"TAG" + b"Title".ljust(30, b"\0") + b"Artist".ljust(30, b"\0") + b"Album".ljust(30, b"\0") + b"2024" + b"\0" * 30 + b"\xff"


def apev2():
    item = struct.pack("<II", 5, 0) + b"Title\0" + b"hello"
    size = len(item) + 32
    hdr = b"APETAGEX" + struct.pack("<IIII", 2000, size, 1, 0xA0000000) + b"\0" * 8
    ftr = b"APETAGEX" + struct.pack("<IIII", 2000, size, 1, 0x80000000) + b"\0" * 8
    return hdr + item + ftr


def lyrics3():
    body = b"LYRICSBEGIN" + b"IND00002" + b"10" + b"LYR00005" + b"hello"
    return body + str(len(body)).zfill(6).encode() + b"LYRICS200"


def frames(data):
    """offsets and sizes of the MPEG frames of a tag-free LAME stream (a plain header walk)"""
    br1 = [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320]
    br2 = [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160]
    sr1, sr2 = [44100, 48000, 32000], [22050, 24000, 16000]
    p, res = 0, []
    while p + 4 <= len(data) and data[p] == 0xFF and data[p + 1] & 0xE0 == 0xE0:
        ver = (data[p + 1] >> 3) & 1
        bi, si, pad = data[p + 2] >> 4, (data[p + 2] >> 2) & 3, (data[p + 2] >> 1) & 1
        br = (br1 if ver else br2)[bi]
        sr = (sr1 if ver else sr2)[si]
        size = (144 if ver else 72) * br * 1000 // sr + pad
        res.append((p, size))
        p += size
    return res


def vbri_frame(ref_hdr, nframes, nbytes):
    """a Fraunhofer VBRI frame: ref_hdr's header at 128 kbps (417 bytes at 44.1 kHz, no padding),
    zero side info, VBRI at byte 36 (frames: the audio frames after it; bytes: the stream, this
    frame included), an empty TOC"""
    h = bytes([0xFF, ref_hdr[1], (9 << 4) | (ref_hdr[2] & 0x0C), ref_hdr[3]])
    size = 144 * 128000 // 44100
    v = b"VBRI" + struct.pack(">HHHIIHHHH", 1, 1105, 75, nbytes, nframes, 0, 1, 2, 0)
    f = h + b"\0" * 32 + v
    return f + b"\0" * (size - len(f))


w44 = wav("m44.wav", 44100, 30)
w44b = wav("m44b.wav", 44100, 31.3, seed=2)
w44m = wav("m44m.wav", 44100, 20, ch=1)
w48 = wav("m48.wav", 48000, 20)
w32 = wav("m32.wav", 32000, 20)
w22 = wav("m22.wav", 22050, 20)
w24 = wav("m24.wav", 24000, 20)
w16 = wav("m16.wav", 16000, 20)
w8 = wav("m8.wav", 8000, 15)
w11 = wav("m11.wav", 11025, 15)
w12 = wav("m12.wav", 12000, 15)
wshort = wav("mshort.wav", 44100, 0.6)

lame("cbr128.mp3", w44, "-b", "128")
lame("cbr128-t.mp3", w44, "-b", "128", "-t")
lame("cbr320.mp3", w44, "-b", "320")
lame("cbr320-t.mp3", w44, "-b", "320", "-t")
lame("cbr192-stereo.mp3", w44b, "-b", "192", "-m", "s")
lame("cbr192-joint-t.mp3", w44b, "-b", "192", "-m", "j", "-t")
lame("vbr-v2.mp3", w44, "-V", "2")
lame("vbr-v2-t.mp3", w44, "-V", "2", "-t")
lame("vbr-v0-stereo-t.mp3", w44b, "-V", "0", "-m", "s", "-t")
lame("abr160.mp3", w44b, "--abr", "160")
lame("abr160-t.mp3", w44b, "--abr", "160", "-t")
lame("mono-cbr96.mp3", w44m, "-b", "96", "-m", "m")
lame("mono-cbr96-t.mp3", w44m, "-b", "96", "-m", "m", "-t")
lame("mono-vbr-t.mp3", w44m, "-V", "4", "-m", "m", "-t")
lame("mpeg1-48k.mp3", w48, "-b", "256")
lame("mpeg1-32k-t.mp3", w32, "-b", "128", "-t")
lame("mpeg2-22k.mp3", w22, "-b", "64")
lame("mpeg2-22k-t.mp3", w22, "-b", "64", "-t")
lame("mpeg2-24k-vbr.mp3", w24, "-V", "5")
lame("mpeg2-16k-t.mp3", w16, "-b", "32", "-t")
lame("mpeg2-16k-mono-vbr-t.mp3", w16, "-V", "6", "-m", "m", "-t")
lame("mpeg25-8k.mp3", w8, "-b", "16")
lame("mpeg25-11k.mp3", w11, "-b", "32")
lame("mpeg25-12k-t.mp3", w12, "-b", "32", "-t")
lame("short-0.6s.mp3", wshort, "-b", "128")
run("ffmpeg", "-nostdin", "-y", "-loglevel", "error", "-i", w44, "-c:a", "mp2", "-b:a", "192k", "-f", "mp2",
    os.path.join(out, "layer2-192.mp3"))

# tags
cbr = get("cbr128.mp3")
cbrt = get("cbr128-t.mp3")
vbr = get("vbr-v2.mp3")
vbrt = get("vbr-v2-t.mp3")
art = rng.integers(0, 256, 3 << 20, dtype=np.uint8).tobytes()          # cover art: random bytes, 3 MB
put("id3v23-art-cbr.mp3", id3v2(art) + cbr)
put("id3v24-art-vbr.mp3", id3v2(art[:500000], 4) + vbr)
put("id3v23-art-vbr-t.mp3", id3v2(art[:200000]) + vbrt)
put("id3v2-twice.mp3", id3v2(b"x" * 5000) + id3v2(b"y" * 300, 4) + cbrt)
put("id3v1-cbr-t.mp3", cbrt + id3v1())
put("id3v1-vbr.mp3", vbr + id3v1())
put("ape-id3v1-cbr-t.mp3", cbrt + apev2() + id3v1())
put("lyrics3-id3v1-cbr-t.mp3", cbrt + lyrics3() + id3v1())
put("ape-only-cbr-t.mp3", cbrt + apev2())
put("id3v2-at-end-cbr-t.mp3", cbrt + id3v2(art[:100000]))             # no footer: not seen at the end
put("id3-unknown-version.mp3", b"ID3\x05\x00\x00" + syncsafe(64) + b"\0" * 64 + cbrt)
put("id3-huge-size.mp3", b"ID3\x03\x00\x00" + bytes([0x7F, 0x7F, 0x7F, 0x7F]) + cbrt)   # 256 MB, past the end
put("id3-size-into-audio.mp3", b"ID3\x03\x00\x00" + syncsafe(3000) + b"\0" * 100 + cbrt)
put("garbage-then-cbr-t.mp3", rng.integers(0, 256, 300000, dtype=np.uint8).tobytes() + cbrt)
put("zeros-then-cbr-t.mp3", b"\0" * 200000 + cbrt)

# first frames: the scanner's restart rule, atypical first frames
f320 = get("cbr320-t.mp3")
fr320 = frames(f320)
put("first-320-then-128-t.mp3", f320[:fr320[0][1]] + cbrt)            # one 320 frame, then CBR 128
put("first4-320-then-128-t.mp3", f320[:sum(s for _, s in fr320[:4])] + cbrt)
m48 = get("mpeg1-48k.mp3")
fr48 = frames(m48)
put("two-48k-frames-then-44k.mp3", m48[:fr48[0][1] + fr48[1][1]] + cbrt)   # restart: 44.1 kHz from 3rd frame
put("three-48k-frames-then-44k.mp3", m48[:sum(s for _, s in fr48[:3])] + cbrt)
put("five-48k-frames-then-44k.mp3", m48[:sum(s for _, s in fr48[:5])] + cbrt)
# three frames mapped, then no header for a while: the probe grows its window (to 16 MB, then refuses)
put("frames3-zeros-300k-then-cbr-t.mp3", cbrt[:sum(s for _, s in frames(cbrt)[:3])] + b"\0" * 300000 + cbrt)
put("frames3-zeros-17m-then-cbr-t.mp3", cbrt[:sum(s for _, s in frames(cbrt)[:3])] + b"\0" * (17 << 20) + cbrt)
c192 = get("cbr192-joint-t.mp3")
third = len(cbrt) // 3
fr128 = frames(cbrt)
cut = [p for p, _ in fr128 if p >= third][0]
put("joined-128-192-128-t.mp3", cbrt[:cut] + c192 + cbrt[cut:])
put("joined-vbr-xing-cbr.mp3", vbr + cbrt)                            # a Xing count of the first part only

# short files: rekordbox's scanner wants three frames in a row
for k in (1, 2, 3, 4, 5, 8):
    put("frames-%d.mp3" % k, cbrt[:sum(s for _, s in fr128[:k])])
put("frames-2-and-a-bit.mp3", cbrt[:sum(s for _, s in fr128[:2]) + 3])

# damage
put("empty.mp3", b"")
put("three-bytes.mp3", b"\xff\xfb\x90")
put("garbage-1mb.mp3", rng.integers(0, 256, 1 << 20, dtype=np.uint8).tobytes())
put("zeros-100k.mp3", b"\0" * 100000)
put("text.mp3", b"this is not an mp3 file\n" * 1000)
put("cut-half-cbr.mp3", cbr[:len(cbr) // 2 + 7])
put("cut-half-vbr.mp3", vbr[:len(vbr) // 2 + 11])
put("cut-997-vbr.mp3", vbr[:len(vbr) * 997 // 1000])
put("cut-half-vbr-t.mp3", vbrt[:len(vbrt) // 2 + 5])
put("cut-in-header-cbr-t.mp3", cbrt[:fr128[200][0] + 2])
put("wav-named-mp3.mp3", open(w44, "rb").read())

# VBRI (no encoder here writes one): headerless VBR with a Fraunhofer-style VBRI frame first
fr = frames(vbrt)
put("vbri-vbr.mp3", vbri_frame(vbrt[:4], len(fr), len(vbrt) + 417) + vbrt)
put("vbri-vbr-id3.mp3", id3v2(art[:40000]) + vbri_frame(vbrt[:4], len(fr), len(vbrt) + 417) + vbrt + id3v1())

if long_:
    wl = wav("mlong.wav", 44100, 360, seed=3)
    lame("long-cbr320-t.mp3", wl, "-b", "320", "-t")
    lame("long-vbr-v0-t.mp3", wl, "-V", "0", "-t")
    lame("long-vbr-v0.mp3", wl, "-V", "0")

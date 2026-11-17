"""The fixtures of helper/tests/win-decode.sh: files the Mac's AudioToolbox path and the Windows
path (pcm.c, m4a.c, alac.c, mp3.c) both read, made in DIR (never committed), each from its own
stretch of music-like noise. Writes DIR/expect.txt: NAME <tab> same | win-refuses | mac-refuses | run-refuses |
both-refuse <tab> why <tab> win, where win is what the real Windows build (rbsp-decode-dump.exe, in
the VM: T16/T17) must give: same (the Mac's samples), within-1 (the Mac's line, and its samples
within one 16-bit step, 1/32768: for the MP3 named in win_within_1 only), decodes (the Mac refuses
it), or refuses (including every rate but 44.1 kHz until T11). helper/tests/win-decode-vm.sh
compares the VM's output with the Mac's by that column. Needs ffmpeg, flac and lame (Homebrew's); afconvert
for Apple's ALAC encoder.

    python3 -I win-decode-fixtures.py DIR
"""
import os, struct, subprocess, sys
import numpy as np

d = sys.argv[1]
os.makedirs(d, exist_ok=True)
expect = []
rates = {}                              # the fixtures not at 44.1 kHz
# rekordbox's own libmpg123 on Windows is not the Mac's build: VM round 3 measured mp3-cbr320.mp3
# 1 step (1/32768) off the Mac's decode in 537 of 711,936 samples, never more. The Mac's two paths
# use the same dylib and stay bit for bit (the second column); only this MP3's win column allows it.
win_within_1 = {"mp3-cbr320.mp3"}


def music(seconds, rate, ch, seed, amp=0.6):
    """interleaved float64 in [-1, 1): a few drifting partials, an envelope and some noise"""
    rnd = np.random.default_rng(seed)
    n = int(seconds * rate)
    t = np.arange(n) / rate
    env = 0.55 + 0.45 * np.sin(2 * np.pi * 0.7 * t + seed)
    v = np.zeros(n)
    for _ in range(6):
        f, a, p = rnd.uniform(60, 3000), rnd.uniform(0.05, 0.3), rnd.uniform(0, 6.28)
        v += a * np.sin(2 * np.pi * f * t * (1 + 0.001 * np.sin(t)) + p)
    out = np.empty((n, ch))
    for c in range(ch):
        out[:, c] = amp * env * v * (1 - 0.2 * c) + 0.02 * (rnd.random(n) - 0.5)
    return np.clip(out, -0.999, 0.999).reshape(-1)


def wav(path, x, rate, ch, fmt="f64"):
    """a WAV of our own: f64/f32 IEEE float, or s16; plain WAVE_FORMAT headers"""
    tag, bits, pack = {"f64": (3, 64, "<%dd"), "f32": (3, 32, "<%df"), "s16": (1, 16, "<%dh")}[fmt]
    x = np.asarray(x, dtype=np.float64)
    data = (np.round(x * 32767).astype("<i2") if fmt == "s16" else x.astype("<f8" if fmt == "f64" else "<f4")).tobytes()
    ba = ch * bits // 8
    fmtc = struct.pack("<HHIIHH", tag, ch, rate, rate * ba, ba, bits)
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 4 + 8 + len(fmtc) + 8 + len(data)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<I", len(fmtc)) + fmtc + b"data" + struct.pack("<I", len(data)) + data)


def run(*args):
    subprocess.run(args, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


seed = [0]


def src(seconds=6.0, rate=44100, ch=2, fmt="f64"):
    seed[0] += 1
    p = os.path.join(d, ".src-%d.wav" % seed[0])
    wav(p, music(seconds, rate, ch, seed[0]), rate, ch, fmt)
    return p


def ff(name, args, expectation="same", why="", seconds=6.0, rate=44100, ch=2):
    s = src(seconds, rate, ch)
    run("ffmpeg", "-nostdin", "-y", "-v", "error", "-i", s, *args, os.path.join(d, name))
    expect.append((name, expectation, why))
    if rate != 44100:
        rates[name] = rate


# WAV: every PCM width, float, mono, more channels, other rates
ff("pcm-u8.wav", ["-c:a", "pcm_u8"])
ff("pcm-s16.wav", ["-c:a", "pcm_s16le"])
ff("pcm-s24.wav", ["-c:a", "pcm_s24le"])
ff("pcm-s32.wav", ["-c:a", "pcm_s32le"])
ff("pcm-f32.wav", ["-c:a", "pcm_f32le"])
ff("pcm-f64.wav", ["-c:a", "pcm_f64le"])
ff("pcm-mono.wav", ["-c:a", "pcm_s16le"], ch=1)
ff("pcm-5.1.wav", ["-c:a", "pcm_s24le", "-channel_layout", "5.1"], ch=6)
ff("pcm-48k.wav", ["-c:a", "pcm_s16le"], rate=48000)
ff("pcm-96k-s24.wav", ["-c:a", "pcm_s24le"], rate=96000)
ff("pcm-22k.wav", ["-c:a", "pcm_s16le"], rate=22050)
ff("pcm-rf64.wav", ["-c:a", "pcm_s16le", "-rf64", "always"])
ff("pcm-ulaw.wav", ["-c:a", "pcm_mulaw"], "win-refuses", "a WAV encoding rekordbox's own parser doesn't take (helper.md §1: PCM and float only)")
ff("pcm-11025.wav", ["-c:a", "pcm_s16le"], "both-refuse", "11025 Hz", rate=11025)

# float WAVs with what the rule must keep: values beyond +-1, quiet ones, signed zeros
seed[0] += 1
x = list(music(4, 44100, 2, seed[0]) * 2.5) + [1e-30, -1e-30, -0.0, 0.0, 1e-45, 3.0e38, -3.0e38, 1.0, -1.0, 0.5]
wav(os.path.join(d, "float-loud.wav"), x, 44100, 2, "f32")
wav(os.path.join(d, "float64-loud.wav"), x + [1e-300, 1 + 2 ** -30, 1 + 2 ** -25, 5e38, -1e-40], 44100, 1, "f64")
expect += [("float-loud.wav", "same", ""), ("float64-loud.wav", "same", "")]

# WAV structure: an extensible 24-in-32 container, a chunk of odd size before data, a cut file
seed[0] += 1
y = music(4, 44100, 2, seed[0])
data = (np.round(y * 8388607).astype(np.int64) * 256).astype("<i4").tobytes()
fmtc = struct.pack("<HHIIHHHHI", 0xFFFE, 2, 44100, 44100 * 8, 8, 32, 22, 24, 3) + bytes([1, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71])
with open(os.path.join(d, "ext-24in32.wav"), "wb") as f:
    f.write(b"RIFF" + struct.pack("<I", 4 + 8 + len(fmtc) + 8 + len(data)) + b"WAVE")
    f.write(b"fmt " + struct.pack("<I", len(fmtc)) + fmtc + b"data" + struct.pack("<I", len(data)) + data)
expect.append(("ext-24in32.wav", "same", ""))
s16 = np.round(y * 32767).astype("<i2").tobytes()
fmtc = struct.pack("<HHIIHH", 1, 2, 44100, 44100 * 4, 4, 16)
with open(os.path.join(d, "odd-chunk.wav"), "wb") as f:
    f.write(b"RIFF" + struct.pack("<I", 4 + 8 + 16 + 8 + 5 + 1 + 8 + len(s16)) + b"WAVE")
    f.write(b"fmt " + struct.pack("<I", 16) + fmtc + b"junk" + struct.pack("<I", 5) + b"12345\0")
    f.write(b"data" + struct.pack("<I", len(s16)) + s16)
expect.append(("odd-chunk.wav", "same", ""))
with open(os.path.join(d, "pcm-s16.wav"), "rb") as f:
    whole = f.read()
with open(os.path.join(d, "cut.wav"), "wb") as f:
    f.write(whole[: len(whole) * 2 // 3])
expect.append(("cut.wav", "same", "a WAV whose data chunk says more than the file holds"))

# AIFF and AIFF-C: the compressions rekordbox takes, and one it doesn't
ff("aiff-s16.aiff", ["-c:a", "pcm_s16be"])
ff("aiff-s24.aiff", ["-c:a", "pcm_s24be"])
ff("aiff-s8.aif", ["-c:a", "pcm_s8"])
ff("aiff-s32.aiff", ["-c:a", "pcm_s32be"])
ff("aifc-sowt.aif", ["-c:a", "pcm_s16le"])
ff("aiff-mono-48k.aiff", ["-c:a", "pcm_s16be"], rate=48000, ch=1)
ff("aifc-fl32.aif", ["-c:a", "pcm_f32be"], "win-refuses", "AIFF-C fl32: rekordbox's AIFF parser takes only NONE, twos, sowt and in24")
s = src(4)
run("afconvert", "-f", "AIFC", "-d", "BEI24", s, os.path.join(d, "aifc-in24.aif"))
expect.append(("aifc-in24.aif", "same", ""))

# FLAC: widths, channels, rates, and the refused rates
ff("flac-16.flac", ["-c:a", "flac"])
ff("flac-24.flac", ["-c:a", "flac", "-sample_fmt", "s32", "-bits_per_raw_sample", "24"])
ff("flac-mono.flac", ["-c:a", "flac"], ch=1)
ff("flac-5.1.flac", ["-c:a", "flac", "-channel_layout", "5.1"], ch=6)
ff("flac-48k.flac", ["-c:a", "flac"], rate=48000)
ff("flac-96k-24.flac", ["-c:a", "flac", "-sample_fmt", "s32", "-bits_per_raw_sample", "24"], rate=96000)
ff("flac-192k.flac", ["-c:a", "flac"], rate=192000, seconds=3)
ff("flac-32k.flac", ["-c:a", "flac"], rate=32000)
ff("flac-11025.flac", ["-c:a", "flac"], "both-refuse", "11025 Hz", rate=11025)
ff("flac-352800.flac", ["-c:a", "flac"], "both-refuse", "352800 Hz", rate=352800, seconds=1)
s = src(5)
w8 = os.path.join(d, ".src-8bit.wav")
run("ffmpeg", "-nostdin", "-y", "-v", "error", "-i", s, "-c:a", "pcm_u8", w8)
run("flac", "-s", "-f", "-o", os.path.join(d, "flac-8bit.flac"), w8)
expect.append(("flac-8bit.flac", "mac-refuses", "AudioToolbox has no 8-bit FLAC; rekordbox reads FLAC with its own libFLAC, as the Windows path does"))
s = src(5)
w16 = os.path.join(d, ".src-16bit.wav")
run("ffmpeg", "-nostdin", "-y", "-v", "error", "-i", s, "-c:a", "pcm_s16le", w16)
run("flac", "-s", "-f", "--no-padding", "-o", os.path.join(d, "flac-cli.flac"), w16)
expect.append(("flac-cli.flac", "same", ""))

# AIFF's length is SSND's, whatever COMM's frame count says (AudioToolbox's rule)
def aiff(name, frames_in_comm, after=b"", ch=2):
    seed[0] += 1
    y = np.round(music(3, 44100, ch, seed[0]) * 32767).astype(">i2").tobytes()
    comm = struct.pack(">hIh", ch, frames_in_comm, 16) + bytes([0x40, 0x0E, 0xAC, 0x44, 0, 0, 0, 0, 0, 0])
    ssnd = struct.pack(">II", 0, 0) + y
    body = b"AIFF" + b"COMM" + struct.pack(">I", len(comm)) + comm + b"SSND" + struct.pack(">I", len(ssnd)) + ssnd + after
    with open(os.path.join(d, name), "wb") as f:
        f.write(b"FORM" + struct.pack(">I", len(body)) + body)
    expect.append((name, "same", "COMM says %d frames" % frames_in_comm))
    return len(y) // (2 * ch)


real = aiff("aiff-comm-right.aiff", 132300)
aiff("aiff-comm-smaller.aiff", 132300 - 5000)
aiff("aiff-comm-larger.aiff", 132300 + 5000)
id3 = b"ID3 " + struct.pack(">I", 40000) + b"ID3\x03\x00\x00" + bytes(39994)
aiff("aiff-comm-larger-id3.aiff", 132300 + 5000, id3)
aiff("aiff-comm-huge.aiff", 0xFFFFFFFF)

# plain format 1 WAV: the declared width, packed, a larger block align ignored (mono 24-bit, align
# 4, 400 bytes: 133 frames); a WAV whose data chunk comes before fmt
def wav_raw(name, fmtc, data, data_first=False):
    chunks = [b"fmt " + struct.pack("<I", len(fmtc)) + fmtc, b"data" + struct.pack("<I", len(data)) + data + b"\0" * (len(data) & 1)]
    if data_first:
        chunks.reverse()
    body = b"WAVE" + b"".join(chunks)
    with open(os.path.join(d, name), "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", len(body)) + body)


rng = np.random.default_rng(99)
wav_raw("wav-24-align4.wav", struct.pack("<HHIIHH", 1, 1, 44100, 44100 * 4, 4, 24), rng.integers(0, 256, 400, dtype=np.uint8).tobytes())
expect.append(("wav-24-align4.wav", "same", "mono 24-bit, block align 4"))
wav_raw("wav-24-align4-long.wav", struct.pack("<HHIIHH", 1, 2, 44100, 44100 * 8, 8, 24),
        (np.round(music(3, 44100, 2, 77) * 8388607).astype("<i4").view(np.uint8).reshape(-1, 4)[:, :3]).tobytes())
expect.append(("wav-24-align4-long.wav", "same", "stereo 24-bit packed, block align 8"))
seed[0] += 1
wav_raw("wav-data-first.wav", struct.pack("<HHIIHH", 1, 2, 44100, 44100 * 4, 4, 16),
        np.round(music(3, 44100, 2, seed[0]) * 32767).astype("<i2").tobytes(), data_first=True)
expect.append(("wav-data-first.wav", "same", "data before fmt"))

# FLAC behind an ID3v2 tag: read by its content, whatever the extension
with open(os.path.join(d, "flac-16.flac"), "rb") as f:
    fl = f.read()
title = b"\x00rbsp test title"
frame = b"TIT2" + struct.pack(">I", len(title)) + b"\x00\x00" + title
tag = b"ID3\x03\x00\x00" + bytes([(len(frame) >> 21) & 0x7F, (len(frame) >> 14) & 0x7F, (len(frame) >> 7) & 0x7F, len(frame) & 0x7F]) + frame
for ext in ("flac", "wav", "m4a"):
    with open(os.path.join(d, "id3-flac." + ext), "wb") as f:
        f.write(tag + fl)
    expect.append(("id3-flac." + ext, "same", "an ID3v2 tag before fLaC"))

# re-review 1's rules, each measured against AudioToolbox
def riff(name, chunks, expectation="same", why=""):
    body = b"WAVE" + b"".join(t + struct.pack("<I", len(c)) + c + b"\0" * (len(c) & 1) for t, c in chunks)
    with open(os.path.join(d, name), "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", len(body)) + body)
    expect.append((name, expectation, why))


def fmt(tag, ch, bits, align, rate=44100):
    return struct.pack("<HHIIHH", tag, ch, rate, rate * max(align, 1), align, bits)


seed[0] += 1
st16 = np.round(music(2, 44100, 2, seed[0]) * 32767).astype("<i2").tobytes()
mo16 = np.round(music(2, 44100, 1, seed[0] + 1) * 32767).astype("<i2").tobytes()
raw400 = np.random.default_rng(400).integers(0, 256, 400, dtype=np.uint8).tobytes()
# I1: the last fmt and the last data win
riff("i1-fmt-fmt-data.wav", [(b"fmt ", fmt(1, 2, 16, 4)), (b"fmt ", fmt(1, 1, 16, 2)), (b"data", st16)])
riff("i1-fmt16-fmt24-data.wav", [(b"fmt ", fmt(1, 2, 16, 4)), (b"fmt ", fmt(1, 2, 24, 6)), (b"data", st16)])
riff("i1-fmt-data-fmt.wav", [(b"fmt ", fmt(1, 2, 16, 4)), (b"data", st16), (b"fmt ", fmt(1, 1, 16, 2))])
riff("i1-data-fmt-fmt.wav", [(b"data", st16), (b"fmt ", fmt(1, 1, 16, 2)), (b"fmt ", fmt(1, 2, 16, 4))])
riff("i1-fmt-data-data400.wav", [(b"fmt ", fmt(1, 2, 16, 4)), (b"data", st16), (b"data", raw400)])
riff("i1-data400-fmt-data.wav", [(b"data", raw400), (b"fmt ", fmt(1, 2, 16, 4)), (b"data", st16)])
# I2: a plain WAV's block align is ignored, also below ch x bytes, and 0
riff("i2-s16-align2.wav", [(b"fmt ", fmt(1, 2, 16, 2)), (b"data", st16)])
riff("i2-s16-align0.wav", [(b"fmt ", fmt(1, 2, 16, 0)), (b"data", st16)])
riff("i2-m24-align2.wav", [(b"fmt ", fmt(1, 1, 24, 2)), (b"data", raw400)])
# I3: plain format 3 at other widths is integer PCM; 16 and 24 bits are refused
riff("i3-f3-8.wav", [(b"fmt ", fmt(3, 2, 8, 2)), (b"data", raw400)])
riff("i3-f3-12.wav", [(b"fmt ", fmt(3, 2, 12, 4)), (b"data", st16)])
riff("i3-f3-25.wav", [(b"fmt ", fmt(3, 1, 25, 5)), (b"data", raw400)])
riff("i3-f3-31.wav", [(b"fmt ", fmt(3, 2, 31, 8)), (b"data", raw400)])
riff("i3-f3-16.wav", [(b"fmt ", fmt(3, 2, 16, 4)), (b"data", st16)], "run-refuses", "16-bit float")
riff("i3-f3-24.wav", [(b"fmt ", fmt(3, 2, 24, 6)), (b"data", st16)], "run-refuses", "24-bit float")
# I5: channel limits (WAV: over 1024 only at 32 bits; AIFF: never over 1024)
rng5 = np.random.default_rng(5)
riff("i5-wav16-1024ch.wav", [(b"fmt ", fmt(1, 1024, 16, 2048)), (b"data", rng5.integers(0, 256, 2048 * 100, dtype=np.uint8).tobytes())])
riff("i5-wav16-1025ch.wav", [(b"fmt ", fmt(1, 1025, 16, 2050)), (b"data", rng5.integers(0, 256, 2050 * 100, dtype=np.uint8).tobytes())],
     "run-refuses", "1025 channels at 16 bits")
fl1025 = (rng5.random(1025 * 100) * 1.8 - 0.9).astype("<f4").tobytes()
riff("i5-f32-1025ch.wav", [(b"fmt ", fmt(3, 1025, 32, 4100)), (b"data", fl1025)])
riff("i5-ch65535.wav", [(b"fmt ", fmt(1, 65535, 16, 65535 * 2 % 65536)), (b"data", raw400 * 10)], "run-refuses", "65535 channels")


def aiff_raw(name, ch, frames, ssnd_extra=0, expectation="same", why=""):
    y = np.random.default_rng(ch + frames + ssnd_extra).integers(-30000, 30000, ch * frames).astype(">i2").tobytes()
    comm = struct.pack(">hIh", ch, frames, 16) + bytes([0x40, 0x0E, 0xAC, 0x44, 0, 0, 0, 0, 0, 0])
    ssnd = struct.pack(">II", 0, 0) + y
    body = b"AIFF" + b"COMM" + struct.pack(">I", len(comm)) + comm + b"SSND" + struct.pack(">I", len(ssnd) + ssnd_extra) + ssnd
    with open(os.path.join(d, name), "wb") as f:
        f.write(b"FORM" + struct.pack(">I", len(body)) + body)
    expect.append((name, expectation, why))


aiff_raw("i5-aiff16-1025ch.aiff", 1025, 100, 0, "run-refuses", "AIFF with 1025 channels")
# I7: SSND claiming more than the file: less than one frame decodes, a frame or more is refused
aiff_raw("i7-ssnd-plus2.aiff", 2, 44100, 2)
for k in (4, 5, 8):
    aiff_raw("i7-ssnd-plus%d.aiff" % k, 2, 44100, k, "run-refuses", "SSND %d bytes past the file" % k)
# I8: an ID3v2.4 tag with a footer before fLaC: refused (AudioToolbox's rule)
foot = b"ID3\x04\x00\x10" + bytes([(len(frame) >> 21) & 0x7F, (len(frame) >> 14) & 0x7F, (len(frame) >> 7) & 0x7F, len(frame) & 0x7F])
footer = b"3DI" + foot[3:]
for ext in ("flac", "wav", "m4a"):
    with open(os.path.join(d, "i8-id3-footer." + ext), "wb") as f:
        f.write(foot + frame + footer + fl)
    expect.append(("i8-id3-footer." + ext, "both-refuse", "an ID3v2.4 tag with a footer before fLaC"))
# I9: two and three ID3v2 tags before fLaC
for k in (2, 3):
    for ext in ("flac", "wav"):
        with open(os.path.join(d, "i9-id3x%d.%s" % (k, ext)), "wb") as f:
            f.write(tag * k + fl)
        expect.append(("i9-id3x%d.%s" % (k, ext), "same", "%d ID3v2 tags before fLaC" % k))

# A 42-byte FLAC (fLaC and STREAMINFO alone) claiming 2^36 - 1 frames (GPT robustness review,
# High 2): AudioToolbox doesn't open it, the Windows probe refuses it (no audio frame), and its
# decode would not size a buffer for the claim first (pcm_out grows as frames decode)
hb = bytearray(fl[:42])
hb[8 + 13] = (hb[8 + 13] & 0xF0) | 0x0F
hb[8 + 14:8 + 18] = b"\xff\xff\xff\xff"
with open(os.path.join(d, "flac-huge-claim.flac"), "wb") as f:
    f.write(bytes(hb))
expect.append(("flac-huge-claim.flac", "both-refuse", "STREAMINFO claims 2^36 - 1 frames, the file holds none"))

# ALAC (helper/tests/win-alac.sh has the full set), MP3 (win-mp3.sh), AAC (T10: not on Windows yet)
s = src(5)
try:   # Apple's encoder: not reachable inside a sandbox
    run("afconvert", "-f", "m4af", "-d", "alac", s, os.path.join(d, "alac-apple.m4a"))
    expect.append(("alac-apple.m4a", "same", ""))
except subprocess.CalledProcessError:
    print("note: afconvert has no ALAC encoder here (a sandbox?): alac-apple.m4a left out")
ff("alac-ffmpeg-24.m4a", ["-c:a", "alac", "-sample_fmt", "s32p"], seconds=5)
ff("alac-48k.m4a", ["-c:a", "alac"], rate=48000, seconds=5)
s = src(8, fmt="s16")                   # lame reads integer WAV only
run("lame", "--quiet", "-b", "320", s, os.path.join(d, "mp3-cbr320.mp3"))
expect.append(("mp3-cbr320.mp3", "same", ""))
s = src(8, rate=48000, fmt="s16")
run("lame", "--quiet", "-V", "2", s, os.path.join(d, "mp3-vbr-48k.mp3"))
expect.append(("mp3-vbr-48k.mp3", "same", ""))
rates["mp3-vbr-48k.mp3"] = 48000
ff("aac.m4a", ["-c:a", "aac", "-b:a", "192k"], "win-refuses", "AAC: Media Foundation's decoder is T10")
for f in os.listdir(d):
    if f.startswith(".src"):
        os.remove(os.path.join(d, f))
with open(os.path.join(d, "expect.txt"), "w") as f:
    for n, e, w in expect:
        win = "refuses" if e in ("win-refuses", "both-refuse", "run-refuses") or n in rates else "decodes" if e == "mac-refuses" else "same"
        if win == "same" and n in win_within_1:
            win = "within-1"
        f.write("%s\t%s\t%s\t%s\n" % (n, e, w, win))
print(len(expect), "fixtures")

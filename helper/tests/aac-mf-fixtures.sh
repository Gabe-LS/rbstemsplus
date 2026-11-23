#!/bin/sh
# The AAC fixtures for Q3 (helper.md §2, T10), made into DIR (never committed): each from its own
# stretch of music-like noise (src.py: seeded tones and filtered noise), with Apple's encoder
# (afconvert, macOS) and ffmpeg's. DIR/<name>.wav is each one's source, for the offset and length
# against Media Foundation's output (helper/tests/aac-mf-compare.py).
#
#   helper/tests/aac-mf-fixtures.sh DIR
set -eu
d="$1"
mkdir -p "$d"
src() {   # NAME SECONDS RATE CHANNELS SEED
  python3 -I - "$d/$1.wav" "$2" "$3" "$4" "$5" << 'PY'
import sys, wave
import numpy as np
out, secs, rate, ch, seed = sys.argv[1], float(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
r = np.random.default_rng(seed)
n = int(secs * rate)
t = np.arange(n) / rate
x = np.zeros((n, ch))
for c in range(ch):
    for f in r.uniform(60, 5000, 12):
        x[:, c] += r.uniform(0.02, 0.08) * np.sin(2 * np.pi * f * t + r.uniform(0, 6.28)) * (0.6 + 0.4 * np.sin(2 * np.pi * r.uniform(0.1, 2) * t))
    noise = np.convolve(r.normal(0, 0.05, n), np.ones(8) / 8, "same")
    x[:, c] += noise * (0.5 + 0.5 * np.sign(np.sin(2 * np.pi * 2 * t)))   # a beat
w = wave.open(out, "wb")
w.setnchannels(ch); w.setsampwidth(2); w.setframerate(rate)
w.writeframes((np.clip(x, -1, 1) * 32767).astype("<i2").tobytes())
w.close()
PY
}
apple() {   # NAME SECONDS RATE CHANNELS SEED FORMAT [AFCONVERT ARGS...]: FORMAT -f m4af or mp4f
  n="$1" s="$2" r="$3" c="$4" k="$5" ext="$6"; shift 6
  src "$n" "$s" "$r" "$c" "$k"
  afconvert -f "$( [ "$ext" = mp4 ] && echo mp4f || echo m4af )" "$@" "$d/$n.wav" "$d/$n.$ext"
}
ff() {   # NAME SECONDS RATE CHANNELS SEED EXT [FFMPEG ARGS...]
  n="$1" s="$2" r="$3" c="$4" k="$5" ext="$6"; shift 6
  src "$n" "$s" "$r" "$c" "$k"
  ffmpeg -nostdin -loglevel error -y -i "$d/$n.wav" "$@" "$d/$n.$ext"
}
apple apple-cbr128 30 44100 2 1 m4a -d aac -b 128000 -s 0
apple apple-cbr320 30 44100 2 2 m4a -d aac -b 320000 -s 0
apple apple-vbr 30 44100 2 3 m4a -d aac -s 3 -q 127
apple apple-mono 20 44100 1 4 m4a -d aac -b 96000
apple apple-48k 20 48000 2 5 m4a -d aac -b 192000
apple apple-he 30 44100 2 6 m4a -d aach -b 64000
apple apple-hev2 30 44100 2 7 m4a -d aacp -b 32000
apple apple-long 360 44100 2 8 m4a -d aac -b 256000
apple apple-mp4 20 44100 2 9 mp4 -d aac -b 192000
apple apple-short 4.5 44100 2 10 m4a -d aac -b 192000
ff ff-128 30 44100 2 11 m4a -c:a aac -b:a 128k
ff ff-320 30 44100 2 12 m4a -c:a aac -b:a 320k
ff ff-mono 20 44100 1 13 m4a -c:a aac -b:a 96k
ff ff-short-last 20.0068 44100 2 14 m4a -c:a aac -b:a 192k
ff ff-noelst 20 44100 2 15 m4a -c:a aac -b:a 192k -use_editlist 0
ff ff-aac_at 20 44100 2 16 m4a -c:a aac_at -b:a 192k
ff ff-lc-22k 5 22050 2 18 m4a -c:a aac -profile:a aac_low -b:a 64k
ff ff-lc-16k-m4a 5 16000 2 20 m4a -c:a aac -profile:a aac_low -b:a 48k
ff ff-lc-16k-mp4 5 16000 2 20 mp4 -c:a aac -profile:a aac_low -b:a 48k
ff ff-lc-24k-m4a 5 24000 2 21 m4a -c:a aac -profile:a aac_low -b:a 64k
ff ff-lc-24k-mp4 5 24000 2 21 mp4 -c:a aac -profile:a aac_low -b:a 64k
ff ff-lc-32k-mono-m4a 5 32000 1 22 m4a -c:a aac -profile:a aac_low -b:a 64k
ff ff-lc-32k-mono-mp4 5 32000 1 22 mp4 -c:a aac -profile:a aac_low -b:a 64k
# mdhd ends before CHUNK_HOP - CHUNK_PRE; the 471st full packet crosses it (T17).
ff ff-hop-boundary 10.9 44100 2 19 m4a -c:a aac -b:a 128k
src ff-video 20 44100 2 17
ffmpeg -nostdin -loglevel error -y -f lavfi -i testsrc=size=320x240:rate=25:duration=20 -i "$d/ff-video.wav" \
  -map 0:v -map 1:a -c:v mpeg4 -c:a aac -b:a 192k "$d/ff-video.mp4"
ls "$d" | grep -cv '\.wav$'

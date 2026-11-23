#!/bin/sh
# Exercise the Windows rate guard on the host, with a real WAV swapped after the first open, and
# a proved rate (48 kHz) passed on to the resampler, including AAC with a simulated Source Reader.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
flac="${FLAC_PREFIX:-/opt/homebrew}"
mkdir -p "$root/build"
scratch="$(mktemp -d "$root/build/rate-host.XXXXXX")"
trap 'rm -rf "${scratch:?}"' EXIT
cd "$root"
python3 -I - "$scratch/track.wav" "$scratch/replacement.wav" "$scratch/proved.wav" "$scratch/long48.wav" <<'PY'
import struct, sys, wave

for path, rate, sample, frames in ((sys.argv[1], 44100, 8192, 4), (sys.argv[2], 176400, 16384, 5), (sys.argv[3], 48000, 16384, 5), (sys.argv[4], 48000, 16384, 524657)):
    with wave.open(path, 'wb') as f:
        f.setparams((1, 2, rate, 0, 'NONE', ''))
        f.writeframes(struct.pack('<h', sample) * frames)
PY
ffmpeg -loglevel error -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=1" -ac 2 -c:a aac -b:a 128k "$scratch/aac48.m4a"
ffmpeg -loglevel error -f lavfi -i "sine=frequency=440:sample_rate=22050:duration=1" -ac 2 -c:a aac -profile:a aac_low -b:a 64k "$scratch/aac22.m4a"
ffmpeg -loglevel error -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=1" -ac 2 -c:a aac -b:a 128k "$scratch/aac44.m4a"
ffmpeg -loglevel error -f lavfi -i "sine=frequency=440:sample_rate=32000:duration=1" -ac 2 -c:a aac -profile:a aac_low -b:a 96k "$scratch/aac32.m4a"
ffmpeg -loglevel error -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=1" -ac 1 -c:a aac -profile:a aac_low -b:a 96k "$scratch/aac48mono.m4a"
mkdir "$scratch/Contents" "$scratch/Contents/MacOS"
ln -s "${MPG123_LIB:-/opt/homebrew/opt/mpg123/lib/libmpg123.0.dylib}" "$scratch/Contents/MacOS/libmpg123.0.dylib"
for ch in 1 2; do
  for mode in cbr vbr; do
    if [ "$mode" = cbr ]; then set -- -b:a 96k; else set -- -q:a 3; fi
    ffmpeg -loglevel error -f lavfi -i "aevalsrc=sin(2*PI*440*t)|sin(2*PI*660*t):s=32000:d=1" \
      -ac "$ch" -c:a libmp3lame "$@" "$scratch/mp3-$mode-${ch}ch.mp3"
  done
done
ffmpeg -loglevel error -f lavfi -i "sine=frequency=440:sample_rate=32000:duration=1" "$scratch/pcm32.wav"
for ch in 1 2; do
  for rate in 88200 192000; do
    stem="$scratch/rate-$rate-$ch"
    ffmpeg -loglevel error -f lavfi -i "sine=sample_rate=$rate:duration=0.1" -ac "$ch" "$stem.wav"
    ffmpeg -loglevel error -i "$stem.wav" "$stem.flac" "$stem.aiff" -c:a alac "$stem.m4a"
    cp "$stem.flac" "$stem.fla"
    cp "$stem.aiff" "$stem.aif"
    cp "$stem.m4a" "$stem.mp4"
    ffmpeg -loglevel error -i "$stem.wav" -c:a alac -sample_fmt s32p "$stem-alac24.m4a"
  done
  for rate in 16000 24000; do
    stem="$scratch/rate-$rate-$ch"
    ffmpeg -loglevel error -f lavfi -i "sine=sample_rate=$rate:duration=0.1" -ac "$ch" \
      -c:a libmp3lame -b:a 48k "$stem-cbr.mp3"
    ffmpeg -loglevel error -f lavfi -i "sine=sample_rate=$rate:duration=0.1" -ac "$ch" \
      -c:a libmp3lame -q:a 3 "$stem-vbr.mp3"
  done
  for rate in 16000 22050 24000 32000; do
    stem="$scratch/rate-$rate-$ch"
    ffmpeg -loglevel error -f lavfi -i "sine=sample_rate=$rate:duration=0.1" -ac "$ch" \
      -c:a aac -profile:a aac_low "$stem-aac.m4a"
    cp "$stem-aac.m4a" "$stem-aac.mp4"
  done
done
for ch in 1 2; do
  # the exact cells at the other resampled rates (decode.c's win_cells)
  for rate in 22050 48000 96000; do
    stem="$scratch/rate-$rate-$ch"
    ffmpeg -loglevel error -f lavfi -i "sine=sample_rate=$rate:duration=0.1" -ac "$ch" "$stem.wav"
    ffmpeg -loglevel error -i "$stem.wav" "$stem.flac" "$stem.aiff" -c:a alac "$stem.m4a"
    cp "$stem.flac" "$stem.fla"
    cp "$stem.aiff" "$stem.aif"
    cp "$stem.m4a" "$stem.mp4"
  done
  for rate in 22050 48000; do
    ffmpeg -loglevel error -f lavfi -i "sine=sample_rate=$rate:duration=0.1" -ac "$ch" \
      -c:a libmp3lame -b:a 48k "$scratch/rate-$rate-$ch-cbr.mp3"
  done
done
for enc in s32 f32; do
  ffmpeg -loglevel error -f lavfi -i "sine=sample_rate=48000:duration=0.1" -ac 2 -c:a "pcm_${enc}le" "$scratch/rate-48000-2-$enc.wav"
done
ffmpeg -loglevel error -f lavfi -i "sine=sample_rate=96000:duration=0.1" -ac 2 -c:a pcm_f32le "$scratch/rate-96000-2-f32.wav"
ffmpeg -loglevel error -f lavfi -i "sine=sample_rate=96000:duration=0.1" -ac 2 -c:a pcm_u8 "$scratch/rate-96000-2-u8.wav"
ffmpeg -loglevel error -f lavfi -i "sine=sample_rate=48000:duration=0.1" -ac 2 -c:a pcm_s16le "$scratch/rate-48000-2-sowt.aiff"
ffmpeg -loglevel error -f lavfi -i "sine=sample_rate=48000:duration=0.1" -ac 6 "$scratch/rate-48000-6.wav"
ffmpeg -loglevel error -i "$scratch/rate-48000-6.wav" "$scratch/rate-48000-6.flac"
cp "$scratch/rate-48000-6.flac" "$scratch/rate-48000-6.fla"
ffmpeg -loglevel error -f lavfi -i "anullsrc=r=48000:cl=2.1:d=0.1" "$scratch/rate-48000-3.wav"
cp "$scratch/aac48.m4a" "$scratch/rate-48000-2-aac.wav"
cp "$scratch/rate-22050-2-aac.m4a" "$scratch/rate-22050-2-aac.flac"
for rate in 88200 192000; do
  ffmpeg -loglevel error -f lavfi -i "anullsrc=r=$rate:cl=2.1:d=0.1" "$scratch/rate-$rate-3.wav"
done
for rate in 16000 24000; do
  ffmpeg -loglevel error -f lavfi -i "sine=sample_rate=$rate:duration=0.1" "$scratch/rate-$rate-1.wav"
done
cp "$scratch/rate-88200-1.wav" "$scratch/rate-88200-1-wav.flac"
cp "$scratch/rate-16000-2-aac.m4a" "$scratch/rate-16000-2-aac.wav"
for bits in 16 24; do
  sample_fmt=s16
  if [ "$bits" = 24 ]; then sample_fmt=s32; fi
  ffmpeg -loglevel error -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=1" -ac 2 \
    -c:a flac -sample_fmt "$sample_fmt" -bits_per_raw_sample "$bits" "$scratch/flac-$bits.flac"
  cp "$scratch/flac-$bits.flac" "$scratch/flac-$bits.FLA"
done

python3 -I "$here/alac20-fixture.py" "$scratch/alac20.m4a"
for rate in 44100 48000; do
  for ch in 1 2; do
    python3 -I "$here/alac20-fixture.py" "$scratch/alac32-$rate-$ch.m4a" 32 "$rate" "$ch"
  done
done
python3 -I - "$scratch" <<'PY'
from pathlib import Path
from struct import pack
import sys


def box(kind, body):
    return pack('>I4s', len(body) + 8, kind) + body


def fixture(name, asc, descriptions=1, rate=48000, ch=2):
    config = b'\x40\x15' + bytes(11) + asc
    esds = box(b'esds', bytes(4) + bytes((4, len(config))) + config)
    entry = box(b'mp4a', bytes(16) + pack('>HHHHI', ch, 16, 0, 0, rate << 16) + esds)
    stsd = box(b'stsd', pack('>II', 0, descriptions) + entry * descriptions)
    stts = box(b'stts', pack('>IIII', 0, 1, 47, 1024))
    mdhd = box(b'mdhd', pack('>IIIII', 0, 0, 0, rate, 48128))
    hdlr = box(b'hdlr', bytes(8) + b'soun')
    mdia = box(b'mdia', mdhd + hdlr + box(b'minf', box(b'stbl', stsd + stts)))
    Path(sys.argv[1], name + '.m4a').write_bytes(box(b'moov', box(b'trak', mdia)))


fixture('lc', b'\x05\x02\x11\x90')                  # LC, 48 kHz, stereo
fixture('he', b'\x05\x04\x2b\x11\x88\x00')          # HE-AAC with a 24 kHz core
fixture('sbr', b'\x05\x05\x11\x90\x56\xe5\x98')     # LC with an SBR sync extension at 48 kHz
fixture('main', b'\x05\x02\x09\x90')                # AAC Main, 48 kHz, stereo
fixture('unknown', b'')                             # Only the sample entry gives rate and channels
fixture('multi-description', b'\x05\x02\x11\x90', descriptions=2)
fixture('rate-16000-2-main', b'\x05\x02\x0c\x10', rate=16000)
# MPEG-2 Layer II headers at 24 kHz, spaced as the shared frame scanner expects.
Path(sys.argv[1], 'rate-24000-1-layer2.mp3').write_bytes((b'\xff\xf5\x64\xc0' + bytes(140)) * 8)
# Layer II at the other MP3 rates: MPEG-2 22.05 kHz 48 kbit/s (156-byte frames), MPEG-1 32 and
# 48 kHz 128 kbit/s (576 and 384 bytes)
Path(sys.argv[1], 'rate-22050-1-layer2.mp3').write_bytes((b'\xff\xf5\x60\xc0' + bytes(152)) * 8)
Path(sys.argv[1], 'rate-32000-1-layer2.mp3').write_bytes((b'\xff\xfd\x88\xc0' + bytes(572)) * 8)
Path(sys.argv[1], 'rate-48000-1-layer2.mp3').write_bytes((b'\xff\xfd\x84\xc0' + bytes(380)) * 8)
PY
for mode in production research; do
  set --
  if [ "$mode" = research ]; then set -- -DRBSP_TEST_FORMAT_RATES; fi
  clang -O2 -Wall -Wextra -Werror -DRBSP_WIN_DECODE -DRBSP_TEST_WIN_RATE -DRBSP_TEST_MF_TRACK "$@" -Ihelper -Icommon \
    -Dplat_file_open_path=rate_test_open -c helper/decode.c -o "$scratch/decode.o"
  clang -O2 -Wall -Wextra -Werror -Wno-deprecated-declarations -Wl,-dead_strip \
    -DRBSP_WIN_DECODE -DRBSP_TEST_WIN_RATE -DRBSP_TEST_MF_TRACK "$@" -Ihelper -Icommon -I"$flac/include" \
    helper/tests/rate-host.c "$scratch/decode.o" helper/pcm.c helper/mp3.c helper/m4a.c helper/big.c \
    common/plat_mac.c common/plat_helper.c -L"$flac/lib" -lFLAC -framework CoreFoundation -framework Accelerate \
    -framework Security -o "$scratch/rate-host"
  cp "$scratch/track.wav" "$scratch/original.wav"
  cp "$scratch/replacement.wav" "$scratch/swap.wav"
  touch -t 200001010000 "$scratch/original.wav"
  echo "rate-host: $mode build"
  "$scratch/rate-host" "$scratch/original.wav" "$scratch/swap.wav" "$scratch/proved.wav" "$scratch/aac48.m4a" "$scratch/aac22.m4a" "$scratch/long48.wav" "$scratch/aac44.m4a" "$scratch"
done

# Execute the build's research marker gate against text fixtures.
rate_gate="$(sed -n '/^for flag in .*AAC MF type change:/,/^done$/p' "$here/../build-win.sh")"
test -n "$rate_gate"
out="$scratch"
fail() { echo "$*" >&2; exit 1; }
expect_gate_rejection() {
  if (eval "$rate_gate") > "$scratch/gate.log" 2>&1 ||
     ! grep -qF -- "$1" "$scratch/gate.log"; then
    fail "FAIL candidate rate gate: expected $1"
  fi
}
research=
for marker in --test-unproved-rates 'Candidate resampler table:'; do
  printf '%s\n' "$marker" > "$out/rbsp-prepare.exe"
  expect_gate_rejection "has $marker"
done
research=1
printf '%s\n' --test-unproved-aac --test-unproved-mp3 'AAC MF type change:' 'Candidate resampler table:' > "$out/rbsp-prepare.exe"
expect_gate_rejection 'has no --test-unproved-rates'
printf '%s\n' --test-unproved-aac --test-unproved-mp3 'AAC MF type change:' --test-unproved-rates > "$out/rbsp-prepare.exe"
expect_gate_rejection 'has no Candidate resampler table:'
echo 'rate-host release gate: candidate option and diagnostics required in research and rejected in release'

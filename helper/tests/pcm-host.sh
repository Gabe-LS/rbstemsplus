#!/bin/sh
# Focused host checks of the Windows PCM reader. FLAC_PREFIX can point at a local libFLAC.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
flac="${FLAC_PREFIX:-/opt/homebrew}"
mkdir -p "$root/build"
scratch="$(mktemp -d "$root/build/pcm-host.XXXXXX")"
trap 'rm -rf "${scratch:?}"' EXIT
cd "$root"
clang -O2 -Wall -Wextra -Werror -Wno-deprecated-declarations -Wl,-dead_strip \
  -DRBSP_WIN_DECODE -Ihelper -Icommon -I"$flac/include" helper/tests/pcm-host.c helper/big.c \
  helper/m4a.c helper/mp3.c common/plat_mac.c \
  -L"$flac/lib" -lFLAC -framework CoreFoundation -framework Accelerate -framework Security -o "$scratch/pcm-host"
python3 -I - "$scratch" <<'PY'
import struct, sys
from pathlib import Path

out = Path(sys.argv[1])

def fmt(rate):
    return struct.pack("<4sIHHIIHH", b"fmt ", 16, 1, 1, rate, 2 * rate, 2, 16)
body = b"WAVE" + fmt(44100) + b"data\x02\x00\x00\x00\x00\x00" + fmt(48000)
(out / "two-fmt.wav").write_bytes(struct.pack("<4sI", b"RIFF", len(body)) + body)

# IEEE float, mono, with a ds64 chunk and placeholder data size for RF64.
for bits, sample_format in ((32, "f"), (64, "d")):
    width = bits // 8
    data = struct.pack("<2" + sample_format, 1.5, -0.25)
    for form, tag in (("plain", 3), ("extensible", 0xFFFE)):
        header = struct.pack("<HHIIHH", tag, 1, 44100, 44100 * width, width, bits)
        if form == "extensible":
            ieee_float = bytes.fromhex("03000000 0000 1000 8000 00aa00389b71")
            header += struct.pack("<HHI", 22, bits, 4) + ieee_float
        else:
            header += struct.pack("<H", 0)
        fmt_chunk = struct.pack("<4sI", b"fmt ", len(header)) + header
        fact = struct.pack("<4sII", b"fact", 4, 2)
        for container in (b"RIFF", b"RF64"):
            riff_size = 4 + len(fmt_chunk) + len(fact) + 8 + len(data)
            ds64 = b""
            data_size = len(data)
            if container == b"RF64":
                ds64 = struct.pack("<4sIQQQI", b"ds64", 28, riff_size + 36, len(data), 2, 0)
                riff_size = data_size = 0xFFFFFFFF
            wav = struct.pack("<4sI4s", container, riff_size, b"WAVE") + ds64 + fmt_chunk + fact
            wav += struct.pack("<4sI", b"data", data_size) + data
            (out / f"{container.decode()}-{form}-f{bits}.wav").write_bytes(wav)
PY
python3 -I - "$scratch/samples.raw" <<'PY'
import struct, sys
with open(sys.argv[1], "wb") as f:
    f.write(struct.pack("<44100h", *(i % 4096 for i in range(44100))))
PY
encode() {
  flac -s --force-raw-format --endian=little --sign=signed --channels=1 --bps=16 --sample-rate=44100 "$@"
}
encode -o "$scratch/known.flac" "$scratch/samples.raw"
# stdin has no declared length, and the pipe prevents rewriting STREAMINFO at the end.
encode -c - < "$scratch/samples.raw" | cat > "$scratch/unknown.flac"
test "$(metaflac --show-total-samples "$scratch/unknown.flac")" = 0
flac -s -t "$scratch/unknown.flac"
"$scratch/pcm-host" "$scratch/two-fmt.wav" "$scratch/unknown.flac" "$scratch/known.flac" "$scratch"/R*-f*.wav

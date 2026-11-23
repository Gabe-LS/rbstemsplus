"""Make an ALAC fixture with one uncompressed packet (20-bit stereo by default).

FFmpeg's ALAC encoder uses at most 24 bits. Use its M4A container,
replace the audio packet and set the ALAC cookie to the actual depth.
"""
from pathlib import Path
from struct import pack_into, unpack_from
import subprocess
import sys

path = Path(sys.argv[1])
bits, rate, channels = map(int, sys.argv[2:]) if len(sys.argv) > 2 else (20, 44100, 2)
frames = 1024
subprocess.run([
    "ffmpeg", "-v", "error", "-f", "lavfi", "-i", f"anullsrc=r={rate}:cl={channels}c",
    "-frames:a", "1", "-af", f"atrim=end_sample={frames}", "-c:a", "alac",
    "-sample_fmt", "s32p", str(path),
], check=True)
data = bytearray(path.read_bytes())
cookie = data.index(b"\x00\x00\x00\x24alac") + 12
assert data[cookie + 5] == 24
data[cookie + 5] = bits

# SCE or CPE, instance 0, partial frame, no shifted bytes, escape coding.
packet = f"{channels - 1:03b}" + "0" * 16 + "1001" + f"{frames:032b}"
sign = 1 << (bits - 1)
mask = (1 << bits) - 1
for i in range(frames):
    packet += f"{(i * 997 + sign) & mask:0{bits}b}"
    if channels == 2:
        packet += f"{(sign - 1 - i * 3121) & mask:0{bits}b}"
packet += "111"  # END
packet += "0" * (-len(packet) % 8)
audio = int(packet, 2).to_bytes(len(packet) // 8, "big")

sizes = data.index(b"stsz") + 8
sample_size, count = unpack_from(">II", data, sizes)
assert count == 1
pack_into(">I", data, sizes if sample_size else sizes + 8, len(audio))
offset = 0
while offset < len(data):
    size, kind = unpack_from(">I4s", data, offset)
    assert size >= 8
    if kind == b"mdat":
        pack_into(">I", data, offset, len(audio) + 8)
        data[offset + 8:offset + size] = audio
        break
    offset += size
else:
    raise ValueError("FFmpeg did not write an mdat box")
path.write_bytes(data)

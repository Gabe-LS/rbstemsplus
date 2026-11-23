#!/usr/bin/env python3
"""Assemble the probe's per-chunk mix dumps into one continuous track per corpus row.

  assemble-dumps.py DIR

DIR is what `pc-session.sh probe fetch` copied: dump/<13-digit ms>-<key>.mix (float32, planar
[2][L], as rekordbox handed the chunk to the model) and rows.txt (the runner's "ROW MS KEY L" for
every KEY line of a load it read back). Writes DIR/tracks/<row>.rb.f32 (headerless little-endian
float32, interleaved stereo: dump-compare.py's default A) and DIR/tracks/<row>.json.

rekordbox's chunking (helper/rbsp.h): chunk i holds track samples [i*HOP - PRE, i*HOP - PRE + L),
zero-padded before the start and after the end, L = 485100, HOP = 483052, PRE = 1024. So
neighbouring chunks share L - HOP = 2048 samples. The chunks are put in order by those shared
samples (exact equality), falling back to the dump times when no chain is unique (silence), and
each overlap's largest difference is recorded. The track is chunk 0 from PRE, then each next
chunk's HOP new samples; the last chunk's trailing zero padding is trimmed (the count is recorded:
a track ending in real digital silence loses that silence too).
"""
import json
import struct
import sys
from array import array
from pathlib import Path

L, HOP, PRE = 485100, 483052, 1024
OV = L - HOP


def load(path):
    a = array("f")
    with open(path, "rb") as f:
        a.frombytes(f.read())
    if sys.byteorder != "little":
        a.byteswap()
    if len(a) != 2 * L:
        raise ValueError(f"{path}: {len(a)} floats, expected 2 x {L}")
    return a[:L], a[L:]


def maxdiff(x, y):
    return max((abs(p - q) for p, q in zip(x, y)), default=0.0)


def order(chunks):
    """chunks: list of (ms, key, (left, right)); the chain by shared samples, else by time"""
    by_time = sorted(chunks)
    def tail(c):
        return tuple(c[2][0][L - OV:]) + tuple(c[2][1][L - OV:])
    def head(c):
        return tuple(c[2][0][:OV]) + tuple(c[2][1][:OV])
    heads = {}
    for c in by_time:
        heads.setdefault(head(c), []).append(c)
    firsts = [c for c in by_time if not any(c[2][0][:PRE]) and not any(c[2][1][:PRE])]
    for first in firsts:
        chain, seen, cur = [first], {first[1]}, first
        while True:
            nxt = [c for c in heads.get(tail(cur), []) if c[1] not in seen]
            if len(nxt) != 1:
                break
            cur = nxt[0]; chain.append(cur); seen.add(cur[1])
        if len(chain) == len(by_time):
            return chain, "shared samples"
    return by_time, "dump time (no unique chain by shared samples)"


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    d = Path(sys.argv[1])
    rows = {}
    for line in (d / "rows.txt").read_text().splitlines():
        p = line.split()
        if len(p) == 4:
            rows.setdefault(p[0], {})[p[2]] = int(p[1])   # repeated loads give the same keys: once each
    dumps = {f.name.split("-", 1)[1][:-4]: f for f in (d / "dump").glob("*.mix")}
    out = d / "tracks"
    out.mkdir(exist_ok=True)
    for row, keys in sorted(rows.items()):
        missing = [k for k in keys if k not in dumps]
        chunks = [(ms, k, load(dumps[k])) for k, ms in keys.items() if k in dumps]
        if not chunks:
            print(f"{row}: no dumps"); continue
        chain, how = order(chunks)
        left, right, diffs = array("f", chain[0][2][0][PRE:PRE + HOP]), array("f", chain[0][2][1][PRE:PRE + HOP]), []
        for prev, cur in zip(chain, chain[1:]):
            diffs.append(max(maxdiff(prev[2][0][L - OV:], cur[2][0][:OV]), maxdiff(prev[2][1][L - OV:], cur[2][1][:OV])))
            left.extend(cur[2][0][PRE:PRE + HOP]); right.extend(cur[2][1][PRE:PRE + HOP])
        # the last chunk's samples past HOP are only in it (no next chunk): kept, then the padding trimmed
        left.extend(chain[-1][2][0][PRE + HOP:]); right.extend(chain[-1][2][1][PRE + HOP:])
        n = len(left)
        while n and left[n - 1] == 0.0 and right[n - 1] == 0.0:
            n -= 1
        trimmed = len(left) - n
        inter = array("f", bytes(8 * n))
        inter[0::2] = left[:n]; inter[1::2] = right[:n]
        if sys.byteorder != "little":
            inter.byteswap()
        (out / f"{row}.rb.f32").write_bytes(inter.tobytes())
        meta = {"row": row, "rate": 44100, "channels": 2, "layout": "interleaved", "format": "float32le",
                "frames": n, "source": "rekordbox for Windows' model input (the probe's mix dumps)",
                "chunk": {"L": L, "hop": HOP, "pre": PRE}, "chunks": [c[1] for c in chain], "order": how,
                "overlap_max_abs_diff": diffs, "trailing_zero_frames_trimmed": trimmed,
                "keys_without_dump": missing}
        (out / f"{row}.json").write_text(json.dumps(meta, indent=1) + "\n")
        print(f"{row}: {len(chain)} chunks ({how}), {n} frames, overlaps max diff {max(diffs, default=0.0)}")


if __name__ == "__main__":
    main()

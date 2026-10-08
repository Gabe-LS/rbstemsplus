#!/usr/bin/env python3
"""Writes a tiny ONNX model shaped like rekordbox's Demucs model, for bridge/tests.

    make_model.py OUT.onnx
    make_model.py --pioneer W0 W1 W2 W3 OUT.onnx

The first: inputs mix [1, 2, L] and mag [1, 1, 1, 1]; outputs x = mag * zeros [1, 4, 1, 1, 1] (all
zero, as with our model, so the bridge caches it) and xt = tile(mix, 4) * 2.475 [1, 8, L].

--pioneer: shaped like rekordbox's own model, whose spectrogram output the bridge rebuilds.
Inputs mix [1, 2, L] and mag [1, 4, 2048, F]; outputs x [1, 4, 4, 2048, F] = mag, for each source
s, times a mask that keeps only bin 0 of the real parts (L.re, R.re) and scales it by Ws (so with
mag's bin 0 at 1, source s's spectrogram is Ws there and 0 elsewhere), and the same xt. Ws may be
"nan". Stdlib only: the protobuf is written by hand (ONNX IR 8, opset 13).
"""
import struct
import sys


def varint(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def field(num, value):
    """value: int -> varint field; bytes/str -> length-delimited field."""
    if isinstance(value, int):
        return varint(num << 3) + varint(value)
    if isinstance(value, str):
        value = value.encode()
    return varint(num << 3 | 2) + varint(len(value)) + value


FLOAT, INT64 = 1, 7


def tensor(name, dims, dtype, raw):
    return b"".join(field(1, d) for d in dims) + field(2, dtype) + field(8, name) + field(9, raw)


def value_info(name, dims):
    """dims: ints, or strings for symbolic dimensions."""
    shape = b"".join(field(1, field(1, d) if isinstance(d, int) else field(2, d)) for d in dims)
    return field(1, name) + field(2, field(1, field(1, FLOAT) + field(2, shape)))


def node(op, inputs, outputs, name):
    return (b"".join(field(1, i) for i in inputs) + b"".join(field(2, o) for o in outputs)
            + field(3, name) + field(4, op))


def zero_model():
    return b"".join([
        field(1, node("Reshape", ["mag", "shape5"], ["mag5"], "reshape")),
        field(1, node("Mul", ["mag5", "zeros"], ["x"], "zero")),
        field(1, node("Tile", ["mix", "reps"], ["mix4"], "tile")),
        field(1, node("Mul", ["mix4", "k"], ["xt"], "scale")),
        field(2, "rbstems_test"),
        field(5, tensor("shape5", [5], INT64, struct.pack("<5q", 1, 1, 1, 1, 1))),
        field(5, tensor("zeros", [1, 4, 1, 1, 1], FLOAT, struct.pack("<4f", 0, 0, 0, 0))),
        field(5, tensor("reps", [3], INT64, struct.pack("<3q", 1, 4, 1))),
        field(5, tensor("k", [], FLOAT, struct.pack("<f", 2.475))),
        field(11, value_info("mix", [1, 2, "L"])),
        field(11, value_info("mag", [1, 1, 1, 1])),
        field(12, value_info("x", [1, 4, 1, 1, 1])),
        field(12, value_info("xt", [1, 8, "L"])),
    ])


def pioneer_model(w):
    mask = []
    for s in range(4):
        for ch in range(4):
            for b in range(2048):
                mask.append(w[s] if b == 0 and ch in (0, 2) else 0.0)
    return b"".join([
        field(1, node("Tile", ["mag", "magreps"], ["mag16"], "tile_mag")),
        field(1, node("Reshape", ["mag16", "shape5"], ["mag5"], "reshape")),
        field(1, node("Mul", ["mag5", "mask"], ["x"], "mask")),
        field(1, node("Tile", ["mix", "reps"], ["mix4"], "tile")),
        field(1, node("Mul", ["mix4", "k"], ["xt"], "scale")),
        field(2, "rbstems_test_pioneer"),
        field(5, tensor("magreps", [4], INT64, struct.pack("<4q", 1, 4, 1, 1))),
        field(5, tensor("shape5", [5], INT64, struct.pack("<5q", 1, 4, 4, 2048, -1))),
        field(5, tensor("mask", [1, 4, 4, 2048, 1], FLOAT, struct.pack("<%df" % len(mask), *mask))),
        field(5, tensor("reps", [3], INT64, struct.pack("<3q", 1, 4, 1))),
        field(5, tensor("k", [], FLOAT, struct.pack("<f", 2.475))),
        field(11, value_info("mix", [1, 2, "L"])),
        field(11, value_info("mag", [1, 4, 2048, "F"])),
        field(12, value_info("x", [1, 4, 4, 2048, "F"])),
        field(12, value_info("xt", [1, 8, "L"])),
    ])


if sys.argv[1] == "--pioneer":
    graph, out = pioneer_model([float(v) for v in sys.argv[2:6]]), sys.argv[6]
else:
    graph, out = zero_model(), sys.argv[1]
model = field(1, 8) + field(2, "rbstems-tests") + field(7, graph) + field(8, field(1, "") + field(2, 13))

with open(out, "wb") as f:
    f.write(model)

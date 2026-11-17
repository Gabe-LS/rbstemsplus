#!/usr/bin/env python3
"""Writes rekordbox's library key to stdout, derived from pyrekordbox's published files exactly
as helper/build.sh does (base85, XOR with BLOB_KEY, zlib; read as literals, nothing of
pyrekordbox's is run). Never print it: pipe it straight into folderpath.
    libkey.py DATABASE_PY UTILS_PY | folderpath DB"""
import ast
import base64
import re
import sys
import zlib


def literal(path, name):
    m = re.search(r'^' + name + r'\s*=\s*(b"[^"]*")', open(path, encoding="utf-8").read(), re.M)
    if not m:
        sys.exit(path + ": no " + name)
    return ast.literal_eval(m.group(1))


blob, k = literal(sys.argv[1], "BLOB"), literal(sys.argv[2], "BLOB_KEY")
data = base64.b85decode(blob)
sys.stdout.buffer.write(zlib.decompress(bytes(b ^ k[i % len(k)] for i, b in enumerate(data))) + b"\n")

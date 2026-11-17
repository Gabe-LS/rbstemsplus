"""The zips unzip.c's tests read (run.sh runs this with python3 -I): one small tree, zipped by
three other tools (Python's zipfile at deflate levels 9 and 1 and stored, Info-ZIP's zip, macOS's
ditto), plus a raw deflate stream and the text it was made from. Deterministic contents (a seeded
random), so the deflate streams cover stored, fixed and dynamic blocks the same way every run.

    python3 -I win/c/tests/gen-zips.py OUT
"""
import os
import random
import shutil
import subprocess
import sys
import zipfile
import zlib

out = os.path.abspath(sys.argv[1])
src = os.path.join(out, "src")
shutil.rmtree(out, ignore_errors=True)
os.makedirs(src)

rng = random.Random(7)
lines = "".join("line %d of the text, with some words repeated: rekordbox stems cache %d\n" % (i, i % 17) for i in range(5000))
files = {
    "empty.txt": b"",
    "hello.txt": b"hello\n",
    "text/lines.txt": lines.encode(),
    "bin/random.bin": bytes(rng.getrandbits(8) for _ in range(100000)),
    "a/b/c/d/deep.txt": b"deep\n" * 100,
    "mixed.bin": lines[:5000].encode() + bytes(rng.getrandbits(8) for _ in range(20000)) + lines[:7000].encode(),
    "RB Stems Plus.exe": b"MZ" + bytes(range(256)) * 40,
}
for name, data in files.items():
    p = os.path.join(src, *name.split("/"))
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "wb") as f:
        f.write(data)
os.makedirs(os.path.join(src, "emptydir"))


def py_zip(name, method, level=None):
    with zipfile.ZipFile(os.path.join(out, name), "w", method, compresslevel=level) as z:
        for d, dirs, names in sorted(os.walk(src)):
            dirs.sort()
            rel = os.path.relpath(d, src)
            if rel != ".":
                z.write(d, rel.replace(os.sep, "/") + "/")
            for n in sorted(names):
                z.write(os.path.join(d, n), (n if rel == "." else rel.replace(os.sep, "/") + "/" + n))


py_zip("python-deflate9.zip", zipfile.ZIP_DEFLATED, 9)
py_zip("python-deflate1.zip", zipfile.ZIP_DEFLATED, 1)
py_zip("python-stored.zip", zipfile.ZIP_STORED)
subprocess.run(["/usr/bin/zip", "-q", "-r", "-X", "-9", os.path.join(out, "infozip.zip")] + sorted(os.listdir(src)), cwd=src, check=True)
subprocess.run(["/usr/bin/ditto", "-c", "-k", "--norsrc", "--noextattr", "--noqtn", "--noacl", src, os.path.join(out, "ditto.zip")], check=True)

# many small entries: the counts and the overlap check on a real zip
with zipfile.ZipFile(os.path.join(out, "many.zip"), "w", zipfile.ZIP_DEFLATED) as z:
    for i in range(1024):
        z.writestr("d/f%d.txt" % i, "%d\n" % i)

raw = lines.encode()[:300000]
assert len(raw) == 300000
c = zlib.compressobj(9, zlib.DEFLATED, -15)
with open(os.path.join(out, "raw.deflate"), "wb") as f:
    f.write(c.compress(raw) + c.flush())
with open(os.path.join(out, "raw.txt"), "wb") as f:
    f.write(raw)
print("zips in %s" % out)

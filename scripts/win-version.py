#!/usr/bin/env python3
"""A Windows PE's version resource, read on the Mac (release.md §3.2's gate: pwsh here can't
read FileVersionInfo). Prints one line per field, sorted:

    fixed.file=X.Y.Z.W      VS_FIXEDFILEINFO's FILEVERSION
    fixed.product=X.Y.Z.W   its PRODUCTVERSION
    fixed.type=N            its FILETYPE (1 an app, 2 a DLL)
    string.<Key>=<value>    each StringFileInfo entry (ProductName, ProductVersion, ...)

or "none" for a file without one. Exit 1 when the file holds more than one (never ours)."""
import struct
import sys


def utf16z(b, i):
    """the NUL-terminated UTF-16LE string at i, and the offset after its NUL"""
    j = i
    while j + 1 < len(b) and b[j:j + 2] != b"\0\0":
        j += 2
    return b[i:j].decode("utf-16-le"), j + 2


def align4(i, base):
    return base + ((i - base + 3) & ~3)


def blocks(b, start, end):
    """the child blocks (wLength, wValueLength, wType, szKey, value, children) between start and end"""
    i = start
    while i + 6 <= end:
        length, vlen, typ = struct.unpack_from("<HHH", b, i)
        if length == 0:
            break
        key, k = utf16z(b, i + 6)
        v = align4(k, i)
        yield i, length, vlen, typ, key, v
        i = align4(i + length, i)


def rsrc(pe):
    """the .rsrc section's raw bytes (b"" when there is none): the code may hold the signature too
    (our programs read version resources)"""
    if pe[:2] != b"MZ":
        sys.exit("not a PE file")
    h = struct.unpack_from("<I", pe, 0x3C)[0]
    if pe[h:h + 4] != b"PE\0\0":
        sys.exit("not a PE file")
    n, = struct.unpack_from("<H", pe, h + 6)
    opt, = struct.unpack_from("<H", pe, h + 20)
    s = h + 24 + opt
    for k in range(n):
        name = pe[s + 40 * k:s + 40 * k + 8].rstrip(b"\0")
        size, off = struct.unpack_from("<II", pe, s + 40 * k + 16)
        if name == b".rsrc":
            return pe[off:off + size]
    return b""


def main(path):
    b = rsrc(open(path, "rb").read())
    sig = struct.pack("<I", 0xFEEF04BD)
    at = [i for i in range(len(b)) if b.startswith(sig, i)]
    if not at:
        print("none")
        return 0
    if len(at) > 1:
        print("more than one VS_FIXEDFILEINFO", file=sys.stderr)
        return 1
    f = at[0]
    (_, _, fms, fls, pms, pls, _, _, _, ftype, _, _, _) = struct.unpack_from("<13I", b, f)
    out = {
        "fixed.file": "%d.%d.%d.%d" % (fms >> 16, fms & 0xFFFF, fls >> 16, fls & 0xFFFF),
        "fixed.product": "%d.%d.%d.%d" % (pms >> 16, pms & 0xFFFF, pls >> 16, pls & 0xFFFF),
        "fixed.type": str(ftype),
    }
    # the VS_VERSIONINFO block holding it: its key, then the fixed info at the next 4-byte boundary
    root = b.rfind("VS_VERSION_INFO".encode("utf-16-le"), 0, f) - 6
    length = struct.unpack_from("<H", b, root)[0]
    children = align4(f + 52, root)
    for bi, ln, _, _, key, v in blocks(b, children, root + length):
        if key != "StringFileInfo":
            continue
        for ti, tl, _, _, _, tv in blocks(b, v, bi + ln):          # the string tables (040904b0, ...)
            for _, _, vlen, _, name, sv in blocks(b, tv, ti + tl):
                text, _ = utf16z(b, sv) if vlen else ("", 0)
                out["string." + name] = text
    for k in sorted(out):
        print("%s=%s" % (k, out[k]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))

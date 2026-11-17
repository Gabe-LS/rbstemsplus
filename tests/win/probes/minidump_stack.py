#!/usr/bin/env python3
"""Phase 0 helper: reads a Windows minidump (WER LocalDumps) and prints the exception, the faulting
thread's RIP/RSP as module+offset, and a heuristic stack: every 8-byte value in the faulting
thread's stack that points into a loaded module's image. No symbols; enough to see which module's
code led to the fault.   minidump_stack.py DUMP [N]"""
import struct
import sys


def main():
    path = sys.argv[1]
    nmax = int(sys.argv[2]) if len(sys.argv) > 2 else 60
    f = open(path, "rb")
    d = f.read()
    sig, ver, nstreams, dir_rva = struct.unpack_from("<4sIII", d, 0)
    assert sig == b"MDMP", sig
    streams = {}
    for i in range(nstreams):
        t, size, rva = struct.unpack_from("<III", d, dir_rva + 12 * i)
        streams.setdefault(t, (size, rva))

    def mstr(rva):
        n = struct.unpack_from("<I", d, rva)[0]
        return d[rva + 4: rva + 4 + n].decode("utf-16-le")

    mods = []
    if 4 in streams:
        _, rva = streams[4]
        n = struct.unpack_from("<I", d, rva)[0]
        for i in range(n):
            o = rva + 4 + 108 * i
            base, size = struct.unpack_from("<QI", d, o)
            name_rva = struct.unpack_from("<I", d, o + 20)[0]
            mods.append((base, size, mstr(name_rva).split("\\")[-1]))

    def where(a):
        for base, size, name in mods:
            if base <= a < base + size:
                return "%s+0x%x" % (name, a - base)
        return None

    mem = []  # (start, size, file offset)
    if 5 in streams:
        _, rva = streams[5]
        n = struct.unpack_from("<I", d, rva)[0]
        for i in range(n):
            start, size, mrva = struct.unpack_from("<QII", d, rva + 4 + 16 * i)
            mem.append((start, size, mrva))
    if 9 in streams:
        _, rva = streams[9]
        n, base_rva = struct.unpack_from("<QQ", d, rva)
        off = base_rva
        for i in range(n):
            start, size = struct.unpack_from("<QQ", d, rva + 16 + 16 * i)
            mem.append((start, size, off))
            off += size

    def read(a, n):
        for start, size, off in mem:
            if start <= a and a + n <= start + size:
                return d[off + a - start: off + a - start + n]
        return None

    tid = None
    if 6 not in streams:
        print("no exception (a dump of a live process)")
    else:
        tid = exception(streams, d, where, read, nmax, mods)
    threads(streams, d, where, read, tid)


def exception(streams, d, where, read, nmax, mods):
    _, rva = streams[6]
    tid = struct.unpack_from("<I", d, rva)[0]
    code, flags, rec, addr, nparams = struct.unpack_from("<IIQQI", d, rva + 8)
    params = struct.unpack_from("<15Q", d, rva + 8 + 32)
    ctx_size, ctx_rva = struct.unpack_from("<II", d, rva + 8 + 152)
    print("exception 0x%08x at %s (0x%x), thread %d, params %s" % (code, where(addr), addr, tid,
          ", ".join("0x%x" % p for p in params[:nparams])))
    print("context: %d bytes" % ctx_size)
    if ctx_size >= 0x100 and ctx_size < 0x800:  # AMD64 CONTEXT
        rsp = struct.unpack_from("<Q", d, ctx_rva + 0x98)[0]
        rip = struct.unpack_from("<Q", d, ctx_rva + 0xF8)[0]
        regs = ["rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"]
        vals = struct.unpack_from("<16Q", d, ctx_rva + 0x78)
        print("rip %s (0x%x) rsp 0x%x" % (where(rip), rip, rsp))
        print("  " + " ".join("%s=%x" % (r, v) for r, v in zip(regs, vals)))
        n = 0
        for k in range(0, 0x4000, 8):
            b = read(rsp + k, 8)
            if b is None:
                break
            v = struct.unpack("<Q", b)[0]
            w = where(v)
            if w and not w.startswith("rekordbox.exe+0x6") and n < nmax:
                print("  [rsp+0x%04x] %s" % (k, w))
                n += 1
    print("modules: %d (ours: %s)" % (len(mods), ", ".join(m[2] for m in mods if m[2].lower() in ("onnxruntime.dll", "demucs3.dll"))))
    return tid


def threads(streams, d, where, read, tid):
    # every thread: its start address (thread info list) and the first module pointers on its stack
    if "--threads" not in sys.argv:
        return
    starts = {}
    if 17 in streams:
        _, rva = streams[17]
        hsize, esize, n = struct.unpack_from("<III", d, rva)
        for i in range(n):
            o = rva + hsize + esize * i
            t = struct.unpack_from("<I", d, o)[0]
            starts[t] = struct.unpack_from("<Q", d, o + 48)[0]
    _, rva = streams[3]
    n = struct.unpack_from("<I", d, rva)[0]
    print("threads: %d" % n)
    for i in range(n):
        t, _, _, _, teb, sstart, ssize, srva, csize, crva = struct.unpack_from("<IIIIQQIIII", d, rva + 4 + 48 * i)
        rsp = struct.unpack_from("<Q", d, crva + 0x98)[0] if csize >= 0x100 else 0
        rip = struct.unpack_from("<Q", d, crva + 0xF8)[0] if csize >= 0x100 else 0
        frames = []
        for k in range(0, 0x3000, 8):
            b = read(rsp + k, 8)
            if b is None:
                break
            w = where(struct.unpack("<Q", b)[0])
            if w and not w.startswith("rekordbox.exe+0x6") and (not frames or frames[-1] != w):
                frames.append(w)
            if len(frames) >= (int(sys.argv[sys.argv.index("--depth") + 1]) if "--depth" in sys.argv else 8):
                break
        print("  thread %d%s start %s rip %s | %s" % (t, " (FAULT)" if t == tid else "", where(starts.get(t, 0)),
                                                       where(rip), " < ".join(frames)))


main()

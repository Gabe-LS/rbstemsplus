#!/usr/bin/env python3
"""The Mac files' input lists (release.md §1.3, R4.I91): which repository paths reach the bytes.

The builds write the lists themselves (scripts/build-app.sh, helper/build.sh, bridge/build.sh):
the clang depfiles of every compile, plus the files and folders they read. A line is a path
from the top of the repository; a line ending in "/" is a folder, every path under it counts
(a source folder the build takes with a glob, or a folder a tool reads whole). Paths outside
the repository (the SDK, the toolchain) and under build/ (downloads and generated files, whose
pins are in the build scripts, which are on the lists) are left out.

  mac-inputs.py deps [--cwd DIR] ROOT DEPFILE...
                                       the repository paths in clang depfiles (make syntax),
                                       one per line, sorted; exit 2 when a depfile names a
                                       relative path, unless --cwd gives the folder the compiles
                                       ran in (relative paths are then taken from there), or a
                                       path with a newline (a list holds one path per line)
  mac-inputs.py changed LIST...        of the changed paths on stdin (git diff -z --name-only:
                                       NUL-separated), prints those on any LIST, one per line (a
                                       name with a newline escaped); exit 1 when one is, 0 when
                                       none is, 2 on an error (a missing or empty list)

Run with python3 -I.
"""
import os
import sys


def rule_paths(text):
    """The names in one joined make rule line, unescaped ("\\ " is a space, "$$" a "$")."""
    out, cur, i = [], "", 0
    while i < len(text):
        c = text[i]
        if c == "\\" and i + 1 < len(text) and text[i + 1] in " #":
            cur += text[i + 1]
            i += 2
        elif c == "$" and i + 1 < len(text) and text[i + 1] == "$":
            cur += "$"
            i += 2
        elif c in " \t":
            if cur:
                out.append(cur)
            cur = ""
            i += 1
        else:
            cur += c
            i += 1
    if cur:
        out.append(cur)
    return out


def deps(root, files, cwd=None):
    root = os.path.realpath(root)
    found = set()
    for f in files:
        with open(f, encoding="utf-8", errors="surrogateescape") as fh:
            text = fh.read()
        for line in text.replace("\\\n", " ").split("\n"):
            # the rule's target (before its first unescaped ": ") is the object, not an input
            j = line.find(": ")
            while j > 0 and line[j - 1] == "\\":
                j = line.find(": ", j + 1)
            if j < 0:
                if not line.strip():
                    continue
                # every line of a clang depfile is a rule: anything else is a path cut by a
                # newline in its name, which a list of one path per line can't hold
                print("mac-inputs.py: %s has a line that isn't a rule (%r): a file name with a"
                      " newline?" % (f, line), file=sys.stderr)
                return 2
            for p in rule_paths(line[j + 2:]):
                if "\n" in p or "\r" in p:
                    print("mac-inputs.py: %s names %r, with a newline: the input lists hold one"
                          " path per line" % (f, p), file=sys.stderr)
                    return 2
                # a relative path is relative to the compile's folder, not to this script's: it
                # is refused unless that folder is given, never guessed (a wrong guess drops
                # inputs from the list without a sound)
                if not os.path.isabs(p):
                    if cwd is None:
                        print("mac-inputs.py: %s names %r, a relative path: give the compile's folder"
                              " (--cwd DIR) or compile with absolute paths" % (f, p), file=sys.stderr)
                        return 2
                    p = os.path.join(cwd, p)
                rp = os.path.realpath(p)
                if rp.startswith(root + os.sep):
                    rel = rp[len(root) + 1:]
                    if rel != "build" and not rel.startswith("build" + os.sep):
                        found.add(rel)
    for p in sorted(found):
        out(p)
    return 0


def out(p):
    """One path per line, its bytes as they came (git and the file system: not re-encoded)."""
    sys.stdout.buffer.write(p.encode("utf-8", "surrogateescape") + b"\n")


def changed(lists):
    entries = set()
    for lf in lists:
        try:
            with open(lf, encoding="utf-8", errors="surrogateescape") as fh:
                got = [l.rstrip("\n") for l in fh]
            got = [l for l in got if l.strip() and not l.startswith("#")]
        except OSError as e:
            print("mac-inputs.py: %s" % e, file=sys.stderr)
            return 2
        if not got:
            print("mac-inputs.py: %s lists nothing" % lf, file=sys.stderr)
            return 2
        entries.update(got)
    folders = tuple(e for e in entries if e.endswith("/"))
    hit = False
    # git diff -z: NUL-separated and never quoted (git's line output quotes a non-ASCII name,
    # "app/Sources/Caf\303\251.swift", and would match no entry)
    sys.stdout.flush()
    for b in sys.stdin.buffer.read().split(b"\0"):
        if not b:
            continue
        p = b.decode("utf-8", "surrogateescape")
        if p in entries or p.startswith(folders):
            # a name with a newline is still a hit: written escaped, on one line
            out(repr(p) if "\n" in p or "\r" in p else p)
            hit = True
    return 1 if hit else 0


def main(argv):
    if len(argv) >= 5 and argv[1] == "deps" and argv[2] == "--cwd":
        if not os.path.isdir(argv[3]):
            print("mac-inputs.py: --cwd %s isn't a folder" % argv[3], file=sys.stderr)
            return 2
        return deps(argv[4], argv[5:], os.path.abspath(argv[3]))
    if len(argv) >= 3 and argv[1] == "deps" and argv[2] != "--cwd":
        return deps(argv[2], argv[3:])
    if len(argv) >= 3 and argv[1] == "changed":
        return changed(argv[2:])
    print(__doc__.strip().split("\n\n")[1], file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))

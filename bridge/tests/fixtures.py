"""The shared config.ini fixtures (tests/config-fixtures.txt), for run.sh.

  fixtures.py list FILE              name TAB expected, one per line (expected's dir decoded)
  fixtures.py write FILE NAME OUT    the fixture's content, as bytes, into OUT
"""
import re
import sys


def decode(s):
    out, i = bytearray(), 0
    while i < len(s):
        c = s[i]
        if c != "\\":
            out += c.encode()
            i += 1
            continue
        n = s[i + 1]
        if n in "nrt\\":
            out += {"n": b"\n", "r": b"\r", "t": b"\t", "\\": b"\\"}[n]
            i += 2
        elif n == "x":
            out.append(int(s[i + 2:i + 4], 16))
            i += 4
        elif n == "*":
            m = re.match(r"\\\*(\d+)\.", s[i:])
            ch = s[i + m.end()]
            out += ch.encode() * int(m.group(1))
            i += m.end() + 1
        else:
            raise SystemExit("bad escape in %r" % s)
    return bytes(out)


def fixtures(path):
    for line in open(path, encoding="utf-8").read().split("\n"):
        if not line or line.startswith("#"):
            continue
        name, expected, content = (line.split("\t", 2) + [""])[:3]   # no content: an empty file
        yield name, expected, content


def main():
    cmd, path = sys.argv[1], sys.argv[2]
    if cmd == "list":
        for name, expected, _ in fixtures(path):
            words = []
            for w in expected.split():
                if w.startswith("dir="):
                    w = "dir=" + decode(w[4:]).decode().replace("%20", " ")
                words.append(w.replace(" ", "%20"))
            print(name + "\t" + " ".join(words))
    elif cmd == "write":
        for name, _, content in fixtures(path):
            if name == sys.argv[3]:
                open(sys.argv[4], "wb").write(decode(content))
                return
        raise SystemExit("no fixture " + sys.argv[3])


main()

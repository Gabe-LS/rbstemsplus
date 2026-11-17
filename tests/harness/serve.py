#!/usr/bin/env python3
"""The TEST builds' local download server on the host (tests/harness/vm.sh), stdlib only.

  serve.py DIR [--bind 192.168.64.1] [--port 8765] [--rate KB_PER_S]

Serves DIR as python3 -m http.server does (links followed); with --rate, every response body is
sent at that many KB per second, so a scenario can act during a download (quit-during-download).
"""
import functools
import http.server
import sys
import time


class Throttled(http.server.SimpleHTTPRequestHandler):
    rate = 0  # bytes per second, 0: as fast as it goes

    def copyfile(self, source, outputfile):
        if not self.rate:
            return super().copyfile(source, outputfile)
        chunk = max(1024, self.rate // 20)
        while True:
            buf = source.read(chunk)
            if not buf:
                return
            outputfile.write(buf)
            time.sleep(len(buf) / self.rate)

    def log_message(self, fmt, *args):
        sys.stderr.write("%s %s\n" % (time.strftime("%H:%M:%S"), fmt % args))


def main(a):
    if len(a) < 2:
        print(__doc__)
        return 2
    d, bind, port, rate = a[1], "192.168.64.1", 8765, 0
    i = 2
    while i < len(a):
        if a[i] == "--bind":
            bind = a[i + 1]
        elif a[i] == "--port":
            port = int(a[i + 1])
        elif a[i] == "--rate":
            rate = int(a[i + 1]) * 1024
        else:
            print(__doc__)
            return 2
        i += 2
    Throttled.rate = rate
    handler = functools.partial(Throttled, directory=d)
    http.server.ThreadingHTTPServer((bind, port), handler).serve_forever()


if __name__ == "__main__":
    sys.exit(main(sys.argv))

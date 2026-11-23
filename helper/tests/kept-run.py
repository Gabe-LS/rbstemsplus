"""Run the helper with the same stdin lifetime as the Windows app."""

import json
import os
from pathlib import Path
import subprocess
import sys
from threading import Event, Timer


def run(chunk, argv):
    # The helper writes progress before each chunk. Close just the writing end of
    # the pipe at the requested milestone and keep draining its final records.
    stopped = False
    carriage_return = False
    timed_out = Event()
    with subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE) as child:
        def timeout():
            timed_out.set()
            child.kill()

        watchdog = Timer(600, timeout)
        watchdog.start()
        try:
            for line in child.stdout:
                carriage_return |= b"\r" in line
                sys.stdout.buffer.write(line)
                if chunk >= 0 and not stopped:
                    try:
                        progress = json.loads(line)
                    except ValueError:
                        continue
                    if isinstance(progress, dict) and progress.get("chunk") == chunk:
                        child.stdin.close()
                        stopped = True
            rc = child.wait()
        finally:
            watchdog.cancel()

    errors = []
    if carriage_return:
        errors.append("helper stdout contains CR")
    if chunk >= 0 and not stopped:
        errors.append(f"helper never reached chunk {chunk}")
    if timed_out.is_set():
        errors.append("helper did not exit within 600 seconds")
    if errors:
        message = "\n".join(errors)
        print(message, file=sys.stderr)
        if marker := os.environ.get("RBSP_TEST_RUN_ERRORS"):
            with Path(marker).open("a", encoding="utf-8") as log:
                print(message, file=log)
        return 125
    return rc if rc >= 0 else 128 - rc


if __name__ == "__main__":
    sys.exit(run(int(sys.argv[1]), sys.argv[2:]))

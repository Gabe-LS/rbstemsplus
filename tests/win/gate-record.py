"""Check Windows release evidence and record the owner's VR2 attestation."""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
PC_ROUND = "worker (release.list's worker row, on the real PC: OD2)"
PC_STEPS = (
    "session", "stage", "preflight", "cache-install", "T2", "hits",
    "directml", "cache-uninstall", "collect", "cleanup",
)
# The owner's ruling for a PC whose rekordbox runs stems on the CPU (pc-session.sh auto record
# --cpu-stems-ok): this line, just before the log line, lets directml be 3; every other step stays 0.
PC_CPU_LINE = "directml cpu, accepted by --cpu-stems-ok"


def check(path, commit, payload_sha):
    # Open once so the digest covers the same bytes that passed validation.
    with os.fdopen(os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK), "rb") as record:
        if not stat.S_ISREG(os.fstat(record.fileno()).st_mode):
            raise ValueError("expected a regular record file")
        data = record.read()

    pc = path.name == "pc-win.ok"
    lines = data.decode("ascii").split("\n")
    cpu = pc and len(lines) == len(PC_STEPS) + 8 and lines[-4] == PC_CPU_LINE
    if len(lines) != ((len(PC_STEPS) + (8 if cpu else 7)) if pc else 6) or lines[-1]:
        raise ValueError("invalid record format")
    if not re.fullmatch(r"commit [0-9a-f]{40}", lines[0]):
        raise ValueError("invalid commit line")
    if lines[0] != f"commit {commit}":
        raise ValueError(f"record commit does not match {commit}")
    if not re.fullmatch(r"payload_sha256 [0-9a-f]{64}", lines[1]):
        raise ValueError("invalid payload_sha256 line")
    if lines[1] != f"payload_sha256 {payload_sha}":
        raise ValueError(f"record TEST payload SHA-256 does not match {payload_sha}")
    if lines[2] != f"round {PC_ROUND if pc else 'VR2'}":
        raise ValueError("invalid round line")
    if not re.fullmatch(r"date [0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z", lines[3]):
        raise ValueError("invalid UTC date line")
    datetime.strptime(lines[3], "date %Y-%m-%dT%H:%M:%SZ")
    if pc:
        for name, line in zip(PC_STEPS, lines[4:-4] if cpu else lines[4:-3]):
            code = 3 if cpu and name == "directml" else 0
            if line != f"step {name} {code}":
                raise ValueError(f"required check {name} did not pass (expected 'step {name} {code}')")
        if not re.fullmatch(r"log [A-Za-z0-9][A-Za-z0-9_.-]*\.log", lines[-3]):
            raise ValueError("invalid log line")
    if lines[-2] != "result pass":
        raise ValueError("expected 'result pass'")
    print(hashlib.sha256(data).hexdigest())


def write_by_hand(commit, dist):
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("expected a full commit ID")
    kind = subprocess.run(
        ["git", "-C", str(ROOT), "cat-file", "-t", commit],
        capture_output=True, text=True, check=False,
    )
    if kind.stdout != "commit\n":
        raise ValueError(f"{commit} is not a commit in this checkout")
    payload = dist / "payload.json"
    data = payload.read_bytes()
    document = json.loads(data)
    if not isinstance(document, dict) or document.get("commit") != commit:
        raise ValueError(f"{payload} does not name commit {commit}")

    path = ROOT / "build" / "gates" / commit / "win-by-hand.ok"
    path.parent.mkdir(parents=True, exist_ok=True)
    date = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    with tempfile.NamedTemporaryFile(
        mode="w", encoding="ascii", prefix=".win-by-hand.", dir=path.parent, delete=False,
    ) as record:
        temp = Path(record.name)
        try:
            record.write(
                f"commit {commit}\npayload_sha256 {hashlib.sha256(data).hexdigest()}\n"
                f"round VR2\ndate {date}\nresult pass\n"
            )
            record.close()
            temp.replace(path)
        finally:
            temp.unlink(missing_ok=True)
    print(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    reader = commands.add_parser("check")
    reader.add_argument("record", type=Path)
    reader.add_argument("commit")
    reader.add_argument("payload_sha256")
    writer = commands.add_parser("write-by-hand")
    writer.add_argument("commit")
    writer.add_argument("dist", type=Path)
    writer.add_argument("--pass", required=True, action="store_true",
                        help="attest that the by-hand round passed on this TEST build")
    args = parser.parse_args()
    try:
        if args.command == "check":
            if args.record.name not in {"pc-win.ok", "win-by-hand.ok"}:
                raise ValueError("unknown record type")
            check(args.record, args.commit, args.payload_sha256)
        else:
            write_by_hand(args.commit, args.dist)
    except (OSError, ValueError) as error:
        label = str(args.record) if args.command == "check" else "VR2 writer"
        reason = "missing record" if args.command == "check" and isinstance(error, FileNotFoundError) else str(error)
        print(f"{label}: {reason}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

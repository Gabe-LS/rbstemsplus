#!/usr/bin/env python3
"""A release's performance record (release.md §6, step 4b; §6.2; T27). Stdlib only.

    record.py write PLATFORM COMMIT PAYLOAD_JSON OUT COMPARE_TSV...
        OUT (build/gates/<commit>/perf-<platform>.tsv) from the compare tables of the plan's
        sessions on the TEST build of COMMIT (tests/perf/stats.py compare --tsv, the bridge's and
        the helper's tests/perf/perfstats.py gate --out, the Windows runner's), bound to COMMIT
        and to PAYLOAD_JSON's SHA-256 (dist-test/payload.json, the TEST build the sessions ran on)
    record.py check RECORD PLATFORM COMMIT PAYLOAD_SHA256
        what scripts/release.sh reads: exit 0 a pass, 10 a pass with BORDERLINE rows (a human
        judges: release.sh --accept-perf REASON), 1 no usable pass (missing, another commit or
        TEST payload or platform, a row failed, a session's measurement failed, malformed)

The record, one tab-separated line each:
    perf            1
    platform        mac | win
    commit          the commit, 40 hex digits
    payload_sha256  the TEST build's payload.json SHA-256
    source          NAME SHA256 VERDICT    each compare table read: pass, or fail when any of its
                                           "# compare:" summary lines says anything but pass
    row             METRIC A B CHANGE ALLOWED VERDICT NOTE
                    VERDICT: pass, borderline, fail (a regression, missing samples, a failed
                    measurement), info, notrun; NOTE: the compare's own words
    result          pass | borderline | fail

A compare table needs a "# compare:" summary line (stats.py compare --tsv and perfstats.py gate
--out write one first; the bridge's and the helper's perf.sh append "# compare: information only
(...)" to a run that is no gate): a table without one isn't a gate session's, and is refused.
The result is the worst of the rows and the sources: any fail is a fail, else any borderline is
a borderline. check recomputes it and refuses a record whose result line says otherwise, one
with no source line, one whose row verdict doesn't match its note, and one with no gating row
(pass, borderline or fail). Which rows the plan needs (§6.2's matrix) is the
sessions' choice: compare.sh's matrix check isn't built (release.md §6.2).
"""
import hashlib
import os
import re
import sys

KEYS = ("perf", "platform", "commit", "payload_sha256", "result")
PLATFORMS = ("mac", "win")
VERDICTS = ("pass", "borderline", "fail", "info", "notrun")
GATING = ("pass", "borderline", "fail")


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def row_verdict(text):
    """a compare table's verdict cell, normalized; matched exactly (the producers' own words)"""
    t = text.strip()
    if t == "pass":
        return "pass"
    if re.fullmatch(r"pass BORDERLINE( .*)?", t):
        return "borderline"
    if t == "info":
        return "info"
    if re.fullmatch(r"not run( .*)?", t):
        return "notrun"
    return "fail"     # REGRESSION[ BORDERLINE...], NO SAMPLES, FAIL..., and words this reader doesn't know


def worst(verdicts):
    v = set(verdicts)
    return "fail" if "fail" in v else "borderline" if "borderline" in v else "pass"


def clean(s):
    return re.sub(r"[\t\r\n]", " ", s)


def read_compare(path):
    """(overall verdict, [(metric, A, B, change, allowed, verdict, note)]): the overall is the
    worst of the table's "# compare:" lines (a later pass never overrides a failure)"""
    overall, rows, col = None, [], None
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line.strip():
                continue
            if line.startswith("#"):
                m = re.match(r"#\s*compare:\s*(.*)$", line)
                if m:
                    v = "pass" if m.group(1).strip() == "pass" else "fail"
                    overall = v if overall in (None, "pass") else overall
                continue
            cells = line.split("\t")
            if cells[0] == "metric":
                names = [c.strip() for c in cells]
                col = names.index("verdict") if "verdict" in names else names.index("result") if "result" in names else None
                if col is None:
                    raise ValueError(f"{path}: its header has no verdict or result column")
                continue
            if col is None:
                raise ValueError(f"{path}: a row before the header (metric ... verdict|result)")
            if len(cells) <= col:
                raise ValueError(f"{path}: a row with no verdict: {line}")
            pad = (cells + ["-"] * 5)[:5]
            rows.append((pad[0], pad[1] or "-", pad[2] or "-", pad[3] or "-", pad[4] or "-",
                         row_verdict(cells[col]), clean(cells[col].strip())))
    if col is None:
        raise ValueError(f"{path}: no compare table (no header line metric ... verdict|result)")
    if overall is None:
        raise ValueError(f"{path}: no '# compare:' summary line: not a gate session's table")
    return overall, rows


def cmd_write(platform, commit, payload, out, tables):
    if platform not in PLATFORMS:
        sys.exit(f"record.py write: the platform is mac or win, not {platform}")
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        sys.exit("record.py write: COMMIT isn't 40 hex digits")
    if not tables:
        sys.exit("record.py write: no compare table given")
    lines = ["perf\t1", f"platform\t{platform}", f"commit\t{commit}", f"payload_sha256\t{sha256(payload)}"]
    verdicts, rowv = [], []
    for t in tables:
        try:
            overall, rows = read_compare(t)
        except (OSError, ValueError) as e:
            sys.exit(f"record.py write: {e}")
        lines.append(f"source\t{clean(os.path.basename(t))}\t{sha256(t)}\t{overall}")
        verdicts.append(overall)
        for r in rows:
            lines.append("row\t" + "\t".join(clean(x) for x in r))
            rowv.append(r[5])
    # check's rule: a gating row (a summary line's verdict alone isn't one)
    if not any(v in GATING for v in rowv):
        sys.exit("record.py write: no gating row in the tables given")
    verdicts += rowv
    result = worst(v for v in verdicts if v in GATING)
    lines.append(f"result\t{result}")
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    tmp = out + ".part"
    with open(tmp, "w", encoding="utf-8") as o:
        o.write("".join(l + "\n" for l in lines))
    os.replace(tmp, out)
    print(f"record.py: {out}: {result}")
    return 0


def cmd_check(record, platform, commit, payload_sha):
    name = os.path.basename(record)

    def refuse(why):
        print(f"record.py check: {name}: {why}")
        return 1

    if not os.path.isfile(record) or os.path.islink(record):
        return refuse(f"no performance record ({record}): run the plan's sessions on the TEST build "
                      f"(release.md §6.2), then tests/perf/record.py write {platform} ...")
    seen, rows, sources = {}, [], []
    try:
        with open(record, encoding="utf-8") as f:
            text = f.read()
    except (OSError, UnicodeDecodeError) as e:
        return refuse(f"can't be read: {e}")
    for line in text.splitlines():
        if not line.strip():
            continue
        cells = line.split("\t")
        k = cells[0]
        if k in KEYS:
            if len(cells) != 2 or k in seen:
                return refuse(f"malformed: the line '{clean(line)}'")
            seen[k] = cells[1]
        elif k == "source":
            if len(cells) != 4 or cells[3] not in ("pass", "fail"):
                return refuse(f"malformed: the line '{clean(line)}'")
            sources.append(cells[3])
        elif k == "row":
            if len(cells) != 8 or cells[6] not in VERDICTS:
                return refuse(f"malformed: the line '{clean(line)}'")
            if row_verdict(cells[7]) != cells[6]:
                return refuse(f"malformed: the row {cells[1]} says {cells[6]}, its note '{cells[7]}' says {row_verdict(cells[7])}")
            rows.append(cells[1:])
        else:
            return refuse(f"malformed: the line '{clean(line)}'")
    missing = [k for k in KEYS if k not in seen]
    if missing:
        return refuse(f"malformed: no {', '.join(missing)} line")
    if seen["perf"] != "1":
        return refuse(f"a record of format {seen['perf']}, not 1")
    if seen["platform"] != platform:
        return refuse(f"a record of platform {seen['platform']}, not {platform}")
    if seen["commit"] != commit:
        return refuse(f"for commit {seen['commit']}, not {commit}: run the sessions on this commit's TEST build")
    if seen["payload_sha256"] != payload_sha:
        return refuse(f"for the TEST payload {seen['payload_sha256'][:12]}, not dist-test/payload.json "
                      f"({payload_sha[:12]}): the sessions ran on another TEST build")
    if not sources:
        return refuse("malformed: no source line (the compare tables it was written from)")
    gating = [r[5] for r in rows if r[5] in GATING]
    if not gating:
        return refuse("no gating row")
    result = worst(gating + sources)
    if seen["result"] != result:
        return refuse(f"its result line says {seen['result']}, its rows and sources {result}")
    failed = [r[0] for r in rows if r[5] == "fail"]
    border = [r for r in rows if r[5] == "borderline"]
    if result == "fail":
        what = f"FAIL: {', '.join(failed)}" if failed else "FAIL: a session's measurement failed (its source line)"
        return refuse(what + " (a FAIL is never accepted: fix it, then run the sessions again)")
    for r in border:
        print(f"record.py check: {name}: BORDERLINE {r[0]}: {r[1]} -> {r[2]} ({r[3]} against {r[4]} allowed: within 25% of the allowance)")
    if border:
        print(f"record.py check: {name}: borderline ({len(border)} row{'s' if len(border) != 1 else ''}), {len(gating)} gating rows")
        return 10
    print(f"record.py check: {name}: pass ({len(gating)} gating rows)")
    return 0


def main(argv):
    if len(argv) >= 6 and argv[1] == "write":
        return cmd_write(argv[2], argv[3], argv[4], argv[5], argv[6:])
    if len(argv) == 6 and argv[1] == "check":
        return cmd_check(*argv[2:])
    print(__doc__.strip().split("\n\n")[1], file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))

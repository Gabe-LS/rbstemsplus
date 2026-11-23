#!/usr/bin/env python3
"""The statistics of the performance gate GP (bridge/tests/perf.sh, helper/tests/perf.sh).
Stdlib only; run with python3 -I.

    perfstats.py summary SAMPLES.tsv
        per metric and side: n, median, p95, p99, max
    perfstats.py gate SAMPLES.tsv --a SIDE --b SIDE --rules FILE [--baseline B0] [--prefix P-B]
                 [--require M,M,...] [--out REPORT.tsv]
        A against B under tests/perf/thresholds.txt's rows for the baseline: each row's median
        over all of a side's samples (its run sets pooled), under the rule of tests/perf/rules.py
        (owner decided, 2026-11-12: a slowdown counts only when it is over both 10% and the
        floor, 20 ms or 20 MB); every other metric is printed for information and never blocks;
        a row within 25% of its allowance is printed BORDERLINE for a human to judge. Exit 0 pass;
        1 a regression (each regressed row named); 3 the measurement failed: a gating or
        --require metric missing on a side, a bad sample line or a sample that isn't a finite
        number, a rules file that can't be read. 2: usage. REPORT.tsv (removed first) starts
        with the summary line tests/perf/record.py reads: "# compare: pass", "# compare: REGRESSION: ..." or
        "# compare: MEASUREMENT FAILED: ..." (the only line when the samples or rules are bad).
    perfstats.py selftest
        the rule on made-up samples

SAMPLES.tsv: one sample per line, "metric<TAB>side<TAB>round<TAB>value" (times in ns, sizes in
bytes); lines starting with # are comments. A row's metric may end in * (a prefix rule: the most
specific prefix applies).
"""
import math
import os
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)            # python3 -I drops the script's folder from the path
import rules as rulesmod              # noqa: E402  the one parser of thresholds.txt and the one rule


class SampleError(SystemExit):
    def __init__(self, msg):
        super().__init__(msg)
        self.code = 3


def percentile(xs, p):
    """linear between closest ranks (numpy's default)"""
    s = sorted(xs)
    if not s:
        return float("nan")
    k = (len(s) - 1) * p / 100.0
    lo, hi = math.floor(k), math.ceil(k)
    return s[lo] + (s[hi] - s[lo]) * (k - lo)


def read_samples(path):
    data = {}  # (metric, side) -> [values]
    for line in open(path, encoding="utf-8"):
        if not line.strip() or line.startswith("#"):
            continue
        parts = line.rstrip("\n").split("\t")
        if len(parts) < 4:
            sys.exit("%s: bad line: %r" % (path, line))
        try:
            x = float(parts[3])
        except ValueError:
            x = float("nan")
        if not math.isfinite(x):       # nan or inf would compare as a pass: the measurement failed
            msg = "%s: a non-finite sample: %r" % (path, line.rstrip("\n"))
            print("FAIL measurement: %s" % msg)
            raise SampleError(msg)
        data.setdefault((parts[0], parts[1]), []).append(x)
    return data


def rules_for(rules, metric):
    exact = [r for r in rules if r["metric"] == metric]
    if exact:
        return exact[0]
    best = [r for r in rules if r["metric"].endswith("*") and metric.startswith(r["metric"][:-1])]
    return max(best, key=lambda r: len(r["metric"])) if best else None


def human(v, metric):
    if any(k in metric for k in (".rss", ".size")):
        return "%.2f MB" % (v / 2 ** 20) if abs(v) >= 2 ** 20 else "%.1f KB" % (v / 1024)
    return "%.3f ms" % (v / 1e6)


def summary(path):
    data = read_samples(path)
    print("%-28s %-6s %5s %12s %12s %12s %12s" % ("metric", "side", "n", "median", "p95", "p99", "max"))
    for (metric, side) in sorted(data):
        xs = data[(metric, side)]
        print("%-28s %-6s %5d %12s %12s %12s %12s" % (metric, side, len(xs), human(percentile(xs, 50), metric),
              human(percentile(xs, 95), metric), human(percentile(xs, 99), metric), human(max(xs), metric)))


def parse_opts(args, opts):
    """--name VALUE pairs into opts; usage on anything else"""
    i = 0
    while i < len(args):
        if args[i] not in opts or i + 1 >= len(args):
            print(__doc__, file=sys.stderr)
            sys.exit(2)
        opts[args[i]] = args[i + 1]
        i += 2
    return opts


def gate(args):
    path = args[0]
    out = None
    for opt, value in zip(args[1::2], args[2::2]):
        if opt == "--out":
            out = value
    if out and os.path.lexists(out):
        os.remove(out)                 # a gate that stops early never leaves an earlier table behind
    opts = parse_opts(args[1:], {"--a": None, "--b": None, "--rules": None, "--baseline": "B0", "--prefix": "",
                                 "--out": None, "--require": ""})
    a, b = opts["--a"], opts["--b"]
    if not a or not b or not opts["--rules"]:
        print(__doc__, file=sys.stderr)
        sys.exit(2)

    def fail_out(msg):
        if out:
            with open(out, "w", encoding="utf-8") as o:
                o.write("# compare: MEASUREMENT FAILED: %s\n" % str(msg).replace("\n", " "))

    try:
        data = read_samples(path)
    except (SystemExit, OSError, UnicodeDecodeError) as e:
        fail_out(e)
        if not isinstance(e, SystemExit) or e.code != 3:
            print(e, file=sys.stderr)
        return 3
    try:
        rules = rulesmod.load(opts["--rules"], opts["--baseline"])[0]
    except rulesmod.RuleError as e:
        print("FAIL measurement: %s" % e)
        fail_out(e)
        return 3
    required = {m for m in opts["--require"].split(",") if m}
    metrics = sorted({m for (m, s) in data if s in (a, b) and m.startswith(opts["--prefix"])} | required)
    rows, regressed, failed, borders = [], [], [], []
    for metric in metrics:
        xa, xb = data.get((metric, a), []), data.get((metric, b), [])
        rule = rules_for(rules, metric)
        if not xa or not xb:
            if rule or metric in required:
                failed.append(metric)
                rows.append((metric, "", "", "", "", "FAIL measurement: missing on %s" % ("both sides" if not xa and not xb else a if not xa else b)))
            continue
        va, vb = statistics.median(xa), statistics.median(xb)
        if not rule:
            rows.append((metric, human(va, metric), human(vb, metric), human(vb - va, metric), "-", "info"))
            continue
        change, allowed, over, border = rulesmod.judge(va, vb, rule["value"], rule["floor"])
        status = "REGRESSION" if over else "pass"
        if over:
            regressed.append(metric)
        if border:
            status += " BORDERLINE (within 25% of the allowance: a human judges)"
            borders.append(metric)
        rows.append((metric, human(va, metric), human(vb, metric), human(change, metric), human(allowed, metric), status))
    fmt = "%-28s %13s %13s %13s %13s  %s"
    print(fmt % ("metric", a, b, "change", "allowed", "result"))
    for r in rows:
        print(fmt % r)
    if out:
        # the summary line first (record.py reads it): the gate's own verdict
        verdict = "MEASUREMENT FAILED: %s" % ", ".join(failed) if failed else \
            "REGRESSION: %s" % ", ".join(regressed) if regressed else "pass"
        with open(out, "w", encoding="utf-8") as o:
            o.write("# compare: %s\n" % verdict)
            o.write("metric\t%s\t%s\tchange\tallowed\tresult\n" % (a, b))
            for r in rows:
                o.write("\t".join(r) + "\n")
    if borders:
        print("GP: borderline, for a human to judge: %s" % ", ".join(borders))
    if failed:
        print("GP: measurement failed: %s" % ", ".join(failed))
        return 3
    if regressed:
        print("GP: REGRESSION: %s" % ", ".join(regressed))
        return 1
    print("GP: pass")
    return 0


def selftest():
    import contextlib
    import io
    import tempfile
    d = tempfile.mkdtemp()
    rules = os.path.join(d, "rules")
    with open(rules, "w") as f:
        f.write("X.lat median 10% 20ms first B0\nX.rss median 10% 20MB first B0\nX.* median 10% 20ms first B0\n"
                "X.lat median 10% 0 first mac-A\n")
    ms, mb = 1e6, 2 ** 20

    def run(rows, extra=(), rulefile=None):
        p = os.path.join(d, "s.tsv")
        with open(p, "w") as f:
            for m, side, vals in rows:
                for v in vals:
                    f.write("%s\t%s\tp1\t%s\n" % (m, side, v))
        with contextlib.redirect_stdout(io.StringIO()) as out:
            try:
                rc = gate([p, "--a", "A", "--b", "B", "--rules", rulefile or rules] + list(extra))
            except SystemExit as e:
                rc = e.code
        return rc, out.getvalue()
    ok = True

    def expect(cond, what):
        nonlocal ok
        print(("ok   " if cond else "FAIL ") + what)
        ok &= bool(cond)
    lat = lambda a, b: [("X.lat", "A", [a * ms] * 4), ("X.lat", "B", [b * ms] * 4)]
    rc, o = run(lat(100, 100))
    expect(rc == 0 and "GP: pass" in o, "the same numbers pass")
    rc, o = run(lat(100, 130))
    expect(rc == 1 and "GP: REGRESSION: X.lat" in o, "+30 ms on 100 ms (over 10% and over 20 ms): a regression, named")
    rc, o = run(lat(100, 112))
    expect(rc == 0 and "BORDERLINE" not in o, "+12 ms on 100 ms: over 10% but under the 20 ms floor, a pass")
    rc, o = run(lat(1000, 1090))
    expect(rc == 0 and "BORDERLINE" in o, "+90 ms on 1 s: over 20 ms but under 10%, a pass, BORDERLINE (75-125% of the 100 ms allowed)")
    rc, o = run(lat(1000, 1110))
    expect(rc == 1 and "BORDERLINE" in o, "+110 ms on 1 s: over both, a regression, BORDERLINE")
    rc, o = run(lat(1000, 1200))
    expect(rc == 1 and "BORDERLINE" not in o, "+200 ms on 1 s: a clear regression")
    rc, o = run(lat(0.5, 5))
    expect(rc == 0, "a sub-millisecond time 10x slower: under the 20 ms floor, a pass")
    rc, o = run([("X.rss", "A", [100 * mb]), ("X.rss", "B", [125 * mb])])
    expect(rc == 1, "+25 MB on 100 MB: a memory regression")
    rc, o = run([("X.rss", "A", [100 * mb]), ("X.rss", "B", [115 * mb])])
    expect(rc == 0, "+15 MB on 100 MB: under the 20 MB floor")
    rc, o = run(lat(100, 100) + [("X.other", "A", [100 * ms]), ("X.other", "B", [200 * ms])])
    expect(rc == 1 and "X.other" in o.split("GP: REGRESSION")[1], "a prefix rule (X.*) applies")
    rc, o = run(lat(100, 100) + [("Y.info", "A", [1 * ms]), ("Y.info", "B", [900 * ms])])
    expect(rc == 0 and " info" in o, "a metric with no row: information, never blocking")
    rc, o = run(lat(100, 100), extra=["--require", "X.lat,Y.gone"])
    expect(rc == 3 and "missing" in o, "a required metric missing: the measurement failed (3)")
    rc, o = run([("X.lat", "A", [100 * ms])])
    expect(rc == 3, "a gating metric on one side only: 3")
    rc, o = run(lat(100, 100) + [("X.lat", "B", ["nan"])])
    expect(rc == 3 and "non-finite" in o, "a sample that isn't a finite number: 3")
    bad = os.path.join(d, "bad")
    for text, why in (("X.lat median 10% 20lightyears first B0\n", "unknown unit"), ("X.lat p95 10% 20ms first B0\n", "unknown statistic"),
                      ("X.lat median +50ms 0 first B0\n", "unknown limit form"), ("X.lat median 10% 20ms first B0 -\n", "not 6 fields"),
                      ("@aa-limit B0 0.5\nX.lat median 10% 20ms first B0\n", "unknown directive"), ("X.lat median 10% 20ms first mac-A\n", "no rule for the baseline B0")):
        open(bad, "w").write(text)
        rc, o = run(lat(100, 100), rulefile=bad)
        expect(rc == 3 and why in o, "a rules file with %s is refused (3)" % why)
    here = os.path.join(os.path.dirname(os.path.abspath(__file__)), "thresholds.txt")
    rc, o = run([("P-B1.warm", "A", [1 * ms] * 4), ("P-B1.warm", "B", [1.4 * ms] * 4), ("P-B4.store", "A", [5 * ms]), ("P-B4.store", "B", [9 * ms])], rulefile=here)
    expect(rc == 0 and "info" in o, "the real thresholds.txt: P-B1.warm +0.4 ms passes, P-B4.store is information")
    return 0 if ok else 1


def main():
    if len(sys.argv) == 2 and sys.argv[1] == "selftest":
        return selftest()
    if len(sys.argv) >= 3 and sys.argv[1] == "summary":
        summary(sys.argv[2])
        return 0
    if len(sys.argv) >= 3 and sys.argv[1] == "gate":
        return gate(sys.argv[2:])
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""The one parser of tests/perf/thresholds.txt, and the one rule (the private Windows plan §8.2). Stdlib only.
Imported by tests/perf/perfstats.py (the bridge's and helper's rows, baseline B0) and
tests/perf/stats.py (the Mac app's rows, baseline mac-A); each selects its rows by baseline.

    rules.py check [FILE]   FILE (default: thresholds.txt beside this script) parsed, with rows
                            for B0, mac-A, WB, WP and W0 and its absolute limits read; exit 0,
                            or 3 with the reason (make check)
    rules.py plan [FILE]    the Mac app's session plan (tests/perf/abab.sh): "SCENARIO LAUNCHES"
                            per @launches mac-A line

The rule (owner decided, 2026-11-12): a slowdown counts only when B is over A by BOTH more than
the row's limit (N% of A) AND more than its floor (20 ms for a time, 20 MB for memory). A change
within 25% of that allowance (75-125% of it) is BORDERLINE: printed for a human to judge, the
verdict staying the rule's.

The file: one gating row per line, whitespace-separated, # starts a comment:
    metric  stat  limit  floor  tag  baseline
  metric    an exact key, or a prefix ending in * (P-B5.*)
  stat      median (over every per-event sample of a side, never over run or launch medians)
  limit     N%
  floor     N<unit>: ns us ms s (time), B KB MB GB (size)
  tag       first | Prepare (the private Windows plan §8.2's tags: the first Windows release, Prepare)
  baseline  B0 | mac-A | WB | WP | W0 (the fixed baselines of release.md §6.1)
Directives:
    @launches BASELINE SCENARIO N METRIC...
                            the session's plan: SCENARIO launched N times per side; the gating
                            METRICs are taken from the scenarios that name them (from every
                            scenario when none does)
    @limit METRIC STAT MAX TAG BASELINE
                            an absolute limit (the Windows app's, app.md §6's table): B alone,
                            STAT (median, p95, max over every per-event sample, or share>Q: the
                            fraction of samples over the bound Q, a quantity with its unit) at
                            most MAX (a quantity, or N% for share>Q); BASELINE names the gate it
                            belongs to (WB, WP, W0), whose first release passes on these alone
Any other line is an error, and so is a baseline with no rows: the tools stop with exit 3.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT = os.path.join(HERE, "thresholds.txt")

UNITS = {
    "ns": ("time", 1), "us": ("time", 1e3), "ms": ("time", 1e6), "s": ("time", 1e9),
    "B": ("size", 1), "KB": ("size", 1024), "MB": ("size", 1024 ** 2), "GB": ("size", 1024 ** 3),
}
STATS = ("median",)
LIMIT_STATS = ("median", "p95", "max", "share>Q")
TAGS = ("first", "Prepare")
BASELINES = ("B0", "mac-A", "WB", "WP", "W0")
NUM = re.compile(r"(\d+(?:\.\d+)?)([A-Za-z]*)$")
BORDER = 0.25


class RuleError(Exception):
    pass


def quantity(text):
    """'20ms' -> (20000000.0, 'time') in base units (ns, bytes); '0' -> (0.0, None)"""
    m = NUM.match(text)
    if not m:
        raise RuleError("not a quantity: %r" % text)
    v, u = float(m.group(1)), m.group(2)
    if not u:
        return v, None
    if u not in UNITS:
        raise RuleError("unknown unit %r in %r" % (u, text))
    dim, scale = UNITS[u]
    return v * scale, dim


def app_units(value, dim):
    """base units -> the Mac app's (stats.py's): ms, MB"""
    if dim == "time":
        return value / 1e6
    if dim == "size":
        return value / 1024 ** 2
    return value


def judge(a, b, limit, floor):
    """the owner's rule: (change, allowed, regression, borderline) of B against A, the row's
    limit a fraction of A and its floor in the same units as A and B"""
    change = b - a
    allowed = max(limit * a, floor)
    return change, allowed, change > allowed, (1 - BORDER) * allowed < change <= (1 + BORDER) * allowed


def limit_row(f, where):
    """one @limit line's fields (after the directive) as a dict; RuleError when it can't be read"""
    if len(f) != 5:
        raise RuleError("%s: can't read %r (@limit METRIC STAT MAX TAG BASELINE)" % (where, " ".join(f)))
    metric, stat, cap, tag, baseline = f
    if not re.fullmatch(r"[A-Za-z0-9_.-]+\*?", metric):
        raise RuleError("%s: not a metric name: %r" % (where, metric))
    bound = bound_dim = None
    if stat.startswith("share"):
        # share>Q: the fraction of samples over Q; a bare share has no bound and is refused
        if not stat.startswith("share>"):
            raise RuleError("%s: a share names its bound, share>Q: %r" % (where, stat))
        try:
            bound, bound_dim = quantity(stat[len("share>"):])
        except RuleError as e:
            raise RuleError("%s: %s" % (where, e))
        if bound_dim is None:
            raise RuleError("%s: a share's bound needs a unit: %r" % (where, stat))
        stat = "share"
    elif stat not in LIMIT_STATS:
        raise RuleError("%s: unknown statistic %r (%s)" % (where, stat, ", ".join(LIMIT_STATS)))
    if stat == "share":
        m = re.fullmatch(r"(\d+(?:\.\d+)?)%", cap)
        if not m:
            raise RuleError("%s: a share's limit is N%%: %r" % (where, cap))
        value, dim = float(m.group(1)) / 100, "share"
    else:
        try:
            value, dim = quantity(cap)
        except RuleError as e:
            raise RuleError("%s: %s" % (where, e))
        if dim is None:
            raise RuleError("%s: a limit needs a unit: %r" % (where, cap))
    if tag not in TAGS:
        raise RuleError("%s: unknown tag %r" % (where, tag))
    if baseline not in BASELINES:
        raise RuleError("%s: unknown baseline %r" % (where, baseline))
    return {"metric": metric, "stat": stat, "max": value, "dim": dim, "bound": bound, "bound_dim": bound_dim,
            "tag": tag, "baseline": baseline}


def limits(path=None):
    """the absolute limits (@limit lines), each a dict; RuleError when the file is bad"""
    path = path or DEFAULT
    parse(path)
    out = []
    for n, line in enumerate(open(path, encoding="utf-8").read().splitlines(), 1):
        f = line.split("#", 1)[0].split()
        if f and f[0] == "@limit":
            d = limit_row(f[1:], "%s:%d" % (os.path.basename(path), n))
            d["line"] = n
            out.append(d)
    return out


def parse(path=None):
    """every row and the plan of the file: (rows, plans {baseline: [...]}); RuleError on anything else
    (the @limit lines are checked here and read by limits())"""
    path = path or DEFAULT
    rows, plans, seen_limits = [], {}, set()
    try:
        lines = open(path, encoding="utf-8").read().splitlines()
    except OSError as e:
        raise RuleError("can't read %s: %s" % (path, e))
    for n, line in enumerate(lines, 1):
        t = line.split("#", 1)[0].strip()
        if not t:
            continue
        f = t.split()
        where = "%s:%d" % (os.path.basename(path), n)
        if f[0] == "@launches":
            if len(f) < 5 or f[1] not in BASELINES or not f[3].isdigit() or int(f[3]) < 1 \
                    or not all(re.fullmatch(r"[A-Za-z0-9_.-]+", m) for m in f[4:]):
                raise RuleError("%s: can't read %r (@launches BASELINE SCENARIO N METRIC...)" % (where, t))
            plan = plans.setdefault(f[1], [])
            if any(e["scenario"] == f[2] for e in plan):
                raise RuleError("%s: @launches %s %s given twice" % (where, f[1], f[2]))
            plan.append({"scenario": f[2], "launches": int(f[3]), "metrics": f[4:]})
            continue
        if f[0] == "@limit":
            d = limit_row(f[1:], where)
            key = (d["metric"], d["stat"], d["baseline"])
            if key in seen_limits:
                raise RuleError("%s: @limit %s %s given twice for %s" % (where, d["metric"], d["stat"], d["baseline"]))
            seen_limits.add(key)
            continue
        if f[0].startswith("@"):
            raise RuleError("%s: unknown directive %r" % (where, f[0]))
        if len(f) != 6:
            raise RuleError("%s: not 6 fields (metric stat limit floor tag baseline): %r" % (where, t))
        if not re.fullmatch(r"[A-Za-z0-9_.-]+\*?", f[0]):
            raise RuleError("%s: not a metric name: %r" % (where, f[0]))
        if f[1] not in STATS:
            raise RuleError("%s: unknown statistic %r (median)" % (where, f[1]))
        m = re.fullmatch(r"(\d+(?:\.\d+)?)%", f[2])
        if not m:
            raise RuleError("%s: unknown limit form %r (N%%)" % (where, f[2]))
        try:
            floor, floor_dim = quantity(f[3])
        except RuleError as e:
            raise RuleError("%s: %s" % (where, e))
        if f[4] not in TAGS:
            raise RuleError("%s: unknown tag %r" % (where, f[4]))
        if f[5] not in BASELINES:
            raise RuleError("%s: unknown baseline %r" % (where, f[5]))
        if any(r["metric"] == f[0] and r["baseline"] == f[5] for r in rows):
            raise RuleError("%s: %s given twice for %s" % (where, f[0], f[5]))
        rows.append({"metric": f[0], "stat": f[1], "limit": f[2], "kind": "rel", "value": float(m.group(1)) / 100,
                     "floor": floor, "floor_dim": floor_dim, "tag": f[4], "baseline": f[5], "line": n})
    # the plan names only metrics that have rows (a typo would never be gated)
    for b, plan in plans.items():
        named = {r["metric"] for r in rows if r["baseline"] == b}
        for m in (x for e in plan for x in e["metrics"]):
            if m not in named:
                raise RuleError("%s: @launches names %s, which has no %s row" % (os.path.basename(path), m, b))
    return rows, plans


def load(path, baseline):
    """the rows of one baseline and its plan; RuleError when the file is bad or holds no row for
    that baseline"""
    rows, plans = parse(path)
    mine = [r for r in rows if r["baseline"] == baseline]
    if not mine:
        raise RuleError("%s holds no rule for the baseline %s" % (path or DEFAULT, baseline))
    return mine, plans.get(baseline, [])


def main(argv):
    if len(argv) in (2, 3) and argv[1] in ("check", "plan"):
        path = argv[2] if len(argv) == 3 else DEFAULT
        try:
            if argv[1] == "check":
                counts = {b: len(load(path, b)[0]) for b in ("B0", "mac-A", "WB", "WP", "W0")}
                nlim = len(limits(path))
            else:
                plan = load(path, "mac-A")[1]
        except RuleError as e:
            print("FAIL measurement: %s" % e)
            return 3
        if argv[1] == "check":
            print("%s: %s; %d absolute limits" % (path, ", ".join("%d rules for %s" % (n, b) for b, n in counts.items()), nlim))
            return 0
        if not plan:
            print("FAIL measurement: %s has no @launches mac-A lines" % path)
            return 3
        for e in plan:
            print("%s %d" % (e["scenario"], e["launches"]))
        return 0
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))

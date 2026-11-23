#!/usr/bin/env python3
"""The Mac app's performance numbers (the private Windows plan's mac-core §2.1-2.2). Stdlib only.

  stats.py samples RUNDIR       the per-event samples of one launch (ms, MB, %)
  stats.py compare ROOT [--a SIDE] [--b SIDE] [--tsv OUT] [common]
        A against B, from one session of tests/perf/abab.sh: each gating row of thresholds.txt
        (baseline mac-A) on its median over the per-event samples of the launches of the
        scenarios its @launches lines name (every scenario where none does), under the rule of
        tests/perf/rules.py; every other number both sides measured is printed for information
        and never blocks. A session of some scenarios only (abab.sh --scenarios) judges the rows
        those scenarios are there for (at least one). Exit 0 pass, 1 a regression (each
        regressed row named), 3 the measurement failed.
  stats.py baselines ROOT [--a SIDE] [--tsv OUT] [common]
        A's numbers alone (mac-A's reference, tests/perf/g0.sh baselines): each gating row's
        median with the spread of its launches' medians, then every other number; a former
        session's own checks are noted (its NOISY rows, perf mode's overhead charges).
        No verdict: exit 0, or 3 when a gating row has no sample.
  stats.py rules [FILE]         thresholds.txt (or FILE) read
  common: --rules FILE     another rules file (the selftest's)
          --no-session     records without abab.sh's session.txt (the selftest's synthetic ones)
          --zip-a Z, --zip-b Z, --zip Z    P13, the app zip's size (information)
          --p15 FILE       P15 (tests/perf/p15.sh): "swift" lines A's, "c" lines B's (information)

The rule (owner decided, 2026-11-12): a slowdown counts only when B is over A by BOTH more than the
row's limit (10%) AND its floor (20 ms, 20 MB). A change within 25% of that allowance is printed
BORDERLINE with each side's per-launch medians, for a human to judge.

The measurement failed (3): a launch of the session's plan (plan_SIDE= in session.txt) missing or
not clean, a launch that failed or had input during it (failed=, disturbed=, idle_unreadable=), a
gating row with no sample on a side, a launch of a row's named scenarios without that row's sample
(a slow B's missing P2, P6 or P7), a measured click without its reaction (P5), a scenario run with
fewer launches than thresholds.txt's plan (-n: a smoke run, not a gate), a session run with
PERF_IDLE_S, PERF_P12_S or PERF_ROUNDS set, or a rules file that can't be read. A launch counts only when its result ends with run.sh's "…: 0
failed" line (no FAIL line), its index is within the plan, and its build.txt names the session's
build of that side and its clone.

ROOT is build/harness/results/LABEL as tests/perf/abab.sh leaves it: ROOT/<SIDE>-<k>/<scenario>/1/
is SIDE's k-th launch of that scenario. The records are the probe's perf-<pid>.tsv
(app/Probe/Probe.swift: time ns, kind, name, number), footprint-*.txt, top*.txt and
watcher-*.json, written by the perf scenarios (tests/harness/scenarios/perf-*.sh).

Metric definitions (start and end events), every event of a launch counted:
  P1        launch (the kernel's process start) -> ev windowDrawn
  P2        launch -> the turn end after the last light change (c.plusIcon/c.cacheIcon) before
            mark "settled" (lights set before the window: the window's own frame); only launches
            that have the mark
  P3.<t>    tp trigger.<t> -> the next tp refresh.end, per trigger; P3 all of them; P3.busy.<t>:
            refresh.begin -> refresh.end; P3.longest.<t>: the longest main-thread turn from the
            trigger to that refresh's turn end
  P4        tp trigger.activate -> the last field set before the round's next mark
  P5        a click command after mark "click N [FIELD]" -> the turn end after its first reaction
            (FIELD, a field id's prefix, or tabShown; else any perf field or ev tabShown) before
            the next command
  P6        cmd select after a "mark P6 …" (full speed) -> the turn end after the tracks table's
            first reload with the whole list (RUNDIR/p6-rows when the scenario writes one, else the
            most rows a reload of that window showed)
  P7.*      from that select to the next mark, by speed (.full, .paced: a playlist ending in 51):
            busy (turns' busy time), stall (the longest turn), lastrow (tp estimate.lastline -> the
            turn end after the last rows reload), done (the select -> that turn end)
  P8.draw   each cmd scroll -> the turn end after the last row fetch before the next command (none:
            after the command); P8.stall: the longest turn in that window; P8.fetch: each row fetch
  P9.<kind> per "mark P9 batch": -> the turn end after the tracks table's first reload in the
            batch's window (to the next mark, answering or starting command, or the dialog's end);
            the kind is the round's "mark P9 <kind>"
  P10.*     phys_footprint (MB) from footprint-idle.txt / footprint-after7.txt
  P11.*     each watcher start: run (launch to exit), cpu, peak (MB)
  P12       CPU % per top sample of perf-idle's window (top.txt, the first left out); P12.start the
            same over perf-start's 60 s window (top-start.txt); P12.wakeups(.start): idle wakeups
            between top's samples
  P13       the app zip's size (MB)
  P14       the old app's "answer Update" -> the new app's ev windowDrawn, the two processes'
            records joined through their kernel start times (pids.txt; perf-update); its split
            (P14.status, .download, .quit, .relaunch, .launch, and with the probe's shutdown
            points and afterexit.txt .shutdown, .exit, .wait, .script, .armed: p14() below) is
            information
  P15       --p15 FILE: the helper-line parse, ns per line ("swift" A, "c" B), shown in µs
  P16       cmd menu Create Report -> its ev dialog ("Report saved"; perf-report), wall time
  P16.own   P16 less P16.children: the app's own time in the report (perf-report's rounds measure
            the app's rusage before the command and after the dialog, rusage-report-N-before.txt
            and -after.txt, tests/harness/vmtool.swift rusage). P16.children: the summed lifetimes
            of the children the app reaped in the round and their reaped descendants (log show,
            codesign, ditto…, which the report runs and waits on), overlapping children counted
            twice: not proof that they ran one at a time. It counts every child reaped in the
            window, also LocationMonitor's pgrep runs that the report's temporary folder sets off
            (A and B alike), and a child started before the command but reaped inside the round.
            More than P16 itself is a measurement failure; a round with only one of its two
            rusage files too (neither: no P16.own for it). P16.cpu: the app's own CPU time in the
            round; P16.children.cpu: the children's

The Windows shell's records (win/shell/Probe.cs; a header with shell=wpf) are read the same way, the
same event names and field ids, with two differences (app.md §6, W11): P1 and P2 start at the
stub's launch when the header has stub_launch_ns (the start is timed from the stub, I5), else at
the app's own launch_ns; P10.* are the private bytes (GetProcessMemoryInfo) from memory-idle.txt and
memory-after7.txt ("private_bytes=N", the Windows runner's), and P10.rows is after7 less idle. A
wpf record is read only for exe/arch anycpu/arm64, anycpu/x64, x64/x64-prism and x64/x64; anything
else is a failed measurement: no exe or another build, another arch, the AnyCPU exe under Prism (an
x64 parent started it, so it ran emulated: app.md §7, the runner starts the app from a native
parent), or the x64-forced exe as arm64.
"""
import glob
import json
import os
import re
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)            # python3 -I drops the script's folder from the path
import rules as rulesmod             # noqa: E402  the one parser of thresholds.txt and the one rule


class MeasurementFailure(Exception):
    pass


def read_perf(path):
    head, ev = {}, []
    for line in open(path, encoding="utf-8"):
        if line.startswith("#"):
            head.update(dict(kv.split("=", 1) for kv in line[1:].split() if "=" in kv))
            continue
        p = line.rstrip("\n").split("\t")
        if len(p) == 4:
            ev.append((int(p[0]), p[1], p[2], int(p[3])))
    return head, ev


def ms(ns):
    return ns / 1e6


def turn_after(ev, t):
    return next((e[0] for e in ev if e[1] == "turn" and e[0] >= t), None)


def rusage(path):
    """vmtool rusage's line (cpu_ns=… child_cpu_ns=… child_elapsed_ns=…), or None: no file"""
    if not os.path.isfile(path):
        return None
    text = open(path, encoding="utf-8", errors="replace").read()
    v = dict(re.findall(r"(\w+)=(\d+)", text))
    if not {"cpu_ns", "child_cpu_ns", "child_elapsed_ns"} <= v.keys():
        raise MeasurementFailure(f"{path}: not vmtool rusage's line ({text.strip()[:80]!r})")
    return {k: int(x) for k, x in v.items()}


def samples_perf(path, p6_rows=None, rundir=None):
    head, ev = read_perf(path)
    if head.get("shell") == "wpf":
        exe, arch = head.get("exe"), head.get("arch")
        if exe not in ("anycpu", "x64"):
            raise MeasurementFailure(f"{path}: exe={exe or '(none)'}: not a known build (anycpu or x64)")
        if arch not in ("arm64", "x64", "x64-prism"):
            raise MeasurementFailure(f"{path}: arch={arch or '(none)'}: not a known architecture")
        if exe == "anycpu" and arch == "x64-prism":
            # an AnyCPU exe runs as ARM64 on ARM64 Windows unless an x64 parent started it (W10's VM
            # round): its numbers are emulated, not the app's (app.md §7: start it from a native parent)
            raise MeasurementFailure(f"{path}: the AnyCPU app ran under x64 emulation (arch=x64-prism): "
                                     "the runner started it from an x64 parent")
        if exe == "x64" and arch == "arm64":
            raise MeasurementFailure(f"{path}: an x64-forced exe can't run as arm64")
    launch = int(head.get("launch_ns", "0"))
    if head.get("shell") == "wpf" and head.get("stub_launch_ns"):
        launch = int(head["stub_launch_ns"])   # Windows: the start is timed from the stub (I5)
    s = {}

    def add(k, v):
        s.setdefault(k, []).append(v)

    marks = [e for e in ev if e[1] == "cmd" and e[2].startswith("mark ")]
    # P1, P2
    drawn = next((e[0] for e in ev if e[1] == "ev" and e[2] == "windowDrawn"), None)
    if drawn is not None and launch:
        add("P1", ms(drawn - launch))
        settled = next((m[0] for m in marks if m[2] == "mark settled"), None)
        lights = [e[0] for e in ev if e[1] == "field" and e[2] in ("c.plusIcon:setImage", "c.cacheIcon:setImage")
                  and settled is not None and e[0] < settled]
        if lights:
            t = drawn if lights[-1] < drawn else turn_after(ev, lights[-1])
            if t:
                add("P2", ms(t - launch))
    # P3 per trigger, P4
    for i, e in enumerate(ev):
        if e[1] != "tp" or not e[2].startswith("trigger."):
            continue
        j = next((j for j in range(i + 1, len(ev)) if ev[j][1] == "tp" and ev[j][2] == "refresh.end"), None)
        if j is None:
            continue
        end, trig = ev[j], e[2].split(".", 1)[1]
        add("P3." + trig, ms(end[0] - e[0]))
        begin = next((x for x in reversed(ev[:j]) if x[1] == "tp" and x[2] == "refresh.begin"), None)
        if begin:
            add("P3.busy." + trig, ms(end[0] - begin[0]))
        stop = turn_after(ev, end[0]) or end[0]
        # the turn the trigger came in counts from the trigger on (at launch it holds the window's set-up)
        turns = [min(x[3], x[0] - e[0]) for x in ev if x[1] == "turn" and e[0] <= x[0] <= stop]
        if turns:
            add("P3.longest." + trig, ms(max(turns)))
        if trig == "activate":
            nxt = next((m[0] for m in marks if m[0] > e[0]), None)
            fields = [x[0] for x in ev if x[1] == "field" and e[0] <= x[0] and (nxt is None or x[0] < nxt)]
            if fields:
                add("P4", ms(fields[-1] - e[0]))
    # P5: the command right after each "mark click N", its reaction before the next command
    acts = ("click ", "menu ", "tab ", "select ", "answer ", "fill ", "password ", "collapse ", "expand ", "select-set ",
            "doubleclick-set ", "until ", "miniaturize", "deminiaturize", "close", "scroll ", "quit", "mark ")
    cmds = [x for x in ev if x[1] == "cmd" and x[2].startswith(acts)]
    for m in marks:
        if not m[2].startswith("mark click"):
            continue
        c = next((x for x in cmds if x[0] > m[0] and not x[2].startswith("mark ")), None)
        if not c:
            continue
        want = m[2].split()[3] if len(m[2].split()) > 3 else None

        def reacts(x):
            if want == "tabShown":
                return x[1] == "ev" and x[2] == "tabShown"
            if want:
                return x[1] == "field" and x[2].startswith(want)
            return x[1] == "field" or (x[1] == "ev" and x[2] == "tabShown")
        end = next((x[0] for x in cmds if x[0] > c[0]), None)
        f = next((x for x in ev if reacts(x) and x[0] >= c[0] and (end is None or x[0] < end)), None)
        t = turn_after(ev, f[0]) if f else None
        if t is not None and (end is None or t < end):
            add("P5", ms(t - c[0]))
        else:
            add("_P5_no_reaction", 1.0)     # a click whose reaction never came: missing data (compare)
    # P6, P7: the scenarios' measured choices only ("mark P6 …" before them)
    for c in [x for x in ev if x[1] == "cmd" and x[2].startswith("select ")]:
        lm = next((m for m in reversed(marks) if m[0] < c[0]), None)
        if lm is None or not lm[2].startswith("mark P6"):
            continue
        nxt = next((m[0] for m in marks if m[0] > c[0]), None)
        win = [x for x in ev if x[0] >= c[0] and (nxt is None or x[0] < nxt)]
        reloads = [x for x in win if x[1] == "field" and x[2] == "prepare.tracksTable:reloadData"]
        need = p6_rows or max((x[3] for x in reloads), default=None)
        big = next((x for x in reloads if x[3] >= need), None) if need else None
        speed = "paced" if c[2].strip().endswith("51") else "full"
        if big and speed == "full":
            t = turn_after(ev, big[0])
            if t:
                add("P6", ms(t - c[0]))
        rows = [x for x in win if x[1] == "field" and x[2].startswith("prepare.tracksTable:")]
        turns = [x[3] for x in win if x[1] == "turn"]
        if rows and turns:
            add("P7.busy." + speed, ms(sum(turns)))
            add("P7.stall." + speed, ms(max(turns)))
            last_line = next((x[0] for x in win if x[1] == "tp" and x[2] == "estimate.lastline"), None)
            t = turn_after(ev, rows[-1][0])
            if t and last_line is not None:
                add("P7.lastrow." + speed, ms(t - last_line))
            if t:
                add("P7.done." + speed, ms(t - c[0]))
    # P8: each scroll step as the user sees it; the row fetches themselves
    for i, c in enumerate(ev):
        if c[1] != "cmd" or not c[2].startswith("scroll "):
            continue
        end = next((x[0] for x in ev[i + 1:] if x[1] == "cmd" and x[2].startswith(acts)), None)
        win = [x for x in ev[i + 1:] if end is None or x[0] < end]
        fetches = [x[0] for x in win if x[1] == "rowfetch"]
        t = turn_after(ev, fetches[-1] if fetches else c[0])
        if t is not None and (end is None or t < end):
            add("P8.draw", ms(t - c[0]))
        turns = [min(x[3], x[0] - c[0]) for x in win if x[1] == "turn"]
        if turns:
            add("P8.stall", ms(max(turns)))
    for x in ev:
        if x[1] == "rowfetch":
            add("P8.fetch", ms(x[3]))
    # P9, per input batch
    stops = ("mark ", "answer ", "click ", "tab ", "select ", "menu ", "quit")
    for i, e in enumerate(ev):
        if e[1] != "cmd" or e[2] != "mark P9 batch":
            continue
        rm = next((m for m in reversed(marks) if m[0] <= e[0] and m[2] != "mark P9 batch"), None)
        if rm is None or not rm[2].startswith("mark P9 "):
            continue
        prior = [x for x in ev[:i] if x[1] == "ev" and x[2] in ("dialog", "dialogEnd")]
        opened = sum(1 for x in prior if x[2] == "dialog") > sum(1 for x in prior if x[2] == "dialogEnd")
        end = next((x[0] for x in ev[i + 1:] if (x[1] == "cmd" and x[2].startswith(stops))
                    or (opened and x[1] == "ev" and x[2] == "dialogEnd")), None)
        row = next((x for x in ev[i + 1:] if x[1] == "field" and x[2].startswith("prepare.tracksTable:") and (end is None or x[0] < end)), None)
        t = turn_after(ev, row[0]) if row else None
        if t is not None and (end is None or t <= end):
            add("P9." + rm[2][len("mark P9 "):], ms(t - e[0]))
    # P16: Create Report, the menu's command to its "Report saved" dialog; P16.own without the
    # waits on its children (the round's "mark report N" names its rusage files)
    for c in [x for x in ev if x[1] == "cmd" and x[2].startswith("menu menu.Help.Create Report")]:
        d = next((x[0] for x in ev if x[1] == "ev" and x[2] == "dialog" and x[0] > c[0]), None)
        if d is None:
            continue
        wall = ms(d - c[0])
        add("P16", wall)
        rm = next((m for m in reversed(marks) if m[0] < c[0]), None)
        if rundir is None or rm is None or not rm[2].startswith("mark report "):
            continue
        n = rm[2][len("mark report "):]
        ru = [rusage(os.path.join(rundir, f"rusage-report-{n}-{w}.txt")) for w in ("before", "after")]
        if ru == [None, None]:
            continue
        if None in ru:
            raise MeasurementFailure(f"{path}: report {n} has only one of its two rusage files")
        child = ms(ru[1]["child_elapsed_ns"] - ru[0]["child_elapsed_ns"])
        if child > wall:
            raise MeasurementFailure(f"{path}: report {n}'s reaped children and their descendants lived {child:.1f} ms "
                                     f"(summed, overlaps counted twice) in its {wall:.1f} ms round")
        add("P16.children", child)
        add("P16.own", wall - child)
        add("P16.cpu", ms(ru[1]["cpu_ns"] - ru[0]["cpu_ns"]))
        add("P16.children.cpu", ms(ru[1]["child_cpu_ns"] - ru[0]["child_cpu_ns"]))
    # P3, P3.busy and P3.longest: every trigger's
    for agg in ("P3", "P3.busy", "P3.longest"):
        subs = sorted(k for k in s if k.startswith(agg + ".") and k.count(".") == agg.count(".") + 1
                      and (agg != "P3" or k.split(".")[1] not in ("busy", "longest")))
        if subs:
            s[agg] = [v for k in subs for v in s[k]]
    return s


def private_mb(path):
    """the Windows runner's memory-*.txt: "private_bytes=N" (GetProcessMemoryInfo), in MB"""
    m = re.search(r"private_bytes=(\d+)", open(path, encoding="utf-8", errors="replace").read())
    return int(m.group(1)) / 1048576 if m else None


def footprint_mb(path):
    text = open(path, encoding="utf-8", errors="replace").read()
    m = re.search(r"(?:phys_footprint|Footprint)\D*?([\d.]+)\s*(KB|MB|GB|B)\b", text)
    if not m:
        return None
    v = float(m.group(1))
    return v / 1024 if m.group(2) == "KB" else v * 1024 if m.group(2) == "GB" else v / 1048576 if m.group(2) == "B" else v


def stamp_ns(text):
    """afterexit.txt's time ("<s>.<ns>", stat -f %Fm) as integer ns, no float rounding; None: not one"""
    m = re.fullmatch(r"(\d+)(?:\.(\d{1,9}))?", text)
    return int(m.group(1)) * 1_000_000_000 + int((m.group(2) or "0").ljust(9, "0")) if m else None


def p14(rundir):
    """P14: the old app's "answer Update" to the new app's windowDrawn, two processes' records
    joined through their kernel start times (pids.txt: pid, µs since 1970; the probe's times are
    relative to its own start, launch_ns the kernel's start in that clock). Then its split, for
    information (each step only when both its ends were recorded; every time in ns since 1970):
      P14.status    answer Update -> the old app's first c.status set
      P14.download  that -> its last c.status set before willTerminate (download and verify)
      P14.quit      that -> ev willTerminate
      P14.relaunch  willTerminate -> the new app's process start (pids.txt)
      P14.launch    the new app's process start -> its windowDrawn
    and, from the probe's shutdown points (perf mode; app/Probe/Probe.swift) and the afterExit
    script's stamps (afterexit.txt: tests/harness/run.sh):
      P14.shutdown  willTerminate -> tp exit.end, the old app's last recorded moment
      P14.exit      tp exit.begin (exit() called) -> tp exit.end: the atexit handlers
      P14.wait      exit.end -> the script's wait over (afterexit-PID-work): the process's
                    teardown plus the poll's rounding up (kill -0 every 0.2 s)
      P14.script    the wait over -> the new app's process start (the copy, signing, --swap,
                    lsregister, open)
      P14.armed     the script's start (afterexit-PID-start) -> exit.end: under one poll
                    interval, the first check after it can find the app gone"""
    out = {}
    starts = {}
    p = os.path.join(rundir, "pids.txt")
    if not os.path.isfile(p):
        return out
    for line in open(p, encoding="utf-8"):
        f = line.split()
        if len(f) == 2 and f[0].isdigit() and f[1].isdigit():
            starts[f[0]] = int(f[1]) * 1000
    old = new = None
    for path in glob.glob(os.path.join(rundir, "perf-*.tsv")):
        head, ev = read_perf(path)
        pid, launch = head.get("pid"), int(head.get("launch_ns", "0"))
        if pid not in starts:
            continue
        wall = [(starts[pid] + e[0] - launch,) + e[1:] for e in ev]
        if any(e[1] == "cmd" and e[2] == "answer Update" for e in wall):
            old = (pid, wall)
        elif any(e[1] == "ev" and e[2] == "windowDrawn" for e in wall):
            new = (starts[pid], wall)
    if old is None or new is None:
        return out

    def first(ev, kind, name, after=None, before=None):
        return next((e[0] for e in ev if e[1] == kind and e[2] == name
                     and (after is None or e[0] >= after) and (before is None or e[0] <= before)), None)

    pid, ev = old
    ans = first(ev, "cmd", "answer Update")
    drawn = first(new[1], "ev", "windowDrawn")
    if drawn is not None and drawn > ans:
        out["P14"] = [ms(drawn - ans)]
    quit_ = first(ev, "ev", "willTerminate", after=ans)
    status = [e[0] for e in ev if e[1] == "field" and e[2].startswith("c.status:") and e[0] >= ans
              and (quit_ is None or e[0] <= quit_)]
    stamps = {}
    p = os.path.join(rundir, "afterexit.txt")
    if os.path.isfile(p):
        for line in open(p, encoding="utf-8"):
            f = line.split()
            m = re.fullmatch(r"afterexit-(\d+)-(start|work)", f[0]) if len(f) == 2 else None
            if m and m.group(1) == pid and stamp_ns(f[1]) is not None:
                stamps[m.group(2)] = stamp_ns(f[1])
    end = first(ev, "tp", "exit.end", after=quit_) if quit_ is not None else None
    begin = first(ev, "tp", "exit.begin", after=quit_) if quit_ is not None else None
    steps = [
        ("P14.status", ans, status[0] if status else None),
        ("P14.download", status[0] if status else None, status[-1] if status else None),
        ("P14.quit", status[-1] if status else None, quit_),
        ("P14.relaunch", quit_, new[0]),
        ("P14.launch", new[0], drawn),
        ("P14.shutdown", quit_, end),
        ("P14.exit", begin, end),
        ("P14.wait", end, stamps.get("work")),
        ("P14.script", stamps.get("work"), new[0]),
        ("P14.armed", stamps.get("start"), end),
    ]
    for name, a, b in steps:
        if a is not None and b is not None and b >= a:
            out[name] = [ms(b - a)]
    return out


def top_cpu(path):
    """top's samples: the CPU % of each (the first left out), and the idle wakeups between
    consecutive samples (top's idlew column is cumulative)"""
    cpu, idlew = [], []
    for line in open(path, encoding="utf-8", errors="replace"):
        f = line.split()
        if len(f) >= 2 and f[0].isdigit():
            try:
                cpu.append(float(f[1]))
            except ValueError:
                continue
            if len(f) >= 4 and f[3].rstrip("+").isdigit():
                idlew.append(int(f[3].rstrip("+")))
    return cpu[1:], [float(b - a) for a, b in zip(idlew, idlew[1:])]


def samples(rundir):
    s = {}
    p6_rows = None
    p = os.path.join(rundir, "p6-rows")
    if os.path.isfile(p):
        try:
            p6_rows = int(open(p, encoding="utf-8").read().strip())
        except ValueError:
            p6_rows = 0
        if p6_rows <= 0:
            raise MeasurementFailure(f"{p}: not a row count")
    for p in sorted(glob.glob(os.path.join(rundir, "perf-*.tsv"))):
        for k, v in samples_perf(p, p6_rows, rundir).items():
            s.setdefault(k, []).extend(v)
    for k, v in p14(rundir).items():
        s[k] = v
    for label in ("idle", "after7"):
        p = os.path.join(rundir, f"footprint-{label}.txt")
        v = footprint_mb(p) if os.path.isfile(p) else None
        if v is not None:
            s.setdefault("P10." + label, []).append(v)
    # Windows: private bytes; P10.rows the growth with 5,000 rows over idle
    mem = {}
    for label in ("idle", "after7"):
        p = os.path.join(rundir, f"memory-{label}.txt")
        v = private_mb(p) if os.path.isfile(p) else None
        if v is not None:
            mem[label] = v
            s.setdefault("P10." + label, []).append(v)
    if len(mem) == 2:
        s.setdefault("P10.rows", []).append(mem["after7"] - mem["idle"])
    for p in sorted(glob.glob(os.path.join(rundir, "watcher-*.json"))):
        w = json.load(open(p))
        s.setdefault("P11.run", []).append(ms(w["exit_ns"] - w["launch_ns"]))
        s.setdefault("P11.cpu", []).append(ms(w["cpu_ns"]))
        s.setdefault("P11.peak", []).append(w["peak_footprint"] / 1048576)
    for name, key, wk in (("top.txt", "P12", "P12.wakeups"), ("top-start.txt", "P12.start", "P12.wakeups.start")):
        p = os.path.join(rundir, name)
        if os.path.isfile(p):
            cpu, wake = top_cpu(p)
            for k, v in ((key, cpu), (wk, wake)):
                if v:
                    s.setdefault(k, []).extend(v)
    return s


RULES_PATH = None           # --rules FILE; default: thresholds.txt
NO_SESSION = False          # --no-session: synthetic records without abab.sh's session.txt


def thresholds():
    """thresholds.txt's mac-A rows (floors in ms and MB) and plan; a bad file stops everything (3)"""
    try:
        found, plan = rulesmod.load(RULES_PATH or os.path.join(HERE, "thresholds.txt"), "mac-A")
    except rulesmod.RuleError as e:
        raise MeasurementFailure(str(e))
    for r in found:
        if not re.fullmatch(r"P\d+(\.\w+)*", r["metric"]):
            raise MeasurementFailure(f"thresholds.txt:{r['line']}: {r['metric']!r} isn't one of the app's metrics (P1-P16)")
        r["floor_app"] = rulesmod.app_units(r["floor"], r["floor_dim"])
    return found, plan


def run_ok(rundir):
    """a launch counts only when its result ends with run.sh's "<name> <verb> <mode>: 0 failed"
    line and has no FAIL line"""
    p = os.path.join(rundir, "result")
    if not os.path.isfile(p):
        return False
    lines = [l.rstrip("\n") for l in open(p, encoding="utf-8", errors="replace") if l.strip()]
    if any(l.startswith("FAIL") for l in lines):
        return False
    return bool(lines) and re.fullmatch(r"\S+ \S+ \S+: 0 failed", lines[-1]) is not None


def provenance_ok(rundir, sha, clone):
    """the run's build.txt names this session's build (and clone, when the session names one)"""
    if sha is None:
        return True
    p = os.path.join(rundir, "build.txt")
    if not os.path.isfile(p):
        return False
    b = dict(l.rstrip("\n").split("=", 1) for l in open(p, encoding="utf-8") if "=" in l)
    return b.get("app_sha256") == sha and (not clone or b.get("clone") == clone)


def session(root):
    """abab.sh's session.txt: the builds (A=, B= with their sha256), clone, knobs, each side's plan
    (plan_SIDE=scen:N,…; a former session's grow_SIDE lines: the last count counts) and the lines
    that fail the measurement; {} with --no-session and no file"""
    p = os.path.join(root, "session.txt")
    if not os.path.isfile(p):
        if NO_SESSION:
            return {}
        raise MeasurementFailure(f"{root}: no session.txt (tests/perf/abab.sh writes it): provenance unknown")
    out, plans, bad = {}, {}, []
    for line in open(p, encoding="utf-8"):
        m = re.match(r"(plan|grow)_(\w+)=(\S*)", line)
        if m:
            for item in filter(None, m.group(3).split(",")):
                scen, n = item.rsplit(":", 1)
                plans.setdefault(m.group(2), {})[scen] = int(n)
            continue
        if re.match(r"(failed|disturbed|idle_unreadable)=", line):
            bad.append(line.strip())
            continue
        m = re.match(r"(A|B)=(.*) sha256=([0-9a-f]{64})\s*$", line)
        if m:
            out.setdefault("sha_" + m.group(1), m.group(3))
            continue
        for m in re.finditer(r"(\w+)=(\S+)", line):
            out.setdefault(m.group(1), m.group(2))
    out["_plans"], out["_bad"] = plans, bad
    return out


def launches(root, side, sess):
    """SIDE's clean launches of the session: [(k, scenario, samples)], and the plan's holes"""
    want = sess.get("sha_B" if side == "B" else "sha_A") if sess else None
    if sess and want is None:
        raise MeasurementFailure(f"{root}: session.txt names no sha256 for {side}")
    plan = sess.get("_plans", {}).get(side) if sess else None
    units, done = [], {}
    for rd in sorted(glob.glob(os.path.join(root, f"{side}-*"))):
        m = re.fullmatch(re.escape(side) + r"-(\d+)", os.path.basename(rd))
        if not m:
            continue
        k = int(m.group(1))
        for r in sorted(glob.glob(os.path.join(rd, "*", "*"))):
            scen = os.path.basename(os.path.dirname(r))
            if not os.path.isdir(r) or (plan is not None and k > plan.get(scen, 0)):
                continue
            if run_ok(r) and provenance_ok(r, want, sess.get("clone") if sess else None):
                units.append((k, scen, samples(r)))
                done[scen] = done.get(scen, 0) + 1
    if sess and not plan:
        return units, [f"{side}: no plan in session.txt"]
    holes = [f"{side}: {scen}: {done.get(scen, 0)} clean launches, {n} planned" for scen, n in sorted((plan or {}).items())
             if done.get(scen, 0) < n]
    return units, holes


def scenarios_of(plan, metric):
    """the scenarios a gating metric is taken from (None: every scenario)"""
    named = {e["scenario"] for e in plan if metric in e["metrics"]}
    return named or None


def gate_values(units, metric, scens):
    """the pooled samples of METRIC from those scenarios, and each launch's own median"""
    per = [u[2][metric] for u in units if (scens is None or u[1] in scens) and u[2].get(metric)]
    return [v for p in per for v in p], [statistics.median(p) for p in per]


def pooled(units):
    out = {}
    for _, _, s in units:
        for k, v in s.items():
            out.setdefault(k, []).extend(v)
    return out


def p15_file(path):
    """--p15 FILE: "swift N" lines A's, "c N" lines B's (ns per line, shown in µs)"""
    out = {"swift": [], "c": []}
    if path and os.path.isfile(path):
        for line in open(path, encoding="utf-8"):
            f = line.split()
            if len(f) == 2 and f[0] in out:
                out[f[0]].append(float(f[1]) / 1000)
    return out["swift"], out["c"]


def fmt(x):
    return "-" if x is None else f"{x:.3f}"


def spread(v):
    return f"{min(v):.1f}-{max(v):.1f}" if v else "-"


def session_problems(sess):
    out = list(sess.get("_bad", [])) if sess else []
    for k in ("PERF_IDLE_S", "PERF_P12_S", "PERF_ROUNDS"):
        if sess and sess.get(k, "default") != "default":
            out.append(f"the session ran with {k}={sess[k]}: not a gate (the scenarios' own forms only)")
    return out


def write_tsv(path, lines):
    if path:
        with open(path, "w", encoding="utf-8") as o:
            o.write("".join(l + "\n" for l in lines))


def cmd_compare(root, a, b, tsv, zips, p15):
    if tsv and os.path.lexists(tsv):
        os.remove(tsv)          # a compare that stops early never leaves an earlier table behind
    rows, plan = thresholds()
    sess = session(root)
    ua, ha = launches(root, a, sess)
    ub, hb = launches(root, b, sess)
    missing = session_problems(sess) + ha + hb
    lines = ["metric\tA\tB\tchange\tallowed\tverdict\tnA\tnB"]
    regressed, borders, gated, judged = [], [], set(), 0
    # a session of some scenarios only (--scenarios: a carried release's smoke set) judges the rows
    # those scenarios are there for; the others are printed as not run
    ran = set(sess.get("_plans", {}).get(a, {})) if sess else None
    # fewer launches than thresholds.txt's plan (abab.sh -n: a smoke run) is not a gate
    want = {e["scenario"]: e["launches"] for e in plan}
    for scen, n in sorted(sess.get("_plans", {}).get(a, {}).items() if sess else []):
        if scen in want and n < want[scen]:
            missing.append(f"{scen}: {n} launches, the plan has {want[scen]}: not a gate")
    for r in rows:
        m = r["metric"]
        gated.add(m)
        scens = scenarios_of(plan, m)
        if ran is not None and scens is not None and not scens & ran:
            lines.append(f"{m}\t-\t-\t-\t-\tnot run (not in this session's plan)\t0\t0")
            continue
        judged += 1
        # every launch of the row's scenarios holds it: a sample that a slow B never produced (P2's
        # lights after "settled", P6's last reload, a click with no reaction) is missing data
        for side, units in ((a, ua), (b, ub)):
            for k, scen, s in units:
                if scens is not None and scen in scens and not s.get(m):
                    missing.append(f"{m}: {side}-{k}/{scen} has no sample")
                if m == "P5" and (scens is None or scen in scens) and s.get("_P5_no_reaction"):
                    missing.append(f"P5: {side}-{k}/{scen}: {len(s['_P5_no_reaction'])} clicks without their reaction")
        (va, la), (vb, lb) = gate_values(ua, m, scens), gate_values(ub, m, scens)
        if not va or not vb:
            missing.append(f"{m}: no samples on {'both sides' if not va and not vb else (a if not va else b)}")
            lines.append(f"{m}\t{fmt(statistics.median(va) if va else None)}\t{fmt(statistics.median(vb) if vb else None)}\t-\t-\tNO SAMPLES\t{len(va)}\t{len(vb)}")
            continue
        xa, xb = statistics.median(va), statistics.median(vb)
        change, allowed, over, border = rulesmod.judge(xa, xb, r["value"], r["floor_app"])
        verdict = "REGRESSION" if over else "pass"
        if over:
            regressed.append(m)
        if border:
            verdict += " BORDERLINE"
            borders.append(f"# BORDERLINE {m}: {change:+.3f} against {allowed:.3f} allowed (within 25% of it): a human judges;"
                           f" per launch {a} {spread(la)}, {b} {spread(lb)}")
        lines.append(f"{m}\t{fmt(xa)}\t{fmt(xb)}\t{change:+.3f}\t{allowed:.3f}\t{verdict}\t{len(va)}\t{len(vb)}")
    # information, never blocking: every other number both sides measured, pooled over every launch
    sa, sb = pooled(ua), pooled(ub)
    for side, z in (("a", zips.get("a")), ("b", zips.get("b"))):
        if z and os.path.isfile(z):
            (sa if side == "a" else sb)["P13"] = [os.path.getsize(z) / 1048576]
    sa["P15"], sb["P15"] = p15_file(p15)
    for k in sorted(k for k in (set(sa) | set(sb)) - gated if not k.startswith("_")):
        va, vb = sa.get(k, []), sb.get(k, [])
        if not va and not vb:
            continue
        xa, xb = (statistics.median(va) if va else None), (statistics.median(vb) if vb else None)
        ch = f"{xb - xa:+.3f}" if va and vb else "-"
        lines.append(f"{k}\t{fmt(xa)}\t{fmt(xb)}\t{ch}\t-\tinfo\t{len(va)}\t{len(vb)}")
    print("\n".join(lines))
    for x in borders:
        print(x)
    print(f"# launches: {a} {len(ua)}, {b} {len(ub)}")
    if not judged:
        missing.append("no gating row in this session's plan")
    rc = 3 if missing else 1 if regressed else 0
    verdict = ("compare: MEASUREMENT FAILED: " + "; ".join(missing)) if missing else \
        (f"compare: REGRESSION: {', '.join(regressed)}") if regressed else "compare: pass"
    print(verdict)
    write_tsv(tsv, [f"# {verdict}"] + lines + borders)
    return rc


def former_notes(root):
    """a former session's own checks (the method before 2026-11-12), noted only: the rows its A/A
    checks called NOISY (aa-N.txt before its growth, aa-final.txt after it), and perf mode's
    overhead charges (overhead-charges.tsv or .refused)"""
    out = []
    for name, what in (("aa-[0-9]*.txt", "its A/A checks before the last"), ("aa-final.txt", "its last A/A check")):
        files = sorted(glob.glob(os.path.join(root, name)))
        rows = sorted({" ".join(l.split("\t")[:2]) for p in files for l in open(p, encoding="utf-8") if "\tNOISY" in l})
        if files:
            out.append(f"# noted: {what} ({', '.join(os.path.basename(p) for p in files)}) called NOISY: {', '.join(rows) or 'none'}")
    for name in ("overhead-charges.tsv", "overhead-charges.tsv.refused"):
        p = os.path.join(root, name)
        if os.path.isfile(p):
            rows = [" ".join(l.split()) for l in open(p, encoding="utf-8") if l.strip() and not l.startswith("#")]
            out.append(f"# noted: perf mode's overhead ({name}: KEY STAT ms charged, information): {'; '.join(rows) or 'none'}")
    return out


def cmd_baselines(root, a, tsv, zipf, p15):
    rows, plan = thresholds()
    sess = session(root)
    units, holes = launches(root, a, sess)
    missing = []
    lines = [f"# mac-A's reference: tests/perf/stats.py baselines of {os.path.basename(os.path.normpath(root))}, side {a}, "
             f"{len(units)} clean launches; medians over every per-event sample (ms, MB, %); owner decided, 2026-11-12",
             "metric\tkind\tA\tn\tlaunches\tper-launch medians"]
    notes = [f"# noted: {h}" for h in holes + session_problems(sess)] + former_notes(root)
    gated = set()
    for r in rows:
        m = r["metric"]
        gated.add(m)
        v, per = gate_values(units, m, scenarios_of(plan, m))
        if not v:
            missing.append(f"{m}: no samples")
        lines.append(f"{m}\tgate\t{fmt(statistics.median(v) if v else None)}\t{len(v)}\t{len(per)}\t{spread(per)}")
    s = pooled(units)
    if zipf and os.path.isfile(zipf):
        s["P13"] = [os.path.getsize(zipf) / 1048576]
    s["P15.swift"] = p15_file(p15)[0]
    for k in sorted(k for k in set(s) - gated if not k.startswith("_")):
        if s[k]:
            lines.append(f"{k}\tinfo\t{fmt(statistics.median(s[k]))}\t{len(s[k])}\t-\t-")
    print("\n".join(lines + notes))
    if missing:
        print("baselines: MEASUREMENT FAILED: " + "; ".join(missing))
    write_tsv(tsv, lines[:1] + notes + lines[1:])
    return 3 if missing else 0


def arg(a, name, default=None):
    return a[a.index(name) + 1] if name in a and a.index(name) + 1 < len(a) else default


def main(a):
    global RULES_PATH, NO_SESSION
    if len(a) >= 3 and a[1] == "samples":
        try:
            got = samples(a[2])
        except MeasurementFailure as e:
            print(f"MEASUREMENT FAILED: {e}")
            return 3
        for k, v in sorted(got.items()):
            print(k, len(v), " ".join(f"{x:.3f}" for x in v[:20]) + (" …" if len(v) > 20 else ""))
        return 0
    RULES_PATH = arg(a, "--rules")
    NO_SESSION = "--no-session" in a
    try:
        if len(a) >= 2 and a[1] == "rules":
            if len(a) > 2:
                RULES_PATH = a[2]
            rows, plan = thresholds()
            print(f"{len(rows)} rules for mac-A, {len(plan)} scenarios in the plan")
            return 0
        if len(a) >= 3 and a[1] == "compare":
            return cmd_compare(a[2], arg(a, "--a", "A"), arg(a, "--b", "B"), arg(a, "--tsv"),
                               {"a": arg(a, "--zip-a"), "b": arg(a, "--zip-b")}, arg(a, "--p15"))
        if len(a) >= 3 and a[1] == "baselines":
            return cmd_baselines(a[2], arg(a, "--a", "A"), arg(a, "--tsv"), arg(a, "--zip"), arg(a, "--p15"))
    except MeasurementFailure as e:
        print(f"MEASUREMENT FAILED: {e}")
        return 3
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))

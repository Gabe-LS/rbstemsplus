#!/usr/bin/env python3
"""The harness's trace tools (the private Windows plan's mac-core §2.3), on the host. Python 3, stdlib only.

  trace.py seq TRACE [KEY-PREFIX]      each field's sequence of distinct frame values, in order
  trace.py compare A B [--scenario S]  the oracle: B's frame sequences against A's (exit 1 on a difference)
  trace.py check TRACE [--scenario S]  the invariants on every captured frame, and the scenario's properties
  trace.py dialogs TRACE               each dialog: its text, where it came from, whether the window
                                       changed under it and whether the work queue was free
  trace.py summary DIR                 every run under DIR: result, frames, dialogs, check
  trace.py checkall TRACEDIR           check on every <scenario>[@verb].jsonl in TRACEDIR (A's traces)
  trace.py sent TRACE [EV] [--after CMD] [--before CMD2]
                                       the Windows probe's sent events (each event the shell sent the
                                       core), in order, as "T EV JSON", then their count: only EV's,
                                       only after the first command CMD (its "verb arg", e.g. "menu
                                       menu.Manage.Show Log") and before the first CMD2 after it (a
                                       tie in t goes by the record's line); an echo guard holds when
                                       none is listed

A trace is the probe's app-<pid>.jsonl (app/Probe/Probe.swift). Frames carry only the fields that
changed; a field's value null means it left the window. Comparisons are on the order of distinct
values, never on times (the harness runs in real time).

A scenario's header may declare (tests/harness/scenarios/*.sh):
  # compare: exact | properties     how the fields named in "allow" are compared; every other
                                    field's sequence must be equal in both modes
  # allow: KEY-PREFIX …             fields (by prefix: name them specifically) whose batching may
                                    differ. exact: the same final value and the same order of first
                                    showing (a value shown again more or fewer times); properties:
                                    the same final value and B's values a subsequence of A's
  # order: KEY1 < KEY2              the frame where KEY1 took its final value comes before KEY2's,
                                    in each trace on its own (check) and alike in A and B (compare)
  # property: no-checking-flash     a row or line that showed a known state never shows "Checking…"
  # must-show: KEY=V1>V2…           the trace shows KEY with V1, later V2, … (whatever the mode)
  # mask: REGEX                       text that differs between runs by nature (a clock time in a
                                    file name) becomes "<masked>" in every value, before anything
  # under-dialog: KEY-PREFIX …      what changes while each dialog is up, compared dialog by
                                    dialog (the fields by prefix): A's and B's alike; with
                                    compare --i2 (B is 1.4.0, PLAN I2, G2.1's waived difference)
                                    B must change them under every dialog A froze them under
                                    (origin main-queue-block: the former work-queue alerts, the
                                    password sheet, "Permission needed"), and alike elsewhere
  # must-show[.pN]: / order[.pN]:   scoped to the scenario's N-th trace (its N-th app copy,
                                    <scenario>.pN.jsonl); unscoped, they apply to the first only
  compare and check take --part N (the trace's copy, default 1); compare takes --i2.
A date picker's unset value (2001-01-01, "days from now" ≤ -3650) reads "unset": it moves daily.
"""
import json
import os
import re
import sys


MASKS = []          # the scenario's "# mask:" patterns (its header), applied by load


def masked(v):
    if isinstance(v, str):
        for m in MASKS:
            v = m.sub("<masked>", v)
    return v


def unset_days(k, v):
    """A date picker never set shows NSDate's reference date: as "days from now" it moves by one each
    day (-9444 on 2026-11-10), so ten years back or more reads "unset"."""
    if k.endswith(".days") and isinstance(v, str) and re.fullmatch(r"-\d+", v) and int(v) <= -3650:
        return "unset"
    return v


def load(path):
    frames, events = [], []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                o = json.loads(line)
            except ValueError:
                continue                      # a line cut by a kill: the run's end
            if "d" in o:
                o["d"] = {k: unset_days(k, masked(v)) for k, v in o["d"].items()}
            if MASKS and "v" in o:
                o["v"] = masked(o["v"])
            if o.get("k") == "frame":
                frames.append(o)
            elif o.get("k") in ("ev", "start", "set"):
                events.append(o)
    return frames, events


def states(frames):
    """The full state after each frame."""
    cur = {}
    for fr in frames:
        for k, v in fr["d"].items():
            if v is None:
                cur.pop(k, None)
            else:
                cur[k] = v
        yield fr, dict(cur)


def sequences(frames):
    seq = {}
    for fr in frames:
        for k, v in fr["d"].items():
            s = seq.setdefault(k, [])
            if not s or s[-1] != v:
                s.append(v)
    return seq


def header(scenario, part=1):
    h = {"compare": "exact", "allow": [], "order": [], "property": [], "must-show": [], "mask": [], "under-dialog": []}
    if not scenario:
        return h
    path = scenario if os.path.isfile(scenario) else os.path.join(os.path.dirname(__file__), "scenarios", scenario.split("@")[0] + ".sh")
    if not os.path.isfile(path):
        return h
    for line in open(path, encoding="utf-8"):
        m = re.match(r"#\s*(compare|allow|order|property|must-show|mask|under-dialog)(?:\[\.p(\d+)\])?:\s*(.*)", line)
        if not m:
            continue
        k, scope, v = m.group(1), m.group(2), m.group(3).strip()
        # must-show and order belong to one trace: unscoped, the first; [.pN], the N-th
        if k in ("must-show", "order") and int(scope or 1) != part:
            continue
        if scope and k not in ("must-show", "order"):
            continue
        if k == "compare":
            h[k] = v.split()[0]
        elif k == "order":
            h[k].append(tuple(x.strip() for x in v.split("<")))
        elif k == "mask":
            h[k].append(re.compile(v))
        elif k == "must-show":
            # the values exactly as written (a row's text may end in a space), ">" between them
            key, _, vals = m.group(3).rstrip("\n").partition("=")
            h[k].append((key.strip(), vals.split(">")))
        else:
            h[k] += v.split()
    return h


def first_seen(s):
    out = []
    for x in s:
        if x not in out:
            out.append(x)
    return out


def subsequence(b, a):
    it = iter(a)
    return all(x in it for x in b)


def dialog_list(events):
    return [e["v"] for e in events if e.get("e") == "dialog"]


def under_dialogs(frames, events, prefixes):
    """Per dialog, in order: (its origin, whether a field of `prefixes` changed while it was up)."""
    out = []
    for d in dialogs_of(frames, events):
        changed = any(k.startswith(p) for k in d["window_changed_under_it"] for p in prefixes)
        out.append((d["origin"], changed))
    return out


def compare(pa, pb, scenario=None, part=1, i2=False):
    h = header(scenario, part)
    fa, ea = load(pa)
    fb, eb = load(pb)
    sa, sb = sequences(fa), sequences(fb)
    problems = []
    allowed = lambda k: any(k.startswith(p) for p in h["allow"])
    for k in sorted(set(sa) | set(sb)):
        a, b = sa.get(k, []), sb.get(k, [])
        if a == b:
            continue
        if allowed(k) and h["compare"] != "properties":
            # a batching difference: a value shown again more or fewer times, never a new value,
            # never another order of first showing, the same final value
            if a[-1:] != b[-1:] or first_seen(a) != first_seen(b):
                problems.append(f"{k} (batching allowed): final {a[-1:]} vs {b[-1:]}, or another order of values:\n    A {a}\n    B {b}")
        elif allowed(k):
            # properties: only the fields the scenario names may lose transient states
            if (a[-1:] != b[-1:]) or not subsequence(b, a):
                problems.append(f"{k}: final {a[-1:]} vs {b[-1:]}, or B's values aren't a subsequence of A's")
        else:
            problems.append(f"{k}:\n    A {a}\n    B {b}")
    da, db = dialog_list(ea), dialog_list(eb)
    if da != db:
        problems.append(f"dialogs:\n    A {da}\n    B {db}")
    problems += [f"B: {x}" for x in must_show(fb, h)]
    for k1, k2 in h["order"]:
        oa, ob = order_of(fa, k1, k2), order_of(fb, k1, k2)
        if oa is not None and ob is not None and oa != ob:
            problems.append(f"order {k1} < {k2}: A {oa}, B {ob}")
    if h["under-dialog"]:
        ua, ub = under_dialogs(fa, ea, h["under-dialog"]), under_dialogs(fb, eb, h["under-dialog"])
        if len(ua) != len(ub):
            problems.append(f"under-dialog: {len(ua)} dialogs in A, {len(ub)} in B")
        for n, ((origin, a_ch), (_, b_ch)) in enumerate(zip(ua, ub), 1):
            if i2 and origin == "main-queue-block":
                # I2: 1.4.0 keeps updating under the dialogs 1.3.0 froze them under
                if not b_ch:
                    problems.append(f"under-dialog {n} ({origin} in A): B changed none of {h['under-dialog']} while it was up (I2 requires it)")
            elif a_ch != b_ch:
                problems.append(f"under-dialog {n} ({origin} in A): {h['under-dialog']} changed under it in {'A, not B' if a_ch else 'B, not A'}")
    return problems


def order_of(frames, k1, k2):
    """Whether KEY1 took its final value in a frame before KEY2 did (None: a key never shown)."""
    last = {}
    for i, fr in enumerate(frames):
        for k in (k1, k2):
            if k in fr["d"]:
                last[k] = i
    if k1 not in last or k2 not in last:
        return None
    return "before" if last[k1] < last[k2] else ("same frame" if last[k1] == last[k2] else "after")


def must_show(frames, h):
    """The scenario's must-show states the trace never shows (in order)."""
    out = []
    seq = sequences(frames)
    for key, want in h["must-show"]:
        if not subsequence(want, seq.get(key, [])):
            out.append(f"must-show {key}={'>'.join(want)}: the trace shows {seq.get(key, [])}")
    return out


def outline_name(row):
    """A Prepare outline row's playlist name: no indentation, open marker or "  (N)" count."""
    t = row.strip()
    if t[:2] in ("v ", "> "):
        t = t[2:]
    m = re.match(r"(.*?)  \((\d+)\)$", t)
    return (m.group(1), int(m.group(2))) if m else (t, None)


MANAGE = ["c.installPlusBtn", "c.reinstallPlusBtn", "c.uninstallPlusBtn", "c.installCacheBtn", "c.reinstallCacheBtn", "c.uninstallCacheBtn"]


def check(path, scenario=None, part=1):
    h = header(scenario, part)
    frames, events = load(path)
    problems = []
    seen_state = {}
    for fr, st in states(frames):
        n = fr["n"]
        # rows: as many recorded as the table says (up to the probe's 500)
        for t in ("prepare.tracksTable", "status.table", "prepare.outline"):
            rows = st.get(t + ".rows")
            if rows is None:
                continue
            have = sum(1 for k in st if re.fullmatch(re.escape(t) + r"\.r\d+", k))
            if have != min(int(rows), 500):
                problems.append(f"frame {n}: {t} has {have} rows recorded, numberOfRows {rows}")
        # Prepare/Keep on only with the current selection's whole estimate
        if st.get("prepare.prepareBtn.en") == "1" and st.get("prepare.prepareBtn.hid") != "1":
            info = st.get("prepare.info.str", "")
            if info.startswith("Checking the playlist"):
                problems.append(f"frame {n}: Prepare on while the summary says Checking")
            checking = [k for k, v in st.items() if k.startswith("prepare.tracksTable.r") and "| Checking… |" in v]
            if checking:
                problems.append(f"frame {n}: Prepare on while {len(checking)} rows say Checking…")
        # while an action runs (the spinner), the Manage buttons are off
        if st.get("c.spinner.anim") == "1":
            on = [b for b in MANAGE if st.get(b + ".en") == "1"]
            if on:
                problems.append(f"frame {n}: spinner on with {on} enabled")
        # the status line and buttons agree with the lights between actions (a Manage button on)
        if st.get("c.spinner.anim") == "0" and any(st.get(b + ".en") == "1" for b in MANAGE):
            plus_on = st.get("c.plusIcon.img") == "on"
            if "c.uninstallPlusBtn.en" in st and (st["c.uninstallPlusBtn.en"] == "1") != plus_on:
                problems.append(f"frame {n}: Uninstall Demucs v4 {'off' if plus_on else 'on'} while its light says {st.get('c.plusIcon.img')}")
            if st.get("c.installPlusBtn.en") == "1" and plus_on:
                problems.append(f"frame {n}: Install Demucs v4 on while its light is on")
            prog = [x for x in st.get("c.status.str", "").split("\n") if x.endswith("…")]
            if prog:
                problems.append(f"frame {n}: an action's progress in the status line between actions: {prog}")
        # Keep and Prepare say what the estimate says; the summary, rows and name are the selection's
        idle = st.get("prepare.pauseBtn.hid") == "1"
        if idle and st.get("prepare.prepareBtn.en") == "1" and st.get("prepare.prepareBtn.hid") != "1":
            info, title = st.get("prepare.info.str", ""), st.get("prepare.prepareBtn.title", "")
            if title == "Keep" and (" to prepare (" in info or " ready" not in info):
                problems.append(f"frame {n}: Keep on while the summary isn't all ready: {info[:80]!r}")
            if title == "Prepare" and " to prepare (" not in info:
                problems.append(f"frame {n}: Prepare on while the summary has nothing to prepare: {info[:80]!r}")
            sel = st.get("prepare.outline.sel", "-1")
            row = st.get(f"prepare.outline.r{sel}") if sel != "-1" else None
            if row is not None:
                name, count = outline_name(row)
                if st.get("prepare.nameLabel.str") != name:
                    problems.append(f"frame {n}: Prepare on for {st.get('prepare.nameLabel.str')!r} while {name!r} is selected")
                if count is not None and st.get("prepare.tracksTable.rows") != str(count):
                    problems.append(f"frame {n}: Prepare on with {st.get('prepare.tracksTable.rows')} rows for {name!r} ({count} tracks): another check's rows")
        if "no-checking-flash" in h["property"]:
            for k, v in st.items():
                if not (k.startswith("status.table.r") or k.startswith("prepare.tracksTable.r") or k == "prepare.info.str"):
                    continue
                prev = seen_state.get(k, "")
                known = prev and not any(x in prev for x in ("Checking", "Not checked", "Choose a playlist", "A folder:", "Intelligent playlists"))
                if "Checking" in v and known:
                    problems.append(f"frame {n}: {k} went from a known state to Checking…: {seen_state[k]!r} -> {v!r}")
                seen_state[k] = v
    for k1, k2 in h["order"]:
        o = order_of(frames, k1, k2)
        if o is not None and o != "before":
            problems.append(f"order {k1} < {k2}: {k1} took its final value {o} {k2}")
    problems += must_show(frames, h)
    return problems


def dialogs(path):
    return dialogs_of(*load(path))


def dialogs_of(frames, events):
    out = []
    opened = {}
    free = {e.get("n"): int(e.get("v") or 0) for e in events if e.get("e") == "workQueueFree"}
    for e in events:
        if e.get("e") == "dialog":
            opened[e["n"]] = e
        elif e.get("e") == "dialogEnd":
            b = opened.pop(e["n"], None)
            if not b:
                continue
            under = [fr for fr in frames if b["t"] < fr["t"] < e["t"]]
            keys = sorted({k for fr in under for k in fr["d"] if not k.startswith(("app.", "win.", "menu."))})
            wq = free.get(b["n"])
            wq_free = wq is not None and b["t"] + wq < e["t"]
            out.append({"n": b["n"], "dialog": b["v"], "origin": b.get("origin", ""),
                        "work_queue": "free" if wq_free else "blocked (asked from the work queue, or busy)",
                        "window_changed_under_it": keys})
    return out


def summary(d):
    for rootdir, _, files in sorted(os.walk(d)):
        traces = [f for f in files if f.startswith("app-") and f.endswith(".jsonl")]
        if "result" not in files:
            continue
        res = open(os.path.join(rootdir, "result"), encoding="utf-8").read().strip().splitlines()
        line = res[-1] if res else "?"
        extra = ""
        if traces:
            t = os.path.join(rootdir, traces[0])
            fr, ev = load(t)
            scen = os.path.basename(os.path.dirname(rootdir))
            MASKS[:] = header(scen)["mask"]
            pr = check(t, scen)
            extra = f" frames={len(fr)} dialogs={len(dialog_list(ev))} invariants={'ok' if not pr else len(pr)}"
        print(f"{os.path.relpath(rootdir, d)}: {line}{extra}")


def sent_name(js):
    """A sent event's name: its JSON's "ev" member, else "?"."""
    try:
        o = json.loads(js)
    except (TypeError, ValueError):
        return "?"
    return (o.get("ev") or "?") if isinstance(o, dict) else "?"


def sent(path, ev=None, after=None, before=None):
    """The sent events [(t, ev, json)], EV's only, after the first command AFTER and before the
    first command BEFORE after it, each bound a (t, record index) pair: a tie in t goes by the
    record's line. None when a named command isn't in the trace."""
    _, events = load(path)

    def cmd_at(v, above=None):
        return next(((e["t"], i) for i, e in enumerate(events) if e.get("e") == "cmd" and e.get("v", "").strip() == v
                     and (above is None or (e["t"], i) > above)), None)
    lo = hi = None
    if after is not None:
        lo = cmd_at(after)
        if lo is None:
            return None
    if before is not None:
        hi = cmd_at(before, lo)
        if hi is None:
            return None
    out = []
    for i, e in enumerate(events):
        if e.get("e") != "sent" or (lo is not None and (e["t"], i) < lo) or (hi is not None and (e["t"], i) > hi):
            continue
        name = sent_name(e.get("v"))
        if ev is None or name == ev:
            out.append((e["t"], name, e.get("v")))
    return out


def main(a):
    if len(a) < 2:
        print(__doc__)
        return 2
    after = before = None
    for flag in ("--after", "--before"):
        if flag in a:
            i = a.index(flag)
            if i + 1 >= len(a):
                print(f"{flag} needs a command")
                return 2
            if flag == "--after":
                after = a[i + 1]
            else:
                before = a[i + 1]
            a = a[:i] + a[i + 2:]
    scen = None
    if "--scenario" in a:
        i = a.index("--scenario")
        scen = a[i + 1]
        a = a[:i] + a[i + 2:]
    part, i2 = 1, False
    if "--part" in a:
        i = a.index("--part")
        part = int(a[i + 1])
        a = a[:i] + a[i + 2:]
    if "--i2" in a:
        i2 = True
        a = [x for x in a if x != "--i2"]
    cmd = a[1]
    MASKS[:] = header(scen)["mask"]
    if cmd == "seq":
        frames, _ = load(a[2])
        for k, s in sorted(sequences(frames).items()):
            if len(a) > 3 and not k.startswith(a[3]):
                continue
            print(k, json.dumps(s, ensure_ascii=False))
    elif cmd == "compare":
        p = compare(a[2], a[3], scen, part, i2)
        for x in p:
            print(x)
        print("same frame sequences" if not p else f"{len(p)} differences")
        return 1 if p else 0
    elif cmd == "check":
        p = check(a[2], scen, part)
        for x in p:
            print(x)
        print("invariants hold" if not p else f"{len(p)} problems")
        return 1 if p else 0
    elif cmd == "dialogs":
        print(json.dumps(dialogs(a[2]), indent=1, ensure_ascii=False))
    elif cmd == "summary":
        summary(a[2])
    elif cmd == "sent":
        s = sent(a[2], a[3] if len(a) > 3 else None, after, before)
        if s is None:
            print(f"no command {after!r} then {before!r} in the trace" if before is not None else f"no command {after!r} in the trace")
            return 1
        for t, name, js in s:
            print(t, name, js)
        print(f"{len(s)} sent")
    elif cmd == "checkall":
        bad = 0
        for f in sorted(os.listdir(a[2])):
            if f.endswith(".jsonl"):
                name, _, pn = f[:-6].partition(".p")
                MASKS[:] = header(name)["mask"]
                p = check(os.path.join(a[2], f), name, int(pn) if pn.isdigit() else 1)
                bad += bool(p)
                print(f"{f[:-6]}: " + ("invariants hold" if not p else f"{len(p)} problems") + "".join("\n    " + x for x in p[:5]))
        return 1 if bad else 0
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

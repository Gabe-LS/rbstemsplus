#!/usr/bin/env python3
"""The core DLL's native steps against what the shared flows ask on Windows (plan N4; make
winc-test): a step the core can ask on Windows with no case in win/c/core_native_steps.c's
nw_steps_run answers CORE_E_STATE, and its flow fails or skips silently (Create Report did, the
GPT audit's a4 F6 / a6 F10). So:

1. Every core_step in core/core.h is classified here: done in nw_steps_run (read from its switch,
   its returned helpers' step switches and the bounded location worker), or NOT_ON_WINDOWS (never
   asked by a Windows core, with proof anchors and every use inside its guarded region).
   A step in core.h that is
   neither fails, as does one the core asks (any core/*.c) that isn't done or classified, and a
   classified one that is now done (drop it from the table). The location choice's steps (plan
   N3: LOCATION_PROBE … THIS_MAC_DELETE) are done since N3; their gate is SETTINGS' can_choose.
2. Read on the code alone (review NS15): comments are stripped before anything is matched, so a
   case, guard or anchor in a comment or #if 0 block counts for nothing; every enumerator of core_step is matched,
   and one without a value fails; the callers are every core/*.c, so a new flow file is read too.
3. A region holds its step's every use, and its static functions are called only from inside it
   or from the functions listed for it (review NS14): a helper of the watcher's rounds called from
   the app's own flows would ask a WATCH_ step outside the watcher's role.
4. Its own mutants first: each must fail, or the check itself is broken.
Exit 0 when all hold, 1 when one doesn't. Stdlib only.
"""
import glob
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
CORE_H = "core/core.h"
STEPS = "win/c/core_native_steps.c"
FLOWS = "core/flows.c"
HOST = "win/shell/Host.cs"

# never asked by a Windows core: (why, anchors: (file, regex) that must match the code, region:
# (file, start regex, end regex) holding every use of the step, allowed: the functions outside the
# region that may call its static functions)
WATCHER_ROLE = ("the watcher's rounds run only for a core made with CORE_ROLE_WATCHER (flows_watch_start); the Windows "
                "watcher is rbsp-watch.exe (win/c/watch*.c) with no core, and the WPF shell makes only CORE_ROLE_APP cores",
                [(FLOWS, r"if \(c->role != CORE_ROLE_WATCHER \|\| c->flow_running\) return CORE_E_STATE;"),
                 (HOST, r"core_new\(ref table, CoreApi\.CORE_ROLE_APP,")],
                (FLOWS, r"^enum \{ WATCH_MAX_ROUNDS = 3 \};", r"^int flows_watch_start\(core \*c\) \{"),   # the rounds' first code line
                ["flows_watch_start"])
NOT_ON_WINDOWS = {
    "DOCK": ("no Dock on Windows: the complete uninstall asks it only for a Mac core",
             [(FLOWS, r"if \(me\[0\] && x->os != CORE_OS_WIN\) \{")],
             (FLOWS, r"if \(me\[0\] && x->os != CORE_OS_WIN\) \{", r"^    \}"),
             []),
}
for k in ["WATCH_LOCK", "WATCH_PAUSE", "WATCH_BUSY", "WATCH_FACTS", "WATCH_REMEMBER", "WATCH_REINSTALL", "WATCH_KEPT", "WATCH_HELPER"]:
    NOT_ON_WINDOWS[k] = WATCHER_ROLE


def strip_comments(t):
    """C/C# comments blanked (newlines kept, so line numbers hold); strings and chars kept"""
    out, i, n = [], 0, len(t)
    while i < n:
        c = t[i]
        if c in "\"'":
            j = i + 1
            while j < n and t[j] != c and t[j] != "\n":
                j += 2 if t[j] == "\\" else 1
            out.append(t[i:j + 1])
            i = j + 1
        elif t.startswith("/*", i):
            j = t.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", t[i:j]))
            i = j
        elif t.startswith("//", i):
            j = t.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def strip_disabled(t):
    """Blank #if 0 branches, preserving line numbers and unknown build conditions."""
    lines, stack, active = [], [], True
    for line in t.splitlines(keepends=True):
        m = re.match(r"^[ \t]*#[ \t]*(if|ifdef|ifndef|elif|else|endif)\b(.*)", line)
        if m:
            directive, condition = m.group(1), m.group(2).strip()
            if directive in ("if", "ifdef", "ifndef"):
                stack.append((active, active and (directive != "if" or condition != "1")))
                active = active and (directive != "if" or condition != "0")
            elif directive in ("else", "elif"):
                parent, available = stack[-1]
                active = available and (directive == "else" or condition != "0")
                stack[-1] = (parent, available and directive == "elif" and condition != "1")
            else:
                active, _ = stack.pop()
        lines.append(line if not m and active else re.sub(r"[^\n]", " ", line))
    return "".join(lines)


def read(path):
    with open(os.path.join(ROOT, path), encoding="utf-8") as f:
        return f.read()


def enum_steps(h):
    """(names, names without a value) of core_step's enumerators"""
    body = re.search(r"typedef enum core_step \{(.*?)\} core_step;", h, re.S)
    if not body:
        return [], []
    names, bare = [], []
    for item in body.group(1).split(","):
        m = re.match(r"\s*(CORE_STEP_[A-Z0-9_]+)\s*(=)?", item)
        if m:
            names.append(m.group(1)[len("CORE_STEP_"):])
            if not m.group(2):
                bare.append(m.group(1))
        elif item.strip():
            bare.append(item.strip())
    return names, bare


FN_DEF = re.compile(r"^(static\s+)?[A-Za-z_]\w*[\w\s*]*?\b([a-z_]\w*)\s*\([^;{}]*\)\s*\{", re.M)
BRACE = re.compile(r"[{}]")
LITERAL = re.compile(r'''"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*' ''', re.X)


def mask_literals(src):
    return LITERAL.sub(lambda m: " " * len(m[0]), src)


def closing_brace(src, start):
    depth = 1
    for m in BRACE.finditer(src, start):
        depth += 1 if m[0] == "{" else -1
        if depth == 0:
            return m.start()
    return len(src)


def step_switches(body):
    """Yield (step -> calls in return expressions, state default) for each step switch."""
    for switch in re.finditer(r"\bswitch\s*\([^{};]*?\)\s*\{", body):
        src = body[switch.end():closing_brace(body, switch.end())]
        labels, depth = [], 0
        for m in re.finditer(r"[{}]|\b(?:case\s+(?:CORE_STEP_(\w+)|[^{}:]+)|default)\s*:", src):
            if m[0] in ("{", "}"):
                depth += 1 if m[0] == "{" else -1
            elif depth == 0:
                labels.append(m)
        cases, default_ok, targets, end = {}, False, (), len(src)
        for label in reversed(labels):
            branch = src[label.end():end].strip()
            if branch:
                targets = [call for result in re.findall(r"\breturn\s+([^;]+);", branch)
                           for call in re.findall(r"\b(\w+)\s*\(", result)]
            if label[1]:
                cases[label[1]] = targets
            elif label[0].startswith("default"):
                default_ok = bool(re.fullmatch(r"return\s+CORE_E_STATE\s*;", branch))
            end = label.start()
        if cases or default_ok:
            yield cases, default_ok


def done_steps(src):
    src = mask_literals(src)
    bodies = {m[2]: src[m.end():closing_brace(src, m.end())] for m in FN_DEF.finditer(src)}
    switches = {name: list(step_switches(body)) for name, body in bodies.items()}
    if "nw_steps_run" not in switches:
        return None, False

    def handles(helper, step, seen):
        if helper in seen:
            return False
        seen = seen | {helper}
        own_switches = switches.get(helper, [])
        if helper == "location_choice_read_step":
            # This helper publishes by kind after native_choice_read runs the worker on check.kind.
            # Both switches must handle the step, including when a switch or the callback is removed.
            if not own_switches:
                return False
            worker = re.search(r"\bnative_choice_read\s*\(\s*(\w+)\s*,", bodies[helper])
            if not worker or not switches.get(worker[1]) or not handles(worker[1], step, seen):
                return False
        return all(step in cases and all(handles(target, step, seen) for target in cases[step])
                   for cases, _ in own_switches)

    entry = [cases for cases, state_default in switches["nw_steps_run"] if state_default]
    done = {step for cases in entry for step, helpers in cases.items()
            if all(handles(helper, step, {"nw_steps_run"}) for helper in helpers)}
    return done, bool(entry)


def callers(texts):
    return sorted(f for f in texts if f.startswith("core/") and f.endswith(".c"))


def uses(texts):
    """step -> [(file, line number)] of its uses in the core's callers"""
    out = {}
    for f in callers(texts):
        for i, line in enumerate(texts[f].split("\n"), 1):
            for k in re.findall(r"\bCORE_STEP_([A-Z0-9_]+)\b", line):
                out.setdefault(k, []).append((f, i))
    return out


def region(texts, r):
    """the line range [a, b] of a region (file, start, end), or None"""
    f, start, end = r
    lines = texts.get(f, "").split("\n")
    a = next((i for i, l in enumerate(lines, 1) if re.search(start, l)), None)
    if a is None:
        return None
    b = next((i for i, l in enumerate(lines, 1) if i > a and re.search(end, l)), None)
    return (f, a, b) if b else None


def definitions(src):
    """Yield (first line, last line, name, static) for definitions, including split signatures."""
    src = mask_literals(src)
    for m in FN_DEF.finditer(src):
        yield src.count("\n", 0, m.start()) + 1, src.count("\n", 0, closing_brace(src, m.end())) + 1, m[2], bool(m[1])


def check(texts):
    texts = {f: strip_comments(t) for f, t in texts.items()}
    texts = {f: strip_disabled(t) if f.endswith((".c", ".h")) else t for f, t in texts.items()}
    errs = []
    steps, bare = enum_steps(texts[CORE_H])
    if not steps:
        return ["core.h: no core_step enum"]
    for b in bare:
        errs.append(f"core.h: core_step's {b} has no value")
    done, default_ok = done_steps(texts[STEPS])
    if done is None:
        return [STEPS + ": no nw_steps_run"]
    if not default_ok:
        errs.append(STEPS + ": nw_steps_run's default isn't CORE_E_STATE")
    used = uses(texts)
    for k in sorted(done - set(steps)):
        errs.append(f"{STEPS}: case CORE_STEP_{k} isn't a core.h step")
    for k in sorted(set(used) - set(steps)):
        errs.append(f"CORE_STEP_{k}: asked at {used[k][0][0]}:{used[k][0][1]}, not a core.h step")
    checked_regions = set()
    for k in steps:
        classes = [n for n, t in (("done", k in done), ("NOT_ON_WINDOWS", k in NOT_ON_WINDOWS)) if t]
        if not classes:
            where = ", ".join(f"{f}:{n}" for f, n in used.get(k, [])[:3]) or "no caller"
            errs.append(f"CORE_STEP_{k}: not done in {STEPS} and not classified here (asked at {where})")
            continue
        if len(classes) > 1:
            errs.append(f"CORE_STEP_{k}: now done in {STEPS}: take it out of {classes[1]}")
            continue
        if k not in NOT_ON_WINDOWS:
            continue
        why, anchors, reg, allowed = NOT_ON_WINDOWS[k]
        for f, rx in anchors:
            if not re.search(rx, texts.get(f, ""), re.M):
                errs.append(f"CORE_STEP_{k}: its proof ({why}) no longer holds: {f}'s code has no /{rx}/")
        r = region(texts, reg)
        for f, n in used.get(k, []):
            if not r or f != r[0] or not r[1] <= n <= r[2]:
                errs.append(f"CORE_STEP_{k}: asked at {f}:{n}, outside its Windows-free region ({reg[1]})")
        if not r or reg in checked_regions:
            continue
        checked_regions.add(reg)
        # the region's static functions: called only inside it or from its allowed functions (NS14)
        f, a, b = r
        funcs = list(definitions(texts[f]))
        own = {name for start, _, name, static in funcs if static and a <= start <= b}
        enc = {i: name for start, end, name, _ in funcs for i in range(start, end + 1)}
        for i, l in enumerate(texts[f].splitlines(), 1):   # static: its own file only
            if a <= i <= b or enc.get(i, "") in allowed or re.match(r"^static\s[^=]*\)\s*;\s*$", l):
                continue
            for name in own:
                if re.search(r"\b" + re.escape(name) + r"\b", l):   # a call or a pointer to it
                    errs.append(f"CORE_STEP_{k}: {name}, a function of its region ({reg[1]}), called at {f}:{i} from "
                                f"{enc.get(i) or 'outside a function'}")
    return errs


def main():
    files = [CORE_H, STEPS, HOST] + sorted(os.path.relpath(p, ROOT) for p in glob.glob(os.path.join(ROOT, "core", "*.c")))
    texts = {}
    for f in files:
        try:
            texts[f] = read(f)
        except FileNotFoundError:
            print(f"native-steps-reach: {f} is missing")
            return 1
    for need in (FLOWS, "core/view.c"):
        if need not in texts:
            print(f"native-steps-reach: {need} is missing")
            return 1
    # a static function of the watcher's rounds, for the NS14 mutant
    flows = strip_disabled(strip_comments(texts[FLOWS]))
    r = region({FLOWS: flows}, WATCHER_ROLE[2])
    helper = next((name for start, _, name, static in definitions(flows) if static and r[1] <= start <= r[2]), None) if r else None
    # its own mutants first: (file, old, new, optional additional (old, new) edits), each must fail
    mutants = {
        "a case taken out (OPEN_URL)": (STEPS, "case CORE_STEP_OPEN_URL:", "case CORE_STEP_OPEN_URL_GONE:"),
        "a case commented out (DATE_TEXT)": (STEPS, "case CORE_STEP_DATE_TEXT:", "/* case CORE_STEP_DATE_TEXT: */"),
        "a location choice's case taken out (THIS_MAC_DELETE)": (STEPS, "case CORE_STEP_THIS_MAC_DELETE: return location_choice_step",
                                                                 "case CORE_STEP_THIS_MAC_DELETE_GONE: return location_choice_step"),
        "the bounded worker case taken out (LOCATION_SIZE)": (
            STEPS, "    case CORE_STEP_LOCATION_SIZE: check->out.size = native_choice_size(check->path); break;\n", ""),
        "the publish case taken out (LOCATION_SIZE)": (
            STEPS, "    case CORE_STEP_LOCATION_SIZE: core_fact_i64(out, FACT_STEP_CODE, check.out.size); break;\n", ""),
        "the publish case taken out after a conditional dispatch (LOCATION_DISCOVER)": (
            STEPS, "    case CORE_STEP_LOCATION_DISCOVER: {\n        native_choice_found *f = &check.out.found;",
            "    case CORE_STEP_LOCATION_DISCOVER_GONE: {\n        native_choice_found *f = &check.out.found;"),
        "the bounded worker call bypassed": (
            STEPS, "native_choice_read(run_location_choice_check, &check, sizeof check, s->choice_deadline)", "0"),
        "a dispatch case disabled (LOCATION_SIZE)": (STEPS, "    case CORE_STEP_LOCATION_SIZE:\n",
                                                     "#if 0\n    case CORE_STEP_LOCATION_SIZE:\n#endif\n"),
        "the Dock's guard gone": (FLOWS, "if (me[0] && x->os != CORE_OS_WIN) {", "if (me[0]) {"),
        "the watcher-role guard commented out": (FLOWS, "if (c->role != CORE_ROLE_WATCHER || c->flow_running) return CORE_E_STATE;",
                                                 "/* if (c->role != CORE_ROLE_WATCHER || c->flow_running) return CORE_E_STATE; */"),
        "a new step in core.h": (CORE_H, "    CORE_STEP_REPORT_FACTS = 61", "    CORE_STEP_REPORT_FACTS = 61,\n    CORE_STEP_NEW_ONE = 62"),
        "an unnumbered step in core.h": (CORE_H, "    CORE_STEP_REPORT_FACTS = 61", "    CORE_STEP_REPORT_FACTS = 61,\n    CORE_STEP_NEW_UNNUMBERED"),
        "a WATCH_ step asked by the app's flows": (FLOWS, "static void startup_work(fx *x) {",
                                                   "static void startup_work(fx *x) {\n    fx_do(x, CORE_STEP_WATCH_KEPT, 0);"),
        "a new flow file asking DOCK": ("core/newflow.c", None, "static void f(fx *x) {\n    fx_do(x, CORE_STEP_DOCK, 0);\n}\n"),
        "a split watcher-round helper called from the app's flows": (
            FLOWS, "static void w_kept_check(watch_ctx *w) {", "static void w_kept_check(\n    watch_ctx *w) {",
            ("static void startup_work(fx *x) {", "static void startup_work(fx *x) {\n    w_kept_check(NULL);")),
    }
    if helper:
        mutants["a watcher-round helper called from the app's flows"] = (FLOWS, "static void startup_work(fx *x) {",
                                                                         f"static void startup_work(fx *x) {{\n    {helper}(x);")
    broken = [] if helper else ["no static function found in the watcher's rounds for the NS14 mutant"]
    for name, (f, a, b, *extra) in mutants.items():
        m = dict(texts)
        if a is None:
            m[f] = b
        else:
            for old, new in [(a, b), *extra]:
                if old not in m[f]:
                    broken.append(f"mutant '{name}': {f} no longer has {old!r}")
                    break
                m[f] = m[f].replace(old, new, 1)
        if not check(m):
            broken.append(f"mutant '{name}' passed")
    if broken:
        print("native-steps-reach: the check itself is broken:\n  " + "\n  ".join(broken))
        return 1
    errs = check(texts)
    if errs:
        print("native-steps-reach: a step the core asks on Windows has no native implementation:\n  " + "\n  ".join(errs))
        return 1
    steps, _ = enum_steps(strip_comments(texts[CORE_H]))
    done, _ = done_steps(strip_disabled(strip_comments(texts[STEPS])))
    print(f"native-steps-reach: {len(steps)} steps: {len(done)} done natively, {len(NOT_ON_WINDOWS)} never asked on Windows; "
          f"{len(callers(texts))} core files read, {len(mutants)} mutants caught")
    return 0


if __name__ == "__main__":
    sys.exit(main())

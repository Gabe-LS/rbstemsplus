#!/usr/bin/env python3
"""Create Report's tools, 1.3.0's against today's (review PO1, make core-test): the report waits on
the tools it runs (log show, codesign, ditto…), so a tool added, run more often, moved into a loop
or given other arguments changes what the user waits for, whatever the P16 gate measures.

1. The multiset of runTool(…) and run(…) call texts (the executable and its arguments, whitespace
   folded) in Report.swift, each with whether it sits inside a loop (for, while, repeat, or a
   closure of map, forEach, compactMap, flatMap, filter, reduce), is the frozen oracle's
   (core/tests/swift/oracle/Report.swift) exactly in app/Sources/Report.swift.
2. The switch-over's own report code starts no process: no runTool(, run(, Process(, posix_spawn
   or child( in ShellFacts.reportLights, Report.swift's reportMeasure, or HostSteps' REPORT_FACTS
   and REPORT_ZIP steps (REPORT_ZIP calls buildReport, which item 1 covers).
3. Its own mutants first: an extra tool call, a call moved into a loop and a process started in
   reportLights must each fail, or the check itself is broken.
Prints the call sites inside loops. Exit 0 when all hold, 1 when one doesn't. Stdlib only.
"""
import collections
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
ORACLE = os.path.join(ROOT, "core/tests/swift/oracle/Report.swift")
LIVE = os.path.join(ROOT, "app/Sources/Report.swift")
FACTS = os.path.join(ROOT, "app/Shell/ShellFacts.swift")
STEPS = os.path.join(ROOT, "app/Shell/HostSteps.swift")

CALL = re.compile(r"(?<![\w.])(runTool|run)\(")
SPAWN = re.compile(r"(?<![\w.])(runTool|run|child)\(|\bProcess\(|\bposix_spawn")
LOOP_HEAD = re.compile(r"^\s*(?:for\b|while\b|repeat\b|\}\s*while\b)")
LOOP_CLOSURE = re.compile(r"\.(?:map|forEach|compactMap|flatMap|filter|reduce)\s*(?:\([^)]*\))?\s*$")
STRING = re.compile(r'#*"(?:[^"\\]|\\.)*"#*')


def blank_strings(text):
    """the text with its string literals (and \"\"\" blocks) as spaces, the same length: for braces"""
    out, i = [], 0
    while True:
        j = text.find('"""', i)
        if j < 0:
            break
        k = text.find('"""', j + 3)
        k = len(text) if k < 0 else k + 3
        out.append(text[i:j]); out.append(re.sub(r"[^\n]", " ", text[j:k])); i = k
    out.append(text[i:])
    text = "".join(out)
    return "\n".join(STRING.sub(lambda m: " " * len(m.group(0)), line) for line in text.split("\n"))


def loop_depths(text):
    """for each offset, how many loops (heads or iterating closures) enclose it"""
    b = blank_strings(text)
    stack, depth, line_start = [], [0] * (len(b) + 1), 0
    for i, ch in enumerate(b):
        if ch == "\n":
            line_start = i + 1
        elif ch == "{":
            before = b[line_start:i]
            stack.append(bool(LOOP_HEAD.match(before) and "{" not in before) or bool(LOOP_CLOSURE.search(before)))
        elif ch == "}" and stack:
            stack.pop()
        depth[i] = sum(stack)
    return depth


def calls(text):
    """the runTool(…)/run(…) call texts, each (its text, inside a loop, its line)"""
    depth, found = loop_depths(text), []
    for m in CALL.finditer(text):
        if text[max(0, m.start() - 5):m.start()] == "func ":
            continue                                   # the run(_:_:) helper's own declaration
        i, n = m.end(), 1
        while i < len(text) and n:
            n += {"(": 1, ")": -1}.get(text[i], 0); i += 1
        found.append((" ".join(text[m.start():i].split()), depth[m.start()] > 0, text.count("\n", 0, m.start()) + 1))
    return found


def body(text, start):
    """the text of the brace block that starts at or after `start` (strings left out of the count)"""
    b = blank_strings(text)
    i = b.find("{", start)
    n, j = 0, i
    while j < len(b):
        n += {"{": 1, "}": -1}.get(b[j], 0)
        if n == 0:
            return text[i:j + 1]
        j += 1
    return text[i:]


def func_body(text, name):
    m = re.search(r"\bfunc\s+" + re.escape(name) + r"\s*\(", text)
    return body(text, m.end()) if m else None


def case_body(text, case):
    """a switch case's lines, to the next case or default at its indentation"""
    m = re.search(r"^(\s*)case\s+" + re.escape(case) + r"\s*:", text, re.M)
    if not m:
        return None
    end = re.compile(r"^" + m.group(1) + r"(?:case\s|default\s*:)", re.M).search(text, m.end())
    return text[m.end():end.start() if end else len(text)]


def check(oracle, live, facts, steps, quiet=False):
    """the problems (nothing: all hold)"""
    problems = []
    o, l = calls(oracle), calls(live)
    co = collections.Counter((t, loop) for t, loop, _ in o)
    cl = collections.Counter((t, loop) for t, loop, _ in l)
    for (t, loop), k in sorted((co - cl).items()):
        problems.append(f"Report.swift: {k}x {t}{' (in a loop)' if loop else ''} is the oracle's, not today's")
    for (t, loop), k in sorted((cl - co).items()):
        problems.append(f"Report.swift: {k}x {t}{' (in a loop)' if loop else ''} is today's, not the oracle's")
    for where, text in (("ShellFacts.reportLights", func_body(facts, "reportLights")),
                        ("Report.swift reportMeasure", func_body(live, "reportMeasure")),
                        ("HostSteps CORE_STEP_REPORT_FACTS", case_body(steps, "CORE_STEP_REPORT_FACTS")),
                        ("HostSteps CORE_STEP_REPORT_ZIP", case_body(steps, "CORE_STEP_REPORT_ZIP"))):
        if text is None:
            problems.append(f"{where}: not found (the check reads it by name)")
            continue
        for m in SPAWN.finditer(text):
            problems.append(f"{where}: starts a process ({m.group(0)}…): the report's tools are Report.swift's, compared with 1.3.0's")
    if not quiet:
        for t, _, line in [x for x in l if x[1]]:
            print(f"report-tools: in a loop: app/Sources/Report.swift:{line}: {t}")
        print(f"report-tools: {len(l)} tool calls in Report.swift ({sum(1 for x in l if x[1])} in a loop), the oracle's {len(o)}")
    return problems


def self_test(oracle, live, facts, steps):
    """the check's own mutants: each must give a problem"""
    bad = []
    anchor = 'let verify = runTool("/usr/bin/codesign", ["--verify", "--deep", "--strict", rbApp])'
    if anchor not in live:
        return ["self-test: the codesign call it mutates isn't in Report.swift"]
    mutants = {
        "an extra tool call": (oracle, live.replace(anchor, anchor + '\n    _ = runTool("/usr/bin/codesign", ["--verify", "--deep", "--strict", rbApp])', 1), facts),
        "a call moved into a loop": (oracle, live.replace(anchor, "for _ in 0..<2 {\n    " + anchor + "\n    }", 1), facts),
        "other arguments": (oracle, live.replace('"--last", "30m"', '"--last", "60m"', 1), facts),
        "a process in reportLights": (oracle, live, facts.replace("static func reportLights(_ o: FactSink, _ m: ReportMeasure) {",
                                                                  "static func reportLights(_ o: FactSink, _ m: ReportMeasure) {\n        _ = runTool(\"/usr/bin/true\", [])", 1)),
    }
    for name, (o, l, f) in mutants.items():
        if (o, l, f) == (oracle, live, facts):
            bad.append(f"self-test: the mutant '{name}' changed nothing")
        elif not check(o, l, f, steps, quiet=True):
            bad.append(f"self-test: the mutant '{name}' wasn't caught")
    return bad


def main():
    texts = [open(p, encoding="utf-8").read() for p in (ORACLE, LIVE, FACTS, STEPS)]
    problems = self_test(*texts) + check(*texts)
    for p in problems:
        print("report-tools: " + p)
    print(f"report-tools: {len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""The frozen Swift oracle against the live app (mac-core §8; review SO6).

core/tests/swift/oracle is app/Sources at the switch-over's base (56d7c11 plus freeze-time fixes),
never edited: the core's vectors (gen-vectors.sh) and the texts lint (texts-in-swift.py) read it.
Many of its functions still ship in app/Sources (the watcher's lock and kept-set check, the
report's lights, the parsers, the floor helpers, ...). If one copy of such a function changes and
the other doesn't, the vectors prove parity with code that no longer runs. So: every function
declared in both, by name, must be byte-identical in the live app to one of the oracle's
declarations of that name, unless it is in CHANGED below with the reason it may differ (the
vectors don't call it, or the change is the switch-over's own). A function only the oracle has
(removed with the Swift UI) is the oracle's alone; one only the app has is new.

Run from the repository's root: python3 -I core/tests/oracle-same.py
"""
import os
import re
import sys

ORACLE = "core/tests/swift/oracle"
LIVE = "app/Sources"

# name -> why the live declaration may differ from the oracle's (each reviewed)
CHANGED = {
    "afterExit": "returns whether its shell started (review SO4); no vector calls it",
    "noteShownVersion": "one at a time behind a lock (review SO11); same rule, nextShownVersion is the vectors'",
    "runPrepareHelper": "its track: argument went with the Swift UI's single-track check (the core passes the run's args)",
    "installWatcher": "a Controller step now (HostSteps' LAUNCH_AGENT_LOAD): the window's refresh calls went with the Swift UI; no vector calls it",
    "removeWatcher": "a Controller step now (LAUNCH_AGENT_UNLOAD), returning whether it went; no vector calls it",
    "checkWatcher": "a Controller step now (the launch's LaunchAgent check), without the Swift UI's state; no vector calls it",
    "buildReport": "takes the lights the core decided and REPORT_FACTS' measurement (REPORT_ZIP's arguments); no vector calls it",
    "reportFacts": "the same: its lights are the core's (plusLight and cacheLight, the vectors' own, are the oracle's only now)",
    "lastProblem": "takes a mask applied before each cut (review NS5, owner: mask before cut, both platforms); its default (no mask) is the oracle's behaviour, which the vectors test",
    "stemsQuality": "reads both attributes from one settings element (NW81); host-side, no vector calls it",
    "keptSets": "validates the full kept file before using its date (NW82); host-side, no vector calls it",
    "writeReinstallRequest": "records the watcher's offered features (audit item 15); host-side, no vector calls it",
    "entriesSize": "reports unknown when the Mac cache can't be read (1.5.0); app/Tests covers the host-side walk, no vector calls it",
}
# main's Mac safety fixes (the model-safety and safe-remove reviews, MS/MP/MQ/SR): host-side file
# operations the core reaches only through its steps; no vector calls them
CHANGED.update({f: "a Mac safety fix from main (model-safety, safe-remove reviews): host-side, no vector calls it" for f in (
    "current", "installOurs", "listing", "mayRemove", "migrateLegacy", "modelHelpURL", "modelLeftBehind", "removeEntry",
    "removeInLocation", "removeOwnFolders", "removeThisMacStems", "restore", "safeRemove", "savedOriginals", "stopKeeping",
    "verifiedOriginals")})
CHANGED["saveOriginal"] = "a saved model's engine file that can't be written fails the save; an untagged copy gets its tag (audit b-a2a-F1): host-side, no vector calls it"

# main.swift isn't in the vectors (gen-vectors.sh leaves it out): the app's entry, rewritten at the switch-over
SKIP_FILES = {"main.swift"}

FUNC = re.compile(r"^(\s*)(?:@\w+(?:\([^)]*\))?\s+)*(?:(?:public|private|fileprivate|internal|static|final|override|mutating|class)\s+)*func\s+([A-Za-z_][A-Za-z0-9_]*)")
STRING = re.compile(r'#*"(?:[^"\\]|\\.)*"#*')


def balance(line):
    """{ minus } on one line, string literals and // comments left out."""
    code = STRING.sub('""', line).split("//", 1)[0]
    return code.count("{") - code.count("}")


def declarations(path):
    """Each func in the file: (name, its text from the func line to its closing brace)."""
    lines = open(path, encoding="utf-8").read().split("\n")
    out = []
    i = 0
    while i < len(lines):
        m = FUNC.match(lines[i])
        if not m:
            i += 1
            continue
        indent, name = m.group(1), m.group(2)
        j, depth, opened = i, 0, False
        while j < len(lines):
            depth += balance(lines[j])
            opened = opened or "{" in STRING.sub('""', lines[j]).split("//", 1)[0]
            if opened and depth <= 0:
                break
            # a requirement without a body (a protocol's): its signature only
            if not opened and j > i and (lines[j].strip() == "" or len(lines[j]) - len(lines[j].lstrip()) <= len(indent)):
                j -= 1
                break
            j += 1
        out.append((name, "\n".join(lines[i:j + 1])))
        i = i + 1 if not opened else j + 1
    return out


def collect(folder):
    found = {}
    for f in sorted(os.listdir(folder)):
        if f.endswith(".swift") and f not in SKIP_FILES:
            for name, text in declarations(os.path.join(folder, f)):
                found.setdefault(name, []).append((f, text))
    return found


def main():
    oracle, live = collect(ORACLE), collect(LIVE)
    both = sorted(set(oracle) & set(live))
    bad = []
    for name in both:
        if name in CHANGED:
            continue
        frozen = {t for _, t in oracle[name]}
        for f, text in live[name]:
            if text not in frozen:
                bad.append(f"{LIVE}/{f}: {name} differs from the oracle's (core/tests/swift/oracle): the vectors would test code that no longer runs")
    stale = sorted(n for n in CHANGED if n not in both or all(t in {x for _, x in oracle[n]} for _, t in live[n]))
    for n in stale:
        bad.append(f"oracle-same.py: CHANGED names {n}, which no longer differs (or is no longer in both): remove it")
    for b in bad:
        print(b)
    print(f"oracle-same: {len(both)} functions in both the oracle and app/Sources, {len(both) - len(CHANGED)} identical, "
          f"{len(CHANGED)} changed on purpose; {len(bad)} problem(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

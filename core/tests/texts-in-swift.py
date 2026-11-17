#!/usr/bin/env python3
"""texts.def's Mac column against the Swift app's own words (mac-core §3.9).

The Swift app's words as they were when the core took them over: core/tests/swift/oracle
(app/Sources at the M7 switch-over's base, d005363 = 56d7c11, plus the freeze-time fix 07db3b2,
frozen; not the v1.3.0 tag: 1.4.0's texts are there too; the Swift UI is gone from app/Sources
since).
Every Mac text (texts.def and the tables it #includes) must be one whole string literal of
those sources, its {slots} where that literal has its interpolations (a String(format:) specifier
counts as one), or else follow its designated template below: whole Swift literals (written with
\\(·) for each interpolation, and what the port puts there) and bare {slots}, concatenated, give
the text exactly. So a text can't be a fragment of a literal, and a typo anywhere fails. A text
the port splits from or joins out of Swift's literals ("this Mac" or a quoted drive in a
sentence, a leading-space piece Swift adds with +) is listed there, with the literals it comes
from. A few constants Swift interpolates are put in first (watcherName, ...).

Also lints tip phrases ("Did you know", "Tip:", "Pro tip") in every text: the dialogs rule (texts
say what the action does).

    python3 -I core/tests/texts-in-swift.py        exits 1 and names each text that fails
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SWIFT = "core/tests/swift/oracle"   # app/Sources at the switch-over's base, frozen (the oracle)
CONSTANTS = {"watcherName": "RB Stems Plus Watcher", "keptSetsMax": "5", "removeAnywayButton": "Remove RB Stems Plus Anyway",
             "locationFolderName": "RB Stems Plus Cache.noindex"}
MARK = "\x01"
ESC = {"n": "\n", "t": "\t", "r": "\r", "0": "\0", '"': '"', "\\": "\\", "'": "'"}
FORMAT = re.compile(r"%(?:\d+|\.\d+)?(?:lld|ld|d|f|@|s)")
SLOT = re.compile(r"\{[a-z_]+\}")
I = "\\(·)"   # an interpolation, in the templates below

# The texts that aren't one whole literal: each a list of parts, either a bare "{slot}" or a
# Swift literal (I for each interpolation) with what goes in each interpolation, in order.
DESIGNATED = {
    # Swift: "Reinstall " + names(...)  (Controller.swift refresh, Actions.swift reinstallLabel)
    "T_REINSTALL_FEATURES": [("Reinstall ", []), "{features}"],
    # confirm(action, ...): alert(action + "?", ...)
    "T_CONFIRM": ["{action}", ("?", [])],
    # "Stop and \(verb)" with the verb each caller passes
    "T_STOP_AND_CONTINUE": [("Stop and " + I, ["Continue"])],
    "T_STOP_AND_UPDATE": [("Stop and " + I, ["Update"])],
    "T_STOP_AND_QUIT": [("Stop and " + I, ["Quit"])],
    "T_STOP_PREPARING_TITLE": [("“" + I + "” is being prepared", ["{playlist}"])],
    # "... yet." + checksAgain
    "T_PLUS_ENGINE_UNLISTED": [("Demucs v4 isn't available for STEMS Engine " + I + " yet.", ["{engine}"])],
    "T_CACHE_RB_UNLISTED": [("Stems Cache doesn't support rekordbox " + I + " yet.", ["{rekordbox}"])],
    "T_CACHE_ORT_UNLISTED": [("Stems Cache doesn't support ONNX Runtime " + I + " yet.", ["{ort}"])],
    "T_PREP_RB_UNLISTED": [("Preparing doesn't support rekordbox " + I + " yet.", ["{rekordbox}"])],
    # cachePausedTip / cachePausedAtInstall: drive.map { "\"\($0)\"" } ?? "your disk"
    "T_CACHE_PAUSED": [("Stems Cache stopped saving stems: " + I + " has less than " + I + " GB free.", ['"{drive}"', "{gb}"])],
    "T_CACHE_PAUSED_DISK": [("Stems Cache stopped saving stems: " + I + " has less than " + I + " GB free.", ["your disk", "{gb}"])],
    "T_CACHE_PAUSED_AT_INSTALL": [("Stems Cache will stop saving stems while " + I + " has less than " + I + " GB free.", ['"{drive}"', "{gb}"])],
    "T_CACHE_PAUSED_AT_INSTALL_DISK": [("Stems Cache will stop saving stems while " + I + " has less than " + I + " GB free.", ["your disk", "{gb}"])],
    # locationLight: drive = b.location.isEmpty ? "this Mac" : "\"\(name)\""
    "T_LOC_CHECKING_DRIVE": [("Stems Cache is still checking " + I + ".", ['"{drive}"'])],
    "T_LOC_CHECKING_THIS_MAC": [("Stems Cache is still checking " + I + ".", ["this Mac"])],
    "T_LOC_AGAIN_SOON_DRIVE": [("Stems Cache will use " + I + " again in a few seconds.", ['"{drive}"'])],
    "T_LOC_AGAIN_SOON_THIS_MAC": [("Stems Cache will use " + I + " again in a few seconds.", ["this Mac"])],
    "T_LOC_NOT_USING_DRIVE": [("Stems Cache isn't using " + I + " right now: ", ['"{drive}"']), "{why}"],
    "T_LOC_NOT_USING_THIS_MAC": [("Stems Cache isn't using " + I + " right now: ", ["this Mac"]), "{why}"],
    "T_LOC_NOT_CONNECTED": [("Stems Cache's drive " + I + " isn't connected: rekordbox separates tracks as usual. " + I
                             + " Renamed the drive, or does it show as \"" + I + " 1\"? Choose it again in Settings: the stems on it are kept.",
                             ['"{drive}"', "{connect}", "{drive}"])],
    "T_LOC_CONNECT_OPEN": [("Connect it, then " + I + " rekordbox.", ["open"])],
    "T_LOC_CONNECT_RESTART": [("Connect it, then " + I + " rekordbox.", ["restart"])],
    "T_LOC_FOLDER_MISSING": [("Stems Cache's folder on " + I + " is missing or was replaced.", ['"{drive}"'])],
    "T_LOC_LINK": [("Stems Cache isn't saving: a folder on the way to Stems Cache's folder on " + I + " is a link.", ['"{drive}"'])],
    "T_LOC_NETWORK": [("Stems Cache isn't saving: " + I + " is a network drive. Choose a drive connected to this Mac in Settings.", ['"{drive}"'])],
    "T_LOC_READ_ONLY": [(I + " is read-only; macOS may need to repair it in Disk Utility.", ['"{drive}"'])],
    "T_LOC_NO_PERMISSION": [("RB Stems Plus isn't allowed to use " + I + ". Allow it in System Settings › Privacy & Security › Files and Folders › RB Stems Plus.",
                             ['"{drive}"'])],
    # durationText: m < 60 ? "\(m) min" : String(format: "%d h %02d min", ...)
    "T_DUR_MIN": [(I + " min", ["{n}"])],
    # aboutSize, sizeText, sizeWords: String(format: " (about %.1f GB)"), "%.1f GB", "\(n) MB"
    "T_SIZE_MB": [(I + " MB", ["{n}"])],
    # estimateSummary: "\(n) \(n == 1 ? "track" : "tracks") ready", and the like
    "T_EST_READY_ONE": [(I + " " + I + " ready", ["{n}", "track"])],
    "T_EST_READY_MANY": [(I + " " + I + " ready", ["{n}", "tracks"])],
    "T_EST_ABOUT": [(" About " + I + " and " + I + ".", ["{size}", "{time}"])],
    "T_FIT_FIRST_ONE": [("Prepare the First " + I + " " + I, ["{n}", "Track"])],
    "T_FIT_FIRST_MANY": [("Prepare the First " + I + " " + I, ["{n}", "Tracks"])],
    "T_FIT_MSG": [("Prepared playlists may use 75% of Stems Cache's " + I + " GB limit (" + I
                   + "), the rest stays for the tracks rekordbox separates during a set. This playlist would take them to " + I + ".",
                   ["{limit}", "{share}", "{size}"])],
    # setState: "\(e.prepare) \(e.prepare == 1 ? "track" : "tracks") to prepare"
    "T_SET_TO_PREPARE_ONE": [(I + " " + I + " to prepare", ["{n}", "track"])],
    "T_SET_TO_PREPARE_MANY": [(I + " " + I + " to prepare", ["{n}", "tracks"])],
    # lastRunLine and the run's phase: "\(files) \(files == 1 ? "track" : "tracks") done"
    "T_LAST_ONE": [("Last time: " + I + " " + I + " done", ["{n}", "track"])],
    "T_LAST_MANY": [("Last time: " + I + " " + I + " done", ["{n}", "tracks"])],
    "T_RUN_DONE_ONE": [(I + " " + I + " done", ["{n}", "track"])],
    "T_RUN_DONE_MANY": [(I + " " + I + " done", ["{n}", "tracks"])],
    "T_RUN_PREPARING": [(I + " “" + I + "”: " + I + "%", ["Preparing", "{playlist}", "{n}"])],
    "T_RUN_PAUSED": [(I + " “" + I + "”: " + I + "%", ["Paused", "{playlist}", "{n}"])],
    "T_RUN_TRACK_OF": [("Track " + I + " of " + I + ".", ["{n}", "{m}"])],
    "T_RUN_USE": [(". Prepared playlists use " + I + ".", ["{size}"])],
    # keptSetNotice: "\(n) \(n == 1 ? "track isn't" : "tracks aren't") fully prepared: ... \(n == 1 ? "it" : "them") ..."
    "T_W_KEPT_ONE": [(I + " " + I + " fully prepared: rekordbox may separate " + I + " as you play. Open RB Stems Plus to see why and prepare it again.",
                      ["{n}", "track isn't", "it"])],
    "T_W_KEPT_MANY": [(I + " " + I + " fully prepared: rekordbox may separate " + I + " as you play. Open RB Stems Plus to see why and prepare it again.",
                       ["{n}", "tracks aren't", "them"])],
    # watcherDialog: "It " + (modelOff ? "isn't available for ..." : rekordboxLine + ".")
    "T_W_IT_ENGINE": [("It ", []), ("isn't available for STEMS Engine " + I + " yet.", ["{engine}"])],
    "T_W_IT_REKORDBOX": [("It ", []), ("doesn't support rekordbox " + I + " yet", ["{rekordbox}"]), (".", [])],
    "T_W_BOTH_BLOCKED": [("Demucs v4 isn't available for STEMS Engine " + I + " yet, and Stems Cache " + I + ".",
                          ["{engine}", "doesn't support rekordbox {rekordbox} yet"])],
    "T_W_BOTH_CACHE_BLOCKED": [("Demucs v4 and Stems Cache are off. Reinstall Demucs v4 to separate tracks with it. Stems Cache " + I
                                + ", so it stays off for now.", ["doesn't support rekordbox {rekordbox} yet"])],
    "T_W_CACHE_OFF_PLUS_WORKS": [("Stems Cache is off" + I + ". Reinstall it to reuse the stems it has saved.", [" (Demucs v4 still works)"])],
    "T_W_CACHE_OFF": [("Stems Cache is off" + I + ". Reinstall it to reuse the stems it has saved.", [""])],
    # start-up: "\(appLock.replacingOccurrences(...)) can't be used (\(why)). ..."
    "T_START_NOT_FOLDER": [(I + " isn't a folder. RB Stems Plus uses only a real folder there: move it to the Trash, then open RB Stems Plus again.", ["{path}"])],
    # runRoot: failure + " " + why
    "T_FAILURE_WHY": ["{message}", (" ", []), "{why}"],
    # install: "Demucs v4 couldn't be \(done). \(r.message) Nothing was changed." with done = "installed" / "reinstalled"
    "T_PLUS_NOT_DONE_INSTALLED": [("Demucs v4 couldn't be " + I + ". " + I + " Nothing was changed.", ["installed", "{message}"])],
    "T_PLUS_NOT_DONE_REINSTALLED": [("Demucs v4 couldn't be " + I + ". " + I + " Nothing was changed.", ["reinstalled", "{message}"])],
    "T_CACHE_NOT_INSTALLED": [("Stems Cache couldn't be " + I + ".", ["installed"])],
    "T_CACHE_NOT_REINSTALLED": [("Stems Cache couldn't be " + I + ".", ["reinstalled"])],
    "T_CACHE_NOT_INSTALLED_LOG": [("Stems Cache couldn't be " + I + ". See the log in this window.", ["installed"])],
    "T_CACHE_NOT_REINSTALLED_LOG": [("Stems Cache couldn't be " + I + ". See the log in this window.", ["reinstalled"])],
    "T_UNINSTALL_STOPPED_CACHE": [("Stems Cache couldn't be removed, so nothing else was deleted. " + I, ["{why}"])],
    # removeAnywayChoice: "\(r.message) Nothing was deleted.\n\n\(removeAnywayButton) deletes only its own files" + stemsNote + ...
    "T_ANYWAY_INTRO": [(I + " Nothing was deleted.\n\nRemove RB Stems Plus Anyway deletes only its own files", ["{message}"])],
    "T_ANYWAY_KEEPS_SAVED": [(" and keeps its saved copy of rekordbox's own stems model. ", [])],
    "T_ANYWAY_GET_BACK": [("To get rekordbox's own stems model back, click Open Help.", [])],
    "T_ANYWAY_ABOUT_KEPT": [(I + " The saved copy of rekordbox's own stems model stays in " + I + ". To put it back, click Open Help.", ["{message}", "{dir}"])],
    "T_ANYWAY_ABOUT": [(I + " To get rekordbox's own stems model back, click Open Help.", ["{message}"])],
    # installNote: "Installs the latest version of " + (...) + " again."
    "T_INSTALL_NOTE_REINSTALL": [("Installs the latest version of ", []), "{features}", (" again.", [])],
    # uninstallEverythingMessage: about + " Your music and rekordbox library aren't touched.\n\n" + deleted
    "T_UNINSTALL_ALL_MSG": ["{message}", (" Your music and rekordbox library aren't touched.\n\n", []), "{why}"],
    # Settings: "Clear Saved Stems" + size; confirm("Clear saved stems" + size, ...)
    "T_SETTINGS_CLEAR": [("Clear Saved Stems", []), "{size}"],
    "T_CLEAR_Q": [("Clear saved stems", []), "{size}"],
    # locationLabel: "Stems Cache location: " + parts.joined(" › ")
    "T_LOCATION_LABEL": [("Stems Cache location: ", []), "{path}"],
    # LocationActions: "Found your saved stems on \"\(drive)\"\(size > 0 ? " (\(sizeWords(size)))" : "")."
    "T_FOUND_MSG": [("Found your saved stems on \"" + I + "\"" + I + ".", ["{drive}", ""])],
    "T_FOUND_SIZE_MSG": [("Found your saved stems on \"" + I + "\"" + I + ".", ["{drive}", " ({size})"])],
    # updateFailedMessage: "RB Stems Plus wasn't changed." + (reason.map { " " + $0 } ?? "") + " If macOS ..."
    "T_UPDATE_FAILED_MSG": [("RB Stems Plus wasn't changed.", []), "{why}",
                            (" If macOS said RB Stems Plus was prevented from modifying apps, allow it, then choose Update RB Stems Plus again. Otherwise try again later.", [])],
    "T_UPDATE_R_APP": [(" ", []), ("RB Stems Plus was moved or changed while it was updating.", [])],
    "T_UPDATE_R_DOWNLOAD": [(" ", []), ("The download wasn't a valid copy of RB Stems Plus.", [])],
    "T_UPDATE_R_STAGING": [(" ", []), ("Something else is in the way next to RB Stems Plus (" + I + ").", ["{name}"])],
    "T_UPDATE_R_COPY": [(" ", []), ("The new version couldn't be copied next to RB Stems Plus.", [])],
    "T_UPDATE_R_COPY_CHECK": [(" ", []), ("The copy of the new version didn't check out.", [])],
    "T_UPDATE_R_SIGN": [(" ", []), ("The new version couldn't be signed.", [])],
    "T_UPDATE_R_SWAP": [(" ", []), ("The new version couldn't take this one's place.", [])],
    # fetchAsset's status lines, with what = "the Demucs v4 model", "Stems Cache", "RB Stems Plus \(version)"
    "T_WHAT_APP": [("RB Stems Plus " + I, ["{version}"])],
    "T_DOWNLOADING_WHAT_PCT": [("Downloading " + I + "… " + I + "%", ["{name}", "{n}"])],
    # the About panel's credits: the home page's text, then this
    "T_ABOUT_CREDITS": [("\n\nFree and open source (MIT licence).\n\nNot made or supported by\nAlphaTheta / Pioneer DJ.", [])],
    # the location's refusal names a drive as "\"\(drive)\""
    "T_REFUSE_READ_ONLY": [("\"" + I + "\" is read-only.", ["{drive}"])],
}


# Mac texts that aren't 1.3.0's words, each by an owner decision (the reason given): not looked
# for in the oracle, still linted. Each must be a text.
NEW = {
    # a SELF_DELETE that couldn't start leaves the app open: one line instead of silence (2026-11-12)
    "T_SELF_DELETE_FAILED": "owner decided 2026-11-12",
}


def literals(src):
    """Every string literal's shape: its text with each interpolation as MARK, nested literals
    (inside interpolations) included."""
    shapes = []
    i, n = 0, len(src)

    def string_at(i, raw_hashes, multi):
        """Parses the literal starting after its opening delimiter; returns (end, segments)."""
        parts, cur = [], []
        close = ('"""' if multi else '"') + "#" * raw_hashes
        while i < n:
            if src.startswith(close, i):
                parts.append("".join(cur))
                return i + len(close), parts
            c = src[i]
            esc_lead = "\\" + "#" * raw_hashes
            if src.startswith(esc_lead, i):
                j = i + len(esc_lead)
                e = src[j] if j < n else ""
                if e == "(":
                    # an interpolation: up to its matching ")", strings inside it scanned too
                    depth, k = 1, j + 1
                    start = k
                    while k < n and depth:
                        if src[k] == '"':
                            k = scan_string(k)
                            continue
                        depth += {"(": 1, ")": -1}.get(src[k], 0)
                        k += 1
                    expr = src[start:k - 1].strip()
                    if expr in CONSTANTS:
                        cur.append(CONSTANTS[expr])
                    else:
                        parts.append("".join(cur))
                        cur = []
                    i = k
                    continue
                if e == "u" and j + 1 < n and src[j + 1] == "{":
                    k = src.index("}", j)
                    cur.append(chr(int(src[j + 2:k], 16)))
                    i = k + 1
                    continue
                if multi and e == "\n":   # a line continued
                    i = j + 1
                    continue
                cur.append(ESC.get(e, e))
                i = j + 1
                continue
            cur.append(c)
            i += 1
        return i, parts

    def scan_string(i):
        """i at a '"' (or '#'): records the literal's shape, returns the index after it."""
        hashes = 0
        while src[i] == "#":
            hashes += 1
            i += 1
        multi = src.startswith('"""', i)
        i += 3 if multi else 1
        end, parts = string_at(i, hashes, multi)
        if multi:
            # the closing line's indentation comes off every line; the first newline goes
            text = "\x00".join(parts)
            lines = text.split("\n")
            indent = len(lines[-1]) - len(lines[-1].lstrip(" "))
            body = "\n".join(l[indent:] if l.startswith(" " * indent) else l.lstrip(" ") for l in lines[1:-1])
            parts = body.split("\x00")
        shapes.append(MARK.join(parts))
        return end

    while i < n:
        if src.startswith("//", i):
            i = src.find("\n", i)
            if i < 0:
                break
            continue
        if src.startswith("/*", i):
            i = src.find("*/", i) + 2
            continue
        if src[i] == '"' or (src[i] == "#" and re.match(r'#+"', src[i:])):
            i = scan_string(i)
            continue
        i += 1
    return shapes


def rows_of(path, seen=()):
    """The TEXT rows of a .def table and of the tables it #includes."""
    out = []
    for line in open(path, encoding="utf-8").read().split("\n"):
        m = re.match(r'\s*#\s*include\s+"([^"]+\.def)"', line)
        if m:
            inc = os.path.join(os.path.dirname(path), m.group(1))
            if inc not in seen:
                out += rows_of(inc, seen + (path,))
            continue
        m = re.match(r'TEXT\((\w+),\s*(\w+),\s*"((?:[^"\\]|\\.)*)",\s*"((?:[^"\\]|\\.)*)"\)', line)
        if m:
            out.append(m.groups())
    return out


def designated(value, parts, shapes):
    """None when the template's literals are the oracle's and give value exactly, else why not."""
    built = ""
    for p in parts:
        if isinstance(p, str):
            if not SLOT.fullmatch(p):
                return f"has a bare part {p!r} that isn't a slot"
            built += p
            continue
        lit, fills = p
        if lit.count(I) != len(fills):
            return f"template {lit!r}: {lit.count(I)} interpolations, {len(fills)} fills"
        if lit.replace(I, MARK) not in shapes:
            return f"template {lit!r} isn't a whole literal of the oracle"
        pieces = lit.split(I)
        built += pieces[0] + "".join(f + s for f, s in zip(fills, pieces[1:]))
    return None if built == value else f"isn't what its template gives ({built!r})"


def main():
    shapes = set()
    for f in sorted(glob.glob(os.path.join(ROOT, SWIFT, "*.swift"))):
        for lit in literals(open(f, encoding="utf-8").read()):
            shapes.add(FORMAT.sub(MARK, lit))
    rows = rows_of(os.path.join(ROOT, "core/texts.def"))
    bad = 0
    for tid, _, mac, _ in rows:
        value = bytes(mac, "utf-8").decode("unicode_escape").encode("latin-1").decode("utf-8")
        if tid in NEW:
            why = None
        elif tid in DESIGNATED:
            why = designated(value, DESIGNATED[tid], shapes)
        else:
            why = None if SLOT.sub(MARK, value) in shapes else "isn't a whole literal of the oracle (slots as its interpolations)"
        if why:
            print(f"texts-in-swift: {tid}: {value!r} {why}")
            bad += 1
        if re.search(r"Did you know|Tip:|Pro tip", value):
            print(f"texts-in-swift: {tid}: a tip phrase in a text")
            bad += 1
    for tid in sorted((set(DESIGNATED) | set(NEW)) - {r[0] for r in rows}):
        print(f"texts-in-swift: {tid}: designated or new, but not a text")
        bad += 1
    if bad:
        sys.exit(1)
    print(f"texts-in-swift: {len(rows)} Mac texts: {len(rows) - len(DESIGNATED) - len(NEW)} whole literals of the oracle, "
          f"{len(DESIGNATED)} from their designated literals, {len(NEW)} new by an owner decision")


main()

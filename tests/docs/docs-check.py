#!/usr/bin/env python3
"""Documentation checks for make check (tests/docs/docs-check.sh).

1. Relative links in docs/ and the top-level .md files resolve: inline links, reference-style
   links (every [text][label] has a "[label]: target" definition, and its target resolves) and
   #anchors into Markdown pages (GitHub's heading slugs, or an <a id/name>). So do the links in
   the Claude memory index (MEMORY.md) when that folder is on this Mac.
2. Pages that carry a docs-check block (an HTML comment starting "<!-- docs-check") keep their
   rules: "required: <heading line>", "ids: <prefix> ..." (an ID is the first cell of a table row
   starting with a prefix; no ID twice, in one page or across pages), "max-lines: N",
   "files: <path> ..." (pages that must exist next to this one and carry a block too),
   "private-names: all" (every file of this page's private folder counts as a private name, see 4),
   "fact: seat N = NAME" (seat N is held by NAME: the page's own table row for N must name it, and
   every seat row there needs a fact; on any other page each "seat N" claim must name N's holder,
   clause by clause, and N must be a known seat). At least one page with a "files:" list must sit in
   a private folder of .publicignore. Every ID that any page in docs/ or at the top mentions with a
   declared prefix (and a digit), in code spans and blocks too, must be defined in an ID table.
3. No private values: every page with a block, and every public document, goes through
   scripts/public-scan.py with .publicallow, as the release export does (locations and types only).
4. No public file names a private path as release.sh refuses it (a private folder by its path, a
   private file by its name and its capitals stem), nor a capitals-named file inside a private folder,
   nor any file of a folder whose page says "private-names: all" (by its bare file name too).

Diagnostics name files, lines and the kind of problem; values found by the privacy scan are never
printed. Usage: docs-check.py ROOT [--selftest]
"""

import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
from urllib.parse import unquote

LINK = re.compile(r"!?\[(?:[^\]\[]|\[[^\]]*\])*\]\(\s*<?([^)\s>]+)>?(?:\s+\"[^\"]*\")?\s*\)")
FENCE = re.compile(r"^\s*(```|~~~)")
CODE_SPAN = re.compile(r"`+[^`]*`+")
BLOCK = re.compile(r"<!--\s*docs-check\b(.*?)-->", re.S)
REF_DEF = re.compile(r"^\s{0,3}\[([^\]]+)\]:\s*<?([^\s>]+)>?")
REF_USE = re.compile(r"\[((?:[^\]\[]|\[[^\]]*\])+)\]\[([^\]]*)\]")
HEADING = re.compile(r"^\s{0,3}#{1,6}\s+(.*?)\s*#*\s*$")
HTML_ANCHOR = re.compile(r"<a\s[^>]*(?:id|name)\s*=\s*[\"']([^\"']+)[\"']", re.I)
WORD = r"[A-Za-z0-9_]"


def git_files(root):
    out = subprocess.run(["git", "-C", str(root), "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=True).stdout
    return sorted({p for p in out.decode("utf-8", "replace").split("\0") if p and (root / p).is_file()})


def private_paths(root):
    path = root / ".publicignore"
    if not path.is_file():
        return []
    result = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip().rstrip("/")
        if line and not line.startswith("#"):
            result.append(line)
    return result


def is_private(rel, private):
    return any(rel == p or rel.startswith(p + "/") for p in private)


def text_lines(path):
    data = path.read_bytes()
    if b"\0" in data:
        return None
    return data.decode("utf-8", "replace").splitlines()


def prose(lines):
    """(line number, line) outside fenced code, each code span replaced by a word: a link inside
    backticks isn't a link, but a code-formatted link label still is one."""
    fenced = False
    for number, line in enumerate(lines, 1):
        if FENCE.match(line):
            fenced = not fenced
            continue
        if not fenced:
            yield number, CODE_SPAN.sub("code", line)


def slug(text):
    text = re.sub(r"`([^`]*)`", r"\1", text)
    text = re.sub(r"!?\[([^\]]*)\]\([^)]*\)", r"\1", text)
    text = re.sub(r"<[^>]+>", "", text).strip().lower()
    return re.sub(r"[^\w\- ]", "", text).replace(" ", "-")


def anchors(path, cache={}):
    if path not in cache:
        found, counts = set(), {}
        lines = text_lines(path) or []
        fenced = False
        for line in lines:
            if FENCE.match(line):
                fenced = not fenced
                continue
            if fenced:
                continue
            found.update(m.group(1) for m in HTML_ANCHOR.finditer(line))
            m = HEADING.match(line)
            if m:
                base = slug(m.group(1))
                n = counts.get(base, 0)
                counts[base] = n + 1
                found.add(base if n == 0 else f"{base}-{n}")
        cache[path] = found
    return cache[path]


def check_target(root, path, number, target, problems, label):
    where = f"{label or path.relative_to(root)}:{number}"
    if re.match(r"^[a-z][a-z0-9+.-]*:", target, re.I):
        return
    file_part, _, frag = target.partition("#")
    file_part = unquote(file_part)
    dest = path if not file_part else path.parent / file_part
    if file_part and not dest.exists():
        problems.append(f"{where}: broken link: {file_part}")
        return
    if frag and dest.is_file() and dest.suffix == ".md" and unquote(frag).lower() not in anchors(dest.resolve()):
        problems.append(f"{where}: missing anchor: {target}")


def check_links(root, path, problems, label=None):
    lines = text_lines(path)
    if lines is None:
        return
    defs, uses = {}, []
    for number, line in prose(lines):
        d = REF_DEF.match(line)
        if d:
            defs[d.group(1).strip().lower()] = (number, d.group(2))
            continue
        for m in LINK.finditer(line):
            check_target(root, path, number, m.group(1), problems, label)
        for m in REF_USE.finditer(line):
            uses.append((number, (m.group(2) or m.group(1)).strip().lower()))
    for number, ref in uses:
        if ref not in defs:
            problems.append(f"{label or path.relative_to(root)}:{number}: undefined reference link: [{ref}]")
    for ref, (number, target) in defs.items():
        check_target(root, path, number, target, problems, label)


def blocks(lines):
    text = "\n".join(lines)
    rules = {"required": [], "ids": [], "max-lines": [], "files": [], "private-names": [], "fact": []}
    found = False
    for m in BLOCK.finditer(text):
        found = True
        for raw in m.group(1).splitlines():
            key, sep, value = raw.strip().partition(":")
            if sep and key.strip() in rules:
                rules[key.strip()].append(value.strip())
    return rules if found else None


def table_ids(lines, prefixes):
    for number, line in enumerate(lines, 1):
        if not line.lstrip().startswith("|"):
            continue
        cell = line.strip().strip("|").split("|", 1)[0].strip().strip("*").strip()
        for prefix in prefixes:
            if cell.startswith(prefix) and re.fullmatch(re.escape(prefix) + r"[A-Za-z0-9.-]+", cell):
                yield number, prefix, cell


def check(root, memory=None):
    root = Path(root).resolve()
    problems = []
    files = git_files(root)
    private = private_paths(root)
    docs = [p for p in files if p.endswith(".md") and (p.startswith("docs/") or "/" not in p)]

    # 1. links
    for rel in docs:
        check_links(root, root / rel, problems)
    if memory is not None:
        index = Path(memory) / "MEMORY.md"
        if index.is_file():
            check_links(root, index, problems, label="memory/MEMORY.md")
        else:
            print("docs-check: no memory index on this Mac, skipped")

    # 2. page rules
    marked = {}
    for rel in files:
        if not rel.endswith(".md"):
            continue
        lines = text_lines(root / rel)
        rules = blocks(lines) if lines is not None else None
        if rules is not None:
            marked[rel] = (lines, rules)
    seen = {}
    manifest_in_private = False
    for rel, (lines, rules) in sorted(marked.items()):
        heads = {line.rstrip() for line in lines}
        for heading in rules["required"]:
            if heading not in heads:
                problems.append(f"{rel}: missing required heading: {heading}")
        for value in rules["max-lines"]:
            if value.isdecimal() and len(lines) > int(value):
                problems.append(f"{rel}: {len(lines)} lines, more than {value}")
        prefixes = [p for value in rules["ids"] for p in value.split()]
        for number, prefix, ident in table_ids(lines, prefixes):
            if (prefix, ident) in seen:
                problems.append(f"{rel}:{number}: duplicate ID {ident} (first at {seen[prefix, ident]})")
            else:
                seen[prefix, ident] = f"{rel}:{number}"
        for value in rules["files"]:
            if is_private(rel, private):
                manifest_in_private = True
            for name in value.split():
                other = os.path.normpath(os.path.join(os.path.dirname(rel), name))
                if other not in marked:
                    problems.append(f"{rel}: listed page {name} is missing or has no docs-check block")
    if any((root / p).is_dir() for p in private) and not manifest_in_private:
        problems.append(".publicignore: no private folder holds a docs-check page with a files: list")

    # 2b. every mentioned ID is defined
    all_prefixes = {prefix for prefix, _ in seen}
    defined = {ident for _, ident in seen}
    if all_prefixes:
        ref = re.compile("(?<![\\w-])(?:" + "|".join(re.escape(p) for p in sorted(all_prefixes)) + ")[A-Za-z0-9]+(?:[.-][A-Za-z0-9]+)*")
        pages = {rel: lines for rel, (lines, _) in marked.items()}
        for rel in docs:
            if rel not in pages:
                pages[rel] = text_lines(root / rel) or []
        for rel, lines in sorted(pages.items()):   # every page, code spans and blocks included
            for number, line in enumerate(lines, 1):
                for m in ref.finditer(line):
                    ident = m.group(0)
                    if any(c.isdigit() for c in ident) and ident not in defined:
                        problems.append(f"{rel}:{number}: ID {ident} is mentioned but defined in no ID table")

    # 2c. seat facts: one holder per seat, everywhere
    holders = {}
    for rel, (lines, rules) in sorted(marked.items()):
        for value in rules["fact"]:
            m = re.fullmatch(r"seat\s+(\d+)\s*=\s*(\S.*)", value)
            if not m:
                problems.append(f"{rel}: unreadable fact: {value}")
                continue
            seat, name = m.group(1), m.group(2).strip()
            holders[seat] = (name, rel)
            row = [l for l in lines if re.match(r"^\|\s*" + seat + r"\s*\|", l)]
            if not row or not re.search(r"(?<![\w-])" + re.escape(name) + r"(?![\w-])", row[0]):
                problems.append(f"{rel}: the seat table's row {seat} doesn't name {name} (fact: seat {seat} = {name})")
    if holders:
        def named_in(text):
            return [n for n, _ in holders.values() if re.search(r"(?<![\w-])" + re.escape(n) + r"(?![\w-])", text)]
        for rel, (lines, rules) in sorted(marked.items()):
            if rules["fact"]:
                for number, line in enumerate(lines, 1):
                    m = re.match(r"^\|\s*(\d+)\s*\|", line)
                    if m and m.group(1) not in holders:
                        problems.append(f"{rel}:{number}: seat table row {m.group(1)} has no fact: line")
                continue
            for number, line in prose(lines):
                hits = list(re.finditer(r"\bseat\s+(\d+)\b", line, re.I))
                for i, m in enumerate(hits):
                    seat = m.group(1)
                    if seat not in holders:
                        problems.append(f"{rel}:{number}: names seat {seat}, which no fact: line defines")
                        continue
                    # the clause of this claim: from the previous claim's end to the next claim's start
                    clause = line[hits[i - 1].end() if i else 0:hits[i + 1].start() if i + 1 < len(hits) else len(line)]
                    named = named_in(clause)
                    holder, home = holders[seat]
                    if not named:
                        problems.append(f"{rel}:{number}: says seat {seat} without naming its holder {holder} ({home}): link the seat table instead")
                    elif holder not in named:
                        problems.append(f"{rel}:{number}: says seat {seat} for {', '.join(named)}; {home} gives it to {holder}")

    # 3. private values
    public_docs = [p for p in files if not is_private(p, private) and p.endswith((".md", ".txt"))
                   and (p.startswith("docs/") or "/" not in p)]
    scan_set = sorted(set(marked) | set(public_docs))
    with tempfile.TemporaryDirectory() as tmp:
        stage = Path(tmp) / "stage"
        for rel in scan_set:
            dest = stage / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(root / rel, dest)
        stage.mkdir(exist_ok=True)
        allow = root / ".publicallow"
        if not allow.is_file():
            allow = Path(tmp) / "empty-allow"
            allow.write_text("", encoding="utf-8")
        scan = subprocess.run([sys.executable, "-I", str(root / "scripts/public-scan.py"), str(stage), str(allow), str(root)],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if scan.returncode == 1:
            problems.extend("private value: " + line for line in scan.stderr.splitlines() if line.strip())
        elif scan.returncode != 0:
            problems.append("private value scan could not run")

    # 4. private names in public files
    names = []
    for p in private:
        if (root / p).is_dir():
            names.append(p)
            for sub in (root / p).rglob("*"):
                stem = sub.stem
                if sub.is_file() and len(stem) >= 5 and not re.search("[a-z]", stem):
                    names.append(sub.name)
        else:
            b = os.path.basename(p)
            names.append(b)
            stem = b.rsplit(".", 1)[0]
            if stem != b and len(stem) >= 5 and not re.search("[a-z]", stem):
                names.append(stem)
    strict = set()
    for rel, (lines, rules) in marked.items():
        if "all" in rules["private-names"]:
            top = next((p for p in private if rel.startswith(p + "/")), None)
            if top and (root / top).is_dir():
                strict.update(sub.name for sub in (root / top).rglob("*") if sub.is_file())
    alternatives = [f"(?<!{WORD}){re.escape(n)}(?!{WORD})" for n in sorted(set(names), key=len, reverse=True)]
    alternatives += [f"(?<![\\w.-]){re.escape(n)}(?![\\w-])" for n in sorted(strict, key=len, reverse=True)]
    if alternatives:
        pattern = re.compile("|".join(alternatives))
        for rel in files:
            if is_private(rel, private) or rel in (".publicignore", ".publicallow"):
                continue
            lines = text_lines(root / rel)
            for number, line in enumerate(lines or [], 1):
                m = pattern.search(line)
                if m:
                    problems.append(f"{rel}:{number}: names the private {m.group(0)} (release.sh would refuse)")
    return problems


def memory_dir(root):
    override = os.environ.get("DOCS_CHECK_MEMORY")
    if override is not None:
        return override or None
    common = subprocess.run(["git", "-C", str(root), "rev-parse", "--path-format=absolute", "--git-common-dir"],
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True).stdout.strip()
    main = Path(common).parent if common else Path(root)
    return str(Path.home() / ".claude" / "projects" / re.sub(r"[^A-Za-z0-9]", "-", str(main)) / "memory")


def selftest(root):
    """Plant one problem of each kind in a scratch repository and expect each to be reported."""
    root = Path(root).resolve()
    ok = True
    lan_address = ".".join(["192", "168", "1", "20"])   # built here, so this file holds no address the export scan flags
    with tempfile.TemporaryDirectory() as tmp:
        t = Path(tmp) / "repo"
        (t / "scripts").mkdir(parents=True)
        shutil.copyfile(root / "scripts/public-scan.py", t / "scripts/public-scan.py")
        (t / "docs/notes").mkdir(parents=True)
        (t / ".publicignore").write_text("docs/notes\n", encoding="utf-8")
        (t / ".publicallow").write_text("", encoding="utf-8")
        block = "<!-- docs-check\nrequired: ## Rules\nids: X-\nmax-lines: 30\n-->\n"
        (t / "docs/notes/TOPICS.md").write_text("# Home\n<!-- docs-check\nprivate-names: all\nfiles: a.md b.md\n-->\n[a](a.md#rules)\n", encoding="utf-8")
        (t / "docs/notes/g1-card.md").write_text("an old card\n", encoding="utf-8")
        (t / "docs/notes/a.md").write_text(f"# A\n{block}## Rules\n| X-1 | one, see X-3 |\n| X-2 | two |\nalpha-box holds seat 1.\n", encoding="utf-8")
        seats = "<!-- docs-check\nrequired: ## Rules\nids: X-\nfact: seat 1 = alpha-box\nfact: seat 2 = beta-box\n-->\n"
        (t / "docs/notes/b.md").write_text(f"# B\n{seats}## Rules\n| X-3 | three |\n\n| Seat | Machine |\n|---|---|\n| 1 | alpha-box |\n| 2 | beta-box |\n", encoding="utf-8")
        (t / "README.md").write_text("# Readme\n[guide](docs/guide.md#guide) `[x](not-a-link.md)` [ref][r]\n\n[r]: docs/guide.md\n", encoding="utf-8")
        (t / "docs/guide.md").write_text("# Guide\n", encoding="utf-8")
        mem = Path(tmp) / "memory"
        mem.mkdir()
        (mem / "MEMORY.md").write_text("- [Rule](rule.md) — a rule\n", encoding="utf-8")
        (mem / "rule.md").write_text("rule\n", encoding="utf-8")
        subprocess.run(["git", "init", "-q", str(t)], check=True)
        clean = check(t, memory=str(mem))
        if clean:
            print("docs-check selftest: the clean tree reported:", *clean, sep="\n  ")
            ok = False
        planted = {
            "broken link": lambda: (t / "docs/guide.md").write_text("# Guide\n[gone](gone.md)\n", encoding="utf-8"),
            "memory/MEMORY.md:1: broken link": lambda: (mem / "MEMORY.md").write_text("- [Gone](gone.md)\n", encoding="utf-8"),
            "duplicate ID": lambda: (t / "docs/notes/b.md").write_text(f"# B\n{block}## Rules\n| X-1 | again |\n", encoding="utf-8"),
            "missing required heading": lambda: (t / "docs/notes/a.md").write_text(f"# A\n{block}| X-1 | one |\n", encoding="utf-8"),
            "more than 30": lambda: (t / "docs/notes/a.md").write_text(f"# A\n{block}## Rules\n" + "line\n" * 40, encoding="utf-8"),
            "listed page": lambda: (t / "docs/notes/b.md").unlink(),
            "private value": lambda: (t / "docs/notes/a.md").write_text(f"# A\n{block}## Rules\nhost {lan_address}\n", encoding="utf-8"),
            "names the private docs/notes": lambda: (t / "docs/guide.md").write_text("# Guide\nsee docs/notes\n", encoding="utf-8"),
            "names the private TOPICS.md": lambda: (t / "docs/guide.md").write_text("# Guide\nsee TOPICS.md\n", encoding="utf-8"),
            "names the private g1-card.md": lambda: (t / "docs/guide.md").write_text("# Guide\nsee tests/g1-card.md\n", encoding="utf-8"),
            "broken link: missing-ref.md": lambda: (t / "docs/guide.md").write_text("# Guide\nsee [it][g]\n\n[g]: missing-ref.md\n", encoding="utf-8"),
            "undefined reference link": lambda: (t / "docs/guide.md").write_text("# Guide\nsee [it][nolabel]\n", encoding="utf-8"),
            "missing anchor": lambda: (t / "docs/guide.md").write_text("# Guide\n[top](../README.md#nope)\n", encoding="utf-8"),
            "says seat 1 for beta-box": lambda: (t / "docs/notes/a.md").write_text(f"# A\n{block}## Rules\n| X-1 | one |\nbeta-box holds seat 1.\n", encoding="utf-8"),
            "doesn't name alpha-box": lambda: (t / "docs/notes/b.md").write_text((t / "docs/notes/b.md").read_text(encoding="utf-8").replace("| 1 | alpha-box |", "| 1 | gamma-box |"), encoding="utf-8"),
            "undefined reference link: [gone-code]": lambda: (t / "docs/guide.md").write_text("# Guide\nsee [`code label`][gone-code]\n", encoding="utf-8"),
            "names seat 4, which no fact": lambda: (t / "docs/notes/a.md").write_text(f"# A\n{block}## Rules\n| X-1 | one |\nomega-box holds seat 4.\n", encoding="utf-8"),
            "says seat 1 without naming its holder": lambda: (t / "docs/notes/a.md").write_text(f"# A\n{block}## Rules\n| X-1 | one |\nomega-box holds seat 1.\n", encoding="utf-8"),
            "seat table row 3 has no fact": lambda: (t / "docs/notes/b.md").write_text((t / "docs/notes/b.md").read_text(encoding="utf-8") + "| 3 | omega-box |\n", encoding="utf-8"),
            "says seat 2 for alpha-box": lambda: (t / "docs/notes/a.md").write_text(f"# A\n{block}## Rules\n| X-1 | one |\nalpha-box holds seat 1 and alpha-box seat 2.\n", encoding="utf-8"),
            "X-8 is mentioned but defined": lambda: (t / "docs/notes/a.md").write_text(f"# A\n{block}## Rules\n| X-1 | one |\nsee `X-8`\n", encoding="utf-8"),
            "docs/guide.md:2: ID X-7 is mentioned but defined": lambda: (t / "docs/guide.md").write_text("# Guide\nsee X-7\n", encoding="utf-8"),
            "X-9 is mentioned but defined in no ID table": lambda: (t / "docs/notes/a.md").write_text(f"# A\n{block}## Rules\n| X-1 | one |\nsee X-9\n", encoding="utf-8"),
        }
        for kind, plant in planted.items():
            saved = {p: p.read_bytes() for p in [*t.rglob("*.md"), mem / "MEMORY.md"] if ".git" not in p.parts}
            plant()
            found = check(t, memory=str(mem))
            if not any(kind in line for line in found):
                print(f"docs-check selftest: missed '{kind}'; got:", *found, sep="\n  ")
                ok = False
            for p, data in saved.items():
                p.write_bytes(data)
    print("docs-check selftest:", "passed" if ok else "FAILED")
    return ok


def main():
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    root = Path(sys.argv[1])
    if "--selftest" in sys.argv[2:]:
        return 0 if selftest(root) else 1
    problems = check(root, memory=memory_dir(root))
    for line in problems:
        print(line, file=sys.stderr)
    if problems:
        print(f"docs-check: {len(problems)} problem(s)", file=sys.stderr)
        return 1
    print("docs-check: passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""The table of every new or changed Windows text (app.md §9 W-core and W14a, I45): each row of
core/texts.def whose Windows column isn't the Mac's words, with the part of texts.def it sits in
(the action it belongs to), its Mac text and its Windows text. Mac-only texts ("" on Windows) are
listed last. Markdown on stdout.

    python3 -I core/tools/win_texts_table.py [core/texts.def]
"""
import os
import re
import sys

LIT = r'"(?:[^"\\]|\\.)*"'
ROW = re.compile(r'^TEXT\((\w+),\s*(all|mac|win),\s*(' + LIT + r'),\s*(' + LIT + r')\)')
HEAD = re.compile(r'^/\*\s*(?:-+\s*)?(.*?)\s*(?:\*/)?$')


def unquote(lit):
    s = lit[1:-1]
    return re.sub(r'\\(.)', lambda m: {'n': '\\n', '"': '"', '\\': '\\'}.get(m.group(1), m.group(1)), s)


def cell(s):
    return s.replace('|', '\\|') if s else '—'


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..', 'texts.def')
    section, changed, mac_only, sections = '', [], [], {}
    with open(path, encoding='utf-8') as f:
        for line in f:
            # a part's heading: a comment that starts at the line's start, after a blank line or
            # right after a row (review C18); its next lines (" * ...") aren't headings
            m = HEAD.match(line)
            if m and not line.startswith('/* texts.def') and m.group(1) and not m.group(1).startswith('*'):
                section = m.group(1).rstrip('*/ ').strip()
            m = ROW.match(line)
            if not m:
                continue
            tid, os_tag, mac, win = m.group(1), m.group(2), unquote(m.group(3)), unquote(m.group(4))
            sections[tid] = section
            if not win:
                mac_only.append((tid, section, mac))
            elif win != mac:
                changed.append((tid, 'new' if os_tag == 'win' else 'changed', section, mac, win))
    print('| Text | Kind | Action (texts.def section) | Mac | Windows |')
    print('|---|---|---|---|---|')
    for tid, kind, sec, mac, win in changed:
        print('| %s | %s | %s | %s | %s |' % (tid, kind, cell(sec), cell(mac), cell(win)))
    print()
    print('Mac-only (no Windows text):', ', '.join(t for t, _, _ in mac_only))
    # the check (make check): each Windows-only text sits under its own heading, not the one before
    wrong = [t for t, sec in sections.items() if t.startswith('T_WIN_') and t != 'T_WIN_STILL_CLOSING'
             and sec.startswith('"rekordbox is still closing"')]
    if wrong:
        sys.exit('win_texts_table.py: under the wrong heading: ' + ', '.join(wrong))


if __name__ == '__main__':
    main()

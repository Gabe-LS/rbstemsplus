#!/bin/sh
# Rows and code dropped per OS (mac-core §3.1, §7): a Windows-only row appended to each .def table
# and a Windows-only C function under CORE_WIN_POLICY leave the Mac objects byte-identical; under
# CORE_WIN_POLICY the row and the function are there. Builds two copies of core/ in a scratch
# folder with core/build.sh lib (both architectures) and compares every object.
#
#   core/tests/os-rows.sh
set -eu
here="$(cd "$(dirname "$0")/.." && pwd)"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/rbsp-os-rows.XXXXXX")"
trap 'rm -rf "${scratch:?}"' EXIT

cp -R "$here" "$scratch/a"
cp -R "$here" "$scratch/b"
rm -rf "$scratch/a/tests" "$scratch/b/tests"   # not part of the objects
# lib's input list reads the depfiles with the repository's scripts/mac-inputs.py (../scripts from the copies)
mkdir "$scratch/scripts" && cp "$here/../scripts/mac-inputs.py" "$scratch/scripts/"

# a row before the closing #undefs of each table
add_row() { # file row
  awk -v row="$2" '!done && /^#undef/ { print row; done = 1 } { print }' "$1" > "$1.new" && mv "$1.new" "$1"
}
b="$scratch/b"
add_row "$b/schema.def" 'FIELD(F_WIN_ONLY_PROBE, S_SETTINGS, str, win, "a Windows-only field (os-rows.sh)")'
add_row "$b/schema.def" 'COLUMN(C_WIN_ONLY_PROBE, T_STATUS_SETS, i64, win, "a Windows-only column (os-rows.sh)")'
# a Windows-only table with its own column (multi-version rekordbox: the first win TABLE), and its
# numbering block in schema.c under CORE_WIN_POLICY, as T_WIN_RB_VERSIONS has
add_row "$b/schema.def" 'TABLE(T_WIN_TABLE_PROBE, S_STATUS, win, "a Windows-only table (os-rows.sh)")'
add_row "$b/schema.def" 'COLUMN(C_WIN_TABLE_PROBE, T_WIN_TABLE_PROBE, str, win, "a Windows-only table'\''s column (os-rows.sh)")'
awk '!done && /^\/\* every field is in one section/ {
  print "#ifdef CORE_WIN_POLICY"
  print "#define CORE_EQ_T_WIN_TABLE_PROBE_T_WIN_TABLE_PROBE CORE_PROBE"
  print "enum {"
  print "#define COLUMN(id, table, kind, os, doc) CORE_KEEP_##os(CORE_IF(CORE_EQ(table, T_WIN_TABLE_PROBE))(CI_##id,))"
  print "#include \"schema.def\""
  print "    CN_T_WIN_TABLE_PROBE"
  print "};"
  print "#endif"
  done = 1 } { print }' "$b/schema.c" > "$b/schema.c.new" && mv "$b/schema.c.new" "$b/schema.c"
grep -q 'CN_T_WIN_TABLE_PROBE' "$b/schema.c" || { echo "os-rows: the probe table's numbering block wasn't added to schema.c" >&2; exit 1; }
add_row "$b/texts.def" 'SLOT(win_probe, win)'
add_row "$b/texts.def" 'TEXT(T_WIN_ONLY_PROBE, win, "", "WINDOWS-ONLY-PROBE-TEXT {win_probe}")'
add_row "$b/menus.def" 'MENU(M_WIN_ONLY_PROBE, win, M_HELP, item, T_WIN_ONLY_PROBE, "", "F2", 0, 0, "win_probe")'
add_row "$b/facts.def" 'FACT(FACT_WIN_ONLY_PROBE, flag, win, "a Windows-only fact (os-rows.sh)")'
add_row "$b/events.def" 'EVENT(EV_WIN_ONLY_PROBE, "win_only_probe", win, "")'
cat >> "$b/core.c" <<'EOF'

#ifdef CORE_WIN_POLICY
int32_t core_win_only_probe(void);
int32_t core_win_only_probe(void) { return F_WIN_ONLY_PROBE + C_WIN_ONLY_PROBE + FACT_WIN_ONLY_PROBE + T_WIN_TABLE_PROBE + C_WIN_TABLE_PROBE; }
#endif
EOF

"$scratch/a/build.sh" lib "$scratch/out-a" > /dev/null
"$scratch/b/build.sh" lib "$scratch/out-b" > /dev/null
n=0
for arch in arm64 x86_64; do
  for o in "$scratch/out-a/$arch/obj/"*.o; do
    name="$(basename "$o")"
    cmp -s "$o" "$scratch/out-b/$arch/obj/$name" || { echo "os-rows: $arch/$name differs with a Windows-only row or function" >&2; exit 1; }
    n=$((n + 1))
  done
  cmp -s "$scratch/out-a/$arch/libcore.a" "$scratch/out-b/$arch/libcore.a" || { echo "os-rows: $arch/libcore.a differs" >&2; exit 1; }
done

# and under CORE_WIN_POLICY they are really there (so the comparison above isn't vacuous)
(cd "$b" && for s in text core schema menus ids; do clang -std=c11 -O2 -Wall -Werror -DCORE_WIN_POLICY -c "$s.c" -o "$scratch/$s-win.o"; done)
strings "$scratch/text-win.o" | grep -q WINDOWS-ONLY-PROBE-TEXT || { echo "os-rows: the Windows text is missing under CORE_WIN_POLICY" >&2; exit 1; }
nm "$scratch/core-win.o" | grep -q '_core_win_only_probe' || { echo "os-rows: the Windows function is missing under CORE_WIN_POLICY" >&2; exit 1; }
if strings "$scratch/out-b/arm64/obj/text.o" | grep -q 'WINDOWS-ONLY-PROBE-TEXT\|win_probe'; then echo "os-rows: the Windows text or slot is in the Mac objects" >&2; exit 1; fi
strings "$scratch/text-win.o" | grep -q '^win_probe$' || { echo "os-rows: the Windows slot is missing under CORE_WIN_POLICY" >&2; exit 1; }
for id in T_WIN_TABLE_PROBE C_WIN_TABLE_PROBE; do
  strings "$scratch/schema-win.o" | grep -qx "$id" || { echo "os-rows: the Windows table's $id is missing under CORE_WIN_POLICY" >&2; exit 1; }
  if strings "$scratch/out-b/arm64/obj/schema.o" | grep -q "$id"; then echo "os-rows: the Windows table's $id is in the Mac objects" >&2; exit 1; fi
done
echo "os-rows: $n Mac objects and both archives identical with a Windows-only row in every table (a Windows-only slot and a Windows-only table with its column included) and a Windows-only function"

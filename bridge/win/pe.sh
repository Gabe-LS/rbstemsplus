# The PE table parsers behind bridge/win/build.sh's export and import gates (BW7-BW9), sourced by
# build.sh and by tests/run.sh, which runs them on tests/pe/*.txt (the pinned tools' text, captured).
# Each prints one line, its rows separated by single spaces, for an exact comparison.

# pe_exports [FILE]: llvm-objdump -p's text (FILE, or stdin) -> every row of its Export Table as
# ord:name, ord:- for a row with no name (NONAME). Fails, printing nothing, on a forwarded row or on
# any other line it doesn't know.
pe_exports() {
  awk '
    /^Export Table:$/ { t = 1; next }
    !t { next }
    /^$/ { t = 0; next }
    /^ DLL name: / || /^ Ordinal base: / || /^ Ordinal +RVA +Name$/ { next }
    !/forwarded to/ && $1 ~ /^[0-9]+$/ && $2 ~ /^0x[0-9a-f]+$/ && NF <= 3 {
      s = s sep $1 ":" (NF == 3 ? $3 : "-"); sep = " "; next
    }
    { bad = 1; row = $0; exit }
    END {
      if (bad) { print "pe_exports: a forwarded or unknown Export Table row: " row > "/dev/stderr"; exit 1 }
      print s
    }
  ' "$@"
}

# pe_imports DLL [FILE]: llvm-readobj --coff-imports's text (FILE, or stdin) -> the Symbol rows
# ("(1)" for an ordinal, "Name (hint)" for a name, at any indent) of every descriptor, Import or
# DelayImport, whose Name: is DLL as the loader reads a name (BX8): both in lower case, trailing
# dots and spaces stripped, ".dll" appended to a name with no dot, then a whole-name match (so a
# dot inside the name is literal).
pe_imports() {
  awk -v dll="$1" '
    function norm(n) { n = tolower(n); sub(/[. ]+$/, "", n); if (index(n, ".") == 0) n = n ".dll"; return n }
    BEGIN { want = norm(dll) }
    /^[A-Za-z]+ \{$/ { b = 1; m = 0; syms = ""; next }
    b && /^\}$/ { if (m) { s = s sep syms; sep = " " } b = 0; next }
    b && /^  Name: / { v = $0; sub(/^  Name: /, "", v); m = (norm(v) == want); next }
    b && /^ +Symbol: / {
      v = $0; sub(/^ +Symbol: +/, "", v); sub(/ +$/, "", v)
      syms = syms (syms == "" ? "" : " ") v
    }
    END { print s }
  ' "${2:--}"
}

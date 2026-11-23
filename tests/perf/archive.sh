# The gate archive's checksums (the private Windows plan §8.2, release.md §6.1), shared by the scripts that
# write baselines into it (tests/perf/mac-a.sh and tests/perf/b0.sh).
# Sourced, with A set to the archive's folder ("${GATES_ARCHIVE:-<checkout>/gates-archive}").
#   seal DIR...  writes A/SHA256SUMS, warning about files missing from the old manifest outside
#                the directories added by this run. *.partial and .DS_Store are left out.
#   verify    checks A/SHA256SUMS; a missing or mismatched file stops the gate (never rebuild a
#             baseline)
# Both return 2 with a message on failure.

archive_files() {
  find "$@" -type f ! -name SHA256SUMS ! -name '.DS_Store' ! -path '*.partial*' | sed 's|^\./||'
}

unlisted() {   # paths from a new manifest (or stdin) missing from the current manifest
  awk 'function filename(line, escaped) {
         escaped = sub(/^\\/, "", line)
         sub(/^[[:xdigit:]]+ [ *]/, "", line)
         if (escaped) gsub(/\\\\/, "\\", line)
         return line
       }
       FILENAME == ARGV[1] { listed[filename($0)] = 1; next }
       { path = FILENAME == "-" ? $0 : filename($0); if (!(path in listed)) print path }' \
    "$A/SHA256SUMS" "${1:--}"
}

warn_unlisted() {
  local extra f dir
  [ -f "$A/SHA256SUMS" ] || return 0
  extra="$(unlisted "$1")" || return 2
  shift
  while IFS= read -r f; do
    [ -n "$f" ] || continue
    for dir; do case "$f" in "$dir"/*) continue 2 ;; esac; done
    echo "seal: warning: $f was missing from the old SHA256SUMS" >&2
  done <<< "$extra"
}

seal() {   # the manifest of everything in the archive
  local m
  m="$(mktemp "${TMPDIR:-/tmp}/rbsp-sums.XXXXXX")" || { echo "seal: no temporary file" >&2; return 2; }
  if (set -o pipefail; cd "$A" && archive_files . | LC_ALL=C sort | tr '\n' '\0' | xargs -0 shasum -a 256 > "$m") \
    && warn_unlisted "$m" "$@" && mv "$m" "$A/SHA256SUMS"; then return 0; fi
  rm -f "$m"; echo "seal: writing $A/SHA256SUMS failed" >&2; return 2
}

verify() {
  [ -f "$A/SHA256SUMS" ] || { echo "verify: no $A/SHA256SUMS" >&2; return 2; }
  (cd "$A" && shasum -a 256 -c --strict --quiet SHA256SUMS) \
    || { echo "verify: the gate archive doesn't match its SHA256SUMS: stop (never rebuild a baseline)" >&2; return 2; }
}

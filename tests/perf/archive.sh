# The gate archive's checksums (the private Windows plan §8.2, release.md §6.1), shared by the scripts that
# write baselines into it (tests/perf/mac-a.sh and tests/perf/b0.sh).
# Sourced, with A set to the archive's folder ("${GATES_ARCHIVE:-<checkout>/gates-archive}").
#   seal      writes A/SHA256SUMS over every file in the archive (a run set still being recorded,
#             *.partial, is left out)
#   verify    checks A/SHA256SUMS; a missing or mismatched file stops the gate (never rebuild a
#             baseline)
# Both return 2 with a message on failure.

seal() {   # the manifest of everything in the archive
  local m
  m="$(mktemp "${TMPDIR:-/tmp}/rbsp-sums.XXXXXX")" || { echo "seal: no temporary file" >&2; return 2; }
  if (cd "$A" && find . -type f ! -name SHA256SUMS ! -name '.DS_Store' ! -path '*.partial*' | LC_ALL=C sort | sed 's|^\./||' | tr '\n' '\0' | xargs -0 shasum -a 256 > "$m") \
    && mv "$m" "$A/SHA256SUMS"; then return 0; fi
  rm -f "$m"; echo "seal: writing $A/SHA256SUMS failed" >&2; return 2
}

verify() {
  [ -f "$A/SHA256SUMS" ] || { echo "verify: no $A/SHA256SUMS" >&2; return 2; }
  (cd "$A" && shasum -a 256 -c --quiet SHA256SUMS) \
    || { echo "verify: the gate archive doesn't match its SHA256SUMS: stop (never rebuild a baseline)" >&2; return 2; }
}

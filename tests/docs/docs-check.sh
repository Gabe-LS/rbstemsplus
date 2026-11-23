#!/bin/bash
# The documentation checks (make check): links in docs/ and the memory index resolve; pages with a
# docs-check block keep their required headings, unique IDs, size and page list; those pages and the
# public documents hold no private values (scripts/public-scan.py, as the release export); no public
# file names a private path. The rules are in tests/docs/docs-check.py's header. First its selftest
# (one planted problem of each kind must be found), then the real tree.
# DOCS_CHECK_MEMORY=DIR checks another memory folder; DOCS_CHECK_MEMORY= (empty) skips it.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
python3 -I "$root/tests/docs/docs-check.py" "$root" --selftest
python3 -I "$root/tests/docs/docs-check.py" "$root"

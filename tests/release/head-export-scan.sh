#!/bin/bash
# The public export privacy scan on HEAD, as release.sh runs it on the release commit: export HEAD,
# drop .publicignore's paths and the metadata, scan with HEAD's .publicallow. make check runs it so a
# private host or name in a public file fails before release day, not at release.sh --plan.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
stage="$tmp/public"; mkdir "$stage"
git -C "$root" archive --format=tar HEAD | tar -x -f - -C "$stage"
rm -f "$stage/.publicignore" "$stage/.publicallow"
while IFS= read -r p; do
  case "$p" in ''|'#'*) continue ;; esac
  rm -rf "${stage:?}/$p"
done < <(git -C "$root" show HEAD:.publicignore)
git -C "$root" show HEAD:.publicallow > "$tmp/publicallow"
python3 -I "$stage/scripts/public-scan.py" "$stage" "$tmp/publicallow" "$root"
echo "head-export-scan: passed"

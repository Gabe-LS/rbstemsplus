#!/bin/bash
# The "no new bundle files" gate: the app's file list must equal tests/mac/bundle-files.txt, the
# 14 paths of 1.3.0's bundle. Run by scripts/build-app.sh on every build. An added or removed path
# fails, and so does anything that isn't a plain file or folder. Changing the list is a reviewed
# commit, made only together with a passing tart run of 1.0.0's and 1.3.0's in-app update.
#
#   tests/mac/bundle-files.sh APP_OR_ZIP [LIST]
#
# APP_OR_ZIP: "RB Stems Plus.app" (a folder), a folder holding the unpacked zip, or the app zip
# (unpacked with ditto into a scratch folder). Paths are compared relative to the folder holding the app, so they start with
# "RB Stems Plus.app/". LIST: the expected list (default: tests/mac/bundle-files.txt).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
die() { echo "bundle-files.sh: $*" >&2; exit 2; }
[ $# -ge 1 ] && [ $# -le 2 ] || { echo "usage: $0 APP_OR_ZIP [LIST]" >&2; exit 2; }
list="${2:-$here/bundle-files.txt}"
[ -f "$list" ] || die "no list at $list"
tmp="$(mktemp -d "${TMPDIR:-/tmp}/rbsp-bundle.XXXXXX")"
trap 'rm -rf "${tmp:?}"' EXIT
if [ -f "$1" ]; then
  top="$tmp/unpacked"
  mkdir "$top"
  ditto -x -k "$1" "$top" || die "couldn't unpack $1"
elif [ -d "$1" ] && [[ "${1%/}" == *.app ]]; then
  # the app itself: its parent is the top, and only the app is listed
  top="$(cd "$(dirname "$1")" && pwd)"
  only="$(basename "$1")"
elif [ -d "$1" ]; then
  top="$1"                     # a folder holding the unpacked zip
else
  die "no such app or zip: $1"
fi
cd "$top"
scope=(.)
[ -z "${only:-}" ] || scope=("./$only")
odd="$(find "${scope[@]}" ! -type f ! -type d | sed 's|^\./||')"
got="$(find "${scope[@]}" -type f | sed 's|^\./||' | LC_ALL=C sort)"
want="$(grep -v '^$' "$list" | LC_ALL=C sort)"
bad=0
if [ -n "$odd" ]; then
  echo "not a plain file or folder:"; sed 's/^/  /' <<< "$odd"; bad=1
fi
added="$(LC_ALL=C comm -13 <(echo "$want") <(echo "$got"))"
removed="$(LC_ALL=C comm -23 <(echo "$want") <(echo "$got"))"
if [ -n "$added" ]; then echo "new files (not in $(basename "$list")):"; sed 's/^/  + /' <<< "$added"; bad=1; fi
if [ -n "$removed" ]; then echo "missing files (in $(basename "$list")):"; sed 's/^/  - /' <<< "$removed"; bad=1; fi
n="$(grep -c . <<< "$got" || true)"
if [ "$bad" = 0 ]; then echo "bundle files: the $n paths of $(basename "$list"), nothing else"; else echo "bundle files: FAILED"; fi
exit "$bad"

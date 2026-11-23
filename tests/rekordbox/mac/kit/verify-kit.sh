#!/bin/bash
# verify-kit.sh: the Mac kit checked in the VM (as win/kit/verify-kit.ps1): every file of SHA256SUMS
# (sha256, two spaces, relative path) is there with its hash, and nothing else is (no unlisted
# file, no dot path, no symbolic link). Exit 0 all good; 1 not.
set -u
kit="$(cd "$(dirname "$0")" && pwd)"
cd "$kit" || exit 1
bad=0; n=0
[ -f SHA256SUMS ] || { echo "no SHA256SUMS"; exit 1; }
l="$(find . \( -type l -o -name '.*' ! -name . \) -print -quit)" || exit 1
[ -z "$l" ] || { echo "dot path or symbolic link: $l"; exit 1; }
listed=$'\n'   # the manifest's paths, each between newlines
while IFS= read -r line || [ -n "$line" ]; do
  case "$line" in '' | '#'*) continue ;; esac
  [[ "$line" =~ ^[0-9a-f]{64}\ \ (.+)$ ]] || { echo "bad manifest line: $line"; exit 1; }
  rel="${BASH_REMATCH[1]}"
  if [[ "$rel" == /* || "/$rel/" == */../* ]]; then echo "bad manifest path: $rel"; exit 1; fi
  if [[ "$listed" == *$'\n'"$rel"$'\n'* ]]; then echo "repeated: $rel"; bad=$((bad + 1)); continue; fi
  listed+="$rel"$'\n'
  if [ ! -f "$rel" ]; then echo "missing: $rel"; bad=$((bad + 1)); continue; fi
  [ "$(shasum -a 256 "$rel" | cut -d' ' -f1)" = "${line:0:64}" ] || { echo "differs: $rel"; bad=$((bad + 1)); }
  n=$((n + 1))
done < SHA256SUMS
files="$(find . -type f)" || exit 1
while IFS= read -r f; do
  f="${f#./}"
  [ "$f" = SHA256SUMS ] && continue
  [[ "$listed" == *$'\n'"$f"$'\n'* ]] || { echo "unlisted: $f"; bad=$((bad + 1)); }
done <<< "$files"
[ "$n" -gt 0 ] || { echo "empty SHA256SUMS"; bad=$((bad + 1)); }
echo "SHA256SUMS: $n files present, $bad bad"
[ "$bad" = 0 ]

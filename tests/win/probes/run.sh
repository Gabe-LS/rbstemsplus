#!/bin/bash
# Runs one of tests/win/probes/vm/*.ps1 in the VM, after syncing vm/ into the share (\\Mac\win\p0).
#   run.sh [--user|--user-signed] NAME ['$var = value' ...]   the extra arguments become the script's first lines
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
# the VM shares only the main checkout's build/win (\\Mac\win), also from a worktree
SHARE="$(cd "$(git -C "$(dirname "$0")" rev-parse --path-format=absolute --git-common-dir)/.." && pwd)/build/win"
user=
case "${1:-}" in --user|--user-signed) user="$1"; shift ;; esac
name="$1"; shift
mkdir -p "$SHARE/p0"
cp "$here"/vm/*.ps1 "$SHARE/p0/"
{ for l in "$@"; do printf '%s\n' "$l"; done; cat "$here/vm/$name.ps1"; } | "$here/winps.sh" $user

#!/bin/bash
# The owner's Little Snitch check: tries every outgoing connection the project's tooling makes, with
# the program that really makes it (Little Snitch's rules are per program: curl's rule says nothing
# about codex's), each bounded, and says which went through. Run it in Terminal while at the Mac:
# a Little Snitch prompt shows for each connection without a rule; answer "Allow" + "Forever" and
# run it again until every row is OK. Nothing here changes Little Snitch, and nothing is sent but a
# plain request (codex and claude: one tiny prompt each, skipped with --no-ai).
#
#   tests/harness/netcheck.sh [--no-ai] [--no-pc]
#   NETCHECK_TSV=FILE: each row also appended to FILE as STATUS<TAB>PROGRAM<TAB>LABEL<TAB>DETAIL
#   (netguard.sh preflight reads them)
#
# Each row: OK (connected; any HTTP status counts), TIMEOUT (no answer in time: most likely a
# Little Snitch prompt nobody answered, or a rule that denies), FAIL (a real error, said), SKIP (not
# checked here: not counted either way).
# The Windows PC's ssh login lives in this Mac's local git config, never in a tracked file (the
# public copy must not hold the owner's name or address):
#   git config --local rbstemsplus.pcSsh "USER@HOST"
# Without it the PC row is SKIP "not set".
# The ntfy and NAS probes also use local settings. Unconfigured probes report SKIP.
#   git config --local rbstemsplus.ntfyHost '<ntfy host>'
#   git config --local rbstemsplus.nasHost '<NAS host>'
# Not covered: the Parallels VMs' own traffic (W3's winhttp-github: Parallels' rules to github.com
# and release-assets.githubusercontent.com, made by hand once) and notarization (the owner's
# release.sh run asks for its own connections then).
set -uo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
ai=1 pc=1
for a in "$@"; do
  case "$a" in
    --no-ai) ai=0 ;;
    --no-pc) pc=0 ;;
    *) echo "usage: $0 [--no-ai] [--no-pc]" >&2; exit 2 ;;
  esac
done
ok=0 bad=0
# within SECS CMD...: runs CMD, killed after SECS (exit 142 then)
within() { local s="$1"; shift; perl -e 'alarm shift; exec @ARGV or exit 127' "$s" "$@"; }
row() {   # row LABEL PROGRAM RC DETAIL
  local st
  case "$3" in
    0) st=OK; ok=$((ok + 1)) ;;
    skip) st=SKIP ;;
    28|142) st=TIMEOUT; bad=$((bad + 1)) ;;
    *) st="FAIL($3)"; bad=$((bad + 1)) ;;
  esac
  printf '%-9s %-6s %-46s %s\n' "$st" "$2" "$1" "$4"
  [ -z "${NETCHECK_TSV:-}" ] || printf '%s\t%s\t%s\t%s\n' "$st" "$2" "$1" "$4" >> "$NETCHECK_TSV"
}
# curl: the Windows build's downloads (deps.sh), the update's release assets, the ntfy notices
web() {   # web HOST URL
  local code rc
  code="$(curl -sS -o /dev/null -w '%{http_code}' --connect-timeout 15 --max-time 30 "$2" 2>/dev/null)"; rc=$?
  [ "$rc" = 0 ] && [ "$code" = 000 ] && rc=7
  row "$1" curl "$rc" "${code:+HTTP $code}"
}
configured_web() {   # configured_web CONFIG_KEY SCHEME PORT LABEL_SUFFIX
  local host
  host="$(git -C "$root" config --get "$1" || true)"
  if [ -n "$host" ]; then
    web "$host$3$4" "$2://$host$3/"
  else
    row "${1#rbstemsplus.}" curl skip "(not set: git config --local $1)"
  fi
}
echo "Little Snitch check ($(date '+%F %T')): answer each prompt with Allow + Forever, then run again"
web github.com                           https://github.com/
web release-assets.githubusercontent.com https://release-assets.githubusercontent.com/
web objects.githubusercontent.com        https://objects.githubusercontent.com/
web raw.githubusercontent.com            https://raw.githubusercontent.com/
web api.nuget.org                        https://api.nuget.org/v3/index.json
web builds.dotnet.microsoft.com          https://builds.dotnet.microsoft.com/dotnet/release-metadata/releases-index.json
web downloads.xiph.org                   https://downloads.xiph.org/
web www.apple.com                        https://www.apple.com/
web rekordbox.com                        https://rekordbox.com/
configured_web rbstemsplus.ntfyHost https '' ''
configured_web rbstemsplus.nasHost http :8090 ' (ntfy, home LAN)'
# git and gh: pushes, pulls and the release drafts
within 60 git -C "$root" ls-remote --heads origin > /dev/null 2>&1; row 'origin (git ls-remote)' git $? ""
within 60 gh api rate_limit > /dev/null 2>&1; row 'api.github.com (gh)' gh $? ""
if [ "$ai" = 1 ]; then
  # codex: the GPT reviews and advice; claude: the budget meter
  d="$(mktemp -d)"
  within 180 codex exec -m gpt-6.2-sol -c model_reasoning_effort='"low"' -s read-only --ephemeral --skip-git-repo-check -C "$d" -o "$d/out" "Reply with the word OK." < /dev/null > /dev/null 2>&1; rc=$?
  [ "$rc" = 0 ] && ! grep -q . "$d/out" 2> /dev/null && rc=1
  row 'OpenAI (codex exec)' codex "$rc" ""
  within 120 claude -p "/usage" > "$d/usage" 2>&1; rc=$?
  [ "$rc" = 0 ] && ! grep -q "Current week" "$d/usage" && rc=1
  row 'Anthropic (claude -p /usage)' claude "$rc" ""
  rm -rf "$d"
fi
if [ "$pc" = 1 ]; then
  # ssh: the owner's Windows PC over Tailscale (a FAIL here may only mean the PC is off)
  target="$(git -C "$root" config --get rbstemsplus.pcSsh || true)"
  if [ -z "$target" ]; then
    row 'the Windows PC (ssh, Tailscale)' ssh skip '(not set: git config --local rbstemsplus.pcSsh)'
  else
    within 30 ssh -o BatchMode=yes -o ConnectTimeout=15 "$target" exit > /dev/null 2>&1; rc=$?
    row 'the Windows PC (ssh, Tailscale)' ssh "$rc" "$([ "$rc" = 0 ] || echo '(or the PC is off)')"
  fi
fi
echo "$ok OK, $bad not OK"
[ "$bad" = 0 ]

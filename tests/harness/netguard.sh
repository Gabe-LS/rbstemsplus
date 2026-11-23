#!/bin/bash
# netguard: runs one network step of an unattended (night) lane so that it fails fast and reports
# instead of hanging behind an unanswered Little Snitch prompt (or a dead network). Little Snitch holds
# a connection without a rule at connect time; nobody answers at night; the step would wait forever.
#
#   tests/harness/netguard.sh [--host H] [--kind K] [--stall SECS] [--watch FILE] -- CMD...
#   tests/harness/netguard.sh preflight [NETCHECK-ARGS...]   evening check (netcheck.sh --no-ai, bounded)
#   tests/harness/netguard.sh summary                        morning summary: prints, one ntfy
#   tests/harness/netguard.sh reset HOST                     closes HOST's circuit breaker by hand
#
# A run: CMD in its own process group; its stdout and stderr pass through unchanged (and into a log).
#   Progress = its output growing, or --watch FILE growing (curl -o FILE is silent), or, as evidence
#   only, nettop's byte counts for its processes (no nettop, e.g. in the sandbox: unknown, never a
#   stall by itself). No progress for --stall SECS (default 600, at most 600) → TERM, a grace, KILL,
#   reaped and checked. If netguard itself dies, its launcher stops CMD's group too.
#   Independent of progress: a status line on stderr at least every 10 minutes while unfinished; an
#   ntfy with it when the step isn't healthy (no progress in that window, or retrying), deduplicated
#   (NETGUARD_NTFY_HEALTHY=1: also for healthy ones).
# --kind (retries are owned here; callers don't retry on top):
#   download, git, gh   transient (reset, DNS, 5xx, 408/429 with a Retry-After ≤ 5 min, a stall
#                       mid-transfer): 2 retries, ~30 s then ~2 min (jitter); connect failure or
#                       timeout: 1 retry after ~1 min; 4xx, checksum, usage: none. gh: reads only.
#   push                git push: 1 retry (pushing the same refs again is harmless)
#   ai                  codex, claude: 1 retry after ~1 min (transient or connect)
#   ssh                 1 retry, only for ssh's own errors before a session (exit 255: connect to host,
#                       banner exchange, kex, resolve hostname): the remote command didn't run
#                       (never after a stall: it may have)
#   once                (default) never retried: release drafts, comments, anything not idempotent
#   Tool timeouts added when CMD is curl (--connect-timeout 20 --speed-limit 1 --speed-time 120), git
#   (GIT_HTTP_LOW_SPEED_LIMIT=1/_TIME=120, no prompts, GIT_SSH_COMMAND with the ssh options) or ssh
#   (-o ConnectTimeout=15 -o BatchMode=yes -o ServerAliveInterval=15 -o ServerAliveCountMax=4
#   -o ControlMaster=no: an existing master is reused, none is started that would outlive it); an
#   option the caller already gave is kept. NETGUARD_TOOL_OPTS=0: none added.
# Circuit breaker per host (build/netguard/breaker/, for the night: 12 h): after 3 consecutive network
#   failures to H, calls fail fast (exit 75) with the reason until one probe of H (curl, any HTTP
#   status) succeeds; probes at most every 2 minutes.
# Before blaming H for a connect failure or timeout, github.com is probed (www.apple.com when H is
#   github.com): "github.com answers, so only H fails" or "network down?". The notice says "timeout,
#   cause unknown (Little Snitch prompt or deny, or the network)": netguard can't see Little Snitch.
# State: build/netguard in the main checkout (git's common dir; lanes run in worktrees), or in this
#   checkout when that isn't writable; NETGUARD_DIR overrides. A pid-owned lock guards it.
# Incidents: one line each in build/netguard/incidents.log (end time, first failure, program, host,
#   kind, phase, attempts, exit status, outcome, reason); steps still failing have a file in
#   build/netguard/open/ (one left behind = an interrupted step, listed by summary). Notices are
#   deduplicated (same program, host and phase: once an hour); the notifier call is bounded and a
#   notice it couldn't deliver waits in build/netguard/outbox (outbox.PID when the lock was busy),
#   sent before the next one, without holding the lock (summary folds them in).
# Exit: CMD's status; 124 stalled and killed; 75 the host's breaker is open; 64 usage.
# Known gaps (review NG, follow-ups): NG4 when netguard itself dies, its launcher stops CMD's group but
#   not the descendants that left it; NG5 a status notice is sent in line, so a slow notifier delays
#   supervision (up to ~80 s with claude-notify); NG7 curl -f never shows Retry-After (no -D header
#   file yet), so a 429/503 from curl retries on the usual backoff; NG10 a connect failure on a
#   redirect target is blamed on --host, not on the host curl/git names.
#
# Night lanes (run netguard unsandboxed: ntfy needs the Keychain, nettop the network stats):
#   evening, before the lanes:  tests/harness/netguard.sh preflight
#       not OK → one ntfy naming program and host; build/netguard/preflight.state lists them as
#       "bad<TAB>program<TAB>host<TAB>status" lines; a lane needing such a host waits for the owner:
#       grep -q "^bad.*api.nuget.org" build/netguard/preflight.state && echo held
#   tests/harness/netguard.sh --kind ai --host api.openai.com -- codex exec ... < /dev/null
#   tests/harness/netguard.sh --kind ai --host api.anthropic.com -- claude -p "/usage"
#   tests/harness/netguard.sh --kind gh --host api.github.com -- gh api rate_limit
#   tests/harness/netguard.sh --kind once --host api.github.com -- gh release create --draft ...
#   tests/harness/netguard.sh --kind push --host github.com -- git push origin w6/x
#   tests/harness/netguard.sh --kind ssh --host PC-HOST -- ssh "$(git config --get rbstemsplus.pcSsh)" ...
#   morning:                    tests/harness/netguard.sh summary
#   The GPT reviews (.orchestration/workflows/lane-review-v3.js) run codex exec this way.
# The owner's side: Little Snitch's own alert timeout (Preferences > Alert: confirm automatically
#   after a timeout, default action Deny) bounds a prompt nobody answers; it is the owner's setting.
#
# Environment (tests and tuning): NETGUARD_DIR (state, default build/netguard), NETGUARD_NOTIFY
# (default ~/.local/bin/claude-notify), NETGUARD_NOTIFY_TIMEOUT 40, NETGUARD_TICK 2, NETGUARD_GRACE 10,
# NETGUARD_REPORT 600, NETGUARD_DEDUP 3600, NETGUARD_BACKOFF_TRANSIENT "30 120",
# NETGUARD_BACKOFF_CONNECT 60, NETGUARD_BACKOFF_ONE 60 (push, ai), NETGUARD_JITTER 1,
# NETGUARD_RETRY_AFTER_MAX 300, NETGUARD_BREAK_AFTER 3, NETGUARD_BREAKER_TTL 43200,
# NETGUARD_PROBE_EVERY 120, NETGUARD_LOCK_WAIT 60, NETGUARD_PROBE / NETGUARD_CONTROL_PROBE (shell
# commands; $NG_HOST is the host), NETGUARD_CONTROL_HOST, NETGUARD_NETTOP 1, NETGUARD_NETTOP_AFTER 60 (s of silence
# before a nettop sample), NETGUARD_SPEED_TIME 120, NETGUARD_NETCHECK, NETGUARD_PREFLIGHT_STALL 180.
# --plain (preflight's own): a failure that isn't a stall passes through, no incident or notice.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
# the state lives in the main checkout (lanes run in worktrees that go away), else in this one
common="$(git -C "$root" rev-parse --path-format=absolute --git-common-dir 2> /dev/null)"
ND="${NETGUARD_DIR:-}"
if [ -z "$ND" ] && [ -n "$common" ] && mkdir -p "$(dirname "$common")/build/netguard" 2> /dev/null; then ND="$(dirname "$common")/build/netguard"; fi
ND="${ND:-$root/build/netguard}"
NOTIFY="${NETGUARD_NOTIFY:-$HOME/.local/bin/claude-notify}"
NOTIFY_TIMEOUT="${NETGUARD_NOTIFY_TIMEOUT:-40}"
TICK="${NETGUARD_TICK:-2}"
GRACE="${NETGUARD_GRACE:-10}"
REPORT="${NETGUARD_REPORT:-600}"
DEDUP="${NETGUARD_DEDUP:-3600}"
BACKOFF_T="${NETGUARD_BACKOFF_TRANSIENT:-30 120}"
BACKOFF_C="${NETGUARD_BACKOFF_CONNECT:-60}"
BACKOFF_1="${NETGUARD_BACKOFF_ONE:-60}"
JITTER="${NETGUARD_JITTER:-1}"
RA_MAX="${NETGUARD_RETRY_AFTER_MAX:-300}"
BREAK_AFTER="${NETGUARD_BREAK_AFTER:-3}"
BREAKER_TTL="${NETGUARD_BREAKER_TTL:-43200}"
PROBE_EVERY="${NETGUARD_PROBE_EVERY:-120}"
CONTROL_HOST="${NETGUARD_CONTROL_HOST:-github.com}"
NETTOP="${NETGUARD_NETTOP:-1}" NETTOP_AFTER="${NETGUARD_NETTOP_AFTER:-60}"
LOCK_WAIT="${NETGUARD_LOCK_WAIT:-60}"
LS_NOTE='timeout, cause unknown (Little Snitch prompt or deny, or the network)'
[ "$REPORT" -gt 600 ] 2> /dev/null && REPORT=600

usage() { sed -n '4,8p' "$0" >&2; exit 64; }
say() { echo "netguard: $*" >&2; }
now() { date +%s; }
now_ds() { perl -MTime::HiRes=time -e 'printf "%d", time * 10'; }   # tenths of a second
stamp() { date -r "$1" '+%F %T'; }
within() { local s="$1"; shift; perl -e 'alarm shift; exec @ARGV or exit 127' "$s" "$@"; }
oneline() { tr '\t\n\r' '   ' | sed 's/  *$//' | cut -c1-"${1:-300}"; }
mkdir -p "$ND/breaker" "$ND/open" "$ND/logs" "$ND/running" 2> /dev/null

alive() {   # PID: running (a pid we may not signal, e.g. from the sandbox, counts as running)
  local e; e="$(kill -0 "$1" 2>&1)" && return 0
  case "$e" in *"not permitted"*) return 0 ;; esac
  return 1
}
# a lock for the shared files (parallel lanes): a directory holding its owner's pid. 0 only when
# this process owns it; taken over only from a dead owner or after 300 s; 1 after LOCK_WAIT s.
lock() {
  local n=0 p m limit
  limit=$((LOCK_WAIT * 20))
  while :; do
    if mkdir "$ND/.lock" 2> /dev/null; then echo $$ > "$ND/.lock/pid"; return 0; fi
    p="$(cat "$ND/.lock/pid" 2> /dev/null)"
    m="$(stat -f %m "$ND/.lock" 2> /dev/null || now)"
    if [ $(($(now) - m)) -gt 300 ] || { [ -n "$p" ] && ! alive "$p"; }; then
      [ "$(cat "$ND/.lock/pid" 2> /dev/null)" = "$p" ] \
        && mv "$ND/.lock" "$ND/.lock.stale.$$" 2> /dev/null && rm -rf "$ND/.lock.stale.$$"
      continue
    fi
    n=$((n + 1)); [ "$n" -ge "$limit" ] && return 1
    sleep 0.05
  done
}
unlock() { [ "$(cat "$ND/.lock/pid" 2> /dev/null)" = "$$" ] && rm -rf "$ND/.lock"; return 0; }
take_queue() {   # DEST: gathers queued notices under the state lock
  local f pid; : > "$1"
  for f in "$ND/outbox" "$ND"/outbox.[0-9]* "$ND"/outbox.sending.[0-9]* "$ND"/outbox.unsent.[0-9]*; do
    [ -f "$f" ] || continue
    case "${f##*/}" in
      outbox.sending.* | outbox.unsent.*)
        pid="${f##*.}"
        case "$pid" in *[!0-9]*) continue ;; esac
        alive "$pid" && continue
        # The sending batch already includes the unsent remainder when both survived.
        if [ "$f" = "$ND/outbox.sending.$pid" ]; then
          rm -f "$ND/outbox.unsent.$pid" || continue
        elif [ -f "$ND/outbox.sending.$pid" ]; then
          continue
        fi ;;
    esac
    cat "$f" >> "$1" && rm -f "$f"
  done
  return 0
}

# ---- notices ---------------------------------------------------------------------------------------
send_raw() {   # MESSAGE TITLE → 0 when the notifier said sent
  [ -x "$NOTIFY" ] || return 1
  within "$NOTIFY_TIMEOUT" "$NOTIFY" "$1" "$2" < /dev/null > /dev/null 2>&1
}
notify() {   # KEY MESSAGE [TITLE]: deduplicated by KEY for DEDUP s; what waits goes first; queued when it can't go out
  local key="$1" msg title="${3:-netguard}" t q="$ND/outbox.sending.$$" u="$ND/outbox.unsent.$$" nq qt ti m i=0 failed=0 repeat=0
  t="$(now)"; msg="$(printf '%s' "$2" | oneline 900)"
  if ! lock; then
    printf '%s\t%s\t%s\n' "$t" "$title" "$msg" >> "$ND/outbox.$$"
    say "the state lock is busy: notice queued in $ND/outbox.$$"; return 0
  fi
  if [ -f "$ND/notified" ] && awk -F'\t' -v k="$key" -v c=$((t - DEDUP)) '$1 == k && $2 > c { f = 1 } END { exit !f }' "$ND/notified"; then
    repeat=1
  else
    { [ -f "$ND/notified" ] && awk -F'\t' -v c=$((t - DEDUP)) '$2 > c' "$ND/notified"; printf '%s\t%s\n' "$key" "$t"; } > "$ND/notified.new"
    mv -f "$ND/notified.new" "$ND/notified"
  fi
  take_queue "$q"
  unlock
  # sent without the lock (a slow notifier mustn't hold the other lanes): oldest first, stop at a failure
  nq="$(grep -c '' "$q")"
  if [ "$repeat" = 1 ]; then
    say "notice not repeated (sent within the last $((DEDUP / 60)) min): $key"
  else
    printf '%s\t%s\t%s\n' "$t" "$title" "$msg" >> "$q"
  fi
  : > "$u"
  while IFS=$'\t' read -r qt ti m; do
    i=$((i + 1))
    if [ "$failed" = 0 ]; then
      if [ "$i" -le "$nq" ]; then send_raw "$m (queued $(stamp "$qt"))" "$ti" && continue
      else send_raw "$m" "$ti" && continue; fi
      failed=1
    fi
    printf '%s\t%s\t%s\n' "$qt" "$ti" "$m" >> "$u"
  done < "$q"
  rm -f "$q"
  if [ -s "$u" ]; then
    if lock; then
      { cat "$u"; cat "$ND/outbox" 2> /dev/null; true; } | tail -n 50 > "$ND/outbox.new" && mv -f "$ND/outbox.new" "$ND/outbox"
      unlock; say "notice queued in $ND/outbox (the notifier didn't deliver it)"
    else cat "$u" >> "$ND/outbox.$$"; say "notice queued in $ND/outbox.$$ (the notifier didn't deliver it, the lock is busy)"; fi
  fi
  rm -f "$u"
}

# ---- circuit breaker --------------------------------------------------------------------------------
bfile() { echo "$ND/breaker/$(printf '%s' "$1" | tr -c 'A-Za-z0-9._-' '_')"; }
b_read() {   # HOST → B_COUNT B_LAST B_PROBE B_REASON
  B_COUNT=0 B_LAST=0 B_PROBE=0 B_REASON=""
  local f; f="$(bfile "$1")"
  [ -f "$f" ] && IFS=$'\t' read -r B_COUNT B_LAST B_PROBE B_REASON < "$f"
  case "$B_COUNT$B_LAST$B_PROBE" in *[!0-9]* | "") B_COUNT=0 B_LAST=0 B_PROBE=0 ;; esac
  [ $(($(now) - B_LAST)) -gt "$BREAKER_TTL" ] && B_COUNT=0
  return 0
}
b_write() { printf '%s\t%s\t%s\t%s\n' "$B_COUNT" "$B_LAST" "$B_PROBE" "$B_REASON" > "$(bfile "$1")"; }
b_fail() { lock || { say "the state lock is busy: $1's failure not counted"; return 0; }; b_read "$1"; B_COUNT=$((B_COUNT + 1)) B_LAST="$(now)" B_REASON="$2"; b_write "$1"; unlock; }
b_ok() { lock || { say "the state lock is busy: $1's breaker not reset"; return 0; }; rm -f "$(bfile "$1")"; unlock; }
probe() {   # HOST → 0 when it answers (any HTTP status), bounded
  if [ -n "${NETGUARD_PROBE:-}" ]; then NG_HOST="$1" within 40 /bin/bash -c "$NETGUARD_PROBE" > /dev/null 2>&1; return; fi
  if [ "$kind" = ssh ]; then within 20 nc -z -G 15 "$1" 22 > /dev/null 2>&1; return; fi
  local url="https://$1/" code
  case "$1" in *:443) ;; *:*) url="http://$1/" ;; esac
  code="$(within 35 curl -sS -o /dev/null -w '%{http_code}' --connect-timeout 15 --max-time 30 "$url" 2> /dev/null)" \
    && [ "$code" != 000 ]
}
control() {   # → a phrase: is the network up, judged by a known-good host
  local c="$CONTROL_HOST"; [ "$c" = "$host" ] && c=www.apple.com
  local ok=1
  if [ -n "${NETGUARD_CONTROL_PROBE:-}" ]; then NG_HOST="$c" within 40 /bin/bash -c "$NETGUARD_CONTROL_PROBE" > /dev/null 2>&1 || ok=0
  else NETGUARD_PROBE="" kind=download probe "$c" || ok=0; fi
  if [ "$ok" = 1 ]; then echo "$c answers, so only ${host:-this host} fails"; else echo "$c doesn't answer either: network down?"; fi
}

# ---- incidents --------------------------------------------------------------------------------------
incident() {   # OUTCOME EXIT REASON: one line in incidents.log; the open file goes
  local line; line="$(printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s' "$(now)" "$(stamp "$first_fail")" "$prog" "${host:-?}" "$kind" "$phase" "$attempt" "$2" "$1" "$(printf '%s' "$3" | oneline 400)")"
  if lock; then printf '%s\n' "$line" >> "$ND/incidents.log"; unlock; else say "the state lock is busy: incident not recorded: $line"; fi
  rm -f "$ND/open/$id"
}
open_incident() {   # still failing (retrying): summary finds it if this run is cut short
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$(now)" "$(stamp "$first_fail")" "$prog" "${host:-?}" "$kind" "$phase" "$attempt" "$1" "unfinished" "$(printf '%s' "$2" | oneline 400)" > "$ND/open/$id"
}

# ---- summary, preflight, reset ----------------------------------------------------------------------
do_summary() {
  local seen=0 total=0 since=0 t lines n interrupted="" breakers="" queued="" f msg pre=""
  t="$(now)"
  # summary.last: "LINES TIME", the incidents.log lines already summarized and when
  [ -f "$ND/summary.last" ] && read -r seen since < "$ND/summary.last"
  case "$seen$since" in "" | *[!0-9]*) seen=0 since=0 ;; esac
  [ -f "$ND/incidents.log" ] && total="$(grep -c '' "$ND/incidents.log")"
  [ "$total" -lt "$seen" ] && seen=0   # the log was cut: all of it
  lines="$( [ -f "$ND/incidents.log" ] && tail -n +$((seen + 1)) "$ND/incidents.log")"
  n="$(printf '%s' "$lines" | grep -c . || true)"
  for f in "$ND"/open/*; do [ -f "$f" ] || continue
    local pid="${f##*.}"; alive "$pid" && continue
    interrupted="$interrupted$(cat "$f")"$'\n'; rm -f "$f"
  done
  for f in "$ND"/breaker/*; do [ -f "$f" ] || continue
    local h; h="$(basename "$f")"; b_read "$h"
    [ "$B_COUNT" -ge "$BREAK_AFTER" ] && breakers="$breakers $h ($B_COUNT failures: $B_REASON);"
  done
  if lock; then take_queue "$ND/outbox.summary.$$"; unlock
    [ -s "$ND/outbox.summary.$$" ] && queued="$(cut -f3 "$ND/outbox.summary.$$")"; rm -f "$ND/outbox.summary.$$"
  else say "the state lock is busy: the outbox is left for the next summary"; fi
  [ -f "$ND/preflight.state" ] && grep -q '^result=bad' "$ND/preflight.state" && pre="$(awk -F'\t' '$1 == "bad" { printf "%s %s (%s); ", $2, $3, $4 }' "$ND/preflight.state")"
  echo "netguard summary since $([ "$since" = 0 ] && echo 'the start' || stamp "$since"):"
  if [ -n "$lines" ]; then
    echo "$n incident(s):"
    printf '%s\n' "$lines" | awk -F'\t' '{ printf "  %s  %s -> %s (%s): %s, %s attempt(s), exit %s, first failure %s: %s\n", substr($2, 12), $3, $4, $5, $9, $7, $8, $2, $10 }'
  else echo "no incidents"; fi
  [ -n "$interrupted" ] && { echo "interrupted while failing:"; printf '%s' "$interrupted" | awk -F'\t' '{ printf "  %s -> %s (%s): %s, first failure %s\n", $3, $4, $5, $10, $2 }'; }
  [ -n "$breakers" ] && echo "breakers open:$breakers (tests/harness/netguard.sh reset HOST)"
  [ -n "$pre" ] && echo "preflight not OK: $pre"
  [ -n "$queued" ] && { echo "notices that couldn't go out:"; printf '%s\n' "$queued" | sed 's/^/  /'; }
  echo "$total $t" > "$ND/summary.last"
  if [ -n "$lines$interrupted$breakers$queued$pre" ]; then
    msg="$n incident(s)"
    [ -n "$lines" ] && msg="$msg, first: $(printf '%s\n' "$lines" | head -1 | awk -F'\t' '{ printf "%s -> %s at %s (%s, %s)", $3, $4, $2, $6, $9 }')"
    [ -n "$interrupted" ] && msg="$msg; $(printf '%s' "$interrupted" | grep -c .) interrupted"
    [ -n "$breakers" ] && msg="$msg; breakers open:$breakers"
    [ -n "$pre" ] && msg="$msg; preflight not OK: $pre"
    [ -n "$queued" ] && msg="$msg; $(printf '%s\n' "$queued" | grep -c .) undelivered notice(s) folded in"
    notify "summary|$t|$$" "$msg. Details: tests/harness/netguard.sh summary output, $ND/incidents.log" "netguard: night summary"
  fi
  return 0
}

do_preflight() {
  local nc="${NETGUARD_NETCHECK:-$here/netcheck.sh}" tsv="$ND/preflight.rows" st="$ND/preflight.state" rc bad t
  rm -f "$tsv"; t="$(now)"
  NETCHECK_TSV="$tsv" "$0" --plain --kind once --host "" --stall "${NETGUARD_PREFLIGHT_STALL:-180}" -- "$nc" --no-ai "$@"; rc=$?
  bad="$( [ -f "$tsv" ] && awk -F'\t' '$1 != "OK" && $1 != "SKIP"' "$tsv")"
  {
    echo "# netguard preflight (netcheck.sh --no-ai): a lane needing a host on a bad line waits for the owner"
    echo "time=$t ($(stamp "$t"))"
    if [ -s "$tsv" ] && [ -z "$bad" ] && [ "$rc" = 0 ]; then echo "result=ok"; else echo "result=bad"; fi
    [ -n "$bad" ] && printf '%s\n' "$bad" | awk -F'\t' '{ printf "bad\t%s\t%s\t%s\n", $2, $3, $1 }'
    [ "$rc" = 124 ] && printf 'bad\tnetcheck.sh\t(the check itself)\tstalled\n'
    [ -s "$tsv" ] || [ "$rc" = 124 ] || printf 'bad\tnetcheck.sh\t(the check itself)\tno rows (exit %s)\n' "$rc"
  } > "$st"
  if grep -q '^result=ok' "$st"; then say "preflight: every connection OK ($st)"; return 0; fi
  local list; list="$(awk -F'\t' '$1 == "bad" { printf "%s%s %s (%s)", (n++ ? ", " : ""), $2, $3, $4 }' "$st")"
  notify "preflight|$list" "Evening check not OK: $list. Lanes needing these wait. At the Mac: tests/harness/netcheck.sh (Little Snitch: Allow + Forever), or check the network." "netguard: preflight"
  say "preflight not OK: $list ($st)"
  [ "$rc" = 124 ] && return 124
  return 1
}

case "${1:-}" in
  summary) shift; [ $# = 0 ] || usage; do_summary; exit ;;
  preflight) shift; kind=once host=""; do_preflight "$@"; exit ;;
  reset) [ $# = 2 ] || usage; rm -f "$(bfile "$2")"; say "breaker for $2 closed"; exit 0 ;;
esac

# ---- a run ------------------------------------------------------------------------------------------
host="" kind=once stall=600 watch="" host_given=0 plain=0
while [ $# -gt 0 ]; do
  case "$1" in
    --host) [ $# -ge 2 ] || usage; host="$2" host_given=1; shift 2 ;;
    --kind) [ $# -ge 2 ] || usage; kind="$2"; shift 2 ;;
    --stall) [ $# -ge 2 ] || usage; stall="$2"; shift 2 ;;
    --watch) [ $# -ge 2 ] || usage; watch="$2"; shift 2 ;;
    --plain) plain=1; shift ;;   # preflight's own: a failure that isn't a stall passes through as is
    --) shift; break ;;
    *) usage ;;
  esac
done
[ $# -ge 1 ] || usage
case "$kind" in download | git | gh | push | ai | ssh | once) ;; *) say "unknown --kind $kind"; usage ;; esac
case "$stall" in "" | *[!0-9]*) say "--stall takes whole seconds"; usage ;; esac
[ "$stall" -ge 1 ] && [ "$stall" -le 600 ] || { say "--stall must be 1..600 s"; usage; }
cmd=("$@")
prog="$(basename "${cmd[0]}")"
if [ "$host_given" = 0 ]; then   # the first URL's host, when the caller named none
  for a in "${cmd[@]}"; do
    case "$a" in http://* | https://*) host="${a#*://}"; host="${host%%/*}"; host="${host##*@}"; break ;; esac
  done
fi

# tool-level timeouts, where the tool has them
if [ "${NETGUARD_TOOL_OPTS:-1}" != 0 ]; then
  has() { local x; for x in "${cmd[@]:1}"; do case "$x" in *"$1"*) return 0 ;; esac; done; return 1; }
  sshopts=()
  for o in ConnectTimeout=15 BatchMode=yes ServerAliveInterval=15 ServerAliveCountMax=4 ControlMaster=no; do
    has "${o%%=*}" || sshopts+=(-o "$o")
  done
  case "$prog" in
    curl)
      extra=()
      has --connect-timeout || extra+=(--connect-timeout 20)
      has --speed-limit || extra+=(--speed-limit 1)
      has --speed-time || extra+=(--speed-time "${NETGUARD_SPEED_TIME:-120}")
      cmd=("${cmd[0]}" ${extra[@]+"${extra[@]}"} "${cmd[@]:1}") ;;
    ssh) cmd=("${cmd[0]}" ${sshopts[@]+"${sshopts[@]}"} "${cmd[@]:1}") ;;
    git)
      export GIT_HTTP_LOW_SPEED_LIMIT="${GIT_HTTP_LOW_SPEED_LIMIT:-1}" GIT_HTTP_LOW_SPEED_TIME="${GIT_HTTP_LOW_SPEED_TIME:-120}"
      export GIT_TERMINAL_PROMPT=0
      if [ -z "${GIT_SSH_COMMAND:-}${GIT_SSH:-}" ] && ! git config core.sshCommand > /dev/null 2>&1; then
        export GIT_SSH_COMMAND="ssh -o ConnectTimeout=15 -o BatchMode=yes -o ServerAliveInterval=15 -o ServerAliveCountMax=4 -o ControlMaster=no"
      fi ;;
  esac
fi

id="$(date +%Y%m%d-%H%M%S).$$"
LOG_OUT="$ND/logs/$id.out" LOG_ERR="$ND/logs/$id.err"
: > "$LOG_OUT"; : > "$LOG_ERR"
find "$ND/logs" -type f -mtime +7 -delete 2> /dev/null
started="$(now)" next_report=$(($(now) + REPORT)) attempt=0 retries=0 cretries=0 first_fail=0 phase="" pid="" state=running
last_prog="$started" last_ds="$(now_ds)" net_note=unknown
cleanup_exit() { rm -f "$ND/running/$id" "$ESC"; }
trap cleanup_exit EXIT

# The launcher: own process group; CMD's output copied to ours and to the logs; exits with CMD's
# status (128+signal when killed). If netguard goes away (its parent changes), it stops the group.
LAUNCHER='
use strict; use POSIX ();
my ($grace, $lo_path, $le_path) = splice(@ARGV, 0, 3);
setpgrp(0, 0);
my $boss = getppid();
pipe(my $or, my $ow) or die "pipe: $!"; pipe(my $er, my $ew) or die "pipe: $!";
my $kid = fork(); die "fork: $!" unless defined $kid;
if (!$kid) {
  close $or; close $er;
  $SIG{INT} = "DEFAULT"; $SIG{QUIT} = "DEFAULT";
  open(STDOUT, ">&", $ow) or POSIX::_exit(127); open(STDERR, ">&", $ew) or POSIX::_exit(127);
  close $ow; close $ew;
  exec { $ARGV[0] } @ARGV;
  print STDERR "netguard: cannot run $ARGV[0]: $!\n"; POSIX::_exit(127);
}
close $ow; close $ew;
$SIG{TERM} = $SIG{INT} = $SIG{HUP} = $SIG{PIPE} = "IGNORE";
open(my $lo, ">>", $lo_path) or die; open(my $le, ">>", $le_path) or die;
my %to = (fileno($or) => [\*STDOUT, $lo], fileno($er) => [\*STDERR, $le]);
my @open = ($or, $er); my ($status, $done);
while (1) {
  my $rin = ""; vec($rin, fileno($_), 1) = 1 for @open;
  if (@open && select(my $rout = $rin, undef, undef, 0.3) > 0) {
    for my $fh (@open) {
      next unless vec($rout, fileno($fh), 1);
      my $n = sysread($fh, my $buf, 65536);
      if (!$n) { @open = grep { $_ != $fh } @open; next; }
      my ($a, $b) = @{$to{fileno($fh)}}; syswrite($a, $buf); syswrite($b, $buf);
    }
  } elsif (!@open) { select(undef, undef, undef, 0.3); }
  if (!defined $status && waitpid($kid, POSIX::WNOHANG()) == $kid) { $status = $?; $done = time; }
  last if defined $status && (!@open || time - $done >= 2);
  if (getppid() != $boss) { kill "TERM", -$$; sleep $grace; kill "KILL", -$$; }
}
exit(($status & 127) ? 128 + ($status & 127) : $status >> 8);
'

sizes() { stat -f %z "$LOG_OUT" "$LOG_ERR" ${watch:+"$watch"} 2> /dev/null | awk '{ s += $1 } END { print s + 0 }'; }
gpids() { ps -A -o pid=,pgid= 2> /dev/null | awk -v g="$1" '$2 == g { printf "%s ", $1 }'; }
group_alive() { kill -0 -- "-$1" 2> /dev/null; }
ESC="$ND/running/$$.escaped"
track_escaped() {   # descendants that left the group (setsid, setpgrp), kept with their start time
  ps -A -o pid=,ppid=,pgid= 2> /dev/null | awk -v L="$pid" '{ p[$1] = $2; g[$1] = $3 }
    END { t[L] = 1; do { c = 0; for (x in p) if (!(x in t) && ((p[x] in t) || g[x] == L)) { t[x] = 1; c = 1 } } while (c)
          for (x in t) if ((x in g) && g[x] != L) print x }' | while read -r e; do
    grep -q "^$e " "$ESC" 2> /dev/null || echo "$e $(ps -o lstart= -p "$e" 2> /dev/null | tr -s ' ')" >> "$ESC"
  done
}
escaped_alive() {   # SIG: signals (0: lists) the escaped ones still running as the same process
  [ -s "$ESC" ] || return 1
  local e st f=1
  while read -r e st; do
    [ -n "$st" ] && [ "$(ps -o lstart= -p "$e" 2> /dev/null | tr -s ' ' | sed 's/^ //; s/ $//')" = "$st" ] || continue
    f=0; [ "$1" = 0 ] && printf '%s ' "$e" || kill "-$1" "$e" 2> /dev/null
  done < "$ESC"
  return $f
}
net_bytes() {   # PIDS... → their nettop bytes in+out; fails when there is no telemetry
  local o; o="$(within 15 nettop -P -L 1 -J bytes_in,bytes_out -x 2> /dev/null)" || return 1
  printf '%s\n' "$o" | head -1 | grep -q bytes_in || return 1
  printf '%s\n' "$o" | awk -F, -v want=" $* " 'NR > 1 { n = $1; sub(/.*\./, "", n); if (index(want, " " n " ")) s += $2 + $3 } END { print s + 0 }'
}
status_line() {   # writes and prints the status; notifies when the step isn't healthy
  local t="$1" what="$2" line quiet
  quiet=$((($(now_ds) - last_ds) / 10))
  line="$prog -> ${host:-?} ($kind): unfinished after $(((t - started) / 60)) min $(((t - started) % 60)) s, attempt $attempt, $what, last progress $quiet s ago, network bytes: $net_note"
  say "$line"
  printf '%s\t%s\n' "$t" "$line" > "$ND/running/$id"
  if [ "$what" != running ] || [ "$quiet" -ge "$REPORT" ] || [ "${NETGUARD_NTFY_HEALTHY:-0}" = 1 ]; then
    notify "status|$prog|${host:-?}" "$line" "netguard: $prog -> ${host:-?}"
  fi
}
maybe_status() { local t; t="$(now)"; if [ "$t" -ge "$next_report" ]; then next_report=$((t + REPORT)); status_line "$t" "$1"; fi; }
nap() {   # SECS: sleeps in ticks, keeping the status lines going
  local end; end="$(perl -MTime::HiRes=time -e 'printf "%.2f", time + $ARGV[0]' "$1")"
  while perl -MTime::HiRes=time -e 'exit !(time < $ARGV[0])' "$end"; do sleep "$TICK"; maybe_status backoff; done
}

stop_group() {   # PGID: TERM, grace, KILL, reap; sets CLEAN
  local g="$1" i=0 steps
  steps="$(perl -e 'print int($ARGV[0] * 10)' "$GRACE")"
  track_escaped
  kill -TERM -- "-$g" 2> /dev/null; escaped_alive TERM
  while { group_alive "$g" || escaped_alive 0 > /dev/null; } && [ "$i" -lt "$steps" ]; do sleep 0.1; i=$((i + 1)); done
  group_alive "$g" && kill -KILL -- "-$g" 2> /dev/null; escaped_alive KILL
  wait "$g" 2> /dev/null
  i=0; while { group_alive "$g" || escaped_alive 0 > /dev/null; } && [ "$i" -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
  if group_alive "$g" || escaped_alive 0 > /dev/null; then CLEAN="processes left: $(gpids "$g")$(escaped_alive 0)"; say "$CLEAN"; else CLEAN="stopped and reaped"; fi
  rm -f "$ESC"
}
on_signal() {
  [ -n "$pid" ] && kill -0 "$pid" 2> /dev/null && stop_group "$pid"
  [ "$first_fail" != 0 ] && incident interrupted "$1" "netguard was stopped (signal) while retrying: ${reason:-}"
  exit "$1"
}
trap 'on_signal 143' TERM; trap 'on_signal 130' INT; trap 'on_signal 129' HUP

run_attempt() {   # → rc, killed (stall0: nothing ever moved; stall: it had moved), text
  local off_o off_e t tds s last_s net_prev="" net_last=0 moved=0
  off_o="$(stat -f %z "$LOG_OUT")" off_e="$(stat -f %z "$LOG_ERR")"
  killed=""
  exec 3<&0
  perl -e "$LAUNCHER" "$GRACE" "$LOG_OUT" "$LOG_ERR" "${cmd[@]}" <&3 &
  pid=$!
  exec 3<&-
  last_s="$(sizes)" last_prog="$(now)" last_ds="$(now_ds)"
  local n=0
  while kill -0 "$pid" 2> /dev/null; do
    if [ "$n" -lt 10 ]; then sleep 0.2; n=$((n + 1)); else sleep "$TICK"; fi
    tds="$(now_ds)"; t=$((tds / 10)); s="$(sizes)"
    track_escaped
    if [ "$s" != "$last_s" ]; then last_s="$s" last_prog="$t" last_ds="$tds" moved=1; fi
    if [ "$NETTOP" = 1 ] && [ $((t - last_prog)) -ge "$NETTOP_AFTER" ] && [ $((t - net_last)) -ge "$NETTOP_AFTER" ]; then
      net_last="$t"
      local nb; if nb="$(net_bytes "$pid" $(gpids "$pid"))"; then
        if [ -n "$net_prev" ] && [ "$nb" -gt "$net_prev" ]; then last_prog="$t" last_ds="$tds" moved=1 net_note=moving; else net_note=quiet; fi
        net_prev="$nb"
      else NETTOP=0 net_note=unknown; fi
    fi
    maybe_status running
    if [ $((tds - last_ds)) -ge $((stall * 10)) ]; then
      kill -0 "$pid" 2> /dev/null || break   # it ended during the last tick: not a stall
      if [ "$moved" = 1 ]; then killed=stall; else killed=stall0; fi
      say "$prog -> ${host:-?}: no progress for $stall s, stopping it"
      stop_group "$pid"
      break
    fi
  done
  wait "$pid" 2> /dev/null; rc=$?
  pid=""
  # the end of each stream (errors are there; a model's answer earlier may quote HTTP codes)
  text="$( { tail -c +$((off_o + 1)) "$LOG_OUT" | tail -n 10; tail -c +$((off_e + 1)) "$LOG_ERR" | tail -n 30; } | tail -c 16384)"
}

classify() {   # → cls (ok usage http4xx transient connect stall0 stall other), code, retry_after
  local lc; code="" retry_after="" ssh_pre=0
  lc="$(printf '%s' "$text" | tr 'A-Z' 'a-z')"
  if [ "$killed" = stall0 ]; then cls=stall0; return; fi
  if [ "$killed" = stall ]; then cls=stall; return; fi
  if [ "$rc" = 0 ]; then cls=ok; return; fi
  case "$rc" in 126 | 127 | 64) cls=usage; return ;; esac
  if [ "$plain" = 1 ]; then cls=other; return; fi
  # ssh's own errors before a session (exit 255): the remote command never ran
  [ "$rc" = 255 ] && printf '%s' "$lc" | grep -Eq 'ssh: connect to host .* port [0-9]+|during banner exchange|kex_exchange_identification|could not resolve hostname' && ssh_pre=1
  code="$(printf '%s' "$lc" | grep -Eo '(returned error|error|http|status( code)?)[: ]+[45][0-9][0-9]([^0-9]|$)' | tail -1 | grep -Eo '[45][0-9][0-9]' | tail -1)"
  retry_after="$(printf '%s' "$lc" | grep -Eo 'retry-after: *[0-9]+' | tail -1 | grep -Eo '[0-9]+$')"
  if [ -n "$code" ]; then
    case "$code" in 5?? | 408 | 429) cls=transient ;; *) cls=http4xx ;; esac
    return
  fi
  if printf '%s' "$lc" | grep -Eq 'operation too slow|timed out after [0-9]+ milliseconds with [1-9][0-9]* (out of [0-9-]+ )?bytes'; then cls=transient; return; fi
  if printf '%s' "$lc" | grep -Eq "could not resolve|couldn.t resolve|resolving timed out|no such host|nodename nor servname|name or service not known|temporary failure in name resolution"; then cls=transient dns=1; return; fi
  if printf '%s' "$lc" | grep -Eq 'failed to connect|connection timed out|connect timeout|operation timed out|connection refused|network is unreachable|no route to host|during banner exchange|i/o timeout|timed out after [0-9]+ milliseconds with 0 bytes'; then cls=connect; return; fi
  if printf '%s' "$lc" | grep -Eq "connection reset|reset by peer|broken pipe|early eof|unexpected disconnect|rpc failed|transfer closed|recv failure|send failure|ssl_error_syscall|ssl connect error|stream disconnected|error sending request|http/2 stream|empty reply|remote end hung up|tls handshake timeout|connection closed by|kex_exchange_identification|server .* not responding|closed by remote host|bad gateway|service unavailable|gateway time-?out"; then cls=transient; return; fi
  if [ "$prog" = curl ]; then
    case "$rc" in 6) cls=transient dns=1; return ;; 7 | 28) cls=connect; return ;; 16 | 18 | 35 | 52 | 55 | 56 | 92) cls=transient; return ;; esac
  fi
  cls=other
}

backoff() {   # → delay (empty: no retry) for cls under kind; bumps the counters
  local base="" t1 t2
  set -- $BACKOFF_T; t1="${1:-30}" t2="${2:-120}"
  case "$cls" in transient | stall | connect | stall0) ;; *) delay=""; return ;; esac
  case "$kind" in
    download | git | gh)
      if [ "$cls" = connect ] || [ "$cls" = stall0 ]; then
        [ "$cretries" -lt 1 ] && [ "$retries" -lt 2 ] && { base="$BACKOFF_C"; cretries=$((cretries + 1)); }
      elif [ "$retries" = 0 ]; then base="$t1"; elif [ "$retries" = 1 ]; then base="$t2"; fi ;;
    push | ai) [ "$retries" -lt 1 ] && base="$BACKOFF_1" ;;
    ssh) [ "$cls" = connect ] && [ "$ssh_pre" = 1 ] && [ "$retries" -lt 1 ] && base="$BACKOFF_C" ;;
    once) ;;
  esac
  if [ -n "$base" ] && [ -n "$retry_after" ]; then
    if [ "$retry_after" -gt "$RA_MAX" ]; then base=""; reason="$reason (Retry-After ${retry_after} s is too long)"
    elif [ "$retry_after" -gt "${base%%.*}" ]; then base="$retry_after"; fi
  fi
  delay="$base"
  [ -n "$delay" ] || return 0
  retries=$((retries + 1))
  if [ "$JITTER" != 0 ]; then case "$delay" in *[!0-9]*) ;; *) delay=$((delay + RANDOM % (delay / 5 + 1))) ;; esac; fi
}

explain() {   # → reason for cls
  local last; last="$(printf '%s\n' "$text" | grep -v '^[[:space:]]*$' | tail -1 | oneline 200)"
  case "$cls" in
    stall0) reason="no output or progress for $stall s: $LS_NOTE" ;;
    stall) reason="stalled mid-transfer for $stall s: $LS_NOTE" ;;
    connect)
      if printf '%s' "$text" | grep -Eqi 'timed out|timeout'; then reason="connect timeout: $LS_NOTE ($last)"
      else reason="could not connect: $last"; fi ;;
    transient) reason="${code:+HTTP $code: }$([ "$dns" = 1 ] && echo 'DNS: ')$last" ;;
    http4xx) reason="HTTP $code: $last" ;;
    *) reason="exit $rc: $last" ;;
  esac
  if [ "$dns" = 1 ]; then reason="$reason; $(control)"; else
    case "$cls" in stall0 | stall | connect) reason="$reason; $(control)" ;; esac
  fi
  return 0
}

gate() {   # fails fast while the host's breaker is open
  [ -n "$host" ] || return 0
  b_read "$host"
  [ "$B_COUNT" -ge "$BREAK_AFTER" ] || return 0
  local t; t="$(now)"
  if [ $((t - B_PROBE)) -ge "$PROBE_EVERY" ]; then
    if probe "$host"; then b_ok "$host"; say "breaker for $host closed: a probe got through"; return 0; fi
    if lock; then b_read "$host"; B_PROBE="$t"; b_write "$host"; unlock; fi
  fi
  first_fail="$B_LAST" phase=breaker
  reason="not run: $B_COUNT consecutive failures to $host (last: $B_REASON), probe at $(stamp "$B_PROBE") failed; tests/harness/netguard.sh reset $host after fixing it"
  say "$prog -> $host: $reason"
  incident breaker-open 75 "$reason"
  notify "breaker|$host" "$prog -> $host: $reason" "netguard: $host blocked"
  exit 75
}

gate
reason=""
while :; do
  attempt=$((attempt + 1)) dns=0
  run_attempt
  classify
  if [ "$cls" = ok ]; then
    [ -n "$host" ] && b_ok "$host"
    if [ "$first_fail" != 0 ]; then incident recovered 0 "after: $reason"; fi
    rm -f "$LOG_OUT" "$LOG_ERR"
    exit 0
  fi
  phase="$cls"; [ "$dns" = 1 ] && phase=dns
  [ -n "$code" ] && phase="$phase HTTP $code"
  explain
  case "$cls" in
    transient | connect | stall0 | stall) [ -n "$host" ] && b_fail "$host" "$reason" ;;
    http4xx) [ -n "$host" ] && b_ok "$host" ;;
  esac
  if [ "$cls" = other ] || [ "$cls" = usage ]; then
    if [ "$first_fail" != 0 ]; then incident failed "$rc" "$reason"; else rm -f "$LOG_OUT" "$LOG_ERR"; fi
    exit "$rc"
  fi
  [ "$first_fail" != 0 ] || first_fail="$(now)"
  final="$rc"; [ -n "$killed" ] && final=124
  backoff
  if [ -n "$delay" ] && [ -n "$host" ]; then
    b_read "$host"; [ "$B_COUNT" -ge "$BREAK_AFTER" ] && { delay=""; reason="$reason; breaker now open for $host"; }
  fi
  if [ -z "$delay" ] && [ "$plain" = 1 ]; then exit "$final"; fi   # preflight reports it itself
  if [ -z "$delay" ]; then
    incident gave-up "$final" "$reason"
    say "$prog -> ${host:-?}: gave up after $attempt attempt(s): $reason (log: $LOG_ERR)"
    notify "fail|$prog|${host:-?}|${phase%% *}" "$prog -> ${host:-?} ($kind) failed after $attempt attempt(s): $reason. Exit $final. Log: $LOG_ERR" "netguard: $prog -> ${host:-?}"
    exit "$final"
  fi
  open_incident "$final" "$reason"
  say "$prog -> ${host:-?}: $reason; retry $retries in $delay s"
  nap "$delay"
done

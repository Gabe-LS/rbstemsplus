#!/bin/bash
# netguard.sh's own test, with fakes only (a fake notifier, fake commands and probes; no network, no
# ntfy), short timeouts through its environment overrides; about 40 s. Run by selftest.sh.
#   tests/harness/netguard-test.sh
set -uo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd -P)"
NG="$root/tests/harness/netguard.sh"
t="$root/build/harness/netguard-test"; rm -rf "$t"; mkdir -p "$t/bin" "$t/path"
# Keep Git discovery inside the checkout or the test fixtures, including in child scripts.
unset GIT_DIR GIT_WORK_TREE GIT_COMMON_DIR GIT_INDEX_FILE GIT_OBJECT_DIRECTORY GIT_ALTERNATE_OBJECT_DIRECTORIES
export GIT_CEILING_DIRECTORIES="$(dirname "$root"):$t"
pass=0; fail=0
ok() { if eval "$1"; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "FAIL: $2"; fi; }

# fakes: a notifier that logs, one that fails; a command that fails N times with a given error first
cat > "$t/bin/notify" << 'EOF'
#!/bin/bash
printf '%s|%s\n' "$2" "$1" >> "$NG_TEST_NOTICES"; echo sent
EOF
printf '#!/bin/bash\nexit 1\n' > "$t/bin/notify-fail"
cat > "$t/bin/flaky" << 'EOF'
#!/bin/bash
# flaky COUNTER FAILS RC MESSAGE: fails FAILS times (MESSAGE on stderr, exit RC), then prints ok
n=$(($(cat "$1" 2> /dev/null || echo 0) + 1)); echo "$n" > "$1"
if [ "$n" -le "$2" ]; then echo "$4" >&2; exit "$3"; fi
echo ok
EOF
printf '#!/bin/bash\necho "args: $*"\n' > "$t/path/ssh"
cat > "$t/path/curl" << 'EOF'
#!/bin/bash
if [ -z "${NG_TEST_CURL_DIR:-}" ]; then echo "args: $*"; exit; fi
while [ "$#" -gt 0 ] && [ "$1" != -o ]; do shift; done
[ "$#" -ge 2 ] || exit 1
exec > "$2"
printf fake
printf '%s\n' "$2" > "$NG_TEST_CURL_DIR/partial"
for ((i=0; i<100; i++)); do
  if [ -f "$NG_TEST_CURL_DIR/finish" ]; then echo ' asset'; exit; fi
  sleep 0.1
done
exit 1
EOF
chmod +x "$t/bin/"* "$t/path/"*

export NETGUARD_TICK=0.2 NETGUARD_GRACE=0.5 NETGUARD_JITTER=0 NETGUARD_BACKOFF_TRANSIENT="0 0" \
  NETGUARD_BACKOFF_CONNECT=0 NETGUARD_BACKOFF_ONE=0 NETGUARD_CONTROL_PROBE=true NETGUARD_PROBE=false \
  NETGUARD_PROBE_EVERY=0 NETGUARD_NETTOP=0 NETGUARD_NOTIFY="$t/bin/notify" NETGUARD_REPORT=600
fresh() { d="$t/$1"; mkdir -p "$d"; export NETGUARD_DIR="$d/state" NG_TEST_NOTICES="$d/notices"; : > "$d/notices"; }
notices() { cat "$NG_TEST_NOTICES" 2> /dev/null | grep -c .; }
incidents() { cat "$NETGUARD_DIR/incidents.log" 2> /dev/null | grep -c .; }
field() { awk -F'\t' -v n="$1" 'END { print $n }' "$NETGUARD_DIR/incidents.log"; }   # of the last incident
secs() { perl -MTime::HiRes=time -e 'printf "%.1f", time - $ARGV[0]' "$1"; }
start() { perl -MTime::HiRes=time -e 'printf "%.3f", time'; }

# a command that hangs silent: killed after the stall, retried once (a connect-like timeout), then one
# incident and one notice that says the cause is unknown
fresh hang; s="$(start)"
"$NG" --kind download --host h.test --stall 1 -- sleep 30 > "$d/out" 2> "$d/err"; rc=$?
ok '[ "$rc" = 124 ] && [ "$(incidents)" = 1 ] && [ "$(field 6)" = stall0 ] && [ "$(field 7)" = 2 ] && [ "$(field 9)" = gave-up ]' "silent hang: killed after the stall (exit 124), retried once, one incident (got rc $rc, $(tail -1 "$NETGUARD_DIR/incidents.log" 2> /dev/null))"
ok '[ "$(notices)" = 1 ] && grep -q "timeout, cause unknown (Little Snitch prompt or deny, or the network)" "$NG_TEST_NOTICES" && grep -q "github.com answers, so only h.test fails" "$NG_TEST_NOTICES"' "silent hang: one notice, cause unknown, the control host's answer"
ok 'perl -e "exit !(\$ARGV[0] < 8)" "$(secs "$s")"' "silent hang: done in seconds ($(secs "$s") s)"
NETGUARD_CONTROL_PROBE=false "$NG" --kind once --host h2.test --stall 1 -- sleep 30 > /dev/null 2>&1
ok 'grep -q "github.com doesn.t answer either: network down?" "$NG_TEST_NOTICES"' "control host down too: 'network down?'"

# a command that streams slowly: never killed, the status lines still come, no notice (healthy)
fresh slow
NETGUARD_REPORT=1 "$NG" --kind ai --host api.test --stall 1 -- /bin/bash -c 'for i in 1 2 3 4 5 6 7 8 9 10 11 12; do echo $i; sleep 0.3; done' > "$d/out" 2> "$d/err"; rc=$?
ok '[ "$rc" = 0 ] && [ "$(grep -c . "$d/out")" = 12 ] && [ "$(incidents)" = 0 ]' "slow stream: never killed, output whole, no incident"
ok '[ "$(grep -c "unfinished after" "$d/err")" -ge 2 ] && [ "$(notices)" = 0 ]' "slow stream: status lines every report interval (1 s here), no notice while healthy"
NETGUARD_REPORT=1 NETGUARD_NTFY_HEALTHY=1 "$NG" --kind ai --host api.test --stall 2 -- /bin/bash -c 'for i in 1 2 3 4 5 6 7 8; do echo $i; sleep 0.3; done' > /dev/null 2>&1
ok '[ "$(notices)" = 1 ] && grep -q "unfinished after" "$NG_TEST_NOTICES"' "NETGUARD_NTFY_HEALTHY=1: the status notice too, deduplicated"

# transient, then success: retried, recorded as recovered, no notice
fresh transient
"$NG" --kind download --host t.test -- "$t/bin/flaky" "$d/n" 1 56 "curl: (56) Recv failure: Connection reset by peer" > "$d/out" 2> "$d/err"; rc=$?
ok '[ "$rc" = 0 ] && [ "$(cat "$d/out")" = ok ] && [ "$(cat "$d/n")" = 2 ] && [ "$(field 9)" = recovered ] && [ "$(field 6)" = transient ] && [ "$(notices)" = 0 ]' "transient then success: one retry, 'recovered', no notice"
fresh transient2
"$NG" --kind download --host t.test -- "$t/bin/flaky" "$d/n" 5 128 "fatal: unable to access 'https://t.test/x/': The requested URL returned error: 503" > /dev/null 2>&1; rc=$?
ok '[ "$rc" = 128 ] && [ "$(cat "$d/n")" = 3 ] && [ "$(field 6)" = "transient HTTP 503" ] && [ "$(notices)" = 1 ]' "503 every time: 2 retries for a download, then one notice"
fresh retryafter; s="$(start)"
"$NG" --kind download --host r.test -- "$t/bin/flaky" "$d/n" 1 22 "< Retry-After: 2
curl: (22) The requested URL returned error: 429" > /dev/null 2>&1; rc=$?
ok '[ "$rc" = 0 ] && [ "$(cat "$d/n")" = 2 ] && perl -e "exit !(\$ARGV[0] >= 2)" "$(secs "$s")"' "429 with Retry-After: 2: waited it, then retried"
fresh retryafter2
"$NG" --kind download --host r.test -- "$t/bin/flaky" "$d/n" 1 22 "< Retry-After: 3600
curl: (22) The requested URL returned error: 429" > /dev/null 2>&1; rc=$?
ok '[ "$rc" = 22 ] && [ "$(cat "$d/n")" = 1 ]' "429 with Retry-After beyond 5 min: no retry"

# 4xx: no retry; once: never retried
fresh fourxx
"$NG" --kind download --host f.test -- "$t/bin/flaky" "$d/n" 5 22 "curl: (22) The requested URL returned error: 404" > /dev/null 2>&1; rc=$?
ok '[ "$rc" = 22 ] && [ "$(cat "$d/n")" = 1 ] && [ "$(field 6)" = "http4xx HTTP 404" ] && [ "$(notices)" = 1 ]' "404: no retry, one incident and notice"
ok '[ ! -f "$NETGUARD_DIR/breaker/f.test" ]' "404: the host answered, its breaker stays closed"
fresh once
"$NG" --kind once --host o.test -- "$t/bin/flaky" "$d/n" 5 56 "curl: (56) Recv failure: Connection reset by peer" > /dev/null 2>&1; rc=$?
ok '[ "$rc" = 56 ] && [ "$(cat "$d/n")" = 1 ] && [ "$(field 7)" = 1 ]' "once: a transient failure isn't retried"
fresh other
"$NG" --kind download --host x.test -- "$t/bin/flaky" "$d/n" 5 3 "make: *** [all] Error 2" > /dev/null 2>&1; rc=$?
ok '[ "$rc" = 3 ] && [ "$(cat "$d/n")" = 1 ] && [ "$(incidents)" = 0 ] && [ "$(notices)" = 0 ]' "a failure that isn't the network's: passed through, no incident, no notice"

# the circuit breaker: open after 3 consecutive failures, fails fast, recovers through a probe
fresh breaker
conn="curl: (7) Failed to connect to b.test port 443 after 3 ms: Connection refused"
for i in 1 2 3; do "$NG" --kind once --host b.test -- "$t/bin/flaky" "$d/n" 99 7 "$conn" > /dev/null 2>&1; done
"$NG" --kind once --host b.test -- "$t/bin/flaky" "$d/n" 99 7 "$conn" > /dev/null 2> "$d/err4"; rc=$?
ok '[ "$rc" = 75 ] && [ "$(cat "$d/n")" = 3 ] && grep -q "3 consecutive failures to b.test" "$d/err4" && [ "$(field 9)" = breaker-open ]' "breaker: after 3 failures the 4th call fails fast (exit 75) without running"
ok '[ "$(notices)" = 2 ]' "breaker: the 3 same failures give one notice, the open breaker one more"
NETGUARD_PROBE=true "$NG" --kind once --host b.test -- /bin/echo fine > "$d/out5" 2> /dev/null; rc=$?
ok '[ "$rc" = 0 ] && [ "$(cat "$d/out5")" = fine ] && [ ! -f "$NETGUARD_DIR/breaker/b.test" ]' "breaker: a probe that gets through closes it, the call runs"
"$NG" --kind once --host b.test -- "$t/bin/flaky" "$d/n2" 1 7 "$conn" > /dev/null 2>&1
"$NG" --kind once --host b.test -- /bin/echo fine > /dev/null 2>&1
ok '[ ! -f "$NETGUARD_DIR/breaker/b.test" ]' "breaker: a success resets the count"

# the summary: every incident since the last one, one notice; then nothing new
"$NG" summary > "$d/sum" 2>&1; rc=$?
ok '[ "$rc" = 0 ] && grep -q "5 incident(s)" "$d/sum" && [ "$(grep -c "night summary" "$NG_TEST_NOTICES")" = 1 ]' "summary: the night's incidents printed, one notice"
"$NG" summary > "$d/sum2" 2>&1
ok 'grep -q "no incidents" "$d/sum2" && [ "$(grep -c "night summary" "$NG_TEST_NOTICES")" = 1 ]' "summary: the next one starts after it, no notice when there's nothing"

# open the breaker while the first curl still has its partial download open
fresh deps
mkdir -p "$d/repo/scripts" "$d/repo/tests/harness"
cp "$root/scripts/deps.sh" "$d/repo/scripts/"
cp "$NG" "$d/repo/tests/harness/"
printf '[asset]\nurl = https://deps.test/asset\nsha256 = %s\n' "$(echo 'fake asset' | shasum -a 256 | cut -d' ' -f1)" > "$d/repo/deps.lock"
deps="$d/repo/scripts/deps.sh"
PATH="$t/path:$PATH" NG_TEST_CURL_DIR="$d" RBSP_DEPS_LOCK="$d/repo/deps.lock" "$deps" fetch asset > "$d/out1" 2> "$d/err1" & fetchp=$!
for ((i=0; i<50; i++)); do [ -s "$d/partial" ] && break; sleep 0.1; done
printf '3\t%s\t0\tconnection refused\n' "$(date +%s)" > "$NETGUARD_DIR/breaker/deps.test"
PATH="$t/path:$PATH" NG_TEST_CURL_DIR="$d" RBSP_DEPS_LOCK="$d/repo/deps.lock" "$deps" fetch asset > "$d/out2" 2> "$d/err2"; refused=$?
touch "$d/finish"; wait "$fetchp"; rc=$?
asset="$d/repo/build/dl/asset/asset"
ok '[ "$refused" = 1 ] && [ "$rc" = 0 ] && grep -q "3 consecutive failures to deps.test" "$d/err2" && grep -q "asset: download failed" "$d/err2" && [ "$(cat "$d/out1")" = "$asset" ] && [ "$(cat "$asset")" = "fake asset" ] && [ "$(ls "$d/repo/build/dl/asset")" = asset ]' "deps: a refused fetch leaves the active fetch's partial download alone"

# the outbox: a notice the notifier can't deliver waits and goes out with the next one
fresh outbox
NETGUARD_NOTIFY="$t/bin/notify-fail" "$NG" --kind once --host q1.test -- "$t/bin/flaky" "$d/n" 5 7 "$conn" > /dev/null 2>&1
ok '[ "$(grep -c . "$NETGUARD_DIR/outbox")" = 1 ] && [ "$(notices)" = 0 ]' "outbox: an undelivered notice is kept"
"$NG" --kind once --host q2.test -- "$t/bin/flaky" "$d/n2" 5 7 "$conn" > /dev/null 2>&1
ok '[ "$(notices)" = 2 ] && head -1 "$NG_TEST_NOTICES" | grep -q "q1.test.*(queued " && [ ! -s "$NETGUARD_DIR/outbox" ]' "outbox: sent first, with the next notice"
NETGUARD_NOTIFY="$t/bin/notify-fail" "$NG" --kind once --host q3.test -- "$t/bin/flaky" "$d/n3" 5 7 "$conn" > /dev/null 2>&1
"$NG" summary > "$d/sum" 2>&1
ok 'grep -q "undelivered notice(s) folded in" "$NG_TEST_NOTICES" && [ ! -s "$NETGUARD_DIR/outbox" ] && grep -q "q3.test" "$d/sum"' "summary: folds the outbox in"

fresh outbox-dedup
"$NG" --kind once --host repeat.test -- "$t/bin/flaky" "$d/n1" 5 7 "$conn" > /dev/null 2>&1
NETGUARD_NOTIFY="$t/bin/notify-fail" "$NG" --kind once --host queued.test -- "$t/bin/flaky" "$d/n2" 5 7 "$conn" > /dev/null 2>&1
"$NG" --kind once --host repeat.test -- "$t/bin/flaky" "$d/n1" 5 7 "$conn" > /dev/null 2> "$d/err"
ok '[ "$(notices)" = 2 ] && tail -1 "$NG_TEST_NOTICES" | grep -q "queued.test.*(queued " && grep -q "notice not repeated" "$d/err" && [ ! -s "$NETGUARD_DIR/outbox" ]' "outbox: a deduplicated notice still flushes the queue"

# a dead sender can leave a batch or the unsent remainder, depending on when it was killed
fresh outbox-recover; mkdir -p "$NETGUARD_DIR"
(exit 0) & sender=$!; wait "$sender"
(exit 0) & requeuer=$!; wait "$requeuer"
printf '%s\tnetguard\tabandoned batch\n' "$(date +%s)" > "$NETGUARD_DIR/outbox.sending.$sender"
cp "$NETGUARD_DIR/outbox.sending.$sender" "$NETGUARD_DIR/outbox.unsent.$sender"
printf '%s\tnetguard\tabandoned remainder\n' "$(date +%s)" > "$NETGUARD_DIR/outbox.unsent.$requeuer"
printf '%s\tnetguard\tactive batch\n' "$(date +%s)" > "$NETGUARD_DIR/outbox.sending.$$"
"$NG" --kind once --host next.test -- "$t/bin/flaky" "$d/n" 5 7 "$conn" > /dev/null 2>&1
ok '[ "$(notices)" = 3 ] && grep -q "abandoned batch (queued " "$NG_TEST_NOTICES" && grep -q "abandoned remainder (queued " "$NG_TEST_NOTICES" && tail -1 "$NG_TEST_NOTICES" | grep -q "next.test" && [ -f "$NETGUARD_DIR/outbox.sending.$$" ] && [ ! -e "$NETGUARD_DIR/outbox.sending.$sender" ] && [ ! -e "$NETGUARD_DIR/outbox.unsent.$sender" ] && [ ! -e "$NETGUARD_DIR/outbox.unsent.$requeuer" ]' "outbox: abandoned batches and unsent notices are recovered"

# process-group cleanup: a command and its child both ignoring TERM get KILLed and reaped
fresh group
"$NG" --kind once --host g.test --stall 1 -- /bin/bash -c 'trap "" TERM; echo $$ > "'"$d"'/main"; (trap "" TERM; exec sleep 60) & echo $! > "'"$d"'/kid"; exec sleep 60' > /dev/null 2> "$d/err"; rc=$?
ok '[ "$rc" = 124 ] && ! kill -0 "$(cat "$d/main")" 2> /dev/null && ! kill -0 "$(cat "$d/kid")" 2> /dev/null && ! grep -q "processes left" "$d/err"' "group: TERM ignored → KILL, the command and its child gone"
if ps -A -o pid= > /dev/null 2>&1; then
  "$NG" --kind once --host g.test --stall 1 -- /bin/bash -c 'perl -e '"'"'setpgrp(0, 0); $SIG{TERM} = "IGNORE"; sleep 60'"'"' & echo $! > "'"$d"'/esc"; exec sleep 60' > /dev/null 2>&1
  ok '! kill -0 "$(cat "$d/esc")" 2> /dev/null' "group: a child that left the process group (setpgrp) is stopped too"
else
  echo "netguard-test: the escaped-child case skipped (ps isn't allowed here: the sandbox)"
fi
# netguard killed itself (SIGKILL, e.g. by a caller's timeout): its launcher stops the command
"$NG" --kind once --host g.test -- /bin/bash -c 'echo $$ > "'"$d"'/orphan"; exec sleep 60' > /dev/null 2>&1 & ngp=$!
for i in 1 2 3 4 5 6 7 8 9 10; do [ -s "$d/orphan" ] && break; sleep 0.2; done
kill -KILL "$ngp"; wait "$ngp" 2> /dev/null
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do kill -0 "$(cat "$d/orphan")" 2> /dev/null || break; sleep 0.2; done
ok '[ -s "$d/orphan" ] && ! kill -0 "$(cat "$d/orphan")" 2> /dev/null' "netguard SIGKILLed: the command doesn't outlive it"

# tool-level timeouts added (fake curl and ssh print their arguments); the caller's own kept
fresh tools
PATH="$t/path:$PATH" "$NG" --kind download -- curl -fsSL -o x https://h.test/f > "$d/c1" 2> /dev/null
ok '[ "$(cat "$d/c1")" = "args: --connect-timeout 20 --speed-limit 1 --speed-time 120 -fsSL -o x https://h.test/f" ]' "curl: connect timeout and speed limit added ($(cat "$d/c1"))"
PATH="$t/path:$PATH" "$NG" --kind download -- curl --connect-timeout 5 https://h.test/f > "$d/c2" 2> /dev/null
ok '[ "$(cat "$d/c2")" = "args: --speed-limit 1 --speed-time 120 --connect-timeout 5 https://h.test/f" ]' "curl: the caller's --connect-timeout kept"
PATH="$t/path:$PATH" "$NG" --kind ssh --host p.test -- ssh -o ConnectTimeout=5 u@p.test exit > "$d/s1" 2> /dev/null
ok '[ "$(cat "$d/s1")" = "args: -o BatchMode=yes -o ServerAliveInterval=15 -o ServerAliveCountMax=4 -o ControlMaster=no -o ConnectTimeout=5 u@p.test exit" ]' "ssh: BatchMode, keepalives and no new master added, the caller's ConnectTimeout kept"
"$NG" --kind git -- /bin/bash -c 'echo "$GIT_HTTP_LOW_SPEED_LIMIT $GIT_HTTP_LOW_SPEED_TIME $GIT_TERMINAL_PROMPT"' > "$d/g1" 2> /dev/null
ok '[ "$(cat "$d/g1")" = "  " ]' "the git settings only when the command is git"

# ssh retries only ssh's own errors before a session (exit 255), never a stall or the remote's output
fresh ssh; mkdir -p "$d/bin"
cat > "$d/bin/ssh" << 'EOF'
#!/bin/bash
echo start >> "$NG_SSH_STARTS"
case "$NG_SSH_MODE" in
  silent) exec sleep 30 ;;
  remote) echo "curl: (7) Failed to connect to x port 80: Connection refused" >&2; exit 1 ;;
  pre) echo "ssh: connect to host x port 22: Operation timed out" >&2; exit 255 ;;
esac
EOF
chmod +x "$d/bin/ssh"
NG_SSH_STARTS="$d/s1" NG_SSH_MODE=silent PATH="$d/bin:$PATH" "$NG" --kind ssh --host s1.test --stall 1 -- ssh x make install > /dev/null 2>&1; rc=$?
ok '[ "$rc" = 124 ] && [ "$(grep -c start "$d/s1")" = 1 ]' "ssh: a silent session is started once, never retried (exit 124)"
NG_SSH_STARTS="$d/s2" NG_SSH_MODE=remote PATH="$d/bin:$PATH" "$NG" --kind ssh --host s2.test -- ssh x curl x > /dev/null 2>&1; rc=$?
ok '[ "$rc" = 1 ] && [ "$(grep -c start "$d/s2")" = 1 ]' "ssh: the remote command's 'Connection refused' (exit 1) isn't retried"
NG_SSH_STARTS="$d/s3" NG_SSH_MODE=pre PATH="$d/bin:$PATH" "$NG" --kind ssh --host s3.test -- ssh x exit > /dev/null 2>&1; rc=$?
ok '[ "$rc" = 255 ] && [ "$(grep -c start "$d/s3")" = 2 ]' "ssh: 'ssh: connect to host ... Operation timed out' (exit 255) retried once"

# a command that ends during the last tick before the stall limit isn't a stall
fresh lasttick
NETGUARD_TICK=2 "$NG" --kind download --host l.test --stall 4 -- /bin/bash -c 'echo run >> "'"$d"'/runs"; sleep 3; touch "'"$d"'/m"' > /dev/null 2>&1; rc=$?
ok '[ "$rc" = 0 ] && [ -f "$d/m" ] && [ "$(grep -c run "$d/runs")" = 1 ] && [ "$(incidents)" = 0 ]' "ended in the last tick: exit 0, one run, no incident"

# the state lock: a slow notifier doesn't hold it; a lock held by a live pid is never taken or removed
fresh lock
printf '#!/bin/bash\nsleep 10\n' > "$d/hang"; chmod +x "$d/hang"
s="$(start)"
NETGUARD_NOTIFY="$d/hang" NETGUARD_NOTIFY_TIMEOUT=2 "$NG" --kind once --host k1.test -- "$t/bin/flaky" "$d/n1" 5 7 "$conn" > /dev/null 2>&1 &
sleep 1
NETGUARD_NOTIFY="$d/hang" NETGUARD_NOTIFY_TIMEOUT=2 "$NG" --kind once --host k2.test -- "$t/bin/flaky" "$d/n2" 5 7 "$conn" > /dev/null 2>&1
wait
ok '[ "$(grep -c . "$NETGUARD_DIR/outbox")" = 2 ] && grep -q k1.test "$NETGUARD_DIR/outbox" && grep -q k2.test "$NETGUARD_DIR/outbox" && [ ! -e "$NETGUARD_DIR/.lock" ]' "lock: a hanging notifier, two failing steps 1 s apart: both notices in the outbox"
ok 'perl -e "exit !(\$ARGV[0] < 6)" "$(secs "$s")"' "lock: the second step didn't wait for the first one's notifier ($(secs "$s") s)"
rm -f "$NETGUARD_DIR/outbox"
sleep 60 & holder=$!
mkdir "$NETGUARD_DIR/.lock"; echo "$holder" > "$NETGUARD_DIR/.lock/pid"
NETGUARD_LOCK_WAIT=1 "$NG" --kind once --host k3.test -- "$t/bin/flaky" "$d/n3" 5 7 "$conn" > /dev/null 2> "$d/err3"
ok '[ "$(cat "$NETGUARD_DIR/.lock/pid")" = "$holder" ] && [ "$(cat "$NETGUARD_DIR"/outbox.[0-9]* | grep -c k3.test)" = 1 ] && [ "$(notices)" = 0 ]' "lock: held by a live pid, never taken or removed; the notice waits in outbox.PID"
kill "$holder"; wait "$holder" 2> /dev/null
"$NG" --kind once --host k4.test -- "$t/bin/flaky" "$d/n4" 5 7 "$conn" > /dev/null 2>&1
ok '[ "$(notices)" = 2 ] && head -1 "$NG_TEST_NOTICES" | grep -q "k3.test.*(queued " && [ ! -e "$NETGUARD_DIR/.lock" ] && ! ls "$NETGUARD_DIR"/outbox.[0-9]* > /dev/null 2>&1' "lock: a dead owner's lock taken over; outbox.PID sent first"

# the state lives in the main checkout's build/netguard when run from a worktree (lanes' worktrees go away)
fresh common
if ! git init -q "$d/main"; then
  ok false "state: git init failed for the temporary checkout"
elif [ "$(git -C "$d/main" rev-parse --show-toplevel)" != "$d/main" ]; then
  ok false "state: the temporary checkout is not its own Git repository"
elif mkdir -p "$d/main/tests/harness" && cp "$NG" "$d/main/tests/harness/" &&
    git -C "$d/main" add -A && git -C "$d/main" -c user.name=t -c user.email=t@t commit -qm t &&
    git -C "$d/main" worktree add -q "$d/wt"; then
  ( unset NETGUARD_DIR; "$d/wt/tests/harness/netguard.sh" --kind once --host c.test -- "$t/bin/flaky" "$d/n" 5 7 "$conn" > /dev/null 2>&1 )
  ok 'grep -q "c.test" "$d/main/build/netguard/incidents.log" 2> /dev/null && [ ! -e "$d/wt/build/netguard" ]' "state: an incident from a worktree lands in the main checkout's build/netguard"
else
  ok false "state: could not set up the temporary worktree"
fi

# preflight: netcheck's rows read; one notice naming program and host; the state file for lanes
fresh preflight
cat > "$t/bin/netcheck" << 'EOF'
#!/bin/bash
echo "args: $*"
printf 'OK\tcurl\tgithub.com\tHTTP 200\n' >> "$NETCHECK_TSV"
printf 'SKIP\tssh\tthe Windows PC (ssh, Tailscale)\t(not set)\n' >> "$NETCHECK_TSV"
[ -n "${NG_TEST_ALL_OK:-}" ] && exit 0
printf 'TIMEOUT\tcurl\tapi.nuget.org\t\n' >> "$NETCHECK_TSV"
exit 1
EOF
chmod +x "$t/bin/netcheck"
NETGUARD_NETCHECK="$t/bin/netcheck" "$NG" preflight > "$d/out" 2> "$d/err"; rc=$?
ok '[ "$rc" = 1 ] && grep -q "^args: --no-ai$" "$d/out" && grep -q "^result=bad" "$NETGUARD_DIR/preflight.state" && grep -q "^bad	curl	api.nuget.org	TIMEOUT$" "$NETGUARD_DIR/preflight.state" && ! grep -q "Windows PC" "$NETGUARD_DIR/preflight.state"' "preflight: a TIMEOUT row → exit 1, state bad with that row (a SKIP row isn't bad)"
ok '[ "$(notices)" = 1 ] && grep -q "curl api.nuget.org (TIMEOUT)" "$NG_TEST_NOTICES" && ! grep -q github.com "$NG_TEST_NOTICES" && [ "$(incidents)" = 0 ]' "preflight: one notice naming program and host only"
NG_TEST_ALL_OK=1 NETGUARD_NETCHECK="$t/bin/netcheck" "$NG" preflight > /dev/null 2>&1; rc=$?
ok '[ "$rc" = 0 ] && grep -q "^result=ok" "$NETGUARD_DIR/preflight.state" && [ "$(notices)" = 1 ]' "preflight: all OK (a SKIP row too) → exit 0, state ok, no notice"
printf '#!/bin/bash\nexec sleep 30\n' > "$t/bin/netcheck-hang"; chmod +x "$t/bin/netcheck-hang"
NETGUARD_PREFLIGHT_STALL=1 NETGUARD_NETCHECK="$t/bin/netcheck-hang" "$NG" preflight > /dev/null 2>&1; rc=$?
ok '[ "$rc" = 124 ] && grep -q "stalled" "$NETGUARD_DIR/preflight.state" && [ "$(notices)" = 2 ] && [ "$(incidents)" = 0 ]' "preflight: a hanging check is bounded too, one notice for it"

# usage
"$NG" --stall 601 -- true 2> /dev/null; r1=$?
"$NG" --kind bogus -- true 2> /dev/null; r2=$?
"$NG" --kind once 2> /dev/null; r3=$?
ok '[ "$r1" = 64 ] && [ "$r2" = 64 ] && [ "$r3" = 64 ]' "usage: --stall over 600, an unknown kind, no command → 64"

echo "netguard-test: $pass passed, $fail failed"
[ "$fail" = 0 ]

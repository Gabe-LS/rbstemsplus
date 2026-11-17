#!/bin/bash
# tests/win/vm-run.sh's offline tests (T16): every command against a fake prlctl (no VM, no
# network but 127.0.0.1). The fake keeps the VM's name, state and snapshots in files, logs every
# call, and plays a step's script by its "#fake:" lines (out TEXT, rc N, sleep N, prljob-once,
# http, mode).
#   tests/win/vm-run-test.sh          exit 0 when every check passed
set -uo pipefail
here="$(cd "$(dirname "$0")/../.." && pwd)"
VR="$here/tests/win/vm-run.sh"
T="$(mktemp -d "${TMPDIR:-/tmp}/vm-run-test.XXXXXX")" || exit 2
trap 'rm -rf "$T"' EXIT
pass=0; fail=0
ok() { if eval "$1"; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "FAIL $2"; fi; }

F="$T/fake"; mkdir -p "$F" "$T/bin" "$T/share/probe" "$T/share/dist-test" "$T/gates" "$T/suites/s"
echo rb-win-template > "$F/name"
echo running > "$F/state"
printf '%s\n' '{11111111-aaaa-bbbb-cccc-000000000001} clean Windows' \
              '{22222222-aaaa-bbbb-cccc-000000000002} rekordbox 7.2.19 logged in' > "$F/snaps"
echo 0 > "$F/bootfails"
echo hidden > "$T/share/probe/hidden.exe"
cat > "$T/bin/prlctl" <<'EOF'
#!/bin/bash
F="$FAKE_DIR"; SHARE="$FAKE_SHARE"
echo "$*" >> "$F/calls"
cmd="$1"; shift
case "$cmd" in
  list) printf 'ID: {%s}\nName: %s\nState: %s\n' "$2" "$(cat "$F/name")" "$(cat "$F/state")" ;;
  snapshot-list)
    if [ "${2:-}" = -i ]; then grep -F "$3" "$F/snaps" | sed 's/^\([^ ]*\) \(.*\)$/ID: \1\nName: \2/'
    else echo 'PARENT_SNAPSHOT_ID                      SNAPSHOT_ID'; awk '{print "                                        " $1}' "$F/snaps"; fi ;;
  snapshot-switch) [ -e "$F/switch-fails" ] && exit 1; echo stopped > "$F/state" ;;
  set) [ "$2 $3" = "--pause-idle off" ] || exit 1 ;;
  start|resume) echo running > "$F/state" ;;
  stop) echo stopped > "$F/state" ;;
  exec)
    [ "$(cat "$F/state")" = running ] || exit 255
    shift
    if [ "$1" = cmd ]; then
      n=$(cat "$F/bootfails"); if [ "$n" -gt 0 ]; then echo $((n - 1)) > "$F/bootfails"; exit 255; fi
      echo 'Microsoft Windows [Version 10.0.26200.1000]'; exit 0
    fi
    file=""; while [ $# -gt 0 ]; do [ "$1" = -File ] && file="$2"; shift; done
    map() { local p="${1//\\//}"; echo "$SHARE/${p#//Mac/win/}"; }
    f="$(map "$file")"
    [ -f "$f" ] || { echo "fake: no $f"; exit 1; }
    if grep -q 'vm-run-kill' "$f"; then   # the end of a timed-out step's processes
      if [ -e "$F/kill-fails" ]; then echo 'still running: 1234 powershell.exe'; echo 'vm-run-kill: 1 left'; exit 1; fi
      echo 'vm-run-kill: 0 left'; exit 0
    fi
    if grep -q rbstems-run "$f"; then   # a user step's launcher: play its job
      mode="$(sed -n "s/^\$mode = '\([^']*\)'.*/\1/p" "$f")"
      job="$(sed -n "s/.*\$job = '\([^']*\)'.*/\1/p" "$f")"
      echo "fake: launched as $mode"
      f="$(map "$job")"
    fi
    marker=""
    if grep -q 'vm-run-wrap' "$f"; then   # the step's wrapper: play the step, then its exit line
      marker="$(sed -n "s/.*\$marker = '\([^']*\)'.*/\1/p" "$f")"
      f="$(map "$(sed -n "s/^\$step = '\([^']*\)'.*/\1/p" "$f")")"
      [ -f "$f" ] || { echo "fake: no step $f"; exit 1; }
    fi
    # prlctl's own failure to start the job: its line alone, nothing run
    if grep -q '^#fake: prljob-once' "$f" && [ ! -e "$F/prljob-done" ]; then
      touch "$F/prljob-done"; echo 'PrlJob_GetResult/GetRetCode: Invalid argument'; exit 255
    fi
    if grep -q 'prepared:' "$f"; then [ -e "$F/prepare-fails" ] && exit 1; echo "prepared: hidden.exe True arch $(cat "$F/arch")"; fi
    url="$(sed -n "s/.*Url = '\([^']*\)'.*/\1/p" "$f")"
    echo "fake: step $(sed -n "s/.*Step = '\([^']*\)'.*/\1/p" "$f")"
    rc=0
    while read -r tag what arg; do
      [ "$tag" = '#fake:' ] || continue
      case "$what" in
        out) echo "$arg" ;;
        rc) rc="$arg" ;;
        sleep) sleep "$arg" ;;
        prljob-after-work) echo 'half the work done'; echo 'PrlJob_GetResult/GetRetCode: Invalid argument'; exit 255 ;;
        http) curl -fsS --max-time 5 "${url}payload.json" || rc=7 ;;
        kill-http) kill "$(cat "$(ls -td "$SHARE"/results/*/ | head -1)http.pid")" ;;
        touch-payload) echo ' ' >> "$SHARE/dist-test/payload.json" ;;
        rc-if) [ -e "$F/${arg%% *}" ] && rc="${arg#* }" ;;
        write) echo x >> "$arg" ;;
        swap) cp "$arg" "$arg.orig"; printf '%s\n' '#fake: out SWAPPED' > "$arg" ;;
        unswap) mv "$arg.orig" "$arg" ;;
        sleep-if) [ -e "$F/${arg%% *}" ] && sleep "${arg#* }" ;;
        run-stage) "$FAKE_VR" stage > "$F/stage-during.out" 2>&1; echo "stage during the run: $?" ;;
        bg-sleep) sleep "$arg" & echo "fake: left a sleep $!" ;;
        get) curl -fsS --max-time 5 "${url}$arg" || echo "GET-REFUSED $arg" ;;
        sha) echo "sha256 $(curl -fsS --max-time 5 "${url}$arg" | shasum -a 256 | cut -c1-64)" ;;
      esac
    done < "$f"
    if [ -n "$marker" ]; then   # the wrapper's map: none or outside 0..255 is 1
      { [ "$rc" -ge 0 ] && [ "$rc" -le 255 ]; } 2> /dev/null || rc=1
      [ -e "$F/no-marker" ] || echo "$marker $rc"
    fi
    [ -e "$F/rc-zero" ] && exit 0   # a prlctl that never passes the guest's status on
    exit $((rc & 255)) ;;
  *) echo "fake prlctl: $cmd?"; exit 1 ;;
esac
EOF
chmod +x "$T/bin/prlctl"

# a checkout to name in records: what stage archives, and the suites --record runs (committed)
R="$T/repo"; RS="$R/tests/win/suites"
mkdir -p "$R/scripts" "$R/tests/win/bootstrap" "$R/core" "$R/win/c" "$RS/s" "$R/tests/win/first-run"
# the runner is committed in the checkout, as in a real one, and run from it: a record names the
# vm-run.sh that made it (VT2); first-run.sh and its suites too, and the real suites (first-run's
# steps run them by name) but for release.list, which is the fake's (written below)
cp "$here/tests/win/vm-run.sh" "$here/tests/win/first-run.sh" "$R/tests/win/"
cp -R "$here/tests/win/first-run/." "$R/tests/win/first-run/"
(cd "$here/tests/win/suites" && tar -cf - --exclude release.list .) | (cd "$RS" && tar -xf -)
echo bootstrap > "$R/scripts/bootstrap.ps1"; echo t > "$R/tests/win/bootstrap/bootstrap.tests.ps1"
echo texts > "$R/core/texts.def"; echo aumid > "$R/win/c/aumid.h"; echo other > "$R/core/other.c"
printf 'build/\ndist-test/\n' > "$R/.gitignore"
printf '%s\n' '#fake: out hello' > "$RS/s/pass.ps1"
printf '%s\n' '#fake: rc-if beta-fails 3' '#fake: sleep-if beta-slow 30' > "$RS/s/maybe.ps1"
printf '%s\n' 'need http' 'need candidate dist-test/new/payload.json' 'need fixture dist-test/a/payload.json' 'sys 60 one s/pass.ps1' > "$RS/upd.suite"
printf '%s\n' 'need http' 'need vm' 'sys 60 one s/pass.ps1' > "$RS/vmneed.suite"
printf '%s\n' '#fake: run-stage' > "$RS/s/runstage.ps1"
printf '%s\n' 'need http' 'sys 60 stage s/runstage.ps1' > "$RS/inter.suite"
printf '%s\n' "#fake: write $T/share/repo/scripts/bootstrap.ps1" > "$RS/s/tamper.ps1"
printf '%s\n' 'need http' 'sys 60 tamper s/tamper.ps1' > "$RS/tamper.suite"
printf '%s\n' '#fake: out skipped here' '#fake: rc 77' > "$RS/s/skip.ps1"
printf '%s\n' '#fake: out from the include' > "$RS/inc.ps1"
printf '%s\n' 'need http' 'include inc.ps1' 'sys 60 one s/pass.ps1' 'user 60 two s/pass.ps1' > "$RS/alpha.suite"
printf '%s\n' 'need http' 'sys 60 one s/pass.ps1' 'sys 5 maybe s/maybe.ps1' > "$RS/beta.suite"
printf '%s\n' 'need http' 'sys 60 one s/pass.ps1' > "$RS/gamma.suite"
printf '%s\n' 'need http' 'sys 60 one s/pass.ps1' 'sys 60 maybe s/skip.ps1' > "$RS/skipper.suite"
printf '%s\n' "#fake: write $R/stray.txt" > "$RS/s/stray.ps1"
printf '%s\n' 'need http' 'sys 60 one s/pass.ps1' 'sys 60 stray s/stray.ps1' > "$RS/delta.suite"
# a step script swapped in the checkout during the run and back (A->B->A, VX4)
printf '%s\n' "#fake: swap $RS/s/victim.ps1" > "$RS/s/swapin.ps1"; printf '%s\n' '#fake: out original' > "$RS/s/victim.ps1"
printf '%s\n' "#fake: unswap $RS/s/victim.ps1" > "$RS/s/swapback.ps1"
printf '%s\n' 'need http' 'sys 60 swap s/swapin.ps1' 'sys 60 victim s/victim.ps1' 'sys 60 back s/swapback.ps1' > "$RS/swap.suite"
printf '%s\n' '# the rows' 'alpha  arm64  clean Windows' 'beta   arm64  {22222222-aaaa-bbbb-cccc-000000000002}' 'upd    arm64  clean Windows' > "$RS/release.list"
# the builders stage --build runs, faked: each writes what the real one builds (FAKE_BUILD_FAIL=NAME fails NAME)
mkdir -p "$R/common/tests" "$R/win/c/tests"
printf '%s\n' '#!/bin/sh' '[ "${FAKE_BUILD_FAIL:-}" = common ] && { echo "common broke"; exit 1; }' \
  'for a in x64 arm64; do mkdir -p "$1/$a"; echo "pwt $a" > "$1/$a/plat-win-test.exe"; echo "dll $a" > "$1/$a/checked.dll"; done' > "$R/common/tests/win.sh"
printf '%s\n' '#!/bin/sh' '[ "${FAKE_BUILD_FAIL:-}" = winc ] && exit 1' \
  'for a in x64 arm64; do mkdir -p "$1/win-$a"; echo "w $a" > "$1/win-$a/winc-wintest.exe"; echo "app $a" > "$1/win-$a/winc-app-wintest.exe"; done; echo fixtures > "$1/fixtures.txt"' > "$R/win/c/tests/run.sh"
printf '%s\n' '#!/bin/sh' '[ "$1 $3" = "dll --test-hooks" ] || exit 2' 'for a in x64 arm64; do mkdir -p "$2/testhooks/$a"; echo "core $a" > "$2/testhooks/$a/rbsp-core.dll"; done' > "$R/core/build.sh"
chmod +x "$R/common/tests/win.sh" "$R/win/c/tests/run.sh" "$R/core/build.sh"
git -C "$R" init -q && git -C "$R" add -A && git -C "$R" -c user.email=t@t -c user.name=t commit -q -m one
C="$(git -C "$R" rev-parse HEAD)"
VR="$R/tests/win/vm-run.sh"   # the checkout's own (VT2)
mkdir -p "$R/dist-test"
printf '{"payload_version": "1.5.0", "commit": "%s"}\n' "$C" > "$T/share/dist-test/payload.json"
cp "$T/share/dist-test/payload.json" "$R/dist-test/payload.json"
# update.suite's shape: new/ this commit's release (a candidate), a/ an older one (a fixture)
mkrels() {
  mkdir -p "$T/share/dist-test/new" "$T/share/dist-test/a"
  printf '{"payload_version": "1.5.1", "commit": "%s"}\n' "$C" > "$T/share/dist-test/new/payload.json"
  printf '{"payload_version": "1.4.0", "commit": "%s"}\n' 1111111111111111111111111111111111111111 > "$T/share/dist-test/a/payload.json"
}
mkrels
echo ARM64 > "$F/arch"

export FAKE_DIR="$F" FAKE_SHARE="$T/share" FAKE_VR="$VR" VMRUN_PRLCTL="$T/bin/prlctl" VMRUN_SHARE="$T/share" VMRUN_GATES="$T/gates" \
  VMRUN_ROOT="$R" VMRUN_HTTP_BIND=127.0.0.1 VMRUN_HTTP_PORT=$((20000 + $$ % 20000)) VMRUN_POLL=0 VMRUN_BOOT=20
vr() { "$VR" "$@" > "$T/out" 2>&1; }
calls() { cat "$F/calls" 2> /dev/null; }
reset() { : > "$F/calls"; echo running > "$F/state"; echo rb-win-template > "$F/name"; rm -f "$F/switch-fails" "$F/prljob-done" "$F/no-marker" "$F/rc-zero"; }
last_results() { ls -td "$T/share/results"/*/ 2> /dev/null | head -1; }

printf '%s\n' '#fake: out hello' > "$T/suites/s/pass.ps1"
printf '%s\n' '#fake: out failing' '#fake: rc 3' > "$T/suites/s/fail.ps1"
printf '%s\n' '#fake: sleep 30' > "$T/suites/s/slow.ps1"
printf '%s\n' '#fake: prljob-once' '#fake: out after a retry' > "$T/suites/s/flaky.ps1"
printf '%s\n' '#fake: http' > "$T/suites/s/http.ps1"
printf '%s\n' 'need http' 'sys 60 one s/pass.ps1' 'user 60 two s/pass.ps1' 'user-high 60 three s/pass.ps1' \
              'user-signed 60 four s/pass.ps1' 'sys 60 flaky s/flaky.ps1' 'sys 60 http s/http.ps1' > "$T/suites/good.suite"
printf '%s\n' 'sys 60 one s/pass.ps1' 'sys 60 bad s/fail.ps1' 'sys 2 slow s/slow.ps1' 'sys 60 last s/pass.ps1' > "$T/suites/mixed.suite"

# ---- the VM guard
reset; echo 'Windows 11' > "$F/name"
vr run 'clean Windows' "$T/suites/good.suite"; rc=$?
ok '[ $rc = 2 ] && grep -q "owner" "$T/out" && ! calls | grep -qE "^(snapshot-switch|exec|start|set)"' "the owner's VM refused, never driven"
reset; echo 'other' > "$F/name"
vr run 'clean Windows' "$T/suites/good.suite"; rc=$?
ok '[ $rc = 2 ] && ! calls | grep -qE "^(snapshot-switch|exec)"' 'a VM of another name refused'
reset
vr run 'no such snapshot' "$T/suites/good.suite"; rc=$?
ok '[ $rc = 2 ] && grep -q "no snapshot named" "$T/out" && ! calls | grep -q "^snapshot-switch"' 'an unknown snapshot refused before anything changes'

# ---- suites refused before the VM is touched
reset
for bad in 'sys 601 x s/pass.ps1' 'sys 0 x s/pass.ps1' 'root 60 x s/pass.ps1' 'sys 60 a/b s/pass.ps1' 'sys 60 x s/none.ps1' 'sys 60 x s/pass.ps1 extra' 'need tea'; do
  printf '%s\n' "$bad" > "$T/suites/bad.suite"
  vr run 'clean Windows' "$T/suites/bad.suite"; rc=$?
  ok '[ $rc = 2 ] && ! calls | grep -q "^snapshot-switch"' "a suite line refused: $bad"
done
printf '%s\n' 'sys 60 x s/pass.ps1' 'sys 60 x s/pass.ps1' > "$T/suites/bad.suite"
vr run 'clean Windows' "$T/suites/bad.suite"; ok '[ $? = 2 ]' 'two steps of one name refused'
mv "$T/share/dist-test" "$T/share/dist-test.off"
vr run 'clean Windows' "$T/suites/good.suite"; rc=$?
ok '[ $rc = 2 ] && grep -q "needs the served folder" "$T/out"' '"need http" without the served folder: refused'
mv "$T/share/dist-test.off" "$T/share/dist-test"
vr run 'clean Windows' "$T/suites/good.suite" --serve "$T"; rc=$?
ok '[ $rc = 2 ] && grep -q "inside the share" "$T/out"' 'a served folder outside the share refused'
mv "$T/share/probe/hidden.exe" "$T/hidden.off"
vr run 'clean Windows' "$T/suites/good.suite"; rc=$?
ok '[ $rc = 2 ] && grep -q hidden.exe "$T/out"' 'user steps without hidden.exe on the share: refused'
mv "$T/hidden.off" "$T/share/probe/hidden.exe"

# ---- a passing run
reset; echo 2 > "$F/bootfails"
vr run 'clean Windows' "$T/suites/good.suite"; rc=$?
res="$(last_results)"
ok '[ $rc = 0 ]' "a passing run exits 0 ($(tail -3 "$T/out" | tr '\n' ' '))"
ok 'calls | grep -n "" | grep -E "snapshot-switch .* -i \{11111111" | head -1 | grep -q "^[0-9]*:snapshot-switch"' 'the revert by name: the snapshot id switched to'
first_switch=$(calls | grep -n '^snapshot-switch' | head -1 | cut -d: -f1)
pause=$(calls | grep -n -- '--pause-idle off' | head -1 | cut -d: -f1)
start=$(calls | grep -n '^start' | head -1 | cut -d: -f1)
firstexec=$(calls | grep -n '^exec' | head -1 | cut -d: -f1)
ok '[ -n "$pause" ] && [ "$first_switch" -lt "$pause" ] && [ "$pause" -lt "$start" ] && [ "$start" -lt "$firstexec" ]' 'switch, then Pause idle off, then start, then exec'
ok '[ "$(calls | grep -c "cmd /c ver")" -ge 3 ]' 'the boot probe retried until the VM answered'
ok '! calls | grep -q -- "--current-user"' 'never --current-user'
ok 'grep -q "prepared: hidden.exe True" "$res/0-prepare.log"' 'hidden.exe copied in after the revert'
ok '[ "$(grep -c " PASS " "$res/results.txt")" = 6 ]' "every step recorded as PASS ($(tr '\n' ';' < "$res/results.txt"))"
ok 'grep -q "^hello" "$res/1-one.log" && grep -q "fake: step one" "$res/1-one.log"' "a SYSTEM step's output collected, with its prelude"
ok 'grep -q "launched as user$" "$res/2-two.log" && grep -q "launched as user-high" "$res/3-three.log" && grep -q "launched as user-signed" "$res/4-four.log"' 'the user modes go through the launcher'
ok 'grep -q "after a retry" "$res/5-flaky.log" && grep -q "^vm-run: attempt 1, nothing run: PrlJob" "$res/5-flaky.log"' "a job prlctl failed to start is tried again, the failed attempt's output kept"
ok 'grep -q payload_version "$res/6-http.log"' 'the served payload.json reached from a step'
last_stop=$(calls | grep -n '^stop' | tail -1 | cut -d: -f1)
last_switch=$(calls | grep -n '^snapshot-switch' | tail -1 | cut -d: -f1)
ok '[ -n "$last_stop" ] && [ "$last_stop" -lt "$last_switch" ] && [ "$(calls | grep -c "^snapshot-switch")" = 2 ]' 'after the steps: stopped, then reverted again'
ok '[ ! -e "$T/share/.vm-run.lock" ]' 'the lock released'
ok '! curl -fsS --max-time 2 -o /dev/null "http://127.0.0.1:$VMRUN_HTTP_PORT/" 2> /dev/null' 'the HTTP server stopped'
ok '[ ! -e "$T/gates/$C" ]' 'no record without --record'

# ---- the HTTP server: only ours, alive, serving the share's payload.json (VR2)
mkdir -p "$T/foreign"; echo '{"payload_version": "0.0.1-OLD"}' > "$T/foreign/payload.json"
python3 -I -m http.server --bind 127.0.0.1 --directory "$T/foreign" "$VMRUN_HTTP_PORT" > /dev/null 2>&1 & foreign=$!
for _ in 1 2 3 4 5 6 7 8 9 10; do curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$VMRUN_HTTP_PORT/" && break; sleep 0.5; done
reset
vr run 'clean Windows' "$T/suites/good.suite"; rc=$?
ok '[ $rc = 2 ] && grep -q "already answers" "$T/out" && ! calls | grep -q "^snapshot-switch"' 'another server on the port: refused before the VM is touched'
kill "$foreign" 2> /dev/null; wait "$foreign" 2> /dev/null
printf '%s\n' '#fake: kill-http' > "$T/suites/s/killhttp.ps1"
printf '%s\n' 'need http' 'sys 60 one s/pass.ps1' 'sys 60 down s/killhttp.ps1' 'sys 60 never s/pass.ps1' > "$T/suites/httpdies.suite"
reset
vr run 'clean Windows' "$T/suites/httpdies.suite"; rc=$?
res="$(last_results)"
ok '[ $rc = 1 ] && grep -q "HTTP server ended during step 2" "$res/results.txt" && ! grep -q " never " "$res/results.txt" && [ "$(calls | grep -c "^snapshot-switch")" = 2 ]' "the server gone after a step: the run stops, FAIL, the VM reverted ($(tr '\n' ';' < "$res/results.txt"))"
cp "$T/share/dist-test/payload.json" "$T/payload.saved"
printf '%s\n' '#fake: touch-payload' > "$T/suites/s/touch.ps1"
printf '%s\n' 'need http' 'sys 60 one s/pass.ps1' 'sys 60 touch s/touch.ps1' > "$T/suites/payloadchanges.suite"
reset
vr run 'clean Windows' "$T/suites/payloadchanges.suite"; rc=$?
res="$(last_results)"
ok '[ $rc = 1 ] && grep -q "payload.json changed during the run" "$res/results.txt"' "the served payload.json changed during the run: FAIL ($(tr '\n' ';' < "$res/results.txt"))"
cp "$T/payload.saved" "$T/share/dist-test/payload.json"
echo SECRET-OUTSIDE > "$T/secret.txt"; ln -s "$T/secret.txt" "$T/share/dist-test/escape.txt"
mkdir -p "$T/share/dist-test/sub"; echo inside > "$T/share/dist-test/sub/in.txt"; ln -s "$T" "$T/share/dist-test/up"
printf '%s\n' '#fake: get escape.txt' '#fake: get up/secret.txt' '#fake: get sub/in.txt' > "$T/suites/s/links.ps1"
printf '%s\n' 'need http' 'sys 60 links s/links.ps1' > "$T/suites/links.suite"
reset
vr run 'clean Windows' "$T/suites/links.suite"; rc=$?
res="$(last_results)"
ok '! grep -q SECRET-OUTSIDE "$res/1-links.log" && grep -q "GET-REFUSED escape.txt" "$res/1-links.log" && grep -q "GET-REFUSED up/secret.txt" "$res/1-links.log" && grep -q "^inside" "$res/1-links.log"' "the server sends no file through a link, a plain one still (VR16)"
# a folder's index page that is a link: the folder isn't served either (VS2)
echo '<p>SECRET-INDEX</p>' > "$T/secret.html"; ln -s "$T/secret.html" "$T/share/dist-test/index.html"
mkdir -p "$T/share/dist-test/sub2"; ln -s "$T/secret.html" "$T/share/dist-test/sub/index.htm"; echo plain > "$T/share/dist-test/sub2/plain.txt"
printf '%s\n' '#fake: get ./' '#fake: get sub/' '#fake: get sub2/' '#fake: get payload.json' > "$T/suites/s/index.ps1"
printf '%s\n' 'need http' 'sys 60 index s/index.ps1' > "$T/suites/index.suite"
reset
vr run 'clean Windows' "$T/suites/index.suite"; rc=$?
res="$(last_results)"
ok '! grep -q SECRET-INDEX "$res/1-index.log" && grep -q "GET-REFUSED ./" "$res/1-index.log" && grep -q "GET-REFUSED sub/" "$res/1-index.log" && grep -q "plain.txt" "$res/1-index.log" && grep -q payload_version "$res/1-index.log"' "an index page that is a link: its folder 404s (the root's, a subfolder's); a plain listing and payload.json still served (VS2)"
rm -f "$T/share/dist-test/index.html" "$T/share/dist-test/sub/index.htm"; rm -rf "$T/share/dist-test/sub2"
rm -rf "$T/share/dist-test/escape.txt" "$T/share/dist-test/up" "$T/share/dist-test/sub"

printf '%s\n' '#fake: prljob-after-work' > "$T/suites/s/halfdone.ps1"
printf '%s\n' 'sys 60 half s/halfdone.ps1' > "$T/suites/half.suite"
reset
vr run 'clean Windows' "$T/suites/half.suite"; rc=$?
res="$(last_results)"
ok '[ $rc = 1 ] && [ "$(calls | grep -c "half.wrap.ps1")" = 1 ] && grep -q "half the work done" "$res/1-half.log"' "a PrlJob error after the job ran is never run again: FAIL (VR28)"

# ---- a failing run: every step still runs; a timeout is bounded
reset
t0=$(date +%s)
vr run 'clean Windows' "$T/suites/mixed.suite"; rc=$?
took=$(( $(date +%s) - t0 ))
res="$(last_results)"
ok '[ $rc = 1 ]' 'a failing run exits 1'
ok 'grep -q "^step sys bad FAIL 3 " "$res/results.txt" && grep -q "^step sys slow TIMEOUT 124 " "$res/results.txt" && grep -q "^step sys last PASS " "$res/results.txt"' "FAIL, TIMEOUT and the steps after them ($(tr '\n' ';' < "$res/results.txt"))"
ok '[ "$took" -lt 25 ]' "the 2 s step stopped at its timeout (the run took ${took}s)"
ok 'calls | grep -q "kill-3.ps1" && grep -qx "vm-run-kill: 0 left" "$res/3-kill.log"' "a timed-out step's processes are ended in the VM before the next step (VR29)"
ok 'grep -qF "\$job = '\''\\job\\$(basename "$res")\\3-'\''" "$T/share/job/$(basename "$res")/kill-3.ps1"' "the kill matches only step 3's own scripts (\\job\\<run>\\3-), never the kill script's parents (VS14)"
reset; touch "$F/kill-fails"
vr run 'clean Windows' "$T/suites/mixed.suite"; rc=$?
res="$(last_results)"
ok '[ $rc = 2 ] && grep -q "couldn.t be ended" "$res/results.txt" && ! grep -q " last " "$res/results.txt" && [ "$(calls | grep -c "^snapshot-switch")" = 2 ]' "processes that can't be ended: the run stops, the VM reverted"
rm -f "$F/kill-fails"

# ---- the verdict is the wrapper's exit line, never prlctl's status alone (VR3)
ok 'grep -q "vm-run-exit" "$(ls "$T/share/job"/*mixed*/1-one.wrap.ps1 | tail -1)" && grep -q -- "-lt 0 -or \$c -gt 255) { \$c = 1 }" "$(ls "$T/share/job"/*mixed*/1-one.wrap.ps1 | tail -1)"' "the wrapper maps a code outside 0..255 to 1 and prints its exit line"
printf '%s\n' '#fake: out unreadable' '#fake: rc -196608' > "$T/suites/s/badfile.ps1"
printf '%s\n' '#fake: rc 256' > "$T/suites/s/r256.ps1"
printf '%s\n' 'sys 60 neg s/badfile.ps1' 'sys 60 big s/r256.ps1' 'user 60 uneg s/badfile.ps1' > "$T/suites/codes.suite"
reset
vr run 'clean Windows' "$T/suites/codes.suite"; rc=$?
res="$(last_results)"
ok '[ $rc = 1 ] && grep -q "^step sys neg FAIL 1 " "$res/results.txt" && grep -q "^step sys big FAIL 1 " "$res/results.txt" && grep -q "^step user uneg FAIL 1 " "$res/results.txt"' "-196608 and 256 are FAILs, not the 0 they truncate to ($(tr '\n' ';' < "$res/results.txt"))"
printf '%s\n' 'sys 60 one s/pass.ps1' 'user 60 two s/pass.ps1' > "$T/suites/two.suite"
reset; touch "$F/no-marker"
vr run 'clean Windows' "$T/suites/two.suite"; rc=$?
res="$(last_results)"
ok '[ $rc = 1 ] && grep -q "^step sys one FAIL 1 " "$res/results.txt" && grep -q "^step user two FAIL 1 " "$res/results.txt" && grep -q "no \"vm-run-exit" "$res/1-one.log"' "no exit line (the wrapper didn't run to its end) with prlctl's 0: FAIL ($(tr '\n' ';' < "$res/results.txt"))"
reset; touch "$F/rc-zero"
vr run 'clean Windows' "$T/suites/mixed.suite"; rc=$?
res="$(last_results)"
ok '[ $rc = 1 ] && grep -q "^step sys one PASS 0 " "$res/results.txt" && grep -q "^step sys bad FAIL 1 " "$res/results.txt" && grep -q "exited 3, prlctl said 0" "$res/2-bad.log"' "a prlctl that says 0 for a step that exited 3: FAIL ($(tr '\n' ';' < "$res/results.txt"))"
reset; touch "$F/no-marker"
printf '%s\n' '#fake: out once' | "$VR" sys > "$T/out" 2>&1; rc=$?
ok '[ $rc = 1 ] && grep -q "no \"vm-run-exit" "$T/out"' 'a one-off script with no exit line: 1'

# ---- --keep
reset
vr run 'clean Windows' "$T/suites/mixed.suite" --keep
ok '[ "$(calls | grep -c "^snapshot-switch")" = 1 ] && ! calls | grep -q "^stop"' '--keep: no stop, no second revert'

# ---- the record and its check (VR1, VR9)
G="$T/gates/$C/vm-win"; K1=11111111-aaaa-bbbb-cccc-000000000001; K2=22222222-aaaa-bbbb-cccc-000000000002
DT="$T/share/dist-test"
reset
vr run 'clean Windows' "$RS/alpha.suite" --record; rc=$?
ok '[ $rc = 2 ] && grep -q "staged from HEAD" "$T/out" && ! calls | grep -q "^snapshot-switch"' "--record before stage (no repo/.commit): refused"
vr stage > /dev/null
vr run 'clean Windows' "$T/suites/good.suite" --record; rc=$?
ok '[ $rc = 2 ] && grep -q "in the checkout" "$T/out"' "--record of a suite outside the checkout: refused"
# a smoke-like suite's pass isn't a release's
reset
vr run 'clean Windows' "$RS/gamma.suite" --record; rc=$?
ok '[ $rc = 0 ] && [ -f "$G/gamma@$K1-arm64.ok" ]' "a passing --record run: vm-win/<suite>@<snapshot id>-<arch>.ok ($(ls "$G" 2> /dev/null | tr '\n' ' '))"
vr check "$C" "$DT"; rc=$?
ok '[ $rc = 1 ] && grep -q "no record of alpha" "$T/out" && [ ! -e "$T/gates/$C/vm-win.ok" ]' "check: a record of a suite release.list doesn't name satisfies nothing"
# one frozen snapshot (VX3, VX4): HEAD's suites and runner archived into the run's src/ before the
# suite is read, and only those bytes parsed, hashed and run: a step script swapped in the checkout
# during the run and back never runs
reset
vr run 'clean Windows' "$RS/swap.suite" --record; rc=$?
res="$(last_results)"; vsha="$(git -C "$R" show "$C:tests/win/suites/s/victim.ps1" | shasum -a 256 | cut -c1-64)"
ok '[ $rc = 0 ] && [ -f "$G/swap@$K1-arm64.ok" ] && grep -q "^original" "$res/2-victim.log" && ! grep -q SWAPPED "$res/2-victim.log" && cmp -s "$res/src/tests/win/vm-run.sh" "$VR" && grep -qx "file tests/win/suites/s/victim.ps1 $vsha" "$G/swap@$K1-arm64.ok"' "--record runs HEAD's frozen suite (src/): a step script swapped in the checkout during the run and back isn't what runs (VX3, VX4: $(grep -h "SWAPPED\|original" "$res/2-victim.log" 2> /dev/null | tr '\n' ' '))"
# a record is rb-win-template's (VX8): another prlctl or uuid without a gates folder of its own is
# refused before the VM is touched, and nothing lands in the checkout's build/gates
reset
env -u VMRUN_GATES "$VR" run 'clean Windows' "$RS/gamma.suite" --record > "$T/out" 2>&1; rc=$?
ok '[ $rc = 2 ] && grep -q "VMRUN_PRLCTL or VMRUN_UUID is set" "$T/out" && ! calls | grep -q "^snapshot-switch" && [ ! -e "$R/build/gates" ]' "--record refused: VMRUN_PRLCTL without VMRUN_GATES (VX8: rc $rc)"
rm -rf "$R/build/gates"; reset
env -u VMRUN_GATES -u VMRUN_PRLCTL PATH="$T/bin:$PATH" VMRUN_UUID=33333333-aaaa-bbbb-cccc-000000000003 "$VR" run 'clean Windows' "$RS/gamma.suite" --record > "$T/out" 2>&1; rc=$?
ok '[ $rc = 2 ] && grep -q "VMRUN_PRLCTL or VMRUN_UUID is set" "$T/out" && ! calls | grep -q "^snapshot-switch" && [ ! -e "$R/build/gates" ]' "--record refused: VMRUN_UUID without VMRUN_GATES (VX8: rc $rc)"
rm -rf "$R/build/gates"
# a missing share under --record: "no share", as without it, and nothing made (BT4)
reset
VMRUN_SHARE="$T/noshare" "$VR" run 'clean Windows' "$RS/gamma.suite" --record > "$T/out" 2>&1; rc=$?
ok '[ $rc = 2 ] && grep -q "no share $T/noshare" "$T/out" && [ ! -e "$T/noshare" ] && ! calls | grep -q "^snapshot-switch"' "--record with a missing share: \"no share\", nothing created (BT4: rc $rc, $(tr '\n' ' ' < "$T/out"))"
reset
vr run 'clean Windows' "$RS/alpha.suite" --record; rc=$?
rec="$G/alpha@$K1-arm64.ok"
ok '[ $rc = 0 ] && [ -f "$rec" ]' "alpha's record"
ok 'grep -qx "commit $C" "$rec" && grep -qx "suite alpha" "$rec" && grep -qx "snapshot {$K1} clean Windows" "$rec" && grep -qx "arch arm64" "$rec" && grep -qx "payload_sha256 $(shasum -a 256 "$DT/payload.json" | cut -c1-64)" "$rec" && grep -qx "stage_commit $C" "$rec" && grep -qx "result pass" "$rec"' "the record: commit, suite, snapshot, arch, payload, staged commit, result"
ok 'grep -qx "file tests/win/vm-run.sh $(shasum -a 256 "$VR" | cut -c1-64)" "$rec"' "the record names the vm-run.sh that made it, in the checkout (VT2)"
ok 'grep -qx "vm_uuid 202a80c8-4bc4-4e95-890e-a31d6bfd5b70" "$rec"' "the record names the VM's uuid (VX8)"
"$here/tests/win/vm-run.sh" run 'clean Windows' "$RS/gamma.suite" --record > "$T/out" 2>&1; rc=$?
ok '[ $rc = 2 ] && grep -q "this vm-run.sh ($(cd "$here" && pwd -P)/tests/win/vm-run.sh) isn.t the checkout.s" "$T/out"' "--record by a vm-run.sh outside the checkout: refused (VT2, VX7)"
ok 'grep -qx "file tests/win/suites/alpha.suite $(shasum -a 256 "$RS/alpha.suite" | cut -c1-64)" "$rec" && grep -q "^file tests/win/suites/inc.ps1 " "$rec" && grep -q "^file tests/win/suites/s/pass.ps1 " "$rec" && grep -q "^assembled 1-one.ps1 [0-9a-f]\{64\}$" "$rec" && [ "$(grep -c "^step .* PASS " "$rec")" = 2 ]' "the record: the suite's and its scripts' shas, the assembled steps', each step"
touch "$F/beta-fails"; reset
vr run 'rekordbox 7.2.19 logged in' "$RS/beta.suite" --record; rc=$?
ok '[ $rc = 1 ] && [ -f "$G/beta@$K2-arm64.failed" ] && [ -f "$rec" ]' "a failing --record run: its own .failed, the other suites' records kept"
reset
vr run 'clean Windows' "$RS/alpha.suite" --record; rc=$?
vr check "$C" "$DT"; rc=$?
ok '[ $rc = 1 ] && grep -q "beta on .* failed" "$T/out" && [ -f "$G/beta@$K2-arm64.failed" ] && [ ! -e "$T/gates/$C/vm-win.ok" ]' "check: a later pass of another suite doesn't erase a failure"
rm -f "$F/beta-fails"; reset
vr run 'rekordbox 7.2.19 logged in' "$RS/beta.suite" --record; rc=$?
ok '[ $rc = 0 ] && [ -f "$G/beta@$K2-arm64.ok" ] && [ ! -e "$G/beta@$K2-arm64.failed" ]' "a rerun of the same suite, snapshot and arch replaces its own pair"
ok 'grep -qx "snapshot {$K2} rekordbox 7.2.19 logged in" "$G/beta@$K2-arm64.ok"' "a snapshot given by {id}: the record names it by id and by its name (VS12)"
# a candidate is this commit's release, a fixture is recorded as it is (VS3)
printf '{"payload_version": "1.5.1", "commit": "%s"}\n' 0000000000000000000000000000000000000000 > "$DT/new/payload.json"
reset
vr run 'clean Windows' "$RS/upd.suite" --record; rc=$?
ok '[ $rc = 2 ] && grep -q "dist-test/new/payload.json (a candidate) was built from" "$T/out" && ! calls | grep -q "^snapshot-switch"' "--record refused: a candidate release that isn't HEAD's"
mkrels; reset
vr run 'clean Windows' "$RS/upd.suite" --record; rc=$?
U="$G/upd@$K1-arm64.ok"
ok '[ $rc = 0 ] && grep -qE "^served dist-test/new/payload.json [0-9a-f]{64} $C$" "$U" && grep -qE "^served dist-test/a/payload.json [0-9a-f]{64} 1111111111111111111111111111111111111111$" "$U"' "the record binds the candidate to HEAD and names the fixture with its own commit"
vr check "$C" "$DT"; rc=$?
ok '[ $rc = 0 ] && grep -qx "commit $C" "$T/gates/$C/vm-win.ok" && grep -q "^row alpha arm64 clean Windows: alpha@$K1-arm64.ok$" "$T/gates/$C/vm-win.ok" && grep -q "^row beta arm64 {$K2}: beta@$K2-arm64.ok$" "$T/gates/$C/vm-win.ok"' "check: every row of release.list passed: vm-win.ok lists them (by name and by {id}) ($(cat "$T/out"))"
vr check 0000000000000000000000000000000000000000 "$DT"; ok '[ $? = 1 ]' 'check: another commit fails'
mkdir -p "$T/other"; echo '{"payload_version": "1.5.1"}' > "$T/other/payload.json"
vr check "$C" "$T/other"; ok '[ $? = 1 ] && grep -q "another payload" "$T/out" && [ ! -e "$T/gates/$C/vm-win.ok" ]' 'check: another payload.json fails, and vm-win.ok goes'
cp "$rec" "$T/rec.saved"; sed -i '' "s|^file tests/win/suites/s/pass.ps1 .*|file tests/win/suites/s/pass.ps1 $(printf '0%.0s' $(seq 64))|" "$rec"
vr check "$C" "$DT"; ok '[ $? = 1 ] && grep -q "pass.ps1 it ran isn" "$T/out"' "check: a record whose script isn't COMMIT's fails"
cp "$T/rec.saved" "$rec"
vr check "$C" "$DT"; ok '[ $? = 0 ]' 'check: passes again'
# forged records, one line each (VS13, VS6): check must fail on every one
forge() {   # NAME SED-SCRIPT [FILE]: the edited copy of FILE (alpha's record) checked, then put back
  local f="${3:-$rec}"; cp "$f" "$T/forge.saved"; sed -i '' "$2" "$f"
  vr check "$C" "$DT"; local rc=$?
  ok '[ $rc = 1 ] && [ ! -e "$T/gates/$C/vm-win.ok" ]' "check: a forged record fails: $1 ($(grep -m1 'check: ' "$T/out"))"
  cp "$T/forge.saved" "$f"
}
forge "another suite line" 's/^suite alpha$/suite gamma/'
forge "one step fewer" '/^step user two /d'
forge "a step that failed" 's/^step sys one PASS 0 /step sys one FAIL 1 /'
forge "a step that passed with another code" 's/^step sys one PASS 0 /step sys one PASS 7 /'
forge "result fail" 's/^result pass$/result fail/'
forge "no suite file line" '/^file tests\/win\/suites\/alpha.suite /d'
forge "another snapshot's id and name" "s/^snapshot .*/snapshot {$K2} rekordbox 7.2.19 logged in/"
forge "another arch" 's/^arch arm64$/arch x64/'
forge "another VM's uuid (VX8)" 's/^vm_uuid .*/vm_uuid 33333333-aaaa-bbbb-cccc-000000000003/'
forge "no vm_uuid line (VX8)" '/^vm_uuid /d'
forge "the steps in another order" 's/^step sys one /step sys TMP /; s/^step user two /step sys one /; s/^step sys TMP /step user two /'
forge "another stage commit" "s/^stage_commit .*/stage_commit 0000000000000000000000000000000000000000/"
forge "no include file line" '/^file tests\/win\/suites\/inc.ps1 /d'
forge "no vm-run.sh file line" '/^file tests\/win\/vm-run.sh /d'
forge "a file line twice" '/^file tests\/win\/suites\/inc.ps1 /p'
forge "a file line the suite doesn't run" '/^file tests\/win\/suites\/inc.ps1 /{p;s/inc.ps1/s\/skip.ps1/;}'
forge "an outside line" '/^run /{p;s/^run .*/outside \/tmp\/x.ps1/;}'
forge "two result lines" '/^result pass$/p'
forge "a result incomplete line too" '/^result pass$/{p;s/pass/incomplete/;}'
forge "no served candidate line" '/^served dist-test\/new\/payload.json /d' "$U"
forge "a candidate of another commit" "s|^served dist-test/new/payload.json \([0-9a-f]*\) .*|served dist-test/new/payload.json \1 0000000000000000000000000000000000000000|" "$U"
cp "$rec" "$G/alpha@99999999-aaaa-bbbb-cccc-000000000009-arm64.ok"
vr check "$C" "$DT"; ok '[ $? = 1 ] && grep -q "two records of alpha" "$T/out"' "check: two records for one row fail"
rm -f "$G/alpha@99999999-aaaa-bbbb-cccc-000000000009-arm64.ok"
mkdir -p "$T/gates/$C/vm-win.ok.tmp"
vr check "$C" "$DT"; ok '[ $? = 1 ] && grep -q "can.t write .*vm-win.ok" "$T/out"' "check: vm-win.ok that can't be written: exit 1 (VS9)"
rmdir "$T/gates/$C/vm-win.ok.tmp"
ls -R "$T/gates" > "$T/gates.before"
vr check ../x "$DT"; ok '[ $? = 1 ] && grep -q "isn.t a full commit id" "$T/out" && ls -R "$T/gates" | cmp -s - "$T/gates.before"' "check ../x: refused, nothing deleted (VS17)"
vr check "$C" "$DT"; ok '[ $? = 0 ]' 'check: passes again after the forged ones'
# a record left by a run that didn't end its own way is a failure (VS1)
touch "$F/beta-slow" "$F/kill-fails"; reset
vr run 'rekordbox 7.2.19 logged in' "$RS/beta.suite" --record; rc=$?
ok '[ $rc = 2 ] && [ -f "$G/beta@$K2-arm64.failed" ] && [ ! -e "$G/beta@$K2-arm64.ok" ] && grep -qx "result fail" "$G/beta@$K2-arm64.failed" && grep -q "couldn.t be ended" "$G/beta@$K2-arm64.failed" && [ "$(calls | grep -c "^snapshot-switch")" = 2 ]' "--record: a step whose processes can't be ended: the VM reverted, .failed written"
vr check "$C" "$DT"; ok '[ $? = 1 ] && grep -q "beta on .* failed" "$T/out"' "check: fails on it"
rm -f "$F/kill-fails"; reset
t0=$(date +%s)
perl -e 'setpgrp(0, 0); exec @ARGV' "$VR" run 'rekordbox 7.2.19 logged in' "$RS/beta.suite" --record > "$T/out" 2>&1 & vp=$!
until grep -q "step 2: maybe" "$T/out" 2> /dev/null || [ $(( $(date +%s) - t0 )) -ge 20 ]; do sleep 0.5; done
sleep 1; kill -TERM -"$vp" 2> /dev/null; wait "$vp"; rc=$?
ok '[ $rc = 130 ] && [ -f "$G/beta@$K2-arm64.failed" ] && grep -qx "result incomplete" "$G/beta@$K2-arm64.failed" && [ ! -e "$G/beta@$K2-arm64.ok" ]' "--record interrupted mid-step: its incomplete record is a .failed"
vr check "$C" "$DT"; ok '[ $? = 1 ] && grep -q "beta on .* failed" "$T/out"' "check: fails on an incomplete record"
rm -f "$F/beta-slow"; reset
vr run 'rekordbox 7.2.19 logged in' "$RS/beta.suite" --record > /dev/null 2>&1
vr check "$C" "$DT"; ok '[ $? = 0 ]' 'check: passes again after a good rerun'
# check under the share's lock (VT1): while another run holds it, check stops and writes nothing
mkdir "$T/share/.vm-run.lock"; sleep 120 & lp=$!; echo "$lp" > "$T/share/.vm-run.lock/pid"
rm -f "$T/gates/$C/vm-win.ok"
vr check "$C" "$DT"; rc=$?
ok '[ $rc = 2 ] && grep -q "another vm-run.sh (pid $lp) drives the VM" "$T/out" && [ ! -e "$T/gates/$C/vm-win.ok" ] && [ "$(cat "$T/share/.vm-run.lock/pid")" = "$lp" ]' "check while a run holds the share's lock: exit 2, said, no vm-win.ok, the lock left alone (VT1)"
kill "$lp" 2> /dev/null; wait "$lp" 2> /dev/null; rm -rf "$T/share/.vm-run.lock"
vr check "$C" "$DT"; ok '[ $? = 0 ] && [ -f "$T/gates/$C/vm-win.ok" ] && [ ! -e "$T/share/.vm-run.lock" ]' 'check: passes once the lock is free, and releases it'
# a holder kill -0 can't signal (EPERM: pid 1, another user's, or an unsandboxed run seen from
# the sandbox) is alive, never stale, however old its lock (VX6)
mkdir "$T/share/.vm-run.lock"; echo 1 > "$T/share/.vm-run.lock/pid"; touch -t 202001010000 "$T/share/.vm-run.lock"
vr check "$C" "$DT"; rc=$?
ok '[ $rc = 2 ] && grep -q "another vm-run.sh (pid 1) drives the VM" "$T/out" && [ "$(cat "$T/share/.vm-run.lock/pid" 2> /dev/null)" = 1 ]' "check: an old lock whose pid gives EPERM (pid 1): exit 2, the lock left alone (VX6)"
rm -rf "$T/share/.vm-run.lock"
# the share under a --record run (VS4, VS7): a stage meanwhile is refused; repo/ changed under it fails
reset
vr run 'clean Windows' "$RS/inter.suite" --record; rc=$?
ok '[ $rc = 0 ] && grep -q "another vm-run.sh" "$F/stage-during.out" && [ -f "$G/inter@$K1-arm64.ok" ]' "a stage while a --record run holds the lock is refused, the run's record stands"
reset
vr run 'clean Windows' "$RS/tamper.suite" --record; rc=$?
ok '[ $rc = 1 ] && grep -q "the share.s repo/ changed" "$G/tamper@$K1-arm64.failed"' "the share's repo/ changed under a --record run (a writer ignoring the lock): .failed"
vr stage > /dev/null
echo extra > "$T/share/repo/scripts/extra.ps1"
vr run 'clean Windows' "$RS/alpha.suite" --record; rc=$?
ok '[ $rc = 2 ] && grep -q "repo/ isn.t what" "$T/out"' "--record refused: repo/ isn't what stage wrote (repo/shas.txt)"
rm -f "$T/share/repo/scripts/extra.ps1"
# repo/ is HEAD's tree file for file (VT3): a changed or an extra file with shas.txt made to match
reshas() { (cd "$T/share" && find repo -type f ! -name 'shas.txt*' ! -path repo/.commit | LC_ALL=C sort | while IFS= read -r f; do echo "$(shasum -a 256 "$f" | cut -c1-64) $f"; done) > "$T/share/repo/shas.txt"; }
vr stage > /dev/null; echo changed >> "$T/share/repo/scripts/bootstrap.ps1"; reshas
vr run 'clean Windows' "$RS/gamma.suite" --record; rc=$?
ok '[ $rc = 2 ] && grep -q "the share.s repo/ isn.t $C.s tree (.*scripts/bootstrap.ps1)" "$T/out"' "--record refused: a repo/ file that isn't HEAD's, shas.txt notwithstanding (VT3)"
vr stage > /dev/null; echo extra > "$T/share/repo/tests/win/extra.ps1"; reshas
vr run 'clean Windows' "$RS/gamma.suite" --record; rc=$?
ok '[ $rc = 2 ] && grep -q "isn.t $C.s tree (.*tests/win/extra.ps1)" "$T/out"' "--record refused: a repo/ file HEAD hasn't (VT3)"
vr stage > /dev/null; rm "$T/share/repo/core/texts.def"; reshas
vr run 'clean Windows' "$RS/gamma.suite" --record; rc=$?
ok '[ $rc = 2 ] && grep -q "isn.t $C.s tree (.*core/texts.def)" "$T/out"' "--record refused: a file of HEAD's missing from repo/ (VT3)"
# the share inside a git checkout, as the default one is (VX1): repo/'s own files are hashed, never
# the enclosing checkout's (here it has HEAD's files at its top, where a relative path would land)
git init -q "$T"; git -C "$R" archive "$C" scripts tests/win core/texts.def win/c/aumid.h | tar -xf - -C "$T"
vr stage > /dev/null; echo changed >> "$T/share/repo/scripts/bootstrap.ps1"; reshas
vr run 'clean Windows' "$RS/gamma.suite" --record; rc=$?
ok '[ $rc = 2 ] && grep -q "the share.s repo/ isn.t $C.s tree (.*scripts/bootstrap.ps1)" "$T/out"' "--record refused: the share inside a checkout, a repo/ file that isn't HEAD's (VX1)"
vr stage > /dev/null; echo extra > "$T/share/repo/tests/win/extra.ps1"; reshas
vr run 'clean Windows' "$RS/gamma.suite" --record; rc=$?
ok '[ $rc = 2 ] && grep -q "isn.t $C.s tree (.*tests/win/extra.ps1)" "$T/out"' "--record refused: the share inside a checkout, a file only the share has (VX1)"
rm -rf "$T/.git" "$T/scripts" "$T/tests" "$T/core" "$T/win"; vr stage > /dev/null
# --record runs only as the checkout's own tests/win/vm-run.sh, never a copy that finds it (VX7)
mkdir -p "$R/build/old"; cp "$VR" "$R/build/old/vm-run.sh"; reset
"$R/build/old/vm-run.sh" run 'clean Windows' "$RS/gamma.suite" --record > "$T/out" 2>&1; rc=$?
ok '[ $rc = 2 ] && grep -q "build/old/vm-run.sh) isn.t the checkout.s" "$T/out" && ! calls | grep -q "^snapshot-switch"' "--record refused: a copy of vm-run.sh, not the checkout's (VX7)"
rm -rf "$R/build/old"
# stage reads HEAD once (VT3): HEAD moving right after git archive leaves repo/.commit naming the
# archived commit, and --record at the new HEAD refuses it
real_git="$(command -v git)"; mkdir -p "$T/gitwrap"
cat > "$T/gitwrap/git" <<EOF
#!/bin/sh
"$real_git" "\$@"; rc=\$?
case " \$* " in *" archive "*)
  if [ -f "$T/gitwrap/on" ]; then rm -f "$T/gitwrap/on"; echo b > "$R/b.txt"; "$real_git" -C "$R" add -A && "$real_git" -C "$R" -c user.email=t@t -c user.name=t commit -q -m B; fi ;;
esac
exit \$rc
EOF
chmod +x "$T/gitwrap/git"; touch "$T/gitwrap/on"
PATH="$T/gitwrap:$PATH" "$VR" stage > "$T/out" 2>&1; rc=$?
NB="$(git -C "$R" rev-parse HEAD)"
ok '[ $rc = 0 ] && [ "$NB" != "$C" ] && [ "$(cat "$T/share/repo/.commit")" = "$C" ] && grep -q "staged repo/ from ${C:0:9}" "$T/out" && [ ! -e "$T/share/repo/b.txt" ]' "stage: HEAD moved right after git archive: repo/.commit names the commit archived (VT3)"
printf '{"payload_version": "1.5.0", "commit": "%s"}\n' "$NB" > "$DT/payload.json"; cp "$DT/payload.json" "$R/dist-test/payload.json"
vr run 'clean Windows' "$RS/gamma.suite" --record; rc=$?
ok '[ $rc = 2 ] && grep -q "repo/ wasn.t staged from HEAD $NB" "$T/out"' "--record at the new HEAD refuses that repo/ (VT3)"
git -C "$R" reset -q --hard "$C"
printf '{"payload_version": "1.5.0", "commit": "%s"}\n' "$C" > "$DT/payload.json"; cp "$DT/payload.json" "$R/dist-test/payload.json"
vr stage > /dev/null
# the TEST build's fixture copies, byte for byte (VS3)
printf B > "$DT/bootstrap.ps1"; printf B > "$R/dist-test/bootstrap.ps1"
mkdir -p "$DT/fault-sig"; cp "$DT/bootstrap.ps1" "$DT/payload.json" "$DT/fault-sig/"; printf X > "$DT/fault-sig/payload.json.sig"
vr run 'clean Windows' "$RS/alpha.suite" --record; rc=$?
ok '[ $rc = 2 ] && grep -q "fault-sig/payload.json.sig isn.t one of dist-test" "$T/out"' "--record refused: a fault copy that isn't the TEST build's"
rm -rf "$DT/fault-sig"; mkdir -p "$DT/fault-json"; cp "$DT/bootstrap.ps1" "$DT/fault-json/"; { cat "$DT/payload.json"; printf Y; } > "$DT/fault-json/payload.json"
vr run 'clean Windows' "$RS/alpha.suite" --record; rc=$?
ok '[ $rc = 2 ] && grep -q "fault-json/payload.json isn.t dist-test.s plus one byte" "$T/out"' "--record refused: a faulted file that isn't the TEST build's plus X"
{ cat "$DT/payload.json"; printf X; } > "$DT/fault-json/payload.json"; mkdir -p "$DT/bad-version"; printf C > "$DT/bad-version/bootstrap.ps1"
vr run 'clean Windows' "$RS/alpha.suite" --record; rc=$?
ok '[ $rc = 2 ] && grep -q "bad-version/ is there but dist-test/payload.json names no windows.app.zip" "$T/out"' "--record refused: a bad-version/ beside a TEST build with no windows zip"
rm -rf "$DT/bad-version" "$DT/fault-json"
# bad-version/ is the TEST build but for windows.app.version, its zip and bootstrap.ps1 included (VT8)
cp "$DT/payload.json" "$T/plain.saved"
printf '{"payload_version": "1.5.0", "commit": "%s", "windows": {"app": {"zip": "w.zip", "version": "1.5.0"}, "x": 1}}\n' "$C" > "$DT/payload.json"
cp "$DT/payload.json" "$R/dist-test/payload.json"; printf Z > "$DT/w.zip"; cp "$DT/w.zip" "$R/dist-test/w.zip"
bv() {   # PYTHON-EDITS: bad-version/ made from the TEST build with these edits to its payload (p)
  rm -rf "$DT/bad-version"; mkdir -p "$DT/bad-version"; cp "$DT/bootstrap.ps1" "$DT/w.zip" "$DT/bad-version/"; printf S > "$DT/bad-version/payload.json.sig"
  python3 -I -c 'import json, sys; p = json.load(open(sys.argv[1])); p["windows"]["app"]["version"] = "1.5.1"; exec(sys.argv[3]); json.dump(p, open(sys.argv[2], "w"), indent=1)' "$DT/payload.json" "$DT/bad-version/payload.json" "${1:-pass}"
}
bv; vr run 'clean Windows' "$RS/gamma.suite" --record; rc=$?
ok '[ $rc = 0 ]' "--record with a bad-version/ that is the TEST build but for its version (written again): runs ($(grep -m1 'vm-run.sh:' "$T/out"))"
bv 'p["windows"]["app"]["zip"] = "stale.zip"'; vr run 'clean Windows' "$RS/gamma.suite" --record; rc=$?
ok '[ $rc = 2 ] && grep -q "bad-version/payload.json names the zip \"stale.zip\"" "$T/out"' "--record refused: bad-version/ naming another zip (VT8)"
bv; rm "$DT/bad-version/bootstrap.ps1"; vr run 'clean Windows' "$RS/gamma.suite" --record; rc=$?
ok '[ $rc = 2 ] && grep -q "bad-version/ has no bootstrap.ps1" "$T/out"' "--record refused: bad-version/ without its bootstrap.ps1 (VT8)"
bv; rm "$DT/bad-version/w.zip"; vr run 'clean Windows' "$RS/gamma.suite" --record; rc=$?
ok '[ $rc = 2 ] && grep -q "bad-version/ has no w.zip" "$T/out"' "--record refused: bad-version/ without its zip (VT8)"
bv; printf Q > "$DT/bad-version/w.zip"; vr run 'clean Windows' "$RS/gamma.suite" --record; rc=$?
ok '[ $rc = 2 ] && grep -q "bad-version/w.zip isn.t dist-test.s" "$T/out"' "--record refused: bad-version/'s zip that isn't the TEST build's (VT8)"
bv 'p["windows"]["x"] = 2'; vr run 'clean Windows' "$RS/gamma.suite" --record; rc=$?
ok '[ $rc = 2 ] && grep -q "differs from dist-test.s in more than windows.app.version" "$T/out"' "--record refused: bad-version/ with a second field changed (VT8)"
bv 'p["windows"]["app"]["version"] = "1.5.0"'; vr run 'clean Windows' "$RS/gamma.suite" --record; rc=$?
ok '[ $rc = 2 ] && grep -q "names dist-test.s own windows.app.version" "$T/out"' "--record refused: bad-version/ with the TEST build's own version (VT8)"
bv; printf E > "$DT/bad-version/extra.txt"; vr run 'clean Windows' "$RS/gamma.suite" --record; rc=$?
ok '[ $rc = 2 ] && grep -q "bad-version/extra.txt isn.t one of a release.s files" "$T/out"' "--record refused: a file in bad-version/ no release has (VT8)"
rm -rf "$DT/bad-version" "$DT/w.zip" "$R/dist-test/w.zip"; cp "$T/plain.saved" "$DT/payload.json"; cp "$T/plain.saved" "$R/dist-test/payload.json"
printf D > "$DT/bootstrap.ps1"; mkdir -p "$DT/fault-json"
vr run 'clean Windows' "$RS/alpha.suite" --record; rc=$?
ok '[ $rc = 2 ] && grep -q "dist-test/bootstrap.ps1 isn.t .*dist-test/bootstrap.ps1" "$T/out"' "--record refused: the share's bootstrap.ps1 isn't the checkout's dist-test/'s"
rm -rf "$DT/fault-json" "$DT/bootstrap.ps1" "$R/dist-test/bootstrap.ps1"
# a server that serves other bytes than the file's: a payload.json link is never served (VS13)
cp "$DT/payload.json" "$T/payload.real"; rm "$DT/payload.json"; ln -s "$T/payload.real" "$DT/payload.json"; reset
vr run 'clean Windows' "$RS/gamma.suite"; rc=$?
ok '[ $rc = 2 ] && grep -q "payload.json served on .* isn.t" "$T/out"' "a payload.json the server won't send as the share's: the run stops"
rm "$DT/payload.json"; cp "$T/payload.real" "$DT/payload.json"
# --record's inputs: the TEST build, the staged share, the checkout (VR9)
cp "$DT/payload.json" "$T/payload.saved"
reset
for pj in '{"payload_version": "1.5.0"}' "{\"payload_version\": \"1.5.0\", \"commit\": \"$C-dirty\"}" '{"payload_version": "1.5.0", "commit": "0000000000000000000000000000000000000000"}'; do
  printf '%s\n' "$pj" > "$DT/payload.json"; cp "$DT/payload.json" "$R/dist-test/payload.json"
  vr run 'clean Windows' "$RS/alpha.suite" --record; rc=$?
  ok '[ $rc = 2 ] && grep -q "served payload.json" "$T/out" && ! calls | grep -q "^snapshot-switch"' "--record refused for the payload $pj"
done
cp "$T/payload.saved" "$DT/payload.json"; cp "$T/payload.saved" "$R/dist-test/payload.json"
echo ' ' >> "$R/dist-test/payload.json"
vr run 'clean Windows' "$RS/alpha.suite" --record; rc=$?
ok '[ $rc = 2 ] && grep -q "isn.t .*dist-test/payload.json" "$T/out"' "--record refused: the served payload.json isn't the checkout's dist-test/payload.json"
cp "$T/payload.saved" "$R/dist-test/payload.json"
mkdir -p "$T/share/vm/x64"; echo extra > "$T/share/vm/x64/extra.exe"
vr run 'clean Windows' "$RS/alpha.suite" --record; rc=$?
ok '[ $rc = 2 ] && grep -q "vm/ isn.t what" "$T/out"' "--record refused: vm/ changed since stage"
rm -f "$T/share/vm/x64/extra.exe"
echo 0000000000000000000000000000000000000000 > "$T/share/repo/.commit"
vr run 'clean Windows' "$RS/alpha.suite" --record; rc=$?
ok '[ $rc = 2 ] && grep -q "staged from HEAD" "$T/out"' "--record refused: repo/ staged from another commit"
vr stage > /dev/null
reset
vr run 'clean Windows' "$RS/delta.suite" --record; rc=$?
ok '[ $rc = 1 ] && [ -f "$G/delta@$K1-arm64.failed" ] && grep -q "^note during the run: the checkout changed" "$G/delta@$K1-arm64.failed"' "the checkout changed during a --record run: .failed, said (VR10)"
rm -f "$R/stray.txt"
touch "$R/dirty"
vr run 'clean Windows' "$RS/alpha.suite" --record; rc=$?
ok '[ $rc = 2 ] && grep -q "clean checkout" "$T/out"' '--record refused in a dirty checkout'
rm -f "$R/dirty"

# ---- one run at a time
reset
mkdir -p "$T/share/.vm-run.lock"; sleep 60 & holder=$!; echo "$holder" > "$T/share/.vm-run.lock/pid"
vr run 'clean Windows' "$T/suites/good.suite"; rc=$?
ok '[ $rc = 2 ] && grep -q "another vm-run.sh" "$T/out" && ! calls | grep -q "^snapshot-switch"' 'a second run refused while one holds the lock'
: > "$F/calls"
printf '%s\n' '#fake: out mine' | "$VR" sys > "$T/out" 2>&1; rc=$?
ok '[ $rc = 2 ] && grep -q "another vm-run.sh" "$T/out" && ! calls | grep -q "^exec"' 'a one-off script refused while a run holds the lock'
mkdir -p "$T/share/repo"; echo keep > "$T/share/repo/keep"
vr stage; rc=$?
ok '[ $rc = 2 ] && grep -q "another vm-run.sh" "$T/out" && [ -f "$T/share/repo/keep" ]' 'stage refused while a run holds the lock: the share as it was'
ok '[ "$(cat "$T/share/.vm-run.lock/pid")" = "$holder" ]' "a refused command leaves the holder's lock"
rm -f "$T/share/repo/keep"
kill "$holder" 2> /dev/null; wait "$holder" 2> /dev/null
rm -f "$T/share/.vm-run.lock/pid"
vr run 'clean Windows' "$T/suites/good.suite"; rc=$?
ok '[ $rc = 2 ] && grep -q "no pid yet" "$T/out" && [ -d "$T/share/.vm-run.lock" ]' "a lock without a pid, younger than 10 s: refused, left (VR12)"
touch -t 202001010000 "$T/share/.vm-run.lock"
vr run 'clean Windows' "$T/suites/good.suite"; ok '[ $? = 0 ] && [ ! -e "$T/share/.vm-run.lock" ]' "an old lock without a pid is taken over, and released"
mkdir -p "$T/share/.vm-run.lock"; echo "$holder" > "$T/share/.vm-run.lock/pid"
vr run 'clean Windows' "$T/suites/good.suite"; ok '[ $? = 0 ] && [ ! -e "$T/share/.vm-run.lock" ]' "a stale lock (its pid gone) is taken over, and released"
runid="$(basename "$(last_results)")"; launch="$T/share/job/$runid/2-two.launch.ps1"
ok 'grep -qF "Get-Content \$out -Encoding UTF8" "$launch" && grep -qF "[Console]::OutputEncoding = New-Object Text.UTF8Encoding \$false" "$launch"' "the launcher reads hidden.exe's log as UTF-8 and passes it on as UTF-8 (VS15)"
ok 'grep -qF "\$task = '\''rbstems-run-$runid'\''" "$launch" && grep -qF "Unregister-ScheduledTask -TaskName \$task" "$launch" && ! grep -qF "TaskName '\''rbstems-run'\''" "$launch"' "a user step's task is the run's own (rbstems-run-<run>), unregistered at its end"

# ---- a failed revert stops the run
reset; touch "$F/switch-fails"
vr run 'clean Windows' "$T/suites/good.suite"; rc=$?
ok '[ $rc = 2 ] && ! calls | grep -q "^exec"' 'a failed snapshot switch: exit 2, nothing run'
reset; echo 1000 > "$F/bootfails"
VMRUN_BOOT=2 vr run 'clean Windows' "$T/suites/good.suite"; rc=$?
ok '[ $rc = 2 ] && grep -q "cmd /c ver within 2 s" "$T/out" && [ "$(calls | grep -c "^snapshot-switch")" = 2 ] && calls | grep -q "^stop"' 'a VM that never answers: bounded, exit 2, stopped and reverted again'
echo 0 > "$F/bootfails"
reset; touch "$F/prepare-fails"
vr run 'clean Windows' "$T/suites/good.suite"; rc=$?
last_stop=$(calls | grep -n '^stop' | tail -1 | cut -d: -f1); last_switch=$(calls | grep -n '^snapshot-switch' | tail -1 | cut -d: -f1)
ok '[ $rc = 2 ] && [ -n "$last_stop" ] && [ "$last_stop" -lt "$last_switch" ] && [ "$(calls | grep -c "^snapshot-switch")" = 2 ] && [ "$(cat "$F/state")" = stopped ] && [ ! -e "$T/share/.vm-run.lock" ]' 'a failed prepare after the revert: stopped, reverted again, then the lock released (VR13)'
rm -f "$F/prepare-fails"; reset; touch "$F/prepare-fails"
vr run 'clean Windows' "$T/suites/good.suite" --keep; rc=$?
ok '[ $rc = 2 ] && [ "$(calls | grep -c "^snapshot-switch")" = 1 ] && ! calls | grep -q "^stop"' 'with --keep, an early end leaves the VM as it is'
rm -f "$F/prepare-fails"

# ---- one-off scripts (winps.sh's job)
reset
printf '%s\n' '#fake: out once' '#fake: rc 5' | "$VR" sys > "$T/out" 2>&1; rc=$?
ok '[ $rc = 5 ] && grep -q "^once" "$T/out" && ! calls | grep -q "^snapshot-switch"' 'sys: the status passed on, no revert'
printf '%s\n' '#fake: out mine' | "$VR" user-signed 30 > "$T/out" 2>&1; rc=$?
ok '[ $rc = 0 ] && grep -q "launched as user-signed" "$T/out" && grep -q "^mine" "$T/out"' 'user-signed: through the launcher'
printf '%s\n' '#fake: sleep 30' | "$VR" sys 2 > "$T/out" 2>&1; rc=$?
ok '[ $rc = 124 ]' 'a one-off script past its timeout: 124'
echo x | "$VR" sys 601 > "$T/out" 2>&1; ok '[ $? = 2 ]' 'a timeout over 600 s refused'
t0=$(date +%s)
printf '%s\n' '#fake: bg-sleep 20' '#fake: out done' | "$VR" sys 60 > "$T/out" 2>&1; rc=$?
took=$(( $(date +%s) - t0 ))
ok '[ $rc = 0 ] && [ "$took" -lt 10 ]' "what a prlctl leaves in its group is killed when it ends: no wait on its output (${took}s) (VR15)"
# an interrupt stops prlctl's whole group and still reverts the VM (VR14, VR13)
reset
printf '%s\n' 'sys 60 slow s/slow.ps1' > "$T/suites/longslow.suite"
t0=$(date +%s)
perl -e 'setpgrp(0, 0); exec @ARGV' "$VR" run 'clean Windows' "$T/suites/longslow.suite" > "$T/out" 2>&1 & vp=$!
until grep -q "step 1: slow" "$T/out" 2> /dev/null || [ $(( $(date +%s) - t0 )) -ge 20 ]; do sleep 0.5; done
sleep 1; kill -TERM -"$vp" 2> /dev/null; wait "$vp"; rc=$?
took=$(( $(date +%s) - t0 ))
ok '[ $rc = 130 ] && [ "$took" -lt 25 ] && [ "$(calls | grep -c "^snapshot-switch")" = 2 ] && [ ! -e "$T/share/.vm-run.lock" ]' "TERM during a step: exit 130 in ${took}s, prlctl's group stopped, the VM reverted, the lock released"
: > "$F/calls"; echo 'Windows 11' > "$F/name"
echo x | "$VR" sys > "$T/out" 2>&1; ok '[ $? = 2 ] && ! calls | grep -q "^exec"' "sys on the owner's VM refused"

# ---- quoting (VR30)
eval "$(grep '^q() ' "$VR")"
ok '[ "$(q "owner'\''s")" = "'\''owner'\'''\''s'\''" ]' "q doubles a ' for PowerShell, also on bash ${BASH_VERSINFO[0]}"
cp "$T/suites/good.suite" "$T/suites/owner's run.suite"
reset
vr run 'clean Windows' "$T/suites/owner's run.suite"; rc=$?
ok '[ $rc = 0 ] && basename "$(last_results)" | grep -qE "^[0-9]{8}-[0-9]{6}-owner_s_run-[0-9]+$"' "a suite name with a quote and a space: the run's id is [A-Za-z0-9._-] ($(basename "$(last_results)"))"
ok 'grep -qF "finally { Set-Content" "$(ls "$T/share/job"/*owner_s_run*/4-four.launch.ps1)"' "user-signed writes .done in a finally"

# ---- need file, include, SKIP (T17's suites)
reset
printf '%s\n' '# a helper' '#fake: out from the include' > "$T/suites/inc.ps1"
printf '%s\n' '#fake: out skipped here' '#fake: rc 77' > "$T/suites/s/skip.ps1"
printf '%s\n' 'include inc.ps1' 'need file staged/thing.txt' 'sys 60 one s/pass.ps1' 'sys 60 maybe s/skip.ps1' > "$T/suites/t17.suite"
vr run 'clean Windows' "$T/suites/t17.suite"; rc=$?
ok '[ $rc = 2 ] && grep -q "needs .*staged/thing.txt" "$T/out" && ! calls | grep -q "^snapshot-switch"' 'need file: a missing file refused before the VM is touched'
mkdir -p "$T/share/staged"; echo x > "$T/share/staged/thing.txt"
reset
vr run 'clean Windows' "$T/suites/t17.suite"; rc=$?
res="$(last_results)"
ok '[ $rc = 0 ] && grep -q "^step sys one PASS " "$res/results.txt" && grep -q "^step sys maybe SKIP 77 " "$res/results.txt" && grep -q "1 skipped" "$T/out"' "a SKIP (77) is said, and isn't a failure ($(tr '\n' ';' < "$res/results.txt"))"
job="$(ls "$T/share/job"/*/1-one.ps1 | tail -1)"
ok 'grep -n "from the include" "$job" | grep -q . && [ "$(grep -n "from the include" "$job" | cut -d: -f1)" -lt "$(grep -n "fake: out hello" "$job" | cut -d: -f1)" ] && [ "$(grep -n "VmRun = " "$job" | cut -d: -f1)" -lt "$(grep -n "from the include" "$job" | cut -d: -f1)" ]' 'include: after the prelude, before the step'
ok 'grep -q "from the include" "$res/1-one.log"' "the include's lines run"
reset
vr run 'clean Windows' "$RS/skipper.suite" --record; rc=$?
ok '[ -f "$G/skipper@$K1-arm64.failed" ] && [ ! -e "$G/skipper@$K1-arm64.ok" ] && grep -qx "result skip" "$G/skipper@$K1-arm64.failed"' "--record with a SKIP: .failed, \"result skip\" ($(grep -m1 'vm-run.sh: ' "$T/out"))"
for bad in 'need file ../x' 'need file a b' 'need tea' 'include none.ps1' 'include inc.ps1 extra'; do
  printf '%s\n' "$bad" 'sys 60 one s/pass.ps1' > "$T/suites/bad.suite"
  vr run 'clean Windows' "$T/suites/bad.suite"; ok '[ $? = 2 ]' "refused: $bad"
done

# ---- stage: the repo's files and the built test exes into the share
mkdir -p "$R/build/winc/win-x64" "$R/build/common-win/arm64" "$R/build/winc"
echo exe > "$R/build/winc/win-x64/winc-wintest.exe"; echo pwt > "$R/build/common-win/arm64/plat-win-test.exe"; echo fx > "$R/build/winc/fixtures.txt"
vr stage; rc=$?
ok '[ $rc = 0 ] && [ -f "$T/share/repo/scripts/bootstrap.ps1" ] && [ -f "$T/share/repo/tests/win/bootstrap/bootstrap.tests.ps1" ] && [ -f "$T/share/repo/core/texts.def" ] && [ -f "$T/share/repo/win/c/aumid.h" ] && [ ! -e "$T/share/repo/core/other.c" ]' "stage: HEAD's scripts, tests/win, texts.def and aumid.h only"
ok '[ -f "$T/share/vm/x64/winc-wintest.exe" ] && [ -f "$T/share/vm/arm64/plat-win-test.exe" ] && [ -f "$T/share/vm/fixtures.txt" ] && [ ! -e "$T/share/vm/x64/plat-win-test.exe" ]' 'stage: the built test exes there are, by architecture'
ok '[ "$(cat "$T/share/repo/.commit")" = "$C" ] && grep -qx "$(shasum -a 256 "$T/share/vm/x64/winc-wintest.exe" | cut -c1-64) vm/x64/winc-wintest.exe" "$T/share/vm/shas.txt" && [ "$(grep -c . "$T/share/vm/shas.txt")" = 3 ]' 'stage: repo/.commit names HEAD, vm/shas.txt lists what it staged'
ok 'grep -q "^still missing" "$T/out" && grep -q "^  vm/x64/plat-win-test.exe (build/common-win/x64/plat-win-test.exe)$" "$T/out" && grep -q "^  vm/arm64/location-win-test.exe" "$T/out" && ! grep -q "^  vm/x64/winc-wintest.exe" "$T/out"' 'stage: says what is still missing, and from where'
FAKE_BUILD_FAIL=common vr stage --build; rc=$?
ok '[ $rc = 2 ] && grep -q "common/tests/win.sh build/common-win failed" "$T/out" && grep -q "common broke" "$R/build/vm-inputs/win-tests.log" && [ ! -e "$T/share/vm/x64/plat-win-test.exe" ] && [ -f "$T/share/vm/x64/winc-wintest.exe" ]' 'stage --build: a builder that fails stops it before the share is touched, its log kept'
vr stage --build; rc=$?
allvm() { local a f; for a in x64 arm64; do for f in winc-wintest.exe winc-app-wintest.exe plat-win-test.exe checked.dll rbsp-core.dll; do [ -f "$T/share/vm/$a/$f" ] || return 1; done; done; }
ok '[ $rc = 0 ] && allvm && [ "$(cat "$T/share/vm/arm64/winc-app-wintest.exe")" = "app arm64" ] && [ -f "$T/share/vm/fixtures.txt" ]' 'stage --build: builds and stages both architectures'"'"' test exes, the core DLL'"'"'s test-hooks build and fixtures.txt'
ok 'grep -q "^  vm/x64/location-win-test.exe" "$T/out" && ! grep -q "^  vm/x64/plat-win-test.exe" "$T/out" && ! grep -q "fixtures.txt" <(sed -n "/^still missing/,\$p" "$T/out")' 'stage --build: only what this branch can'"'"'t build is still missing'
# vm/.commit: only test exes stage --build built, now, from a clean HEAD (VS10)
ok '[ "$(cat "$T/share/vm/.commit")" = "$C" ] && grep -qx "$(shasum -a 256 "$T/share/repo/scripts/bootstrap.ps1" | cut -c1-64) repo/scripts/bootstrap.ps1" "$T/share/repo/shas.txt"' "stage --build at a clean HEAD: vm/.commit names it; repo/shas.txt lists repo/ (VS10, VS7)"
reset
vr run 'clean Windows' "$RS/vmneed.suite" --record; rc=$?
ok '[ $rc = 0 ] && grep -qx "vm_commit $C" "$G/vmneed@$K1-arm64.ok"' "a need vm suite records vm/'s commit"
vr stage > /dev/null
ok '[ ! -e "$T/share/vm/.commit" ]' "stage without --build: no vm/.commit (exes copied as already built)"
vr run 'clean Windows' "$RS/vmneed.suite" --record; rc=$?
ok '[ $rc = 2 ] && grep -q "weren.t built from HEAD" "$T/out"' "--record refused: a need vm suite without vm/ built from HEAD"
touch "$R/dirty"; vr stage --build; rc=$?; rm -f "$R/dirty"
ok '[ $rc = 0 ] && [ ! -e "$T/share/vm/.commit" ] && grep -q "vm/ gets no .commit" "$T/out"' "stage --build in a dirty checkout: no vm/.commit"
# --build with HEAD moving (VT3): a git wrapper commits B in the checkout right after its Nth
# `git status` (stage --build's first is before the build, its second just after it, before the
# lock and the archive): either way vm/ gets no .commit, and repo/.commit names the commit archived
real_git="$(command -v git)"; mkdir -p "$T/gitbuild"
cat > "$T/gitbuild/git" <<EOF
#!/bin/sh
"$real_git" "\$@"; rc=\$?
case " \$* " in *" status "*)
  n=\$((\$(cat "$T/gitbuild/n" 2> /dev/null || echo 0) + 1)); echo \$n > "$T/gitbuild/n"
  if [ "\$n" = "\$(cat "$T/gitbuild/at" 2> /dev/null)" ]; then echo b > "$R/b.txt"; "$real_git" -C "$R" add -A && "$real_git" -C "$R" -c user.email=t@t -c user.name=t commit -q -m B; fi ;;
esac
exit \$rc
EOF
chmod +x "$T/gitbuild/git"
for at in 1 2; do
  rm -f "$T/gitbuild/n"; echo "$at" > "$T/gitbuild/at"
  PATH="$T/gitbuild:$PATH" "$VR" stage --build > "$T/out" 2>&1; rc=$?
  NB="$(git -C "$R" rev-parse HEAD)"
  case $at in
    1) said="the checkout isn't clean (or HEAD moved): vm/ gets no .commit"; when="during the build" ;;
    2) said="HEAD is ${NB:0:9} now, not ${C:0:9} the exes were built from: vm/ gets no .commit"; when="after the build, before the archive" ;;
  esac
  ok '[ $rc = 0 ] && [ "$NB" != "$C" ] && [ ! -e "$T/share/vm/.commit" ] && grep -qF "$said" "$T/out" && [ "$(cat "$T/share/repo/.commit")" = "$NB" ]' "stage --build with HEAD moving $when: no vm/.commit, said; repo/.commit names the archived ${NB:0:9} (VT3)"
  git -C "$R" reset -q --hard "$C"
done
vr stage --build > /dev/null; ok '[ "$(cat "$T/share/vm/.commit")" = "$C" ]' "stage --build at the clean HEAD again: vm/.commit names it"
vr stage bogus; ok '[ $? = 2 ]' 'stage: an unknown option refused'

# ---- T17's suites parse, find their includes and scripts, and run through the fake
mkdir -p "$T/share/dist-test/a" "$T/share/dist-test/carried" "$T/share/dist-test/new"
echo x > "$T/share/dist-test/bootstrap.ps1"
for r in a carried new; do echo '{}' > "$T/share/dist-test/$r/payload.json"; done
# bridge.suite's need file lines: what bridge/win/tests/stage.sh puts in the share
mkdir -p "$T/share/bridge-win/fake" "$T/share/bridge-win/model"
for f in run.ps1 onnxruntime.dll harness.exe fake/onnxruntime.dll model/hdemucs.onnx; do echo x > "$T/share/bridge-win/$f"; done
for s in bootstrap uninstall platform update smoke worker bridge uninstall-cases restore volumes; do
  reset
  snap='clean Windows'; case "$s" in worker|bridge|restore) snap='rekordbox 7.2.19 logged in' ;; esac   # each suite's snapshot (release.list's for its rows)
  vr run "$snap" "$here/tests/win/suites/$s.suite"; rc=$?
  res="$(last_results)"
  n=$(grep -c '^step ' "$res/results.txt" 2> /dev/null)
  ok '[ $rc = 0 ] && [ "$n" -ge 1 ] && ! grep -qv " PASS " "$res/results.txt"' "suite $s: $n steps parse and run through the fake ($(tail -2 "$T/out" | tr '\n' ' '))"
  # bridge.suite's network step (review TR71: BW3's row, the share mapped as a network drive)
  [ "$s" != bridge ] || ok 'grep -q "^step user network PASS " "$res/results.txt"' "suite bridge: its network step runs through the fake ($(grep "^step user network " "$res/results.txt" 2> /dev/null))"
done
ok 'grep -l "function Check" "$T/share/job"/*bootstrap*/1-unit-51.ps1 > /dev/null 2>&1' "a T17 step starts with vmlib.ps1"
ok 'f="$(ls "$T/share/job"/*worker*/3-cache-install.ps1 2> /dev/null | head -1)"; [ -n "$f" ] && [ "$(grep -n "function Check" "$f" | cut -d: -f1)" -lt "$(grep -n "function Close-Rekordbox" "$f" | cut -d: -f1)" ]' "worker.suite's steps start with vmlib.ps1, then worker/rblib.ps1"
ok 'f="$(ls "$T/share/job"/*restore*/5-restore.ps1 2> /dev/null | head -1)"; [ -n "$f" ] && [ "$(grep -n "function Check" "$f" | cut -d: -f1)" -lt "$(grep -n "function Close-Rekordbox" "$f" | cut -d: -f1)" ] && [ "$(grep -n "function Close-Rekordbox" "$f" | cut -d: -f1)" -lt "$(grep -n "function Invoke-Restore" "$f" | cut -d: -f1)" ]' "restore.suite's steps start with vmlib.ps1, worker/rblib.ps1, then restore/rslib.ps1"
ok 'f="$(ls "$T/share/job"/*uninstall-cases*/3-menu-uninstall.ps1 2> /dev/null | head -1)"; [ -n "$f" ] && grep -q "function Invoke-MenuUninstall" "$f"' "uninstall-cases.suite's steps start with uninstall/cases/unlib.ps1 too"
ok 'f="$(ls "$T/share/job"/*volumes*/2-ntfs-cases.ps1 2> /dev/null | head -1)"; [ -n "$f" ] && grep -q "function Test-Loc" "$f" && grep -q "function Check" "$f"' "volumes.suite's steps start with vmlib.ps1 and volumes/vlib.ps1"
# ---- the PowerShell itself (VR17): the text ids exist; with pwsh, every assembled script (the
# steps with their prelude and includes, the wrappers, the launchers, the prepare step) parses
# with no error and uses no PowerShell 7-only syntax (&&, ||, ??, ?., ?[, the ternary ?:)
bad=""
for id in $(grep -rhoE "Get-CoreText '[A-Z0-9_]+'" "$here/tests/win/suites" | sed "s/.*'\(.*\)'/\1/" | sort -u); do
  grep -qE "^TEXT\($id," "$here/core/texts.def" || bad+=" $id"
done
ok '[ -z "$bad" ]' "every Get-CoreText id is in core/texts.def${bad:+ (missing:$bad)}"
bad=""
for k in $(grep -rhoE "Get-BootText '[A-Za-z0-9]+'" "$here/tests/win/suites" | sed "s/.*'\(.*\)'/\1/" | sort -u); do
  grep -qE "^[[:space:]]+$k[[:space:]]+=[[:space:]]+['\"]" "$here/scripts/bootstrap.ps1" || bad+=" $k"
done
ok '[ -z "$bad" ]' "every Get-BootText key is one of scripts/bootstrap.ps1's \$Texts${bad:+ (missing:$bad)}"
ok '! grep -rF "'\''RB Stems Plus cleanup'\''" "$here/tests/win/suites" | grep -qv "StartsWith("' "the RunOnce cleanup values are matched by their prefix (UN13: one per temp folder), never by one name"
if command -v pwsh > /dev/null 2>&1; then
  cat > "$T/parse.ps1" <<'EOF'
$n = 0; $errs = 0; $seven = 0
$kinds = 'AndAnd', 'OrOr', 'QuestionQuestion', 'QuestionQuestionEquals', 'QuestionDot', 'QuestionLBracket', 'QuestionMark'
foreach ($f in Get-ChildItem -LiteralPath $args[0] -Recurse -File -Filter '*.ps1') {
    $n++
    $t = $null; $e = $null
    [void][System.Management.Automation.Language.Parser]::ParseFile($f.FullName, [ref]$t, [ref]$e)
    foreach ($x in $e) { $errs++; "PARSE $($f.Name):$($x.Extent.StartLineNumber): $($x.Message)" }
    foreach ($k in $t) { if ($kinds -contains [string]$k.Kind) { $seven++; "PS7 $($f.Name):$($k.Extent.StartLineNumber): $($k.Text)" } }
}
"pwsh: $n files, $errs parse errors, $seven 7-only tokens"
EOF
  pwsh -NoProfile -NonInteractive -File "$T/parse.ps1" "$T/share/job" > "$T/parse.out" 2>&1
  nps="$(sed -n 's/^pwsh: \([0-9]*\) files.*/\1/p' "$T/parse.out")"
  ok 'grep -q "^pwsh: [0-9]* files, 0 parse errors, 0 7-only tokens$" "$T/parse.out" && [ "${nps:-0}" -ge 60 ]' "every assembled script parses in pwsh, no 7-only syntax ($(grep -v '^PS7\|^PARSE' "$T/parse.out" | tr '\n' ' ')$(grep -m3 '^PS7\|^PARSE' "$T/parse.out" | tr '\n' ' '))"
  # the Windows bridge's run.ps1 (bridge.suite's steps dot-source it from the share)
  pwsh -NoProfile -NonInteractive -File "$T/parse.ps1" "$here/bridge/win/tests" > "$T/parse-bridge.out" 2>&1
  ok 'grep -q "^pwsh: [1-9][0-9]* files, 0 parse errors, 0 7-only tokens$" "$T/parse-bridge.out"' "bridge/win/tests' PowerShell parses in pwsh, no 7-only syntax ($(tr '\n' ' ' < "$T/parse-bridge.out"))"
  printf '%s\n' '$a = "no $x: here"' > "$T/share/job/broken.ps1"
  pwsh -NoProfile -NonInteractive -File "$T/parse.ps1" "$T/share/job" > "$T/parse.out" 2>&1
  ok 'grep -q "^PARSE broken.ps1:1:" "$T/parse.out"' "the parse check finds a \"\$x:\" string (VR4's kind)"
  rm -f "$T/share/job/broken.ps1"
else
  echo "SKIP the pwsh parse of the assembled scripts: no pwsh here (brew install powershell)"
fi

# ---- stage-dist.sh: the fault releases and bad-version/, with scratch keys made here only
SD="$here/tests/win/suites/stage-dist.sh"
mkkey() { mkdir -p "$1" && (umask 077 && /usr/bin/openssl ecparam -name prime256v1 -genkey -noout -out "$1/test-key.pem") && /usr/bin/openssl ec -in "$1/test-key.pem" -pubout -out "$1/test.pub.pem" 2> /dev/null; }
mkkey "$T/k1"; mkkey "$T/k2"; mkdir -p "$T/norel"
export STAGE_DIST_RELEASE_KEYS="$T/norel"
D="$T/sd"; mkdir -p "$D/dist-test"
printf '%s' '{"payload_version": "1.5.0", "windows": {"app": {"zip": "w.zip", "version": "1.5.0"}}}' > "$D/dist-test/payload.json"
/usr/bin/openssl dgst -sha256 -sign "$T/k1/test-key.pem" -out "$D/dist-test/payload.json.sig" "$D/dist-test/payload.json"
cp "$D/dist-test/payload.json.sig" "$T/sig.saved"
printf 'ZIP' > "$D/dist-test/w.zip"; printf 'B' > "$D/dist-test/bootstrap.ps1"
"$SD" "$D" --key "$T/k2" --key "$T/k1" > "$T/out" 2>&1; rc=$?
ok '[ $rc = 0 ] && [ "$(cat "$D/dist-test/fault-zip/w.zip")" = ZIPX ] && cmp -s "$D/dist-test/fault-zip/payload.json.sig" "$T/sig.saved" && [ "$(tail -c 1 "$D/dist-test/fault-sig/payload.json.sig")" = X ] && [ "$(tail -c 1 "$D/dist-test/fault-json/payload.json")" = X ] && [ -f "$D/dist-test/fault-json/bootstrap.ps1" ]' 'stage-dist.sh: one byte appended to the zip, the json, the sig, the rest as it was'
B="$D/dist-test/bad-version"
ok '[ -f "$B/bootstrap.ps1" ] && [ "$(cat "$B/w.zip")" = ZIP ] && [ "$(python3 -I -c "import json,sys; print(json.load(open(sys.argv[1]))[\"windows\"][\"app\"][\"version\"])" "$B/payload.json")" = 1.5.1 ] && /usr/bin/openssl dgst -sha256 -verify "$T/k1/test.pub.pem" -signature "$B/payload.json.sig" "$B/payload.json" > /dev/null 2>&1 && grep -q "k2 didn.t sign this TEST build" "$T/out"' "stage-dist.sh: bad-version/ names another app version, signed with the key that signed the TEST build (a key that didn't is passed over)"
"$SD" "$D" --key "$T/k2" > "$T/out" 2>&1; rc=$?
ok '[ $rc = 0 ] && [ ! -e "$B" ] && grep -q "bad-version/ not made" "$T/out" && [ -d "$D/dist-test/fault-zip" ]' "stage-dist.sh: no key that signed the TEST build: no bad-version/, said; the faults still made"
cp "$T/k1/test.pub.pem" "$T/norel/release.pub.pem"
"$SD" "$D" --key "$T/k1" > "$T/out" 2>&1; rc=$?
ok '[ $rc = 2 ] && grep -q "a release key never signs" "$T/out" && [ ! -e "$B" ]' "stage-dist.sh: a key pair whose public half is one of keys/*.pub.pem is refused"
rm -f "$T/norel/release.pub.pem"
printf '%s' '{"windows": {"app": {"zip": "../w.zip"}}}' > "$D/dist-test/payload.json"
"$SD" "$D" --key "$T/k1" > "$T/out" 2>&1; ok '[ $? = 2 ]' 'stage-dist.sh: a zip name with a path refused'
# stage makes them too, for the share's signed TEST build, with the checkout's own test key
mkdir -p "$R/build/test-keys"; cp "$T/k1/test-key.pem" "$T/k1/test.pub.pem" "$R/build/test-keys/"
cp "$DT/payload.json" "$T/payload.saved"
printf '%s' '{"payload_version": "1.5.0", "windows": {"app": {"zip": "w.zip", "version": "2.0.0"}}}' > "$DT/payload.json"
/usr/bin/openssl dgst -sha256 -sign "$T/k1/test-key.pem" -out "$DT/payload.json.sig" "$DT/payload.json"; printf 'ZIP' > "$DT/w.zip"
vr stage; rc=$?
ok '[ $rc = 0 ] && [ -d "$DT/fault-zip" ] && [ -d "$DT/fault-json" ] && [ -d "$DT/fault-sig" ] && grep -q "2.0.0 -> 2.0.1, signed with the test key in $R/build/test-keys" "$T/out" && ! grep -q "fault-\*/ and bad-version/" "$T/out"' "stage: the share's signed TEST build gets its faults and bad-version/, with the checkout's test key"
cp "$T/payload.saved" "$DT/payload.json"; rm -f "$DT/payload.json.sig"

# ---- tests/win/first-run.sh against the fake (never against a VM here)
FRS="$R/tests/win/first-run.sh"   # the checkout's own, as vm-run.sh (VT2)
# the share as a release's --record runs want it: no stray fixture copies, the checkout's TEST build
rm -rf "$DT"/fault-* "$DT/bad-version"; cp "$DT/bootstrap.ps1" "$R/dist-test/bootstrap.ps1"; mkrels
reset
"$FRS" --dry-run < /dev/null > "$T/out" 2>&1; rc=$?
ok '[ $rc = 0 ] && [ ! -s "$F/calls" ] && [ "$(grep -c "== step [0-9]" "$T/out")" = 10 ] && grep -q "would run (at most 1800s): vm-run.sh run clean Windows smoke" "$T/out" && grep -q "DRY RUN: nothing was run" "$T/out"' "first-run.sh --dry-run: steps 0-9 said, prlctl never called"
reset
printf 'y\nn\n' | "$FRS" > "$T/out" 2>&1; rc=$?
ok '[ $rc = 0 ] && grep -q "first run: every step passed" "$T/out" && grep -q "ok: exit 3 reaches the Mac as 3" "$T/out" && grep -q "ok: its processes in the VM were ended" "$T/out" && grep -q "ok: the VM is stopped, no lock" "$T/out" && grep -q "ok: the defender step passed" "$T/out" && grep -q "vm-run.sh check: $C passed" "$T/out" && ls "$T/share/results"/first-run-*.log > /dev/null 2>&1' "first-run.sh: steps 0-9 through the fake, with its log ($(grep -m1 STOP "$T/out"))"
reset
printf 'y\ny\n' | "$FRS" --from 1 > "$T/out" 2>&1; rc=$?
ok '[ $rc = 1 ] && grep -q "STOP at step 1: the answer was y to: Did any window" "$T/out" && ! grep -q "== step 2" "$T/out"' "first-run.sh: a window on the Mac's screen stops it at step 1"
reset; touch "$F/rc-zero"
printf 'y\n' | "$FRS" --from 3 > "$T/out" 2>&1; rc=$?
ok '[ $rc = 1 ] && grep -q "STOP at step 3: exit 3 reaches the Mac as 3" "$T/out" && grep -q "the VM: stopped" "$T/out" && ! grep -q "== step 4" "$T/out"' "first-run.sh: a prlctl that loses exit codes stops it at step 3, the VM stopped"
rm -f "$F/rc-zero"
printf '' | FIRST_RUN_ASK_SECS=1 "$FRS" --from 6 > "$T/out" 2>&1; rc=$?
ok '[ $rc = 1 ] && grep -q "no answer within 1 s" "$T/out"' "first-run.sh: no answer from the owner: it stops"
# --from N reads first-run.state, never the newest folder (VT9)
cp "$T/share/results/first-run.state" "$T/state.saved"
ok 'grep -q "^smoke $C " "$T/state.saved" && grep -q "^uninstall $C " "$T/state.saved"' "first-run.sh: steps 1 and 7 record their results folders with HEAD in first-run.state (VT9)"
sed -i '' "s/^\(uninstall\) $C /\1 0000000000000000000000000000000000000000 /" "$T/share/results/first-run.state"
printf 'y\n' | "$FRS" --from 8 > "$T/out" 2>&1; rc=$?
ok '[ $rc = 1 ] && grep -q "STOP at step 8: .*first-run.state.s uninstall run is of 0000000000000000000000000000000000000000, not HEAD $C" "$T/out"' "first-run.sh --from 8: a state of another HEAD stops it (VT9)"
rm -f "$T/share/results/first-run.state"
printf 'y\n' | "$FRS" --from 2 > "$T/out" 2>&1; rc=$?
ok '[ $rc = 1 ] && grep -q "STOP at step 2: .*names no smoke run" "$T/out" && ! grep -q "== step 3" "$T/out"' "first-run.sh --from 2: no state, no guess from the newest folder: it stops (VT9)"
cp "$T/state.saved" "$T/share/results/first-run.state"
# a share whose path has a space, as the owner's (RB Stems Plus): steps 2 and 8 read back the
# folders steps 1 and 7 wrote (VX2)
ln -s "$T/share" "$T/the share"; reset
printf 'y\nn\n' | VMRUN_SHARE="$T/the share" "$FRS" --from 1 > "$T/out" 2>&1; rc=$?
ok '[ $rc = 0 ] && grep -q "first run: every step passed" "$T/out" && grep -q "^smoke $C $T/the share/results/" "$T/share/results/first-run.state"' "first-run.sh: a share path with a space: steps 1-2 and 7-8 through first-run.state (VX2: $(grep -m1 STOP "$T/out"))"
rm -f "$T/the share"
# TERM to first-run.sh's own pid (not its group) ends vm-run.sh too, after its cleanup: the VM
# reverted, the lock released, then the stop said (VX10)
reset; rm -f "$T/fr.out"
printf 'y\n' | perl -e 'setpgrp(0, 0); exec @ARGV' "$FRS" --from 5 > "$T/fr.out" 2>&1 & fp=$!
t0=$(date +%s)
until grep -q "step 1: sleeper" "$T/fr.out" 2> /dev/null || [ $(( $(date +%s) - t0 )) -ge 20 ]; do sleep 0.5; done
sleep 1; kill -TERM "$fp" 2> /dev/null; wait "$fp"; rc=$?
lock_after="$([ -e "$T/share/.vm-run.lock" ] && echo held || echo free)"; switches="$(calls | grep -c "^snapshot-switch")"
t0=$(date +%s); until grep -q "STOP at step 5" "$T/fr.out" 2> /dev/null || [ $(( $(date +%s) - t0 )) -ge 5 ]; do sleep 0.2; done
ok '[ $rc = 1 ] && [ "$lock_after" = free ] && [ "$switches" = 2 ] && grep -q "STOP at step 5: interrupted during vm-run.sh" "$T/fr.out"' "first-run.sh: TERM to its own pid ends vm-run.sh after its cleanup, then stops (VX10: rc $rc, lock $lock_after, $switches switches)"
# (unfixed, vm-run.sh lives on: the next cases wait for the share's lock to go, bounded)
t0=$(date +%s); while [ -e "$T/share/.vm-run.lock" ] && [ $(( $(date +%s) - t0 )) -lt 60 ]; do sleep 0.5; done
# first-run.sh's bounded (VT4, VT6): a leftover in the child's group ends with it; a child's
# cleanup after TERM is waited for past the old 60 s, its lines streamed as they come
eval "$(sed -n '/^bounded() {/,/^}/p' "$FRS")"
t0=$(date +%s); out="$(bounded 1 bash -c 'sleep 4 & echo hi; exit 0')"; rc=$?; t1=$(date +%s)
ok '[ $rc = 0 ] && [ "$out" = hi ] && [ $((t1 - t0)) -le 2 ]' "first-run.sh bounded: a child's leftover (sleep 4 &) is killed when it ends: back in $((t1 - t0)) s (VT6)"
t0=$(date +%s)
bounded 2 bash -c 'trap "echo cleanup-start; sleep 70; echo cleanup-done; exit 3" TERM; echo started; while :; do sleep 1; done' > "$T/bounded.out" 2>&1 & bp=$!
until grep -q cleanup-start "$T/bounded.out" 2> /dev/null || [ $(( $(date +%s) - t0 )) -ge 20 ]; do sleep 0.5; done
ok 'grep -q "^started$" "$T/bounded.out" && grep -q "^cleanup-start$" "$T/bounded.out" && kill -0 "$bp" 2> /dev/null' "first-run.sh bounded: the child's lines come while it runs (VT5's streaming has them)"
wait "$bp"; rc=$?; t1=$(date +%s)
ok '[ $rc = 124 ] && grep -q "^cleanup-done$" "$T/bounded.out" && [ $((t1 - t0)) -ge 70 ]' "first-run.sh bounded: a 70 s cleanup after TERM is waited for, then 124 (VT4: $((t1 - t0)) s)"
# a TERM, then the bound running out during the child's cleanup: finish runs once, its one TERM
# to the group, and the cleanup ends whole: 130, never the alarm's 124 (VX5)
# (the child writes its parent's pid, bounded's perl: no pgrep in the sandbox)
rm -f "$T/bounded.pid"
bounded 2 bash -c 'echo $PPID > "$1"; trap "echo cleanup-start; sleep 3; echo cleanup-done; exit 3" TERM; while :; do sleep 0.1; done' x "$T/bounded.pid" > "$T/bounded.out" 2>&1 & bp=$!
sleep 0.5; kill -TERM "$(cat "$T/bounded.pid" 2> /dev/null)"
wait "$bp"; rc=$?
ok '[ $rc = 130 ] && grep -q "^cleanup-done$" "$T/bounded.out"' "first-run.sh bounded: TERM, then the bound out during a 3 s cleanup: 130, the cleanup whole (VX5: rc $rc)"
# step 9's verdict on check (VT7): exit 1 with exactly the expected lines, nothing else
eval "$(sed -n '/^check_ok() {/,/^}/p' "$FRS")"
NR1='vm-run.sh check: no record of worker on "rekordbox 7.2.19 logged in" (arm64) in /g/vm-win'
NR2='vm-run.sh check: no record of other on "clean Windows" (arm64) in /g/vm-win'
check_ok 1 "$NR1" "$NR1" > /dev/null; ok '[ $? = 0 ]' "check_ok: exit 1 with exactly the row without a suite: ok"
check_ok 1 "$NR1"$'\n'"$NR2" "$NR2"$'\n'"$NR1" > /dev/null; ok '[ $? = 0 ]' "check_ok: each expected line once, in any order: ok"
check_ok 42 "$NR1" "" > /dev/null; ok '[ $? = 1 ]' "check_ok: check ending 42 with no output stops step 9 (VT7)"
check_ok 1 "$NR1" "vm-run.sh: another vm-run.sh (pid 1) drives the VM" > /dev/null; ok '[ $? = 1 ]' "check_ok: a vm-run.sh stop line instead of the expected one stops step 9 (VT7)"
check_ok 1 "$NR1"$'\n'"$NR2" "$NR2" > /dev/null; ok '[ $? = 1 ]' "check_ok: a missing \"no record of worker\" line stops step 9 (VT7)"
check_ok 1 "$NR1" "$NR1"$'\n'"$NR1" > /dev/null; ok '[ $? = 1 ]' "check_ok: an expected line twice stops step 9 (VT7)"
check_ok 1 "$NR1" "$NR1"$'\n'"vm-run.sh check: beta on \"x\" (arm64) failed: beta@k-arm64.failed" > /dev/null; ok '[ $? = 1 ]' "check_ok: another failing row besides stops step 9 (VT7)"
check_ok 1 "" "" > /dev/null; ok '[ $? = 1 ]' "check_ok: a silent failure with no row expected to fail stops step 9 (VT7)"
check_ok 0 "" "vm-run.sh check: x passed" > /dev/null; ok '[ $? = 0 ]' "check_ok: a pass with no row expected to fail: ok"
check_ok 0 "$NR1" "" > /dev/null; ok '[ $? = 1 ]' "check_ok: a pass although a row has no suite stops step 9"
# end to end: a release.list row with no suite: step 9 passes on exactly its "no record" line
printf '%s\n' 'ghost arm64 clean Windows' >> "$RS/release.list"
git -C "$R" add -A && git -C "$R" -c user.email=t@t -c user.name=t commit -q -m ghost
C="$(git -C "$R" rev-parse HEAD)"
printf '{"payload_version": "1.5.0", "commit": "%s"}\n' "$C" > "$T/share/dist-test/payload.json"
cp "$T/share/dist-test/payload.json" "$R/dist-test/payload.json"; mkrels
reset
printf 'y\nn\n' | "$FRS" > "$T/out" 2>&1; rc=$?
ok '[ $rc = 0 ] && grep -q "ghost on \"clean Windows\" (arm64): no ghost.suite yet, not run" "$T/out" && grep -q "ok: check fails closed only on the rows no suite can pass yet: no suite: ghost;" "$T/out" && grep -q "first run: every step passed" "$T/out"' "first-run.sh: a row with no suite: step 9 passes on its one \"no record\" line ($(grep -m1 STOP "$T/out"))"
# step 9 and a known gap (first-run.sh's GAPS, worker:cache-hit): a row whose record skipped only
# that step fails check closed, and step 9 says so and passes; a SKIP of any other step stops it.
# The checkout gets a worker row and suite, committed: HEAD, the TEST build and the releases follow
gaprun() {   # FIRST-STEP-SCRIPT [older]: older: an older gap-only record under another snapshot id
  printf '%s\n' 'need http' "sys 60 one s/$1.ps1" 'sys 60 cache-hit s/skip.ps1' "# ${2:-}" > "$RS/worker.suite"
  git -C "$R" add -A && git -C "$R" -c user.email=t@t -c user.name=t commit -q -m "worker $1 ${2:-}"
  C="$(git -C "$R" rev-parse HEAD)"
  printf '{"payload_version": "1.5.0", "commit": "%s"}\n' "$C" > "$T/share/dist-test/payload.json"
  cp "$T/share/dist-test/payload.json" "$R/dist-test/payload.json"; mkrels
  if [ "${2:-}" = older ]; then
    mkdir -p "$T/gates/$C/vm-win"
    printf 'commit %s\nsuite worker\nsnapshot {99999999-aaaa-bbbb-cccc-000000000009} other\narch arm64\nrun older\nstep sys one PASS 0 1s\nstep sys cache-hit SKIP 77 1s\nresult skip\n' "$C" > "$T/gates/$C/vm-win/worker@99999999-aaaa-bbbb-cccc-000000000009-arm64.failed"
  fi
  reset
  printf 'y\nn\n' | "$FRS" > "$T/out" 2>&1
}
printf '%s\n' 'worker arm64 clean Windows' >> "$RS/release.list"
gaprun pass; rc=$?
ok '[ $rc = 0 ] && grep -q "vm-run.sh check: worker on \"clean Windows\" (arm64) failed: worker@" "$T/out" && grep -q "only a known gap skipped (worker:cache-hit): worker" "$T/out" && grep -q "first run: every step passed" "$T/out"' "first-run.sh: a known gap's SKIP (worker's cache-hit) fails check closed; step 9 says so and passes ($(grep -m1 STOP "$T/out"))"
gaprun skip; rc=$?
ok '[ $rc = 1 ] && grep -q "STOP at step 9: vm-run.sh check $C failed: vm-run.sh check: worker on" "$T/out"' "first-run.sh: the gap's row with another step skipped too stops step 9"
gaprun pass older; rc=$?
ok '[ $rc = 1 ] && grep -q "STOP at step 9: worker (arm64) has 2 records in" "$T/out"' "first-run.sh: an older gap-only record of worker under another snapshot id stops step 9 (WS3)"
# the gap row's line is accepted only as the whole of what check says: any other status or line
# stops step 9 (WS1)
GL='vm-run.sh check: worker on "clean Windows" (arm64) failed: worker@11111111-aaaa-bbbb-cccc-000000000001-arm64.failed'
check_ok 1 "$GL" "$GL" > /dev/null; ok '[ $? = 0 ]' "check_ok: exit 1 with exactly the gap row's line: ok"
check_ok 1 "$GL" "" > /dev/null; ok '[ $? = 1 ]' "check_ok: on the gap row, check exiting 1 silently stops step 9 (WS1)"
check_ok 2 "$GL" "vm-run.sh: another vm-run.sh (pid 1) drives the VM" > /dev/null; ok '[ $? = 1 ]' "check_ok: on the gap row, exit 2 with a stop line stops step 9 (WS1)"
check_ok 137 "$GL" "" > /dev/null; ok '[ $? = 1 ]' "check_ok: on the gap row, exit 137 stops step 9 (WS1)"
check_ok 1 "$GL" "$GL"$'\n'"tests/win/vm-run.sh: line 700: x: unbound variable" > /dev/null; ok '[ $? = 1 ]' "check_ok: the gap line followed by a crash line stops step 9 (WS1)"

echo "vm-run.sh: $pass passed, $fail failed"
[ "$fail" = 0 ]

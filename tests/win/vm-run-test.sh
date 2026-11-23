#!/bin/bash
# tests/win/vm-run.sh's offline tests (T16): every command against a fake prlctl (no VM, no
# network but 127.0.0.1). The fake keeps the VM's name, state and snapshots in files, logs every
# call, and plays a step's script by its "#fake:" lines (out TEXT, rc N, sleep N, hold NAME,
# prljob-once, http, mode).
#   tests/win/vm-run-test.sh          exit 0 when every check passed
set -uo pipefail
here="$(cd "$(dirname "$0")/../.." && pwd)"
VR="$here/tests/win/vm-run.sh"
T="$(mktemp -d "${TMPDIR:-/tmp}/vm-run-test.XXXXXX")" || exit 2
trap 'rm -rf "$T"' EXIT
pass=0; fail=0
ok() { if eval "$1"; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "FAIL $2"; fi; }

# stage and --record hash each file in a separate process. Use the native SHA-256
# utility when available to avoid starting Perl for each file in this harness.
if VMRUN_TEST_SHA256SUM=$(type -P sha256sum) && VMRUN_TEST_SHASUM=$(type -P shasum); then
  mkdir "$T/hash-bin"
  cat > "$T/hash-bin/shasum" <<'EOF'
#!/bin/sh
if [ "$1" = -a ] && [ "$2" = 256 ]; then
  shift 2
  exec "$VMRUN_TEST_SHA256SUM" "$@"
fi
exec "$VMRUN_TEST_SHASUM" "$@"
EOF
  chmod +x "$T/hash-bin/shasum"
  export VMRUN_TEST_SHA256SUM VMRUN_TEST_SHASUM PATH="$T/hash-bin:$PATH"
fi

wait_for() {
  local deadline=$((SECONDS + 20))
  until "$@"; do
    [ "$SECONDS" -lt "$deadline" ] || return 1
    sleep 0.05
  done
}

# VT4 needs to run past the old 60 s cleanup limit. Overlap it with the rest of the
# suite using only its own logs, and capture streaming while the child is still live.
first_run_cleanup_case() (
  eval "$(sed -n '/^bounded() {/,/^}/p' "$here/tests/win/first-run.sh")"
  local t0=$SECONDS bp rc
  bounded 2 bash -c 'trap "echo cleanup-start; sleep 70; echo cleanup-done; exit 3" TERM; echo started; while :; do sleep 1; done' > "$T/cleanup.out" 2>&1 & bp=$!
  if wait_for grep -qx cleanup-start "$T/cleanup.out" 2> /dev/null && kill -0 "$bp" 2> /dev/null; then
    touch "$T/cleanup.streaming"
  fi
  wait "$bp"; rc=$?
  printf '%s %s\n' "$rc" "$((SECONDS - t0))" > "$T/cleanup.result"
)
first_run_cleanup_case & cleanup_case=$!

# run parses before it checks the share. An absent share stops it before any VM or HTTP work.
parse_suite() {
  VMRUN_SHARE="$T/no-share" VMRUN_CONF="$T/no-conf" VMRUN_PRLCTL=false VMRUN_PRLSRVCTL=false \
    "$VR" run 'clean Windows' "$1" 2>&1
}
for suite in "$here"/tests/win/suites/*.suite; do
  suite_out="$(parse_suite "$suite")"; rc=$?
  ok '[ "$rc" = 2 ] && [ "$suite_out" = "vm-run.sh: no share $T/no-share" ]' \
    "parse ${suite##*/} (exit $rc: $suite_out)"
done
printf '%s\n' 'sys 60 missing missing-step.ps1' > "$T/missing-step.suite"
suite_out="$(parse_suite "$T/missing-step.suite")"; rc=$?
ok '[ "$rc" = 2 ] && [ "$suite_out" = "vm-run.sh: $T/missing-step.suite:1: no script $(cd "$T" && pwd)/missing-step.ps1" ]' \
  "parse missing-step.suite rejects its missing script (exit $rc: $suite_out)"

F="$T/fake"; mkdir -p "$F" "$T/bin" "$T/share/probe" "$T/share/dist-test" "$T/gates" "$T/suites/s"
echo rb-win-template > "$F/name"
echo running > "$F/state"
printf '%s\n' '{11111111-aaaa-bbbb-cccc-000000000001} clean Windows' \
              '{22222222-aaaa-bbbb-cccc-000000000002} rekordbox 7.2.19 installed' > "$F/snaps"
echo 0 > "$F/bootfails"
# the VM's host sharing (shares: NAME|+ or -|PATH; shf: on/off; defined: prlctl's "Host defined
# sharing"), and what a revert brings back (snapshares, snapdefined: "clean Windows" shares nothing)
printf 'win|+|%s\n' "$T/share" > "$F/shares"; echo on > "$F/shf"; echo Unknown > "$F/defined"
: > "$F/snapshares"; echo Off > "$F/snapdefined"
echo hidden > "$T/share/probe/hidden.exe"
cat > "$T/bin/prlctl" <<'EOF'
#!/bin/bash
F="$FAKE_DIR"; SHARE="$FAKE_SHARE"
fake_sleep() {
  local sleeper
  sleep "$1" & sleeper=$!
  echo "$sleeper" > "$F/sleep.pid"
  wait "$sleeper"
}
echo "$*" >> "$F/calls"
cmd="$1"; shift
case "$cmd" in
  list) printf 'ID: {%s}\nName: %s\nState: %s\n' "$2" "$(< "$F/name")" "$(< "$F/state")"
    printf 'Host Shared Folders: (%s)\n' "$([ "$(< "$F/shf")" = on ] && echo + || echo -)"
    while IFS='|' read -r n e p; do printf "  %s (%s) path='%s' mode='rw'\n" "$n" "$e" "$p"; done < "$F/shares"
    printf 'Host defined sharing: %s\nGuest Shared Folders: (+)\n' "$(< "$F/defined")" ;;
  snapshot-list)
    if [ "${2:-}" = -i ]; then grep -F "$3" "$F/snaps" | sed 's/^\([^ ]*\) \(.*\)$/ID: \1\nName: \2/'
    else echo 'PARENT_SNAPSHOT_ID                      SNAPSHOT_ID'; awk '{print "                                        " $1}' "$F/snaps"; fi ;;
  snapshot-switch) [ -e "$F/switch-fails" ] && exit 1
    cp "$F/snapshares" "$F/shares"; echo off > "$F/shf"; cp "$F/snapdefined" "$F/defined"; rm -f "$F/shf-pending"
    # as Parallels: Guest Shared Folders come back with the snapshot (leaves-hidden: a revert leaves
    # an empty "[C] <vm> N.hidden" folder in the Mac's volumes)
    echo on > "$F/shfguest"
    if [ -e "$F/leaves-hidden" ]; then n=$(grep -c '^snapshot-switch' "$F/calls")
      mkdir -p "$FAKE_VOLUMES/[C] $(< "$F/name") $n.hidden" "$FAKE_VOLUMES/[C] other $n.hidden" "$FAKE_VOLUMES/C $(< "$F/name") $n.hidden"; fi
    # a snapshot taken while the VM ran comes back running
    if [ -e "$F/switch-running" ]; then echo running; else echo stopped; fi > "$F/state" ;;
  set)
    case "$2" in
      --pause-idle) [ "$3" = off ] || exit 1 ;;
      --shf-guest) [ "$3" = off ] || exit 1; echo off > "$F/shfguest" ;;
      # as Parallels: switched on while the VM runs, host sharing takes effect at its next stop
      # (shf-lags: as Parallels 27 on a stopped VM, the switch shows only after the VM has run)
      --shf-host) if [ "$(< "$F/state")" = running ] || [ -e "$F/shf-lags" ]; then echo "$3" > "$F/shf-pending"; else echo "$3" > "$F/shf"; fi ;;
      --shf-host-add) [ "$4 $6 $7" = "--path --mode rw" ] || exit 1; printf '%s|+|%s\n' "$3" "$5" >> "$F/shares" ;;
      --shf-host-del) grep -v "^$3|" "$F/shares" > "$F/shares.new"; mv "$F/shares.new" "$F/shares" ;;
      --shf-host-set) [ "$4" = --enable ] || exit 1; sed "s/^$3|-|/$3|+|/" "$F/shares" > "$F/shares.new"; mv "$F/shares.new" "$F/shares" ;;
      *) exit 1 ;;
    esac ;;
  start|resume) echo running > "$F/state" ;;
  snapshot) [ "$2" = -n ] && [ -n "$3" ] || exit 1; [ -e "$F/snapshot-fails" ] && exit 1
    printf '{33333333-aaaa-bbbb-cccc-%012d} %s\n' "$(( $(wc -l < "$F/snaps") + 1 ))" "$3" >> "$F/snaps" ;;
  stop) echo stopped > "$F/state"; [ ! -e "$F/shf-pending" ] || mv "$F/shf-pending" "$F/shf" ;;
  exec)
    [ "$(< "$F/state")" = running ] || exit 255
    shift
    map() { local p="${1//\\//}"; f="$SHARE/${p#//Mac/win/}"; }
    if [ "$1 $2 $3" = "cmd /c dir" ]; then   # the guest looks for a file on \\Mac\win
      if [ -e "$F/share-unseen" ]; then
        n=$(< "$F/share-unseen")
        if [ "$n" -gt 0 ]; then echo $((n - 1)) > "$F/share-unseen"; exit 1; fi
      fi
      map "${@: -1}"
      grep -q '^win|+|' "$F/shares" && [ -e "$f" ]; exit $?
    fi
    if [ "$1" = cmd ]; then
      n=$(< "$F/bootfails"); if [ "$n" -gt 0 ]; then echo $((n - 1)) > "$F/bootfails"; exit 255; fi
      echo 'Microsoft Windows [Version 10.0.26200.1000]'; exit 0
    fi
    file=""; while [ $# -gt 0 ]; do [ "$1" = -File ] && file="$2"; shift; done
    grep -q '^win|+|' "$F/shares" || { echo "The argument '$file' to the -File parameter does not exist."; exit 1; }
    map "$file"
    [ -f "$f" ] || { echo "fake: no $f"; exit 1; }
    body=$(< "$f")
    if [[ "$body" = *vm-run-wait* ]]; then   # the wait step's sign-in probe (wait-left: "not yet" that many times)
      [ -e "$F/wait-nomark" ] && { echo 'vm-run-wait: no C:\rbtest\signout.mark'; exit 3; }
      n=0; [ ! -e "$F/wait-left" ] || n=$(< "$F/wait-left")
      if [ "$n" -gt 0 ]; then echo $((n - 1)) > "$F/wait-left"; echo 'vm-run-wait: not yet'; exit 75; fi
      [ -e "$F/wait-down" ] && { rm -f "$F/wait-down"; echo 'fake: the VM is restarting'; exit 255; }
      echo 'vm-run-wait: rb signed in'; exit 0
    fi
    if [[ "$body" = *vm-run-kill* ]]; then   # the end of a timed-out step's processes
      if [ -e "$F/kill-fails" ]; then echo 'still running: 1234 powershell.exe'; echo 'vm-run-kill: 1 left'; exit 1; fi
      echo 'vm-run-kill: 0 left'; exit 0
    fi
    if [[ "$body" = *rbstems-run* ]]; then   # a user step's launcher: play its job
      mode_pattern="[\$]mode = '([^']*)'"; job_pattern="[\$]job = '([^']*)'"
      [[ "$body" =~ $mode_pattern ]]; mode="${BASH_REMATCH[1]}"
      [[ "$body" =~ $job_pattern ]]; job="${BASH_REMATCH[1]}"
      echo "fake: launched as $mode"
      map "$job"
      body=$(< "$f")
    fi
    marker=""
    if [[ "$body" = *vm-run-wrap* ]]; then   # the step's wrapper: play the step, then its exit line
      marker_pattern="[\$]marker = '([^']*)'"; step_pattern="[\$]step = '([^']*)'"
      [[ "$body" =~ $marker_pattern ]]; marker="${BASH_REMATCH[1]}"
      [[ "$body" =~ $step_pattern ]]; map "${BASH_REMATCH[1]}"
      [ -f "$f" ] || { echo "fake: no step $f"; exit 1; }
      body=$(< "$f")
    fi
    # prlctl's own failure to start the job: its line alone, nothing run
    if grep -q '^#fake: prljob-once' "$f" && [ ! -e "$F/prljob-done" ]; then
      touch "$F/prljob-done"; echo 'PrlJob_GetResult/GetRetCode: Invalid argument'; exit 255
    fi
    if [[ "$body" = *prepared:* ]]; then [ -e "$F/prepare-fails" ] && exit 1; echo "prepared: hidden.exe True arch $(< "$F/arch")"; fi
    run_fields="[\$]VmRun = @[{] Url = '([^']*)'; Run = '([^']*)'; Share = '[^']*'; Results = '[^']*'; Step = '([^']*)';"
    [[ "$body" =~ $run_fields ]]
    url="${BASH_REMATCH[1]}"; run="${BASH_REMATCH[2]}"
    echo "fake: step ${BASH_REMATCH[3]}"
    rc=0
    while read -r tag what arg; do
      [ "$tag" = '#fake:' ] || continue
      case "$what" in
        out) echo "$arg" ;;
        rc) rc="$arg" ;;
        sleep) fake_sleep "$arg" ;;
        hold) touch "$F/$arg.ready"; until [ -e "$F/$arg.release" ]; do sleep 0.05; done ;;
        prljob-after-work) echo 'half the work done'; echo 'PrlJob_GetResult/GetRetCode: Invalid argument'; exit 255 ;;
        http) curl -fsS --max-time 5 "${url}payload.json" || rc=7 ;;
        https) curl -fsSk --max-time 5 "https://${url#http://}payload.json" || rc=7 ;;
        kill-http) kill "$(cat "$(ls -td "$SHARE"/results/*/ | head -1)http.pid")" ;;
        touch-payload) echo ' ' >> "$SHARE/dist-test/payload.json" ;;
        rc-if) [ -e "$F/${arg%% *}" ] && rc="${arg#* }" ;;
        write) echo x >> "$arg" ;;
        swap) cp "$arg" "$arg.orig"; printf '%s\n' '#fake: out SWAPPED' > "$arg" ;;
        unswap) mv "$arg.orig" "$arg" ;;
        sleep-if) [ -e "$F/${arg%% *}" ] && fake_sleep "${arg#* }" ;;
        run-stage) "$FAKE_VR" stage > "$F/stage-during.out" 2>&1; echo "stage during the run: $?" ;;
        bg-sleep) sleep "$arg" & echo "fake: left a sleep $!" ;;
        get) curl -fsS --max-time 5 "${url}$arg" || echo "GET-REFUSED $arg" ;;
        sha) echo "sha256 $(curl -fsS --max-time 5 "${url}$arg" | shasum -a 256 | cut -c1-64)" ;;
        jobfile) if [ -e "$SHARE/job/$run/$arg" ]; then echo "JOBFILE $arg: $(cat "$SHARE/job/$run/$arg")"; else echo "NO JOBFILE $arg"; fi ;;
        range) echo "range ${arg%% *}: $(curl -sS --max-time 5 -o "$F/range.body" -w '%{http_code} %header{content-range}' -H "Range: bytes=${arg%% *}" "${url}${arg#* }") sha256 $(shasum -a 256 < "$F/range.body" | cut -c1-64)" ;;
        location) echo "location $(curl -sS --max-time 5 -o /dev/null -w '%{http_code} %{redirect_url}' "${url}$arg")" ;;
        tunnel) echo "tunnel sha256 $(curl -fsSp --noproxy '' --max-time 5 -x "$url" "${url}$arg" | shasum -a 256 | cut -c1-64)" ;;
        tunnel-to) echo "tunnel-to $arg: $(curl -sSp --noproxy '' --max-time 5 -x "$url" -o /dev/null -w '%{http_connect}' "http://$arg/" 2> /dev/null)" ;;
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
# the fake prlsrvctl: usb set ID VM and usb del ID, logged in the same calls file as "srv ..."
cat > "$T/bin/prlsrvctl" <<'EOF'
#!/bin/bash
echo "srv $*" >> "$FAKE_DIR/calls"
[ -e "$FAKE_DIR/usb-fails" ] && exit 1
case "$1 $2" in
  'usb set') [ $# = 4 ] ;;
  'usb del') [ $# = 3 ] ;;
  *) echo "fake prlsrvctl: $*?"; exit 1 ;;
esac
EOF
chmod +x "$T/bin/prlsrvctl"

# a checkout to name in records: what stage archives, and the suites --record runs (committed)
R="$T/repo"; RS="$R/tests/win/suites"
mkdir -p "$R/scripts" "$R/tests/win/bootstrap" "$R/core" "$R/win/c" "$RS/s" "$R/tests/win/first-run"
# The runner is committed in the checkout and run from it (VT2). Only copy the suites
# needed by first-run.sh when those cases start. The real suites are also run below
# from the source tree, and the record fixtures have their own release.list.
cp "$here/tests/win/vm-run.sh" "$here/tests/win/first-run.sh" "$R/tests/win/"
cp -R "$here/tests/win/first-run/." "$R/tests/win/first-run/"
# The sessions suites also declare two inputs from the staged repo.
(cd "$here/tests/win/suites" && tar -cf - stage-dist.sh lib/vmlib.ps1 sessions/as-b.ps1) | tar -xf - -C "$RS" || exit 2
# Keep the fake sleeper longer than the scratch suite's deadline.
sed -i '' -E 's/^sys[[:space:]]+10[[:space:]]+/sys 1 /' "$R/tests/win/first-run/timeout.suite"
sed -i '' 's/^#fake: sleep 30$/#fake: sleep 4/' "$R/tests/win/first-run/timeout/sleeper.ps1"
echo bootstrap > "$R/scripts/bootstrap.ps1"; echo t > "$R/tests/win/bootstrap/bootstrap.tests.ps1"
echo texts > "$R/core/texts.def"; echo aumid > "$R/win/c/aumid.h"; echo afterexit > "$R/win/c/afterexit.h"; echo other > "$R/core/other.c"
printf 'build/\ndist-test/\n' > "$R/.gitignore"
printf '%s\n' '#fake: out hello' > "$RS/s/pass.ps1"
printf '%s\n' '#fake: rc-if beta-fails 3' '#fake: sleep-if beta-slow 4' > "$RS/s/maybe.ps1"
printf '%s\n' 'need http' 'need candidate dist-test/new/payload.json' 'need fixture dist-test/a/payload.json' 'sys 60 one s/pass.ps1' > "$RS/upd.suite"
printf '%s\n' "#fake: write $T/share/dist-test/new/payload.json.sig" > "$RS/s/tamper-new.ps1"
printf '%s\n' 'need http' 'need candidate dist-test/new/payload.json' 'sys 60 tamper s/tamper-new.ps1' > "$RS/upd-tamper.suite"
printf '%s\n' 'need http' 'need vm' 'sys 60 one s/pass.ps1' > "$RS/vmneed.suite"
printf '%s\n' '#fake: run-stage' > "$RS/s/runstage.ps1"
printf '%s\n' 'need http' 'sys 60 stage s/runstage.ps1' > "$RS/inter.suite"
printf '%s\n' "#fake: write $T/share/repo/scripts/bootstrap.ps1" > "$RS/s/tamper.ps1"
printf '%s\n' 'need http' 'sys 60 tamper s/tamper.ps1' > "$RS/tamper.suite"
printf '%s\n' '#fake: out skipped here' '#fake: rc 77' > "$RS/s/skip.ps1"
printf '%s\n' '#fake: out from the include' > "$RS/inc.ps1"
printf '%s\n' '#fake: out high' > "$RS/s/high.ps1"
printf '%s\n' 'need http' 'include inc.ps1' 'sys 60 one s/pass.ps1' 'user-signed-high 60 two s/high.ps1' > "$RS/alpha.suite"
printf '%s\n' 'need http' 'sys 60 one s/pass.ps1' 'sys 2 maybe s/maybe.ps1' > "$RS/beta.suite"
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
  'for a in x64 arm64; do mkdir -p "$1/win-$a"; echo "w $a" > "$1/win-$a/winc-wintest.exe"; echo "app $a" > "$1/win-$a/winc-app-wintest.exe"; echo "upd $a" > "$1/win-$a/winc-update-wintest.exe"; echo "nw6 $a" > "$1/win-$a/memo-stamp-wintest.exe"; done; echo fixtures > "$1/fixtures.txt"' > "$R/win/c/tests/run.sh"
printf '%s\n' '#!/bin/sh' '[ "$1 $3" = "dll --test-hooks" ] || exit 2' 'for a in x64 arm64; do mkdir -p "$2/testhooks/$a"; echo "core $a" > "$2/testhooks/$a/rbsp-core.dll"; done' > "$R/core/build.sh"
mkdir -p "$R/tests/win/uia"
printf '%s\n' '#!/bin/sh' '[ "${FAKE_BUILD_FAIL:-}" = uia ] && exit 1' \
  'for a in x64 arm64; do mkdir -p "$1/$a"; echo "sysdesk $a" > "$1/$a/sysdesk.exe"; done' \
  'mkdir -p "$1/rbui"; for f in rbui.exe rbui.exe.config FlaUI.Core.dll FlaUI.UIA3.dll Interop.UIAutomationClient.dll LICENSE-flaui-core.txt LICENSE-flaui-uia3.txt LICENSE-interop-uiautomationclient.txt; do echo "$f" > "$1/rbui/$f"; done' > "$R/tests/win/uia/build.sh"
chmod +x "$R/common/tests/win.sh" "$R/win/c/tests/run.sh" "$R/core/build.sh" "$R/tests/win/uia/build.sh"
mkdir -p "$R/win/shell/vm" "$R/tests/harness/traces/A"
cp "$here"/win/shell/vm/{w13b,w13b-checks,w13b-fixture,prepare-fixture}.ps1 "$R/win/shell/vm/"
cp "$here/tests/harness/traces/A/prepare-choose-estimate.jsonl" "$R/tests/harness/traces/A/"
printf '%s\n' '#!/bin/sh' '[ "$1 $2 $3" = "--x64 --no-test --test-app" ] || exit 2' \
  'for app in app-x64 app-test app-x64-test; do mkdir -p "$4/$app"; echo forced > "$4/$app/RB Stems Plus.exe"; done' > "$R/win/shell/build.sh"
chmod +x "$R/win/shell/build.sh"
printf '%s\n' 'need http' 'sys 60 one s/pass.ps1' 'wait 30 back logon' 'sys 60 two s/pass.ps1' > "$RS/waiter.suite"
git -C "$R" init -q && git -C "$R" add -A && git -C "$R" -c user.email=t@t -c user.name=t commit -q -m one
C="$(git -C "$R" rev-parse HEAD)"
VR="$R/tests/win/vm-run.sh"   # the checkout's own (VT2)
mkdir -p "$R/dist-test"
printf '{"payload_version": "1.5.0", "commit": "%s"}\n' "$C" > "$T/share/dist-test/payload.json"
cp "$T/share/dist-test/payload.json" "$R/dist-test/payload.json"
# update.suite's shape: new/ this commit's release (a candidate), a/ an older one (a fixture)
mkrels() {
  mkdir -p "$T/share/dist-test/new" "$T/share/dist-test/a"
  cp "$T/share/dist-test/payload.json" "$T/share/dist-test/new/payload.json"
  printf '{"payload_version": "1.4.0", "commit": "%s"}\n' 1111111111111111111111111111111111111111 > "$T/share/dist-test/a/payload.json"
}
mkrels
echo ARM64 > "$F/arch"

mkdir -p "$T/volumes"
export FAKE_DIR="$F" FAKE_SHARE="$T/share" FAKE_VR="$VR" FAKE_VOLUMES="$T/volumes" VMRUN_VOLUMES="$T/volumes" VMRUN_PRLCTL="$T/bin/prlctl" VMRUN_PRLSRVCTL="$T/bin/prlsrvctl" VMRUN_CONF="$T/vm-run.conf" VMRUN_SHARE="$T/share" VMRUN_GATES="$T/gates" \
  VMRUN_ROOT="$R" VMRUN_HTTP_BIND=127.0.0.1 VMRUN_HTTP_PORT=$((20000 + $$ % 20000)) VMRUN_TEST_FAST_HTTP=1 VMRUN_POLL=0 VMRUN_BOOT=20
vr() { "$VR" "$@" > "$T/out" 2>&1; }
calls() { cat "$F/calls" 2> /dev/null; }
reset() {
  : > "$F/calls"; echo running > "$F/state"; echo rb-win-template > "$F/name"; rm -f "$F/switch-fails" "$F/prljob-done" "$F/no-marker" "$F/rc-zero"
  printf 'win|+|%s\n' "$T/share" > "$F/shares"; echo on > "$F/shf"; echo Unknown > "$F/defined"; : > "$F/snapshares"; echo Off > "$F/snapdefined"
  rm -f "$F/switch-running" "$F/share-unseen" "$F/shf-pending" "$F/shf-lags"
}
last_results() { ls -td "$T/share/results"/*/ 2> /dev/null | head -1; }

printf '%s\n' '#fake: out hello' > "$T/suites/s/pass.ps1"
printf '%s\n' '#fake: out failing' '#fake: rc 3' > "$T/suites/s/fail.ps1"
printf '%s\n' '#fake: sleep 30' > "$T/suites/s/slow.ps1"
printf '%s\n' '#fake: prljob-once' '#fake: out after a retry' > "$T/suites/s/flaky.ps1"
printf '%s\n' '#fake: http' > "$T/suites/s/http.ps1"
printf '%s\n' 'need http' 'sys 60 one s/pass.ps1' 'user 60 two s/pass.ps1' 'user-high 60 three s/pass.ps1' \
              'user-signed 60 four s/pass.ps1' 'sys 60 flaky s/flaky.ps1' 'sys 60 http s/http.ps1' > "$T/suites/good.suite"
printf '%s\n' 'sys 60 one s/pass.ps1' 'sys 60 bad s/fail.ps1' 'sys 1 slow s/slow.ps1' 'sys 60 last s/pass.ps1' > "$T/suites/mixed.suite"

# ---- the VM guard
reset; echo 'Windows 11' > "$F/name"
vr run 'clean Windows' "$T/suites/good.suite"; rc=$?
ok '[ $rc = 2 ] && grep -q "owner" "$T/out" && ! calls | grep -qE "^(snapshot-switch|exec|start|set)"' "the owner's VM refused, never driven"
reset; echo 'other' > "$F/name"
vr run 'clean Windows' "$T/suites/good.suite"; rc=$?
ok '[ $rc = 2 ] && ! calls | grep -qE "^(snapshot-switch|exec)"' 'a VM of another name refused'
# VMRUN_VM: another test VM (a clone such as rb-win-hand) by its name, checked to bear it; never
# the owner's
reset; echo rb-win-hand > "$F/name"
VMRUN_VM=rb-win-hand vr run 'clean Windows' "$T/suites/good.suite"; rc=$?
ok '[ $rc = 0 ] && calls | grep -q "^snapshot-switch rb-win-hand -i " && calls | grep -q "^exec rb-win-hand " && ! calls | grep -q 202a80c8' "VMRUN_VM drives the VM of that name (rc $rc)"
reset
VMRUN_VM=rb-win-hand vr run 'clean Windows' "$T/suites/good.suite"; rc=$?
ok '[ $rc = 2 ] && grep -q "not rb-win-hand" "$T/out" && ! calls | grep -qE "^(snapshot-switch|exec|start|set)"' "VMRUN_VM: a VM by that name that says another name refused"
reset
VMRUN_VM='Windows 11' vr run 'clean Windows' "$T/suites/good.suite"; rc=$?
ok '[ $rc = 2 ] && grep -q "owner" "$T/out" && [ ! -s "$F/calls" ]' "VMRUN_VM=\"Windows 11\" refused before prlctl is called"
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

# ---- HTTPS beside HTTP on the one port (the Windows TEST app's downloads), the CA for the VM
mkdir -p "$T/tls"
openssl req -x509 -newkey rsa:2048 -nodes -subj /CN=127.0.0.1 -days 2 -keyout "$T/tls/server-key.pem" -out "$T/tls/server.pem" > /dev/null 2>&1
cp "$T/tls/server.pem" "$T/share/dist-test/test-ca.pem"
printf '%s\n' '#fake: https' > "$T/suites/s/https.ps1"
printf '%s\n' '#fake: get latest/download/payload.json' '#fake: get download/v1.3.0/payload.json' '#fake: get latest/download/sub/payload.json' \
  '#fake: get cur/latest/download/payload.json' '#fake: get cur/download/v1.3.0/payload.json' '#fake: get cur/latest/download/sub/payload.json' > "$T/suites/s/rel.ps1"
# a base with a folder (update.suite's current/): its files, as the root's
mkdir -p "$T/share/dist-test/cur"; printf '{"payload_version": "9.9.9"}\n' > "$T/share/dist-test/cur/payload.json"
printf '%s\n' 'need http' 'sys 60 http s/http.ps1' 'sys 60 https s/https.ps1' 'sys 60 rel s/rel.ps1' > "$T/suites/tls.suite"
reset
VMRUN_TLS="$T/tls" vr run 'clean Windows' "$T/suites/tls.suite"; rc=$?
res="$(last_results)"; job="$T/share/job/$(basename "$res")"
ok '[ $rc = 0 ] && grep -q payload_version "$res/1-http.log" && grep -q payload_version "$res/2-https.log" && grep -q "HTTPS too" "$res/http.log"' "with the TLS certificate the server answers HTTP and HTTPS on one port (rc $rc)"
ok 'cmp -s "$job/test-ca.cer" "$T/share/dist-test/test-ca.pem" && grep -q "Import-Certificate" "$job/0-prepare.ps1"' "the served test-ca.pem goes to the VM's prepare step to be trusted"
ok '[ "$(grep -c payload_version "$res/3-rel.log")" = 4 ] && [ "$(grep -c "^GET-REFUSED" "$res/3-rel.log")" = 2 ] && grep -q "^GET-REFUSED latest/download/sub/payload.json" "$res/3-rel.log"' "a release file's GitHub paths (latest/download/NAME, download/vTAG/NAME) serve the folder's NAME, nothing deeper"
ok '[ "$(grep -Fxc "{\"payload_version\": \"9.9.9\"}" "$res/3-rel.log")" = 2 ] && grep -q "^GET-REFUSED cur/latest/download/sub/payload.json" "$res/3-rel.log"' "under a base with a folder (DIR/latest/download/NAME, DIR/download/vTAG/NAME): DIR's NAME, nothing deeper"
reset
VMRUN_TLS="$T/no-tls" vr run 'clean Windows' "$T/suites/tls.suite"; rc=$?
res="$(last_results)"
ok '[ $rc = 1 ] && grep -q payload_version "$res/1-http.log" && ! grep -q "HTTPS too" "$res/http.log"' "without a certificate: HTTP only (rc $rc)"
rm -rf "$T/share/dist-test/test-ca.pem" "$T/share/dist-test/cur"

# ---- the server's /w3/ (platform.suite's winhttp): a made-up file with Range, redirects, a PAC
# file, a CONNECT tunnel to the server alone
blob() { python3 -I -c 'import hashlib, sys; a, n = int(sys.argv[1]), int(sys.argv[2]); print(hashlib.sha256(bytes(i % 251 for i in range(a, n))).hexdigest())' "$@"; }
printf '%s\n' '#fake: sha w3/blob/100000' '#fake: range 40000- w3/blob/100000' '#fake: range 99999- w3/blob/100000' '#fake: range 100000- w3/blob/100000' \
  '#fake: range 10-19 w3/blob/100000' '#fake: location w3/to-https/blob/100000' '#fake: location w3/to-http/blob/100000' \
  '#fake: get w3/proxy.pac' '#fake: tunnel w3/blob/100000' '#fake: tunnel-to 192.0.2.1:80' '#fake: get w3/blob/16777217' '#fake: get w3/nothing' > "$T/suites/s/w3.ps1"
printf '%s\n' 'need http' 'sys 60 w3 s/w3.ps1' > "$T/suites/w3.suite"
reset
vr run 'clean Windows' "$T/suites/w3.suite"; rc=$?
res="$(last_results)"; hp="${VMRUN_HTTP_BIND}:${VMRUN_HTTP_PORT}"
ok '[ $rc = 0 ] && grep -q "^sha256 $(blob 0 100000)$" "$res/1-w3.log"' "/w3/blob/N: N bytes, byte i being i % 251 (rc $rc)"
ok 'grep -q "^range 40000-: 206 bytes 40000-99999/100000 sha256 $(blob 40000 100000)$" "$res/1-w3.log" && grep -q "^range 99999-: 206 bytes 99999-99999/100000 " "$res/1-w3.log" && grep -q "^range 10-19: 206 bytes 10-19/100000 sha256 $(blob 10 20)$" "$res/1-w3.log"' "/w3/blob: Range A- and A-B answered 206 with their Content-Range and bytes"
ok 'grep -q "^range 100000-: 416 bytes \*/100000 " "$res/1-w3.log"' "/w3/blob: a Range past the end is 416"
ok 'grep -q "^location 302 https://$hp/w3/blob/100000$" "$res/1-w3.log" && grep -q "^location 302 http://$hp/w3/blob/100000$" "$res/1-w3.log"' "/w3/to-https/ and /w3/to-http/: a 302 to /w3/ on this server over that scheme"
ok 'grep -qF "if (host == \"$VMRUN_HTTP_BIND\") return \"PROXY $hp\"; return \"DIRECT\";" "$res/1-w3.log"' "/w3/proxy.pac sends this server through the proxy at this server"
ok 'grep -q "^tunnel sha256 $(blob 0 100000)$" "$res/1-w3.log" && grep -q "\"CONNECT $hp HTTP" "$res/http.log"' "CONNECT to this server: tunnelled (in http.log)"
ok 'grep -q "^tunnel-to 192.0.2.1:80: 403$" "$res/1-w3.log" && ! grep -q "\"CONNECT 192.0.2.1:80 HTTP[^\"]*\" 200" "$res/http.log"' "CONNECT anywhere else: 403, never tunnelled"
ok 'grep -q "^GET-REFUSED w3/blob/16777217$" "$res/1-w3.log" && grep -q "^GET-REFUSED w3/nothing$" "$res/1-w3.log"' "/w3/: more than 16 MiB, or another path: 404"

# ---- snapshot NAME (sac.suite's "SAC On")
reset; cp "$F/snaps" "$F/snaps.saved"
vr snapshot 'SAC On'; rc=$?
ok '[ $rc = 0 ] && calls | grep -qx "snapshot 202a80c8-4bc4-4e95-890e-a31d6bfd5b70 -n SAC On" && grep -q " SAC On$" "$F/snaps" && grep -q "snapshot {33333333-aaaa-bbbb-cccc-[0-9]*} \"SAC On\" taken" "$T/out"' "snapshot NAME takes one of the VM by that name (rc $rc)"
reset
vr snapshot 'SAC On'; rc=$?
ok '[ $rc = 2 ] && grep -q "has a snapshot named \"SAC On\" already" "$T/out" && ! calls | grep -q "^snapshot "' "snapshot NAME refused when one of that name is there"
reset; echo 'Windows 11' > "$F/name"
vr snapshot 'x'; rc=$?
ok '[ $rc = 2 ] && grep -q owner "$T/out" && ! calls | grep -q "^snapshot "' "snapshot: the owner's VM refused"
reset
vr snapshot; rc=$?
ok '[ $rc = 2 ] && ! calls | grep -q "^snapshot "' "snapshot without a name refused"
reset; touch "$F/snapshot-fails"
vr snapshot 'Other'; rc=$?
ok '[ $rc = 2 ] && grep -q "prlctl snapshot .* failed" "$T/out"' "snapshot: prlctl's failure said, exit 2"
rm -f "$F/snapshot-fails"; mv "$F/snaps.saved" "$F/snaps"

# ---- usb attach|detach, need usb, the host facts (vm-run.conf)
UID_T=202a80c8-4bc4-4e95-890e-a31d6bfd5b70
reset; rm -f "$T/vm-run.conf"
vr usb attach; rc=$?
ok '[ $rc = 77 ] && grep -q "no USB stick is assigned to rb-win-template" "$T/out" && ! calls | grep -q "^srv"' "usb attach without vm-run.conf: 77, nothing assigned"
printf '%s\n' '# the sticks' 'usb rb-win-hand 1111:2222|hand' 'usb rb-win-template 0781:5581|abc # in the left port' > "$T/vm-run.conf"
reset
vr usb attach; rc=$?
ok '[ $rc = 0 ] && calls | grep -qx "srv usb set 0781:5581|abc $UID_T" && ! calls | grep -q "^srv usb del"' "usb attach: vm-run.conf's id for this VM assigned to it, and left so (rc $rc)"
reset
vr usb detach; rc=$?
ok '[ $rc = 0 ] && calls | grep -qx "srv usb del 0781:5581|abc" && ! calls | grep -q "^srv usb set"' "usb detach: that id taken back"
reset; touch "$F/usb-fails"
vr usb detach; rc=$?
ok '[ $rc = 1 ] && grep -q "couldn.t be taken back" "$T/out"' "usb detach: prlsrvctl's failure said, exit 1"
rm -f "$F/usb-fails"
reset; echo 'Windows 11' > "$F/name"
VMRUN_VM='Windows 11' vr usb attach; rc=$?
ok '[ $rc = 2 ] && ! calls | grep -q "^srv"' "usb: the owner's VM refused"
reset
vr usb plug; rc=$?
ok '[ $rc = 2 ] && ! calls | grep -q "^srv"' "usb: only attach or detach"
printf '%s\n' 'usb rb-win-template -rf' > "$T/vm-run.conf"
reset
vr usb attach; rc=$?
ok '[ $rc = 2 ] && grep -q "starts with -" "$T/out" && ! calls | grep -q "^srv"' "a device id that starts with - refused"
# a run with need usb: the stick assigned before the VM starts, taken back after the end; the
# steps told (job/usb.assigned, job/github.allowed)
printf '%s\n' '#fake: jobfile usb.assigned' '#fake: jobfile github.allowed' > "$T/suites/s/facts.ps1"
printf '%s\n' 'need usb' 'sys 60 facts s/facts.ps1' > "$T/suites/usb.suite"
printf '%s\n' 'usb rb-win-template 0781:5581|abc' 'github allowed' > "$T/vm-run.conf"
reset
vr run 'clean Windows' "$T/suites/usb.suite"; rc=$?
res="$(last_results)"
set_at=$(calls | grep -n '^srv usb set' | head -1 | cut -d: -f1); start=$(calls | grep -n '^start' | head -1 | cut -d: -f1)
del_at=$(calls | grep -n '^srv usb del' | tail -1 | cut -d: -f1); last_switch=$(calls | grep -n '^snapshot-switch' | tail -1 | cut -d: -f1)
ok '[ $rc = 0 ] && [ -n "$set_at" ] && [ -n "$start" ] && [ "$set_at" -lt "$start" ] && calls | grep -qx "srv usb set 0781:5581|abc $UID_T"' "need usb: the stick assigned before the VM starts (rc $rc)"
ok '[ -n "$del_at" ] && [ "$del_at" -gt "$last_switch" ] && calls | grep -qx "srv usb del 0781:5581|abc"' "need usb: taken back after the VM's stop and revert"
ok 'grep -q "^JOBFILE usb.assigned: 0781:5581|abc$" "$res/1-facts.log" && grep -q "^JOBFILE github.allowed: allowed$" "$res/1-facts.log"' "the steps see the host facts in the job folder"
reset; touch "$F/prepare-fails"
vr run 'clean Windows' "$T/suites/usb.suite"; rc=$?
ok '[ $rc = 2 ] && calls | grep -qx "srv usb del 0781:5581|abc"' "a run that stops early takes the stick back too"
rm -f "$F/prepare-fails"
# a snapshot taken running comes back from memory: with need usb, stopped and started cold, the
# stick assigned before that start (W3R2)
reset; touch "$F/switch-running"
vr run 'clean Windows' "$T/suites/usb.suite"; rc=$?
first_switch=$(calls | grep -n '^snapshot-switch' | head -1 | cut -d: -f1); stop_at=$(calls | grep -n '^stop' | head -1 | cut -d: -f1)
set_at=$(calls | grep -n '^srv usb set' | head -1 | cut -d: -f1); start=$(calls | grep -n '^start' | head -1 | cut -d: -f1)
ok '[ $rc = 0 ] && [ -n "$stop_at" ] && [ -n "$set_at" ] && [ -n "$start" ] && [ "$first_switch" -lt "$stop_at" ] && [ "$stop_at" -lt "$set_at" ] && [ "$set_at" -lt "$start" ]' "need usb from a snapshot taken running: stopped, the stick assigned, then a cold start (rc $rc)"
rm -f "$F/switch-running"
printf '%s\n' 'usb rb-win-hand 1111:2222|hand' > "$T/vm-run.conf"
reset
vr run 'clean Windows' "$T/suites/usb.suite"; rc=$?
res="$(last_results)"
ok '[ $rc = 0 ] && ! calls | grep -q "^srv" && grep -q "^NO JOBFILE usb.assigned$" "$res/1-facts.log" && grep -q "^NO JOBFILE github.allowed$" "$res/1-facts.log" && grep -q "no USB stick is assigned to rb-win-template" "$T/out"' "need usb with no stick for this VM: nothing assigned, the steps told none (rc $rc)"
printf '%s\n' 'sys 60 facts s/facts.ps1' > "$T/suites/nousb.suite"
printf '%s\n' 'usb rb-win-template 0781:5581|abc' > "$T/vm-run.conf"
reset
vr run 'clean Windows' "$T/suites/nousb.suite"; rc=$?
ok '[ $rc = 0 ] && ! calls | grep -q "^srv"' "a suite without need usb assigns no stick"
printf '%s\n' 'need usb extra' 'sys 60 facts s/facts.ps1' > "$T/suites/bad.suite"
reset
vr run 'clean Windows' "$T/suites/bad.suite"; rc=$?
ok '[ $rc = 2 ] && ! calls | grep -qE "^(snapshot-switch|srv)"' '"need usb extra" refused'
rm -f "$T/vm-run.conf"

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
ok '! grep -q "appeared during the run" "$T/out"' 'no new [C] hidden folder: no warning'

ok '[ ! -e "$T/gates/$C" ]' 'no record without --record'
add=$(calls | grep -n -- "--shf-host-add win --path $T/share --mode rw" | head -1 | cut -d: -f1)
seen=$(calls | grep -n "cmd /c dir /b .*0-prepare.ps1" | head -1 | cut -d: -f1)
prep=$(calls | grep -n -- "-File .*0-prepare.ps1" | head -1 | cut -d: -f1)
ok '[ -n "$add" ] && [ "$first_switch" -lt "$add" ] && [ "$add" -lt "$start" ] && [ -n "$seen" ] && [ "$seen" -lt "$prep" ]' 'a snapshot without the share: win added before the start, seen by the guest before prepare'
ok 'calls | grep -q -- "--shf-host on" && ! calls | grep -q -- "--shf-host-defined"' 'host sharing switched on, never --shf-host-defined (it would switch sharing off)'

# ---- guest sharing off after every revert; a new "[C] <vm>*.hidden" folder warned of, not a failure
reset; rm -rf "$T/volumes"; mkdir -p "$T/volumes/[C] rb-win-template old.hidden" "$T/volumes/[C] rb-win-template 1.hidden extra.hidden"
touch "$F/leaves-hidden"
vr run 'clean Windows' "$T/suites/good.suite"; rc=$?
rm -f "$F/leaves-hidden"
guest_ok=1; prev=""
while IFS= read -r line; do
  case "$prev" in snapshot-switch*) [ "$line" = "set 202a80c8-4bc4-4e95-890e-a31d6bfd5b70 --shf-guest off" ] || guest_ok=0 ;; esac
  prev="$line"
done < "$F/calls"
ok '[ $rc = 0 ] && [ "$guest_ok" = 1 ] && [ "$(calls | grep -c -- "--shf-guest off")" = 2 ] && [ "$(cat "$F/shfguest")" = off ]' "--shf-guest off right after each of the 2 reverts (rc $rc)"
ok 'grep -q "warning: $T/volumes/\[C\] rb-win-template 1.hidden appeared" "$T/out" && grep -q "warning: $T/volumes/\[C\] rb-win-template 2.hidden appeared" "$T/out" && [ "$(grep -c "appeared during the run" "$T/out")" = 2 ]' "the run's new [C] hidden folders warned of, the pre-existing ones (one a prefix match) and the run's own other-VM and bracketless names not ($(grep warning "$T/out" | tr '\n' ' '))"
rm -rf "$T/volumes"; mkdir -p "$T/volumes"
# a stopped VM whose host sharing still reads off after --shf-host on: the run goes on, the guest
# seeing the share is what counts
reset; touch "$F/shf-lags"
vr run 'clean Windows' "$T/suites/good.suite"; rc=$?
ok '[ $rc = 0 ] && calls | grep -q -- "--shf-host on" && ! grep -q "still off" "$T/out"' "host sharing that reads off on the stopped VM after --shf-host on: the run goes on (rc $rc)"
reset; touch "$F/shf-lags"; echo 30 > "$F/share-unseen"
VMRUN_SHAREWAIT=1 VMRUN_POLL=0.05 vr run 'clean Windows' "$T/suites/good.suite"; rc=$?
ok '[ $rc = 2 ] && ! calls | grep -q "^exec .*-File"' "... but a guest that never sees the share stops the run before any step (rc $rc)"
rm -f "$F/shf-lags" "$F/share-unseen"

# ---- the share after a revert: win alone, the guest sees it, nothing wider
reset; touch "$F/switch-running"; printf '%s\n' 'home|+|/Users' "win|-|$T/share" 'old|+|/tmp' > "$F/snapshares"
vr run 'clean Windows' "$T/suites/good.suite"; rc=$?
ok '[ $rc = 0 ] && calls | grep -q -- "--shf-host-del home" && calls | grep -q -- "--shf-host-del old" && calls | grep -q -- "--shf-host-set win --enable" && ! calls | grep -q -- "--shf-host-add"' "other shares removed, a disabled win enabled ($(tail -2 "$T/out" | tr '\n' ' '))"
last_stop=$(calls | grep -n '^stop' | tail -1 | cut -d: -f1); last_switch=$(calls | grep -n '^snapshot-switch' | tail -1 | cut -d: -f1)
ok '[ "$(cat "$F/state")" = stopped ] && [ -n "$last_stop" ] && [ "$last_stop" -gt "$last_switch" ] && ! calls | grep -q "^start"' 'a snapshot that comes back running: not started, and stopped again after the final revert'
reset; echo Home > "$F/snapdefined"
vr run 'clean Windows' "$T/suites/good.suite"; rc=$?
ok '[ $rc = 2 ] && grep -q "shares more than build/win" "$T/out" && ! calls | grep -q "^exec" && [ "$(cat "$F/state")" = stopped ] && [ "$(calls | grep -c "^snapshot-switch")" = 2 ]' 'a snapshot sharing the home folder: refused, nothing run, stopped and reverted'
reset; touch "$F/switch-running"; echo 30 > "$F/share-unseen"
VMRUN_SHAREWAIT=1 VMRUN_POLL=0.05 vr run 'clean Windows' "$T/suites/good.suite"; rc=$?
ok '[ $rc = 2 ] && grep -q "doesn.t see .*0-prepare.ps1 within 1 s" "$T/out" && ! calls | grep -q -- "-File" && [ "$(cat "$F/state")" = stopped ] && grep -q "the VM is stopped and reverted" "$T/out"' "a share the guest never sees: bounded, exit 2, no prepare, stopped and reverted ($(tail -2 "$T/out" | tr '\n' ' '))"

# ---- the HTTP server: only ours, alive, serving the share's payload.json (VR2)
mkdir -p "$T/foreign"; echo '{"payload_version": "0.0.1-OLD"}' > "$T/foreign/payload.json"
python3 -I -m http.server --bind 127.0.0.1 --directory "$T/foreign" "$VMRUN_HTTP_PORT" > /dev/null 2>&1 & foreign=$!
wait_for curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$VMRUN_HTTP_PORT/"
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
ok '[ "$took" -lt 25 ]' "the 1 s step stopped at its timeout (the run took ${took}s)"
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
reset; echo 1 > "$F/wait-left"
vr run 'clean Windows' "$RS/waiter.suite" --record; rc=$?
wrec="$G/waiter@$K1-arm64.ok"
ok '[ $rc = 0 ] && [ -f "$wrec" ] && grep -q "^step wait back PASS 0 " "$wrec" && ! grep -q "^outside " "$wrec" && [ "$(grep -c "^file " "$wrec")" = 3 ]' "--record of a suite with a wait step: its result recorded, no script named for it ($(grep "^step\|^file\|^outside" "$wrec" 2> /dev/null | tr '\n' ';'))"
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
ok '[ $rc = 2 ] && grep -q "VMRUN_PRLCTL, VMRUN_UUID or VMRUN_VM is set" "$T/out" && ! calls | grep -q "^snapshot-switch" && [ ! -e "$R/build/gates" ]' "--record refused: VMRUN_PRLCTL without VMRUN_GATES (VX8: rc $rc)"
rm -rf "$R/build/gates"; reset
env -u VMRUN_GATES -u VMRUN_PRLCTL PATH="$T/bin:$PATH" VMRUN_UUID=33333333-aaaa-bbbb-cccc-000000000003 "$VR" run 'clean Windows' "$RS/gamma.suite" --record > "$T/out" 2>&1; rc=$?
ok '[ $rc = 2 ] && grep -q "VMRUN_PRLCTL, VMRUN_UUID or VMRUN_VM is set" "$T/out" && ! calls | grep -q "^snapshot-switch" && [ ! -e "$R/build/gates" ]' "--record refused: VMRUN_UUID without VMRUN_GATES (VX8: rc $rc)"
rm -rf "$R/build/gates"
reset
env -u VMRUN_GATES -u VMRUN_PRLCTL PATH="$T/bin:$PATH" VMRUN_VM=rb-win-hand "$VR" run 'clean Windows' "$RS/gamma.suite" --record > "$T/out" 2>&1; rc=$?
ok '[ $rc = 2 ] && grep -q "VMRUN_PRLCTL, VMRUN_UUID or VMRUN_VM is set" "$T/out" && ! calls | grep -q "^snapshot-switch" && [ ! -e "$R/build/gates" ]' "--record refused: VMRUN_VM without VMRUN_GATES (rc $rc)"
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
vr run 'rekordbox 7.2.19 installed' "$RS/beta.suite" --record; rc=$?
ok '[ $rc = 1 ] && [ -f "$G/beta@$K2-arm64.failed" ] && [ -f "$rec" ]' "a failing --record run: its own .failed, the other suites' records kept"
reset
vr run 'clean Windows' "$RS/alpha.suite" --record; rc=$?
vr check "$C" "$DT"; rc=$?
ok '[ $rc = 1 ] && grep -q "beta on .* failed" "$T/out" && [ -f "$G/beta@$K2-arm64.failed" ] && [ ! -e "$T/gates/$C/vm-win.ok" ]' "check: a later pass of another suite doesn't erase a failure"
rm -f "$F/beta-fails"; reset
vr run 'rekordbox 7.2.19 installed' "$RS/beta.suite" --record; rc=$?
ok '[ $rc = 0 ] && [ -f "$G/beta@$K2-arm64.ok" ] && [ ! -e "$G/beta@$K2-arm64.failed" ]' "a rerun of the same suite, snapshot and arch replaces its own pair"
ok 'grep -qx "snapshot {$K2} rekordbox 7.2.19 installed" "$G/beta@$K2-arm64.ok"' "a snapshot given by {id}: the record names it by id and by its name (VS12)"
# a candidate is this commit's release, a fixture is recorded as it is (VS3)
printf '{"payload_version": "1.5.1", "commit": "%s"}\n' 0000000000000000000000000000000000000000 > "$DT/new/payload.json"
reset
vr run 'clean Windows' "$RS/upd.suite" --record; rc=$?
ok '[ $rc = 2 ] && grep -q "dist-test/new/payload.json (a candidate) was built from" "$T/out" && ! calls | grep -q "^snapshot-switch"' "--record refused: a candidate release that isn't HEAD's"
# new/ must be the same release as the root, even when both name HEAD.
printf '{"payload_version": "1.5.1", "commit": "%s"}\n' "$C" > "$DT/new/payload.json"
reset
vr run 'clean Windows' "$RS/upd.suite" --record; rc=$?
ok '[ $rc = 2 ] && grep -q "dist-test/new/payload.json differs from the served payload.json" "$T/out" && ! calls | grep -q "^snapshot-switch"' "--record refused: same-commit new/ uses version 1.5.1 under a 1.5.0 root"
mkrels
printf '%s\n' 'root signature fixture' > "$DT/payload.json.sig"
cp "$DT/payload.json.sig" "$R/dist-test/payload.json.sig"
printf '%s\n' 'different signature fixture' > "$DT/new/payload.json.sig"
reset
vr run 'clean Windows' "$RS/upd.suite" --record; rc=$?
ok '[ $rc = 2 ] && grep -q "dist-test/new/payload.json.sig differs from the served payload.json.sig" "$T/out" && ! calls | grep -q "^snapshot-switch"' "--record refused: new/ has a different signature"
cp "$DT/payload.json.sig" "$DT/new/payload.json.sig"
reset
vr run 'clean Windows' "$RS/upd-tamper.suite" --record; rc=$?
ok '[ $rc = 1 ] && grep -q "dist-test/new/payload.json.sig differs from the served payload.json.sig" "$G/upd-tamper@$K1-arm64.failed" && [ ! -e "$G/upd-tamper@$K1-arm64.ok" ]' "--record fails: new/ signature changed during the run"
rm -f "$DT/payload.json.sig" "$DT/new/payload.json.sig" "$R/dist-test/payload.json.sig"
mkrels; reset
vr run 'clean Windows' "$RS/upd.suite" --record; rc=$?
U="$G/upd@$K1-arm64.ok"
ok '[ $rc = 0 ] && grep -qxF "served dist-test/new/payload.json $(shasum -a 256 "$DT/payload.json" | cut -c1-64) $C" "$U" && grep -qE "^served dist-test/a/payload.json [0-9a-f]{64} 1111111111111111111111111111111111111111$" "$U"' "the record binds the candidate to the root payload and names the fixture with its own commit"
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
forge "one step fewer" '/^step user-signed-high two /d'
forge "a step that failed" 's/^step sys one PASS 0 /step sys one FAIL 1 /'
forge "a step that passed with another code" 's/^step sys one PASS 0 /step sys one PASS 7 /'
forge "result fail" 's/^result pass$/result fail/'
forge "no suite file line" '/^file tests\/win\/suites\/alpha.suite /d'
forge "another snapshot's id and name" "s/^snapshot .*/snapshot {$K2} rekordbox 7.2.19 installed/"
forge "another arch" 's/^arch arm64$/arch x64/'
forge "another VM's uuid (VX8)" 's/^vm_uuid .*/vm_uuid 33333333-aaaa-bbbb-cccc-000000000003/'
forge "no vm_uuid line (VX8)" '/^vm_uuid /d'
forge "the steps in another order" 's/^step sys one /step sys TMP /; s/^step user-signed-high two /step sys one /; s/^step sys TMP /step user-signed-high two /'
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
forge "a candidate with other payload bytes from the same commit" \
  "s|^served dist-test/new/payload.json [0-9a-f]* |served dist-test/new/payload.json $(printf '%064d' 0) |" "$U"
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
vr run 'rekordbox 7.2.19 installed' "$RS/beta.suite" --record; rc=$?
ok '[ $rc = 2 ] && [ -f "$G/beta@$K2-arm64.failed" ] && [ ! -e "$G/beta@$K2-arm64.ok" ] && grep -qx "result fail" "$G/beta@$K2-arm64.failed" && grep -q "couldn.t be ended" "$G/beta@$K2-arm64.failed" && [ "$(calls | grep -c "^snapshot-switch")" = 2 ]' "--record: a step whose processes can't be ended: the VM reverted, .failed written"
vr check "$C" "$DT"; ok '[ $? = 1 ] && grep -q "beta on .* failed" "$T/out"' "check: fails on it"
rm -f "$F/kill-fails" "$F/sleep.pid"; reset
perl -e 'setpgrp(0, 0); exec @ARGV' "$VR" run 'rekordbox 7.2.19 installed' "$RS/beta.suite" --record > "$T/out" 2>&1 & vp=$!
wait_for test -s "$F/sleep.pid"; ready=$?
kill -TERM -"$vp" 2> /dev/null; wait "$vp"; rc=$?
ok '[ $ready = 0 ] && [ $rc = 130 ] && [ -f "$G/beta@$K2-arm64.failed" ] && grep -qx "result incomplete" "$G/beta@$K2-arm64.failed" && [ ! -e "$G/beta@$K2-arm64.ok" ]' "--record interrupted mid-step: its incomplete record is a .failed"
vr check "$C" "$DT"; ok '[ $? = 1 ] && grep -q "beta on .* failed" "$T/out"' "check: fails on an incomplete record"
rm -f "$F/beta-slow"; reset
vr run 'rekordbox 7.2.19 installed' "$RS/beta.suite" --record > /dev/null 2>&1
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
git init -q "$T"; git -C "$R" archive "$C" scripts tests/win core/texts.def win/c/aumid.h win/c/afterexit.h | tar -xf - -C "$T"
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

# ---- two VMs side by side: a lock and a port per VM; stage and check wait for no run on any VM
# rb-win-hand's fake: the same share, its own state and calls
F2="$T/fake2"; rm -rf "$F2"; cp -R "$F" "$F2"
reset2() {
  : > "$F2/calls"; echo running > "$F2/state"; echo rb-win-hand > "$F2/name"; rm -f "$F2/switch-fails" "$F2/prljob-done" "$F2/no-marker" "$F2/rc-zero"
  printf 'win|+|%s\n' "$T/share" > "$F2/shares"; echo on > "$F2/shf"; echo Unknown > "$F2/defined"; : > "$F2/snapshares"; echo Off > "$F2/snapdefined"
  rm -f "$F2/switch-running" "$F2/share-unseen" "$F2/shf-pending"
}
hand() { FAKE_DIR="$F2" VMRUN_VM=rb-win-hand "$@"; }
reset; reset2
mkdir -p "$T/share/.vm-run.lock"; sleep 60 & holder=$!; echo "$holder" > "$T/share/.vm-run.lock/pid"
hand vr run 'clean Windows' "$T/suites/good.suite"; rc=$?
ok '[ $rc = 0 ] && [ "$(cat "$T/share/.vm-run.lock/pid")" = "$holder" ] && [ ! -e "$T/share/.vm-run.rb-win-hand.lock" ] && grep -q "^exec rb-win-hand " "$F2/calls" && ! calls | grep -q .' "rb-win-template's lock held: a run on rb-win-hand goes on under its own lock, released after (rc $rc)"
ok 'basename "$(last_results)" | grep -qE -- "-good-rb-win-hand-[0-9]+\$"' "a run on another VM names it in its id (its job and results folders)"
mv "$T/share/.vm-run.lock" "$T/share/.vm-run.rb-win-hand.lock"; reset2
hand vr run 'clean Windows' "$T/suites/good.suite"; rc=$?
ok '[ $rc = 2 ] && grep -q "another vm-run.sh (pid $holder) drives the VM" "$T/out" && ! grep -q "^snapshot-switch" "$F2/calls"' "rb-win-hand's lock held: a second run on rb-win-hand refused"
reset
vr run 'clean Windows' "$T/suites/good.suite"; rc=$?
ok '[ $rc = 0 ] && [ "$(cat "$T/share/.vm-run.rb-win-hand.lock/pid")" = "$holder" ] && [ ! -e "$T/share/.vm-run.lock" ]' "... while a run on rb-win-template goes on (rc $rc)"
kill "$holder" 2> /dev/null; wait "$holder" 2> /dev/null; rm -rf "$T/share/.vm-run.rb-win-hand.lock"
# each VM's own port when VMRUN_HTTP_PORT doesn't set one: rb-win-template 8766, another VM 8767
reset; reset2
env -u VMRUN_HTTP_PORT "$VR" run 'clean Windows' "$T/suites/good.suite" > "$T/out" 2>&1; rc=$?
res="$(last_results)"
ok '[ $rc = 0 ] && grep -q "port 8766 " "${res}http.log" && grep -qF "127.0.0.1:8766/" "$T/share/job/$(basename "$res")/6-http.ps1"' "rb-win-template serves on 8766 (rc $rc)"
hand env -u VMRUN_HTTP_PORT "$VR" run 'clean Windows' "$T/suites/good.suite" > "$T/out" 2>&1; rc=$?
res="$(last_results)"
ok '[ $rc = 0 ] && grep -q "port 8767 " "${res}http.log" && grep -qF "127.0.0.1:8767/" "$T/share/job/$(basename "$res")/6-http.ps1" && grep -q payload_version "${res}6-http.log"' "rb-win-hand serves on 8767 (rc $rc)"
# a run on each VM at once: a stage and a check refused meanwhile (the share's flock), each run's
# own port, folders and VM, both passing, both locks released, then the share free again
printf '%s\n' '#fake: hold nap' '#fake: out napped' > "$T/suites/s/nap.ps1"
printf '%s\n' '#fake: out dozed' > "$T/suites/s/doze.ps1"
printf '%s\n' 'need http' 'sys 30 nap s/nap.ps1' 'sys 60 http s/http.ps1' > "$T/suites/nap.suite"
printf '%s\n' 'need http' 'sys 60 doze s/doze.ps1' 'sys 60 http s/http.ps1' > "$T/suites/doze.suite"
reset; reset2; P2=$((VMRUN_HTTP_PORT + 1))
mkdir -p "$T/share/repo"; echo keep > "$T/share/repo/keep"
hand env VMRUN_HTTP_PORT=$P2 "$VR" run 'clean Windows' "$T/suites/nap.suite" > "$T/outB" 2>&1 & pb=$!
wait_for test -e "$F2/nap.ready"; ready=$?
vr stage; rc=$?
ok '[ $ready = 0 ] && [ $rc = 2 ] && grep -q "another vm-run.sh reads the share" "$T/out" && [ -f "$T/share/repo/keep" ] && [ ! -e "$T/share/.vm-run.lock" ]' "stage refused while a run on rb-win-hand alone holds the share: the share as it was (rc $rc)"
vr check 0000000000000000000000000000000000000000 "$DT"; rc=$?
ok '[ $rc = 2 ] && grep -q "another vm-run.sh reads the share" "$T/out"' "check refused while a run on rb-win-hand holds the share (rc $rc)"
"$VR" run 'clean Windows' "$T/suites/doze.suite" > "$T/outA" 2>&1; ra=$?
kill -0 "$pb" 2> /dev/null; b_alive=$?
touch "$F2/nap.release"
wait "$pb"; rb=$?
ra_res="$(ls -d "$T/share/results/"*-doze-[0-9]*/ | tail -1)"; rb_res="$(ls -d "$T/share/results/"*-nap-rb-win-hand-*/ | tail -1)"
ok '[ $ra = 0 ] && [ $rb = 0 ] && [ $b_alive = 0 ]' "a run on rb-win-template starts and ends while one on rb-win-hand goes on, both pass (rc $ra, $rb)"
ok 'grep -q "port $VMRUN_HTTP_PORT " "${ra_res}http.log" && grep -q "port $P2 " "${rb_res}http.log" && grep -q payload_version "${ra_res}2-http.log" && grep -q payload_version "${rb_res}2-http.log"' "each run served on its own port"
ok '[ "$ra_res" != "$rb_res" ] && grep -q "^step sys doze PASS" "${ra_res}results.txt" && ! grep -q nap "${ra_res}results.txt" && grep -q "^step sys nap PASS" "${rb_res}results.txt" && ! grep -q doze "${rb_res}results.txt" && [ -f "$T/share/job/$(basename "$ra_res")/1-doze.ps1" ] && [ ! -e "$T/share/job/$(basename "$ra_res")/1-nap.ps1" ] && [ -f "$T/share/job/$(basename "$rb_res")/1-nap.ps1" ]' "each run's job and results folders its own"
ok 'grep -q "^exec rb-win-hand " "$F2/calls" && ! calls | grep -q rb-win-hand && grep -q "^snapshot-switch rb-win-hand " "$F2/calls" && [ "$(calls | grep -c "^snapshot-switch")" = 2 ]' "each run drove its own VM only"
ok '[ ! -e "$T/share/.vm-run.lock" ] && [ ! -e "$T/share/.vm-run.rb-win-hand.lock" ]' "both locks released"
vr check 0000000000000000000000000000000000000000 "$DT"; rc=$?
ok '[ $rc = 1 ] && ! grep -q "reads the share" "$T/out"' "after both runs the share is free: check goes on (rc $rc: no such commit)"
rm -f "$T/share/repo/keep"

# ---- a failed revert stops the run
reset; touch "$F/switch-fails"
vr run 'clean Windows' "$T/suites/good.suite"; rc=$?
ok '[ $rc = 2 ] && ! calls | grep -q "^exec"' 'a failed snapshot switch: exit 2, nothing run'
reset; echo 12 > "$F/bootfails"
VMRUN_BOOT=1 VMRUN_POLL=0.2 vr run 'clean Windows' "$T/suites/good.suite"; rc=$?
ok '[ $rc = 2 ] && grep -q "cmd /c ver within 1 s" "$T/out" && [ "$(calls | grep -c "^snapshot-switch")" = 2 ] && calls | grep -q "^stop"' 'a VM that does not answer by the boot deadline: exit 2, stopped and reverted again'
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
printf '%s\n' '#fake: out mine' | "$VR" user-signed-high 30 > "$T/out" 2>&1; rc=$?
ok '[ $rc = 0 ] && grep -q "^fake: launched as user-signed-high$" "$T/out" && grep -q "^mine" "$T/out"' 'user-signed-high: through the launcher'
printf '%s\n' '#fake: sleep 4' | "$VR" sys 1 > "$T/out" 2>&1; rc=$?
ok '[ $rc = 124 ]' 'a one-off script past its timeout: 124'
echo x | "$VR" sys 601 > "$T/out" 2>&1; ok '[ $? = 2 ]' 'a timeout over 600 s refused'
t0=$(date +%s)
printf '%s\n' '#fake: bg-sleep 20' '#fake: out done' | "$VR" sys 60 > "$T/out" 2>&1; rc=$?
took=$(( $(date +%s) - t0 ))
ok '[ $rc = 0 ] && [ "$took" -lt 10 ]' "what a prlctl leaves in its group is killed when it ends: no wait on its output (${took}s) (VR15)"
# an interrupt stops prlctl's whole group and still reverts the VM (VR14, VR13)
reset; rm -f "$F/sleep.pid"
printf '%s\n' 'sys 60 slow s/slow.ps1' > "$T/suites/longslow.suite"
t0=$(date +%s)
perl -e 'setpgrp(0, 0); exec @ARGV' "$VR" run 'clean Windows' "$T/suites/longslow.suite" > "$T/out" 2>&1 & vp=$!
wait_for test -s "$F/sleep.pid"; ready=$?
kill -TERM -"$vp" 2> /dev/null; wait "$vp"; rc=$?
took=$(( $(date +%s) - t0 ))
ok '[ $ready = 0 ] && [ $rc = 130 ] && [ "$took" -lt 25 ] && [ "$(calls | grep -c "^snapshot-switch")" = 2 ] && [ ! -e "$T/share/.vm-run.lock" ]' "TERM during a step: exit 130 in ${took}s, prlctl's group stopped, the VM reverted, the lock released"
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

# ---- wait SECS NAME logon (a step that signed out or restarted; vmlib's Set-AutoLogon signs back in)
reset
for bad in 'wait 60 x other' 'wait 0 x logon' 'wait 601 x logon' 'wait 60 x logon extra' 'wait 60 x' 'need autologon'; do
  printf '%s\n' "$bad" 'sys 60 one s/pass.ps1' > "$T/suites/bad.suite"
  vr run 'clean Windows' "$T/suites/bad.suite"; rc=$?
  ok '[ $rc = 2 ] && ! calls | grep -q "^snapshot-switch"' "a suite line refused: $bad"
done
printf '%s\n' 'sys 60 one s/pass.ps1' 'wait 30 back logon' 'sys 60 two s/pass.ps1' > "$T/suites/waits.suite"
reset; echo 3 > "$F/wait-left"; touch "$F/wait-down"
vr run 'clean Windows' "$T/suites/waits.suite"; rc=$?
res="$(last_results)"
ok '[ $rc = 0 ] && grep -q "^step wait back PASS 0 " "$res/results.txt" && grep -q "^step sys two PASS 0 " "$res/results.txt"' "a wait step passes once the user is signed in again ($(tr '\n' ';' < "$res/results.txt"))"
ok '[ "$(calls | grep -c "2-back.wait.ps1")" = 5 ] && grep -q "vm-run-wait: rb signed in" "$res/2-back.log" && [ "$(grep -c "not yet" "$res/2-back.log")" = 3 ] && grep -q "restarting" "$res/2-back.log"' "the wait probe is tried again while not yet signed in and while the VM doesn't answer ($(calls | grep -c "2-back.wait.ps1") probes)"
ok 'grep -q "C:.rbtest.signout.mark" "$T/share/job/$(basename "$res")/2-back.wait.ps1" && ! calls | grep -q "back.wrap.ps1"' "the wait probe reads the sign-out mark; no step script runs for it"
reset; touch "$F/wait-nomark"
vr run 'clean Windows' "$T/suites/waits.suite"; rc=$?
res="$(last_results)"
ok '[ $rc = 1 ] && grep -q "^step wait back FAIL 3 " "$res/results.txt" && grep -q "^step sys two PASS 0 " "$res/results.txt" && [ "$(calls | grep -c "2-back.wait.ps1")" = 1 ]' "a wait step with no sign-out mark fails at once, the steps after it still run ($(tr '\n' ';' < "$res/results.txt"))"
rm -f "$F/wait-nomark"
printf '%s\n' 'sys 60 one s/pass.ps1' 'wait 1 back logon' 'sys 60 two s/pass.ps1' > "$T/suites/waits.suite"
reset; echo 12 > "$F/wait-left"
VMRUN_POLL=0.2 vr run 'clean Windows' "$T/suites/waits.suite"; rc=$?
res="$(last_results)"
ok '[ $rc = 1 ] && grep -q "^step wait back TIMEOUT 124 " "$res/results.txt" && grep -q "^step sys two PASS 0 " "$res/results.txt" && ! calls | grep -q "kill-2.ps1"' "a wait step sees no sign-in by its deadline: TIMEOUT, no kill, then the next step runs ($(tr '\n' ';' < "$res/results.txt"))"
rm -f "$F/wait-left"

# ---- stage: the repo's files and the built test exes into the share
mkdir -p "$R/build/winc/win-x64" "$R/build/common-win/arm64" "$R/build/winc"
echo exe > "$R/build/winc/win-x64/winc-wintest.exe"; echo pwt > "$R/build/common-win/arm64/plat-win-test.exe"; echo fx > "$R/build/winc/fixtures.txt"
vr stage; rc=$?
ok '[ $rc = 0 ] && [ -f "$T/share/repo/scripts/bootstrap.ps1" ] && [ -f "$T/share/repo/tests/win/bootstrap/bootstrap.tests.ps1" ] && [ -f "$T/share/repo/core/texts.def" ] && [ -f "$T/share/repo/win/c/aumid.h" ] && [ -f "$T/share/repo/win/c/afterexit.h" ] && [ ! -e "$T/share/repo/core/other.c" ]' "stage: HEAD's scripts, tests/win, texts.def, aumid.h and afterexit.h only"
ok '[ -f "$T/share/vm/x64/winc-wintest.exe" ] && [ -f "$T/share/vm/arm64/plat-win-test.exe" ] && [ -f "$T/share/vm/fixtures.txt" ] && [ ! -e "$T/share/vm/x64/plat-win-test.exe" ]' 'stage: the built test exes there are, by architecture'
ok '[ "$(cat "$T/share/repo/.commit")" = "$C" ] && grep -qx "$(shasum -a 256 "$T/share/vm/x64/winc-wintest.exe" | cut -c1-64) vm/x64/winc-wintest.exe" "$T/share/vm/shas.txt" && [ "$(grep -c . "$T/share/vm/shas.txt")" = 3 ]' 'stage: repo/.commit names HEAD, vm/shas.txt lists what it staged'
ok 'grep -q "^still missing" "$T/out" && grep -q "^  vm/x64/plat-win-test.exe (build/common-win/x64/plat-win-test.exe)$" "$T/out" && grep -q "^  vm/arm64/location-win-test.exe" "$T/out" && ! grep -q "^  vm/x64/winc-wintest.exe" "$T/out"' 'stage: says what is still missing, and from where'
FAKE_BUILD_FAIL=common vr stage --build; rc=$?
ok '[ $rc = 2 ] && grep -q "common/tests/win.sh build/common-win failed" "$T/out" && grep -q "common broke" "$R/build/vm-inputs/win-tests.log" && [ ! -e "$T/share/vm/x64/plat-win-test.exe" ] && [ -f "$T/share/vm/x64/winc-wintest.exe" ]' 'stage --build: a builder that fails stops it before the share is touched, its log kept'
vr stage --build; rc=$?
allvm() { local a f; for a in x64 arm64; do for f in winc-wintest.exe winc-app-wintest.exe winc-update-wintest.exe plat-win-test.exe checked.dll rbsp-core.dll; do [ -f "$T/share/vm/$a/$f" ] || return 1; done; done; }
ok '(for a in x64 arm64; do [ -f "$T/share/vm/$a/memo-stamp-wintest.exe" ] && grep -q " vm/$a/memo-stamp-wintest.exe$" "$T/share/vm/shas.txt" || exit 1; done)' 'NW6: stage --build stages and binds the memo probe for both architectures'
ok '(for app in app-test app-x64-test; do [ -f "$T/share/vm/shell/$app/RB Stems Plus.exe" ] && grep -q " vm/shell/$app/RB Stems Plus.exe$" "$T/share/vm/shas.txt" || exit 1; done)' 'W13b: stage --build stages and binds the native and forced-x64 TEST shells'
ok '[ "$(cat "$T/share/vm/x64/winc-update-wintest.exe")" = "upd x64" ] && grep -q " vm/arm64/winc-update-wintest.exe$" "$T/share/vm/shas.txt"' 'stage --build: winc-update-wintest.exe (review K2) staged by architecture and listed'
ok '[ $rc = 0 ] && allvm && [ "$(cat "$T/share/vm/arm64/winc-app-wintest.exe")" = "app arm64" ] && [ -f "$T/share/vm/fixtures.txt" ]' 'stage --build: builds and stages both architectures'"'"' test exes, the core DLL'"'"'s test-hooks build and fixtures.txt'
ok '[ "$(cat "$T/share/vm/x64/sysdesk.exe")" = "sysdesk x64" ] && [ "$(cat "$T/share/vm/arm64/sysdesk.exe")" = "sysdesk arm64" ] && [ -f "$T/share/vm/uia/rbui/rbui.exe" ] && [ -f "$T/share/vm/uia/rbui/Interop.UIAutomationClient.dll" ] && grep -q " vm/uia/rbui/FlaUI.Core.dll$" "$T/share/vm/shas.txt"' 'stage --build: the UI driver too (tests/win/uia/build.sh): sysdesk.exe by architecture, rbui and its DLLs in vm/uia/rbui/, listed in vm/shas.txt'
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
mkdir -p "$T/share/vm/uia/rbui"
echo x > "$T/share/vm/uia/rbui/rbui.exe"
for r in a carried new; do echo '{}' > "$T/share/dist-test/$r/payload.json"; done
# bridge.suite's need file lines: what bridge/win/tests/stage.sh puts in the share
mkdir -p "$T/share/bridge-win/fake" "$T/share/bridge-win/model"
for f in run.ps1 onnxruntime.dll harness.exe fake/onnxruntime.dll model/hdemucs.onnx; do echo x > "$T/share/bridge-win/$f"; done
suites=(bootstrap startup uninstall platform platform-owner prepare-w13b update update-rollback smoke worker worker-choice bridge
        uninstall-cases restore volumes sessions shell-ui sessions-interactive shell-copy watcher stems-cache-uac
        prepare-app prepare-facts prepare-update)
# Each lane gets its own fake state, staged share and port. Keep the results for
# assertions in suite order and gather both sets of scripts for the parser below.
run_suite_lane() (
  local i="$2" s snap rc
  T="$1"; F="$T/fake"
  export FAKE_DIR="$F" FAKE_SHARE="$T/share" FAKE_VOLUMES="$T/volumes" VMRUN_SHARE="$T/share" \
    VMRUN_VOLUMES="$T/volumes" VMRUN_HTTP_PORT="$3"
  for ((; i < ${#suites[@]}; i += 2)); do
    s="${suites[i]}"
    reset
    snap='clean Windows'; case "$s" in worker|worker-choice|bridge|restore|shell-copy|watcher|stems-cache-uac|prepare-app|prepare-facts|prepare-update) snap='rekordbox 7.2.19 installed' ;; esac
    "$VR" run "$snap" "$here/tests/win/suites/$s.suite" > "$T/$s.out" 2>&1; rc=$?
    printf '%s\n%s\n' "$rc" "$(last_results)" > "$T/$s.result"
  done
)
other_lane="$T/suite-lane"
mkdir -p "$other_lane/share" "$other_lane/volumes"
cp -R "$F" "$other_lane/fake" || exit 2
cp -R "$T/share/"{bridge-win,dist-test,probe,repo,vm} "$other_lane/share/" || exit 2
run_suite_lane "$other_lane" 1 "$((VMRUN_HTTP_PORT + 20000))" & suite_lane_pid=$!
run_suite_lane "$T" 0 "$VMRUN_HTTP_PORT"
wait "$suite_lane_pid" || exit 2
for ((i = 0; i < ${#suites[@]}; i++)); do
  s="${suites[i]}"
  lane="$T"; [ $((i % 2)) = 0 ] || lane="$other_lane"
  { read -r rc; read -r res; } < "$lane/$s.result" || exit 2
  n=$(grep -c '^step ' "$res/results.txt" 2> /dev/null)
  ok '[ $rc = 0 ] && [ "$n" -ge 1 ] && ! grep -qv " PASS " "$res/results.txt"' "suite $s: $n steps parse and run through the fake ($(tail -2 "$lane/$s.out" | tr '\n' ' '))"
  # bridge.suite's network step (review TR71: BW3's row, the share mapped as a network drive)
  [ "$s" != bridge ] || ok 'grep -q "^step user network PASS " "$res/results.txt"' "suite bridge: its network step runs through the fake ($(grep "^step user network " "$res/results.txt" 2> /dev/null))"
done
mv "$other_lane/share/job/"* "$T/share/job/" || exit 2
ok 'grep -l "function Check" "$T/share/job"/*bootstrap*/1-unit-51.ps1 > /dev/null 2>&1' "a T17 step starts with vmlib.ps1"
ok 'f="$(ls "$T/share/job"/*worker*/3-cache-install.ps1 2> /dev/null | head -1)"; [ -n "$f" ] && [ "$(grep -n "function Check" "$f" | cut -d: -f1)" -lt "$(grep -n "function Close-Rekordbox" "$f" | cut -d: -f1)" ]' "worker.suite's steps start with vmlib.ps1, then worker/rblib.ps1"
ok 'f="$(ls "$T/share/job"/*restore*/5-restore.ps1 2> /dev/null | head -1)"; [ -n "$f" ] && [ "$(grep -n "function Check" "$f" | cut -d: -f1)" -lt "$(grep -n "function Close-Rekordbox" "$f" | cut -d: -f1)" ] && [ "$(grep -n "function Close-Rekordbox" "$f" | cut -d: -f1)" -lt "$(grep -n "function Invoke-Restore" "$f" | cut -d: -f1)" ]' "restore.suite's steps start with vmlib.ps1, worker/rblib.ps1, then restore/rslib.ps1"
ok 'f="$(ls "$T/share/job"/*uninstall-cases*/3-menu-uninstall.ps1 2> /dev/null | head -1)"; [ -n "$f" ] && grep -q "function Invoke-MenuUninstall" "$f"' "uninstall-cases.suite's steps start with uninstall/cases/unlib.ps1 too"
ok 'f="$(ls "$T/share/job"/*sessions*/2-account-b.ps1 2> /dev/null | head -1)"; [ -n "$f" ] && grep -q "function New-TestAccount" "$f" && grep -q "function Start-AfterExitWatch" "$f" && grep -q "function Get-BProcesses" "$f"' "sessions.suite's steps start with vmlib.ps1, uninstall/cases/unlib.ps1, then sessions/sslib.ps1"
ok 'f="$(ls "$T/share/job"/*volumes*/2-ntfs-cases.ps1 2> /dev/null | head -1)"; [ -n "$f" ] && grep -q "function Test-Loc" "$f" && grep -q "function Check" "$f"' "volumes.suite's steps start with vmlib.ps1 and volumes/vlib.ps1"
# ---- the PowerShell itself (VR17): the text ids exist; with pwsh, every assembled script (the
# steps with their prelude and includes, the wrappers, the launchers, the prepare step) parses
# with no error and uses no PowerShell 7-only syntax (&&, ||, ??, ?., ?[, the ternary ?:)
bad=""
for id in $(grep -rhoE "Get-(Core|Menu)Text '[A-Z0-9_]+'" "$here/tests/win/suites" | sed "s/.*'\(.*\)'/\1/" | sort -u); do
  grep -qE "^TEXT\($id," "$here/core/texts.def" || bad+=" $id"
done
ok '[ -z "$bad" ]' "every Get-CoreText and Get-MenuText id is in core/texts.def${bad:+ (missing:$bad)}"
bad=""
for k in $(grep -rhoE "Get-BootText '[A-Za-z0-9]+'" "$here/tests/win/suites" | sed "s/.*'\(.*\)'/\1/" | sort -u); do
  grep -qE "^[[:space:]]+$k[[:space:]]+=[[:space:]]+['\"]" "$here/scripts/bootstrap.ps1" || bad+=" $k"
done
ok '[ -z "$bad" ]' "every Get-BootText key is one of scripts/bootstrap.ps1's \$Texts${bad:+ (missing:$bad)}"
ok '! grep -rF "'\''RB Stems Plus cleanup'\''" "$here/tests/win/suites" | grep -qv "StartsWith("' "the RunOnce cleanup values are matched by their prefix (UN13: one per temp folder), never by one name"
if command -v pwsh > /dev/null 2>&1; then
  cat > "$T/worker-choice-protocol.ps1" <<'EOF'
param($ScriptPath, $Work)
$ErrorActionPreference = 'Stop'
$ast = [System.Management.Automation.Language.Parser]::ParseFile($ScriptPath, [ref]$null, [ref]$null)
foreach ($node in $ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] }, $false)) {
    . ([scriptblock]::Create($node.Extent.Text))
}
function Get-Sha($path) { (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLowerInvariant() }
$RbExe = Join-Path $Work 'rekordbox.exe'
Set-Content -LiteralPath $RbExe -Value 'rekordbox fixture'
$manifest = 'c' * 64
$name = 'rbstemsplus-admin-' + ('d' * 32)
$source = 'C:\B source'
$exe = 'C:\Program Files\RB Stems Plus\bin\worker.exe'
$setup = New-FixtureWorkerStartInfo $exe 'install-worker' $source $manifest $name
$install = New-FixtureWorkerStartInfo $exe 'install' $source $manifest $name
$exe16 = (Get-Sha $RbExe).Substring(0, 16)
$tail = '"--manifest" "{0}" "--source" "{1}" "--force" "{{0}}" "--pipe" "\\.\pipe\{2}"' -f $manifest, $source, $name
if ($setup.Arguments -cne ('"install-worker" "--proto" "2" "--targets" "*" ' + ($tail -f 0)) -or
    $install.Arguments -cne ('"install" "--proto" "2" "--targets" "7.2.19:' + $exe16 + '" ' + ($tail -f 1))) {
    throw "Wrong protocol arguments: $($setup.Arguments) / $($install.Arguments)"
}
Confirm-FixtureWorkerResult 'install-worker' 13 13 'run the app''s own worker now' | Out-Null
Confirm-FixtureWorkerResult 'install' 0 0 "log`r`n@target 7.2.19 0 installed`r`n" | Out-Null
function Assert-ResultFailure($code, $pipeLog, $expected) {
    try { Confirm-FixtureWorkerResult 'install' 0 $code $pipeLog | Out-Null }
    catch {
        if ($_.Exception.Message -ceq $expected) { return }
        throw
    }
    throw "Expected fixture failure: $expected"
}
Assert-ResultFailure 0 '@target 7.2.20 0 installed' 'Expected an installed result for rekordbox 7.2.19'
Assert-ResultFailure 15 '' 'install scope or staged payload changed (exit 15)'
Assert-ResultFailure 16 "@target 7.2.19 0 installed`n@target 7.2.20 6 put-back`n@target 7.2.21 - not-started" `
    'install only partially completed (exit 16): 7.2.19 0 installed, 7.2.20 6 put-back, 7.2.21 - not-started'
'PASS VM worker protocol 2'
EOF
  pwsh -NoProfile -NonInteractive -File "$T/worker-choice-protocol.ps1" "$here/tests/win/suites/sessions/worker-choice-install.ps1" "$T" > "$T/worker-choice.out" 2>&1; rc=$?
  ok '[ "$rc" = 0 ] && grep -qx "PASS VM worker protocol 2" "$T/worker-choice.out"' "worker-choice protocol 2 ($(cat "$T/worker-choice.out"))"
  pwsh -NoProfile -NonInteractive -File "$here/tests/win/w13b-checks-test.ps1" \
    "$here/win/shell/vm/w13b-checks.ps1" "$here/tests/harness/traces/A/prepare-choose-estimate.jsonl" "$T" > "$T/w13b-cases.out" 2>&1; rc=$?
  ok '[ "$rc" = 0 ] && [ "$(grep -c "^PASS " "$T/w13b-cases.out")" -eq 10 ]' "W13b trace and pixel assertions ($(tail -1 "$T/w13b-cases.out"))"
  pwsh -NoProfile -NonInteractive -File "$here/tests/win/suites/shell-ui/a11y-test.ps1" > "$T/a11y.out" 2>&1; rc=$?
  ok '[ "$rc" = 0 ] && [ "$(grep -c "^PASS " "$T/a11y.out")" -eq 5 ]' "shell UI accessibility host checks ($(tr '\n' ' ' < "$T/a11y.out"))"
  pwsh -NoProfile -NonInteractive -File "$here/tests/win/suites/prepare/app-test.ps1" > "$T/prepare-app.out" 2>&1; rc=$?
  ok '[ "$rc" = 0 ] && [ "$(grep -c "^PASS " "$T/prepare-app.out")" -eq 6 ]' "Prepare app host checks ($(tr '\n' ' ' < "$T/prepare-app.out"))"
  pwsh -NoProfile -NonInteractive -File "$here/tests/win/update-suite-test.ps1" "$T/update-cases" > "$T/update-cases.out" 2>&1; rc=$?
  ok '[ "$rc" = 0 ] && [ "$(grep -c "^PASS UP" "$T/update-cases.out")" -eq 8 ]' "update sticky and stub-change host cases ($(tail -1 "$T/update-cases.out"))"
  pwsh -NoProfile -NonInteractive -File "$here/tests/win/update-suite-test.ps1" -Work "$T/rollback-cases" -Group rollback > "$T/rollback-cases.out" 2>&1; rc=$?
  ok '[ "$rc" = 0 ] && [ "$(grep -c "^PASS UP" "$T/rollback-cases.out")" -eq 13 ]' "update-rollback host cases ($(tail -1 "$T/rollback-cases.out"))"

  pwsh -NoProfile -NonInteractive -File "$here/tests/win/suites/uac/trace-order-test.ps1" > "$T/session-trace.out" 2>&1; rc=$?
  ok '[ "$rc" = 0 ]' "session trace ($(tail -1 "$T/session-trace.out"))"
  cat > "$T/switch-sweep.ps1" <<'EOF'
param($switchScript, $fixture)
$ErrorActionPreference = 'Stop'
function Share($path) { Join-Path $fixture $path }
function Need($path, $why) {
    $file = Share $path
    if (-not (Test-Path -LiteralPath $file)) { throw "missing release fixture: $path" }
    $file
}
function Check($pass, $what) { if (-not $pass) { throw $what } }
function Done {}
$VmRun = @{ Step = '' }
foreach ($source in 'a', 'new') {
    $dir = New-Item -ItemType Directory -Force (Share "dist-test/$source")
    @{ payload_version = $source } | ConvertTo-Json | Set-Content (Join-Path $dir.FullName 'payload.json')
}
foreach ($step in 'switch-a-native-0', 'switch-new-x64-2000', 'switch-a') {
    $VmRun.Step = $step
    & $switchScript
    $served = Get-Content -Raw (Share 'dist-test/current/payload.json') | ConvertFrom-Json
    if ($served.payload_version -ne ($step -split '-')[1]) { throw "wrong fixture served for $step" }
}
'shell-ui switch: both sweep variants and the plain update step served the expected fixture'
EOF
  pwsh -NoProfile -NonInteractive -File "$T/switch-sweep.ps1" "$here/tests/win/suites/update/switch.ps1" "$T/w2-fixture" > "$T/switch.out" 2>&1; rc=$?
  ok '[ "$rc" = 0 ] && grep -q "^shell-ui switch: both sweep variants" "$T/switch.out"' "shell-ui switch serves the named release for each sweep variant ($(tail -1 "$T/switch.out"))"

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
  mkdir "$T/parse-broken"
  printf '%s\n' '$a = "no $x: here"' > "$T/parse-broken/broken.ps1"
  pwsh -NoProfile -NonInteractive -File "$T/parse.ps1" "$T/parse-broken" > "$T/parse.out" 2>&1
  ok 'grep -q "^PARSE broken.ps1:1:" "$T/parse.out"' "the parse check finds a \"\$x:\" string (VR4's kind)"
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
ok '[ -f "$B/bootstrap.ps1" ] && [ "$(cat "$B/w.zip")" = ZIP ] && [ "$(python3 -I -c "import json,sys; print(json.load(open(sys.argv[1]))[\"windows\"][\"app\"][\"version\"])" "$B/payload.json")" = 0.0.0 ] && /usr/bin/openssl dgst -sha256 -verify "$T/k1/test.pub.pem" -signature "$B/payload.json.sig" "$B/payload.json" > /dev/null 2>&1 && grep -q "k2 didn.t sign this TEST build" "$T/out"' "stage-dist.sh: bad-version/ names another app version, signed with the key that signed the TEST build (a key that didn't is passed over)"
"$SD" "$D" --key "$T/k2" > "$T/out" 2>&1; rc=$?
ok '[ $rc = 0 ] && [ ! -e "$B" ] && grep -q "bad-version/ not made" "$T/out" && [ -d "$D/dist-test/fault-zip" ]' "stage-dist.sh: no key that signed the TEST build: no bad-version/, said; the faults still made"
cp "$T/k1/test.pub.pem" "$T/norel/release.pub.pem"
"$SD" "$D" --key "$T/k1" > "$T/out" 2>&1; rc=$?
ok '[ $rc = 2 ] && grep -q "a release key never signs" "$T/out" && [ ! -e "$B" ]' "stage-dist.sh: a key pair whose public half is one of keys/*.pub.pem is refused"
rm -f "$T/norel/release.pub.pem"
# carried/ from update.suite's a/: the next payload_version, a/'s windows.app.version and files, signed with a/'s key
A="$D/dist-test/a"; CR="$D/dist-test/carried"; mkdir -p "$A"
printf '%s' '{"payload_version": "1.4.9", "commit": "c1", "windows": {"app": {"zip": "w.zip", "version": "1.4.9"}}}' > "$A/payload.json"
/usr/bin/openssl dgst -sha256 -sign "$T/k2/test-key.pem" -out "$A/payload.json.sig" "$A/payload.json"; printf 'AZIP' > "$A/w.zip"; printf 'AB' > "$A/bootstrap.ps1"
"$SD" "$D" --key "$T/k1" --key "$T/k2" > "$T/out" 2>&1; rc=$?
pv() { python3 -I -c 'import json, sys; p = json.load(open(sys.argv[1])); print(p["payload_version"], p["windows"]["app"]["version"], p["commit"])' "$1"; }
ok '[ $rc = 0 ] && [ "$(pv "$CR/payload.json")" = "1.4.10 1.4.9 c1" ] && [ "$(cat "$CR/w.zip")" = AZIP ] && [ "$(cat "$CR/bootstrap.ps1")" = AB ] && /usr/bin/openssl dgst -sha256 -verify "$T/k2/test.pub.pem" -signature "$CR/payload.json.sig" "$CR/payload.json" > /dev/null 2>&1 && grep -q "payload_version 1.4.9 -> 1.4.10, windows.app.version 1.4.9" "$T/out"' "stage-dist.sh: carried/ is a/ with the next payload_version, a/'s app version, signed with a/'s key (rc $rc)"
"$SD" "$D" --key "$T/k1" > "$T/out" 2>&1; rc=$?
ok '[ $rc = 0 ] && [ "$(pv "$CR/payload.json")" = "1.4.10 1.4.9 c1" ] && grep -q "carried/ not made: no test key pair" "$T/out" && [ -d "$B" ]' "stage-dist.sh: no key that signed a/: carried/ left as it was, said; bad-version/ still made"
rm -f "$A/payload.json.sig"
"$SD" "$D" --key "$T/k2" > "$T/out" 2>&1; rc=$?
ok '[ $rc = 0 ] && [ -f "$CR/payload.json" ] && grep -q "carried/ not made: no signed update.suite release" "$T/out"' "stage-dist.sh: an unsigned a/: carried/ left as it was, said"
rm -rf "$A" "$CR"
"$SD" "$D" --key "$T/k1" > "$T/out" 2>&1; rc=$?
ok '[ $rc = 0 ] && [ ! -e "$CR" ] && grep -q "carried/ not made: no signed update.suite release" "$T/out"' "stage-dist.sh: no a/: no carried/, said"
printf '%s' '{"windows": {"app": {"zip": "../w.zip"}}}' > "$D/dist-test/payload.json"
"$SD" "$D" --key "$T/k1" > "$T/out" 2>&1; ok '[ $? = 2 ]' 'stage-dist.sh: a zip name with a path refused'
# stage makes them too, for the share's signed TEST build, with the checkout's own test key
mkdir -p "$R/build/test-keys"; cp "$T/k1/test-key.pem" "$T/k1/test.pub.pem" "$R/build/test-keys/"
cp "$DT/payload.json" "$T/payload.saved"
printf '{"payload_version": "1.5.0", "commit": "%s", "windows": {"app": {"zip": "w.zip", "version": "1.5.0"}}}\n' "$C" > "$DT/payload.json"
# a TEST build in a plan's shape is signed with the gate archive's test key (build.sh), not the
# checkout's: stage passes that key too, so bad-version/ is made
mkdir -p "$T/arch/tag-v9.9.9/test-keys"; cp "$T/k2/test-key.pem" "$T/k2/test.pub.pem" "$T/arch/tag-v9.9.9/test-keys/"
/usr/bin/openssl dgst -sha256 -sign "$T/k2/test-key.pem" -out "$DT/payload.json.sig" "$DT/payload.json"; printf 'ZIP' > "$DT/w.zip"; printf B > "$DT/bootstrap.ps1"
GATES_ARCHIVE="$T/arch" vr stage; rc=$?
ok '[ $rc = 0 ] && [ -f "$DT/bad-version/payload.json" ] && /usr/bin/openssl dgst -sha256 -verify "$T/k2/test.pub.pem" -signature "$DT/bad-version/payload.json.sig" "$DT/bad-version/payload.json" > /dev/null 2>&1 && grep -q "signed with the test key in $T/arch/tag-v9.9.9/test-keys" "$T/out"' "stage: a TEST build signed with the gate archive's test key gets bad-version/, with that key (rc $rc)"
/usr/bin/openssl dgst -sha256 -sign "$T/k1/test-key.pem" -out "$DT/payload.json.sig" "$DT/payload.json"; printf 'ZIP' > "$DT/w.zip"; printf B > "$DT/bootstrap.ps1"
cp "$DT/payload.json" "$DT/payload.json.sig" "$DT/w.zip" "$DT/bootstrap.ps1" "$R/dist-test/"
vr stage; rc=$?
ok '[ $rc = 0 ] && [ -d "$DT/fault-zip" ] && [ -d "$DT/fault-json" ] && [ -d "$DT/fault-sig" ] && grep -q "signed with the test key in $R/build/test-keys" "$T/out" && ! grep -q "fault-\*/ and bad-version/" "$T/out"' "stage: the share's signed TEST build gets its faults and bad-version/, with the checkout's test key"
reset
vr run 'clean Windows' "$RS/gamma.suite" --record; rc=$?
ok '[ $rc = 0 ] && [ -f "$DT/bad-version/payload.json" ]' "--record accepts the bad-version/ made by stage (rc $rc, $(grep -m1 '^vm-run.sh:' "$T/out"))"
# stage --serve DIR: the folder a run --serve DIR serves gets them too (bootstrap's faults fetch them from the run's server)
SV="$T/share/dist-other"; rm -rf "$SV"; mkdir -p "$SV"
printf '%s' '{"payload_version": "0.0.0", "windows": {"app": {"zip": "w.zip", "version": "0.0.0"}}}' > "$SV/payload.json"
/usr/bin/openssl dgst -sha256 -sign "$T/k1/test-key.pem" -out "$SV/payload.json.sig" "$SV/payload.json"; printf 'OZIP' > "$SV/w.zip"; printf 'OB' > "$SV/bootstrap.ps1"
vr stage --serve "$SV"; rc=$?
ok '[ $rc = 0 ] && [ "$(cat "$SV/fault-zip/w.zip")" = OZIPX ] && [ "$(tail -c 1 "$SV/fault-json/payload.json")" = X ] && [ -d "$SV/fault-sig" ] && grep -Fq "0.0.0 -> 0.0.00," "$T/out" && [ -d "$DT/fault-zip" ]' "stage --serve DIR: DIR's signed TEST build gets its faults and bad-version/ too, dist-test/ still its own"
vr stage --serve "$T/sd/dist-test"; ok '[ $? = 2 ] && grep -q "must be inside the share" "$T/out"' "stage --serve: a folder outside the share refused"
rm -f "$SV/payload.json.sig"; vr stage --serve "$SV"; ok '[ $? = 2 ] && grep -q "no signed TEST build" "$T/out"' "stage --serve: a folder with no signed TEST build refused"
rm -rf "$SV"
cp "$T/payload.saved" "$DT/payload.json"; rm -f "$DT/payload.json.sig"
cp "$DT/payload.json" "$R/dist-test/payload.json"; rm -f "$R/dist-test/payload.json.sig" "$R/dist-test/w.zip"

# ---- tests/win/first-run.sh against the fake (never against a VM here)
FRS="$R/tests/win/first-run.sh"   # the checkout's own, as vm-run.sh (VT2)
first_run_base="$C"
# Only these suites run by name here. Copy their actual definitions and referenced
# inputs, so the early stage and record cases don't hash the unrelated suite tree.
(
  cd "$here/tests/win/suites" || exit 2
  suites=(smoke.suite platform.suite bootstrap.suite uninstall.suite update.suite)
  {
    printf '%s\n' "${suites[@]}"
    awk '$1 == "include" { print $2 } $1 ~ /^(sys|user|user-high|user-signed|user-signed-high)$/ { print $4 }' "${suites[@]}"
  } | LC_ALL=C sort -u | tar -cf - -T -
) | tar -xf - -C "$RS" || exit 2
git -C "$R" add tests/win/suites && git -C "$R" -c user.email=t@t -c user.name=t commit -q -m 'first-run suites' || exit 2
C="$(git -C "$R" rev-parse HEAD)"
# the share as a release's --record runs want it: no stray fixture copies, the checkout's TEST build
rm -rf "$DT"/fault-* "$DT/bad-version"; cp "$DT/bootstrap.ps1" "$R/dist-test/bootstrap.ps1"
printf '{"payload_version": "1.5.0", "commit": "%s"}\n' "$C" > "$DT/payload.json"
cp "$DT/payload.json" "$R/dist-test/payload.json"
# Like the RC builder, populate new/ from the finalized root payload.
mkrels
ok 'cmp -s "$DT/payload.json" "$DT/new/payload.json"' "first-run.sh: the new/ candidate matches the staged TEST payload"
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
reset; rm -f "$T/fr.out" "$F/sleep.pid"
printf 'y\n' | perl -e 'setpgrp(0, 0); exec @ARGV' "$FRS" --from 5 > "$T/fr.out" 2>&1 & fp=$!
wait_for test -s "$F/sleep.pid"; ready=$?
kill -TERM "$fp" 2> /dev/null; wait "$fp"; rc=$?
lock_after="$([ -e "$T/share/.vm-run.lock" ] && echo held || echo free)"; switches="$(calls | grep -c "^snapshot-switch")"
t0=$(date +%s); until grep -q "STOP at step 5" "$T/fr.out" 2> /dev/null || [ $(( $(date +%s) - t0 )) -ge 5 ]; do sleep 0.2; done
ok '[ $ready = 0 ] && [ $rc = 1 ] && [ "$lock_after" = free ] && [ "$switches" = 2 ] && grep -q "STOP at step 5: interrupted during vm-run.sh" "$T/fr.out"' "first-run.sh: TERM to its own pid ends vm-run.sh after its cleanup, then stops (VX10: rc $rc, lock $lock_after, $switches switches)"
# (unfixed, vm-run.sh lives on: the next cases wait for the share's lock to go, bounded)
t0=$(date +%s); while [ -e "$T/share/.vm-run.lock" ] && [ $(( $(date +%s) - t0 )) -lt 60 ]; do sleep 0.5; done
# first-run.sh's bounded (VT4, VT6): a leftover in the child's group ends with it; a child's
# cleanup after TERM is waited for past the old 60 s, its lines streamed as they come
eval "$(sed -n '/^bounded() {/,/^}/p' "$FRS")"
t0=$(date +%s); out="$(bounded 1 bash -c 'sleep 4 & echo hi; exit 0')"; rc=$?; t1=$(date +%s)
ok '[ $rc = 0 ] && [ "$out" = hi ] && [ $((t1 - t0)) -le 2 ]' "first-run.sh bounded: a child's leftover (sleep 4 &) is killed when it ends: back in $((t1 - t0)) s (VT6)"
wait "$cleanup_case" && read -r rc took < "$T/cleanup.result" || { rc=1; took=0; }
ok '[ -e "$T/cleanup.streaming" ] && grep -qx started "$T/cleanup.out" && grep -qx cleanup-start "$T/cleanup.out"' "first-run.sh bounded: the child's lines come while it runs (VT5's streaming has them)"
ok '[ $rc = 124 ] && grep -qx cleanup-done "$T/cleanup.out" && [ "$took" -ge 70 ]' "first-run.sh bounded: a 70 s cleanup after TERM is waited for, then 124 (VT4: ${took}s in background)"
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
NR1='vm-run.sh check: no record of worker on "rekordbox 7.2.19 installed" (arm64) in /g/vm-win'
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
# Step 9 only needs the record fixtures from the checkout before the full first-run cases.
git -C "$R" reset --hard "$first_run_base" > /dev/null || exit 2
first_run_step9() {
  vr stage || return $?
  printf 'y\n' | "$FRS" --from 9 > "$T/out" 2>&1
}
# One passing row covers how the expected diagnostics interact with a valid record.
printf '%s\n' 'alpha arm64 clean Windows' 'ghost arm64 clean Windows' > "$RS/release.list"
git -C "$R" add -A && git -C "$R" -c user.email=t@t -c user.name=t commit -q -m ghost
C="$(git -C "$R" rev-parse HEAD)"
printf '{"payload_version": "1.5.0", "commit": "%s"}\n' "$C" > "$T/share/dist-test/payload.json"
cp "$T/share/dist-test/payload.json" "$R/dist-test/payload.json"; mkrels
reset
first_run_step9; rc=$?
ok '[ $rc = 0 ] && grep -q "ghost on \"clean Windows\" (arm64): no ghost.suite yet, not run" "$T/out" && grep -q "ok: check fails closed only on the rows no suite can pass yet: no suite: ghost;" "$T/out" && grep -q "first run: every step passed" "$T/out"' "first-run.sh: a row with no suite: step 9 passes on its one \"no record\" line ($(grep -m1 STOP "$T/out"))"
# step 9 and a known gap (first-run.sh's GAPS, worker:cache-hit): a row whose record skipped only
# that step fails check closed, and step 9 says so and passes; a SKIP of any other step stops it.
# The checkout gets a worker row and suite, committed: HEAD, the TEST build and the releases follow
gaprun() {   # FIRST-STEP-SCRIPT [SNAPSHOT-NAME for an older record with another snapshot ID]
  printf '%s\n' 'need http' "sys 60 one s/$1.ps1" 'sys 60 cache-hit s/skip.ps1' "# ${2:-}" > "$RS/worker.suite"
  git -C "$R" add -A && git -C "$R" -c user.email=t@t -c user.name=t commit -q -m "worker $1 ${2:-}"
  C="$(git -C "$R" rev-parse HEAD)"
  printf '{"payload_version": "1.5.0", "commit": "%s"}\n' "$C" > "$T/share/dist-test/payload.json"
  cp "$T/share/dist-test/payload.json" "$R/dist-test/payload.json"; mkrels
  if [ -n "${2:-}" ]; then
    mkdir -p "$T/gates/$C/vm-win"
    printf 'commit %s\nsuite worker\nsnapshot {99999999-aaaa-bbbb-cccc-000000000009} %s\narch arm64\nrun older\nstep sys one PASS 0 1s\nstep sys cache-hit SKIP 77 1s\nresult skip\n' "$C" "$2" > "$T/gates/$C/vm-win/worker@99999999-aaaa-bbbb-cccc-000000000009-arm64.failed"
  fi
  reset
  first_run_step9
}
printf '%s\n' 'worker arm64 clean Windows' >> "$RS/release.list"
gaprun pass; rc=$?
ok '[ $rc = 0 ] && grep -q "vm-run.sh check: worker on \"clean Windows\" (arm64) failed: worker@" "$T/out" && grep -q "only a known gap skipped (worker:cache-hit): worker" "$T/out" && grep -q "first run: every step passed" "$T/out"' "first-run.sh: a known gap's SKIP (worker's cache-hit) fails check closed; step 9 says so and passes ($(grep -m1 STOP "$T/out"))"
gaprun skip; rc=$?
ok '[ $rc = 1 ] && grep -q "STOP at step 9: vm-run.sh check $C failed: vm-run.sh check: worker on" "$T/out"' "first-run.sh: the gap's row with another step skipped too stops step 9"
gaprun pass other; rc=$?
ok '[ $rc = 0 ] && grep -q "first run: every step passed" "$T/out"' "first-run.sh: step 9 ignores a worker record for a different snapshot (VX9: $(grep -m1 STOP "$T/out"))"
gaprun pass 'clean Windows'; rc=$?
ok '[ $rc = 1 ] && grep -q "STOP at step 9: worker on \"clean Windows\" (arm64) has 2 records" "$T/out"' "first-run.sh: step 9 rejects two records for the worker row's snapshot (VX9)"
# The result from a previously passing run of a regular row is insufficient, even if check itself
# would accept it. Match by snapshot ID here (the worker cases above match by name).
record_pick() (
  eval "$(sed -n '/^record_for_run() {/,/^}/p' "$FRS")"
  fail() { echo "$*"; exit 1; }
  GATES="$T/gates" head=vx9
  mkdir -p "$GATES/$head/vm-win"
  printf '%s\n' 'snapshot {id} other' 'run previous' > "$GATES/$head/vm-win/alpha@id-arm64.ok"
  record_for_run alpha arm64 '{id}' "$1"
)
record_pick previous > "$T/out"; ok '[ $? = 0 ]' "first-run.sh: step 9 matches snapshot IDs (VX9)"
record_pick current > "$T/out"; rc=$?
ok '[ $rc = 1 ] && grep -q "isn.t the run step 9 just made (current)" "$T/out"' "first-run.sh: step 9 rejects a regular row's stale record (VX9)"
# the gap row's line is accepted only as the whole of what check says: any other status or line
# stops step 9 (WS1)
GL='vm-run.sh check: worker on "clean Windows" (arm64) failed: worker@11111111-aaaa-bbbb-cccc-000000000001-arm64.failed'
check_ok 1 "$GL" "$GL" > /dev/null; ok '[ $? = 0 ]' "check_ok: exit 1 with exactly the gap row's line: ok"
check_ok 1 "$GL" "" > /dev/null; ok '[ $? = 1 ]' "check_ok: on the gap row, check exiting 1 silently stops step 9 (WS1)"
check_ok 2 "$GL" "vm-run.sh: another vm-run.sh (pid 1) drives the VM" > /dev/null; ok '[ $? = 1 ]' "check_ok: on the gap row, exit 2 with a stop line stops step 9 (WS1)"
check_ok 137 "$GL" "" > /dev/null; ok '[ $? = 1 ]' "check_ok: on the gap row, exit 137 stops step 9 (WS1)"
check_ok 1 "$GL" "$GL"$'\n'"tests/win/vm-run.sh: line 700: x: unbound variable" > /dev/null; ok '[ $? = 1 ]' "check_ok: the gap line followed by a crash line stops step 9 (WS1)"

# ---- the owner's stick and empty reader
printf '%s\n' 'need usb' 'need usb-reader' 'sys 60 facts s/reader.ps1' > "$T/suites/reader.suite"
printf '%s\n' '#fake: jobfile usb.assigned' '#fake: jobfile usb-reader.assigned' > "$T/suites/s/reader.ps1"
printf '%s\n' 'usb rb-win-template stick-id' 'usb-reader rb-win-template reader-id' > "$T/vm-run.conf"
reset
vr run 'clean Windows' "$T/suites/reader.suite"; rc=$?
res="$(last_results)"
ok '[ $rc = 0 ] && [ "$(calls | awk "/^start/{exit} /^srv usb set/{print \$4}" | xargs)" = "stick-id reader-id" ] && [ "$(calls | awk "/^srv usb del/{print \$4}" | xargs)" = "stick-id reader-id" ] && grep -qx "JOBFILE usb.assigned: stick-id" "$res/1-facts.log" && grep -qx "JOBFILE usb-reader.assigned: reader-id" "$res/1-facts.log"' 'empty reader: both devices are independently assigned, reported and released'
reset; touch "$F/prepare-fails"
vr run 'clean Windows' "$T/suites/reader.suite"; rc=$?
ok '[ $rc = 2 ] && [ "$(calls | awk "/^srv usb del/{print \$4}" | xargs)" = "stick-id reader-id" ]' 'empty reader: an early failure releases both attached devices'
rm -f "$F/prepare-fails"
printf '%s\n' 'usb rb-win-template same-id' 'usb-reader rb-win-template same-id' > "$T/vm-run.conf"
reset
vr run 'clean Windows' "$T/suites/reader.suite"; rc=$?
ok '[ $rc = 2 ] && grep -q "same device id" "$T/out" && ! calls | grep -qE "^(snapshot-switch|srv)"' 'empty reader: a shared device ID is rejected before the revert'
rm -f "$T/vm-run.conf"

# ---- the release architecture matrix
ok '(for s in platform startup; do for a in arm64 x64-prism; do grep -Eq "^$s +$a +clean Windows$" "$here/tests/win/suites/release.list" || exit 1; done; done)' 'Prism: the release matrix requires native and Prism records for platform and startup'
printf '%s\n' 'gamma arm64 clean Windows' 'gamma x64-prism clean Windows' > "$RS/release.list"
git -C "$R" add -A && git -C "$R" -c user.email=t@t -c user.name=t commit -q -m matrix
C="$(git -C "$R" rev-parse HEAD)"
DT="$T/share/dist-test"
rm -rf "$DT"; mkdir "$DT"
printf '{"payload_version":"1.5.0","commit":"%s"}\n' "$C" > "$DT/payload.json"
cp "$DT/payload.json" "$R/dist-test/payload.json"
"$R/tests/win/first-run.sh" --dry-run > "$T/out" 2>&1; rc=$?
ok '[ $rc = 0 ] && grep -q "gamma.suite --arch x64-prism --record" "$T/out"' 'Prism: the first-run driver selects the release row architecture'
vr stage --build; rc=$?
ok '[ $rc = 0 ] && grep -qx "$(printf "forced\n" | shasum -a 256 | cut -c1-64) vm/x64/forced-shell.exe" "$T/share/vm/shas.txt"' 'Prism: stage --build stages and hashes the forced shell'
reset; echo ARM64 > "$F/arch"
vr run 'clean Windows' gamma --arch arm64 --record; arm_rc=$?
vr run 'clean Windows' gamma --arch x64-prism --record; prism_rc=$?
g="$T/gates/$C/vm-win"; base="$g/gamma@11111111-aaaa-bbbb-cccc-000000000001"
res="$(last_results)"
job="$T/share/job/$(basename "$res")"
ok '[ "$arm_rc:$prism_rc" = 0:0 ] && [ -f "$base-arm64.ok" ] && grep -qx "arch arm64" "$base-x64-prism.ok" && grep -qx "lane x64-prism" "$base-x64-prism.ok" && grep -q "TestArch = '\''x64'\''" "$job/1-one.ps1"' 'Prism: ARM64 and forced x64 records coexist and x64 reaches the test prelude'
vr check "$C" "$DT"; rc=$?
ok '[ $rc = 0 ] && grep -q "passed (2 rows" "$T/out"' 'Prism: check accepts the two distinct ARM64 and Prism passes'
sed 's/^arch arm64$/arch x64/' "$base-x64-prism.ok" > "$T/prism-rec"
mv "$T/prism-rec" "$base-x64-prism.ok"
vr check "$C" "$DT"; rc=$?
ok '[ $rc = 1 ] && grep -q "isn.t an x64 run on ARM64" "$T/out"' 'Prism: check rejects a Prism record claiming an x64 guest'
reset; echo AMD64 > "$F/arch"
vr run 'clean Windows' gamma --arch x64-prism; rc=$?
ok '[ $rc = 2 ] && grep -q "cannot run on this VM (x64)" "$T/out" && ! calls | grep -q "1-one.ps1"' 'Prism: an x64 guest cannot run the Prism lane'

TMPDIR="$T" bash "$here/tests/win/update-fixtures-test.sh" > "$T/fixture-build.out" 2>&1; rc=$?
ok '[ "$rc" = 0 ] && grep -q "^PASS IN3/IN5:" "$T/fixture-build.out"' "IN3/IN5 fixture builder ($(tail -1 "$T/fixture-build.out"))"

# ---- the published bootstrap path on the TEST server
printf '%s\n' 'published bootstrap fixture' > "$T/share/dist-test/bootstrap.ps1"
printf '%s\n' '#fake: get releases/latest/download/bootstrap.ps1' \
  '#fake: get releases/latest/download/sub/bootstrap.ps1' > "$T/suites/s/published.ps1"
printf '%s\n' 'need http' 'sys 60 published s/published.ps1' > "$T/suites/published.suite"
reset; echo ARM64 > "$F/arch"
vr run 'clean Windows' "$T/suites/published.suite"; rc=$?
res="$(last_results)"
ok '[ $rc = 0 ] && grep -qx "published bootstrap fixture" "$res/1-published.log" && grep -qx "GET-REFUSED releases/latest/download/sub/bootstrap.ps1" "$res/1-published.log"' 'published bootstrap: the server maps the exact release asset path to the TEST file'
if command -v pwsh > /dev/null; then
  cat > "$T/published.ps1" <<'PS'
param([string]$Suites)
$VmRun = @{ Url = 'http://10.211.55.2:8766/'; Step = 'published' }
. (Join-Path $Suites 'bootstrap/published.ps1')
$capture = @{ Base = $null }
function Get-Layout { @{} }
function Get-ItemProperty { @{ VerifiedAndReputablePolicyState = 2 } }
function Invoke-OneLiner([string]$base) {
    $capture.Base = $base
    throw 'mock one-liner'
}
try { & (Join-Path $Suites 'bootstrap/install.ps1') }
catch { if ($_.Exception.Message -ne 'mock one-liner') { throw } }
if ($capture.Base -ceq 'https://10.211.55.2:8766/releases/latest/download/') { exit 0 }
"unexpected bootstrap base: $($capture.Base)"
exit 1
PS
  pwsh -NoProfile -NonInteractive -File "$T/published.ps1" "$here/tests/win/suites" > "$T/published.out" 2>&1; rc=$?
  ok '[ $rc = 0 ]' "published bootstrap: the installer calls the one-liner at the published HTTPS base ($(tail -1 "$T/published.out"))"
fi

echo "vm-run.sh: $pass passed, $fail failed"
[ "$fail" = 0 ]

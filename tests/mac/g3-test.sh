#!/bin/bash
# tests/mac/g3.sh's selftest, on the host in seconds, no VM (make check; g3.sh selftest): a fake
# TEST build, plan and gate archive; tart, vm.sh and the download server stubbed (G3_TART, G3_VM,
# G3_SERVE_STUB). The dry run (the scenarios per plan, refused inputs, missing templates, no VM
# touched), the run (the record, the order of the steps, every clone deleted, the owner's clicks
# when Accessibility is refused, a failing step, a step stopped at the time limit, --only never
# passing), release.sh's check (a stub's record, another plan or payload refused), every wait at
# most 10 minutes, g3ax.swift and g3-guest.sh build and parse.
set -uo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
g3="$root/tests/mac/g3.sh"
t="$(mktemp -d "${TMPDIR:-/tmp}/rbsp-g3-test.XXXXXX")"
[ -n "${G3_TEST_KEEP:-}" ] && echo "kept: $t" || trap 'rm -rf "${t:?}"' EXIT      # G3_TEST_KEEP=1: the folder kept
pass=0; fail=0
ok() { if eval "$1"; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "FAIL: $2"; fi; }
sha() { shasum -a 256 "$1" | cut -c1-64; }

# ---- fixtures: a TEST build (1.4.1, app new), a carried one (1.4.0's app), the gate archive ----
commit=1111111111111111111111111111111111111111; ccommit=2222222222222222222222222222222222222222
mkbuild() {   # mkbuild DIR COMMIT APPV BRIDGE [notest]; RELV=X: payload_version X (a carried app's release)
  local d="$1"; mkdir -p "$d/x/RB Stems Plus.app/Contents/MacOS"
  if [ "${5:-}" = notest ]; then echo "a release" > "$d/x/RB Stems Plus.app/Contents/MacOS/RB Stems Plus"
  else printf 'TEST BUILD rbstemsplus-probe\n' > "$d/x/RB Stems Plus.app/Contents/MacOS/RB Stems Plus"; fi
  (cd "$d/x" && zip -qr ../rbstemsplus-app.zip "RB Stems Plus.app"); rm -rf "$d/x"
  echo "$4" > "$d/libonnxruntime.1.18.0.dylib"; echo model > "$d/stemsplus-model.onnx"; echo boot > "$d/bootstrap.sh"; echo sig > "$d/payload.json.sig"
  printf '{"payload_version":"%s","commit":"%s","app":{"version":"%s"},"bridge":{"sha256":"%s"},"model":{"sha256":"m0"}}\n' "${RELV:-$3}" "$2" "$3" "$4" > "$d/payload.json"
}
mkbuild "$t/build" "$commit" 1.4.1 bnew
RELV=1.4.1 mkbuild "$t/cbuild" "$ccommit" 1.4.0 bnew
A="$t/archive"
for v in 1.0.0:bold 1.3.0:bnew 1.3.1:bnew 1.4.0:bnew; do mkbuild "$A/tag-v${v%%:*}" 0000000000000000000000000000000000000000 "${v%%:*}" "${v##*:}"; done
RELV=1.4.1 mkbuild "$A/tag-v1.4.1" 0000000000000000000000000000000000000000 1.4.0 bnew     # a release that carried app 1.4.0
(. "$root/tests/perf/archive.sh" && seal) || { echo "FAIL: the archive couldn't be sealed"; exit 1; }
G="$t/gates"; mkdir -p "$G/$commit" "$G/$ccommit"
printf 'plan 1\ncommit %s\nversion 1.4.1\nprevious v1.4.0 x\nmac.app new\nmac.bridge carry c x\nwindows.app none\n' "$commit" > "$G/$commit/plan"
printf 'plan 1\ncommit %s\nversion 1.4.1\nprevious v1.4.0 x\nmac.app carry 1.4.0 c x\nmac.bridge carry c x\nwindows.app none\n' "$ccommit" > "$G/$ccommit/plan"

# ---- stubs ----
S="$t/state"; mkdir -p "$S/vm"
cat > "$t/tart" <<'EOF'
#!/bin/bash
# tart's stub: the templates (stopped) and the clones vm.sh's stub made (running)
[ "$1" = list ] || exit 1
[ -n "${STUB_TART_FAIL:-}" ] && exit 1
[ -n "${STUB_TART_SLEEP:-}" ] && sleep "$STUB_TART_SLEEP"
# a list that hangs once a delete was asked for (clone_down's confirmation)
[ -n "${STUB_TART_SLOW_AFTER_DELETE:-}" ] && grep -q "^delete" "${STUB_LOG:-$G3_STATE/vm.log}" 2>/dev/null && sleep 30
echo "Source Name Disk Size Accessed State"
for v in ${STUB_TEMPLATES-rb26-template rb12-template} ${STUB_EXTRA_VMS:-}; do echo "local $v 50 33 now stopped"; done
cl="${STUB_CLONES:-$G3_STATE/stub-clones}"
[ -f "$cl" ] && while read -r c st; do echo "local $c 50 33 now $st"; done < "$cl"
exit 0
EOF
cat > "$t/vm.sh" <<'EOF'
#!/bin/bash
# vm.sh's stub: the calls into STUB_LOG; the guest's steps answered as STUB_* say. Its state is
# G3_STATE, else its own checkout's build/harness, as vm.sh's is
export LC_ALL=C
st="${G3_STATE:-$(cd "$(dirname "$0")/../.." && pwd)/build/harness}"; mkdir -p "$st"
log="${STUB_LOG:-$st/vm.log}"; cl="${STUB_CLONES:-$st/stub-clones}"
case "$1" in
up) echo "up $2 ${TEMPLATE:-}${3:+ $3}" >> "$log"; echo "${TEMPLATE:-}" > "$st/stub-template"
    echo "$2" > "$st/clone"; grep -q "^$2 " "$cl" 2>/dev/null || echo "$2 running" >> "$cl"
    # --vnc: tart's VNC server, its full URL (with the password) in vnc-url, the address alone in
    # vm.sh up's output (STUB_NO_VNC_URL: none)
    rm -f "$st/vnc-url"
    if [ "${3:-}" = --vnc ] && [ -z "${STUB_NO_VNC_URL:-}" ]; then echo "vnc://:stub-pass@127.0.0.1:5999" > "$st/vnc-url"; fi
    # vm.sh up's exit 3: the template's MAC address not held (the clone made, stopped)
    [ -n "${STUB_UP_MAC:-}" ] && { echo "vm.sh: $2 stopped: its en0 is 0e:00:00:00:00:01, not rb26-template's MAC address f2:29:15:0c:4b:bc" >&2; exit 3; }
    # a slow up (GR2): its pid and its child's, kept for the selftest
    if [ -n "${STUB_SLOW_UP:-}" ]; then echo $$ > "$st/up.pid"; sleep "$STUB_SLOW_UP" & echo $! > "$st/upchild.pid"; wait; fi
    [ -s "$st/vnc-url" ] && echo "vnc: vnc://127.0.0.1:5999"
    exit 0 ;;
delete)
  c="$(cat "$st/clone" 2>/dev/null)"; echo "delete $c" >> "$log"
  [ -n "${STUB_DELETE_FAIL:-}" ] && exit 1
  # a delete that says it worked while the VM stays
  [ -n "${STUB_DELETE_KEEP:-}" ] && exit 0
  grep -v "^$c " "$cl" > "$cl.n"; mv "$cl.n" "$cl"; rm -f "$st/clone"
  # another lane starts a VM meanwhile
  [ -n "${STUB_OTHER_VM:-}" ] && echo "other-lane running" >> "$cl"
  exit 0 ;;
build) echo build >> "$log" ;;
down) echo "down $(cat "$st/clone" 2>/dev/null)" >> "$log" ;;
sh)
  step="$(sed -n 's|.*g3-guest\.sh \([a-z0-9-]*\) .*|\1|p' <<< "$2")"
  run="$(sed -n 's|.*g3-guest\.sh [a-z0-9-]* /Users/admin/xfer/\([^ ]*\).*|\1|p' <<< "$2")"
  [ -n "$step" ] || { echo "push" >> "$log"; exit 0; }
  echo "sh $step $(sed -e 's|.*g3-guest\.sh [a-z0-9-]* [^ ]* *||' -e 's/ *$//' <<< "$2")" >> "$log"
  [ "$step" = "${STUB_SLEEP:-}" ] && sleep 5
  # another lane's vm.sh up moves the pointer
  [ "$step" = "${STUB_STEAL:-}" ] && echo other-lane > "$st/clone"
  # a step whose child ignores TERM and keeps the output open
  if [ "$step" = "${STUB_HANG:-}" ]; then bash -c 'trap "" TERM; exec sleep 60' & trap '' TERM; sleep 60; fi
  [ "$step" = "${STUB_LATE:-}" ] && sleep 3
  if [ "$step" = "${STUB_FAIL:-}" ]; then echo "FAIL stub: $step" >> "$st/xfer/$run/result"; exit 1; fi
  # rekordbox's licence text seen (g3-guest.sh's rb_guard): a STOP line, exit 4
  if [ "$step" = "${STUB_STOP:-}" ]; then echo "STOP rekordbox's automatic activation notice (stub)" >> "$st/xfer/$run/result"; exit 4; fi
  case "$step" in
    facts) [ "$(cat "$st/stub-template")" = rb12-template ] && m=12.7.6 || m=26.6.2
           printf 'macos %s\nsip enabled\nconsole dj\naccessibility %s\n' "$m" "${STUB_AX:-yes}" ;;
    ax-grant) echo "refused (stub)"; exit 1 ;;
    dialog) [ "${STUB_AX:-yes}" = yes ] || exit 1 ;;
    faillogs) mkdir -p "$st/xfer/$run/fail-logs"; echo "the app's log" > "$st/xfer/$run/fail-logs/app.log" ;;
    ax) [ "${STUB_AX:-yes}" = yes ] || { echo "FAIL no Accessibility permission"; exit 3; }
        # g3ax's answers: STUB_AX_NOPRESS=TITLE (or "menu"): that press "didn't take" (a modal
        # dialog's); STUB_AX_NOFIND=TITLE: no such button
        eval "set -- $(sed 's|.*g3-guest\.sh ax [^ ]* *||' <<< "$2")"; v="$1"; shift 3; done_=""
        if [ "$v" = menu ]; then
          [ "${STUB_AX_NOPRESS:-}" = menu ] && { echo "FAIL the menu item didn't take the press"; exit 1; }
          echo "ok pressed: $1"; exit 0
        fi
        for x in "$@"; do x="${x%\?}"
          [ "$x" = "${STUB_AX_NOFIND:-}" ] && { echo "FAIL no enabled \"$x\" within 30000 ms (the window's texts follow)"; echo "Some text"; exit 1; }
          [ "$x" = "${STUB_AX_NOPRESS:-}" ] && { echo "FAIL \"$x\" didn't take the press"; printf '%s' "$done_"; exit 1; }
          done_+="pressed: $x"$'\n'
        done
        echo ok; printf '%s' "$done_"; exit 0 ;;
  esac ;;
esac
exit 0
EOF
printf '#!/bin/bash\necho "serve $1" >> "${STUB_LOG:-$G3_STATE/vm.log}"\n' > "$t/serve"
chmod +x "$t/tart" "$t/vm.sh" "$t/serve"
# the G3 Driver, built once here (g3.sh builds it only when the source is newer)
D="$S/vm/G3 Driver.app/Contents/MacOS"; mkdir -p "$D" "$t/sc"
xcrun swiftc -module-cache-path "$t/sc" -sdk "$(xcrun --sdk macosx --show-sdk-path)" -target arm64-apple-macos12.0 "$root/tests/mac/g3ax.swift" -o "$D/g3ax" > "$t/swiftc.log" 2>&1
ok '[ -x "$D/g3ax" ]' "g3ax.swift builds for macOS 12 (arm64)"
"$D/g3ax" "$t/ax.out" trusted > /dev/null; rc=$?
ok '[ "$rc" = 0 ] && grep -Eq "^ok (yes|no)$" "$t/ax.out"' "g3ax trusted answers in its OUT file"
"$D/g3ax" "$t/ax2.out" rosetta get /x.app > /dev/null; rc=$?
ok '[ "$rc" != 0 ] && ! grep -q "^ok" "$t/ax2.out"' "g3ax: no rosetta verb (Intel Macs aren't tested)"
touch "$S/vm/vmtool"; chmod +x "$S/vm/vmtool"

g3e() { env G3_TART="$t/tart" G3_VM="$t/vm.sh" G3_SERVE_STUB="$t/serve" G3_STATE="$S" G3_GATES="$G" GATES_ARCHIVE="$A" "$@"; }
r3() { : > "$S/vm.log"; rm -f "$S/stub-clones" "$S/clone" "$S/g3-leftover"; g3e "$@" "$g3" ${args[@]+"${args[@]}"} > "$t/out" 2>&1; rc=$?; }

# ---- usage and limits ----
args=(); r3; ok '[ "$rc" = 2 ]' "no command: usage, exit 2"
args=(bogus); r3; ok '[ "$rc" = 2 ]' "an unknown command: exit 2"
args=(plan --build "$t/build"); r3 G3_LIMIT=900
ok '[ "$rc" = 2 ] && grep -q "at most 600" "$t/out"' "G3_LIMIT over 10 minutes refused"
args=(check deadbeef); r3; ok '[ "$rc" = 2 ]' "check needs a full commit"

# ---- the dry run ----
args=(plan --build "$t/build"); r3
ok '[ "$rc" = 0 ] && grep -q "dry run: no VM touched" "$t/out" && [ ! -s "$S/vm.log" ]' "plan: exit 0, no VM touched"
ok 'grep -q "^starting points: v1.0.0 v1.3.0 v1.3.1 v1.4.0;" "$t/out"' "plan: v1.0.0, v1.3.0, the archived 1.3.x and the plan's previous release"
ok 'grep -q "^- rb26-template: bootstrap$" "$t/out" && ! grep -q "rb12-template" "$t/out"' "plan: the bootstrap, on the macOS 26 template only"
ok '[ "$(grep -c "^- rb26-template: update from" "$t/out")" = 4 ]' "plan: an update from every starting point"
ok '! grep -qiE "x86|rosetta|translated" "$t/out"' "plan: no Rosetta scenario (Intel Macs aren't tested)"
ok 'grep -A2 "update from v1.0.0" "$t/out" | grep -q "offer: expected" && grep -A2 "rb26-template: update from v1.3.0" "$t/out" | grep -q "offer: none"' "plan: the Stems Cache offer only where the bridge changed"
ok '! grep -q carried "$t/out"' "plan: no carried scenario when the app is new"
args=(plan --build "$t/cbuild"); r3
ok '[ "$rc" = 0 ] && grep -q "^- rb26-template: carried from v1.4.0$" "$t/out" && grep -q "^- rb26-template: carried-update from v1.0.0$" "$t/out" && ! grep -q ": update from" "$t/out"' "plan, the app carried: the previous release and 1.0.0's update"
args=(plan --build "$t/build"); r3 STUB_TEMPLATES=""
ok '[ "$rc" = 1 ] && grep -q "MISSING templates: rb26-template" "$t/out" && grep -q "asking the owner" "$t/out"' "plan: a missing template is named, exit 1 (no download)"
args=(run --build "$t/build"); r3 STUB_TEMPLATES=""
ok '[ "$rc" = 2 ] && [ ! -s "$S/vm.log" ]' "run: a missing template stops before any VM"
mkbuild "$t/dirty" "$commit-dirty" 1.4.1 bnew
args=(plan --build "$t/dirty"); r3; ok '[ "$rc" = 2 ] && grep -q "no -dirty" "$t/out"' "plan: a -dirty TEST build refused"
mkbuild "$t/rel" "$commit" 1.4.1 bnew notest
args=(plan --build "$t/rel"); r3; ok '[ "$rc" = 2 ] && grep -q "isn.t a TEST build" "$t/out"' "plan: a release build refused (no probe, no TEST BUILD)"
args=(plan --build "$t/build" --plan "$G/$ccommit/plan"); r3; ok '[ "$rc" = 2 ] && grep -q "isn.t the plan of" "$t/out"' "plan: another commit's plan refused"
cp "$A/tag-v1.3.0/payload.json" "$t/p.bak"; echo " " >> "$A/tag-v1.3.0/payload.json"
args=(plan --build "$t/build"); r3; ok '[ "$rc" = 2 ] && grep -q "doesn.t match its SHA256SUMS" "$t/out"' "plan: a gate archive that doesn't match its checksums stops"
cp "$t/p.bak" "$A/tag-v1.3.0/payload.json"
args=(plan --build "$t/build" --from "v1.2.0"); r3; ok '[ "$rc" = 2 ] && grep -q "isn.t in the gate archive" "$t/out"' "plan: a starting point not archived refused"

# ---- the run ----
args=(run --build "$t/build"); r3
rec="$G/$commit/g3-mac.ok"
ok '[ "$rc" = 0 ] && [ -f "$rec" ] && [ ! -e "$G/$commit/g3-mac.failed" ]' "run: every scenario passing writes g3-mac.ok, exit 0"
ok 'grep -qx "commit $commit" "$rec" && grep -qx "payload_sha256 $(sha "$t/build/payload.json")" "$rec" && grep -qx "plan_sha256 $(sha "$G/$commit/plan")" "$rec"' "the record: the commit, the TEST payload's and the plan's SHA-256"
ok '[ "$(grep -c "^scenario .* pass " "$rec")" = 5 ] && ! grep -q "^scenario .* FAIL" "$rec"' "the record: 5 scenarios, each passed"
ok 'grep -qx "coverage ok" "$rec" && grep -qx "result pass" "$rec" && grep -q "^owner_clicks 0$" "$rec" && grep -q "^stub" "$rec"' "the record: coverage, result, no owner click, marked as a stub's"
ok 'grep -q "^guest_versions rb26-template=26.6.2$" "$rec"' "the record: the guest's macOS"
ok '[ "$(grep -c "^up " "$S/vm.log")" = 5 ] && [ "$(grep -c "^delete g3-" "$S/vm.log")" = 5 ] && [ "$(LC_ALL=C sed -n "s/^up \([^ ]*\) .*/\1/p" "$S/vm.log" | sort -u | wc -l | tr -d " ")" = 5 ]' "run: one fresh clone per scenario, each with its own name, each deleted"
ok '[ ! -s "$S/stub-clones" ] && [ ! -e "$S/g3-leftover" ]' "run: no clone left"
ok '! grep -q "^sh faillogs" "$S/vm.log" && ! ls -d "$G/$commit"/g3-fail-* > /dev/null 2>&1' "run: every scenario passing, no guest logs kept"
ok '[ "$(grep -cF "OWNER: G3: open vnc://127.0.0.1:5999 in Screen Sharing now (password: $S/vnc-url); click Allow on RB Stems Plus Watcher when macOS asks (update-v1.4.0 clone)" "$t/out")" = 1 ]' "the owner is told once, with the clone's VNC address (the password left in vnc-url), as the core paths' update scenario starts"
ok '[ "$(grep -c "^up .* --vnc$" "$S/vm.log")" = 5 ] && [ "$(grep -c "^up " "$S/vm.log")" = 5 ] && ! grep -q "^down" "$S/vm.log" && ! grep -q "stub-pass" "$t/out"' "screens: every clone with VNC (no window), none restarted; the password never in the output"
ok 'LC_ALL=C sed -n "s/^up \([^ ]*\) .*/\1/p" "$S/vm.log" | grep -qE "^g3-[0-9]{14}-[0-9]+-[0-9]+$"' "clone names: the date to the second, the run's pid, a counter"
ok '! grep -q "rb12-template" "$S/vm.log"' "run: the macOS 26 template only"
args=(run --build "$t/build" --guests "rb12-template"); r3
ok '[ "$rc" = 1 ] && grep -q "^coverage the guests: a guest.s version isn.t macOS 26 or later" "$G/$commit/g3-mac.failed" && [ ! -e "$rec" ]' "run on a macOS 12 guest: never a pass (macOS 26 only)"
args=(run --build "$t/build"); r3
seq_of() { LC_ALL=C awk -v s="$1" '/^up /{n++} n==s' "$S/vm.log" | LC_ALL=C sed -n 's/^\(sh [a-z0-9-]*\).*/\1/p; s/^serve .*\/\(tag-\|g3-serve\/\)\{0,1\}\([^/]*\)$/serve \2/p' | tr '\n' ' '; }
ok '[ "$(seq_of 2)" = "sh facts sh prepare serve v1.0.0 sh bootstrap sh emulate sh ax sh cond sh before-update serve build sh switch sh cond sh ax sh ax sh cond sh after-update sh offer sh manage sh manage sh uninstall " ]' "update from 1.0.0: the steps in order ($(seq_of 2))"
ok 'grep -q "^sh offer 1" "$S/vm.log" && grep -q "^sh offer 0" "$S/vm.log"' "the offer expected from 1.0.0 (another bridge), none from 1.3.0"
# the switch-over's core paths (the report's lights, the kept-sets check over two sessions, a queued
# action): once, in the last starting point's update scenario, after Manage and before Uninstall
ok '[ "$(grep -c "^sh lights" "$S/vm.log")" = 1 ] && [ "$(grep -c "^sh kept 1$" "$S/vm.log")" = 1 ] && [ "$(grep -c "^sh kept 2$" "$S/vm.log")" = 1 ] && [ "$(grep -c "^sh kept" "$S/vm.log")" = 2 ] && [ "$(grep -c "^sh queued" "$S/vm.log")" = 1 ]' "the core's paths: once per run's guest (kept as two steps, one a session)"
ok '(for i in 1 2 3 4 5; do case "$(seq_of $i)" in *"sh manage sh manage sh lights sh kept sh kept sh queued sh uninstall "*) exit 0 ;; esac; done; exit 1)' "the core's paths: after Manage, before Uninstall, in one update scenario"
ok '! LC_ALL=C grep -qE "^sh (rosetta|rosetta-pref|x86-install|x86-uninstall)|x86" "$S/vm.log"' "run: no Rosetta step anywhere"
ok '[ "$(seq_of 1)" = "sh facts sh prepare serve build sh bootstrap sh quit sh new-start " ]' "bootstrap: the new build's bootstrap, then the app's start ($(seq_of 1))"
ok '[ -d "$G/$commit/g3/2-rb26-template-update-v1.0.0" ] && grep -q "^ok .*(Accessibility)" "$G/$commit/g3/2-rb26-template-update-v1.0.0/result"' "the scenarios' results kept with the record"

# release.sh's check
args=(check "$commit" "$t/build"); r3
ok '[ "$rc" = 1 ] && grep -q "a selftest.s record" "$t/out"' "check: a stub's record never counts"
grep -v '^stub' "$rec" > "$t/r" && mv "$t/r" "$rec"
args=(check "$commit" "$t/build"); r3
ok '[ "$rc" = 0 ] && grep -q "passed for $commit (5 scenarios" "$t/out"' "check: a passing record of this commit, payload and plan counts"
args=(check "$commit" "$t/cbuild"); r3
ok '[ "$rc" = 1 ] && grep -q "another TEST payload" "$t/out"' "check: another TEST payload refused"
cp "$G/$commit/plan" "$t/plan.bak"; echo "# changed" >> "$G/$commit/plan"
args=(check "$commit" "$t/build"); r3
ok '[ "$rc" = 1 ] && grep -q "the plan changed" "$t/out"' "check: a plan made again since the run refused"
cp "$t/plan.bak" "$G/$commit/plan"
args=(check "$ccommit" "$t/cbuild"); r3
ok '[ "$rc" = 1 ] && grep -q "no G3 record" "$t/out"' "check: no record, refused"
cp "$rec" "$t/good.ok"
grep -v "^scenario rb26-template update-v1.3.1 " "$t/good.ok" > "$rec"
args=(check "$commit" "$t/build"); r3
ok '[ "$rc" = 1 ] && grep -q "required scenarios not passed: rb26-template update-v1.3.1" "$t/out"' "check: a record with one scenario row deleted is refused (G31)"
sed 's/^\(scenario rb26-template update-v1.3.0\) pass /\1 FAIL /' "$t/good.ok" > "$rec"
args=(check "$commit" "$t/build"); r3
ok '[ "$rc" = 1 ] && grep -q "a scenario failed" "$t/out"' "check: a failed scenario row is refused"
# GR1: the guests a record names (macOS 26 only): each edit below is refused
ok 'grep -qx "guest_versions rb26-template=26.6.2" "$t/good.ok" && ! grep -q "^host_macos" "$t/good.ok"' "the record: guest_versions, no host_macos"
gr1() {   # gr1 NAME SED…: the good record edited, refused by check with REASON
  local name="$1" why="$2"; shift 2
  sed "$@" "$t/good.ok" > "$rec"
  args=(check "$commit" "$t/build"); r3
  ok '[ "$rc" = 1 ] && grep -q "$why" "$t/out"' "GR1: $name refused ($why)"
}
gr1 "whitespace-only guests" "no guests" -e 's/^guests .*/guests    /' -e '/^scenario /d'
gr1 "a version below macOS 26" "isn.t macOS 26 or later" -e 's/^guest_versions .*/guest_versions rb26-template=25.4/'
gr1 "a non-numeric version" "isn.t macOS 26 or later" -e 's/^guest_versions .*/guest_versions rb26-template=?/'
gr1 "guest_versions naming a guest guests lacks" "doesn.t name exactly the guests" -e 's/^guest_versions .*/& rb27-template=26.1/'
# GS1: the host's own macOS doesn't matter (macOS 26 only is tested): a run on a macOS 27 host passes
mkdir -p "$t/bin27"; printf '#!/bin/bash\necho 27.0\n' > "$t/bin27/sw_vers"; chmod +x "$t/bin27/sw_vers"
args=(run --build "$t/build"); r3 PATH="$t/bin27:$PATH"
ok '[ "$rc" = 0 ] && grep -qx "coverage ok" "$rec" && grep -qx "result pass" "$rec"' "GS1: a run on a macOS 27 host: coverage ok, result pass"
sed '/^stub/d' "$rec" > "$t/r27" && mv "$t/r27" "$rec"; args=(check "$commit" "$t/build"); r3 PATH="$t/bin27:$PATH"
ok '[ "$rc" = 0 ]' "GS1: its record passes check"
# two guests: either one edited out of guests with its rows is refused (guest_versions still names it)
args=(run --build "$t/build" --guests "rb26-template rb26b-template"); r3 STUB_TEMPLATES="rb26-template rb26b-template"
sed '/^stub/d' "$rec" > "$t/two.ok"
cp "$t/two.ok" "$rec"; args=(check "$commit" "$t/build"); r3
ok '[ "$rc" = 0 ]' "a two-guest record passes"
for gg in rb26-template rb26b-template; do
  other="rb26-template"; [ "$gg" = rb26-template ] && other="rb26b-template"
  sed -e "s/^guests .*/guests $other/" -e "/^scenario $gg /d" "$t/two.ok" > "$rec"
  args=(check "$commit" "$t/build"); r3
  ok '[ "$rc" = 1 ] && grep -q "doesn.t name exactly the guests" "$t/out"' "GR1: $gg and its rows edited out: refused"
done
cp "$t/good.ok" "$rec"
args=(check "$commit" "$t/build"); r3
ok '[ "$rc" = 0 ]' "the good record still passes"

# the app carried (G32, G37, G48): release 1.4.1, app 1.4.0; the payload proven, both numbers passed
args=(run --build "$t/cbuild"); r3
crec="$G/$ccommit/g3-mac.ok"
ok '[ "$rc" = 0 ] && [ -f "$crec" ] && grep -qx "version 1.4.1 (Mac app 1.4.0)" "$crec" && [ "$(grep -c "^scenario .* pass " "$crec")" = 3 ]' "carried run: 3 scenarios pass, the record names release and app"
ok '[ "$(grep -c "^sh carried 1.4.1 1.4.0 0$" "$S/vm.log")" = 2 ] && [ "$(grep -c "^sh payload $(sha "$t/cbuild/payload.json")$" "$S/vm.log")" = 2 ]' "carried: the payload proven, then the display checks with release 1.4.1 and app 1.4.0, on both paths"
ok 'grep -q "^sh bootstrap http://192.168.64.1:8765/ 1.4.0$" "$S/vm.log" && grep -q "^sh cond notice 1.4.0$" "$S/vm.log" && grep -q "^sh cond updated 1.4.0$" "$S/vm.log" && grep -q "^sh after-update 1.4.0$" "$S/vm.log"' "carried: the notice, the update and Info.plist checked against the app's 1.4.0"
ok '[ "$(LC_ALL=C sed -n "/^up .*/,\$p" "$S/vm.log" | LC_ALL=C awk "/^up /{n++} n==2" | LC_ALL=C sed -n "s/^\(sh [a-z-]*\).*/\1/p" | tr "\n" " ")" = "sh facts sh prepare sh bootstrap sh emulate sh ax sh cond sh switch sh payload sh carried " ]' "carried from the previous release: switch, the payload proven, then the checks"
sed '/^stub/d' "$crec" > "$t/c" && mv "$t/c" "$crec"
args=(check "$ccommit" "$t/cbuild"); r3
ok '[ "$rc" = 0 ]' "check: the carried record counts"
# G39: an archived release that carried its app is a starting point; its bootstrap installs app 1.4.0
args=(run --build "$t/build" --from "v1.4.1" --only "update-v1.4.1"); r3
ok '! grep -q "holds release" "$t/out" && grep -q "^sh bootstrap http://192.168.64.1:8765/ 1.4.0$" "$S/vm.log"' "a starting point v1.4.1 carrying app 1.4.0: accepted, its app version expected"
# G40: post-publish of a release carrying its app: the notice, the update and the one-liner expect app 1.4.0
args=(post-publish 1.4.1); r3 G3_POST_PAYLOAD="$t/cbuild/payload.json"
ok '[ "$rc" = 0 ] && grep -q "^sh cond notice 1.4.0$" "$S/vm.log" && grep -q "^sh cond version 1.4.0$" "$S/vm.log" && grep -q "^sh bootstrap real 1.4.0$" "$S/vm.log" && grep -qx "result pass" "$G/post-1.4.1.txt"' "post-publish 1.4.1 (app 1.4.0): the published payload's app version expected"
ok '[ "$(grep -c "^up .* --vnc$" "$S/vm.log")" = 2 ] && [ "$(grep -c "^up " "$S/vm.log")" = 2 ]' "post-publish: both clones with VNC (no window)"
args=(post-publish 1.4.1); r3 G3_POST_PAYLOAD="$t/cbuild/payload.json" STUB_AX=no
ok '[ "$(LC_ALL=C grep "^up \|^down" "$S/vm.log" | sed "s/ g3-[^ ]*//" | tr "\n" "|")" = "up rb26-template --vnc|up rb26-template --vnc|" ] && [ "$(grep -c "the old app.s clicks are yours" "$t/out")" = 1 ]' "post-publish without Accessibility: no restart; the update's clicks sent to the owner, the one-liner's clone not"
args=(post-publish 1.4.2); r3 G3_POST_PAYLOAD="$t/cbuild/payload.json"
ok '[ "$rc" = 2 ] && ! grep -q "^up" "$S/vm.log"' "post-publish: a payload of another release stops before any VM"

# a narrowed --from (G31): coverage partial, never a pass; a forged pass on it is refused by check
args=(run --build "$t/build" --from "v1.3.0"); r3
ok '[ "$rc" = 1 ] && grep -qx "coverage partial (--from omits v1.0.0 v1.3.1 v1.4.0)" "$G/$commit/g3-mac.failed" && [ ! -e "$rec" ]' "run --from leaving out required starting points: coverage partial, no .ok"
sed -e '/^stub/d' -e 's/^coverage .*/coverage ok/' -e 's/^result .*/result pass/' "$G/$commit/g3-mac.failed" > "$rec"
args=(check "$commit" "$t/build"); r3
ok '[ "$rc" = 1 ] && grep -q "required scenarios not passed: .*update-v1.0.0" "$t/out"' "check recomputes the required scenarios: a narrowed run's forged pass is refused"
args=(run --build "$t/build" --from "v1.0.0 v1.3.0 v1.3.1 v1.4.0 v1.3.0"); r3
ok '[ "$rc" = 0 ] && [ -f "$rec" ]' "extra --from points are allowed (a full list passes)"
# a required 1.3.x tag that isn't archived stops the plan (G47)
mv "$A/tag-v1.3.1" "$t/tag-v1.3.1.away"; (cd "$A" && grep -v "tag-v1.3.1/" SHA256SUMS > S && mv S SHA256SUMS)
args=(plan --build "$t/build"); r3
ok '[ "$rc" = 0 ] && ! grep -q "v1.3.1" "$t/out"' "an archived 1.3.x that isn't a tag is required only while archived"
args=(plan --build "$t/build"); r3 G3_TAGS="v1.3.0 v1.3.1"
ok '[ "$rc" = 2 ] && grep -q "the starting point v1.3.1 (required) isn.t in the gate archive" "$t/out"' "a shipped 1.3.x tag with no archived TEST build stops the plan (G47)"
mv "$t/tag-v1.3.1.away" "$A/tag-v1.3.1"; (. "$root/tests/perf/archive.sh" && seal)

# --maint and G3_ROOT (the maintenance branch, T29): its plan written (app and bridge new, the
# previous release the newest tag below it), then the run and check read that checkout's data
mr="$t/mrepo"; git init -q "$mr" && git -C "$mr" -c user.name=t -c user.email=t@x.invalid commit -q --allow-empty -m x \
  && git -C "$mr" tag v1.3.0 && git -C "$mr" tag v1.3.1 && git -C "$mr" tag v1.4.0
mcommit=3333333333333333333333333333333333333333
mkbuild "$t/mbuild" "$mcommit" 1.3.2 bnew
args=(plan --build "$t/mbuild" --maint); r3 G3_ROOT="$mr"
ok '[ "$rc" = 0 ] && grep -qx "previous v1.3.1" "$G/$mcommit/plan" && grep -qx "mac.app new" "$G/$mcommit/plan" && grep -qx "mac.bridge new" "$G/$mcommit/plan" && grep -q "^starting points: v1.0.0 v1.3.0 v1.3.1;" "$t/out"' "--maint: the plan written (app new, previous v1.3.1), the starting points from 1.0.0, 1.3.0 and 1.3.1"
args=(run --build "$t/mbuild" --maint); r3 G3_ROOT="$mr"
sed '/^stub/d' "$G/$mcommit/g3-mac.ok" > "$t/m" 2>/dev/null && mv "$t/m" "$G/$mcommit/g3-mac.ok"
args=(check "$mcommit" "$t/mbuild"); r3 G3_ROOT="$mr"
ok '[ "$rc" = 0 ]' "--maint: its run's record passes check with G3_ROOT (as the branch's release.sh runs it)"
# GR3: its own tag (v1.3.2, not archived) is no starting point of 1.3.2: check still passes, run starts
git -C "$mr" tag v1.3.2
args=(check "$mcommit" "$t/mbuild"); r3 G3_ROOT="$mr"
ok '[ "$rc" = 0 ]' "GR3: tagging v1.3.2 doesn't void 1.3.2's own record"
args=(plan --build "$t/mbuild" --maint); r3 G3_ROOT="$mr"
ok '[ "$rc" = 0 ] && grep -q "^starting points: v1.0.0 v1.3.0 v1.3.1;" "$t/out"' "GR3: run's plan doesn't stop on the release's own unarchived tag"
args=(plan --build "$t/mbuild" --maint); r3 G3_ROOT="$mr" G3_TAGS="v1.3.1 v1.3.10 v1.3.2"
ok '[ "$rc" = 0 ] && grep -q "^starting points: v1.0.0 v1.3.0 v1.3.1;" "$t/out"' "GR3: versions compared as numbers (1.3.10 is above 1.3.2)"
rm -rf "$t/mbuild"
# GR4: G3_ROOT with no G3_STATE, G3_GATES or archive override: the tools checkout (its own copy
# here) keeps vm.sh's state and the archive; the maintenance checkout gets the record
tl="$t/tools"; mkdir -p "$tl/tests/mac" "$tl/tests/harness" "$tl/tests/perf" "$tl/build/harness/vm"
cp "$g3" "$root/tests/mac/g3-guest.sh" "$root/tests/mac/g3ax.swift" "$tl/tests/mac/"
cp "$root/tests/harness/lib.sh" "$root/tests/harness/run.sh" "$tl/tests/harness/"; cp "$t/vm.sh" "$tl/tests/harness/vm.sh"
cp "$root/tests/perf/archive.sh" "$tl/tests/perf/"; ln -s "$A" "$tl/gates-archive"
cp -R "$S/vm/G3 Driver.app" "$tl/build/harness/vm/"; touch "$tl/build/harness/vm/G3 Driver.app/Contents/MacOS/g3ax" "$tl/build/harness/vm/vmtool"
m4=4444444444444444444444444444444444444444; mkbuild "$t/mbuild4" "$m4" 1.3.2 bnew
: > "$t/tools.log"
env G3_TART="$t/tart" G3_SERVE_STUB="$t/serve" STUB_LOG="$t/tools.log" STUB_CLONES="$tl/build/harness/stub-clones" G3_ROOT="$mr" \
  "$tl/tests/mac/g3.sh" run --build "$t/mbuild4" --maint > "$t/out" 2>&1; rc=$?
ok '[ "$rc" = 0 ] && [ -f "$mr/build/gates/$m4/g3-mac.ok" ] && [ -f "$mr/build/gates/$m4/plan" ] && grep -q "^delete g3-" "$t/tools.log" && [ ! -s "$tl/build/harness/stub-clones" ] && [ ! -e "$tl/build/harness/clone" ] && [ ! -e "$mr/build/harness" ]' "G3_ROOT alone: vm.sh's state and the archive from the tools checkout, every clone deleted, the record in the maintenance checkout ($rc)"

# GR2: TERM during a slow up: its group (the stub and its child) is stopped, the clone deleted
: > "$S/vm.log"; rm -f "$S/stub-clones" "$S/clone" "$S/up.pid" "$S/upchild.pid"
( exec env G3_TART="$t/tart" G3_VM="$t/vm.sh" G3_SERVE_STUB="$t/serve" G3_STATE="$S" G3_GATES="$G" GATES_ARCHIVE="$A" STUB_LOG="$S/vm.log" STUB_CLONES="$S/stub-clones" STUB_SLOW_UP=60 "$g3" run --build "$t/build" --only "update-v1.3.0" > "$t/out" 2>&1 ) & gp=$!
for _ in $(seq 1 100); do [ -s "$S/upchild.pid" ] && break; sleep 0.1; done
s0=$SECONDS; kill -TERM "$gp"; wait "$gp"; rc=$?
sleep 0.5
ok '[ "$rc" = 130 ] && [ $((SECONDS - s0)) -le 10 ] && ! kill -0 "$(cat "$S/up.pid")" 2>/dev/null && ! kill -0 "$(cat "$S/upchild.pid")" 2>/dev/null && [ ! -s "$S/stub-clones" ] && grep -q "^delete g3-" "$S/vm.log" && ! ls -d "$S"/g3-active.* > /dev/null 2>&1' "GR2: TERM during a slow up: no worker and no clone survive, the registry removed ($rc, $((SECONDS - s0)) s)"

# interrupted (G33): an earlier .ok is gone at once, the partial record becomes .failed
cp "$t/good.ok" "$rec"; : > "$S/vm.log"; rm -f "$S/stub-clones" "$S/clone"
# (the background subshell execs env, which execs g3.sh: gp is g3.sh itself)
( exec env G3_TART="$t/tart" G3_VM="$t/vm.sh" G3_SERVE_STUB="$t/serve" G3_STATE="$S" G3_GATES="$G" GATES_ARCHIVE="$A" STUB_SLEEP=manage "$g3" run --build "$t/build" --only "update-v1.3.0" > "$t/out" 2>&1 ) & gp=$!
for _ in $(seq 1 100); do grep -q "^sh manage" "$S/vm.log" && break; sleep 0.1; done
kill -TERM "$gp"; wait "$gp"; rc=$?
ok '[ "$rc" = 130 ] && [ ! -e "$rec" ] && grep -qx "result FAIL (interrupted)" "$G/$commit/g3-mac.failed" && grep -q "^delete g3-" "$S/vm.log"' "SIGTERM mid-run: no .ok left, the record .failed (interrupted), the clone deleted"

# Accessibility refused: the owner clicks, recorded
args=(run --build "$t/build" --only "update-v1.3.0"); r3 STUB_AX=no
ok 'grep -qF "in the VM'"'"'s screen (g3-" "$t/out" && grep -qF ", vnc://127.0.0.1:5999 in Screen Sharing, password: $S/vnc-url): on the Manage tab, click Install Stems Cache" "$t/out" && grep -qF ", vnc://127.0.0.1:5999 in Screen Sharing, password: $S/vnc-url): in the RB Stems Plus menu choose Update RB Stems Plus…, then click Update" "$t/out"' "no Accessibility: the owner is asked for each click, with what to do and the VNC address"
ac="$(LC_ALL=C sed -n 's/^up \([^ ]*\) .*/\1/p' "$S/vm.log" | head -1)"
ok '[ "$(LC_ALL=C grep "^up \|^down" "$S/vm.log" | tr "\n" "|")" = "up $ac rb26-template --vnc|" ] && grep -qF "OWNER: G3: the old app'"'"'s clicks are yours in clone $ac (the G3 Driver has no Accessibility): open vnc://127.0.0.1:5999 in Screen Sharing (password: $S/vnc-url)" "$t/out"' "no Accessibility: no restart (the clone has VNC from its start), the owner sent its address"
ok 'grep -q "^sh ax-grant" "$S/vm.log" && ! grep -q "^sh ax " "$S/vm.log"' "no Accessibility: the grant is tried, then no press is attempted"
ok 'grep -q "^owner_clicks 2 " "$G/$commit/g3-mac.failed" && grep -q "clicks=owner:v1.3.0: Stems Cache installed by the old app,v1.3.0: Update" "$G/$commit/g3-mac.failed"' "the owner's clicks are counted and named per scenario"
ok '[ "$rc" = 1 ] && grep -qx "coverage partial (--only)" "$G/$commit/g3-mac.failed" && [ ! -e "$G/$commit/g3-mac.ok" ]' "--only: never a passing record"
# another lane's vm.sh up moves the pointer during the facts: no down, no second up, the run stops
args=(run --build "$t/build" --only "update-v1.3.0"); r3 STUB_STEAL=facts STUB_AX=no
ok '[ "$rc" = 1 ] && ! grep -q "^down" "$S/vm.log" && [ "$(grep -c "^up " "$S/vm.log")" = 1 ] && grep -q "FAIL the VM pointer .* names other-lane" "$G/$commit/g3/1-"*/result' "the pointer moved during the facts (no Accessibility): no down, no second up, the pointer FAIL"

# a press whose action opens a modal dialog "didn't take" (the driver's AX timeout): taken when its
# effect shows (the next button found; the last press: the step's condition), else a FAIL as before
# shellcheck disable=SC2034  # axres: read in ok's eval
axr() { args=(run --build "$t/build" --only "$1"); shift; r3 "$@"; axres="$(cat "$G/$commit/g3/1-"*/result 2>/dev/null)"; }
axr update-v1.0.0 STUB_AX_NOPRESS="Install Stems Plus + Stems Cache"
ok 'grep -q "^note \"Install Stems Plus + Stems Cache\": press confirmed by its effect" <<< "$axres" && grep -q "^ok v1.0.0: Stems Cache installed by the old app (Accessibility)$" <<< "$axres" && ! grep -q "^FAIL" <<< "$axres" && grep -q "^scenario rb26-template update-v1.0.0 pass " "$G/$commit/g3-mac.failed"' "a press that didn't take, its dialog's button found next: confirmed by its effect, the scenario passes"
ok '[ "$(LC_ALL=C grep -c "^sh ax press" "$S/vm.log")" = 3 ] && LC_ALL=C grep -q "^sh ax press io.github.rbstemsplus.app 30000 Install\\\\ Stems\\\\ Plus\\\\ +\\\\ Stems\\\\ Cache Install$" "$S/vm.log" && LC_ALL=C grep -q "^sh ax press io.github.rbstemsplus.app 30000 Install$" "$S/vm.log" && grep -q "press confirmed by its effect" "$G/$commit/g3/1-"*/ax.log' "the rest pressed in a new driver call (only the titles after it), logged in ax.log"
axr update-v1.3.0 STUB_AX_NOPRESS="Install Stems Cache"
ok 'grep -q "^note \"Install Stems Cache\": press confirmed" <<< "$axres" && LC_ALL=C grep -q "^sh ax press io.github.rbstemsplus.app 30000 Install$" "$S/vm.log" && ! grep -q "^FAIL" <<< "$axres"' "1.3.0: Manage, Install Stems Cache (no answer), then Install alone: confirmed"
axr update-v1.3.0 STUB_AX_NOPRESS="Install Stems Cache" STUB_AX_NOFIND=Install
ok '[ "$rc" = 1 ] && grep -q "^FAIL v1.3.0: Stems Cache installed by the old app: the G3 Driver couldn.t press it (ax.log)$" <<< "$axres" && ! grep -q "press confirmed" <<< "$axres" && grep -q "didn.t take the press, and its effect didn.t show" "$G/$commit/g3/1-"*/ax.log && ! grep -q "^sh cond" "$S/vm.log"' "no next button after a press that didn't take: FAIL as before, no wait"
axr update-v1.3.0 STUB_AX_NOPRESS=Install
ok 'grep -q "^ok v1.3.0: Stems Cache installed by the old app (Accessibility; \"Install\": press confirmed by its effect)$" <<< "$axres" && ! grep -q "^FAIL" <<< "$axres"' "the last press didn't take: confirmed by the step's condition"
axr update-v1.3.0 G3_LIMIT=2 STUB_AX_NOPRESS=Install STUB_FAIL=cond
ok '[ "$rc" = 1 ] && grep -q "^FAIL v1.3.0: Stems Cache installed by the old app: the G3 Driver couldn.t press it (\"Install\" didn.t take the press, cache-on didn.t hold within 2 s; ax.log)$" <<< "$axres" && ! grep -q "^sh before-update" "$S/vm.log"' "the last press didn't take and the condition never holds: FAIL, nothing after it"
axr update-v1.3.0 STUB_AX_NOPRESS=menu
ok 'grep -q "^note \"the menu item Update RB Stems Plus\": press confirmed by its effect" <<< "$axres" && grep -q "^ok v1.3.0: Update RB Stems Plus in the old app (Accessibility)$" <<< "$axres" && ! grep -q "^FAIL" <<< "$axres"' "the Update menu item didn't take, its sheet's Update found: confirmed"
axr update-v1.3.0 STUB_AX_NOPRESS=menu STUB_AX_NOFIND=Update
ok '[ "$rc" = 1 ] && grep -q "^FAIL v1.3.0: Update RB Stems Plus in the old app: the G3 Driver couldn.t press it (ax.log)$" <<< "$axres" && ! grep -q "press confirmed" <<< "$axres"' "the Update menu item didn't take and no Update button: FAIL as before"
axr update-v1.3.0 STUB_AX_NOPRESS=Update
ok 'grep -q "^ok v1.3.0: Update RB Stems Plus in the old app (Accessibility; \"Update\": press confirmed by its effect)$" <<< "$axres" && ! grep -q "^FAIL" <<< "$axres"' "Update didn't take (the app quits for the update): confirmed by updated"

# a failing step, a step at the time limit
args=(run --build "$t/build" --only "update-v1.0.0"); r3 STUB_FAIL=uninstall
ok '[ "$rc" = 1 ] && [ -f "$G/$commit/g3-mac.failed" ] && [ ! -e "$G/$commit/g3-mac.ok" ] && grep -q "^scenario rb26-template update-v1.0.0 FAIL" "$G/$commit/g3-mac.failed" && grep -q "^result FAIL (1 scenarios failed" "$G/$commit/g3-mac.failed"' "a failing step fails its scenario and the run (g3-mac.failed, no .ok)"
ok '[ "$(grep -c "^up " "$S/vm.log")" = "$(grep -c "^delete " "$S/vm.log")" ]' "every clone is deleted, failures too"
ok 'grep -q "FAIL stub: uninstall" "$G/$commit/g3/1-rb26-template-update-v1.0.0/result"' "the failing step's reason kept"
fc="$(LC_ALL=C sed -n 's/^up \([^ ]*\) .*/\1/p' "$S/vm.log" | head -1)"
ok '[ -n "$fc" ] && grep -q "^sh faillogs" "$S/vm.log" && [ "$(cat "$G/$commit/g3-fail-$fc/app.log" 2>/dev/null)" = "the app'"'"'s log" ] && [ "$(grep -n "^sh faillogs\|^delete" "$S/vm.log" | head -1 | cut -d: -f2 | cut -c1-11)" = "sh faillogs" ]' "a failed scenario: the guest's logs kept in g3-fail-<clone>/, before the clone is deleted"
printf '#!/bin/bash\necho "$1" >> "%s"\n' "$t/notified" > "$t/notify"; chmod +x "$t/notify"; rm -f "$t/notified"
args=(run --build "$t/build" --only "update-v1.4.0"); r3 G3_NOTIFY="$t/notify"
ok '[ "$(cat "$t/notified" 2>/dev/null)" = "G3: open vnc://127.0.0.1:5999 in Screen Sharing now (password: $S/vnc-url); click Allow on RB Stems Plus Watcher when macOS asks (update-v1.4.0 clone)" ]' "G3_NOTIFY: the owner's notice sent with it, the VNC address in it (no password)"
rm -f "$t/notified"; args=(run --build "$t/build" --only "update-v1.4.0"); r3 G3_NOTIFY="$t/notify" STUB_AX=no
ok '[ "$(cat "$t/notified" 2>/dev/null)" = "G3: open vnc://127.0.0.1:5999 in Screen Sharing now (password: $S/vnc-url); first the old app'"'"'s clicks (no Accessibility): Install Stems Cache, then Update; then click Allow on RB Stems Plus Watcher when macOS asks (update-v1.4.0 clone)" ]' "the core paths without Accessibility: one notice, the old app's clicks first, then Allow"
rm -f "$t/notified"; args=(run --build "$t/build" --only "update-v1.4.0"); r3 G3_NOTIFY="$t/notify" STUB_NO_VNC_URL=1
ok '[ "$rc" = 1 ] && grep -q "^FAIL the clone g3-.* of rb26-template came up without its VNC URL (.*/vnc-url)$" "$G/$commit/g3/1-"*/result && ! grep -q "^sh prepare" "$S/vm.log" && [ "$(grep -c "^delete g3-" "$S/vm.log")" = 1 ] && [ ! -e "$t/notified" ]' "the core paths' clone without its VNC URL: FAIL before any step, the clone deleted, no notice"
rm -f "$t/notified"; args=(run --build "$t/build" --only "update-v1.0.0"); r3 G3_NOTIFY="$t/notify"
ok '[ ! -e "$t/notified" ]' "no notice for a scenario without the core paths"
# a failing core-path step (the last starting point's scenario): it fails, no pass, its clone
# deleted, nothing after it run (the next core step, Uninstall)
for sf in lights kept queued; do
  args=(run --build "$t/build" --only "update-v1.4.0"); r3 STUB_FAIL=$sf
  ok '[ "$rc" = 1 ] && [ ! -e "$G/$commit/g3-mac.ok" ] && grep -q "^scenario rb26-template update-v1.4.0 FAIL" "$G/$commit/g3-mac.failed" && grep -q "FAIL stub: $sf" "$G/$commit/g3/1-rb26-template-update-v1.4.0/result"' "a failing $sf step fails its scenario and the run (no .ok), its reason kept"
  ok '[ "$(grep -c "^up " "$S/vm.log")" = 1 ] && [ "$(grep -c "^delete g3-" "$S/vm.log")" = 1 ] && [ "$(grep -c "^sh $sf" "$S/vm.log")" = 1 ] && ! grep -q "^sh uninstall" "$S/vm.log"' "a failing $sf step: the clone deleted, the step not repeated, nothing after it"
done
# rekordbox's licence text in the clone (exit 4 from kept or queued): G3 stops at once, the clone
# kept running (no logs taken, no delete), the owner told, exit 4
for sp in kept queued; do
  rm -f "$t/notified"; args=(run --build "$t/build" --only "update-v1.4.0"); r3 STUB_STOP=$sp G3_NOTIFY="$t/notify"
  kc="$(LC_ALL=C sed -n 's/^up \([^ ]*\) .*/\1/p' "$S/vm.log" | head -1)"
  ok '[ "$rc" = 4 ] && [ -n "$kc" ] && ! grep -q "^delete\|^sh faillogs\|^sh uninstall" "$S/vm.log" && [ "$(grep -c "^sh $sp" "$S/vm.log")" = 1 ] && grep -qx "$kc running" "$S/stub-clones" && [ ! -e "$S/g3-leftover" ]' "a STOP in $sp: exit 4 at once, the clone kept running (no logs taken, no delete, nothing after)"
  ok '[ "$(tail -1 "$t/notified")" = "G3 STOPPED in clone $kc (kept running, nothing clicked; open vnc://127.0.0.1:5999 in Screen Sharing, password: $S/vnc-url): rekordbox'"'"'s automatic activation notice (stub)" ] && grep -q "^stopped: $kc kept running: rekordbox.s automatic activation notice (stub)$" "$G/$commit/g3-mac.failed" && [ ! -e "$G/$commit/g3-mac.ok" ]' "a STOP in $sp: the owner told (G3_NOTIFY), the record says it (g3-mac.failed)"
done
# vm.sh up's exit 3 (the template's MAC address not held): the run stops, the clone deleted
args=(run --build "$t/build"); r3 STUB_UP_MAC=1
ok '[ "$rc" = 1 ] && [ "$(grep -c "^up " "$S/vm.log")" = 1 ] && [ "$(grep -c "^delete g3-" "$S/vm.log")" = 1 ] && grep -q "FAIL the clone g3-.* of rb26-template didn.t come up with rb26-template.s MAC address (vm.sh: .* stopped: its en0 is" "$G/$commit/g3/1-"*/result && grep -q "^stopped after" "$G/$commit/g3-mac.failed"' "vm.sh up's exit 3 (the MAC not held): FAIL, the clone deleted, no other clone started"
args=(run --build "$t/build" --only "update-v1.3.0"); r3 G3_LIMIT=2 STUB_SLEEP=manage
ok '[ "$rc" = 1 ] && grep -q "FAIL timed out after 2 s" "$G/$commit/g3/"*-rb26-template-update-v1.3.0/result' "a guest step over the time limit is stopped and fails"

# the clones (G35, G36, G38, G44): a failed delete, another lane's VM or pointer, tart not listing
args=(run --build "$t/build"); r3 STUB_DELETE_FAIL=1
c1="$(LC_ALL=C sed -n 's/^up \([^ ]*\) .*/\1/p' "$S/vm.log" | head -1)"
ok '[ "$rc" = 1 ] && [ "$(grep -c "^up " "$S/vm.log")" = 1 ] && grep -qx "$c1" "$S/g3-leftover" && grep -q "FAIL cleanup: clone $c1 not deleted" "$G/$commit/g3/1-"*/result && [ ! -e "$G/$commit/g3-mac.ok" ]' "a delete that fails: FAIL cleanup, the name in g3-leftover, no second clone, no .ok"
args=(run --build "$t/build"); r3 STUB_OTHER_VM=1
ok '[ "$rc" = 1 ] && [ "$(grep -c "^up " "$S/vm.log")" = 1 ] && grep -q "FAIL a VM is running (other-lane)" "$G/$commit/g3/2-"*/result' "another lane's VM running before a clone: the run stops, no second clone"
args=(run --build "$t/build" --only "update-v1.3.0"); r3 STUB_STEAL=prepare
ok '[ "$rc" = 1 ] && ! grep -q "^delete" "$S/vm.log" && ! grep -q "^sh bootstrap" "$S/vm.log" && grep -q "FAIL the VM pointer .* names other-lane" "$G/$commit/g3/1-"*/result && [ -s "$S/g3-leftover" ] && [ "$(grep -c "^up " "$S/vm.log")" = 1 ]' "the pointer moved by another lane: no step and no delete there, the run stops"
args=(run --build "$t/build"); r3 STUB_TART_FAIL=1
ok '[ "$rc" = 2 ] && grep -q "tart list failed" "$t/out" && [ ! -s "$S/vm.log" ]' "tart can't list: the run doesn't start (the VM state unknown)"
args=(smoke); r3 STUB_TART_FAIL=1
ok '[ "$rc" = 2 ] && ! grep -q "^up" "$S/vm.log"' "smoke: tart can't list, no VM started"
args=(smoke); r3
ok '[ "$rc" = 0 ] && [ "$(LC_ALL=C sed -e "s/ g3-[^ ]*//" -e "s/^\(sh [a-z-]*\).*/\1/" "$S/vm.log" | tr "\n" "|")" = "up rb26-template --vnc|push|sh facts|sh dialog|delete|" ] && [ -s "$S/g3-smoke.txt" ]' "smoke: a clone with VNC (no window), its facts, the driver's press on a dialog of macOS's own, deleted"
args=(smoke); r3 STUB_AX=no
ok '[ "$rc" = 0 ] && grep -q "^note dialog skipped: no Accessibility for the G3 Driver" "$S/g3-smoke.txt" && ! grep -q "^sh dialog" "$S/vm.log"' "smoke without the driver's Accessibility: passes, the dialog skipped and said"
args=(smoke); r3 STUB_FAIL=dialog
ok '[ "$rc" != 0 ] && grep -q "^FAIL stub: dialog" "$S/g3-smoke.txt" && grep -q "^delete g3-" "$S/vm.log"' "smoke: the dialog's press failing fails the smoke, the clone deleted"
sleep 30 & sp=$!; echo "$sp" > "$S/g3-server.pid"
args=(run --build "$t/build" --only "update-v1.3.0"); r3
ok 'kill -0 "$sp" 2>/dev/null && [ ! -e "$S/g3-server.pid" ]' "a stale server pid naming another process isn't killed (G46)"
kill "$sp" 2>/dev/null; wait "$sp" 2>/dev/null

# bounded (G41): a grandchild that ignores TERM and holds a $(…) open is killed with its group
s0=$SECONDS; out="$(g3e STUB_HANG=facts "$g3" _t bounded 2 "$t/vm.sh" sh "sudo bash /x/g3-guest.sh facts /Users/admin/xfer/selftest" 2>&1)"
ok '[ "$out" = "rc=124 out=" ] && [ $((SECONDS - s0)) -le 9 ]' "bounded: a TERM-ignoring grandchild inside \$(…) is stopped within the limit and the grace period ($((SECONDS - s0)) s: $out)"
# GS2 (GR5): a leader that exits at once, leaving a child holding its stdout: bounded returns at
# once (stdout only: stderr stays inherited by design) and doesn't kill the child
printf '#!/bin/bash\necho leader-done\nsleep 8 & echo $! > "%s"\nexit 0\n' "$t/linger.pid" > "$t/lead.sh"; chmod +x "$t/lead.sh"
s0=$SECONDS; out="$(g3e "$g3" _t bounded 3 "$t/lead.sh" 2>/dev/null)"
ok '[ "$out" = "rc=0 out=leader-done" ] && [ $((SECONDS - s0)) -le 2 ] && kill -0 "$(cat "$t/linger.pid")" 2>/dev/null' "GS2: a child left holding stdout neither delays bounded nor is killed ($out, $((SECONDS - s0)) s)"
kill "$(cat "$t/linger.pid")" 2>/dev/null; while kill -0 "$(cat "$t/linger.pid")" 2>/dev/null; do sleep 0.1; done
# wait_cond (G42): a poll that hangs ends at the limit; a success after the deadline doesn't count
s0=$SECONDS; out="$(g3e G3_LIMIT=2 STUB_HANG=cond "$g3" _t wait_cond 2 cache-on 2>&1)"
ok '[ "$out" = "rc=1 abort=0" ] && [ $((SECONDS - s0)) -le 9 ]' "wait_cond: a hanging poll ends at the limit ($((SECONDS - s0)) s)"
out="$(g3e G3_LIMIT=2 STUB_LATE=cond "$g3" _t wait_cond 2 cache-on 2>&1)"
ok '[ "$out" = "rc=1 abort=0" ]' "wait_cond: a success after the deadline isn't one ($out)"
out="$(g3e G3_LIMIT=2 "$g3" _t wait_cond 2 cache-on 2>&1)"
ok '[ "$out" = "rc=0 abort=0" ]' "wait_cond: a success within the limit ($out)"
# GR8: the pointer moves during the polls: FAIL and ABORT after at most one foreign poll
: > "$S/vm.log"
out="$(g3e G3_LIMIT=20 STUB_STEAL=cond STUB_FAIL=cond "$g3" _t wait_cond 20 cache-on 2>&1)"
ok '[ "$out" = "rc=1 abort=1" ] && [ "$(grep -c "^sh cond" "$S/vm.log")" = 1 ]' "wait_cond: a pointer moved by another lane stops it after one poll ($out)"
# GR6: tart list is bounded: a hanging list stops vms() (and clone_down's confirmation fails) in time
s0=$SECONDS; args=(plan --build "$t/build"); r3 G3_LIMIT=2 STUB_TART_SLEEP=30
ok '[ "$rc" = 2 ] && grep -q "tart list failed or took over 2 s" "$t/out" && [ $((SECONDS - s0)) -le 12 ]' "GR6: a hanging tart list stops the plan within the bound ($((SECONDS - s0)) s)"
s0=$SECONDS; args=(run --build "$t/build" --only "update-v1.3.0"); r3 G3_LIMIT=2 STUB_TART_SLOW_AFTER_DELETE=1
ok '[ "$rc" = 1 ] && grep -q "FAIL cleanup: clone" "$G/$commit/g3/1-"*/result && [ -s "$S/g3-leftover" ] && [ "$(grep -c "^up " "$S/vm.log")" = 1 ] && [ $((SECONDS - s0)) -le 30 ]' "GR6: a hanging list after the delete is a FAIL cleanup with g3-leftover, in time ($((SECONDS - s0)) s)"
# GR10: a delete that says it worked while the VM stays listed; a name already in the list
args=(run --build "$t/build"); r3 STUB_DELETE_KEEP=1
ok '[ "$rc" = 1 ] && grep -q "FAIL cleanup: clone" "$G/$commit/g3/1-"*/result && [ -s "$S/g3-leftover" ] && [ "$(grep -c "^up " "$S/vm.log")" = 1 ]' "GR10: delete exit 0 but still listed: FAIL cleanup, g3-leftover, no second clone"
args=(run --build "$t/build" --only "update-v1.3.0"); r3 G3_TEST_CLONE=g3-taken STUB_EXTRA_VMS=g3-taken
ok '[ "$rc" = 1 ] && grep -q "FAIL a VM named g3-taken exists already" "$G/$commit/g3/1-"*/result && ! grep -q "^up \|^delete" "$S/vm.log"' "GR10: a clone name already listed: refused, nothing cloned or deleted"

# GR7: About's version in the probe's trace, not followed by more of a version
eval "$(sed -n '/^version_in()/p' "$root/tests/mac/g3-guest.sh")"
printf '{"k":"frame","d":{"w":"Version 1.4.10"}}\n' > "$t/v10.jsonl"; printf '{"k":"frame","d":{"w":"Version 1.4.1"}}\n' > "$t/v1.jsonl"
printf 'Version 1.4.1.2\n' > "$t/v1x.txt"; printf 'Version 1.4.1.\n' > "$t/vdot.txt"
ok '! version_in "$t/v10.jsonl" 1.4.1 && version_in "$t/v1.jsonl" 1.4.1 && ! version_in "$t/v1x.txt" 1.4.1 && version_in "$t/vdot.txt" 1.4.1 && version_in "$t/v10.jsonl" 1.4.10' "GR7: 1.4.10 isn't 1.4.1 (the trace fallback)"
# kept: the counts' verdict reads the grant at the session's end, so an Allow click during kept 1 counts
eval "$(sed -n '/^kept_verdict()/,/^}/p' "$root/tests/mac/g3-guest.sh")"
kv() { [ "$(kept_verdict "$1" "$2" "$3" "$4" "$5")" = "$6" ]; }
ok 'kv 1 no no 0 1 no && kv 1 yes no 3 4 no && kv 1 no yes "?" 1 unread && kv 0 yes yes 1 "?" unread' "kept: no grant at the session's end, or a count unread: not proven (no, unread)"
ok 'kv 1 no yes 0 1 ok && kv 1 no yes 0 0 late && kv 1 no yes 0 2 fail && kv 1 yes yes 2 3 ok && kv 1 yes yes 2 2 fail' "kept 1: Allow during the session counts (0 -> 1 ok, unchanged late, 0 -> 2 a FAIL); granted before, unchanged a FAIL"
ok 'kv 0 yes yes 1 1 ok && kv 0 yes yes 1 2 fail && kv 0 no yes 1 1 late && kv 0 no yes 1 2 fail' "kept 2: silence checked when granted at its start (1 -> 2 a FAIL), unchanged after a late Allow not proven"
# kept, queued: rekordbox quit as a user quits it (its Quit, never a kill: a killed rekordbox
# leaves rekordboxAgent running, which the watcher takes for rekordbox open); its startup notices
# closed only when they are the known ones. The guest's functions, its commands stubbed:
# RBQ_QUIT (one answer per ask, "ok" or "no"), RBQ_LEFT (the processes left after an accepted
# Quit: none, agent, main), RBQ_TEXTS (what rekordbox's windows show), RBQ_UNC (what
# UserNotificationCenter's prompts show; empty: not running); the calls into $t/rbq.log, notes into
# $t/rbq.note
rbq() {   # rbq VAR=VALUE…: rb_quit kept's exit status into rc, its FAILs into $t/rbq.fail
  rm -rf "$t/rbq.log" "$t/rbq.fail" "$t/rbq.n" "$t/rbq.note" "$t/rbqrun"
  ( export "$@"
    eval "$(sed -n '/^RBID=/p; /^UNC=/p; /^RB_LICENCE=/p; /^TCC_DENY=/p; /^rb_guard()/,/^}/p; /^tcc_prompts()/,/^}/p; /^rbpid()/p; /^rbany()/p; /^rb_ask_quit()/p; /^drive()/,/^}/p; /^rb_notices()/,/^}/p; /^rb_quit()/,/^}/p' "$root/tests/mac/g3-guest.sh")"
    H="$t/rbqh"; RUN="$t/rbqrun"; mkdir -p "$RUN"; DRIVER="$t/rbqh/G3 Driver.app"; mkdir -p "$DRIVER"
    sleep() { :; }
    fail() { echo "FAIL $*" >> "$t/rbq.fail"; }
    note() { echo "note $*" >> "$t/rbq.note"; }
    pkill() { echo "pkill $*" >> "$t/rbq.log"; echo killed > "$t/rbq.n"; }
    ps() { echo "/Applications/rekordbox 7/rekordbox.app/Contents/MacOS/rekordboxAgent.app/Contents/MacOS/rekordboxAgent"; }
    asked() { [ -f "$t/rbq.n" ] && [ "$(cat "$t/rbq.n")" != killed ] && [ "$(cut -d' ' -f1 "$t/rbq.n")" = quit ]; }
    pgrep() {
      [ "$(cat "$t/rbq.n" 2>/dev/null)" = killed ] && return 1
      asked || { echo 101; return 0; }
      case "$1 $2 ${RBQ_LEFT:-none}" in
        *"MacOS/rekordbox( |\$)"*main) echo 101 ;;
        *"rekordbox\.app/Contents/ "*agent|*"rekordbox\.app/Contents/ "*main) echo 102 ;;
        *) return 1 ;;
      esac
    }
    as_user() {
      case "$1" in
        osascript) local k; k=$(( $(grep -c "^quit" "$t/rbq.log" 2>/dev/null) + 1 )); echo "quit $*" >> "$t/rbq.log"
          [ "$(echo "${RBQ_QUIT:-ok}" | cut -d' ' -f"$k")" = ok ] && { echo "quit $k" > "$t/rbq.n"; return 0; }
          echo "execution error: rekordbox got an error: User canceled. (-128)" >&2; return 1 ;;
        open) shift 6; local o="$1"; shift; echo "drive $*" >> "$t/rbq.log"
          case "$1" in
            text) case "${RBQ_TEXTS:-}" in *"$4"*) echo ok > "$o" ;; *) echo "FAIL no text" > "$o" ;; esac ;;
            press) printf 'ok\npressed: %s\n' "${4%\?}" > "$o" ;;
            texts) if [ "$2" != com.apple.UserNotificationCenter ]; then printf 'ok\n%s\n' "${RBQ_TEXTS:-}" > "$o"
                   elif [ -n "${RBQ_UNC:-}" ]; then printf 'ok\n%s\n' "$RBQ_UNC" > "$o"; else echo "FAIL not running: $2" > "$o"; fi ;;
          esac ;;
      esac
    }
    rb_quit kept ); rc=$?
}
rbq RBQ_QUIT=ok RBQ_LEFT=none
ok '[ "$rc" = 0 ] && [ ! -s "$t/rbq.fail" ] && [ "$(grep -c "^quit" "$t/rbq.log")" = 1 ] && ! grep -q "^pkill\|^drive press" "$t/rbq.log" && [ ! -s "$t/rbq.note" ] && grep -q "tell application id \"com.pioneerdj.rekordboxdj\" to quit" "$t/rbq.log"' "rb_quit: rekordbox's Quit (an Apple Event to its bundle id), everything gone: passes, nothing killed or pressed"
# rekordbox's licence, sign-in or activation texts: never a press (not even the activation
# notice's OK); G3 stops at once (exit 4, a STOP line, the texts kept), nothing asked of rekordbox
for x in "This computer was activated automatically. Of the 3 computers that can be activated, 3 computers have been activated." "Sign in to your account" "Your licence has expired"; do
  rbq RBQ_QUIT="no ok" RBQ_TEXTS="$x
FirmwareUpdateManager"
  ok '[ "$rc" = 4 ] && ! grep -q "^drive press\|^quit\|^pkill" "$t/rbq.log" && grep -q "^STOP .*nothing pressed, the clone kept running (rekordbox-licence.txt" "$t/rbqrun/result" && grep -qF "$x" "$t/rbqrun/rekordbox-licence.txt"' "rb_quit: rekordbox shows \"${x:0:40}\": STOP (exit 4), nothing pressed or asked, its texts kept"
done
ok 'grep -q "^STOP rekordbox.s automatic activation notice (a seat used" <<< "$(rbq RBQ_TEXTS="This computer was activated automatically."; cat "$t/rbqrun/result")"' "rb_guard: the automatic activation's notice named as such (a seat used)"
rbq RBQ_QUIT="no no ok" RBQ_LEFT=none RBQ_TEXTS="FirmwareUpdateManager"
ok '[ "$rc" = 0 ] && [ ! -s "$t/rbq.fail" ] && [ "$(grep -c "^quit" "$t/rbq.log")" = 3 ] && [ "$(grep -c "^drive press com.pioneerdj.rekordboxdj 3000 Later$" "$t/rbq.log")" = 2 ] && [ "$(grep -c "^drive press" "$t/rbq.log")" = 2 ] && ! grep -q "^pkill" "$t/rbq.log"' "rb_quit: a Quit refused (the firmware offer): Later, asked again; nothing else pressed"
rbq RBQ_QUIT="no ok" RBQ_TEXTS="What's New in rekordbox 7.2"
ok '[ "$rc" = 0 ] && [ "$(grep -c "^drive press com.pioneerdj.rekordboxdj 3000 Close$" "$t/rbq.log")" = 1 ] && [ "$(grep -c "^drive press" "$t/rbq.log")" = 1 ]' "rb_quit: a Quit refused (a What's New window): Close, asked again"
rbq RBQ_QUIT=ok RBQ_TEXTS="INFORMATION
NOTIFICATION
rekordbox ver. 7.2.19 has been released.
Display at startup only when new information is available."
ok '[ "$rc" = 0 ] && [ ! -s "$t/rbq.fail" ] && ! grep -q "^drive press" "$t/rbq.log" && [ ! -e "$t/rbqrun/rekordbox-licence.txt" ]' "rb_quit: rekordbox's INFORMATION window (seen at each start) left open, no STOP"
rbq RBQ_QUIT="no no no no" RBQ_TEXTS="Something else entirely"
ok '[ "$rc" = 1 ] && grep -q "FAIL kept: rekordbox refused its Quit 4 times" "$t/rbq.fail" && [ "$(grep -c "^quit" "$t/rbq.log")" = 4 ] && ! grep -q "^drive press" "$t/rbq.log" && grep -q "^pkill -9 -f rekordbox" "$t/rbq.log"' "rb_quit: an unknown notice never pressed; 4 refusals: FAIL, killed"
ok 'grep -q "FAIL kept: rekordbox refused its Quit 4 times (execution error: rekordbox got an error: User canceled. (-128); its windows. texts: rb-quit-kept.txt)" "$t/rbq.fail" && [ "$(grep -c "^== ask [1-4] refused: execution error: .*(-128)$" "$t/rbqrun/rb-quit-kept.txt")" = 4 ] && [ "$(grep -c "^Something else entirely$" "$t/rbqrun/rb-quit-kept.txt")" = 4 ]' "rb_quit: each refusal kept (osascript's error, rekordbox's window texts) in RUN/rb-quit-WHO.txt, the error in the FAIL"
# macOS's privacy prompts (TCC's): rekordbox's known ones Don't Allow, never Allow; the watcher's
# notification prompt left alone (nothing pressed beside it); any other prompt or alert a FAIL
mic="“rekordbox” would like to access the microphone."
rbq RBQ_QUIT="no ok" RBQ_UNC="$mic"
ok '[ "$rc" = 0 ] && [ "$(grep -c "^drive press com.apple.UserNotificationCenter 3000 Don’t Allow? Don.t Allow?$" "$t/rbq.log")" = 2 ] && ! grep "^drive press" "$t/rbq.log" | grep -v "Don.t Allow? Don.t Allow?$" | grep -q . && [ "$(grep -c "^note rekordbox.s privacy prompt answered Don.t Allow (1: “rekordbox” would like to access the microphone.)$" "$t/rbq.note")" = 2 ] && [ ! -s "$t/rbq.fail" ]' "tcc_prompts: rekordbox's microphone prompt answered Don't Allow (before the Quit and after a refusal), logged; nothing else pressed"
rbq RBQ_QUIT=ok RBQ_UNC="“rekordbox” would like to find and connect to devices on your local network.
“rekordbox” would like to access your contacts."
ok '[ "$rc" = 0 ] && [ "$(grep -c "^drive press com.apple.UserNotificationCenter" "$t/rbq.log")" = 1 ] && grep -q "answered Don.t Allow (2: “rekordbox” would like to find and connect to devices on your local network.; “rekordbox” would like to access your contacts.)$" "$t/rbq.note"' "tcc_prompts: rekordbox's other known prompts (the local network, contacts) Don't Allow too"
rbq RBQ_QUIT=ok RBQ_UNC="$mic
“RB Stems Plus Watcher” would like to send you notifications."
ok '[ "$rc" = 0 ] && ! grep -q "^drive press" "$t/rbq.log" && grep -q "^note rekordbox.s privacy prompt left open: the watcher.s notification prompt is there too$" "$t/rbq.note" && [ ! -s "$t/rbq.fail" ]' "tcc_prompts: beside the watcher's notification prompt: nothing pressed (its grant is the owner's), said so"
rbq RBQ_QUIT=ok RBQ_UNC="“RB Stems Plus Watcher” would like to send you notifications."
ok '[ "$rc" = 0 ] && ! grep -q "^drive press" "$t/rbq.log" && [ ! -s "$t/rbq.note" ] && [ ! -s "$t/rbq.fail" ]' "tcc_prompts: the watcher's notification prompt alone: left alone, no FAIL"
for x in "“Music” would like to access the microphone." "“RB Stems Plus Watcher” would like to access files in your Documents folder." "The disk you attached was not readable by this computer."; do
  rbq RBQ_QUIT=ok RBQ_UNC="$x"
  ok '! grep -q "^drive press" "$t/rbq.log" && grep -qF "FAIL an unknown macOS prompt (nothing pressed): $x" "$t/rbq.fail"' "tcc_prompts: \"${x:0:40}\": a FAIL quoting it, nothing pressed"
done
rbq RBQ_QUIT=ok
ok '[ "$rc" = 0 ] && ! grep -q "^drive press" "$t/rbq.log" && [ ! -s "$t/rbq.fail" ] && [ ! -s "$t/rbq.note" ]' "tcc_prompts: UserNotificationCenter not running: nothing"
ok '! grep -q "user chose: Try Again" "$root/tests/mac/g3-guest.sh"' "queued: the rekordbox open question's Try Again not looked for in the log (neither 1.3.0 nor 1.4.0 logs it)"
ok '[ "$(grep -cxF "  rb_started || exit 1" "$root/tests/mac/g3-guest.sh")" = 2 ] && ! grep -q "rb_mic" "$root/tests/mac/g3-guest.sh"' "rb_started (rb_guard over rekordbox's first 20 s, its notices, the privacy prompts) after each rekordbox launch (kept, queued)"
rbq RBQ_QUIT=ok RBQ_LEFT=agent
ok '[ "$rc" = 1 ] && grep -q "FAIL kept: rekordbox quit, but rekordboxAgent still running 30 s later" "$t/rbq.fail" && grep -q "^pkill -9 -f rekordbox\\\\.app/Contents/$" "$t/rbq.log"' "rb_quit: rekordboxAgent left after rekordbox's Quit: a FAIL naming it (the product's case, not hidden), every process of the bundle killed"
rbq RBQ_QUIT=ok RBQ_LEFT=main
ok '[ "$rc" = 1 ] && grep -q "FAIL kept: rekordbox didn.t quit within 30 s of its Quit" "$t/rbq.fail"' "rb_quit: rekordbox still there 30 s after its Quit: FAIL"
ok '! grep -q "pkill -x rekordbox\|pkill -9 -x rekordbox" "$root/tests/mac/g3-guest.sh" && [ "$(grep -cxF -e "  rb_quit kept || exit 1" -e "  rb_quit queued" "$root/tests/mac/g3-guest.sh")" = 2 ]' "kept and queued end rekordbox with rb_quit, nowhere with a kill"
# GR10: every guest step g3.sh asks for has its case in g3-guest.sh
steps="$( { grep -oE 'gst [a-z][a-z0-9-]*' "$g3" | cut -d' ' -f2; echo cond; } | sort -u)"
cases="$(grep -E '^[a-z][a-z0-9|-]*\)' "$root/tests/mac/g3-guest.sh" | sed 's/).*//' | tr '|' '\n' | sort -u)"
missing_steps="$(comm -23 <(echo "$steps") <(echo "$cases") | tr '\n' ' ')"
ok '[ -z "$missing_steps" ] && [ "$(echo "$steps" | wc -l | tr -d " ")" -ge 15 ]' "GR10: every gst step has a case in g3-guest.sh (missing: ${missing_steps:-none})"

# ---- static: every wait at most 10 minutes; the guest script parses ----
ok 'bash -n "$root/tests/mac/g3-guest.sh" && bash -n "$g3"' "g3.sh and g3-guest.sh parse"
over="$(grep -hoE '(wait_for|wait_event|next_dialog|wait_dialog) [0-9]+' "$root/tests/mac/g3-guest.sh" | awk '$2 > 600000')"
ok '[ -z "$over" ]' "g3-guest.sh: every probe wait at most 600000 ms ($over)"
over="$(grep -hoE '(bounded|wait_cond) [0-9]+' "$g3" | awk '$2 > 600')"
ok '[ -z "$over" ]' "g3.sh: every bounded wait at most 600 s ($over)"
ok 'bash "$root/tests/mac/g3-guest.sh" > /dev/null 2>&1; [ $? = 2 ]' "g3-guest.sh without a step: usage, exit 2"

echo "g3-test: $pass passed, $fail failed"
[ "$fail" = 0 ]

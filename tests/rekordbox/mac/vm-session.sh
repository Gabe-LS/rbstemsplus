#!/bin/bash
# vm-session.sh: the Mac driver of the rekordbox test toolkit (tests/rekordbox/README.md). Runs on
# the Mac, unsandboxed (tart, ssh), and drives rekordbox in the tart VM that tests/harness/vm.sh
# brought up, through its kit (tests/rekordbox/mac/kit, staged into /Users/Shared/rbsp-kit).
#
#   vm-session.sh preflight
#   vm-session.sh stage [--dist DIR] [--corpus DIR]
#   vm-session.sh run T2|hits [--from NAME] [--to NAME] [--max N] [--inject-at N] [--minutes N] [--keep-open]
#   vm-session.sh run watchtest
#   vm-session.sh shot [--out DIR]
#   vm-session.sh collect [--out DIR]
#   vm-session.sh cleanup [--keep-running]
#
# The VM: tests/harness/vm.sh up CLONE first (one VM at a time, with rb26-template's MAC address:
# one rekordbox seat); its clone, SSH key and shared folder are vm.sh's (build/harness).
# Nothing personal is needed or kept: the VM is local, reached as admin over vm.sh's key.
# RBKIT_NOTIFY: the notifier for a STOP (default ~/.local/bin/claude-notify MESSAGE TITLE).
# RBKIT_OUT: where logs, stages, builds and collections go (default build/rekordbox/mac, git-ignored).
# RBKIT_VM_STATE, TART, TART_HOME, TEMPLATE, RBKIT_VMSH: vm.sh's state folder, tart, its VMs, the
# template, vm.sh itself (the tests' stubs).
# Exit: 0 done; 1 a STOP or a step not OK; 2 bad arguments or state; 3 an expectation not met.
# After a STOP (a "<step>: STOP:" line) or an unexpected exit, nothing more is done in the VM:
# rekordbox is left open as it is, and the owner is notified.
# Nothing here answers a dialog: sign-in, licence, activation and macOS's security prompts are the
# owner's, and rekordbox is never started in a VM without the template's MAC address.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(git -C "$here" rev-parse --show-toplevel 2> /dev/null)" || root="$(cd "$here/../../.." && pwd)"
kit="$here/kit"
NOTIFY="${RBKIT_NOTIFY:-$HOME/.local/bin/claude-notify}"
OUT="${RBKIT_OUT:-$root/build/rekordbox/mac}"
VMSTATE="${RBKIT_VM_STATE:-${VM_STATE:-$root/build/harness}}"
TART="${TART:-$HOME/Applications/tart.app/Contents/MacOS/tart}"
VMS_DIR="${TART_HOME:-$HOME/.tart}/vms"
TEMPLATE="${TEMPLATE:-rb26-template}"
VMSH="${RBKIT_VMSH:-$root/tests/harness/vm.sh}"
VK=/Users/Shared/rbsp-kit
VB=/Users/Shared/rbst

usage() { sed -n '6,12p' "$0" | sed 's/^# *//' >&2; exit 2; }
die() { echo "vm-session: $*" >&2; exit 2; }
within() { local s="$1"; shift; perl -e 'alarm shift; exec @ARGV or exit 127' "$s" "$@"; }
# As within, also killing the command if it prints nothing for IDLE seconds (exit 142 then).
idle_within() {   # SECS IDLE CMD...
  perl -MIO::Select -e '
    my ($cap, $idle) = splice @ARGV, 0, 2;
    my $pid = open my $pipe, "-|";
    defined $pid or die "open: $!";
    if (!$pid) { open STDERR, ">&STDOUT" or die "stderr: $!"; alarm $cap; exec @ARGV or exit 127; }
    my $output = IO::Select->new($pipe);
    $| = 1;
    while ($output->can_read($idle)) {
      my $n = sysread($pipe, my $data, 65536);
      defined $n or die "read: $!";
      if (!$n) { close $pipe; exit (($? & 127) ? 128 + ($? & 127) : $? >> 8) }
      print $data;
    }
    kill "KILL", $pid; close $pipe;
    print "\nRB-KIT: no output for $idle s\n";
    exit 142;
  ' "$@"
}
name_ok() { [[ "$1" =~ ^[A-Za-z0-9._~-]+$ ]]; }
num_ok() { [[ "$1" =~ ^[1-9][0-9]*$ ]]; }   # no leading 0: bash would read 08 as octal
need() { [ $# -ge 2 ] && [ -n "$2" ] || die "$1 needs a value"; }
# a results folder inside the checkout only where git ignores it (logs and screenshots of the VM)
out_ok() { local o; o="$(cd "$1" && pwd -P)" || return 1; [[ "$o/" != "$(cd "$root" && pwd -P)/"* ]] || git -C "$root" check-ignore -q "$o/${2-x}"; }

[ $# -ge 1 ] || usage
cmd="$1"; shift
kind="" from="" to="" max="" inject="" minutes="" keep=0 dist="" corpus="" outdir="" running=0
case "$cmd" in
  preflight | stage | collect | cleanup | shot) ;;
  run) [ $# -ge 1 ] || die "run needs T2, hits or watchtest"; kind="$1"; shift
       case "$kind" in T2 | hits | watchtest) ;; prepare) die "run prepare: not on the Mac driver yet (README: the Mac driver)" ;;
         *) die "run: unknown kind '$kind' (T2, hits, watchtest)" ;; esac ;;
  -h | --help) usage ;;
  *) die "unknown command '$cmd' (preflight, stage, run, shot, collect, cleanup)" ;;
esac
while [ $# -gt 0 ]; do
  o="$1"; v="${2-}"
  case "$cmd:$kind:$o" in
    run:T2:--from | run:hits:--from) need "$o" "$v"; name_ok "$v" || die "--from: not a corpus name: $v"; from="$v"; shift ;;
    run:T2:--to | run:hits:--to) need "$o" "$v"; name_ok "$v" || die "--to: not a corpus name: $v"; to="$v"; shift ;;
    run:T2:--max | run:hits:--max) need "$o" "$v"; num_ok "$v" || die "--max: not a number: $v"; max="$v"; shift ;;
    run:T2:--inject-at | run:hits:--inject-at) need "$o" "$v"; num_ok "$v" || die "--inject-at: not a row number: $v"; inject="$v"; shift ;;
    run:T2:--keep-open | run:hits:--keep-open) keep=1 ;;
    run:T2:--minutes | run:hits:--minutes)
      need "$o" "$v"
      [[ "$v" =~ ^[1-9][0-9]{0,3}$ ]] && [ "$v" -le 1440 ] || die "--minutes: expected 1-1440: $v"
      minutes="$v"; shift ;;
    stage::--dist) need "$o" "$v"; [ -f "$v/payload.json" ] || die "--dist: no payload.json in $v"; dist="$v"; shift ;;
    stage::--corpus) need "$o" "$v"; [ -d "$v/v1" ] || die "--corpus: $v needs v1/"; corpus="$v"; shift ;;
    collect::--out | shot::--out) need "$o" "$v"; outdir="$v"; shift ;;
    cleanup::--keep-running) running=1 ;;
    *) die "$cmd${kind:+ $kind}: unknown option '$o'" ;;
  esac
  shift
done

# the VM: vm.sh's clone and key (after the arguments: a bad argument never reaches the VM)
CLONE="$(cat "$VMSTATE/clone" 2> /dev/null || true)"
KEY="$VMSTATE/ssh/id"
[ -n "$CLONE" ] && name_ok "$CLONE" || die "no VM: tests/harness/vm.sh up CLONE first (no clone named in $VMSTATE/clone)"
[ "$CLONE" != "$TEMPLATE" ] || die "$CLONE is the template: the kit runs in clones only"
[ -f "$KEY" ] || die "no SSH key at $KEY: tests/harness/vm.sh up $CLONE first"
mkdir -p "$OUT" || die "can't make $OUT"
out_ok "$OUT" || die "the results folder $OUT is in the checkout and not git-ignored"
stamp="$(date +%Y%m%d-%H%M%S)"
log="$OUT/$stamp-$cmd${kind:+-$kind}.log"
[ ! -e "$log" ] || log="${log%.log}-$$.log"
step="$log.step"   # the last auto call's output
out_ok "$OUT" "${log##*/}" && out_ok "$OUT" "${step##*/}" || die "the log paths in $OUT are in the checkout and not git-ignored"
say() { echo "$*" | tee -a "$log"; }
bad=0
row() { printf '%-5s %s\n' "$1" "$2" | tee -a "$log"; case "$1" in OK | SKIP | NOTE) ;; *) bad=$((bad + 1)) ;; esac; }
sent=0
notify() {   # a blocking event for the owner (ntfy); its status (also in $sent: 1 sent)
  sent=0
  local full="$1: $2" message
  # Omit screenshot trailers and drop absolute paths in free-form errors before clipping.
  message="$(printf '%s' "$full" | perl -CS -0777 -pe '
    s/\R.*//s;
    s/; (?:\d\d:\d\d:\d\d(?:\.\d+)?\s+)?(?:shot |no screenshot\b|screenshot failed:).*//;
    s/ \(kept: .*//;
    s{(?:[A-Za-z]:[\\/]|\\\\|(?<![\w/])~?/(?=[^\s/])).*}{[path]};
    $_ = substr($_, 0, 157) . "..." if length > 160;
  ')" || return 1
  if [ -x "$NOTIFY" ]; then
    if within 30 "$NOTIFY" "$message" "RB-KIT VM" > /dev/null 2>&1; then sent=1; say "(notified: $full) (log $log)"; else say "(notification NOT sent: $full) (log $log)"; fi
  else say "(no notifier at $NOTIFY: $full) (log $log)"; fi
  [ "$sent" = 1 ]
}
SSH_OPTS=(-i "$KEY" -o IdentitiesOnly=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR
          -o ConnectTimeout=10 -o BatchMode=yes -o ServerAliveInterval=30)
vmip=""
ip() {
  local a="$vmip"
  [ -n "$a" ] || a="$(within 40 "$TART" ip "$CLONE" --wait 30 2> /dev/null | tr -d '\r' | head -1)" || return 1
  [[ "$a" =~ ^[0-9.]+$ ]] && echo "$a"
}
# vssh SECS [IDLE] -- COMMAND: COMMAND in the VM as admin, bounded; stdout (CRs dropped); its exit
vssh() {
  local secs="$1" idle="" a
  shift; if [ "$1" != -- ]; then idle="$1"; shift; fi; shift
  a="$(ip)" || return 255
  local -a limit=(within "$secs"); [ -z "$idle" ] || limit=(idle_within "$secs" "$idle")
  "${limit[@]}" ssh "${SSH_OPTS[@]}" -- "admin@$a" "$*" < /dev/null | tr -d '\r'
  return "${PIPESTATUS[0]}"
}
# midi-auto.sh in the VM, as root, its output logged here too (and in $step)
auto() {   # SECS ARGS (values already checked)
  local secs="$1" rc msg; shift
  : > "$step"
  vssh "$secs" 600 -- "sudo /bin/bash $VK/midi-auto.sh $*" | tee -a "$log" "$step"
  rc="${PIPESTATUS[0]}"
  if [ "$rc" = 142 ] && msg="$(idle_message)" && ! stopped; then notify "${cmd}${kind:+ $kind}" "$msg" || true; fi
  return "$rc"
}
idle_message() { grep -m1 -xE 'RB-KIT: no output for [0-9]+ s' "$step" 2> /dev/null; }
stopped() { grep -q ': STOP:' "$step"; }
lost() {   # an exit that means the VM wasn't reached or the step overran
  case "$1" in
    255) notify "$2" "the VM can't be reached over ssh (is it running?)"; return 0 ;;
    142) idle_message > /dev/null || notify "$2" "no end in the time allowed (the runner left in the VM; a dialog?)"; return 0 ;;
  esac
  return 1
}
gone() { [ "$1" = 255 ] || [ "$1" = 142 ]; }
stops() {
  local s; s="$(grep -m1 ': STOP:' "$log")" || return 1
  s="${s#*': STOP:'}"; s="${s# }"
  [[ "$s" != *'(watch test)'* ]] || s="[watch test, nothing to do] $s"
  notify "$1" "$s"; return 0
}
# one rekordbox seat: the VM's network card has the template's MAC address (vm.sh pins it; a clone
# with another one is a new computer to rekordbox, which activates it by itself)
template_mac() {
  local t
  t="$(python3 -I -c 'import json, sys; print(json.load(open(sys.argv[1]))["macAddress"].lower())' "$VMS_DIR/$TEMPLATE/config.json" 2> /dev/null)" || return 1
  [[ "$t" =~ ^([0-9a-f]{2}:){5}[0-9a-f]{2}$ ]] && printf '%s\n' "$t"
}
seat() {
  local t g
  t="$(template_mac)"
  g="$(vssh 30 -- "o=\$(ifconfig en0) && printf '%s\\n' \"\$o\" | awk '\$1 == \"ether\" {print \$2}'" | tr 'A-F' 'a-f')" || g=""
  if [ -n "$t" ] && [ "$g" = "$t" ]; then
    SEAT_MAC="$g"
    return 0
  fi
  SEAT="the VM's MAC address ${g:-unread} is not $TEMPLATE's ${t:-unread}"; return 1
}

case "$cmd" in
preflight)
  say "preflight: $CLONE ($stamp)"
  if ! within 30 "$TART" list 2> /dev/null | awk -v c="$CLONE" '$2 == c {print $NF}' | grep -qx running; then
    row BAD "the VM $CLONE isn't running: tests/harness/vm.sh up $CLONE"; say "$bad not OK"; exit 1
  fi
  out="$(vssh 40 -- "echo reached \$(sw_vers -productVersion)")"; rc=$?
  if [ "$rc" != 0 ]; then row BAD "ssh to the VM (exit $rc)"; say "$bad not OK"; exit 1; fi
  row OK "ssh to the VM (${out#reached }: macOS)"
  if seat; then row OK "the VM's MAC address is $TEMPLATE's (one rekordbox seat)"; else row BAD "$SEAT: rekordbox must not start here (tests/harness/vm.sh down, then up)"; fi
  vssh 600 -- "if [ ! -f $VK/verify-kit.sh ]; then echo 'no kit'; exit 9; fi; sudo /bin/bash $VK/verify-kit.sh" | tee -a "$log"
  v="${PIPESTATUS[0]}"
  case "$v" in 0) row OK "the kit matches its SHA256SUMS" ;; 9) row BAD "no kit in the VM: run stage" ;; *) row BAD "the kit doesn't match its SHA256SUMS: stage again" ;; esac
  if [ "$v" != 0 ]; then row SKIP "midi-auto ensure (the kit isn't verified)"
  else auto 300 ensure && row OK "midi-auto ensure (the lines above)" || row BAD "midi-auto ensure (see the BAD lines above)"; fi
  [ -x "$NOTIFY" ] && row OK "the notifier ($NOTIFY)" || row BAD "no notifier at $NOTIFY: a STOP would reach nobody"
  row NOTE "rekordbox's sign-in, licence and activation are the owner's; a STOP on any such window"
  say "$bad not OK"; [ "$bad" = 0 ] || exit 1 ;;

stage)
  mac="$(template_mac)" || die "stage: can't read $TEMPLATE's MAC address"
  for d in "$kit" "$corpus" "$dist"; do
    [ -n "$d" ] || continue
    while [[ "$d" == */ ]]; do d="${d%/}"; done
    p="$(find "$d" \( -type l -o -name '.*' \) -print -quit)" || die "couldn't inspect $d"
    [ -z "$p" ] || die "stage: symlink or dot path: $p"
  done
  # the tools, built here (RBKit Screen only when its source changed: macOS's Screen Recording
  # permission is for its code hash)
  bd="$OUT/build"; mkdir -p "$bd/bin" "$bd/swift-cache" || die "can't make $bd"
  sdk="$(xcrun --sdk macosx --show-sdk-path 2> /dev/null)" || die "no macOS SDK (xcrun)"
  sw() { xcrun swiftc -O -sdk "$sdk" -module-cache-path "$bd/swift-cache" -target arm64-apple-macos13.0 "$@"; }
  if [ ! -x "$bd/bin/rbkit" ] || [ "$kit/rbkit.swift" -nt "$bd/bin/rbkit" ]; then
    sw "$kit/rbkit.swift" -o "$bd/bin/rbkit" > "$bd/rbkit.build.log" 2>&1 && codesign -f -s - "$bd/bin/rbkit" 2>> "$bd/rbkit.build.log" || { cat "$bd/rbkit.build.log"; die "rbkit doesn't build"; }
  fi
  app="$bd/RBKit Screen.app"
  if [ ! -x "$app/Contents/MacOS/rbkit-screen" ] || [ "$kit/rbkit-screen.swift" -nt "$app/Contents/MacOS/rbkit-screen" ]; then
    mkdir -p "$app/Contents/MacOS"
    cat > "$app/Contents/Info.plist" << 'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>io.github.rbstemsplus.rbkit.screen</string>
<key>CFBundleExecutable</key><string>rbkit-screen</string>
<key>CFBundleName</key><string>RBKit Screen</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>LSMinimumSystemVersion</key><string>13.0</string>
<key>LSUIElement</key><true/>
</dict></plist>
EOF
    sw "$kit/rbkit-screen.swift" -o "$app/Contents/MacOS/rbkit-screen" > "$bd/screen.build.log" 2>&1 \
      && codesign -f -s - --identifier io.github.rbstemsplus.rbkit.screen "$app" 2>> "$bd/screen.build.log" || { cat "$bd/screen.build.log"; die "RBKit Screen doesn't build"; }
    say "stage: RBKit Screen built anew: the owner allows its Screen Recording again (README)"
  fi
  cdh="$(codesign -dvvv "$app" 2>&1 | sed -n 's/^CDHash=//p')"
  st="$(mktemp -d "$OUT/stage-$stamp.XXXXXX")" || die "couldn't make a local stage in $OUT"
  cp "$kit"/*.sh "$kit/screen.conf" "$st/" && mkdir -p "$st/bin" && cp "$bd/bin/rbkit" "$st/bin/" && ditto "$app" "$st/RBKit Screen.app" || die "couldn't fill $st"
  printf '%s\n' "$mac" > "$st/template-mac" || die "couldn't write $st/template-mac"
  if [ -n "$dist" ]; then mkdir -p "$st/dist" && cp "$dist/payload.json" "$st/dist/" || die "couldn't copy $dist/payload.json"; fi
  if [ -n "$corpus" ]; then
    mkdir -p "$st/corpus" && cp -R "$corpus/v1" "$st/corpus/" || die "couldn't copy $corpus/v1"
    [ ! -d "$corpus/mac" ] || cp -R "$corpus/mac" "$st/corpus/" || die "couldn't copy $corpus/mac"
  fi
  (cd "$st" && find . -type f ! -path './SHA256SUMS' | sed 's|^\./||' | LC_ALL=C sort | while IFS= read -r f; do shasum -a 256 "$f" || exit; done > SHA256SUMS) || die "couldn't hash $st"
  (cd "$st" && shasum -a 256 -c --quiet SHA256SUMS) || die "the stage doesn't match its own SHA256SUMS"
  say "stage: $(grep -c . "$st/SHA256SUMS") files in $st (RBKit Screen's code hash ${cdh:-unknown})"
  out="$(vssh 40 -- "sudo mkdir -p $VK && if [ -f $VK/SHA256SUMS ]; then cat $VK/SHA256SUMS; fi")"; rc=$?
  [ "$rc" = 0 ] || { lost "$rc" stage || true; say "stage: the VM not reached (exit $rc)"; exit 1; }
  # the VM's corpus and dist entries are kept when this stage doesn't bring them
  awk -v corpus="$corpus" -v dist="$dist" 'NR == FNR { have[substr($0, 67)]; print; next }
    { p = substr($0, 67); if (!(p in have) && ((corpus == "" && p ~ /^corpus\//) || (dist == "" && p ~ /^dist\//))) print }' \
    "$st/SHA256SUMS" <(printf '%s\n' "$out" | grep -E '^[0-9a-f]{64}  ') > "$st/.sums" &&
    mv "$st/.sums" "$st/SHA256SUMS" || die "couldn't merge the kept manifest"
  # a brought corpus replaces the VM's (its old files would be unlisted)
  clear=""; [ -z "$corpus" ] || clear="sudo rm -rf $VK/corpus && "
  [ -z "$dist" ] || clear="${clear}sudo rm -rf $VK/dist && "
  (cd "$st" && COPYFILE_DISABLE=1 tar --no-mac-metadata -cf - .) | within 1800 ssh "${SSH_OPTS[@]}" -- "admin@$(ip)" \
    "${clear}sudo tar -xof - -C $VK && sudo chmod -R a+rX $VK && sudo chmod 755 $VK/*.sh $VK/bin/rbkit" || { say "stage: the copy into the VM failed"; exit 1; }
  vssh 600 -- "sudo /bin/bash $VK/verify-kit.sh" | tee -a "$log"
  if [ "${PIPESTATUS[0]}" != 0 ]; then row BAD "the kit in the VM doesn't match the stage's SHA256SUMS"; say "$bad not OK"; exit 1; fi
  row OK "the kit in the VM matches the stage's SHA256SUMS"
  if auto 300 ensure && ! stopped; then row OK "midi-auto ensure"; else row BAD "midi-auto ensure (see the BAD lines above)"; fi
  say "$bad not OK"; [ "$bad" = 0 ] || exit 1 ;;

run)
  case "$kind" in
  T2 | hits)
    vmip="$(ip)" || die "can't resolve the VM address for $CLONE"
    if ! seat; then notify "run $kind" "$SEAT: rekordbox not started" || true; say "run $kind: STOP: $SEAT (rekordbox not started)"; exit 1; fi
    m="${minutes:-90}"
    expect=any; [ "$kind" != hits ] || expect=hits
    a="run --expect $expect"
    [ -z "$from" ] || a="$a --from '$from'"
    [ -z "$to" ] || a="$a --to '$to'"
    [ -z "$max" ] || a="$a --max $max"
    [ -z "$inject" ] || a="$a --inject-at $inject"
    a="$a --minutes $m"   # the VM stops at the deadline itself
    auto 90 ready --expect "$expect"; rc=$?
    if [ "$rc" != 0 ] || stopped; then
      stops "run $kind" || lost "$rc" "run $kind ready" || notify "run $kind" "readiness check failed (exit $rc)" || true
      [ "$rc" != 2 ] || exit 2
      exit 1
    fi
    say "run $kind: start, $a, $([ "$keep" = 1 ] && echo 'rekordbox left open' || echo quit) (up to $m min)"
    auto 300 start --mac "$SEAT_MAC"; rc=$?
    if [ "$rc" != 0 ] || stopped; then stops "run $kind" || lost "$rc" "run $kind start" || notify "run $kind" "rekordbox didn't start (exit $rc)" || true; exit 1; fi
    auto $((m * 60 + 720)) "$a"; rc=$?   # a row in progress finishes: up to its 420 s + 300
    if gone "$rc"; then stops "run $kind" || lost "$rc" "run $kind" || true; exit 1; fi
    # rekordbox is quit only after a run that ended as planned (0, or 3: rows not as expected)
    if { [ "$rc" != 0 ] && [ "$rc" != 3 ]; } || stopped; then
      stops "run $kind" || notify "run $kind" "exit $rc without a STOP line" || true
      say "run $kind: exit $rc; rekordbox left as it is"
      [ "$rc" = 0 ] && exit 1; exit "$rc"
    fi
    if [ "$keep" = 0 ]; then
      auto 150 quit; q=$?
      if [ "$q" != 0 ]; then stops "run $kind quit" || lost "$q" "run $kind quit" || notify "run $kind" "quit exit $q" || true; rc=1; fi
    fi
    say "run $kind: exit $rc"; exit "$rc" ;;
  watchtest)
    # the STOP the test window causes goes out as a real notification: the whole path is tested
    auto 120 watchtest; rc=$?
    if lost "$rc" "run watchtest"; then exit 1; fi
    if ! stops "run watchtest"; then say "watchtest: FAIL: no STOP line: the watch missed the test window"; exit 1; fi
    if [ "$sent" != 1 ]; then say "watchtest: FAIL: the notification was not sent"; exit 1; fi
    [ "$rc" = 0 ] || { say "watchtest: FAIL: exit $rc"; exit 1; }
    say "watchtest: PASS (the STOP line seen, the notification sent)" ;;
  esac ;;

shot | collect)
  dir="${outdir:-$OUT/$cmd-$stamp}"; mkdir -p "$dir" || die "can't make $dir"
  out_ok "$dir" && out_ok "$dir" '' || die "--out $dir is in the checkout and not git-ignored"
  if [ "$cmd" = shot ]; then
    auto 60 shot > /dev/null; rc=$?
    p="$(sed -n 's/^[0-9:]*  shot \(\/Users\/Shared\/rbst\/shots\/[0-9A-Za-z._-]*\.png\)$/\1/p' "$step" | tail -1)"
    [ "$rc" = 0 ] && [ -n "$p" ] || { lost "$rc" shot || true; say "shot: none (exit $rc): $(tail -1 "$step")"; exit 1; }
    within 120 scp -q "${SSH_OPTS[@]}" -- "admin@$(ip):$p" "$dir/" || { say "shot: the copy of $p failed"; exit 1; }
    say "shot: $dir/${p##*/}"; exit 0
  fi
  out="$(auto 600 bundle)"; rc=$?
  zip="$(printf '%s\n' "$out" | sed -n 's/^BUNDLE //p' | tail -1)"
  [ "$rc" = 0 ] && [[ "$zip" =~ ^/Users/Shared/rbst/collect-[0-9]{8}-[0-9]{6}\.zip$ ]] || { lost "$rc" collect || true; say "collect: no bundle (exit $rc)"; exit 1; }
  within 1800 scp -q "${SSH_OPTS[@]}" -- "admin@$(ip):$zip" "$dir/" || { say "collect: the copy of $zip failed"; exit 1; }
  (cd "$dir" && unzip -q -o "${zip##*/}") || { say "collect: unzip failed (the screenshots stay in the VM)"; exit 1; }
  auto 60 bundled --keys "${zip##*/}" > /dev/null || say "collect: the VM's copies of the screenshots not deleted (harmless)"
  say "collect: $(find "$dir" -type f | wc -l | tr -d ' ') files in $dir" ;;

cleanup)
  # rekordbox closed first; if it can't be (a STOP: a dialog, or it won't close), nothing else is done
  auto 150 quit; q=$?
  if [ "$q" != 0 ]; then
    stops "cleanup" || lost "$q" "cleanup quit" || notify "cleanup" "quit exit $q" || true
    say "cleanup: stopped: rekordbox left as it is; the VM left running"; exit 1
  fi
  row OK "rekordbox closed"
  out="$(vssh 60 -- "sudo /bin/bash -c '. $VK/kit-lib.sh && midi_down && cache_state'")"; rc=$?
  [ "$rc" = 0 ] && row OK "the MIDI device RBSP Test removed" || row BAD "the MIDI daemon (exit $rc)"
  [ -z "$out" ] || row NOTE "rekordbox's onnxruntime: $out (the clone keeps it; a fresh clone of $TEMPLATE has Pioneer's)"
  if [ "$running" = 1 ]; then row NOTE "the VM left running (--keep-running)"
  elif ! VM_STATE="$VMSTATE" within 180 "$VMSH" down > /dev/null 2>&1; then
    row BAD "tests/harness/vm.sh down failed for $CLONE"
  elif ! vm_list="$(within 30 "$TART" list 2> /dev/null)"; then
    row BAD "tart list failed after down (couldn't confirm $CLONE stopped)"
  elif awk -v c="$CLONE" '$2 == c && $NF == "running" {found = 1} END {exit !found}' <<< "$vm_list"; then
    row BAD "the VM $CLONE is still running after tests/harness/vm.sh down"
  else
    row OK "the VM $CLONE stopped (tests/harness/vm.sh down)"
  fi
  say "$bad not OK"
  [ "$bad" = 0 ] || { notify "cleanup" "$bad step(s) not OK" || true; exit 1; } ;;
esac

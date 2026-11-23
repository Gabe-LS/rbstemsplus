#!/bin/bash
# pc-session.sh: the Windows driver of the rekordbox test toolkit (tests/rekordbox/README.md). Runs
# on the Mac, unsandboxed (ssh), and drives the test account on the Windows test PC through its kit
# (tests/rekordbox/win/kit, staged into the account's %TEMP%\rbsp-kit).
#
#   pc-session.sh preflight [--server HOST:PORT]
#   pc-session.sh stage [--probe DLL] [--dist DIR] [--corpus DIR] [--formats DIR (--format-rows ROW|FROM:TO[,...] | --format-tier published|discovery|all) | --clear-formats] [--helper EXE]
#   pc-session.sh run T2|hits [--rekordbox VERSION] [--from NAME] [--to NAME] [--max N] [--inject-at N] [--minutes N] [--keep-open]
#     run T2 --keys [--fresh] [--aac]
#   pc-session.sh run prepare [--playlist NAME] [--minutes N] [--pause-test]   (--pause-test: Pause ~60 s in, Resume, Stop)
#   pc-session.sh run loc --step STEP [--arg FOLDER|LETTER]   (plan N3's rows in our app's own UI)
#   pc-session.sh run rowkeys --from ROW | --formats [--from ROW] [--to ROW] [--rows ROW|FROM:TO[,...]]   (the staged research helper's --keys on corpus rows)
#     run rowkeys --formats --from ROW --test-unproved-aac|--test-unproved-mp3 [--rekordbox VERSION]   (one format fixture with rekordbox closed)
#   pc-session.sh run keycompare [--from ROW [--to ROW] | --rows ROW|FROM:TO[,...]] [--helper EXE]   (rekordbox's dumped keys vs our helper's)
#   pc-session.sh run hits --formats [--format-batch N|FIRST:LAST] [--confirm-import-refusals] [--rekordbox VERSION]
#   pc-session.sh run keptinfo   (read only: our kept folder, the Status tab in UI Automation, app.log's end)
#   pc-session.sh run stopkeep [--playlist NAME]   (the kept set dropped by our app's Stop Keeping…)
#   pc-session.sh run library-check [--rekordbox VERSION]   (read the installed helper's RBSP playlists)
#   pc-session.sh run format-import [--fresh-format-import | --resume-import] [--rekordbox VERSION]
#   pc-session.sh run format-uidump [--explore] [--rekordbox VERSION]   (dump the running rekordbox UI)
#   pc-session.sh run winlist [--rekordbox VERSION]   (read-only top-level window facts and cropped screenshots)
#   pc-session.sh run dismiss-rb-error --rekordbox VERSION   (dismiss the verified OK-only error)
#   pc-session.sh run start [--rekordbox VERSION]   (start without loading corpus rows)
#   pc-session.sh run format-step ACTION [TARGET] [--rekordbox VERSION]   (one action, leave rekordbox open)
#   pc-session.sh run watchtest
#   pc-session.sh run appswap --dist DIR --base NAME|/   (our app replaced by a TEST build the Mac's server serves)
#   pc-session.sh run blackrepro [--cycles N] [--every N] [--no-minimize]   (our app's black window after session switches)
#   pc-session.sh run preconditions --plan PLAN   (an auto plan's preconditions, all at once; each plan runs it first)
#   pc-session.sh run autoanalysis-off   (rekordbox's Auto Analysis off through its Preferences)
#   pc-session.sh run dml-bench [--from NAME] [--to NAME] [--helper EXE]   (the helper's --dml against its CPU)
#   pc-session.sh run benchstop   (the kit's own bench helpers stopped; a timed-out run is stopped by itself)
#   pc-session.sh run playlists [--open]   (the installed helper's --playlists alone, rekordbox closed or open)
#   pc-session.sh collect [--out DIR]
#   pc-session.sh cleanup [--label NAME] [--no-logoff | --lock] [--no-snapshot]
#   pc-session.sh session up|down|state|cleanup-runners|display   (display: RBKIT_TARGET=vm only)
#   pc-session.sh cache install [--chosen]|uninstall|check   (--chosen: the test account's own choice set too, put back at uninstall or cleanup)
#   pc-session.sh auto session2|prepare|record|dumps [--dist DIR] [--corpus DIR] [--probe DLL] [--commit SHA] [--max N]
#     record: [--cpu-stems-ok]   (the owner's ruling only: directml's NOTE, rekordbox on the CPU, doesn't block the record)
#     dumps: [--rows ROW|FROM:TO[,...]] [--helper EXE] [--no-dumps]
#     formats: auto dumps --formats-only --format-batch N|FIRST:LAST [--fresh-format-import | --reuse-format-import] [--confirm-import-refusals] [--probe DLL]
#              --test-unproved-aac and --test-unproved-mp3 opt the staged research helper into those codec comparisons
#     M12: --from-dist DIR --dist DIR --uia DIR --bootstrap-server HOST:PORT [--installer NAME]
#   pc-session.sh probe install [--no-dumps]|remove|state|fetch [--no-dumps] [--out DIR]   (the research probe for T2's mix dumps)
#   pc-session.sh disk probe --drive LETTER        (read-only, on the owner's PC)
#   pc-session.sh t5 rows [--idle MIN] [--wintest EXE]   (J28 and the sleeping-disk row on the owner's T5)
#   pc-session.sh t5 prep|list|finish|show   (the T5 side of plan N3's app rows and read-only disk inventory)
#
# Settings live in this Mac's local git config, never in a tracked file:
#   rbstemsplus.pcSsh         the owner's admin login (USER@HOST), as tests/harness/netcheck.sh; used
#                             by cleanup (admin rights), the T5 check and disk tools
#   rbstemsplus.pcTestSsh     the test account's login (default: rbtest@ the host of pcSsh)
#   rbstemsplus.pcTestServer  the Mac's TEST server as the PC reaches it (HOST:PORT), for preflight
#   rbstemsplus.pcT5Serial    the dedicated Samsung T5 serial, pinned by the owner
# RBKIT_TARGET=pc|vm: default pc; vm uses a Parallels VM (see vm-target.md):
#   RBKIT_VM       the VM's name (default rb-win-hand-rekordbox; never the owner's "Windows 11")
#   RBKIT_VM_USER  the VM's activated Windows user signed in at its console (default: the Mac's
#                  git config --local rbstemsplus.vmUser)
# RBKIT_NOTIFY: the notifier for a STOP (default ~/.local/bin/claude-notify MESSAGE TITLE).
# RBKIT_EXISTING_SESSION_ONLY=1: session up requires an existing test desktop session.
# RBKIT_OUT: where logs, stages and collections go (default build/rekordbox/win, git-ignored).
# RBKIT_STOP_ON_UPDATE=1: stop on a rekordbox update offer, except during normal close.
# RBKIT_POLL: seconds between cleanup's checks that the account has signed out (default 10; at most 12 checks,
# all within 120 s).
# RBKIT_TRACE=1 logs kit script phases to stderr. Session up defaults to 1 and puts stderr in its log.
# RBKIT_TIMING=1 records elapsed times in each command's .log.timing and in Windows runner output.
# rbstemsplus.pcTest (the Mac's login Keychain, service rbstemsplus.pcTest, account the test account's
# name): its password, read only by session up when the account has no session, and only into
# FreeRDP's stdin (RBKIT_FREERDP: the client; default sdl3-freerdp, sdl-freerdp or xfreerdp on PATH).
# Exit: 0 done; 1 a STOP or a step not OK; 2 bad arguments or settings; 3 an expectation not met.
# After a STOP (a "<step>: STOP:" line) or an unexpected exit, nothing more is done on the PC:
# rekordbox is left open as it is, and the owner is notified.
# The owner handles sign-in, licence, security and UAC prompts. Tests name their app confirmations.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(git -C "$here" rev-parse --show-toplevel 2> /dev/null)" || root="$(cd "$here/../../.." && pwd)"
kit="$here/kit"
NOTIFY="${RBKIT_NOTIFY:-$HOME/.local/bin/claude-notify}"
OUT="${RBKIT_OUT:-$root/build/rekordbox/win}"

usage() { sed -n '6,20p' "$0" | sed 's/^# *//' >&2; exit 2; }
die() { echo "pc-session: $*" >&2; exit 2; }
cfg() { git -C "$root" config --get "$1" 2> /dev/null || true; }
# within SECS CMD...: runs CMD, killed after SECS (exit 142 then)
within() { local s="$1"; shift; perl -e 'alarm shift; exec @ARGV or exit 127' "$s" "$@"; }
# As within, also killing the command if neither output stream produces bytes for IDLE seconds.
idle_within() {   # SECS IDLE CMD...
  perl -MIO::Select -e '
    my ($cap, $idle) = splice @ARGV, 0, 2;
    my $pid = open my $pipe, "-|";
    defined $pid or die "open: $!";
    if (!$pid) {
      open STDERR, ">&STDOUT" or die "stderr: $!";
      alarm $cap;
      exec @ARGV or exit 127;
    }
    my $output = IO::Select->new($pipe);
    $| = 1;
    while ($output->can_read($idle)) {
      my $n = sysread($pipe, my $data, 65536);
      defined $n or die "read: $!";
      if (!$n) { close $pipe; exit (($? & 127) ? 128 + ($? & 127) : $? >> 8) }
      print $data;
    }
    kill "KILL", $pid;
    close $pipe;
    print "\nRB-KIT: no output for $idle s\n";
    exit 142;
  ' "$@"
}
name_ok() { [[ "$1" =~ ^[A-Za-z0-9._~-]+$ ]]; }
num_ok() { [[ "$1" =~ ^[1-9][0-9]*$ ]]; }   # no leading 0: bash would read 08 as octal
text_ok() { [[ "$1" =~ ^[A-Za-z0-9\ ._-]+$ ]]; }
server_ok() { [[ "$1" =~ ^[A-Za-z0-9.-]+:[0-9]+$ ]]; }
login_ok() { [[ "$1" =~ ^[^-@][^@]*@[A-Za-z0-9.:-]+$ ]]; }   # USER@HOST (a space in USER allowed)
need() { [ $# -ge 2 ] && [ -n "$2" ] || die "$1 needs a value"; }
# a results folder inside the checkout only where git ignores it (logs name the PC's accounts and windows)
out_ok() { local o; o="$(cd "$1" && pwd -P)" || return 1; [[ "$o/" != "$(cd "$root" && pwd -P)/"* ]] || git -C "$root" check-ignore -q "$o/${2-x}"; }

[ $# -ge 1 ] || usage
cmd="$1"; shift
kind="" from="" to="" max="" inject="" minutes="" keep=0 playlist="RBSP Tests" server="" dist="" corpus="" formats="" format_rows="" format_tier="" clear_formats=0 formats_only=0 outdir="" label="" logoff=1 commit="" probe="" helper="" plan="" cycles="" every="" nomin=0 plopen=0 pausetest=0 wintest="" idle="" swapbase="" swapbase_set=0 rows="" probekeys=0 fresh=0 aac=0 no_dumps=0 locstep="" locarg="" chosen=0 cpu_ok=0 cpu_note=0
format_batch="" format_last="" reuse_format_import=0 fresh_format_import="" confirm_import=0 test_unproved_aac=0 test_unproved_mp3=0 snapshot=1 rekordbox="" explore="" format_action="" resume_import=""
from_dist="" uia="" bootstrap_server="" installer="" drive=""
case "$cmd" in
  preflight | stage | collect | cleanup) ;;
  session) [ $# -ge 1 ] || die "session needs up, down, state, cleanup-runners or display"; kind="$1"; shift
       case "$kind" in up | down | state | cleanup-runners | display) ;; *) die "session: unknown action '$kind' (up, down, state, cleanup-runners, display)" ;; esac ;;
  cache) [ $# -ge 1 ] || die "cache needs install, uninstall or check"; kind="$1"; shift
       case "$kind" in install | uninstall | check) ;; *) die "cache: unknown action '$kind' (install, uninstall, check)" ;; esac ;;
  auto) [ $# -ge 1 ] || die "auto needs a plan: session2, prepare, record or dumps"; kind="$1"; shift
       case "$kind" in session2 | prepare | record | dumps) ;; *) die "auto: unknown plan '$kind' (session2, prepare, record, dumps)" ;; esac ;;
  m12) [ "${RBKIT_AUTO:-0}" = 1 ] && [ $# -ge 1 ] || die "m12 runs through an auto plan"
       kind="$1"; shift
       case "$kind" in install-old | update | pin | settings-kept | settings-retry | install-new | worker-source | smartscreen | signout) ;; *) die "unknown M12 step '$kind'" ;; esac ;;
  probe) [ $# -ge 1 ] || die "probe needs install, remove, state or fetch"; kind="$1"; shift
       case "$kind" in install | remove | state | fetch) ;; *) die "probe: unknown action '$kind' (install, remove, state, fetch)" ;; esac ;;
  disk) [ "${1:-}" = probe ] || die "disk needs probe --drive LETTER"; kind="$1"; shift ;;
  t5) case "${1:-}" in rows | prep | list | finish | show) ;; *) die "t5 needs rows, prep, list, finish or show" ;; esac; kind="$1"; shift ;;
  run) [ $# -ge 1 ] || die "run needs T2, hits, prepare, watchtest, directml, close or a T2 round-2 action"; kind="$1"; shift
       case "$kind" in T2 | hits | prepare | watchtest | directml | dml-bench | benchstop | playlists | stopkeep | keptinfo | rowkeys | keycompare | loc | blackrepro | appswap | start | close | library-check | format-import | format-uidump | format-step | winlist | dismiss-rb-error | autoanalysis-off | preconditions | t2-facts | t2-probe | t2-model-on | t2-model-off) ;; *) die "run: unknown kind '$kind' (see --help)" ;; esac
       if [ "$kind" = format-step ]; then
         case "${1:-}" in
           focus | dump | escape | cancel-dialog) format_action="$1"; shift ;;
           click | key)
             case "$1:${2:-}" in
               click:playlists | click:file | click:import | click:import-playlist | key:alt+f | key:down | key:right | key:up | key:left)
                 format_action="$1 $2"; shift 2 ;;
               *) die 'format-step: unknown click or key target' ;;
             esac ;;
           *) die 'format-step needs focus, dump, click, key, escape or cancel-dialog' ;;
         esac
       fi ;;
  -h | --help) usage ;;
  *) die "unknown command '$cmd' (preflight, stage, run, collect, cleanup, session, cache, disk, t5, probe, auto)" ;;
esac
# options, each allowed only where it means something
while [ $# -gt 0 ]; do
  o="$1"; v="${2-}"
  case "$cmd:$kind:$o" in
    run:T2:--rekordbox | run:hits:--rekordbox | run:start:--rekordbox | run:close:--rekordbox | run:library-check:--rekordbox | run:format-import:--rekordbox | run:format-uidump:--rekordbox | run:format-step:--rekordbox | run:winlist:--rekordbox | run:dismiss-rb-error:--rekordbox | run:directml:--rekordbox | run:dml-bench:--rekordbox | run:playlists:--rekordbox | run:autoanalysis-off:--rekordbox | run:preconditions:--rekordbox | run:t2-facts:--rekordbox | run:t2-probe:--rekordbox | run:keycompare:--rekordbox | run:rowkeys:--rekordbox | preflight::--rekordbox | cleanup::--rekordbox | probe:install:--rekordbox | probe:remove:--rekordbox | probe:state:--rekordbox | auto:dumps:--rekordbox)
      need "$o" "$v"; [[ "$v" =~ ^[0-9]{1,9}\.[0-9]{1,9}\.[0-9]{1,9}$ ]] || die "$o: a rekordbox version such as 7.2.20"
      rekordbox="$v"; shift ;;
    run:T2:--from | run:hits:--from) need "$o" "$v"; name_ok "$v" || die "--from: not a corpus name: $v"; from="$v"; shift ;;
    run:T2:--to | run:hits:--to) need "$o" "$v"; name_ok "$v" || die "--to: not a corpus name: $v"; to="$v"; shift ;;
    run:T2:--max | run:hits:--max) need "$o" "$v"; num_ok "$v" || die "--max: not a number: $v"; max="$v"; shift ;;
    run:T2:--inject-at | run:hits:--inject-at) need "$o" "$v"; num_ok "$v" || die "--inject-at: not a row number: $v"; inject="$v"; shift ;;
    run:T2:--keep-open | run:hits:--keep-open) keep=1 ;;
    run:T2:--keys) probekeys=1 ;;
    run:T2:--fresh) fresh=1 ;;
    run:T2:--aac) aac=1 ;;
    run:T2:--formats) die 'use auto dumps --formats-only for format measurement' ;;
    run:rowkeys:--formats | run:hits:--formats | run:prepare:--formats) formats_only=1 ;;
    probe:install:--no-dumps | probe:fetch:--no-dumps | auto:dumps:--no-dumps) no_dumps=1 ;;
    stage::--probe | auto:session2:--probe | auto:dumps:--probe | probe:install:--probe) need "$o" "$v"; [ -f "$v" ] && [ "$(basename "$v")" = onnxruntime.dll ] || die "--probe: the probe's onnxruntime.dll"; probe="$v"; shift ;;
    run:T2:--minutes | run:hits:--minutes | run:prepare:--minutes) need "$o" "$v"; num_ok "$v" || die "--minutes: not a number: $v"; minutes="$v"; shift ;;
    run:prepare:--playlist) need "$o" "$v"; text_ok "$v" || die "--playlist: letters, digits, spaces, . _ - only"; playlist="$v"; shift ;;
    preflight::--server) need "$o" "$v"; server_ok "$v" || die "--server: HOST:PORT"; server="$v"; shift ;;
    t5:rows:--wintest) need "$o" "$v"; [ -f "$v" ] && [ "$(basename "$v")" = winc-location-wintest.exe ] || die "--wintest: an x64 winc-location-wintest.exe"; wintest="$v"; shift ;;
    cache:install:--chosen) chosen=1 ;;
    t5:rows:--idle) need "$o" "$v"; num_ok "$v" && [ "$v" -le 20 ] || die "--idle: minutes, at most 20"; idle="$v"; shift ;;
    run:appswap:--dist) need "$o" "$v"; [ -f "$v/bootstrap.ps1" ] && [ -f "$v/payload.json" ] || die "--dist: a TEST build's folder (bootstrap.ps1, payload.json)"; dist="$v"; shift ;;
    run:appswap:--base) need "$o" "$v"; [[ "$v" = / || "$v" =~ ^[A-Za-z0-9._-]+$ ]] || die "--base: one folder name under the server's root, or / for the root"; swapbase="${v#/}"; swapbase_set=1; shift ;;
    run:blackrepro:--cycles) need "$o" "$v"; num_ok "$v" || die "--cycles: not a number: $v"; cycles="$v"; shift ;;
    run:blackrepro:--no-minimize) nomin=1 ;;
    run:blackrepro:--every) need "$o" "$v"; num_ok "$v" || die "--every: not a number: $v"; every="$v"; shift ;;
    run:playlists:--open) plopen=1 ;;
    run:prepare:--pause-test) pausetest=1 ;;
    run:preconditions:--plan) need "$o" "$v"; case "$v" in session2 | prepare | record | dumps) plan="$v" ;; *) die "--plan: session2, prepare, record or dumps" ;; esac; shift ;;
    stage::--helper | run:dml-bench:--helper | run:keycompare:--helper | auto:dumps:--helper) need "$o" "$v"; [ -f "$v" ] && [ "$(basename "$v")" = rbsp-prepare.exe ] || die "--helper: a timing build's rbsp-prepare.exe"; helper="$v"; shift ;;
    run:loc:--step) need "$o" "$v"; case "$v" in set | thispc | pick | texts | install | uninstall | netmap | netunmap | windows) locstep="$v" ;; *) die "--step: set, thispc, pick, texts, install, uninstall, netmap or netunmap" ;; esac; shift ;;
    run:loc:--arg) need "$o" "$v"; [[ "$v" =~ ^[A-Za-z0-9:\\._\ -]+$ ]] || die "--arg: a folder path or a drive letter"; locarg="$v"; shift ;;
    run:dml-bench:--from | run:rowkeys:--from | run:keycompare:--from) need "$o" "$v"; name_ok "$v" || die "--from: not a corpus name: $v"; from="$v"; shift ;;
    run:dml-bench:--to | run:rowkeys:--to | run:keycompare:--to) need "$o" "$v"; name_ok "$v" || die "--to: not a corpus name: $v"; to="$v"; shift ;;
    stage::--dist) need "$o" "$v"; [ -f "$v/payload.json" ] || die "--dist: no payload.json in $v"; dist="$v"; shift ;;
    stage::--corpus) need "$o" "$v"; [ -d "$v/v1" ] && [ -d "$v/win" ] || die "--corpus: $v needs v1/ and win/"; corpus="$v"; shift ;;
    stage::--formats) need "$o" "$v"; [ -d "$v" ] && { [ -f "$v/rows.tsv" ] || [ -f "$v/pc-rows.tsv" ]; } && [ -f "$v/SHA256SUMS" ] || die "$o: $v needs SHA256SUMS and rows.tsv or pc-rows.tsv"; formats="$v"; shift ;;
    stage::--format-rows) need "$o" "$v"; [[ "$v" =~ ^[A-Za-z0-9._~-]+(:[A-Za-z0-9._~-]+)?(,[A-Za-z0-9._~-]+(:[A-Za-z0-9._~-]+)?)*$ ]] || die "$o: ROW or FROM:TO, comma-separated"; format_rows="$v"; shift ;;
    stage::--format-tier) need "$o" "$v"; case "$v" in published | discovery | all) ;; *) die "$o: published, discovery or all" ;; esac; format_tier="$v"; shift ;;
    stage::--clear-formats) clear_formats=1 ;;
    auto:dumps:--formats-only) formats_only=1 ;;
    run:format-uidump:--explore) explore=-Explore ;;
    run:format-import:--resume-import) resume_import=1 ;;
    auto:dumps:--format-batch | run:hits:--format-batch)
      need "$o" "$v"
      [[ "$v" =~ ^[1-9][0-9]{0,3}(:[1-9][0-9]{0,3})?$ ]] || die "$o: a positive batch number or FIRST:LAST"
      format_batch="${v%%:*}"; format_last="${v##*:}"
      [ "$format_batch" -le "$format_last" ] || die "$o: the last batch precedes the first"
      shift ;;
    auto:dumps:--reuse-format-import) reuse_format_import=1 ;;
    auto:dumps:--fresh-format-import | run:format-import:--fresh-format-import) fresh_format_import=1 ;;
    auto:dumps:--confirm-import-refusals | run:hits:--confirm-import-refusals) confirm_import=1 ;;
    auto:dumps:--test-unproved-aac | run:rowkeys:--test-unproved-aac) test_unproved_aac=1 ;;
    auto:dumps:--test-unproved-mp3 | run:rowkeys:--test-unproved-mp3) test_unproved_mp3=1 ;;
    disk:probe:--drive)
      need "$o" "$v"
      [[ "$v" =~ ^[A-Za-z]$ ]] && [ -z "$drive" ] || die "--drive: give one drive letter, without a colon"
      drive="$(printf '%s' "$v" | tr '[:lower:]' '[:upper:]')"; shift ;;
    collect::--out | probe:fetch:--out) need "$o" "$v"; outdir="$v"; shift ;;
    cleanup::--label) need "$o" "$v"; [[ "$v" =~ ^[A-Za-z0-9_-]+$ ]] || die "--label: letters, digits, - and _"; label="$v"; shift ;;
    cleanup::--no-logoff) logoff=0 ;;
    cleanup::--lock) logoff=2 ;;
    cleanup::--no-snapshot) snapshot=0 ;;
    auto:*:--dist) need "$o" "$v"; [ -f "$v/payload.json" ] || die "--dist: no payload.json in $v"; dist="$v"; shift ;;
    auto:*:--corpus) need "$o" "$v"; [ -d "$v/v1" ] && [ -d "$v/win" ] || die "--corpus: $v needs v1/ and win/"; corpus="$v"; shift ;;
    auto:*:--from-dist | stage::--from-dist) need "$o" "$v"; [ -f "$v/payload.json" ] || die "$o: no payload.json in $v"; from_dist="$v"; shift ;;
    auto:*:--uia | stage::--uia) need "$o" "$v"; [ -f "$v/rbui.exe" ] || die "$o: no rbui.exe in $v"; uia="$v"; shift ;;
    auto:*:--bootstrap-server | preflight::--bootstrap-server | m12:*:--bootstrap-server) need "$o" "$v"; server_ok "$v" || die "$o: HOST:PORT"; bootstrap_server="$v"; shift ;;
    auto:*:--installer | stage::--installer | m12:smartscreen:--installer) need "$o" "$v"; [[ "$v" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*\.(exe|msi)$ ]] || die "$o: a Downloads basename ending in .exe or .msi"; installer="$v"; shift ;;
    auto:dumps:--rows | run:rowkeys:--rows | run:keycompare:--rows)
      # ROW or FROM:TO, comma-separated: corpus names, each run its own range
      need "$o" "$v"; [[ "$v" =~ ^[A-Za-z0-9._~-]+(:[A-Za-z0-9._~-]+)?(,[A-Za-z0-9._~-]+(:[A-Za-z0-9._~-]+)?)*$ ]] || die "--rows: ROW or FROM:TO, comma-separated corpus names"
      rows="$v"; shift ;;
    auto:record:--commit) need "$o" "$v"; [[ "$v" =~ ^[0-9a-f]{40}$ ]] || die "--commit: the RC's full 40-hex commit"; commit="$v"; shift ;;
    auto:*:--max) need "$o" "$v"; num_ok "$v" || die "--max: not a number: $v"; max="$v"; shift ;;
    auto:record:--cpu-stems-ok) cpu_ok=1 ;;
    *) die "$cmd${kind:+ $kind}: unknown option '$o'" ;;
  esac
  shift
done

if [ "$fresh$aac" != 00 ] && [ "$probekeys" = 0 ]; then die "--fresh and --aac apply to T2 --keys"; fi
if [ "$aac" = 1 ] && [ -n "$from$to$max" ]; then die "--aac selects the AAC rows itself"; fi
if [ -n "$formats" ] && [ -z "$format_rows$format_tier" ]; then die "--formats needs --format-rows or --format-tier"; fi
if [ -z "$formats" ] && [ -n "$format_tier" ]; then die "--format-tier requires --formats"; fi
if [ "$cmd:$kind" = run:hits ]; then
  if [ "$formats_only" = 1 ]; then
    [ -z "$from$to$max$inject$minutes" ] && [ "$keep" = 0 ] ||
      die 'run hits --formats selects rows by --format-batch'
    format_batch="${format_batch:-1}"
  elif [ -n "$format_batch" ] || [ "$confirm_import" = 1 ]; then
    die 'format batch options require run hits --formats'
  fi
fi
if [ "$cmd:$kind:$formats_only" = run:prepare:1 ]; then
  [[ "$playlist" =~ ^RBSP\ Formats\ [a-f0-9]{16}-[a-f0-9]{12}$ ]] && [ "$pausetest" = 0 ] ||
    die 'run prepare --formats requires the unique imported playlist name and no pause test'
fi
if [ -z "$formats" ] && [ -n "$format_rows" ]; then die "--format-rows requires --formats"; fi
if [ -n "$format_tier" ] && [ -n "$format_rows" ]; then die "--format-tier and --format-rows cannot be combined"; fi
if [ "$clear_formats" = 1 ] && [ -n "$formats$format_rows$format_tier" ]; then die "--clear-formats cannot be combined with --formats"; fi
if [ -n "$fresh_format_import" ] && { [ "$reuse_format_import" = 1 ] || [ -n "$resume_import" ]; }; then
  die "--fresh-format-import conflicts with --reuse-format-import or --resume-import"
fi
if [ "$cmd:$kind" = auto:dumps ]; then
  if [ "$formats_only" = 1 ]; then
    [ -z "$rows$dist$corpus$helper$max" ] || die "stage the formats, probe and helper separately before auto dumps --formats-only"
    format_batch="${format_batch:-1}"
    no_dumps=1
  elif [ -n "$format_batch$fresh_format_import" ] || [ "$reuse_format_import$confirm_import$test_unproved_aac$test_unproved_mp3" != 0000 ]; then
    die "format import and batch options require --formats-only"
  fi
fi
format_last="${format_last:-$format_batch}"
if { [ "$kind" = rowkeys ] || [ "$kind" = keycompare ]; } && [ -n "$to" ] && [ -z "$from" ]; then die "--to requires --from"; fi
if [ -n "$rows" ] && [ -n "$from$to" ]; then die "--rows cannot be combined with --from or --to"; fi
if [ "$cmd:$kind:$test_unproved_aac" = run:rowkeys:1 ]; then
  [ "$formats_only" = 1 ] && [[ "$from" == fmt-aac-* ]] && [ -z "$to$rows" ] ||
    die '--test-unproved-aac requires --formats and one AAC row selected with --from'
fi
if [ "$cmd:$kind:$test_unproved_mp3" = run:rowkeys:1 ]; then
  [ "$formats_only" = 1 ] && [[ "$from" == fmt-mp3-* ]] && [ -z "$to$rows" ] ||
    die '--test-unproved-mp3 requires --formats and one MP3 row selected with --from'
fi
[ "$cmd" != disk ] || [ -n "$drive" ] || die "disk probe needs --drive LETTER"

if [ "$cmd:$kind" = run:dismiss-rb-error ] && [ -z "$rekordbox" ]; then
  die "run dismiss-rb-error requires --rekordbox VERSION"
fi

# the logins (after the arguments: a bad argument never reaches a target)
target="${RBKIT_TARGET-pc}"
case "$target" in
vm)
  case "$cmd:$kind" in disk:* | t5:* | auto:record) die "$cmd${kind:+ $kind} requires RBKIT_TARGET=pc" ;; esac
  vm_name="${RBKIT_VM:-rb-win-hand-rekordbox}"
  test_user="${RBKIT_VM_USER:-$(cfg rbstemsplus.vmUser)}"; test_login=vm-user; owner=vm-system
  [ -n "$test_user" ] || die "no VM user: RBKIT_VM_USER or git config --local rbstemsplus.vmUser (the VM's activated Windows user)"
  name_ok "$test_user" || die "RBKIT_VM_USER: letters, digits, . _ ~ - only ($test_user)"
  name_ok "$vm_name" || die "RBKIT_VM: letters, digits, . _ ~ - only ($vm_name)"
  . "$here/vm-transport.sh" || die 'could not load the VM transport'
  if [ -z "$server" ]; then
    if [ "${RBKIT_VM_SERVER+x}" ]; then server="$RBKIT_VM_SERVER"
    else server="$(vm_run server-address)" || die 'could not read the VM TEST server address'; fi
  fi
  [ -z "$server" ] || server_ok "$server" || die 'RBKIT_VM_SERVER: not HOST:PORT'
  OUT="${RBKIT_OUT:-$root/build/rekordbox/win-vm}"
  machine=VM; connection='Parallels to the test account'; notify_title='RB-KIT VM'; copy_method='Parallels share'; format_step_version=7.2.19 ;;
pc)
[ "$cmd:$kind" != session:display ] || die "session display requires RBKIT_TARGET=vm"
machine=PC; connection='ssh to the test account'; notify_title='RB-KIT PC'; copy_method=scp; format_step_version=7.2.20
owner="$(cfg rbstemsplus.pcSsh)"
test_login="$(cfg rbstemsplus.pcTestSsh)"
[ -n "$test_login" ] || { [ -n "$owner" ] && test_login="rbtest@${owner##*@}"; }
[ -n "$test_login" ] || die "no PC login: git config --local rbstemsplus.pcSsh USER@HOST (and optionally rbstemsplus.pcTestSsh)"
[ -z "$owner" ] || login_ok "$owner" || die "rbstemsplus.pcSsh: not USER@HOST"
login_ok "$test_login" || die "rbstemsplus.pcTestSsh: not USER@HOST"
test_user="${test_login%@*}"
name_ok "$test_user" || die "rbstemsplus.pcTestSsh: the account name must be letters, digits, . _ ~ - ($test_user)"
if [ -z "$server" ]; then server="$(cfg rbstemsplus.pcTestServer)"; [ -z "$server" ] || server_ok "$server" || die "rbstemsplus.pcTestServer: not HOST:PORT"; fi
;;
*) die "RBKIT_TARGET must be pc or vm (got '$target')" ;;
esac
mkdir -p "$OUT" || die "can't make $OUT"
out_ok "$OUT" || die "the results folder $OUT is in the checkout and not git-ignored"
stamp="$(date +%Y%m%d-%H%M%S)"
log="$OUT/$stamp-$cmd${kind:+-$kind}.log"
[ ! -e "$log" ] || log="${log%.log}-$$.log"   # another call in the same second: its own log (stops reads it)

step="$log.step"   # the last auto call's output
out_ok "$OUT" "${log##*/}" && out_ok "$OUT" "${step##*/}" || die "the log paths in $OUT are in the checkout and not git-ignored"
timing_now() { perl -MTime::HiRes=clock_gettime,CLOCK_MONOTONIC -e 'printf "%d", 1000 * clock_gettime(CLOCK_MONOTONIC)'; }
timing_record() {
  local end
  [ -n "$2" ] && end="$(timing_now)" || return 0
  printf 'TIMING mac %s start_ms=%s elapsed_ms=%s exit=%s\n' "$1" "$2" "$((end - $2))" "$3" >> "$log.timing"
}
timed() {
  local phase="$1" start rc; shift
  if [ "${RBKIT_TIMING:-0}" != 1 ]; then "$@"; return "$?"; fi
  start="$(timing_now)"
  "$@"; rc=$?
  timing_record "$phase" "$start" "$rc"
  return "$rc"
}
timing_start=''
finish_timing() { timing_record "${cmd}${kind:+-$kind}" "$timing_start" "$1"; }
if [ "${RBKIT_TIMING:-0}" = 1 ]; then
  out_ok "$OUT" "${log##*/}.timing" || die "the timing log in $OUT is not git-ignored"
  timing_start="$(timing_now)"
  trap 'finish_timing "$?"' EXIT
fi
say() { echo "$*" | tee -a "$log"; }
bad=0
row() { printf '%-5s %s\n' "$1" "$2" | tee -a "$log"; case "$1" in OK | SKIP | NOTE) ;; *) bad=$((bad + 1)) ;; esac; }
sent=0
notify() {   # COMMAND REASON: one successful ntfy alert per command
  [ "$sent" = 0 ] || return 0
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
    if "$NOTIFY" "$message" "$notify_title" > /dev/null 2>&1; then
      sent=1
      if [ -f "${RBKIT_NOTIFY_MARKER:-}" ]; then printf 'sent\n' >> "$RBKIT_NOTIFY_MARKER"; fi
      say "(notified: $full) (log $log)"
    else say "(notification NOT sent: $full) (log $log)"; fi
  else say "(no notifier at $NOTIFY: $full) (log $log)"; fi
  [ "$sent" = 1 ]
}
# PowerShell on the PC, as LOGIN, bounded by SECS. The script goes as -EncodedCommand (UTF-16LE,
# base64): no quoting through ssh, cmd and PowerShell. Output to stdout (CRs dropped); exit = its exit.
enc() { printf '%s' "$1" | iconv -f UTF-8 -t UTF-16LE | base64 | tr -d '\n'; }
pcps() {
  local login="$1" secs="$2" idle="${4:-}" ps="\$ProgressPreference = 'SilentlyContinue'; \$k = Join-Path \$env:TEMP 'rbsp-kit'; $3"
  if [ "$target" = vm ]; then vm_ps "$login" "$secs" "$ps" "${idle:-0}"; return; fi
  local -a limit=(within "$secs")
  if [ -n "${idle:-}" ]; then limit=(idle_within "$secs" "$idle"); fi
  timed ssh "${limit[@]}" ssh -o BatchMode=yes -o ConnectTimeout=15 -o ServerAliveInterval=30 -- "$login" \
    "powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand $(enc "$ps")" < /dev/null | tr -d '\r'
  return "${PIPESTATUS[0]}"
}
put() {   # SECS RECURSIVE LOGIN DEST FILE...
  local secs="$1" recursive="$2" login="$3" dest="$4"; shift 4
  if [ "$target" = vm ]; then vm_transfer put "$secs" "$login" "$dest" "$@"; return; fi
  timed scp within "$secs" scp -q ${recursive:+-r} -o BatchMode=yes -- "$@" "$login:$dest"
}
get() {   # SECS RECURSIVE LOGIN DEST REMOTE...
  local secs="$1" recursive="$2" login="$3" dest="$4" path; shift 4
  if [ "$target" = vm ]; then vm_transfer get "$secs" "$login" "$dest" "$@"; return; fi
  local -a paths=()
  for path; do paths+=("$login:$path"); done
  timed scp within "$secs" scp -q ${recursive:+-r} -o BatchMode=yes -- "${paths[@]}" "$dest"
}
# midi-auto.ps1 on the PC, as the test account, its output logged here too (and in $step)
auto() {   # SECS ARGS (PowerShell syntax, values already checked)
  local secs="$1"; shift
  local idle=600 rc msg guard=""
  [ "${RBKIT_STOP_ON_UPDATE:-0}" != 1 ] || guard="\$env:RBKIT_STOP_ON_UPDATE = '1'; "
  [ "${RBKIT_TIMING:-0}" != 1 ] || guard+="\$env:RBKIT_TIMING = '1'; "
  : > "$step"
  timed "windows-$2" pcps "$test_login" "$secs" "$guard& (Join-Path \$k 'midi-auto.ps1') $*${rekordbox:+ -RbVersion '$rekordbox'}; exit \$LASTEXITCODE" "$idle" | tee -a "$log" "$step"
  rc="${PIPESTATUS[0]}"
  if [ "$rc" = 142 ] && msg="$(idle_message)" && ! stopped; then notify "${cmd}${kind:+ $kind}" "$msg" || true; fi
  return "$rc"
}
idle_message() { grep -m1 -xE 'RB-KIT: no output for [0-9]+ s' "$step" 2> /dev/null; }
stopped() { grep -q ': STOP:' "$step"; }   # the last auto call wrote a STOP line
format_continuation() {
  local token stamp='(absent|[0-9]+:[0-9]+)'
  token="$(sed -n 's/^FORMAT CONTINUATION //p' "$step")"
  [[ "$token" =~ ^[0-9]+:[0-9]+/$stamp,$stamp$ ]] || return 1
  printf '%s\n' "$token"
}
format_run_id() { uuidgen | tr -d '-' | tr '[:upper:]' '[:lower:]'; }
lost() {   # an ssh exit that means the PC wasn't reached or the step overran
  case "$1" in
    255) if [ "$target" = vm ]; then notify "$2" "the VM can't be reached through the Parallels runner";
         else notify "$2" "the PC can't be reached over ssh (off, asleep, network?)"; fi; return 0 ;;
    142) idle_message > /dev/null || notify "$2" "no end in the time allowed (the runner left on the $machine; a dialog?)"; return 0 ;;
  esac
  return 1
}
gone() { [ "$1" = 255 ] || [ "$1" = 142 ]; }   # the PC not reached, or the step overran
# the STOP lines of the given log, one notification for the first (marked when it is the watch's test
# window); 1 when there is none. $sent says whether the notification went out.
stops() {
  local s; s="$(grep -m1 ': STOP:' "${2:-$log}")" || return 1
  s="${s#*': STOP:'}"; s="${s# }"
  [[ "$s" != *'(watch test)'* ]] || s="[watch test, nothing to do] $s"
  notify "$1" "$s"; return 0
}
# the test account's SID, its PC's name and whether it is in Administrators (by SID), read over
# its own login: t_sid, t_pc, t_admin
ident() {
  local out
  local login="$test_login" sid='[Security.Principal.WindowsIdentity]::GetCurrent().User.Value'
  if [ "$target" = vm ]; then login="$owner"; sid="(Get-LocalUser -Name '$test_user').SID.Value"; fi
  out="$(pcps "$login" 60 "\$ErrorActionPreference = 'Stop'; \$me = $sid; 'sid ' + \$me; 'pc ' + \$env:COMPUTERNAME; 'admin ' + @(Get-LocalGroupMember -SID 'S-1-5-32-544' | Where-Object { \$_.SID.Value -eq \$me }).Count")" || return 1
  t_sid="$(sed -n 's/^sid //p' <<< "$out")"; t_pc="$(sed -n 's/^pc //p' <<< "$out")"; t_admin="$(sed -n 's/^admin //p' <<< "$out")"
  [[ "$t_sid" =~ ^S-1-5-21-[0-9-]+$ ]] && name_ok "$t_pc" && [[ "$t_admin" =~ ^[0-9]+$ ]]
}
# the start of a script for the owner's login: refused unless it reaches the test account's PC
onpc() { printf '%s' "\$ErrorActionPreference = 'Stop'; if (\$env:COMPUTERNAME -ne '$t_pc') { 'not the test account''s PC: ' + \$env:COMPUTERNAME; exit 2 }; "; }
KITPS_SHORT_SECS=60 KITPS_CONSOLE_SECS=120 KITPS_PROBE_SECS=300 KITPS_WORKER_SECS=900
KITPS_FLAGS='-NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand'
# the admin login's copy of the runner and the kit scripts: a folder in the owner's own profile,
# which the test account can't write
KITPS_DIR='rbsp-kit-admin'
kitps_trace() {
  if [ "${RBKIT_TRACE:-0}" = 1 ]; then
    printf '%s\n' 'function Trace-KitPhase([string]$phase) { [Console]::Error.WriteLine(("{0:o} {1}" -f [DateTime]::UtcNow, $phase)) }'
  else
    printf '%s\n' 'function Trace-KitPhase([string]$phase) {}'
  fi
}
# A kit script for the owner's login (console.ps1, admin-worker.ps1, ...): nothing on ssh's stdin
# (the PC's Windows OpenSSH could leave piped stdin undelivered, 2026-11-19). The script and the
# runner (kitps-run.ps1) are copied to %USERPROFILE%\rbsp-kit-admin; a short encoded command (well
# under cmd.exe's 8191 characters) reads both, checks their SHA-256 against this Mac's files, runs
# nothing on a mismatch or a missing file ("RBKIT: ...", exit 9), and otherwise calls the runner with
# the deadline, the PC check, the trace function and PRELUDE (the script's settings, values already
# checked), sent base64-encoded, and the verified script text. Never run from the test account's
# %TEMP%, which that account can write.
kitps_stub() {   # SECS SCRIPT PRELUDE: the encoded command
  local rh sh p64
  rh="$(shasum -a 256 < "$here/kitps-run.ps1" | cut -d' ' -f1)" && sh="$(shasum -a 256 < "$kit/$2" | cut -d' ' -f1)" || return 1
  p64="$({ onpc; printf '\n'; kitps_trace; printf '%s\n' "$3"; } | base64 | tr -d '\n')"
  enc "# rbkit-runner
\$ErrorActionPreference = 'Stop'; \$ProgressPreference = 'SilentlyContinue'; \$d = Join-Path \$env:USERPROFILE '$KITPS_DIR'
function Get-KitFile(\$n, \$h) {
  \$p = Join-Path \$d \$n; if (-not [IO.File]::Exists(\$p)) { \"RBKIT: missing \$n\"; exit 9 }
  \$b = [IO.File]::ReadAllBytes(\$p); \$x = -join ([Security.Cryptography.SHA256]::Create().ComputeHash(\$b) | ForEach-Object { \$_.ToString('x2') })
  if (\$x -ne \$h) { \"RBKIT: hash mismatch \$n\"; exit 9 }
  [Text.Encoding]::UTF8.GetString(\$b).TrimStart([char]0xFEFF)
}
\$r = Get-KitFile 'kitps-run.ps1' '$rh'; \$s = Get-KitFile '$2' '$sh'
& ([scriptblock]::Create(\$r)) $1 ([Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('$p64'))) \$s"
}
kitps_once() {   # SECS SCRIPT PRELUDE
  local stub
  stub="$(kitps_stub "$@")" || return 2
  if [ "$target" = vm ]; then
    vm_ps "$owner" "$1" "& ([scriptblock]::Create([Text.Encoding]::Unicode.GetString([Convert]::FromBase64String('$stub')))); exit \$LASTEXITCODE"
    return
  fi
  timed ssh within "$1" ssh -o BatchMode=yes -o ConnectTimeout=15 -o ServerAliveInterval=30 -- "$owner" "powershell $KITPS_FLAGS $stub" < /dev/null | tr -d '\r'
  return "${PIPESTATUS[0]}"
}
kitps_copy() {   # SCRIPT: the folder made, the runner and SCRIPT copied (scp, as stage copies the kit)
  if [ "$target" = vm ]; then
    local dest
    dest="$(vm_ps "$owner" 60 "\$ErrorActionPreference = 'Stop'; (New-Item -ItemType Directory -Force -Path (Join-Path \$env:USERPROFILE '$KITPS_DIR')).FullName")" &&
      [[ "$dest" == [A-Za-z]:\\* && "$dest" != *$'\n'* ]] &&
      put 120 '' "$owner" "$dest" "$here/kitps-run.ps1" "$kit/$1"
    return
  fi
  timed ssh within 60 ssh -o BatchMode=yes -o ConnectTimeout=15 -- "$owner" \
    "powershell $KITPS_FLAGS $(enc "New-Item -ItemType Directory -Force -Path (Join-Path \$env:USERPROFILE '$KITPS_DIR') | Out-Null")" < /dev/null > /dev/null &&
    timed scp within 120 scp -q -o BatchMode=yes -- "$here/kitps-run.ps1" "$kit/$1" "$owner:$KITPS_DIR/" < /dev/null
}
kitps() {   # SECS SCRIPT PRELUDE: a refusal for a missing or changed file copies it once, then runs again
  local out rc
  [ -f "$kit/$2" ] || { echo "no $kit/$2"; return 2; }
  out="$(kitps_once "$@")"; rc=$?
  if [ "$rc" = 9 ] && [[ "$out" =~ ^RBKIT:\ (missing|hash\ mismatch)\  ]]; then
    kitps_copy "$2" || { printf '%s\n' "$out" 'RBKIT: the copy to the admin login failed'; return 2; }
    out="$(kitps_once "$@")"; rc=$?
  fi
  [ -z "$out" ] || printf '%s\n' "$out"
  return "$rc"
}
# state|up|down (needs ident). A timeout (142) is retried once with a fresh runner.
# The intermittent hangs seen on 2026-11-19 were in the old stdin transport.
# A second timeout, or any other failure, is returned as it is.
console() {
  local out rc
  out="$(timed "console-$1" kitps "$KITPS_CONSOLE_SECS" console.ps1 "\$Mode = '$1'; \$Sid = '$t_sid'")"; rc=$?
  if [ "$rc" = 142 ]; then
    printf 'session: console %s: retry after timeout\n' "$1" | tee -a "$log" >&2
    out="$(timed "console-$1" kitps "$KITPS_CONSOLE_SECS" console.ps1 "\$Mode = '$1'; \$Sid = '$t_sid'")"; rc=$?
  fi
  [ -z "$out" ] || printf '%s\n' "$out"
  return "$rc"
}
# the kit's own stale runners (cleanup-runners.ps1 knows them by their decoded encoded command)
cleanup_runners() { kitps "$KITPS_SHORT_SECS" cleanup-runners.ps1 ''; }
# The account signed in without a person: FreeRDP over an SSH tunnel as the account, its password
# from the Mac's Keychain straight into FreeRDP's stdin (/args-from:stdin: never an argument, a file
# or a log), no window on the Mac (SDL's dummy video driver). The session it makes is then put on
# the console (console up), which ends the RDP connection. 0 signed in; 4 no client; 5 no password.
KEYCHAIN_SERVICE=rbstemsplus.pcTest
signin() {
  local rdp="${RBKIT_FREERDP:-}" port tun rp i st
  if [ -z "$rdp" ]; then rdp="$(command -v sdl3-freerdp || command -v sdl-freerdp || command -v xfreerdp || true)"; fi
  [ -n "$rdp" ] && [ -x "$rdp" ] || return 4
  security find-generic-password -s "$KEYCHAIN_SERVICE" -a "$test_user" > /dev/null 2>&1 || return 5
  port=$((20000 + RANDOM % 20000))
  ssh -N -o BatchMode=yes -o ExitOnForwardFailure=yes -o ConnectTimeout=15 -L "127.0.0.1:$port:127.0.0.1:3389" -- "$test_login" < /dev/null > /dev/null 2>&1 &
  tun=$!
  if [ "${RBKIT_TIMING:-0}" = 1 ]; then
    # This connection stays open through the sign-in. Keep the real ssh PID for teardown.
    printf 'TIMING mac ssh-tunnel pid=%s\n' "$tun" >> "$log.timing"
  fi
  for i in $(seq 1 30); do nc -z 127.0.0.1 "$port" > /dev/null 2>&1 && break; sleep 0.5; done
  { printf '/v:127.0.0.1:%s\n/u:%s\n/cert:ignore\n/size:1280x720\n/p:' "$port" "$test_user"; security find-generic-password -s "$KEYCHAIN_SERVICE" -a "$test_user" -w; } |
    SDL_VIDEO_DRIVER=dummy SDL_VIDEODRIVER=dummy within 150 "$rdp" /args-from:stdin > "$log.rdp" 2>&1 &
  rp=$!
  st=1
  for i in $(seq 1 30); do
    sleep "${RBKIT_POLL:-3}"
    case "$(console state 2>> "$log" | tail -1)" in "CONSOLE none") ;; CONSOLE*) st=0; break ;; esac
    kill -0 "$rp" 2> /dev/null || break
  done
  if [ "$st" = 0 ]; then say "session: signed in over RDP ($(basename "$rdp"))"; else say "session: the RDP sign-in made no session (see $log.rdp)"; fi
  kill "$rp" "$tun" 2> /dev/null; wait "$rp" "$tun" 2> /dev/null
  return "$st"
}
# the account's desktop session ready on the console: put there when it exists, else signed in
# first (signin). 0 ready; 1 not
# The VM target's display made the PC's (1920x1080 at 100 %: the shots and OCR regions): Parallels'
# high resolution off for the VM, the size set in the account's session, and Windows restarted once
# when the scale needs a fresh sign-in (never with rekordbox running). After every snapshot revert.
vm_display() {
  local hires out shown rc left end pass
  hires="$(vm_hires_off)" || { row BAD "the VM's Parallels high resolution setting not read or set"; return 1; }
  [ "$hires" = off ] || hires='switched off'
  for pass in 1 2; do
    out="$(pcps "$test_login" 120 "& (Join-Path \$k 'vm-display.ps1') -Width 1920 -Height 1080; exit \$LASTEXITCODE")"; rc=$?
    printf '%s\n' "$out" | tee -a "$log"; shown="$(sed -n 's/^DISPLAY //p' <<< "$out" | tail -1)"
    case "$rc" in
      0) row OK "the VM display: $shown (Parallels high resolution $hires)"; return 0 ;;
      3) [ "$pass" = 1 ] || break ;;
      *) row BAD "the VM display not set to 1920x1080 (exit $rc)"; return 1 ;;
    esac
    say "session display: the scale needs a fresh sign-in: Windows restarts"
    pcps "$owner" 60 "if (Get-Process rekordbox -ErrorAction SilentlyContinue) { 'rekordbox is running'; exit 2 }; & shutdown.exe /r /t 5 /c 'RB-KIT display'; exit \$LASTEXITCODE" | tee -a "$log"
    [ "${PIPESTATUS[0]}" = 0 ] || { row BAD "Windows not restarted for the display scale (rekordbox running?)"; return 1; }
    sleep $((${RBKIT_POLL:-10} * 6))
    end=$((SECONDS + 480)); out=""
    while [ "$SECONDS" -lt "$end" ]; do
      left=$((end - SECONDS)); [ "$left" -le 30 ] || left=30
      out="$(pcps "$owner" "$left" "\$u = (Get-CimInstance Win32_ComputerSystem).UserName; if (\$u -eq (\$env:COMPUTERNAME + '\\$test_user') -and (Get-Process explorer -ErrorAction SilentlyContinue)) { 'signed in' }" 2> /dev/null)"
      [ "$out" = "signed in" ] && break
      sleep "${RBKIT_POLL:-10}"
    done
    [ "$out" = "signed in" ] || { row BAD "$test_user not signed in again within 8 min of the restart"; return 1; }
    sleep $((${RBKIT_POLL:-10} * 2))
  done
  row BAD "the VM display scale is not 100 % after a restart: $shown"; return 1
}
session_up() {
  local out rc RBKIT_TRACE="${RBKIT_TRACE:-1}"
  [ -n "$owner" ] || { row BAD "session up needs the admin login (rbstemsplus.pcSsh)"; return 1; }
  ident || { row BAD "the test account's SID and PC not read"; return 1; }
  out="$(console up 2>> "$log")"; rc=$?; printf '%s\n' "$out" | sed 's/^/      /' >> "$log"
  if [ "$rc" = 3 ]; then
    if [ "$target" = vm ]; then row BAD "$test_user has no VM desktop session; sign it in at the VM console"; return 1; fi
    if [ "${RBKIT_EXISTING_SESSION_ONLY:-0}" = 1 ]; then
      row BAD "$test_user has no existing desktop session"; return 1
    fi
    signin; rc=$?
    case "$rc" in
      4) row BAD "$test_user has no session and there is no RDP client (FreeRDP): the owner signs it in once, or installs FreeRDP (README)"; return 1 ;;
      5) row BAD "$test_user has no session and the Keychain has no password for it ($KEYCHAIN_SERVICE): the owner stores it once (README)"; return 1 ;;
    esac
    out="$(console up 2>> "$log")"; rc=$?; printf '%s\n' "$out" | sed 's/^/      /' >> "$log"
  fi
  if [ "$rc" = 0 ] && [[ "$(tail -1 <<< "$out")" == "CONSOLE ok "* ]]; then row OK "$test_user's session on the console, unlocked"; return 0; fi
  row BAD "$test_user's session not on the console (exit $rc): $(tail -1 <<< "$out")"; return 1
}
# the research probe (probe.ps1 on the owner's login): its log and dump folder are fixed in probe.c
PROBE_LOG='C:\rbtest\probe.log'
probe_run() {   # install|remove|state [resolve]; the staged probe's SHA-256 from this run's stage (or $probe)
  local sha=""
  [ -z "$probe" ] || sha="$(shasum -a 256 "$probe" | cut -d' ' -f1)"
  local run_id="${RBKIT_EVIDENCE_RUN:-}"
  [ -z "$run_id" ] || [[ "$run_id" =~ ^[a-f0-9]{32}$ ]] || die 'invalid probe evidence run ID'
  timed "probe-worker-$1${2:+-$2}" kitps "$KITPS_PROBE_SECS" probe.ps1 "\$EvidenceRun = '$run_id'; \$Mode = '$1'; \$Sid = '$t_sid'; \$ProbeSha = '$sha'; \$NoDumps = \$$([ "$no_dumps" = 1 ] && echo true || echo false); \$RbVersion = '$rekordbox'; \$ResolveOnly = \$$([ "${2:-}" = resolve ] && echo true || echo false)"
}
# Stems Cache's elevated half through the owner's login (admin-worker.ps1), never the app's UAC
# prompt. VERB install|uninstall|check (check: install's staging, nothing run). 0 worker exit 0.
# The optional quiet mode reports status to the caller without logging the worker's paths.
cache_run() {
  local v="$1" pre out rc code
  pre="\$Verb = '$([ "$v" = check ] && echo install || echo "$v")'; \$Sid = '$t_sid'; \$Check = \$$([ "$v" = check ] && echo true || echo false)"
  out="$(timed "cache-worker-$v" kitps "$KITPS_WORKER_SECS" admin-worker.ps1 "$pre")"; rc=$?
  if [ "${2:-}" != quiet ]; then printf '%s\n' "$out" | sed 's/^/      /' | tee -a "$log"; fi
  if [ "$v" = check ]; then [ "$rc" = 0 ] && grep -qx 'WORKER ready (not started)' <<< "$out"; return; fi
  code="$(sed -n 's/^WORKER exit \([0-9]*\)$/\1/p' <<< "$out" | tail -1)"
  [ "$rc" = 0 ] && [ "$code" = 0 ]
}

# Put back the test account's temporary choice (cache install --chosen).
restore_cache_choice() {
  local out rc
  out="$(pcps "$test_login" 60 "if (Test-Path (Join-Path \$k 'chosen.ps1')) { & (Join-Path \$k 'chosen.ps1') -Mode restore } else { 'chosen: no chosen.ps1 staged' }")"; rc=$?
  printf '%s\n' "$out" | tee -a "$log" > /dev/null
  if [ "$rc" != 0 ] || ! grep -q '^chosen:' <<< "$out"; then
    row BAD "$test_user's support\\installed not checked (exit $rc)"; return 1
  elif ! grep -q '^chosen: nothing the kit set' <<< "$out"; then row OK "$(grep '^chosen:' <<< "$out" | head -1)"; fi
}

# Require the account to disappear from quser before opening a fresh session.
wait_signout() {
  local u end left login="$test_login"
  [ "$target" != vm ] || login="$owner"
  # fails closed: quser missing, failing or silent is never "signed out"; one 2-minute deadline
  q="-"; qrc=1; u="$(printf '%s' "$test_user" | sed 's/[.]/\\./g')"; end=$((SECONDS + 120))
  for _ in 1 2 3 4 5 6 7 8 9 10 11 12; do
    left=$((end - SECONDS)); [ "$left" -gt 0 ] || break; [ "$left" -le 30 ] || left=30
    q="$(pcps "$login" "$left" "if (-not (Get-Command quser.exe -ErrorAction SilentlyContinue)) { 'quser missing'; exit 2 }; \$q = @(quser.exe 2>&1 | ForEach-Object { \"\$_\" }); \$e = \$LASTEXITCODE; if (\$e -eq 1 -and (\$q -join ' ') -match 'No User exists') { 'signed out'; exit 0 }; if (\$e -ne 0 -or -not \$q.Count) { 'quser failed: exit ' + \$e + ': ' + (\$q -join ' '); exit 2 }; if (@(\$q | Select-Object -Skip 1 | Where-Object { \$_ -match '^[ >]\\s*$u\\s' }).Count) { 'signed in' } else { 'signed out' }")"; qrc=$?
    [ "$qrc" = 0 ] && [ "$q" = "signed out" ] && break
    sleep "${RBKIT_POLL:-10}"
  done
  [ "$qrc" = 0 ] && [ "$q" = "signed out" ]
}

# Read once, including empty values. Reject the entire list before serializing it.
protected_disks() {
  git -C "$root" config --null --get-all rbstemsplus.pcProtectedDisks 2> /dev/null |
    perl -0 -MMIME::Base64 -MEncode -e '
      my @entries;
      while (<STDIN>) {
        chomp;
        my $bytes = decode_base64($_);
        exit 2 unless length && encode_base64($bytes, "") eq $_;
        my $entry = eval { decode("UTF-8", $bytes, Encode::FB_CROAK) };
        exit 2 if $@ || $entry =~ /\p{Cc}/;
        my ($serial, $model, @extra) = split /\|/, $entry, -1;
        exit 2 if @extra || !defined $model || $model !~ /\S/ ||
          $serial !~ /\S/ || $serial =~ /\A\s|\s\z/;
        push @entries, $_;
      }
      exit 1 unless @entries;
      print "$_\n" for @entries;
    '
}
protected_ps() {
  local p out="@("
  while IFS= read -r p; do out="$out'$p',"; done <<< "$1"
  printf '%s)' "${out%,}"
}
t5_serial_ok() { text_ok "$1" && [[ "$1" != ' '* && "$1" != *' ' ]]; }

case "$cmd" in
disk)
  [ -n "$owner" ] || die "disk probe needs the owner's PC login"
  if ! ident; then row BAD "the test account's PC could not be verified"; exit 1; fi
  out="$(kitps "$KITPS_SHORT_SECS" disk-probe.ps1 "\$Drive = '$drive'")"; rc=$?
  idl="$(printf '%s\n' "$out" | sed -n 's/^ID \([A-Za-z0-9+\/=]*\) NAME .*/\1/p' | head -1)"; out="$(printf '%s\n' "$out" | grep -v '^ID ' | head -1)"
  if [ "$rc" = 0 ] && [[ "$out" =~ ^DISK\ $drive:\ type\ (Removable|Fixed)\ bus\ (USB|unknown)\ ready\ (True|False)\ fs\ [A-Za-z0-9_-]+$ ]]; then
    row OK "$out"
    if [ -n "$idl" ]; then
      if ! protected_list="$(protected_disks)"; then row NOTE "disk $drive: key $idl; no valid protection list is configured, so the kit changes no disk (to protect this one: git config --local --add rbstemsplus.pcProtectedDisks $idl)"
      elif grep -Fxq -- "$idl" <<< "$protected_list"; then row NOTE "disk $drive: a protected disk (rbstemsplus.pcProtectedDisks): read only, never changed or written to"
      else row NOTE "disk $drive: key $idl (to protect it: git config --local --add rbstemsplus.pcProtectedDisks $idl)"; fi
    fi
  else
    row BAD "disk probe (exit $rc): $out"
    exit 1
  fi ;;
t5)
  [ -n "$owner" ] || die "t5 $kind needs the owner's PC login"
  t5_prelude="\$Phase = '$kind'"
  if [ "$kind" != show ]; then
    if ! protected_list="$(protected_disks)"; then
      row BAD "t5 $kind: no valid protected disk list (rbstemsplus.pcProtectedDisks): no disk action"; exit 1
    fi
    serial="$(cfg rbstemsplus.pcT5Serial)"
    t5_serial_ok "$serial" || die "t5 $kind: set rbstemsplus.pcT5Serial to the dedicated T5's serial"
    t5_prelude="$t5_prelude; \$T5Serial = '$serial'; \$Protected = $(protected_ps "$protected_list")"
  fi
  if ! ident; then row BAD "the test account's PC could not be verified"; exit 1; fi
  if [ "$kind" != rows ]; then
    kitps 300 t5-app.ps1 "$t5_prelude; \$TestUser = '$test_user'" | tee -a "$log"; rc="${PIPESTATUS[0]}"
    case "$rc" in
      0) if grep -q '^CHANGED OUTSIDE' "$log"; then row BAD "t5 $kind: something outside our folder on the T5 changed"; exit 1; fi; row OK "t5 $kind" ;;
      3) row NOTE "t5 $kind: the configured Samsung Portable SSD T5 was not found on USB"; exit 3 ;;
      *) row BAD "t5 $kind: exit $rc"; exit 1 ;;
    esac
    exit 0
  fi
  w="${wintest:-$root/build/winc/win-x64/winc-location-wintest.exe}"; [ -f "$w" ] || die "no $w (win/c's x64 build)"
  i="${idle:-10}"; wsha="$(shasum -a 256 "$w" | cut -c1-64)"
  timed scp within 120 scp -q -o BatchMode=yes -- "$w" "$owner:$KITPS_DIR/winc-location-wintest.exe" < /dev/null || { row BAD "t5: the copy of $w failed"; exit 1; }
  kitps $((i * 60 + 600)) t5-app.ps1 "$t5_prelude; \$Wintest = '$wsha'; \$IdleMinutes = $i" | tee -a "$log"; rc="${PIPESTATUS[0]}"
  case "$rc" in
    0) if grep -q '^CLEAN: the listing matches' "$log"; then row OK "t5: J28 and the sleeping-disk row read; the T5's other contents unchanged"
       else row BAD "t5: the T5's listing changed (see CLEAN above)"; exit 1; fi ;;
    3) row NOTE "t5: the configured Samsung Portable SSD T5 was not found on USB: J28's and the sleeping-disk rows wait for it"; exit 3 ;;
    *) row BAD "t5 rows: exit $rc"; exit 1 ;;
  esac ;;
preflight)
  say "preflight: $test_user on the $machine ($stamp)"
  out="$(pcps "$test_login" 40 "'reached ' + \$env:USERNAME")"; rc=$?
  if [ "$rc" != 0 ]; then row BAD "$connection (exit $rc; the $machine off or asleep?)"; echo "$bad not OK"; exit 1; fi
  row OK "$connection (${out#reached })"
  pcps "$test_login" 600 "if (-not (Test-Path (Join-Path \$k 'verify-kit.ps1'))) { 'no kit'; exit 9 }; & (Join-Path \$k 'verify-kit.ps1'); exit \$LASTEXITCODE" | tee -a "$log"
  v="${PIPESTATUS[0]}"
  case "$v" in 0) row OK "the kit matches its SHA256SUMS" ;; 9) row BAD "no kit on the $machine: run stage" ;; *) row BAD "the kit doesn't match its SHA256SUMS: stage again" ;; esac
  if [ "$v" != 0 ]; then row SKIP "midi-auto ensure (the kit isn't verified)"
  else auto 180 -Do ensure && row OK "midi-auto ensure (task, loopback, mapping, OCR)" || row BAD "midi-auto ensure (see the BAD lines above)"; fi
  # rekordbox's connections (read-only: the owner keeps it online), the test CA, sync settings
  pcps "$test_login" 120 "& (Join-Path \$k 'fw.ps1') -Mode Check${rekordbox:+ -RbVersion '$rekordbox'} | Select-String 'processes running|NON-LOOPBACK'; \$c = Join-Path \$k 'dist\\test-ca.cer'; if (Test-Path \$c) { \$t = (New-Object Security.Cryptography.X509Certificates.X509Certificate2 \$c).Thumbprint; 'ca ' + @(Get-ChildItem Cert:\\CurrentUser\\Root | Where-Object Thumbprint -eq \$t).Count } else { 'ca none' }" > "$log.tmp"
  sed 's/^/      /' "$log.tmp" | grep -v '^ *ca ' | tee -a "$log"
  case "$(grep '^ca ' "$log.tmp")" in
    "ca 1") row OK "the test CA is trusted in the account's Root store" ;;
    "ca none") row SKIP "no dist/test-ca.cer in the kit (stage --dist)" ;;
    *) row NOTE "the test CA is not trusted yet: the owner adds it (one click, README)" ;;
  esac
  rm -f "$log.tmp"
  if [ -z "$server" ]; then
    if [ "$target" = vm ]; then row SKIP "the TEST server (RBKIT_VM_SERVER is empty)"
    else row SKIP "the TEST server (not set: --server or git config --local rbstemsplus.pcTestServer)"; fi
  else
    pcps "$test_login" 120 "& (Join-Path \$k 'net-probe.ps1') -Server '$server' ${bootstrap_server:+-BootstrapServer '$bootstrap_server'}; exit \$LASTEXITCODE" | tee -a "$log"
    [ "${PIPESTATUS[0]}" = 0 ] && row OK "the TEST server $server over HTTP and HTTPS" || row BAD "the TEST server $server (is serve running on the Mac?)"
  fi
  [ -x "$NOTIFY" ] && row OK "the notifier ($NOTIFY)" || row BAD "no notifier at $NOTIFY: a STOP would reach nobody"
  row NOTE "by eye, once per session: rekordbox's Cloud Library Sync off, no cloud drive, Collection Auto Upload off"
  say "$bad not OK"; [ "$bad" = 0 ] || exit 1 ;;

stage)
  for d in "$kit" "$corpus" "$formats" "$dist" "$from_dist" "$uia"; do
    [ -n "$d" ] || continue
    while [[ "$d" == */ ]]; do d="${d%/}"; done
    p="$(find "$d" \( -type l -o -name '.*' \) -print -quit)" || die "couldn't inspect $d"
    [ -z "$p" ] || die "stage: symlink or dot path: $p"
  done
  st="$(mktemp -d "$OUT/stage-$stamp.XXXXXX")" || die "couldn't make a local stage in $OUT"
  fmt_count=0
  if [ -n "$formats" ]; then
    [ -f "$formats/pc-rows.tsv" ] && ftsv="$formats/pc-rows.tsv" || ftsv="$formats/rows.tsv"
    fmt_count="$(python3 - "$formats" "$ftsv" "$format_tier" "$format_rows" "$st/formats-upload" << 'EOF'
import csv, hashlib, os, re, shutil, sys

fmt_dir, tsv_path, tier, row_spec, out_dir = sys.argv[1:6]
sums_path = os.path.join(fmt_dir, "SHA256SUMS")

def die2(msg):
    sys.stderr.write(f"pc-session: {msg}\n")
    sys.exit(2)

def fail1(msg):
    sys.stderr.write(f"pc-session: {msg}\n")
    sys.exit(1)

sums = {}
with open(sums_path, "r", encoding="utf-8") as f:
    for line in f:
        line = line.rstrip("\r\n")
        if not line:
            continue
        m = re.match(r"^([0-9a-fA-F]{64})  (.+)$", line)
        if not m:
            fail1(f"bad SHA256SUMS line in {sums_path}: {line}")
        sums[m.group(2)] = m.group(1).lower()

rows = []
by_name = {}
with open(tsv_path, "r", encoding="utf-8", newline="") as f:
    reader = csv.DictReader(f, delimiter="\t")
    if not reader.fieldnames or not {"row", "file", "scope"}.issubset(set(reader.fieldnames)):
        die2(f"formats TSV missing required columns (row, file, scope): {tsv_path}")
    for r in reader:
        name = (r.get("row") or "").strip()
        fname = (r.get("file") or "").strip()
        scope = (r.get("scope") or "").strip()
        if name.lower() in by_name or name != os.path.splitext(fname)[0] or not re.match(r"^fmt-[A-Za-z0-9._~-]+\.[A-Za-z0-9]+$", fname):
            die2(f"invalid format row entry in {tsv_path}: row={name!r} file={fname!r}")
        by_name[name] = len(rows)
        if name.lower() != name:
            die2("format row IDs must be lowercase")
        rows.append((name, fname, scope, (r.get("sha256") or "").strip().lower()))

selected_idx = []
seen = set()
if tier:
    for i, (_, _, scope, _) in enumerate(rows):
        if tier == "all" or scope == tier:
            selected_idx.append(i)
elif row_spec:
    for part in row_spec.split(","):
        if ":" in part:
            a, b = part.split(":", 1)
            if a not in by_name:
                die2(f"--format-rows: unknown row {a}")
            if b not in by_name:
                die2(f"--format-rows: unknown row {b}")
            ia, ib = by_name[a], by_name[b]
            if ia > ib:
                die2(f"--format-rows: inverted range {a}:{b}")
            rng = range(ia, ib + 1)
        else:
            if part not in by_name:
                die2(f"--format-rows: unknown row {part}")
            rng = (by_name[part],)
        for idx in rng:
            if idx not in seen:
                seen.add(idx)
                selected_idx.append(idx)

if not selected_idx:
    die2("formats selection matched 0 rows")

os.makedirs(out_dir, exist_ok=True)
catalog = "".join(f"{name}\n" for name, _, _, _ in rows).encode("ascii")
catalog_hash = hashlib.sha256(catalog).hexdigest()
with open(os.path.join(out_dir, f"catalog-{catalog_hash}.txt"), "wb") as out:
    out.write(catalog)

for idx in selected_idx:
    name, fname, _, tsv_sha = rows[idx]
    src = os.path.join(fmt_dir, fname)
    if not os.path.isfile(src):
        fail1(f"missing format file: {src}")
    if fname not in sums:
        fail1(f"format file not listed in SHA256SUMS: {fname}")
    h = hashlib.sha256()
    with open(src, "rb") as rf:
        for chunk in iter(lambda: rf.read(65536), b""):
            h.update(chunk)
    digest = h.hexdigest().lower()
    if digest != sums[fname] or (tsv_sha and digest != tsv_sha):
        fail1(f"SHA-256 mismatch for {fname}")
    shutil.copy2(src, os.path.join(out_dir, fname))

print(len(selected_idx))
EOF
)" ; rc=$?
    case "$rc" in 0) ;; 2) exit 2 ;; *) exit 1 ;; esac
  fi
  formats_upload=""
  if [ -n "$formats" ]; then
    formats_upload="$st/formats-upload"
    (cd "$formats_upload" && shasum -a 256 catalog-*.txt fmt-* > SHA256SUMS) || die "couldn't hash the format subset"
  fi
  cp "$kit"/*.ps1 "$st/"
  # from dist only what the kit reads: payload.json, and the test CA in the form Windows loads
  if [ -n "$dist" ]; then
    mkdir -p "$st/dist" && cp "$dist/payload.json" "$st/dist/" || die "couldn't copy $dist/payload.json"
    if [ -f "$dist/test-ca.pem" ]; then openssl x509 -in "$dist/test-ca.pem" -outform der -out "$st/dist/test-ca.cer" || die "couldn't convert $dist/test-ca.pem"
    else say "stage: no test-ca.pem in $dist (net-probe -Pinned needs it)"; fi
  fi
  if [ -n "$from_dist" ]; then
    mkdir -p "$st/m12" || die "couldn't make the M12 stage"
    cp "$from_dist/payload.json" "$st/m12/old.json" &&
      cp "$dist/payload.json" "$st/m12/new.json" &&
      { shasum -a 256 < "$from_dist/bootstrap.ps1" | cut -d' ' -f1; } > "$st/m12/old-bootstrap.sha" &&
      { shasum -a 256 < "$dist/bootstrap.ps1" | cut -d' ' -f1; } > "$st/m12/new-bootstrap.sha" || die "couldn't stage the M12 manifests"
    if [ -n "$installer" ]; then
      shasum -a 256 < "$dist/$installer" | cut -d' ' -f1 > "$st/m12/installer.sha" || die "couldn't hash the installer fixture"
    fi
  fi
  [ -z "$uia" ] || cp -R "$uia" "$st/uia" || die "couldn't stage rbui"
  [ -z "$corpus" ] || { mkdir -p "$st/corpus" && cp -R "$corpus/v1" "$corpus/win" "$st/corpus/"; }
  # the research probe: only staged here; the admin login checks its hash and places it (probe install)
  [ -z "$probe" ] || { mkdir -p "$st/probe" && cp "$probe" "$st/probe/onnxruntime.dll"; } || die "couldn't copy $probe"
  # the DirectML bench's helper (a --test-timing build): run only by run dml-bench, as the test account
  [ -z "$helper" ] || { mkdir -p "$st/bench" && cp "$helper" "$st/bench/rbsp-prepare.exe"; } || die "couldn't copy $helper"
  (cd "$st" && find . -path './formats-upload' -prune -o -type f ! -path './SHA256SUMS' -print | sed 's|^\./||' | LC_ALL=C sort | while IFS= read -r f; do shasum -a 256 "$f" || exit; done > SHA256SUMS) || die "couldn't hash $st"
  (cd "$st" && shasum -a 256 -c --quiet SHA256SUMS) || die "the stage doesn't match its own SHA256SUMS"
  say "stage: $(grep -c . "$st/SHA256SUMS") files in $st"
  format_change=0
  if [ "$clear_formats" = 1 ] || [ -n "$formats" ]; then
    format_change=1
    # The guard runs before scp, including before the script that publishes the subset.
    # Check ancestors and existing children without descending through reparse points.
    guard="$(cat <<'EOF'
$ErrorActionPreference = 'Stop'
$full = [IO.Path]::GetFullPath($k); $root = [IO.Path]::GetPathRoot($full)
if ($root -ne ($env:SystemDrive + [char]92)) { throw 'formats: destination must be on the system volume' }
$p = $root
foreach ($part in ($full.Substring($root.Length) -split '\\')) {
  $p = Join-Path $p $part
  try { $attrs = [IO.File]::GetAttributes($p) }
  catch [IO.FileNotFoundException], [IO.DirectoryNotFoundException] { break }
  if ($attrs -band [IO.FileAttributes]::ReparsePoint) { throw 'formats: reparse point in destination' }
}
function CheckStage($dir) {
  foreach ($f in [IO.DirectoryInfo]::new($dir).EnumerateFileSystemInfos()) {
    if ($f.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'formats: reparse point in destination' }
    if ($f -is [IO.DirectoryInfo]) { CheckStage ($dir + [char]92 + $f.Name) }
  }
}
if ([IO.Directory]::Exists($full)) { CheckStage ('\\?\' + $full) }
$active = Join-Path $full 'corpus\formats'
$marker = Join-Path $active 'RBSP-FORMATS'
if ((Test-Path -LiteralPath $active) -and
    (-not [IO.File]::Exists($marker) -or [IO.File]::ReadAllText($marker) -cne 'RBSP format subset v1')) {
  throw 'formats: directory lacks the kit ownership marker. Preserve or move it manually before restaging.'
}
EOF
)"
  else guard="\$ErrorActionPreference = 'Stop'"; fi
  out="$(pcps "$test_login" 60 "$guard
New-Item -ItemType Directory -Force \$k | Out-Null; \$env:TEMP; \$m = Join-Path \$k 'SHA256SUMS'; if (Test-Path -LiteralPath \$m) { Get-Content -LiteralPath \$m }")"; rc=$?
  tmp="${out%%$'\n'*}"
  if [ "$rc" != 0 ] || [[ "$tmp" != [A-Za-z]:\\* ]]; then
    lost "$rc" stage || true
    say "stage: the $machine's stage path refused or unavailable (exit $rc)"
    exit 1
  fi
  # Fresh entries replace the same paths. A supplied corpus also refreshes its two derived files.
  # Format changes use a separate manifest inside the owned subset. Drop stale legacy entries.
  awk -v refresh="${corpus:+1}" -v formats="$format_change" 'NR == FNR { have[tolower(substr($0,67))]; print; next }
    { p=tolower(substr($0,67)); if (p ~ /^(corpus|dist|probe|m12|uia|bench)\// && !(p in have) && !(refresh && p ~ /^corpus\/(long\/|long-path\.txt$)/) && !(formats && p ~ /^corpus\/formats\//)) print }' \
    "$st/SHA256SUMS" <(tail -n +2 <<< "$out") > "$st/.sums" &&
    mv "$st/.sums" "$st/SHA256SUMS" || die "couldn't merge the kept manifest"
  dest="$(printf '%s' "$tmp" | tr '\\' '/')/rbsp-kit"
  upload=()
  for f in "$st"/*; do [ "$f" = "$formats_upload" ] || upload+=("$f"); done
  put 1800 r "$test_login" "$dest/" "${upload[@]}" || { say "stage: $copy_method to $dest failed"; exit 1; }
  # An earlier stage had a separate T5 rows script. Drop that one obsolete file before verification.
  pcps "$test_login" 600 "if (-not (Test-Path (Join-Path \$k 'verify-kit.ps1'))) { 'no verify-kit.ps1'; exit 9 }; \$obsolete = Join-Path \$k 't5-rows.ps1'; if (Test-Path -LiteralPath \$obsolete -PathType Leaf) { Remove-Item -LiteralPath \$obsolete -Force -ErrorAction Stop }; & (Join-Path \$k 'verify-kit.ps1') ${corpus:+-RefreshLongPath}; exit \$LASTEXITCODE" | tee -a "$log"
  # a kit that doesn't verify goes no further
  if [ "${PIPESTATUS[0]}" != 0 ]; then row BAD "the kit on the $machine doesn't match the stage's SHA256SUMS"; say "$bad not OK"; exit 1; fi
  row OK "the kit on the $machine matches the stage's SHA256SUMS"
  if [ "$clear_formats" = 1 ] || [ -n "$formats" ]; then
    transfer="$(uuidgen | tr -d '-' | tr '[:upper:]' '[:lower:]')"
    [[ "$transfer" =~ ^[a-f0-9]{32}$ ]] || die 'could not create format transfer ID'
    format_stage() {
      local manifest_arg=""
      [ -z "${2:-}" ] || manifest_arg="-ManifestSha '$2'"
      pcps "$test_login" 600 "& (Join-Path \$k 'format-stage.ps1') -Mode '$1' -Transfer '$transfer' $manifest_arg; exit \$LASTEXITCODE" | tee -a "$log"
      return "${PIPESTATUS[0]}"
    }
    if [ "$clear_formats" = 1 ]; then
      format_stage clear || exit 1
      if [ -z "$corpus$dist$from_dist$helper$probe$uia" ]; then
        row OK "formats: active subset cleared, media retained"
        say "$bad not OK"
        exit 0
      fi
    else
      manifest_sha="$(shasum -a 256 "$formats_upload/SHA256SUMS" | cut -d' ' -f1)"
      format_stage prepare || exit 1
      if ! put 1800 '' "$test_login" "$dest/corpus/formats-incoming-$transfer/" "$formats_upload"/*; then
        say 'stage: format upload failed; the previous verified subset is kept'
        format_stage discard
        exit 1
      fi
      if ! format_stage publish "$manifest_sha"; then
        format_stage discard
        exit 1
      fi
    fi
  fi
  if auto 180 -Do ensure && ! stopped; then row OK "midi-auto ensure"
  else
    row BAD "midi-auto ensure"
    say "$bad not OK"; exit 1
  fi
  serial=""; [ "$target" != pc ] || serial="$(cfg rbstemsplus.pcT5Serial)"
  if [ -n "$serial" ]; then
    if ! protected_list="$(protected_disks)"; then row SKIP "T5 identity: no valid protected disk list (rbstemsplus.pcProtectedDisks)"
    elif ! t5_serial_ok "$serial"; then row BAD "stage: invalid rbstemsplus.pcT5Serial"
    elif [ -z "$owner" ] || ! ident; then row BAD "stage: T5 identity needs the owner's login on the verified PC"
    else
      out="$(kitps "$KITPS_SHORT_SECS" t5-app.ps1 "\$Phase = 'check'; \$T5Serial = '$serial'; \$Protected = $(protected_ps "$protected_list")")"; rc=$?
      if [ "$rc" = 0 ] && [[ "$out" == 'T5: verified '* ]]; then row OK "$out"
      else row BAD "stage: T5 identity (exit $rc): $out"; fi
    fi
  fi
  if [ -n "$formats" ]; then
    row OK "formats: $fmt_count file(s) staged and verified in corpus\\formats"
    row NOTE "run format-import looks for a matching RBSP Formats playlist before importing"
  elif [ "$clear_formats" = 1 ]; then
    row OK "formats: active subset cleared, media retained"
  fi
  say "$bad not OK"; [ "$bad" = 0 ] || exit 1 ;;

run)
  case "$kind" in
  T2 | hits)
    if [ "$formats_only" = 1 ]; then
      mode=""
      [ "$confirm_import" = 0 ] || mode="-ConfirmImportRefusals"
      if ! auto 150 -Do quit || stopped || ! auto 220 -Do format-verify || stopped; then
        stops 'run hits --formats' || true
        exit 1
      fi
      trap '
        rc=$?
        trap - EXIT
        if { [ "$rc" = 0 ] || [ "$rc" = 3 ]; } && ! stopped; then
          if ! auto 150 -Do quit -Formats || stopped; then
            say "formats: STOP: cache pass close unconfirmed"
            rc=1
          fi
        fi
        finish_timing "$rc"
        exit "$rc"
      ' EXIT
      trap 'exit 130' INT
      trap 'exit 143' TERM
      trap 'exit 129' HUP
      if ! auto 300 -Do start -Formats || stopped; then
        stops 'run hits --formats' || true
        exit 1
      fi
      pass="$(format_run_id)"
      [[ "$pass" =~ ^[a-f0-9]{32}$ ]] || die 'could not create cache pass ID'
      continuation=""; result=0
      for ((batch = format_batch; batch <= format_last; batch++)); do
        dir="$OUT/formats-cache-batch-$batch-$pass"
        mkdir "$dir" && out_ok "$dir" 'hits.log' || die "could not make $dir"
        pass_args=()
        [ "$format_batch" = "$format_last" ] || pass_args+=(-FormatPass)
        [ -z "$continuation" ] || pass_args+=(-FormatContinuation "'$continuation'")
        auto 2700 -Do format-hits -FormatBatch "$batch" $mode ${pass_args[@]+"${pass_args[@]}"}; rc=$?
        cp "$step" "$dir/hits.log" || exit 1
        say "formats: batch $batch cache evidence in $dir/hits.log"
        if { [ "$rc" != 0 ] && [ "$rc" != 3 ]; } || stopped; then
          stops 'run hits --formats' || true
          exit 1
        fi
        [ "$rc" != 3 ] || result=3
        if [ "$batch" -lt "$format_last" ]; then
          continuation="$(format_continuation)" || die 'invalid format pass continuation'
        fi
      done
      exit "$result"
    fi
    m="${minutes:-90}"
    a="-Do run -Expect $([ "$kind" = hits ] && echo hits || echo any)"
    # --keys: the probe's log stands in for the bridge's; each load must log KEY lines (mix dumps),
    # and a load is done after 30 quiet seconds (the probe logs once a chunk)
    [ "$probekeys" = 0 ] || a="-Do run -Expect keys -Activity '$PROBE_LOG' -Quiet 30"
    [ "$aac" = 0 ] || a="$a -Aac"
    if [ "$fresh" = 1 ]; then
      auto 150 -Do quit; rc=$?
      if [ "$rc" != 0 ] || stopped; then stops "run T2" || notify "run T2" "STOP before fresh start" || true; exit 1; fi
      a="$a -Warmup 90"
    fi
    [ -z "$from" ] || a="$a -From '$from'"
    [ -z "$to" ] || a="$a -To '$to'"
    [ -z "$max" ] || a="$a -Max $max"
    [ -z "$inject" ] || a="$a -InjectAt $inject"
    a="$a -Minutes $m"   # the PC stops at the deadline itself
    say "run $kind: start, $a, $([ "$keep" = 1 ] && echo 'rekordbox left open' || echo quit) (up to $m min)"
    auto 300 -Do start; rc=$?
    if [ "$rc" != 0 ]; then stops "run $kind" || lost "$rc" "run $kind start" || notify "run $kind" "rekordbox didn't start (exit $rc)" || true; exit 1; fi
    auto $((m * 60 + 720)) $a; rc=$?   # a row in progress finishes: up to MaxTrack 420 s + 300
    if gone "$rc"; then stops "run $kind" || lost "$rc" "run $kind" || true; exit 1; fi
    # rekordbox is quit only after a run that ended as planned (0, or 3: rows not as expected)
    if { [ "$rc" != 0 ] && [ "$rc" != 3 ]; } || stopped; then
      stops "run $kind" || notify "run $kind" "exit $rc without a STOP line" || true
      say "run $kind: exit $rc; rekordbox left as it is"
      [ "$rc" = 0 ] && exit 1; exit "$rc"
    fi
    if [ "$keep" = 0 ]; then
      auto 150 -Do quit; q=$?
      if [ "$q" != 0 ]; then stops "run $kind quit" || lost "$q" "run $kind quit" || notify "run $kind" "quit exit $q" || true; rc=1; fi
    fi
    say "run $kind: exit $rc"; exit "$rc" ;;
  prepare)
    m="${minutes:-60}"
    pt=""; [ "$pausetest" = 1 ] && pt="-PauseTest"
    fm=""; [ "$formats_only" = 0 ] || fm="-Formats"
    auto $((m * 60 + 720)) -Do prepare -Playlist "'$playlist'" -Minutes "$m" $pt $fm; rc=$?
    if gone "$rc"; then stops "run prepare" || lost "$rc" "run prepare" || true; exit 1; fi
    if [ "$rc" != 0 ]; then stops "run prepare" || notify "run prepare" "exit $rc without a STOP line" || true; fi
    exit "$rc" ;;
  format-uidump)
    auto 120 -Do format-uidump ${explore:+"$explore"} && ! stopped || exit 1 ;;
  winlist)
    auto 60 -Do winlist && ! stopped || exit 1
    while IFS= read -r path; do
      remote="${path//\\//}"
      [[ "$remote" =~ ^[Cc]:/([A-Za-z0-9_.\ -]+/)+rbst/shots/winlist-[a-f0-9]{32}-0x[0-9A-F]+\.png$ &&
         "$remote" != */../* && "$remote" != */./* ]] || die 'winlist: invalid crop path'
      file="${remote##*/}"
      dir="$OUT/shots"
      mkdir -p "$dir" || die "winlist: can't make $dir"
      out_ok "$dir" "$file" || die 'winlist: crop path must be git-ignored inside the checkout'
      get 60 '' "$test_login" "$dir/" "$remote" < /dev/null || exit 1
      say "winlist: $dir/$file"
    done < <(sed -n 's/^WINLIST SHOT //p' "$step") ;;
  dismiss-rb-error)
    auto 60 -Do desktop -Step "'dismiss-rb-error'" -Seconds 30 && ! stopped || exit 1 ;;
  format-step)
    rekordbox="${rekordbox:-$format_step_version}"
    auto 120 -Do format-step -Argument "'$format_action'" && ! stopped || exit 1 ;;
  library-check)
    auto 220 -Do library-check && ! stopped || exit 1
    report="$(sed -n 's/^LIBRARY CHECK //p' "$step")"
    [[ "$report" == [Cc]:\\*"\\rbst\\library-check-"*.json && "$report" != *$'\n'* &&
       "${report##*\\}" =~ ^library-check-[a-f0-9]{32}\.json$ ]] || die 'library-check: invalid summary path'
    get 120 '' "$test_login" "$OUT/" "${report//\\//}" || exit 1
    say "library-check: $OUT/${report##*\\}" ;;
  format-import)
    if [ -z "$resume_import$fresh_format_import" ]; then
      if ! auto 150 -Do quit || stopped; then
        stops 'run format-import' || notify 'run format-import' 'close before playlist reuse failed' || true
        exit 1
      fi
      auto 220 -Do format-reuse; rc=$?
      if [ "$rc" = 4 ] && ! stopped && grep -Fxq 'FORMAT REUSE NONE' "$step"; then
        say 'formats: no reusable playlist matches the staged subset'
      elif [ "$rc" = 0 ] && ! stopped; then
        exit 0
      else
        stops 'run format-import' || notify 'run format-import' 'format playlist reuse failed' || true
        exit 1
      fi
    fi
    if { [ -n "$resume_import" ] ||
         { auto 300 -Do start -Formats && ! stopped && auto 460 -Do format-import && ! stopped; }; } &&
       auto 150 -Do quit && ! stopped &&
       auto 220 -Do format-verify && ! stopped; then exit 0; fi
    stops 'run format-import' || notify 'run format-import' 'format import or close failed' || true
    exit 1 ;;
  close)
    auto 150 -Do quit; rc=$?
    [ "$rc" = 0 ] && ! stopped || exit 1 ;;
  start)
    auto 300 -Do start && ! stopped || exit 1 ;;
  t2-facts | t2-probe | t2-model-on | t2-model-off)
    case "$kind" in
      # our app closed by its own Exit first and started again after (owner decision 2026-11-19)
      t2-model-*) auto 90 -Do appexit && auto 305 -Do desktop -Step "'$kind'" -Seconds 280 && auto 120 -Do appopen ;;
      *) auto 120 -Do "$kind" ;;
    esac
    rc=$?
    [ "$rc" = 0 ] && ! stopped || exit 1 ;;
  dml-bench)
    # the helper's DirectML on the PC's GPU against its CPU (coordinator decision 2026-11-19), with
    # Stems Cache installed (the helper runs on our root's runtime): the timing with rekordbox closed,
    # then run hits on the same rows (rekordbox serving the --dml entries), then best effort deck 1
    # playing during a --dml run of another row (09-flac-16)
    f="${from:-19-mp3-cbr128}"; t="${to:-20-wav-16-30s}"; bench_bad=0
    if [ -n "$helper" ]; then RBKIT_AUTO=1 "$0" stage --helper "$helper" | tee -a "$log"; [ "${PIPESTATUS[0]}" = 0 ] || { say "run dml-bench: stage --helper failed"; exit 1; }; fi
    auto 150 -Do quit; rc=$?
    if [ "$rc" != 0 ] || stopped; then stops "run dml-bench" || lost "$rc" "run dml-bench quit" || true; exit 1; fi
    auto 4200 -Do dmlbench -From "'$f'" -To "'$t'" -Minutes 30; rc=$?
    if gone "$rc"; then stops "run dml-bench" || lost "$rc" "run dml-bench" || true; exit 1; fi
    case "$rc" in
      0) row OK "DirectML ran the helper's model (timings above)" ;;
      3) row NOTE "DirectML fell back to the CPU (its lines above)" ;;
      *) stops "run dml-bench" || notify "run dml-bench" "exit $rc" || true; exit 1 ;;
    esac
    RBKIT_AUTO=1 "$0" run hits --from "$f" --to "$t" --keep-open ${rekordbox:+--rekordbox "$rekordbox"} 2>&1 | tee -a "$log"; h="${PIPESTATUS[0]}"
    if [ "$h" = 0 ]; then row OK "rekordbox served the --dml entries of $f..$t (run hits)"
    else row BAD "run hits on the --dml entries: exit $h"; bench_bad=1; fi
    if [ "$h" = 0 ] || [ "$h" = 3 ]; then
      auto 1200 -Do dmlstall -From "'09-flac-16'" -Minutes 15; s2=$?
      if gone "$s2" || stopped; then stops "run dml-bench" || lost "$s2" "run dml-bench stall" || true; exit 1; fi
      [ "$s2" = 0 ] || { row BAD "the playback check's --dml run: exit $s2"; bench_bad=1; }
      auto 150 -Do quit; q=$?
      if [ "$q" != 0 ] || stopped; then stops "run dml-bench quit" || lost "$q" "run dml-bench quit" || true; exit 1; fi
    else exit 1; fi
    [ "$bench_bad" = 0 ] || exit 3
    exit "$rc" ;;
  benchstop)
    # the kit's own bench helpers (its bench\rbsp-prepare.exe) stopped: one past its time holds our
    # app's ORT, and Stems Cache's install or uninstall then can't replace it
    auto 90 -Do benchstop; rc=$?
    if gone "$rc"; then lost "$rc" "run benchstop" || true; exit 1; fi
    if [ "$rc" = 0 ]; then row OK "the kit's bench helpers stopped"; exit 0; fi
    row BAD "the kit's bench helpers not all stopped (exit $rc)"; notify "run benchstop" "helpers left running" || true; exit 1 ;;
  loc)
    # plan N3's rows in our app's own UI (kit/loc-ui.ps1), one step a run
    [ -n "$locstep" ] || die "run loc needs --step"
    case "$locstep" in set | netmap | netunmap) [ -n "$locarg" ] || die "run loc --step $locstep needs --arg" ;; esac
    auto 500 -Do loc -LocStep "$locstep" -LocArg "'$locarg'"; rc=$?
    if gone "$rc"; then lost "$rc" "run loc $locstep" || true; exit 1; fi
    exit "$rc" ;;
  rowkeys)
    # the staged research helper's --keys on corpus rows (stage --helper first; read only:
    # the model only hashed, nothing written)
    [ -n "$from$rows" ] || [ "$formats_only" = 1 ] || die "run rowkeys needs --from ROW, --rows, or --formats"
    a="-Do rowkeys"
    [ "$formats_only" = 0 ] || a="$a -Formats"
    [ -z "$from" ] || a="$a -From '$from'"
    [ -z "$to" ] || a="$a -To '$to'"
    [ -z "$rows" ] || a="$a -Rows '$rows'"
    [ "$test_unproved_aac" = 0 ] || a="$a -TestUnprovedAac"
    [ "$test_unproved_mp3" = 0 ] || a="$a -TestUnprovedMp3"
    auto 400 $a; rc=$?
    if gone "$rc"; then lost "$rc" "run rowkeys" || true; exit 1; fi
    exit "$rc" ;;
  keycompare)
    # compare rekordbox's dumped keys (C:\rbtest\rows.txt) with our staged helper's --keys
    [ -n "$from$rows" ] || die 'run keycompare needs --from or --rows'
    if [ -n "$helper" ]; then
      "$0" stage --helper "$helper" || { row BAD "keycompare stage --helper failed"; exit 1; }
    fi
    a="-Do keycompare"
    if [ -n "${RBKIT_EVIDENCE_RUN:-}" ]; then
      [[ "$RBKIT_EVIDENCE_RUN" =~ ^[a-f0-9]{32}$ ]] || die 'invalid probe evidence run ID'
      a="$a -EvidenceRun '$RBKIT_EVIDENCE_RUN'"
    fi
    [ -z "$from" ] || a="$a -From '$from'"
    [ -z "$to" ] || a="$a -To '$to'"
    [ -z "$rows" ] || a="$a -Rows '$rows'"
    auto 1800 $a; rc=$?
    if gone "$rc"; then lost "$rc" "run keycompare" || true; exit 1; fi
    case "$rc" in
      0) row OK "keycompare: rekordbox's dumped keys match our helper's" ;;
      3) row NOTE "keycompare: mismatches or refusals found (see lines above and keycompare.tsv)" ;;
      *) stops "run keycompare" || notify "run keycompare" "exit $rc" || true ;;
    esac
    exit "$rc" ;;
  keptinfo)
    # read only: our app's kept folder (names, sizes, .kept headers without keys), the Status tab
    # as UI Automation sees it, app.log's last lines
    auto 500 -Do keptinfo; rc=$?
    if gone "$rc"; then lost "$rc" "run keptinfo" || true; exit 1; fi
    exit "$rc" ;;
  stopkeep)
    # the kept set --playlist dropped by our app's own Stop Keeping… (Status tab, confirmed)
    auto 200 -Do stopkeep -Playlist "'$playlist'"; rc=$?
    if gone "$rc"; then lost "$rc" "run stopkeep" || true; exit 1; fi
    if [ "$rc" = 0 ]; then row OK "'$playlist' no longer kept (our app's Stop Keeping…)"; exit 0; fi
    stops "run stopkeep" || notify "run stopkeep" "exit $rc" || true; exit 1 ;;
  playlists)
    # the installed helper's --playlists on its own, as the core runs it and with the whole
    # environment (coordinator 2026-11-20: no Prepare run listed playlists); rekordbox closed, or
    # with --open started first and quit after
    if [ "$plopen" = 1 ]; then auto 300 -Do start; rc=$?; else auto 150 -Do quit; rc=$?; fi
    if [ "$rc" != 0 ] || stopped; then stops "run playlists" || lost "$rc" "run playlists" || true; exit 1; fi
    auto 240 -Do playlists -Playlist "'$playlist'"; rc=$?
    if [ "$plopen" = 1 ]; then auto 150 -Do quit || { stops "run playlists quit" || true; exit 1; }; fi
    if gone "$rc"; then lost "$rc" "run playlists" || true; exit 1; fi
    case "$rc" in 0) row OK "the helper's --playlists lists '$playlist' both ways" ;; 3) row NOTE "the helper's --playlists: see its lines above" ;; *) row BAD "run playlists: exit $rc" ;; esac
    exit "$rc" ;;
  appswap)
    # our app replaced by the TEST build in --dist, served by the Mac's TEST server under --base
    # (its root when /): the installed one's own complete uninstall, then that build's bootstrap,
    # byte-checked against --dist/bootstrap.ps1 (coordinator 2026-11-19, the black-window fix)
    [ -n "$dist" ] && [ "$swapbase_set" = 1 ] || die "run appswap needs --dist DIR and --base NAME (or /)"
    [ -n "$server" ] || die "run appswap needs rbstemsplus.pcTestServer (the Mac's TEST server)"
    swap_check="\$ErrorActionPreference = 'Stop'; \$RbVersion = '$rekordbox'; . (Join-Path \$k 'kit-lib.ps1'); Get-AppSwapCacheState"
    state="$(pcps "$test_login" 60 "$swap_check" 2> /dev/null)"; rc=$?
    if [ "$rc" != 0 ]; then state=failed; fi
    swap_stop() { say "appswap: STOP: $1"; stops "run appswap" || true; exit 1; }
    case "$state" in
      clear) ;;
      leftover)
        say "appswap: clearing the leftover Stems Cache root through the admin cache uninstall path"
        [ -n "$owner" ] || swap_stop "cache uninstall needs the admin login (rbstemsplus.pcSsh)"
        ident 2> /dev/null || swap_stop "the test account could not be identified for cache uninstall"
        cache_run uninstall quiet 2> /dev/null || swap_stop "the admin cache uninstall could not clear the leftover Stems Cache root"
        state="$(pcps "$test_login" 60 "$swap_check" 2> /dev/null)" &&
          [ "$state" = clear ] || swap_stop "the Stems Cache root could not be confirmed absent after cache uninstall"
        ;;
      blocked) swap_stop "the Stems Cache root is present and rekordbox must be closed with only verifiable Pioneer runtimes" ;;
      *) swap_stop "the Stems Cache root could not be checked" ;;
    esac
    sha="$(shasum -a 256 "$dist/bootstrap.ps1" | cut -c1-64)"; sub="${swapbase:+/$swapbase}"
    auto 900 -Do appswap -Argument "'https://$server$sub|http://$server$sub|$sha'"; rc=$?
    if gone "$rc"; then stops "run appswap" || lost "$rc" "run appswap" || true; exit 1; fi
    if [ "$rc" = 0 ]; then row OK "our app swapped for the build in $dist (served at https://$server$sub)"; exit 0; fi
    stops "run appswap" || notify "run appswap" "exit $rc" || true; exit 1 ;;
  blackrepro)
    # our app's black window (coordinator 2026-11-19): a fresh app, then N cycles of minimize (unless
    # --no-minimize), session down, session up and a restore through UI Automation; every 5 cycles
    # its title, tabs and a screenshot's black share; the first black cycle, or none
    n="${cycles:-20}"; first=""
    auto 90 -Do appexit && auto 120 -Do appopen || { stops "run blackrepro" || lost "$?" "run blackrepro" || true; exit 1; }
    auto 120 -Do appcheck; c=$?
    drawn=0; white=0; black=0
    case "$c" in 0) drawn=1 ;; 3) black=1; say "blackrepro: the fresh app is BLACK" ;; 4) white=1; say "blackrepro: the fresh app is WHITE" ;; *) say "blackrepro: the fresh app is not checked (exit $c)"; exit 1 ;; esac
    for i in $(seq 1 "$n"); do
      if [ "$nomin" = 0 ]; then auto 60 -Do appmin || { say "blackrepro: cycle $i: not minimized"; exit 1; }; fi
      RBKIT_AUTO=1 "$0" session down 2>&1 | tee -a "$log" > /dev/null; [ "${PIPESTATUS[0]}" = 0 ] || { say "blackrepro: cycle $i: session down failed"; exit 1; }
      RBKIT_AUTO=1 "$0" session up 2>&1 | tee -a "$log" > /dev/null; [ "${PIPESTATUS[0]}" = 0 ] || { say "blackrepro: cycle $i: session up failed"; exit 1; }
      auto 120 -Do appopen || { say "blackrepro: cycle $i: the app not restored"; exit 1; }
      if [ $((i % ${every:-5})) = 0 ] || [ "$i" = "$n" ]; then
        auto 120 -Do appcheck; c=$?
        case "$c" in 0) drawn=$((drawn + 1)); say "blackrepro: cycle $i: drawn" ;; 3) black=$((black + 1)); say "blackrepro: cycle $i: BLACK"; first="${first:-$i}" ;; 4) white=$((white + 1)); say "blackrepro: cycle $i: WHITE"; first="${first:-$i}" ;; *) say "blackrepro: cycle $i: not checked (exit $c)"; exit 1 ;; esac
      fi
    done
    mode="$([ "$nomin" = 1 ] && echo 'without minimizing' || echo 'minimized each time')"
    say "blackrepro ($mode): checks drawn $drawn, white $white, black $black (the fresh app included)"
    if [ -n "$first" ]; then row NOTE "blackrepro ($mode): black first seen at the check after cycle $first of $n"; exit 3; fi
    row OK "blackrepro ($mode): no repro after $n cycles"; exit 0 ;;
  preconditions)
    # an auto plan's preconditions, all read in its first minute (coordinator 2026-11-19): every
    # failed one in one list (the PC's pre-lib.ps1 judges them)
    [ -n "$plan" ] || die "run preconditions needs --plan session2|prepare|record|dumps"
    auto 180 -Do preconditions -Plan "$plan"; rc=$?
    if gone "$rc"; then lost "$rc" "run preconditions" || true; exit 1; fi
    if [ "$rc" = 0 ]; then row OK "auto $plan: its preconditions"; exit 0; fi
    pf="$(sed -n 's/^.*preconditions: \([0-9]* failed: .*\)$/\1/p' "$step" | tail -1)"
    row BAD "auto $plan: $pf"; notify "auto $plan" "preconditions ${pf:-not met}" || true; exit 1 ;;
  autoanalysis-off)
    # rekordbox's Auto Analysis off through its Preferences (owner decision 2026-11-19, the test
    # account only): nothing when rekordbox3.settings already says 0; else rekordbox started, the
    # check box unticked, rekordbox quit, and the setting read back
    auto 60 -Do autoanalysis-read; rc=$?
    if [ "$rc" = 0 ]; then row OK "rekordbox's Auto Analysis already off"; exit 0; fi
    [ "$rc" = 3 ] || { lost "$rc" "run autoanalysis-off read" || say "run autoanalysis-off: the setting not read (exit $rc)"; exit 1; }
    auto 300 -Do start; rc=$?
    if [ "$rc" != 0 ] || stopped; then stops "run autoanalysis-off" || lost "$rc" "run autoanalysis-off start" || true; exit 1; fi
    auto 200 -Do autoanalysis; rc=$?
    if [ "$rc" != 0 ] || stopped; then stops "run autoanalysis-off" || lost "$rc" "run autoanalysis-off" || notify "run autoanalysis-off" "exit $rc" || true; exit 1; fi
    auto 150 -Do quit; rc=$?
    if [ "$rc" != 0 ] || stopped; then stops "run autoanalysis-off quit" || lost "$rc" "run autoanalysis-off quit" || true; exit 1; fi
    auto 60 -Do autoanalysis-read; rc=$?
    if [ "$rc" = 0 ]; then row OK "rekordbox's Auto Analysis off (Preferences; rekordbox3.settings reads 0)"; exit 0; fi
    row BAD "rekordbox's Auto Analysis: rekordbox3.settings does not read 0 after Preferences (exit $rc)"
    notify "run autoanalysis-off" "STOP: the setting did not read back as off" || true; exit 1 ;;
  directml)
    # M12's DirectML: the DLLs rekordbox (running, at the console) has loaded; 3 when DirectML isn't
    auto 90 -Do modules; rc=$?
    if gone "$rc"; then lost "$rc" "run directml" || true; exit 1; fi
    case "$rc" in 0) row OK "rekordbox loaded DirectML.dll" ;; 3) row NOTE "rekordbox did not load DirectML.dll (CPU stems)" ;; *) row BAD "run directml: exit $rc" ;; esac
    exit "$rc" ;;
  watchtest)
    # the STOP the test window causes goes out as a real notification: the whole path is tested
    auto 120 -Do watchtest; rc=$?
    if lost "$rc" "run watchtest"; then exit 1; fi
    if ! stops "run watchtest"; then say "watchtest: FAIL: no STOP line: the watch missed the test window"; exit 1; fi
    if [ "$sent" != 1 ]; then say "watchtest: FAIL: the notification was not sent"; exit 1; fi
    [ "$rc" = 0 ] || { say "watchtest: FAIL: exit $rc"; exit 1; }
    say "watchtest: PASS (the STOP line seen, the notification sent)" ;;
  esac ;;

collect)
  dir="${outdir:-$OUT/collect-$stamp}"; mkdir -p "$dir" || die "can't make $dir"
  out_ok "$dir" && out_ok "$dir" '' || die "--out $dir is in the checkout and not git-ignored"
  out="$(auto 600 -Do bundle)"; rc=$?
  zip="$(printf '%s\n' "$out" | sed -n 's/^BUNDLE //p' | tail -1)"
  [ "$rc" = 0 ] && [[ "$zip" =~ ^[A-Za-z]:\\[^\']+\.zip$ ]] || { lost "$rc" collect || true; say "collect: no bundle (exit $rc)"; exit 1; }
  get 1800 '' "$test_login" "$dir/" "$(printf '%s' "$zip" | tr '\\' '/')" || { say "collect: $copy_method of $zip failed"; exit 1; }
  (cd "$dir" && unzip -q -o "$(basename "${zip//\\//}")") || { say "collect: unzip failed (the screenshots stay on the PC)"; exit 1; }
  auto 60 -Do bundled -Keys "'$(basename "${zip//\\//}")'" > /dev/null || say "collect: the PC's copies of the screenshots not deleted (harmless)"
  say "collect: $(find "$dir" -type f | wc -l | tr -d ' ') files in $dir" ;;

m12)
  secs=290
  case "$kind" in
    signout)
      auto 60 -Do logoff
      rc=$?
      if [ "$rc" = 0 ] && ! stopped; then
        if wait_signout; then say "M12: signed out for RunOnce"; exit 0; fi
      fi
      say "m12 signout: STOP: sign-out not confirmed"
      exit 1 ;;
  esac
  arg=""
  case "$kind" in
    install-old | install-new | update | worker-source) arg="$bootstrap_server $server" ;;
    smartscreen) arg="$installer" ;;
  esac
  auto 320 -Do desktop -Step "'m12-$kind'" -Argument "'$arg'" -Seconds "$secs"
  rc=$?
  [ "$rc" = 0 ] && ! stopped || exit 1 ;;


auto)
  # One named sequence, each step this script's own command (its own log too, all of it here).
  # The first step that exits 1 or 2 ends the plan: nothing more is done on the PC (no cleanup:
  # after a STOP the PC is left as it is) and the owner is notified. Exit 3 (an expectation not
  # met) is recorded and the plan goes on; the plan's exit is then 3.
  [ "$kind" != record ] || { [ -n "$commit" ] && [ -n "$dist" ]; } || die "auto record needs --commit SHA and --dist DIR (the RC's TEST build)"
  [ "$kind" != record ] || git -C "$root" cat-file -e "$commit^{commit}" 2> /dev/null || die "--commit $commit is not a commit here"
  # a new record run drops the commit's earlier PC record first: only this run's result may stand
  [ "$kind" != record ] || rm -f "${RBKIT_GATES:-$root/build/gates}/$commit/pc-win.ok" || die "auto record: the earlier ${RBKIT_GATES:-$root/build/gates}/$commit/pc-win.ok can't be removed"
  old_dist="" fixture=()
  if [ -n "$from_dist$uia$bootstrap_server$installer" ]; then
    [ "$kind" != dumps ] || die "auto dumps does not take M12 fixtures"
    [ -n "$from_dist" ] && [ -n "$dist" ] && [ -n "$uia" ] && [ -n "$bootstrap_server" ] && [ -n "$server" ] ||
      die "M12 needs --from-dist, --dist, --uia, --bootstrap-server and rbstemsplus.pcTestServer"
    from_dist="$(cd "$from_dist" && pwd -P)" && dist="$(cd "$dist" && pwd -P)" || die "the M12 release folders are missing"
    old_dist="$from_dist"
    server_dir="${dist%/*}"
    [ "${from_dist%/*}" = "$server_dir" ] && [ "$from_dist" != "$dist" ] &&
      name_ok "${from_dist##*/}" && name_ok "${dist##*/}" ||
      die "M12 releases must be distinct sibling folders on the TEST server"
    [ -f "$server_dir/serve-current.txt" ] && [ ! -L "$server_dir/serve-current.txt" ] ||
      die "no regular TEST server selector at $server_dir/serve-current.txt"
    for d in "$from_dist" "$dist"; do
      [ -f "$d/test-ca.pem" ] && [ -f "$d/bootstrap.ps1" ] || die "M12 needs a Windows TEST build at $d"
    done
    [ -z "$installer" ] || [ -f "$dist/$installer" ] || die "the installer fixture is missing from $dist"
    fixture=(--from-dist "$from_dist" --uia "$uia")
    [ -z "$installer" ] || fixture+=(--installer "$installer")
  fi
  say "auto $kind: start ($stamp)"
  # Only a successful send in the current child can suppress the plan's fallback.
  notify_marker="$(mktemp "$log.notified.XXXXXX")" || notify_marker=""
  [ -z "$notify_marker" ] || step="$notify_marker.step"
  trap 'rc=$?; rm -f "$step" ${notify_marker:+"$notify_marker"}; finish_timing "$rc"' EXIT
  results=()
  probe_run_id=""
  pstep() {   # NAME ARGS...: one step; 0 go on, 1 the plan ends here
    local n="$1" r marker="$notify_marker" evidence="${RBKIT_EVIDENCE_RUN:-}"; shift
    case "$1:${2:-}" in
      probe:install | probe:remove) evidence="$probe_run_id" ;;
    esac
    case "$1:${2:-}" in
      run:* | preflight:* | cleanup:* | probe:install | probe:remove)
        [ -z "$rekordbox" ] || set -- "$@" --rekordbox "$rekordbox" ;;
    esac
    say "auto: --- $n: pc-session.sh $*"
    if [ -n "$marker" ]; then : > "$marker" || marker=""; fi
    : > "$step"
    RBKIT_AUTO=1 RBKIT_NOTIFY_MARKER="$marker" RBKIT_EVIDENCE_RUN="$evidence" "$0" "$@" 2>&1 | tee -a "$log" "$step"; r="${PIPESTATUS[0]}"
    results+=("$n $r")
    case "$r" in
      0) return 0 ;;
      3) say "auto: $n: an expectation not met (exit 3): the plan goes on"; exp=1; return 0 ;;
      *) say "auto: STOP: $n exit $r"
         [ -s "$marker" ] || stops "auto $kind" "$step" || notify "auto $kind" "$n exited $r" || true
         return 1 ;;
    esac
  }
  pin_rekordbox() {
    [ "$kind" = dumps ] || return 0
    local version
    if auto 60 -Do version && ! stopped; then
      # The idle watchdog logs SSH diagnostics alongside PowerShell output.
      version="$(grep '^REKORDBOX ' "$step")"
    fi
    if ! [[ "${version:-}" =~ ^REKORDBOX\ ([0-9]{1,9}\.[0-9]{1,9}\.[0-9]{1,9})$ ]]; then
      row BAD "auto dumps: the rekordbox install could not be selected"
      return 1
    fi
    rekordbox="${BASH_REMATCH[1]}"
  }
  serve_release() {
    local dest="$server_dir/serve-current.txt" swap
    say "M12: serving ${1##*/}"
    swap="$(mktemp "$dest.XXXXXX")" || return 1
    if printf '%s\n' "${1##*/}" > "$swap" && mv -f "$swap" "$dest"; then return 0; fi
    rm -f "$swap"
    return 1
  }
  m12_start() {
    [ -n "$old_dist" ] || return 0
    pstep m12-quit run close &&
    pstep m12-install m12 install-old --bootstrap-server "$bootstrap_server" &&
    { serve_release "$dist" || { say "auto: STOP: the TEST server selector could not be changed"; notify "auto $kind" "the TEST server selector could not be changed" || true; return 1; }; } &&
    pstep m12-update m12 update --bootstrap-server "$bootstrap_server" &&
    pstep m12-pin m12 pin &&
    pstep m12-worker-source m12 worker-source --bootstrap-server "$bootstrap_server"
  }
  m12_finish() {
    [ -n "$old_dist" ] || return 0
    pstep m12-settings-kept m12 settings-kept &&
    pstep m12-signout m12 signout &&
    pstep m12-session session up &&
    pstep m12-settings-retry m12 settings-retry || return 1
    if [ "$kind" = record ]; then pstep m12-restore-rc m12 install-new --bootstrap-server "$bootstrap_server" || return 1; fi
    if [ -n "$installer" ]; then
      pstep m12-smartscreen m12 smartscreen --installer "$installer"
    else
      row NOTE "M12 SmartScreen requires an owner-supplied downloaded installer fixture"
    fi
  }
  t2_round2() {
    if [ -z "$probe" ]; then
      row NOTE "T2 items 3, 4 and 6: pass --probe for Demucs rounds B/C and probe telemetry"
      return 0
    fi
    # Demucs v4 is on already (session2 turns it on before Stems Cache, and off after this)
    restore=1
    pstep t2-probe-install probe install --no-dumps --probe "$probe" &&
    pstep t2-B run T2 --keys --fresh &&
    pstep t2-C1 run T2 --keys --fresh --aac &&
    pstep t2-C2 run T2 --keys --fresh --aac &&
    pstep t2-C3 run T2 --keys --fresh --aac &&
    pstep t2-probe-evidence run t2-probe &&
    pstep t2-probe-remove probe remove &&
    restore=0
  }
  # rounds the kit has no step for yet: named in the log, never a pass
  notyet() { row SKIP "$1: not built yet ($2)"; results+=("$1 skip"); }
  prepare_rekordbox_checks() {
    # The application-only VM steps run with rekordbox closed. These require the real
    # Preferences UI, a running bridge, or a live rekordbox session. Keep them visible
    # in the plan and distinguish their pending evidence from the completed Prepare run.
    notyet "W13b settings-location" "set Prioritize speed in rekordbox; record the saved settings path and verify the Prepare speed line; return it to sound quality"
    notyet "W13b bridge-release" "after Stop Keeping, verify in running rekordbox that the bridge no longer protects that set"
    notyet "W13b watcher-reminder" "open rekordbox with a current newly unready set; record one notification, then click its body and verify RB Stems Plus opens"
    notyet "W13b watcher-dedupe" "restart the watcher during the same rekordbox session, then reopen rekordbox with the same unready tracks; verify no repeat in either case"
    notyet "W13b watcher-active-set" "open fresh rekordbox sessions with the target Prepare running and paused; require a skipped-set log and no estimate for that set"
    notyet "W13b watcher-set-filter" "in fresh sessions verify no helper with a missing/empty kept folder or expired sets only, and a helper check for a malformed date"
  }
  exp=0
  mx=(); [ -z "$max" ] || mx=(--max "$max")
  st=(); [ -z "$dist" ] || st+=(--dist "$dist"); [ -z "$corpus" ] || st+=(--corpus "$corpus")
  if [ -z "$old_dist" ] && [ "$kind" = session2 ]; then
    row NOTE "M12 fixtures absent: provide --from-dist, --dist, --uia and --bootstrap-server"
  fi
  if [ "$kind" = dumps ]; then
    probe="${probe:-${RBKIT_PROBE:-$root/build/win/probe-t1/onnxruntime.dll}}"
    [ -f "$probe" ] || die "auto dumps: no probe at $probe (the research probe, built for Windows into build/win/probe-t1)"
  fi
  [ -z "$probe" ] || st+=(--probe "$probe")
  [ -z "$helper" ] || st+=(--helper "$helper")
  if [ -n "$probe" ] && { [ "$kind" = dumps ] || [ "$kind" = session2 ]; }; then
    # The probe uses a fixed path. Reserve a fresh instance of that directory for this run.
    run_id="$(format_run_id)"
    [[ "$run_id" =~ ^[a-f0-9]{32}$ ]] || die 'could not create probe evidence run ID'
    probe_run_id="$run_id"
    if [ "$kind" = dumps ] && { [ "$formats_only" = 1 ] || [ -n "$helper" ]; }; then
      export RBKIT_EVIDENCE_RUN="$run_id"
      no_dumps=1
    fi
    restore=0
    trap '
      rc=$?
      trap - EXIT
      if [ "$restore" = 1 ] && ! pstep probe-remove probe remove; then
        say "auto: STOP: probe restore unconfirmed; close rekordbox and use probe remove before another run"
        rc=1
      fi
      rm -f "$step" ${notify_marker:+"$notify_marker"}
      finish_timing "$rc"
      exit "$rc"
    ' EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM
    trap 'exit 129' HUP
  fi
  if [ "$kind:$formats_only" = dumps:1 ]; then
    pstep session session up && { [ "$target" != vm ] || pstep display session display; } && pin_rekordbox && pstep preflight preflight &&
    pstep preconditions run preconditions --plan dumps || exit 1
    if [ "$reuse_format_import" = 0 ]; then
      pstep format-import run format-import ${fresh_format_import:+--fresh-format-import} || exit 1
    else
      auto 220 -Do format-verify && ! stopped || exit 1
    fi
    batch_args=(); batch_rows=(); batch_dirs=(); batch_ids=()
    for ((batch = format_batch; batch <= format_last; batch++)); do
      if [ "$batch" != "$format_batch" ]; then run_id="$(format_run_id)"; fi
      [[ "$run_id" =~ ^[a-f0-9]{32}$ ]] || die 'could not create format batch ID'
      dir="$OUT/formats-batch-$batch-$run_id"
      mkdir "$dir" && out_ok "$dir" '' || die "could not make $dir"
      args="-FormatBatch $batch -FormatRun '$run_id'"
      [ "$confirm_import" = 0 ] || args="$args -ConfirmImportRefusals"
      if ! auto 240 -Do format-plan $args || stopped; then
        stops 'auto dumps' || notify 'auto dumps' 'format playlist verification failed' || true
        exit 1
      fi
      planned="$(sed -n 's/^FORMAT PLAN //p' "$step")"
      [[ "$planned" =~ ^fmt-[a-z0-9._~-]+(,fmt-[a-z0-9._~-]+){0,3}$ ]] || die 'invalid format batch plan'
      batch_args+=("$args"); batch_rows+=("$planned"); batch_dirs+=("$dir"); batch_ids+=("$run_id")
    done
    restore=1
    pstep probe-install probe install --no-dumps --probe "$probe" || exit 1
    if ! auto 300 -Do start -Formats || stopped; then
      stops 'auto dumps' || notify 'auto dumps' 'format measurement failed' || true
      exit 1
    fi
    continuation=""
    for ((i = 0; i < ${#batch_args[@]}; i++)); do
      pass_args=()
      [ "$format_batch" = "$format_last" ] || pass_args+=(-FormatPass)
      [ -z "$continuation" ] || pass_args+=(-FormatContinuation "'$continuation'")
      if ! auto 2700 -Do format-run ${batch_args[i]} -Rows "'${batch_rows[i]}'" ${pass_args[@]+"${pass_args[@]}"} || stopped; then
        stops 'auto dumps' || notify 'auto dumps' 'format measurement failed' || true
        exit 1
      fi
      if [ "$((format_batch + i))" -lt "$format_last" ]; then
        continuation="$(format_continuation)" || die 'invalid format pass continuation'
      fi
    done
    pstep probe-remove probe remove || exit 1
    restore=0
    result=0
    for ((i = 0; i < ${#batch_args[@]}; i++)); do
      compare_args="${batch_args[i]}"
      [ "$test_unproved_aac" = 0 ] || compare_args="$compare_args -TestUnprovedAac"
      [ "$test_unproved_mp3" = 0 ] || compare_args="$compare_args -TestUnprovedMp3"
      auto 1260 -Do keycompare $compare_args -Rows "'${batch_rows[i]}'"; rc=$?
      case "$rc" in 0) ;; 3) result=3 ;; *) exit 1 ;; esac
      stopped && exit 1
      # The idle SSH wrapper can append stdout to a partial CLIXML stderr line.
      report="$(sed -n 's/^.*FORMAT REPORT //p' "$step")"
      [[ "$report" == [A-Za-z]:\\*"\\rbst\\t2\\format-${batch_ids[i]}\\keycompare.tsv" && "$report" != *$'\n'* ]] ||
        die 'format batch report path missing'
      remote_report="${report//\\//}"
      get 120 '' "$test_login" "${batch_dirs[i]}/" "$remote_report" "${remote_report%/*}/chunk-hashes.tsv" \
        "${remote_report%/*}/helper-stderr.log" || exit 1
      say "auto: formats batch $((format_batch + i)): results in ${batch_dirs[i]}/keycompare.tsv"
    done
    pstep session-down session down || exit 1
    exit "$result"
  fi
  # T2 round 2's dump rows (t2-pc1-analysis.md): each run's sort check loads row 1 (01, MP3 44.1 kHz)
  # too; then AAC (04, 05; w10 with a video track), the 44.1 kHz control (09), resampled (17), MP3 (19)
  dumprows=(01-mp3-cbr320:05-aac-128 09-flac-16:09-flac-16 17-aiff-24-48k:17-aiff-24-48k 19-mp3-cbr128:19-mp3-cbr128 w10-mp4-video-first:w10-mp4-video-first)
  if [ -n "$rows" ]; then   # --rows: these instead (a single ROW is ROW:ROW)
    dumprows=(); IFS=, read -r -a rl <<< "$rows"
    for r in "${rl[@]}"; do [[ "$r" == *:* ]] || r="$r:$r"; dumprows+=("$r"); done
  fi
  {
    pstep cleanup-runners session cleanup-runners &&
    { [ -z "$old_dist" ] || serve_release "$old_dist" ||
      { say "auto: STOP: the TEST server selector could not be changed"; notify "auto $kind" "the TEST server selector could not be changed" || true; false; }; } &&
    pstep session session up &&
    { [ "${#st[@]}" = 0 ] || pstep stage stage "${st[@]}" ${fixture[@]+"${fixture[@]}"}; } &&
    pin_rekordbox &&
    { if [ -n "$old_dist" ]; then pstep preflight preflight --bootstrap-server "$bootstrap_server"; else pstep preflight preflight; fi; } &&
    pstep preconditions run preconditions --plan "$kind" &&
    m12_start &&
    case "$kind" in
      dumps)
        pi=(); [ "$no_dumps" = 0 ] || pi=(--no-dumps)
        restore=1
        pstep probe-install probe install ${pi[@]+"${pi[@]}"} --probe "$probe" &&
        { ok2=0
          for i in "${!dumprows[@]}"; do
            r="${dumprows[$i]}"; ko=(--keep-open); [ "$i" -lt $((${#dumprows[@]} - 1)) ] || ko=()
            pstep "dumps ${r%%:*}" run T2 --keys --from "${r%%:*}" --to "${r##*:}" ${ko[@]+"${ko[@]}"} || { ok2=1; break; }
          done
          [ "$ok2" = 0 ]; } &&
        pstep probe-fetch probe fetch ${pi[@]+"${pi[@]}"} &&
        { [ -z "$helper" ] || { planned="$(IFS=,; printf '%s' "${dumprows[*]}")"; pstep keycompare run keycompare --rows "$planned"; }; } &&
        pstep probe-remove probe remove &&
        { restore=0; } &&
        pstep collect collect &&
        pstep cleanup cleanup --lock ;;
      session2)
        # Demucs v4 on before Stems Cache (T2 and hits need our model: rekordbox's own isn't cached;
        # a model change needs Pioneer's runtime in place), off again once the rounds are done
        pstep autoanalysis run autoanalysis-off &&
        pstep t2-facts run t2-facts &&
        pstep watchtest run watchtest &&
        pstep t2-model-on run t2-model-on &&
        pstep cache-install cache install &&
        pstep T2 run T2 ${mx[@]+"${mx[@]}"} &&
        pstep hits run hits ${mx[@]+"${mx[@]}"} --keep-open &&
        pstep directml run directml &&
        pstep cache-uninstall cache uninstall &&
        { notyet "T11 FMA3 proof" "helper.md T11's sin/pow proof isn't built yet (only cpuprobe's FMA3 flag exists)"
          row NOTE "T2 item 1: mix dumps use auto dumps"
          row NOTE "T2 handoffs: item 2 export inspection, items 5 and 7, and item 6's second threading setting (README)"; } &&
        t2_round2 &&
        pstep t2-model-off run t2-model-off &&
        m12_finish &&
        pstep collect collect &&
        pstep cleanup cleanup --lock ;;
      prepare)
        # Prepare is for our model (its tab shows only with Demucs v4 installed): on first, off at the end
        pstep t2-model-on run t2-model-on &&
        pstep cache-install cache install &&
        pstep prepare run prepare &&
        prepare_rekordbox_checks &&
        pstep hits run hits ${mx[@]+"${mx[@]}"} &&
        pstep cache-uninstall cache uninstall &&
        pstep t2-model-off run t2-model-off &&
        m12_finish &&
        pstep collect collect &&
        pstep cleanup cleanup --lock ;;
      record)
        pstep cache-install cache install &&
        pstep T2 run T2 --max "${max:-2}" &&
        pstep hits run hits --max "${max:-2}" --keep-open &&
        { e0="$exp"; pstep directml run directml &&
          if [ "$cpu_ok" = 1 ] && [ "${results[${#results[@]}-1]}" = "directml 3" ]; then
            exp="$e0"; cpu_note=1; row NOTE "directml: rekordbox on the CPU, accepted by --cpu-stems-ok (the owner's ruling)"
          fi; } &&
        pstep cache-uninstall cache uninstall &&
        m12_finish &&
        pstep collect collect &&
        pstep cleanup cleanup --lock ;;
    esac
  }; ok=$?
  say "auto $kind: $(printf '%s; ' "${results[@]}")"
  if [ "$ok" != 0 ]; then say "auto $kind: stopped"; exit 1; fi
  if [ "$kind" = record ]; then
    # the PC record (M6): only a plan with every step 0 writes it, bound to the RC commit and to the
    # TEST payload the PC's app was installed from
    want="$(shasum -a 256 "$dist/payload.json" | cut -d' ' -f1)"
    got="$(pcps "$test_login" 60 "(Get-FileHash -LiteralPath (Join-Path \$env:LOCALAPPDATA 'rbstemsplus\\installed-release\\payload.json') -Algorithm SHA256).Hash.ToLowerInvariant()")"
    gates="${RBKIT_GATES:-$root/build/gates}/$commit"
    if [ "$exp" != 0 ]; then row BAD "pc-win.ok not written: an expectation was not met"; exit 3
    elif [ -z "$want" ] || [ "$got" != "$want" ]; then row BAD "pc-win.ok not written: the PC's installed release ($got) isn't $dist/payload.json ($want)"; exit 1
    else
      # the record carries the steps the RC's own gate (tests/win/gate-record.py at --commit) requires, in
      # its order, and is kept only once that gate accepts it: the plan's other steps passed too, or
      # the plan would have stopped before here
      mkdir -p "$gates" && tmpd="$(mktemp -d "$gates/.pc-win.XXXXXX")" || { row BAD "pc-win.ok not written: no temp folder in $gates"; exit 1; }
      grec="$tmpd/gate-record.py"
      git -C "$root" show "$commit:tests/win/gate-record.py" > "$grec" 2> /dev/null &&
        need_steps="$(python3 -I -c 'import runpy, sys; print("\n".join(runpy.run_path(sys.argv[1])["PC_STEPS"]))' "$grec")" &&
        [ -n "$need_steps" ] || { rm -rf "$tmpd"; row BAD "pc-win.ok not written: no PC_STEPS in $commit's tests/win/gate-record.py"; exit 1; }
      lines=""
      while read -r gs; do
        gr=""; for x in "${results[@]}"; do [ "${x% *}" != "$gs" ] || gr="${x##* }"; done
        [ -n "$gr" ] || { rm -rf "$tmpd"; row BAD "pc-win.ok not written: the gate's step $gs didn't run in this plan"; exit 1; }
        lines+="step $gs $gr"$'\n'
      done <<< "$need_steps"
      { echo "commit $commit"; echo "payload_sha256 $want"; echo "round worker (release.list's worker row, on the real PC: OD2)"
        echo "date $(date -u +%FT%TZ)"; printf '%s' "$lines"
        [ "$cpu_note" = 0 ] || echo "directml cpu, accepted by --cpu-stems-ok"; echo "log ${log##*/}"; echo "result pass"; } > "$tmpd/pc-win.ok" || { rm -rf "$tmpd"; row BAD "pc-win.ok not written"; exit 1; }
      if ! why="$(python3 -I "$grec" check "$tmpd/pc-win.ok" "$commit" "$want" 2>&1 > /dev/null)"; then
        rm -rf "$tmpd"; row BAD "pc-win.ok not written: $commit's gate refuses it: ${why##*: }"; exit 1
      fi
      mv -f "$tmpd/pc-win.ok" "$gates/pc-win.ok" && rm -rf "$tmpd" && row OK "the PC record: $gates/pc-win.ok" || { rm -rf "$tmpd"; row BAD "pc-win.ok not written"; exit 1; }
    fi
  fi
  [ "$exp" = 0 ] || exit 3 ;;

session)
  case "$kind" in
    up) session_up || { notify "session up" "$test_user's desktop not ready" || true; exit 1; } ;;
    display) vm_display || { notify "session display" "the VM display not 1920x1080 at 100 %" || true; exit 1; } ;;
    cleanup-runners)
      [ -n "$owner" ] || die "session cleanup-runners needs the admin login (rbstemsplus.pcSsh)"
      ident || { row BAD "the test account's SID and PC not read"; exit 1; }
      cleanup_runners | tee -a "$log" || { row BAD "session cleanup-runners failed (log $log)"; exit 1; } ;;
    state | down)
      [ -n "$owner" ] || die "session $kind needs the admin login (rbstemsplus.pcSsh)"
      ident || { say "session: the test account's SID and PC not read"; exit 1; }
      out="$(console "$kind")"; rc=$?; printf '%s\n' "$out" | tee -a "$log"
      if [ "$kind" = down ]; then [ "$rc" = 0 ] && [ "$(tail -1 <<< "$out")" = "CONSOLE down" ] && row OK "$test_user's session disconnected (locked)" || { row BAD "$test_user's session not disconnected (exit $rc)"; exit 1; }
      else [ "$rc" = 0 ] || exit 1; fi ;;
  esac ;;

probe)
  [ -n "$owner" ] || die "probe needs the admin login (rbstemsplus.pcSsh)"
  ident || { say "probe: the test account's SID and PC not read"; exit 1; }
  case "$kind" in
    state) probe_run state | tee -a "$log"; exit "${PIPESTATUS[0]}" ;;
    fetch)
      # the dumps and the log, as the test account (it may read C:\rbtest); kept on the PC until remove
      dir="${outdir:-$OUT/dumps-$stamp${RBKIT_EVIDENCE_RUN:+-$RBKIT_EVIDENCE_RUN}}"; mkdir -p "$dir" || die "can't make $dir"
      out_ok "$dir" && out_ok "$dir" '' || die "$dir is in the checkout and not git-ignored"
      if [ -n "${RBKIT_EVIDENCE_RUN:-}" ]; then
        get 120 '' "$test_login" "$dir/" C:/rbtest/rows.txt || { row BAD "probe fetch: $copy_method failed"; exit 1; }
        row OK "probe fetch: row evidence in $dir"
        exit 0
      fi
      if [ "$no_dumps" = 1 ]; then
        get 3600 r "$test_login" "$dir/" C:/rbtest/probe.log C:/rbtest/rows.txt || { row BAD "probe fetch: $copy_method failed"; exit 1; }
        row OK "probe fetch: probe.log and rows.txt in $dir (--no-dumps: no mix dumps)"
        exit 0
      fi
      get 3600 r "$test_login" "$dir/" C:/rbtest/dump C:/rbtest/probe.log C:/rbtest/rows.txt || { row BAD "probe fetch: $copy_method failed"; exit 1; }
      row OK "probe fetch: $(find "$dir/dump" -name '*.mix' | wc -l | tr -d ' ') mix dumps, probe.log and rows.txt in $dir"
      # one continuous track per row for dump-compare.py: tracks/<row>.rb.f32 and <row>.json
      python3 "$root/tests/rekordbox/analysis/assemble-dumps.py" "$dir" 2>&1 | tee -a "$log"
      [ "${PIPESTATUS[0]}" = 0 ] && row OK "probe fetch: tracks assembled in $dir/tracks" || { row BAD "probe fetch: the tracks not assembled (the dumps are kept)"; exit 1; }
      exit 0 ;;
  esac
  [ "$kind" != install ] || [ -n "$probe" ] || probe="${RBKIT_PROBE:-$root/build/win/probe-t1/onnxruntime.dll}"
  [ "$kind" != install ] || [ -f "$probe" ] || die "probe install: no probe at $probe (the research probe, built for Windows into build/win/probe-t1)"
  # Resolve the target before closing anything, including a backup made for an older install.
  out="$(probe_run "$kind" resolve)"; rc=$?
  probe_target="${out##*$'\n'}"
  if [ "$rc" = 0 ] && [ "$kind" = remove ] && [[ "$probe_target" == 'PROBE pioneer '* ]]; then
    say "$probe_target"; exit 0
  fi
  if [ "$rc" != 0 ] || [[ ! "$probe_target" =~ ^PROBE\ TARGET\ ([0-9]{1,9}\.[0-9]{1,9}\.[0-9]{1,9})$ ]]; then
    row BAD "probe $kind (exit $rc): $probe_target"; notify "probe $kind" "not confirmed" || true; exit 1
  fi
  rekordbox="${BASH_REMATCH[1]}"
  # The selected rekordbox is closed first in the account's session.
  auto 150 -Do quit; q=$?
  if [ "$q" != 0 ]; then stops "probe $kind" || lost "$q" "probe $kind quit" || notify "probe $kind" "quit exit $q" || true; exit 1; fi
  out="$(probe_run "$kind")"; rc=$?; printf '%s\n' "$out" | sed 's/^/      /' | tee -a "$log"
  case "$kind:$rc:$(tail -1 <<< "$out")" in
    install:0:"PROBE probe "*) row OK "the probe is rekordbox's onnxruntime.dll (Pioneer's in C:\\rbtest\\real)" ;;
    remove:0:"PROBE pioneer "*) row OK "rekordbox's onnxruntime.dll is Pioneer's" ;;
    *) row BAD "probe $kind (exit $rc): $(tail -1 <<< "$out")"; notify "probe $kind" "not confirmed" || true; exit 1 ;;
  esac ;;

cache)
  [ -n "$owner" ] || die "cache needs the admin login (rbstemsplus.pcSsh)"
  ident || { say "cache: the test account's SID and PC not read"; exit 1; }
  if [ "$kind" = check ]; then cache_run check && row OK "Stems Cache's worker and source found (not run)" || { row BAD "Stems Cache's worker or source not found"; exit 1; }; exit 0; fi
  # the worker refuses while rekordbox runs in any session: it is quit first in the account's
  auto 150 -Do quit; q=$?
  if [ "$q" != 0 ]; then stops "cache $kind" || lost "$q" "cache $kind quit" || notify "cache $kind" "quit exit $q" || true; exit 1; fi
  # The cache phase has ended. Restore the choice before the worker makes the cache unavailable.
  if [ "$kind" = uninstall ] && ! restore_cache_choice; then
    notify "cache uninstall" "temporary Stems Cache choice not restored" || true
    exit 1
  fi
  cache_run "$kind"; rc=$?
  ort="$(pcps "$test_login" 60 "& { . (Join-Path \$k 'kit-lib.ps1') > \$null; Get-OrtState }")"
  case "$kind:$rc:$ort" in
    install:0:other*) row OK "Stems Cache installed by its worker (rekordbox's onnxruntime.dll: $ort)"
      if [ "$chosen" = 1 ]; then
        # --chosen (coordinator 2026-11-20): the test account's own choice set as our app's own install
        # sets it (kit/chosen.ps1, CORE_STEP_SET_CHOSEN's bytes); uninstall or cleanup puts it back
        out="$(pcps "$test_login" 60 "& (Join-Path \$k 'chosen.ps1') -Mode set")"; rc=$?; printf '%s\n' "$out" | tee -a "$log"
        [ "$rc" = 0 ] && grep -q '^chosen: the kit set cache=1' <<< "$out" && row OK "the kit set cache=1 in $test_user's support\\installed (put back at uninstall or cleanup)" || { row BAD "cache=1 not set (exit $rc)"; exit 1; }
      fi ;;
    uninstall:0:pioneer) row OK "Stems Cache uninstalled by its worker (rekordbox's onnxruntime.dll is Pioneer's)" ;;
    *) row BAD "Stems Cache $kind: the worker or the DLL not as expected (onnxruntime.dll: $ort)"
       notify "cache $kind" "not confirmed ($ort)" || true; exit 1 ;;
  esac ;;

cleanup)
  if [ -n "$rekordbox" ] && ! auto 60 -Do version; then exit 1; fi
  label="${label:-after-$stamp}"
  restore_cache_choice || true
  # rekordbox closed first; if it can't be (a STOP: a dialog, or it won't close), nothing else is done
  auto 150 -Do quit; q=$?
  if [ "$q" != 0 ]; then
    stops "cleanup" || lost "$q" "cleanup quit" || notify "cleanup" "quit exit $q" || true
    say "cleanup: stopped: rekordbox left as it is; nothing else done (Stems Cache, admin rights, session)"; exit 1
  fi
  # rekordbox's onnxruntime.dll (anything but "pioneer" when the read fails)
  ortstate() { local o r; o="$(pcps "$test_login" 60 "& { \$ErrorActionPreference = 'Stop'; \$RbVersion = '$rekordbox'; . (Join-Path \$k 'kit-lib.ps1') > \$null; Get-OrtState }")"; r=$?; if [ "$r" = 0 ]; then printf '%s' "$o"; else printf 'not read (exit %s) %s' "$r" "$o"; fi; }
  ort="$(ortstate)"
  if [ -n "$rekordbox" ] && [ "$ort" != pioneer ]; then
    row BAD "cleanup: rekordbox $rekordbox needs its Pioneer runtime restored ($ort)"
    exit 1
  fi
  if [ "$ort" != pioneer ]; then
    # an uninstall that STOPs or fails: nothing more is done, as for our DLL left under rekordbox.
    # With the admin login, its worker runs there (no UAC prompt); else the app's own button.
    if [ -n "$owner" ] && ident; then : > "$step"; cache_run uninstall; u=$?
    else auto 300 -Do uninstallcache; u=$?; fi
    if [ "$u" != 0 ] || stopped; then
      stops "cleanup" || lost "$u" "cleanup uninstallcache" || notify "cleanup" "uninstallcache exit $u" || true
      say "cleanup: stopped: Stems Cache's uninstall not confirmed; nothing else done (snapshot, admin rights, session)"; exit 1
    fi
    ort="$(ortstate)"
  fi
  # our DLL still under rekordbox, which the owner's own rekordbox on this PC loads too: the
  # account keeps its admin rights and its session, so that the uninstall can be finished (README)
  if [ "$ort" != pioneer ]; then
    row BAD "rekordbox's onnxruntime.dll: $ort (Stems Cache still installed)"
    notify "cleanup" "Stems Cache not uninstalled ($ort); $test_user keeps its admin rights and session" || true
    say "cleanup: stopped before the snapshot, the admin rights and the sign-out"; exit 1
  fi
  row OK "rekordbox's onnxruntime.dll is Pioneer's"
  if [ "$snapshot" = 1 ]; then
    out="$(pcps "$test_login" 3600 "& (Join-Path \$k 'rbstate.ps1') -Mode Snapshot; 'exit ' + \$LASTEXITCODE; & (Join-Path \$k 'rbstate.ps1') -Mode Inventory -Name '$label'; 'exit ' + \$LASTEXITCODE")"; rc=$?
    grep -E '^(SNAPSHOT|INVENTORY|REFUSED|FAIL)' <<< "$out" | tee -a "$log"
    [ "$rc" = 0 ] && [ "$(grep -c '^exit 0$' <<< "$out")" = 2 ] && row OK "snapshot and inventory '$label'" || row BAD "snapshot or inventory failed (exit $rc)"
  fi
  # Administrators by its SID (the group's name is localised); the account by its own SID, read
  # over its own login; the owner's login must reach the same PC and be another account
  if ! ident; then row BAD "the test account's SID, PC and admin rights not read"; t_pc=""
  elif [ -z "$owner" ]; then
    [ "$t_admin" = 0 ] && row OK "$test_user is not an administrator (its own login, by SID)" || row BAD "$test_user is an administrator (rbstemsplus.pcSsh not set: the owner removes it)"
  else
    out="$(pcps "$owner" 60 "$(onpc)\$g = 'S-1-5-32-544'; if ([Security.Principal.WindowsIdentity]::GetCurrent().User.Value -eq '$t_sid') { 'the owner login is the test account'; exit 2 }; \$m = @(Get-LocalGroupMember -SID \$g | Where-Object { \$_.SID.Value -eq '$t_sid' }); if (\$m.Count) { Remove-LocalGroupMember -SID \$g -Member \$m }; 'admin ' + @(Get-LocalGroupMember -SID \$g | Where-Object { \$_.SID.Value -eq '$t_sid' }).Count")"; rc=$?
    [ "$rc" = 0 ] && [ "$out" = "admin 0" ] && row OK "$test_user is not an administrator" || row BAD "$test_user's admin rights (exit $rc): $out"
  fi
  if [ "$logoff" = 2 ]; then
    # --lock: the session stays, disconnected (only its password reconnects it); the next
    # session up puts it on the console again without a sign-in
    if [ -z "$owner" ] || [ -z "$t_pc" ]; then row BAD "--lock needs the admin login and the test account's PC"
    else
      out="$(console down)"; rc=$?
      [ "$rc" = 0 ] && [ "$(tail -1 <<< "$out")" = "CONSOLE down" ] && row OK "$test_user's session disconnected (locked)" || row BAD "$test_user's session not disconnected (exit $rc): $(tail -1 <<< "$out")"
    fi
  fi
  if [ "$logoff" = 1 ]; then
    # signed out only when its session is gone (quser), not when the sign-out was asked
    if auto 60 -Do logoff; then
      wait_signout && row OK "$test_user signed out (no session left)" ||
        row BAD "$test_user not confirmed signed out within 120 s (exit $qrc: $q)"
    else row BAD "$test_user not signed out (the session task didn't confirm)"; fi
  fi
  say "$bad not OK"
  [ "$bad" = 0 ] || { notify "cleanup" "$bad step(s) not OK" || true; exit 1; } ;;
esac

#!/bin/bash
# The harness's own checks, on the host, in seconds (no VM): every script parses, the fake helper
# behaves as its header says (scripts, pace, @wait, SIGTERM's @onterm part, argv log), trace.py and
# stats.py read synthetic records right.
#   tests/harness/selftest.sh
set -uo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
t="$root/build/harness/selftest"; rm -rf "$t"; mkdir -p "$t/fake/script"
pass=0; fail=0
ok() { if eval "$1"; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "FAIL: $2"; fi; }

for f in "$root"/tests/harness/*.sh "$root"/tests/harness/scenarios/*.sh "$root"/tests/perf/*.sh; do
  ok "bash -n '$f'" "syntax: $f"
done
for f in "$root"/tests/harness/scenarios/*.sh; do
  ok "grep -q '^# tag: *\(first\|Prepare\)' '$f'" "tag first or Prepare: $f"
  ok "grep -q '^setup()' '$f' && grep -q '^steps()' '$f'" "setup and steps: $f"
done
ok "python3 -I -c 'import sys; [compile(open(f).read(), f, \"exec\") for f in sys.argv[1:]]' '$root/tests/harness/trace.py' '$root/tests/perf/stats.py' '$root/tests/perf/rules.py'" "python compiles"

# the fake helper
clang -O2 -Wall -Wextra -Werror -o "$t/fake/fake-prepare" "$root/tests/harness/fake-prepare.c" || { echo "FAIL: fake-prepare doesn't build"; exit 1; }
f="$t/fake/fake-prepare"; s="$t/fake/script"
printf '{"playlist":"1","parent":"","name":"A","kind":"playlist","tracks":1}\n' > "$s/playlists.jsonl"
ok '[ "$("$f" --playlists)" = "{\"playlist\":\"1\",\"parent\":\"\",\"name\":\"A\",\"kind\":\"playlist\",\"tracks\":1}" ]' "playlists script printed"
ok '! "$f" --tracks 7 >/dev/null 2>&1' "no script: exit 1"
printf '{"file":"/x/default.mp3"}\n' > "$s/tracks-default.jsonl"
ok '[ "$("$f" --tracks 7)" = "{\"file\":\"/x/default.mp3\"}" ]' "kind-default script"
printf '@repeat 3 {"n":{n},"i":{i}}\n@exit 3\n' > "$s/estimate-9.jsonl"
out="$("$f" --estimate --playlist 9)"; rc=$?
ok '[ "$rc" = 3 ] && [ "$(echo "$out" | wc -l | tr -d " ")" = 3 ] && echo "$out" | tail -1 | grep -q "\"n\":3,\"i\":2"' "repeat and exit status"
printf '{"estimate":0}\n' > "$s/estimate-keep-4.jsonl"
ok '[ "$("$f" --estimate --keep 4)" = "{\"estimate\":0}" ]' "estimate of a set alone"
printf '@wait go\n{"after":"go"}\n' > "$s/run-5.jsonl"
"$f" --keep 5 --playlist 5 > "$t/run5.out" & p=$!
sleep 0.3; ok '[ ! -s "$t/run5.out" ]' "@wait holds the line"
touch "$s/go"; wait $p
ok 'grep -q after "$t/run5.out"' "@wait goes on once the file is there"
printf '@pace 100\n@repeat 100 {"at":{i}}\n@onterm\n{"done":1,"stopped":"stopped"}\n' > "$s/run-6.jsonl"
"$f" --playlist 6 > "$t/run6.out" & p=$!
sleep 0.35; kill -STOP $p; sleep 0.4; n1="$(wc -l < "$t/run6.out")"; sleep 0.3; n2="$(wc -l < "$t/run6.out")"
ok '[ "$n1" = "$n2" ]' "SIGSTOP stops it"
kill -CONT $p; sleep 0.25; kill -TERM $p; wait $p; rc=$?
ok '[ "$rc" = 0 ] && tail -1 "$t/run6.out" | grep -q stopped' "SIGTERM: the @onterm part, exit 0"
ok '[ "$(wc -l < "$t/run6.out")" -lt 20 ]' "SIGTERM stopped the lines"
printf '@pace 50\n@repeat 100 {"at":{i}}\n' > "$s/run-7.jsonl"
"$f" --playlist 7 > /dev/null & p=$!; sleep 0.2; kill -TERM $p; wait $p; rc=$?
ok '[ "$rc" = 143 ]' "SIGTERM without @onterm: 143"
ok '[ "$(wc -l < "$t/fake/log/argv.log" | tr -d " ")" -ge 8 ] && grep -q "run-6 <- run-6	--playlist	6" "$t/fake/log/argv.log"' "argv log"

# trace.py on a synthetic trace
cat > "$t/a.jsonl" <<'EOF'
{"k":"start","t":0,"mode":"trace","pid":1,"launch_ns":-5,"arch":"arm64"}
{"k":"frame","t":10,"n":1,"busy":5,"d":{"app.active":"1","prepare.tracksTable.rows":"1","prepare.tracksTable.r0":"a | Checking… | ","prepare.prepareBtn.en":"0"}}
{"k":"ev","t":11,"e":"dialog","n":1,"v":"Hi {OK*}","origin":"event"}
{"k":"frame","t":12,"n":2,"busy":5,"d":{"prepare.tracksTable.r0":"a | Ready | "}}
{"k":"ev","t":13,"e":"dialogEnd","n":1,"v":"Hi {OK*}","origin":""}
{"k":"ev","t":14,"e":"workQueueFree","n":1,"v":"1"}
{"k":"frame","t":20,"n":3,"busy":5,"d":{"prepare.prepareBtn.en":"1"}}
EOF
ok '"$root/tests/harness/trace.py" compare "$t/a.jsonl" "$t/a.jsonl" >/dev/null' "a trace equals itself"
ok '"$root/tests/harness/trace.py" check "$t/a.jsonl" >/dev/null' "invariants hold on a good trace"
grep -v '"n":2,' "$t/a.jsonl" > "$t/b.jsonl"
ok '! "$root/tests/harness/trace.py" compare "$t/a.jsonl" "$t/b.jsonl" >/dev/null' "a missing frame is a difference"
ok '! "$root/tests/harness/trace.py" check "$t/b.jsonl" >/dev/null' "Prepare on over a Checking row is caught"
ok '"$root/tests/harness/trace.py" dialogs "$t/a.jsonl" | grep -q "prepare.tracksTable.r0"' "the window changed under the dialog"
# the Windows probe's sent events: an echo guard checked from the records (ShowLogEnd sends no tab)
cat > "$t/sent.jsonl" <<'EOF'
{"k":"start","t":0,"mode":"trace","pid":1,"launch_ns":-5,"arch":"arm64","shell":"wpf"}
{"k":"ev","t":5,"e":"sent","v":"{\"ev\":\"tab\",\"tab\":1}"}
{"k":"ev","t":6,"e":"sent","v":"{\"ev\":\"tab\",\"tab\":2}"}
{"k":"ev","t":6,"e":"cmd","v":"menu menu.Manage.Show Log"}
{"k":"ev","t":7,"e":"sent","v":"{\"ev\":\"click\",\"id\":\"show_log\"}"}
{"k":"ev","t":8,"e":"tabShown","v":"Manage"}
{"k":"ev","t":9,"e":"cmd","v":"tab Prepare"}
{"k":"ev","t":10,"e":"sent","v":"{\"ev\":\"tab\",\"tab\":1}"}
EOF
ok '[ "$("$root/tests/harness/trace.py" sent "$t/sent.jsonl" | tail -1)" = "4 sent" ] && "$root/tests/harness/trace.py" sent "$t/sent.jsonl" tab | grep -q "^5 tab {"' "trace.py sent: every sent event, and one event's"
ok '[ "$("$root/tests/harness/trace.py" sent "$t/sent.jsonl" tab --after "menu menu.Manage.Show Log" --before "tab Prepare")" = "0 sent" ]' "trace.py sent --after --before: no tab sent in the log's one-shot's window (the echo guard holds; a tie in t before the command's line is out)"
ok '[ "$("$root/tests/harness/trace.py" sent "$t/sent.jsonl" tab --after "menu menu.Manage.Show Log" | tail -1)" = "1 sent" ]' "trace.py sent --after alone: the user's later tab is in the window (hence --before)"
ok '[ "$("$root/tests/harness/trace.py" sent "$t/sent.jsonl" click --after "menu menu.Manage.Show Log")" = "$(printf '"'"'7 click {"ev":"click","id":"show_log"}\n1 sent'"'"')" ]' "trace.py sent --after: a sent event after the command is listed"
ok '! "$root/tests/harness/trace.py" sent "$t/sent.jsonl" tab --after "menu menu.Manage.Show" >/dev/null' "trace.py sent --after: a command's prefix is not the command"
ok '! "$root/tests/harness/trace.py" sent "$t/sent.jsonl" tab --after "menu nosuch" >/dev/null' "trace.py sent --after a command the trace lacks: refused"
ok '! "$root/tests/harness/trace.py" sent "$t/sent.jsonl" tab --after "menu menu.Manage.Show Log" --before "tab Status" >/dev/null' "trace.py sent --before a command the trace lacks: refused"
rc=0; "$root/tests/harness/trace.py" sent "$t/sent.jsonl" tab --after > /dev/null 2>&1 || rc=$?
ok '[ "$rc" = 2 ]' "trace.py sent --after with no command: refused (2)"
# the scenario's declarations: allow (by mode), must-show, order
printf '%s\n' '{"k":"frame","t":1,"n":1,"busy":0,"d":{"x.f":"1"}}' '{"k":"frame","t":2,"n":2,"busy":0,"d":{"x.f":"2"}}' '{"k":"frame","t":3,"n":3,"busy":0,"d":{"x.f":"3"}}' > "$t/p1.jsonl"
grep -v '"n":2,' "$t/p1.jsonl" > "$t/p2.jsonl"
printf '# compare: properties\n# allow: x.f\n' > "$t/s-prop.sh"
ok '"$root/tests/harness/trace.py" compare "$t/p1.jsonl" "$t/p2.jsonl" --scenario "$t/s-prop.sh" >/dev/null' "properties: an allowed field may lose a transient state"
printf '# compare: properties\n' > "$t/s-prop0.sh"
ok '! "$root/tests/harness/trace.py" compare "$t/p1.jsonl" "$t/p2.jsonl" --scenario "$t/s-prop0.sh" >/dev/null' "properties: a field not allowed may not"
printf '# must-show: prepare.tracksTable.r0=a | Checking… | >a | Ready | \n' > "$t/s-must.sh"
ok '"$root/tests/harness/trace.py" check "$t/a.jsonl" --scenario "$t/s-must.sh" >/dev/null' "must-show: shown in order"
ok '! "$root/tests/harness/trace.py" compare "$t/a.jsonl" "$t/b.jsonl" --scenario "$t/s-must.sh" >/dev/null' "must-show: B never shows it"
printf '# order: prepare.prepareBtn.en < prepare.tracksTable.r0\n' > "$t/s-order.sh"
ok '! "$root/tests/harness/trace.py" check "$t/a.jsonl" --scenario "$t/s-order.sh" >/dev/null' "order: a declared pair in the wrong order"
cat > "$t/k.jsonl" <<'EOF'
{"k":"frame","t":10,"n":1,"busy":5,"d":{"prepare.pauseBtn.hid":"1","prepare.prepareBtn.hid":"0","prepare.prepareBtn.en":"1","prepare.prepareBtn.title":"Keep","prepare.info.str":"1 track ready, 2 to prepare (8 min of music).","prepare.outline.rows":"1","prepare.outline.r0":"  Friday  (3)","prepare.outline.sel":"0","prepare.nameLabel.str":"Late","prepare.tracksTable.rows":"0"}}
EOF
ok '[ "$("$root/tests/harness/trace.py" check "$t/k.jsonl" | grep -c "^frame")" = 3 ]' "Keep, the selection's name and its rows are checked"
cat > "$t/m.jsonl" <<'EOF'
{"k":"frame","t":10,"n":1,"busy":5,"d":{"c.spinner.anim":"0","c.plusIcon.img":"on","c.uninstallPlusBtn.en":"0","c.installPlusBtn.en":"1","c.status.str":"Installing…"}}
EOF
ok '[ "$("$root/tests/harness/trace.py" check "$t/m.jsonl" | grep -c "^frame")" = 3 ]' "the buttons and status line against the lights"

# stats.py on synthetic perf records: two events of each kind per launch (every event counts):
# P1 = 100 ms, P3 0.2 and 2 ms, P5 0.2 and 3 ms, P6 0.2 and 2 ms, P7.lastrow 0.2 and 4 ms
st="$root/tests/perf/stats.py"
for side in A B; do
  d="$t/results/$side-1/perf-x/1"; mkdir -p "$d"
  {
    echo "# mode=perf pid=1 launch_ns=-100000000 probe_busy_ns=1000 arch=arm64"
    printf '0\tev\twindowDrawn\t0\n'
    printf '1000\tturn\tturn\t500\n'
    printf '1500000\ttp\ttrigger.activate\t0\n1600000\ttp\trefresh.begin\t0\n1700000\ttp\trefresh.end\t0\n1800000\tturn\tturn\t100\n'
    printf '2000000\tcmd\tmark click 0\t0\n2100000\tcmd\tclick c.x\t0\n2200000\tfield\tc.status:setStringValue\t0\n2300000\tturn\tturn\t100\n'
    printf '2900000\tcmd\tmark P6 full\t0\n3000000\tcmd\tselect 50\t0\n3100000\tfield\tprepare.tracksTable:reloadData\t5000\n3200000\tturn\tturn\t100\n'
    printf '3300000\ttp\testimate.lastline\t0\n3400000\tfield\tprepare.tracksTable:reloadData\t5000\n3500000\tturn\tturn\t100\n'
    printf '4900000\tcmd\tmark warm\t0\n'
    printf '5000000\ttp\ttrigger.activate\t0\n6000000\ttp\trefresh.begin\t0\n7000000\ttp\trefresh.end\t0\n7100000\tturn\tturn\t2000000\n'
    printf '8000000\tcmd\tmark click 1\t0\n9000000\tcmd\tclick c.x\t0\n10000000\tfield\tc.status:setStringValue\t0\n12000000\tturn\tturn\t100\n'
    printf '19000000\tcmd\tmark P6 full\t0\n20000000\tcmd\tselect 50\t0\n21000000\tfield\tprepare.tracksTable:reloadData\t5000\n22000000\tturn\tturn\t100\n'
    printf '23000000\ttp\testimate.lastline\t0\n26000000\tfield\tprepare.tracksTable:reloadData\t5000\n27000000\tturn\tturn\t100\n'
    printf '30000000\tcmd\tmark end\t0\n'
  } > "$d/perf-1.tsv"
  printf 'ok\nperf-x reopen perf: 0 failed\n' > "$d/result"
done
printf 'P1 median 10%% 20ms first mac-A\nP3 median 10%% 20ms first mac-A\nP5 median 10%% 20ms first mac-A\nP-B1.warm median 10%% 20ms first B0\n' > "$t/rules.txt"
ok '"$st" samples "$t/results/A-1/perf-x/1" | grep -q "^P1 1 100.000$"' "P1 sample"
ok '"$st" samples "$t/results/A-1/perf-x/1" | grep -q "^P3 2 0.200 2.000$"' "P3: every trigger, the launch's first too"
ok '"$st" samples "$t/results/A-1/perf-x/1" | grep -q "^P5 2 0.200 3.000$"' "P5 samples"
ok '"$st" samples "$t/results/A-1/perf-x/1" | grep -q "^P6 2 0.200 2.000$"' "P6 samples"
ok '"$st" samples "$t/results/A-1/perf-x/1" | grep -q "^P7.lastrow.full 2 0.200 4.000$" && "$st" samples "$t/results/A-1/perf-x/1" | grep -q "^P7.done.full 2 0.500 7.000$"' "P7.lastrow from the estimate's last line, P7.done from the choice"
# P16 (the menu's Create Report to its dialog: 40 ms), P14 (two processes joined by their start
# times: the old app's answer at 1.010 s after its start, the new one's window at 0.5 s after its
# own, started 3 s after the old: 2490 ms)
d="$t/p14/1"; mkdir -p "$d"
{
  echo "# mode=perf pid=7 launch_ns=-1000000000 probe_busy_ns=0 arch=arm64"
  printf '10000000\tcmd\tanswer Update\t0\n20000000\tcmd\tmenu menu.Help.Create Report\t0\n60000000\tev\tdialog\t1\n'
} > "$d/perf-7.tsv"
{ echo "# mode=perf pid=8 launch_ns=-50000000 probe_busy_ns=0 arch=arm64"; printf '450000000\tev\twindowDrawn\t0\n'; } > "$d/perf-8.tsv"
printf '7 1000000000000\n8 1000003000000\n' > "$d/pids.txt"
ok '"$st" samples "$d" | grep -q "^P16 1 40.000$"' "P16 sample"
ok '! "$st" samples "$d" | grep -q "^P16.own "' "P16.own only with the round's rusage files"
ok '"$st" samples "$d" | grep -q "^P14 1 2490.000$"' "P14 across two processes"
# P16.own: each round's rusage (vmtool rusage, before the command and after the dialog) takes its
# children's lifetimes out of its wall time: round 1 40 ms less 30, round 2 50 ms less 25 (each
# round its own files, by its "mark report N"); the CPU times are the differences
d="$t/p16/1"; mkdir -p "$d"
{
  echo "# mode=perf pid=9 launch_ns=-1000000000 probe_busy_ns=0 arch=arm64"
  printf '10000000\tcmd\tmark report 1\t0\n20000000\tcmd\tmenu menu.Help.Create Report\t0\n60000000\tev\tdialog\t1\n'
  printf '100000000\tcmd\tmark report 2\t0\n110000000\tcmd\tmenu menu.Help.Create Report\t0\n160000000\tev\tdialog\t2\n'
} > "$d/perf-9.tsv"
printf 'cpu_ns=1000000 child_cpu_ns=0 child_elapsed_ns=5000000\n' > "$d/rusage-report-1-before.txt"
printf 'cpu_ns=4000000 child_cpu_ns=20000000 child_elapsed_ns=35000000\n' > "$d/rusage-report-1-after.txt"
printf 'cpu_ns=5000000 child_cpu_ns=20000000 child_elapsed_ns=40000000\n' > "$d/rusage-report-2-before.txt"
printf 'cpu_ns=7000000 child_cpu_ns=30000000 child_elapsed_ns=65000000\n' > "$d/rusage-report-2-after.txt"
ok '"$st" samples "$d" > "$t/p16.out" && grep -q "^P16 2 40.000 50.000$" "$t/p16.out" && grep -q "^P16.own 2 10.000 25.000$" "$t/p16.out" && grep -q "^P16.children 2 30.000 25.000$" "$t/p16.out"' "P16.own: each round's wall time less its own children's lifetimes"
ok 'grep -q "^P16.cpu 2 3.000 2.000$" "$t/p16.out" && grep -q "^P16.children.cpu 2 20.000 10.000$" "$t/p16.out"' "P16.cpu and P16.children.cpu: each round's differences"
mv "$d/rusage-report-2-after.txt" "$t/p16-2-after.txt"
ok '! "$st" samples "$d" > /dev/null 2>&1' "P16.own: a round with only one of its two rusage files fails the measurement"
rm -f "$d/rusage-report-2-before.txt"
ok '"$st" samples "$d" > "$t/p16.out" && grep -q "^P16 2 40.000 50.000$" "$t/p16.out" && grep -q "^P16.own 1 10.000$" "$t/p16.out"' "P16.own: a round with neither file has no P16.own (P16 still)"
printf 'cpu_ns=4000000 child_cpu_ns=20000000 child_elapsed_ns=55000000\n' > "$d/rusage-report-1-after.txt"
ok '! "$st" samples "$d" > /dev/null 2>&1' "P16.own: children living longer than the round fail the measurement"
printf 'not a rusage line\n' > "$d/rusage-report-1-after.txt"
ok '! "$st" samples "$d" > /dev/null 2>&1' "P16.own: a rusage file that isn't vmtool's line fails the measurement"
# a child started before the round (LocationMonitor's pgrep, 8 ms before the command) and reaped
# inside it: its whole lifetime is counted, beside the report's 30 ms one (documented: P16.own
# is biased low)
printf 'cpu_ns=4000000 child_cpu_ns=20000000 child_elapsed_ns=43000000\n' > "$d/rusage-report-1-after.txt"
ok '"$st" samples "$d" > "$t/p16.out" && grep -q "^P16.children 1 38.000$" "$t/p16.out" && grep -q "^P16.own 1 2.000$" "$t/p16.out"' "P16.children: a child started before the round, reaped in it, counted whole"
# vmtool rusage on this host (when tests/harness/vm.sh build has built it from today's source),
# read from outside a target whose children are known: sleep 0.4 alone, then sh -c 'sleep 0.4;
# true'. A child's reaped descendants count too: the second phase counts sh's lifetime and
# sleep's, summed, about 400 ms more than the first (overlaps counted twice, P16.children's bias)
vmt="$root/build/harness/vm/vmtool"
if [ -x "$vmt" ] && [ ! "$root/tests/harness/vmtool.swift" -nt "$vmt" ]; then
  rm -f "$t/ru-a" "$t/ru-b"
  # (": " last: bash would exec a last command in its place, with a fresh rusage)
  bash -c 'sleep 0.4; : > "$1/ru-a"; sh -c "sleep 0.4; true"; : > "$1/ru-b"; sleep 10; :' _ "$t" & rp=$!
  ce() { "$vmt" rusage "$rp" | sed -n 's/.*child_elapsed_ns=\([0-9]*\).*/\1/p'; }
  c0="$(ce)"
  for _ in $(seq 1 100); do [ -e "$t/ru-a" ] && break; sleep 0.1; done; c1="$(ce)"
  for _ in $(seq 1 100); do [ -e "$t/ru-b" ] && break; sleep 0.1; done; c2="$(ce)"
  kill "$rp" 2>/dev/null; wait "$rp" 2>/dev/null
  one=$(( (${c1:-0} - ${c0:-0}) / 1000000 )); two=$(( (${c2:-0} - ${c1:-0}) / 1000000 ))
  ok '[ -n "$c0" ] && [ -n "$c2" ] && [ "$one" -ge 380 ] && [ "$two" -ge $((one + 300)) ]' "vmtool rusage: a reaped descendant counted beside its parent (sleep alone $one ms, sh with its sleep $two ms)"
else
  echo "selftest: vmtool rusage's host case skipped (no vmtool built from today's vmtool.swift: tests/harness/vm.sh build)"
fi
# the Windows shell's records (shell=wpf): P1 from the stub's launch when the header has it, the
# private bytes for P10 and their growth with rows
d="$t/wpf/1"; mkdir -p "$d"
{ echo "# mode=perf pid=5 launch_ns=-100000000 probe_busy_ns=0 wait_ns=0 arch=arm64 exe=anycpu shell=wpf stub_launch_ns=-300000000"; printf '500000000\tev\twindowDrawn\t0\n'; } > "$d/perf-5.tsv"
printf 'private_bytes=52428800\n' > "$d/memory-idle.txt"
printf 'private_bytes=73400320\n' > "$d/memory-after7.txt"
ok '"$st" samples "$d" | grep -q "^P1 1 800.000$"' "wpf: P1 from the stub's launch"
ok '"$st" samples "$d" | grep -q "^P10.idle 1 50.000$" && "$st" samples "$d" | grep -q "^P10.rows 1 20.000$"' "wpf: P10 the private bytes, P10.rows their growth"
d="$t/wpf/2"; mkdir -p "$d"
{ echo "# mode=perf pid=6 launch_ns=-100000000 probe_busy_ns=0 wait_ns=0 arch=arm64 exe=anycpu shell=wpf"; printf '500000000\tev\twindowDrawn\t0\n'; } > "$d/perf-6.tsv"
ok '"$st" samples "$d" | grep -q "^P1 1 600.000$"' "wpf without the stub's launch: P1 from the app's own"
# an AnyCPU exe recorded under Prism ran emulated (an x64 parent started it): refused; the
# x64-forced exe under Prism (the Prism pass) and the AnyCPU exe natively are read
d="$t/wpf/3"; mkdir -p "$d"
{ echo "# mode=perf pid=7 launch_ns=-100000000 probe_busy_ns=0 wait_ns=0 arch=x64-prism exe=anycpu shell=wpf"; printf '500000000\tev\twindowDrawn\t0\n'; } > "$d/perf-7.tsv"
rc=0; "$st" samples "$d" > "$t/wpf3.txt" 2>&1 || rc=$?
ok '[ "$rc" = 3 ] && grep -q "^MEASUREMENT FAILED: .*the AnyCPU app ran under x64 emulation" "$t/wpf3.txt"' "wpf: an AnyCPU exe under Prism (an x64 parent): the measurement failed (3)"
d="$t/wpf/4"; mkdir -p "$d"
{ echo "# mode=perf pid=8 launch_ns=-100000000 probe_busy_ns=0 wait_ns=0 arch=x64-prism exe=x64 shell=wpf"; printf '500000000\tev\twindowDrawn\t0\n'; } > "$d/perf-8.tsv"
{ echo "# mode=perf pid=9 launch_ns=-100000000 probe_busy_ns=0 wait_ns=0 arch=arm64 exe=anycpu shell=wpf"; printf '500000000\tev\twindowDrawn\t0\n'; } > "$d/perf-9.tsv"
ok '"$st" samples "$d" | grep -q "^P1 2 600.000 600.000$"' "wpf: the x64-forced exe under Prism and the AnyCPU exe natively are read"
d="$t/wpf/5"; mkdir -p "$d"
{ echo "# mode=perf pid=10 launch_ns=-100000000 probe_busy_ns=0 wait_ns=0 arch=x64 exe=anycpu shell=wpf"; printf '500000000\tev\twindowDrawn\t0\n'; } > "$d/perf-10.tsv"
{ echo "# mode=perf pid=11 launch_ns=-100000000 probe_busy_ns=0 wait_ns=0 arch=x64 exe=x64 shell=wpf"; printf '500000000\tev\twindowDrawn\t0\n'; } > "$d/perf-11.tsv"
ok '"$st" samples "$d" | grep -q "^P1 2 600.000 600.000$"' "wpf: anycpu/x64 (a real x64 PC) and x64/x64 are read"
# only the four known exe/arch pairs are read: every other header is a failed measurement (3)
for bad in "arch=x64-prism" "arch=arm64" "arch=x64-prism exe=other" "arch=x86 exe=anycpu" "arch=arm64 exe=x64"; do
  d="$t/wpf/bad-$(echo "$bad" | tr ' =' '__')"; mkdir -p "$d"
  { echo "# mode=perf pid=12 launch_ns=-100000000 probe_busy_ns=0 wait_ns=0 $bad shell=wpf"; printf '500000000\tev\twindowDrawn\t0\n'; } > "$d/perf-12.tsv"
  rc=0; "$st" samples "$d" > "$d/out.txt" 2>&1 || rc=$?
  ok '[ "$rc" = 3 ] && grep -q "^MEASUREMENT FAILED: " "$d/out.txt"' "wpf: $bad refused (3)"
done
# P9 per input batch ("mark P9 batch"), its window ending at the dialog's end: a row drawn after
# the dialog closed is no sample
d="$t/p9/1"; mkdir -p "$d"
{
  echo "# mode=perf pid=9 launch_ns=-1000000 probe_busy_ns=0 arch=arm64"
  printf '1000000\tcmd\tmark P9 none\t0\n1200000\tcmd\tmark P9 batch\t0\n1300000\tfield\tprepare.tracksTable:reloadDataForRowIndexes\t0\n1400000\tturn\tturn\t100\n'
  printf '5000000\tcmd\tmark P9 main\t0\n6000000\tev\tdialog\t1\n6200000\tcmd\tmark P9 batch\t0\n6300000\tfield\tprepare.tracksTable:reloadDataForRowIndexes\t1\n6400000\tturn\tturn\t100\n'
  printf '7000000\tcmd\tmark P9 batch\t0\n13000000\tev\tdialogEnd\t1\n14000000\tfield\tprepare.tracksTable:reloadDataForRowIndexes\t1\n14100000\tturn\tturn\t100\n'
} > "$d/perf-9.tsv"
ok '"$st" samples "$d" | grep -q "^P9.none 1 0.200$" && "$st" samples "$d" | grep -q "^P9.main 1 0.200$"' "P9 per batch and kind; a row after the dialog's end is no sample"
# P6 on a list reloaded as its rows come (the G1 spike): it ends at the reload with the whole list
d="$t/p6/1"; mkdir -p "$d"
{
  echo "# mode=perf pid=1 launch_ns=-100000000 probe_busy_ns=1000 arch=arm64"
  printf '19000000\tcmd\tmark P6 full 20k\t0\n20000000\tcmd\tselect 150\t0\n21000000\tfield\tprepare.tracksTable:reloadData\t5000\n22000000\tturn\tturn\t100\n'
  printf '25000000\tfield\tprepare.tracksTable:reloadData\t20000\n26000000\tturn\tturn\t100\n30000000\tcmd\tmark end\t0\n'
} > "$d/perf-1.tsv"
ok '"$st" samples "$d" | grep -q "^P6 1 6.000$"' "P6: the window's largest reload (20,000 rows), not the first with 5,000"
ok '[ "$(sed -n "/^steps()/,/^}/p" "$root/tests/harness/scenarios/perf-spike-20k.sh" | grep -A2 "^  mark settled" | sed -n "2p;3p" | sed "s/ *#.*//" | tr -s " " | tr "\n" "|")" = " CMD_WAIT_S=20 cmd quiet 1500 15000| mark \"P6 full 20k\"|" ]' "perf-spike-20k: the spike starts at mark settled, a quiet, then P6's labelled select"
echo 5000 > "$d/p6-rows"
ok '"$st" samples "$d" | grep -q "^P6 1 2.000$"' "P6: the scenario's row count (p6-rows) when it names one"
# compare, the owner's rule (2026-11-12): a slowdown counts only when over both 10% and 20 ms
cmp() { "$st" compare "$1" --no-session --rules "${2:-$t/rules.txt}" > "$t/cmp.txt"; rc=$?; }
cmp "$t/results"
ok '[ "$rc" = 0 ] && grep -q "^compare: pass$" "$t/cmp.txt" && grep -q "^P1	100.000	100.000	+0.000	20.000	pass	1	1$" "$t/cmp.txt"' "compare: B equal to A passes"
ok 'grep -q "^P3.busy	.*	info	" "$t/cmp.txt" && grep -q "^P7.done.full	.*	info	" "$t/cmp.txt"' "compare: the numbers with no row are printed for information"
setdrawn() { sed -i '' "s/^[0-9]*	ev	windowDrawn/$1	ev	windowDrawn/" "$t/results/B-1/perf-x/1/perf-1.tsv"; }
setdrawn 30000000; cmp "$t/results"
ok '[ "$rc" = 1 ] && grep -q "^compare: REGRESSION: P1$" "$t/cmp.txt" && ! grep -q BORDERLINE "$t/cmp.txt"' "compare: +30 ms on a 100 ms start: a regression, named"
setdrawn 22000000; cmp "$t/results"
ok '[ "$rc" = 1 ] && grep -q "^# BORDERLINE P1: +22.000 against 20.000 allowed" "$t/cmp.txt"' "compare: +22 ms: a regression, BORDERLINE for a human"
setdrawn 18000000; cmp "$t/results"
ok '[ "$rc" = 0 ] && grep -q "pass BORDERLINE" "$t/cmp.txt"' "compare: +18 ms: a pass, BORDERLINE"
setdrawn 12000000; cmp "$t/results"
ok '[ "$rc" = 0 ] && ! grep -q BORDERLINE "$t/cmp.txt"' "compare: +12 ms (12%, under the 20 ms floor): a clear pass"
setdrawn 0
sed -i '' 's/^27000000	turn/900000000	turn/' "$t/results/B-1/perf-x/1/perf-1.tsv"; cmp "$t/results"
ok '[ "$rc" = 0 ] && grep -q "^P7.done.full	3.750	" "$t/cmp.txt"' "compare: an information number 100x slower never blocks"
sed -i '' 's/^900000000	turn/27000000	turn/' "$t/results/B-1/perf-x/1/perf-1.tsv"
# S3: a click whose reaction never comes (a slow or broken B) is missing data, never a smaller sample
cp "$t/results/B-1/perf-x/1/perf-1.tsv" "$t/b1.keep"; grep -v "^10000000	field	c.status" "$t/b1.keep" > "$t/results/B-1/perf-x/1/perf-1.tsv"; cmp "$t/results"
ok '[ "$rc" = 3 ] && grep -q "P5: B-1/perf-x: 1 clicks without their reaction" "$t/cmp.txt" && ! grep -q "^_P5" "$t/cmp.txt"' "compare: a click without its reaction: the measurement failed (3), no private key printed"
cp "$t/b1.keep" "$t/results/B-1/perf-x/1/perf-1.tsv"
# S2: a launch of the row's scenarios without the row's sample (B-4 with no footprint-after7.txt)
fp="$t/fp"; rm -rf "$fp"
for s in A B; do for k in 1 2 3 4; do d="$fp/$s-$k/perf-start/1"; mkdir -p "$d"; printf 'ok\nperf-start reopen perf: 0 failed\n' > "$d/result"; echo "phys_footprint: 90 MB" > "$d/footprint-after7.txt"; done; done
printf 'P10.after7 median 10%% 20MB Prepare mac-A\n@launches mac-A perf-start 4 P10.after7\n' > "$t/fp-rules.txt"
cmp "$fp" "$t/fp-rules.txt"; ok '[ "$rc" = 0 ]' "compare: P10.after7 in every launch on both sides passes"
rm "$fp/B-4/perf-start/1/footprint-after7.txt"; cmp "$fp" "$t/fp-rules.txt"
ok '[ "$rc" = 3 ] && grep -q "P10.after7: B-4/perf-start has no sample" "$t/cmp.txt"' "compare: a launch without its gating sample: the measurement failed (3)"
printf 'P14 median 10%% 20ms first mac-A\n' > "$t/p14-rules.txt"; cmp "$t/results" "$t/p14-rules.txt"
ok '[ "$rc" = 3 ] && grep -q "^compare: MEASUREMENT FAILED: P14: no samples on both sides" "$t/cmp.txt"' "compare: a gating row with no sample: the measurement failed (3)"
for bad in 'P1 median 10%% 20lightyears first mac-A' 'P1 median 10%% 20ms first mac-A -' 'P1 p95 10%% 20ms first mac-A' 'P1 median +50ms 0 first mac-A' '@runs mac-A 1' 'P-B1.warm median 10%% 20ms first B0'; do
  printf "$bad\n" > "$t/bad-rules.txt"; cmp "$t/results" "$t/bad-rules.txt"
  ok '[ "$rc" = 3 ]' "compare: a rules file that can't be read or has no mac-A row is refused (3): $bad"
done
ok '"$st" rules >/dev/null && python3 -I "$root/tests/perf/rules.py" check >/dev/null' "the real thresholds.txt reads"
# the plan: a gating metric is taken from the scenarios that name it; a FAILed run doesn't count
cp -R "$t/results/B-1/perf-x" "$t/results/B-1/perf-y"; sed -i '' 's/^0	ev	windowDrawn/500000000	ev	windowDrawn/' "$t/results/B-1/perf-y/1/perf-1.tsv"
printf 'P1 median 10%% 20ms first mac-A\n@launches mac-A perf-x 1 P1\n' > "$t/plan-rules.txt"; cmp "$t/results" "$t/plan-rules.txt"
ok '[ "$rc" = 0 ]' "compare: a slow P1 from a scenario the plan doesn't name for P1: not gated"
cmp "$t/results" "$t/rules.txt"
ok '[ "$rc" = 1 ]' "compare: without @launches, every scenario's P1 counts"
echo "FAIL something" >> "$t/results/B-1/perf-y/1/result"; cmp "$t/results"
ok '[ "$rc" = 0 ]' "compare: a launch with a FAIL line isn't counted"
rm -rf "$t/results/B-1/perf-y"
# P13 and P15: information
printf 'swift 2000\nc 1500\n' > "$t/p15.txt"
ok '"$st" compare "$t/results" --no-session --rules "$t/rules.txt" --p15 "$t/p15.txt" | grep -q "^P15	2.000	1.500	-0.500	-	info	1	1$"' "P15 from its file: both sides printed, information"
# session.txt: provenance, the plan, the lines that fail the measurement, the knobs
pv="$t/prov"; rm -rf "$pv"; mkdir -p "$pv"; cp -R "$t/results/A-1" "$t/results/B-1" "$pv/"
for s in A B; do printf 'app_sha256=%064d\nclone=c1\n' "$([ $s = A ] && echo 1 || echo 2)" > "$pv/$s-1/perf-x/1/build.txt"; done
sess() { printf 'label=prov mode=ab clone=c1 started=x\nA=a.zip sha256=%064d\nB=b.zip sha256=%064d\nscenarios=perf-x PERF_IDLE_S=%s PERF_P12_S=default PERF_ROUNDS=default\nplan_A=perf-x:%s\nplan_B=perf-x:1\n' 1 2 "${2:-default}" "${1:-1}" > "$pv/session.txt"; }
pcmp() { "$st" compare "$pv" --rules "$t/rules.txt" > "$t/pv.txt"; rc=$?; }
sess; pcmp
ok '[ "$rc" = 0 ]' "session: a clean session of the session's builds passes"
sess 2; pcmp
ok '[ "$rc" = 3 ] && grep -q "A: perf-x: 1 clean launches, 2 planned" "$t/pv.txt"' "session: a launch of the plan missing: the measurement failed"
sess; echo "disturbed=B-1/perf-x (input during the launch: idle 3 s)" >> "$pv/session.txt"; pcmp
ok '[ "$rc" = 3 ] && grep -q "disturbed=B-1" "$t/pv.txt"' "session: a launch disturbed by input: the measurement failed"
sess; echo "failed=A-1/perf-x" >> "$pv/session.txt"; pcmp
ok '[ "$rc" = 3 ]' "session: a failed launch: the measurement failed"
sess 1 30; pcmp
ok '[ "$rc" = 3 ] && grep -q "PERF_IDLE_S=30" "$t/pv.txt"' "session: a knob set (PERF_IDLE_S): not a gate (3)"
sess; printf 'app_sha256=%064d\nclone=c1\n' 3 > "$pv/B-1/perf-x/1/build.txt"; pcmp
ok '[ "$rc" = 3 ]' "session: a launch of another build doesn't count (a hole in the plan)"
printf 'app_sha256=%064d\nclone=c2\n' 2 > "$pv/B-1/perf-x/1/build.txt"; pcmp
ok '[ "$rc" = 3 ]' "session: a launch of another clone doesn't count"
printf 'app_sha256=%064d\nclone=c1\n' 2 > "$pv/B-1/perf-x/1/build.txt"
printf 'P1 median 10%% 20ms first mac-A\nP5 median 10%% 20ms first mac-A\n@launches mac-A perf-x 1 P1\n@launches mac-A perf-click 1 P5\n' > "$t/smoke-rules.txt"
"$st" compare "$pv" --rules "$t/smoke-rules.txt" > "$t/pv.txt"; rc=$?
ok '[ "$rc" = 0 ] && grep -q "^P5	.*not run (not in this session.s plan)" "$t/pv.txt"' "session: a session of some scenarios (a smoke set) judges their rows, the others not run"
printf 'P1 median 10%% 20ms first mac-A\n@launches mac-A perf-x 2 P1\n' > "$t/short-rules.txt"
"$st" compare "$pv" --rules "$t/short-rules.txt" > "$t/pv.txt"; rc=$?
ok '[ "$rc" = 3 ] && grep -q "perf-x: 1 launches, the plan has 2: not a gate" "$t/pv.txt"' "session: fewer launches than the plan (abab.sh -n, a smoke run): not a gate (3)"
rm -f "$pv/session.txt"; pcmp
ok '[ "$rc" = 3 ]' "session: no session.txt without --no-session: provenance unknown (3)"
# baselines: A alone, no verdict; a former session's checks noted
bl="$t/bl"; rm -rf "$bl"; mkdir -p "$bl"; cp -R "$t/results/A-1" "$bl/"
printf 'P5\tmedian\tA'"'"'s halves\t1.6\tnoise 1.6\troom 2\tNOISY: more launches\n' > "$bl/aa-1.txt"; printf 'P5\tmedian\tok\n' > "$bl/aa-final.txt"
printf '# overhead_final=1\nP7.done.full\tmedian\t6.014\n' > "$bl/overhead-charges.tsv.refused"
"$st" baselines "$bl" --no-session --rules "$t/rules.txt" --p15 "$t/p15.txt" --tsv "$t/bl.tsv" > "$t/bl.txt"; rc=$?
ok '[ "$rc" = 0 ] && grep -q "^P1	gate	100.000	1	1	100.0-100.0$" "$t/bl.tsv" && grep -q "^P15.swift	info	2.000	1" "$t/bl.tsv" && grep -q "^P3.busy	info	" "$t/bl.tsv"' "baselines: the gating rows with their launches' spread, every other number as information"
ok 'grep -q "^# noted: its A/A checks before the last (aa-1.txt) called NOISY: P5 median$" "$t/bl.tsv" && grep -q "^# noted: its last A/A check (aa-final.txt) called NOISY: none$" "$t/bl.tsv" && grep -q "^# noted: perf mode.s overhead (overhead-charges.tsv.refused.*P7.done.full median 6.014" "$t/bl.tsv"' "baselines: a former session's noisy rows and overhead charges noted, never a failure"
"$st" baselines "$bl" --no-session --rules "$t/p14-rules.txt" > /dev/null; rc=$?
ok '[ "$rc" = 3 ]' "baselines: a gating row with no sample: 3"

# trace.py's under-dialog rule (H1): what changes under each dialog, A against B; --i2 requires B to
# update under the dialogs A froze them under (origin main-queue-block)
ud="$t/ud"; rm -rf "$ud"; mkdir -p "$ud"
printf '# scenario: selftest\n# under-dialog: prepare.tracksTable\n' > "$ud/scen.sh"
udtrace() {   # udtrace FILE T_R0 T_R1: dialog 1 (event) 10-20, dialog 2 (main-queue-block) 30-40
  {
    printf '{"k":"ev","t":10,"e":"dialog","n":1,"v":"one","origin":"event"}\n'
    printf '{"k":"frame","t":%s,"n":1,"d":{"prepare.tracksTable.r0":"a"}}\n' "$2"
    printf '{"k":"ev","t":20,"e":"dialogEnd","n":1,"v":"one"}\n'
    printf '{"k":"ev","t":30,"e":"dialog","n":2,"v":"two","origin":"main-queue-block"}\n'
    printf '{"k":"ev","t":40,"e":"dialogEnd","n":2,"v":"two"}\n'
    printf '{"k":"frame","t":%s,"n":2,"d":{"prepare.tracksTable.r1":"b"}}\n' "$3"
  } | sort -t: -k3 -n > "$1"
}
udtrace "$ud/a.jsonl" 15 45; udtrace "$ud/moved.jsonl" 25 45; udtrace "$ud/i2.jsonl" 15 35
python3 -c 'import sys,json; [json.loads(l) for l in open(sys.argv[1])]' "$ud/a.jsonl" 2>/dev/null || echo bad
ok 'python3 "$root/tests/harness/trace.py" compare "$ud/a.jsonl" "$ud/a.jsonl" --scenario "$ud/scen.sh" >/dev/null' "under-dialog: A against itself passes"
ok '! python3 "$root/tests/harness/trace.py" compare "$ud/a.jsonl" "$ud/moved.jsonl" --scenario "$ud/scen.sh" >/dev/null' "under-dialog: rows that changed under a dialog in A, moved after it in B, fail"
ok '! python3 "$root/tests/harness/trace.py" compare "$ud/a.jsonl" "$ud/a.jsonl" --scenario "$ud/scen.sh" --i2 >/dev/null' "under-dialog --i2: B frozen under a former work-queue dialog fails"
ok 'python3 "$root/tests/harness/trace.py" compare "$ud/a.jsonl" "$ud/i2.jsonl" --scenario "$ud/scen.sh" --i2 >/dev/null' "under-dialog --i2: B updating under it passes"
ok '! python3 "$root/tests/harness/trace.py" compare "$ud/a.jsonl" "$ud/i2.jsonl" --scenario "$ud/scen.sh" >/dev/null' "under-dialog without --i2: A against A must match dialog by dialog"
# a date picker's unset value moves daily: read as "unset" (H15)
printf '{"k":"frame","t":1,"n":1,"d":{"prepare.until.days":"-9444"}}\n' > "$ud/d1.jsonl"
printf '{"k":"frame","t":1,"n":1,"d":{"prepare.until.days":"-9445"}}\n' > "$ud/d2.jsonl"
ok 'python3 "$root/tests/harness/trace.py" compare "$ud/d1.jsonl" "$ud/d2.jsonl" >/dev/null' "an unset date picker (≤ -3650 days) compares equal across days"
printf '{"k":"frame","t":1,"n":1,"d":{"prepare.until.days":"30"}}\n' > "$ud/d3.jsonl"
ok '! python3 "$root/tests/harness/trace.py" compare "$ud/d1.jsonl" "$ud/d3.jsonl" >/dev/null' "a set date picker still counts"
# must-show scoped per trace (H24): [.p2] applies to the second copy only
printf '# scenario: selftest\n# must-show: x=1\n# must-show[.p2]: y=2\n' > "$ud/scope.sh"
printf '{"k":"frame","t":1,"n":1,"d":{"x":"1"}}\n' > "$ud/p1.jsonl"; printf '{"k":"frame","t":1,"n":1,"d":{"y":"2"}}\n' > "$ud/p2.jsonl"
ok 'python3 "$root/tests/harness/trace.py" check "$ud/p1.jsonl" --scenario "$ud/scope.sh" >/dev/null && python3 "$root/tests/harness/trace.py" check "$ud/p2.jsonl" --scenario "$ud/scope.sh" --part 2 >/dev/null' "must-show scoped: each trace its own"
ok '! python3 "$root/tests/harness/trace.py" check "$ud/p2.jsonl" --scenario "$ud/scope.sh" >/dev/null' "an unscoped must-show applies to the first trace"
# update-server.sh: "next" is the source app's version plus one patch (H13); a release key is refused (H27)
us="$t/us"; rm -rf "$us"; mkdir -p "$us/src/x/RB Stems Plus.app/Contents" "$us/keys"
printf '<?xml version="1.0" encoding="UTF-8"?>\n<plist version="1.0"><dict><key>CFBundleShortVersionString</key><string>1.4.0</string></dict></plist>\n' > "$us/src/x/RB Stems Plus.app/Contents/Info.plist"
(cd "$us/src/x" && ditto -c -k --keepParent "RB Stems Plus.app" "$us/src/rbstemsplus-app.zip")
echo bridge > "$us/src/libonnxruntime.1.18.0.dylib"; echo model > "$us/src/stemsplus-model.onnx"
printf '{"payload_version":"1.4.0","app":{"version":"1.4.0","zip":"rbstemsplus-app.zip","sha256":"x"},"bridge":{"file":"libonnxruntime.1.18.0.dylib","sha256":"x"}}\n' > "$us/src/payload.json"
openssl ecparam -name prime256v1 -genkey -noout -out "$us/test.pem" 2>/dev/null
ok 'RBSP_RELEASE_KEYS="$us/keys" "$root/tests/harness/update-server.sh" "$us/out" "$us/src" "$us/test.pem" --app-version next | grep -q "app 1.4.1"' "update-server.sh --app-version next: 1.4.0 becomes 1.4.1"
openssl ec -in "$us/test.pem" -pubout > "$us/keys/release.pub.pem" 2>/dev/null
RBSP_RELEASE_KEYS="$us/keys" "$root/tests/harness/update-server.sh" "$us/out2" "$us/src" "$us/test.pem" >/dev/null 2>&1; rc=$?
ok '[ "$rc" != 0 ] && [ ! -e "$us/out2/payload.json.sig" ]' "update-server.sh refuses to sign with a release key"
# g1-round.sh's verify steps fail when a check fails (H8), and verify-allow reads App Management's
# refusal, Try Again and success in one process from the app's log
g1="$t/g1"; rm -rf "$g1"; mkdir -p "$g1/st"
cat > "$g1/vm.sh" <<'STUB'
#!/bin/bash
[ "$1" = sh ] || exit 0
case "$2" in *app.log*) [ -n "${STUB_APPLOG:-}" ] && cat "$STUB_APPLOG"; exit 0 ;; esac
[ "${STUB_MODE:-fail}" = pass ]
STUB
chmod +x "$g1/vm.sh"
g1r() { G1_VM="$g1/vm.sh" G1_STATE="$g1/st" "$root/tests/harness/g1-round.sh" "$@" >/dev/null 2>&1; }
: > "$g1/st/rekordbox-before.txt"
ok 'grep -q "sudo sh -c .if \[ -f .*wc -l <" "$root/tests/harness/g1-round.sh" && ! grep -q "sudo wc -l <" "$root/tests/harness/g1-round.sh"' "g1-round.sh switch: the app log's length is read as root (sudo sh -c), never with a redirect outside sudo"
STUB_MODE=fail g1r verify-uninstall v1.3.0; rc=$?
ok '[ "$rc" = 1 ] && grep -q "NOT VERIFIED" "$g1/st/verify.txt" && ! grep -q "^verified" "$g1/st/verify.txt"' "g1-round.sh: a failed check fails the verify step (exit 1, never \"verified\")"
: > "$g1/st/verify.txt"
STUB_MODE=pass g1r verify-uninstall v1.3.0; rc=$?
ok '[ "$rc" = 0 ] && grep -q "^verified: the complete uninstall" "$g1/st/verify.txt"' "g1-round.sh: all checks passing: verified, exit 0"
printf '%s\n' "x  started: RB Stems Plus 1.4.0 (1.4.0)" "x  admin run (reinstall), attempt 1" "x    FAILED: cp: Operation not permitted [exit 1]" "x    user chose: Try Again" "x  admin run (reinstall), attempt 2" "x    ok: rekordbox checked" > "$g1/app-ok.log"
printf '%s\n' "x  started: RB Stems Plus 1.4.0 (1.4.0)" "x  admin run (reinstall), attempt 1" "x    FAILED: cp: Operation not permitted [exit 1]" "x    user chose: Try Again" "x  started: RB Stems Plus 1.4.0 (1.4.0)" "x    ok: rekordbox checked" > "$g1/app-relaunch.log"
STUB_APPLOG="$g1/app-ok.log" g1r verify-allow v1.3.0; rc=$?
ok '[ "$rc" = 0 ]' "verify-allow: refused, Try Again, success in the same process"
STUB_APPLOG="$g1/app-relaunch.log" g1r verify-allow v1.3.0; rc=$?
ok '[ "$rc" = 1 ]' "verify-allow: a success only after a relaunch isn't the same process"
# b0.sh status: B0 is one complete run set; a partial one doesn't count
b0g="$t/b0ga"; rm -rf "$b0g"
mkdir -p "$b0g/B0-records/20261110-2300.partial/bridge"
ok 'GATES_ARCHIVE="$b0g" "$root/tests/perf/b0.sh" status 2>/dev/null | grep -q "B0: incomplete"' "b0.sh: a partial set is not B0"
s=20261111-0100; mkdir -p "$b0g/B0-records/$s/bridge" "$b0g/B0-records/$s/helper"; echo "# x" > "$b0g/B0-records/$s/bridge/samples.tsv"; echo "# x" > "$b0g/B0-records/$s/helper/samples.tsv"
ok 'GATES_ARCHIVE="$b0g" "$root/tests/perf/b0.sh" status 2>/dev/null | grep -q "B0: complete"' "b0.sh: one complete set: complete"
B0_BRIDGE_PERF=/bin/true GATES_ARCHIVE="$b0g" "$root/tests/perf/b0.sh" record --volume /Volumes/x > "$t/b0r.out" 2>&1; rc=$?
ok '[ "$rc" = 2 ] && grep -q "are the selftest.s" "$t/b0r.out"' "b0.sh record: a real record refuses the selftest's stubs"
# bridge/tests/perf.sh gate (GP, bridge.md §0b) with a stub run set (BRIDGE_PERF_TEST): two
# alternating pairs, the owner's rule, the idle guard
gq="$t/gp"; rm -rf "$gq"; mkdir -p "$gq/A" "$gq/B"
echo "built: stub A" > "$gq/A/IDENTITY"; echo "built: stub B" > "$gq/B/IDENTITY"
cat > "$gq/bench.sh" <<'STUB'
#!/bin/bash
# a run set: every metric of the gate, 4 samples of 100 ms (P-B7: one value), B's times STUB_GP_BMUL
dir="$1" side="$2" round="$3" out="$4" meta="$5"
echo "$side-$round" >> "$STUB_GP_DIR/log"
[ "${STUB_GP_EMPTY:-}" = "$side-$round" ] && exit 0      # a set that wrote no samples
mul=1; [ "$side" = B ] && mul="${STUB_GP_BMUL:-1}"
for x in P-B1.warm P-B1.warm-drive P-B1.cold P-B2.near2k P-B2.near20k P-B3.miss P-B3.miss-rebuilt P-B4.store P-B4.index20k P-B4.prune10k P-B6.start P-B8.memo P-B8.nomemo; do
  for i in 1 2 3 4; do awk -v x="$x" -v s="$side" -v r="$round" -v m="$mul" 'BEGIN { printf "%s\t%s\t%s\t%d\n", x, s, r, 100000000 * m }' >> "$out"; done
done
printf 'P-B7.rss1000\t%s\t%s\t52428800\nP-B7.size\t%s\t%s\t512000\n' "$side" "$round" "$side" "$round" >> "$out"
[ "${STUB_GP_SHORT:-}" = "$side-$round" ] && echo "  WARNING P-B3.miss: 3 samples, expected 4" >> "$meta"
[ "${STUB_GP_DISTURB:-}" = "$side-$round" ] && { echo 0 > "$STUB_GP_DIR/idle-next"; sleep 1; }
if [ "${STUB_GP_DISTURB_ONCE:-}" = "$side-$round" ] && [ ! -e "$STUB_GP_DIR/once" ]; then : > "$STUB_GP_DIR/once"; echo 0 > "$STUB_GP_DIR/idle-next"; sleep 1; fi
[ "${STUB_GP_UNREADABLE:-}" = "$side-$round" ] && echo x > "$STUB_GP_DIR/idle-next"
exit 0
STUB
cat > "$gq/idle.sh" <<'STUB'
#!/bin/bash
f="$STUB_GP_DIR/idle-next"
if [ -f "$f" ]; then cat "$f"; rm -f "$f"; else echo 100000; fi
STUB
chmod +x "$gq/bench.sh" "$gq/idle.sh"
pf="$root/bridge/tests/perf.sh"
gp() {   # gp NAME ARGS…: a stub session into $gq/NAME (its own stub folder), the exit status in $rc
  local n="$1"; shift
  rm -rf "$gq/$n.d"; mkdir -p "$gq/$n.d"
  STUB_GP_DIR="$gq/$n.d" BRIDGE_PERF_TEST=1 BRIDGE_PERF_BENCH="$gq/bench.sh" PERF_IDLE_CMD="$gq/idle.sh" PERF_IDLE_POLL=0 \
    GATES_ARCHIVE="$gq/archive" "$pf" gate "$@" > "$gq/$n.out" 2>&1
  rc=$?
}
gs() { cat "$gq/$1/session.txt" 2>/dev/null; }
order() { tr '\n' ' ' < "$gq/$1.d/log" | sed 's/ $//'; }
gp clean "$gq/A" "$gq/B" "$gq/clean"
ok '[ "$rc" = 3 ] && gs clean | grep -qx "verdict=0" && grep -q "tooling check only" "$gq/clean/gate.txt" && [ "$(order clean)" = "A-p1 B-p1 B-p2 A-p2" ]' "GP: a clean stub session judges 0 over two alternating pairs (A B, B A); stubs are a tooling check only (3)"
ok 'grep -q "^GP: information only (A is not B0, " "$gq/clean/gate.txt" && ! grep -q "^GP: pass" "$gq/clean/gate.txt"' "GP: a gate whose A isn't the archive's B0 is information only, never \"GP: pass\""
mkdir -p "$gq/archive"; ln -s "$gq/A" "$gq/archive/B0"
gp isb0 "$gq/A" "$gq/B" "$gq/isb0"
ok 'grep -q "^GP: pass" "$gq/isb0/gate.txt" && ! grep -q "information only" "$gq/isb0/gate.txt" && gs isb0 | grep -qx "verdict=0"' "GP: ...with A the archive's B0 it is the gate"
STUB_GP_BMUL=1.3 gp regr "$gq/A" "$gq/B" "$gq/regr"
ok 'gs regr | grep -qx "verdict=1" && grep -q "^GP: REGRESSION: P-B1.cold, P-B1.warm" "$gq/regr/gate.txt" && grep -q "^P-B4.store .* info$" "$gq/regr/gate.txt"' "GP: +30 ms on 100 ms: a regression, each row named; the rows with no rule information"
STUB_GP_BMUL=1.18 gp border "$gq/A" "$gq/B" "$gq/border"
ok 'gs border | grep -qx "verdict=0" && grep -q "^GP: borderline, for a human to judge: P-B1.cold" "$gq/border/gate.txt"' "GP: +18 ms: a pass, borderline for a human"
STUB_GP_BMUL=1.15 gp under "$gq/A" "$gq/B" "$gq/under"
ok 'gs under | grep -qx "verdict=0" && ! grep -q "borderline" "$gq/under/gate.txt"' "GP: +15 ms (15%, under the 20 ms floor): a pass"
rm -f "$gq/archive/B0"
STUB_GP_SHORT=B-p1 gp short "$gq/A" "$gq/B" "$gq/short"
ok '[ "$rc" = 3 ] && gs short | grep -q "^short=B-p1" && gs short | grep -q "^finished=.* rc=3" && [ "$(order short)" = "A-p1 B-p1" ]' "GP: a short set ends the session: the measurement failed (3)"
STUB_GP_EMPTY=B-p2 gp empty "$gq/A" "$gq/B" "$gq/empty"
ok '[ "$rc" = 3 ] && gs empty | grep -q "^short=B-p2"' "GP: a set that wrote no samples is short (3)"
STUB_GP_DISTURB=B-p2 gp dist "$gq/A" "$gq/B" "$gq/dist" --idle-wait 5
ok '[ "$rc" = 3 ] && gs dist | grep -q "^disturbed=B-p2 " && gs dist | grep -q "^idle_at=A-p1 100000" && gs dist | grep -q "^display_start=" && [ "$(order dist)" = "A-p1 B-p1 B-p2 B-p2" ] && gs dist | grep -q "^disturbed_retried=B-p2 " && ! gs dist | grep -q "^verdict="' "GP: input during a set and again during its retry: the measurement failed (3)"
STUB_GP_DISTURB_ONCE=B-p1 gp once "$gq/A" "$gq/B" "$gq/once" --idle-wait 5
ok 'gs once | grep -qx "verdict=0" && gs once | grep -q "^disturbed_retried=B-p1 " && ! gs once | grep -q "^disturbed=" && [ "$(order once)" = "A-p1 B-p1 B-p1 B-p2 A-p2" ] && [ "$(grep -c "	B	p1	" "$gq/once/samples.tsv")" = "$(grep -c "	A	p1	" "$gq/once/samples.tsv")" ]' "GP: a disturbed set runs again once (choice 4), its samples counted once"
STUB_GP_UNREADABLE=A-p1 gp unread "$gq/A" "$gq/B" "$gq/unread" --idle-wait 5
ok '[ "$rc" = 2 ] && gs unread | grep -q "^stopped=.* rc=2 HIDIdleTime unreadable"' "GP: an idle time that can't be read stops the session (2)"
gp dry "$gq/A" "$gq/B" "$gq/dry" --dry-run
ok '[ "$rc" = 0 ] && grep -q "4 run sets, A B then B A" "$gq/dry.out" && [ ! -e "$gq/dry" ]' "GP --dry-run: the plan, nothing run"
gp twice "$gq/A" "$gq/B" "$gq/clean"
ok '[ "$rc" = 2 ] && grep -q "exists: a new OUT" "$gq/twice.out"' "GP: an existing OUT is refused"
BRIDGE_PERF_BENCH="$gq/bench.sh" "$pf" gate "$gq/A" "$gq/B" "$gq/real" > "$gq/real.out" 2>&1; rc=$?
ok '[ "$rc" = 2 ] && grep -q "are the selftest.s" "$gq/real.out" && [ ! -e "$gq/real" ]' "GP: a real gate refuses the selftest's stubs"
# helper/tests/perf.sh gate with a stub run set (HELPER_PERF_TEST): alternating pairs, the idle
# guard and its one retry (S6, choice 4)
hq="$t/hgp"; rm -rf "$hq"; mkdir -p "$hq/corpus"; : > "$hq/corpus/SHA256SUMS"
cat > "$hq/bench.sh" <<'STUB'
#!/bin/bash
dir="$1" side="$2" round="$3" out="$4" meta="$5"
echo "$side-$round" >> "$STUB_GP_DIR/log"
mul=1; [ "$side" = B ] && mul="${STUB_GP_BMUL:-1}"
for i in 1 2 3 4; do awk -v s="$side" -v r="$round" -v m="$mul" 'BEGIN { printf "P-H1.run.demucs4\t%s\t%s\t%d\n", s, r, 1000000000 * m }' >> "$out"; done
[ "${STUB_GP_SHORT:-}" = "$side-$round" ] && echo "  WARNING P-H1: short" >> "$meta"
[ "${STUB_GP_DISTURB:-}" = "$side-$round" ] && { echo 0 > "$STUB_GP_DIR/idle-next"; sleep 1; }
if [ "${STUB_GP_DISTURB_ONCE:-}" = "$side-$round" ] && [ ! -e "$STUB_GP_DIR/once" ]; then : > "$STUB_GP_DIR/once"; echo 0 > "$STUB_GP_DIR/idle-next"; sleep 1; fi
[ "${STUB_GP_UNREADABLE:-}" = "$side-$round" ] && echo x > "$STUB_GP_DIR/idle-next"
exit 0
STUB
chmod +x "$hq/bench.sh"
hp="$root/helper/tests/perf.sh"
hgp() {   # hgp NAME ARGS…: a stub helper gate into $hq/NAME, the exit status in $rc
  local n="$1"; shift
  rm -rf "$hq/$n.d"; mkdir -p "$hq/$n.d"
  STUB_GP_DIR="$hq/$n.d" HELPER_PERF_TEST=1 HELPER_PERF_BENCH="$hq/bench.sh" PERF_IDLE_CMD="$gq/idle.sh" PERF_IDLE_POLL=0 \
    "$hp" gate "$gq/A" "$gq/B" "$hq/$n" --corpus "$hq/corpus" "$@" > "$hq/$n.out" 2>&1
  rc=$?
}
hs() { cat "$hq/$1/session.txt" 2>/dev/null; }
horder() { tr '\n' ' ' < "$hq/$1.d/log" | sed 's/ $//'; }
hgp clean --idle-wait 5
ok '[ "$rc" = 3 ] && hs clean | grep -qx "verdict=0" && [ "$(horder clean)" = "A-r1 B-r1 B-r2 A-r2" ] && hs clean | grep -q "^idle_at=A-r1 100000" && hs clean | grep -q "^display_start=" && grep -q "tooling check only" "$hq/clean/gate.txt"' "helper GP: alternating pairs under the idle guard, judged 0; stubs are a tooling check only (3)"
STUB_GP_BMUL=1.3 hgp regr
ok 'hs regr | grep -qx "verdict=1" && grep -q "^GP: REGRESSION: P-H1.run.demucs4" "$hq/regr/gate.txt"' "helper GP: +30% per track: a regression, named"
STUB_GP_DISTURB_ONCE=B-r1 hgp once --idle-wait 5
ok 'hs once | grep -qx "verdict=0" && hs once | grep -q "^disturbed_retried=B-r1 " && [ "$(horder once)" = "A-r1 B-r1 B-r1 B-r2 A-r2" ] && [ "$(grep -c "	B	r1	" "$hq/once/samples.tsv")" = 4 ]' "helper GP: a disturbed set runs again once, its samples counted once"
STUB_GP_DISTURB=B-r2 hgp dist --idle-wait 5
ok '[ "$rc" = 3 ] && hs dist | grep -q "^disturbed=B-r2 " && ! hs dist | grep -q "^verdict="' "helper GP: input during a set and again during its retry: the measurement failed (3)"
STUB_GP_UNREADABLE=A-r1 hgp unread --idle-wait 5
ok '[ "$rc" = 2 ] && hs unread | grep -q "^stopped=.* rc=2 HIDIdleTime unreadable"' "helper GP: an idle time that can't be read stops it (2)"
STUB_GP_SHORT=A-r2 hgp short
ok '[ "$rc" = 3 ] && hs short | grep -qx "short=A-r2"' "helper GP: a set that came out short: the measurement failed (3)"
HELPER_PERF_BENCH="$hq/bench.sh" "$hp" gate "$gq/A" "$gq/B" "$hq/real" --corpus "$hq/corpus" > "$hq/real.out" 2>&1; rc=$?
ok '[ "$rc" = 2 ] && grep -q "are the selftest.s" "$hq/real.out" && [ ! -e "$hq/real" ]' "helper GP: a real gate refuses the selftest's stubs"
# abab.sh with a stub vm.sh (ABAB_VM): a TEST build folder whose payload.json its key signs
tb="$t/tb"; rm -rf "$tb"; mkdir -p "$tb/test-keys"
openssl ecparam -name prime256v1 -genkey -noout -out "$tb/test-keys/test-key.pem" 2>/dev/null
echo zip > "$tb/rbstemsplus-app.zip"; echo model > "$tb/stemsplus-model.onnx"; echo '{}' > "$tb/payload.json"
openssl dgst -sha256 -sign "$tb/test-keys/test-key.pem" -out "$tb/payload.json.sig" "$tb/payload.json"
cat > "$t/stubvm.sh" <<'STUB'
#!/bin/bash
case "$1" in
  install) exit 0 ;;
  run)
    label="$2"; shift 2
    for s in "$@"; do
      d="$STUB_ROOT/build/harness/results/$label/$s/1"; mkdir -p "$d"
      printf 'app_sha256=%s\nclone=unknown\n' "$STUB_SHA" > "$d/build.txt"; echo "$s reopen perf: 0 failed" > "$d/result"
      cp "$STUB_PERF" "$d/perf-1.tsv"
      echo "$label $SERVER_SRC $SERVER_KEY" >> "$STUB_LOG"
    done
    case "$label" in */warmup-*) exit 0 ;; esac
    [ -n "${STUB_SLEEP:-}" ] && sleep "$STUB_SLEEP"
    [ "${STUB_DISTURB:-}" = "${label##*/}" ] && echo 0 > "$STUB_IDLE_FILE"
    if [ "${STUB_DISTURB_ONCE:-}" = "${label##*/}" ] && [ ! -e "$STUB_IDLE_FILE.once" ]; then : > "$STUB_IDLE_FILE.once"; echo 0 > "$STUB_IDLE_FILE"; fi
    [ "${STUB_FAIL:-}" = "${label##*/}" ] && exit 1
    exit 0 ;;
esac
STUB
printf '#!/bin/bash\nv="$(cat "$STUB_IDLE_FILE")"; echo "$v"; [ "$v" = 0 ] && echo 1000 > "$STUB_IDLE_FILE"; true\n' > "$t/stubidle.sh"
chmod +x "$t/stubvm.sh" "$t/stubidle.sh"
ab="$root/tests/perf/abab.sh"; lb="selftest-abab-$$"; lbdir="$root/build/harness/results/$lb"
abenv() { rm -rf "$lbdir"; : > "$t/stub.log"; ABAB_IDLE_WAIT=0 ABAB_VM="$t/stubvm.sh" STUB_ROOT="$root" STUB_SHA="$(shasum -a 256 "$tb/rbstemsplus-app.zip" | cut -d' ' -f1)" STUB_LOG="$t/stub.log" STUB_PERF="$t/results/A-1/perf-x/1/perf-1.tsv" "$@" > "$t/ab.out" 2>&1; rc=$?; }
openssl ecparam -name prime256v1 -genkey -noout -out "$t/other-key.pem" 2>/dev/null
abenv env SERVER_KEY_A="$t/other-key.pem" "$ab" "$lb" "$tb/rbstemsplus-app.zip" -n 1 --scenarios perf-x
ok '[ "$rc" = 2 ] && [ ! -e "$lbdir/A-1" ]' "abab.sh: a side whose payload.json its key doesn't sign is refused"
abenv "$ab" "$lb" "$tb/rbstemsplus-app.zip" -n 2 --scenarios perf-x
ok '[ "$rc" = 0 ] && [ -d "$lbdir/A-2/perf-x/1" ] && [ ! -e "$lbdir/B-1" ] && grep -qx "plan_A=perf-x:2" "$lbdir/session.txt" && grep -qx "warmup=A/perf-x" "$lbdir/session.txt" && grep -q "^finished=.* rc=0" "$lbdir/session.txt"' "abab.sh: A alone (mac-A's reference): its plan, a warm-up launch, no verdict"
ok 'grep -q "server_A=$tb key_A=$tb/test-keys/test-key.pem" "$lbdir/session.txt"' "abab.sh: the side's own folder and key serve its scenarios"
abenv "$ab" "$lb" "$tb/rbstemsplus-app.zip" "$tb/rbstemsplus-app.zip" -n 2 --scenarios perf-x
ok '[ "$rc" = 3 ] && [ "$(sed -n "s| .*||p" "$t/stub.log" | tr "\n" " ")" = "$lb/warmup-A $lb/warmup-B $lb/A-1 $lb/B-1 $lb/B-2 $lb/A-2 " ] && grep -q "^compare: MEASUREMENT FAILED: no gating row in this session.s plan" "$lbdir/compare.txt" && grep -q "^finished=.* rc=3$" "$lbdir/session.txt"' "abab.sh: A and B interleaved (A B, then B A), then compare under thresholds.txt (no gating row is perf-x's: 3), session.txt's rc the compare's"
ok '"$st" compare "$lbdir" --rules "$t/rules.txt" | grep -q "^compare: pass$"' "stats.py reads abab.sh's session.txt (provenance, the plan)"
# S1: each scenario's two warm-ups (one per side) come before its first measured launch; stats.py
# never reads them (a warm-up 10 s slower changes nothing)
abenv "$ab" "$lb" "$tb/rbstemsplus-app.zip" "$tb/rbstemsplus-app.zip" -n 1 --scenarios "perf-x perf-y"
ok '[ "$(sed -n "s| .*||p" "$t/stub.log" | tr "\n" " ")" = "$lb/warmup-A $lb/warmup-B $lb/A-1 $lb/B-1 $lb/warmup-A $lb/warmup-B $lb/A-1 $lb/B-1 " ] && [ "$(grep "^warmup=" "$lbdir/session.txt" | tr "\n" " ")" = "warmup=A/perf-x warmup=B/perf-x warmup=A/perf-y warmup=B/perf-y " ]' "abab.sh: a warm-up launch per side before each scenario's measured launches"
sed -i '' 's/^0	ev	windowDrawn/10000000000	ev	windowDrawn/' "$lbdir/warmup-B/perf-y/1/perf-1.tsv"
ok '"$st" compare "$lbdir" --rules "$t/rules.txt" | grep -q "^compare: pass$"' "stats.py ignores the warm-up launches"
abenv "$ab" "$lb" "$tb/rbstemsplus-app.zip" -n 1 --scenarios perf-x
ABAB_VM="$t/stubvm.sh" "$ab" "$lb" "$tb/rbstemsplus-app.zip" -n 1 --scenarios perf-x > /dev/null 2>&1; rc=$?
ok '[ "$rc" = 2 ]' "abab.sh: an existing label is refused (two sessions never mix)"
abenv env STUB_FAIL=B-1 "$ab" "$lb" "$tb/rbstemsplus-app.zip" "$tb/rbstemsplus-app.zip" -n 2 --scenarios perf-x
ok '[ "$rc" = 3 ] && grep -qx "failed=B-1/perf-x" "$lbdir/session.txt" && [ ! -e "$lbdir/B-2" ] && grep -q "^finished=.* rc=3" "$lbdir/session.txt"' "abab.sh: a failed launch ends the session: the measurement failed (3), no replacement"
idenv() { echo 100000 > "$t/idle.txt"; abenv env ABAB_IDLE_WAIT=2 PERF_IDLE_POLL=1 PERF_IDLE_CMD="$t/stubidle.sh" STUB_IDLE_FILE="$t/idle.txt" STUB_SLEEP=1 "$@"; }
idenv env STUB_DISTURB=A-2 "$ab" "$lb" "$tb/rbstemsplus-app.zip" "$tb/rbstemsplus-app.zip" -n 2 --scenarios perf-x
ok '[ "$rc" = 3 ] && grep -q "^disturbed_retried=A-2/perf-x" "$lbdir/session.txt" && grep -q "^disturbed=A-2/perf-x" "$lbdir/session.txt" && grep -q "^idle_at=A-1 100000" "$lbdir/session.txt" && grep -q "^display_start=" "$lbdir/session.txt" && [ ! -e "$lbdir/compare.txt" ]' "abab.sh: input during a launch and again during its retry: the measurement failed (3)"
"$st" compare "$lbdir" --rules "$t/rules.txt" > "$t/dist.txt"; rc=$?
ok '[ "$rc" = 3 ] && grep -q "disturbed=A-2" "$t/dist.txt"' "stats.py: a session with a disturbed launch: the measurement failed"
rm -f "$t/idle.txt.once"; idenv env STUB_DISTURB_ONCE=A-1 "$ab" "$lb" "$tb/rbstemsplus-app.zip" -n 2 --scenarios perf-x
ok '[ "$rc" = 0 ] && grep -qx "disturbed_retried=A-1/perf-x (input during the launch: idle 0 s)" "$lbdir/session.txt" && ! grep -q "^disturbed=" "$lbdir/session.txt" && [ "$(grep -c "^dur=.* A-1/perf-x" "$lbdir/session.txt")" = 2 ] && [ -d "$lbdir/A-2/perf-x/1" ]' "abab.sh: a disturbed launch runs again once (choice 4), the session goes on"
ok '[ "$(python3 -I -c "import sys; sys.path.insert(0, sys.argv[1]); import stats; print(stats.session(sys.argv[2])[\"_bad\"])" "$root/tests/perf" "$lbdir")" = "[]" ]' "stats.py: a retried launch (disturbed_retried=) is no failure"
rm -f "$t/idle.txt.once"; idenv env STUB_DISTURB_ONCE=A-1 STUB_FAIL=A-2 "$ab" "$lb" "$tb/rbstemsplus-app.zip" -n 2 --scenarios perf-x
ok '[ "$rc" = 3 ] && grep -qx "failed=A-2/perf-x" "$lbdir/session.txt"' "abab.sh: a failed launch is never retried (3)"
idenv env PERF_IDLE_CMD=/usr/bin/false "$ab" "$lb" "$tb/rbstemsplus-app.zip" -n 1 --scenarios perf-x
ok '[ "$rc" = 2 ] && grep -q "^idle_unreadable=" "$lbdir/session.txt"' "abab.sh: an idle time that can't be read stops the session (2)"
rm -rf "$lbdir"
# g0.sh: the plan, mac-A's reference from a copy, a run with stubs (G0_TEST)
ok '"$root/tests/perf/g0.sh" plan | grep -q "^perf-start [0-9]"' "g0.sh plan: the text and the plan"
gc="$t/g0copy"; rm -rf "$gc"; mkdir -p "$gc/results" "$gc/perf"; cp -R "$bl" "$gc/results/g0-aa-x"; echo "aa=g0-aa-x" > "$gc/perf/g0-labels.txt"
printf 'label=x mode=a clone=c1\nA=a.zip sha256=%064d\nplan_A=perf-x:1\n' 1 > "$gc/results/g0-aa-x/session.txt"; printf 'app_sha256=%064d\nclone=c1\n' 1 > "$gc/results/g0-aa-x/A-1/perf-x/1/build.txt"
G0_RESULTS="$gc/results" G0_PERF_DIR="$gc/perf" G0_RULES="$t/rules.txt" "$root/tests/perf/g0.sh" baselines > /dev/null 2>&1; rc=$?
ok '[ "$rc" = 0 ] && grep -q "^P1	gate	100.000" "$gc/perf/g0-A-baselines.tsv"' "g0.sh baselines: the last aa= label of a copy into its g0-A-baselines.tsv"
G0_RESULTS="$gc/results" "$root/tests/perf/g0.sh" baselines > "$t/g0v8.txt" 2>&1; rc=$?
ok '[ "$rc" = 2 ] && grep -q "go together" "$t/g0v8.txt"' "g0.sh: G0_RESULTS without G0_PERF_DIR is refused"
printf '#!/bin/bash\necho "$*" >> "%s/g0stub.log"\nexit 0\n' "$t" > "$t/g0vm.sh"; printf '#!/bin/bash\necho "abab $*" >> "%s/g0stub.log"\nexit "${STUB_ABAB_RC:-0}"\n' "$t" > "$t/g0abab.sh"
# STUB_ABAB_SESSION: the session's folder as abab.sh leaves it (G0_TEST_RESULTS/LABEL): its session.txt
# with that line added, and compare.txt's measurement failure (a rehearsal's fewer launches, a missing sample)
cat > "$t/g0abab-session.sh" <<'STUB'
#!/bin/bash
echo "abab $*" >> "$G0_STUB_LOG"
d="$G0_TEST_RESULTS/$1"; mkdir -p "$d"
printf 'label=%s\n%s\n' "$1" "$STUB_ABAB_SESSION" > "$d/session.txt"
printf '%s\n' "compare: MEASUREMENT FAILED: perf-start: 1 launches, the plan has 4: not a gate; P6: no samples on B" > "$d/compare.txt"
exit 3
STUB
chmod +x "$t/g0vm.sh" "$t/g0abab.sh"
g0r() { : > "$t/g0stub.log"; G0_TEST=1 G0_PERF_DIR="$t/g0perf" G0_VM="$t/g0vm.sh" G0_ABAB="$t/g0abab.sh" G0_P15="${STUB_P15:-/bin/echo}" GATES_ARCHIVE="$gc/archive" "$root/tests/perf/g0.sh" run "$@" > "$t/g0r.txt" 2>&1; rc=$?; }
STUB_ABAB_RC=1 g0r "$tb/rbstemsplus-app.zip"
ok '[ "$rc" = 1 ] && grep -q "^abab gate-[0-9-]* .*mac-A/rbstemsplus-app.zip $tb/rbstemsplus-app.zip$" "$t/g0stub.log" && grep -qx "delete" "$t/g0stub.log" && grep -q "the session: REGRESSION (1)" "$t/g0r.txt"' "g0.sh run B.zip: a gate session against mac-A, the VM deleted, its verdict the exit"
STUB_ABAB_RC=3 g0r
ok '[ "$rc" = 3 ] && grep -q "^abab g0-aa-" "$t/g0stub.log" && ! grep -q "mac-A.s reference" "$t/g0r.txt"' "g0.sh run: A alone whose measurement failed: no reference exported"
# S4: the carried smoke set through g0.sh (the VM, the idle guard), --scenarios passed to abab.sh
g0r "$tb/rbstemsplus-app.zip" --scenarios "perf-activation perf-playlist-5000"
ok '[ "$rc" = 0 ] && grep -q "^abab gate-[0-9-]* .*mac-A/rbstemsplus-app.zip $tb/rbstemsplus-app.zip --scenarios perf-activation perf-playlist-5000$" "$t/g0stub.log"' "g0.sh run B.zip --scenarios: passed through to abab.sh"
g0r --scenarios perf-x
ok '[ "$rc" = 2 ] && [ ! -s "$t/g0stub.log" ]' "g0.sh run --scenarios without a B: refused, nothing run"
# the rehearsal (--quick): abab.sh -n 1, its own label, no P15, its verdict information; the idle
# guard off only for it
STUB_ABAB_RC=1 STUB_P15=/usr/bin/false g0r "$tb/rbstemsplus-app.zip" --quick --idle-wait 0
ok '[ "$rc" = 0 ] && grep -q "^abab rehearsal-[0-9-]* .*mac-A/rbstemsplus-app.zip $tb/rbstemsplus-app.zip -n 1$" "$t/g0stub.log" && grep -qx "delete" "$t/g0stub.log" && grep -q "REHEARSAL (--quick): not a measurement; one launch per scenario and side, idle guard 0s" "$t/g0r.txt" && ! grep -q "P15" "$t/g0r.txt" && grep -q "^rh=rehearsal-" "$t/g0perf/g0-labels.txt"' "g0.sh run B.zip --quick --idle-wait 0: a rehearsal, one launch each, no P15, its verdict information"
STUB_ABAB_RC=3 g0r "$tb/rbstemsplus-app.zip" --quick --scenarios "perf-start"
ok '[ "$rc" = 3 ] && grep -q "^abab rehearsal-[0-9-]* .*-n 1 --scenarios perf-start$" "$t/g0stub.log"' "g0.sh run --quick: a rehearsal whose launch failed fails; --scenarios passed through"
g0r "$tb/rbstemsplus-app.zip" --idle-wait 0
ok '[ "$rc" = 2 ] && [ ! -s "$t/g0stub.log" ]' "g0.sh run --idle-wait 0 without --quick: refused, nothing run"
# a rehearsal whose every launch ran: compare's fewer-launches failure is information, a missing sample named
chmod +x "$t/g0abab-session.sh"
g0q() { : > "$t/g0stub.log"; G0_STUB_LOG="$t/g0stub.log" G0_TEST_RESULTS="$t/g0res" G0_TEST=1 G0_PERF_DIR="$t/g0perf" G0_VM="$t/g0vm.sh" G0_ABAB="$t/g0abab-session.sh" G0_P15=/bin/echo GATES_ARCHIVE="$gc/archive" "$root/tests/perf/g0.sh" run "$@" > "$t/g0r.txt" 2>&1; rc=$?; }
STUB_ABAB_SESSION="dur=75 B-1/perf-start" g0q "$tb/rbstemsplus-app.zip" --quick --idle-wait 0
ok '[ "$rc" = 0 ] && grep -q "rehearsal: missing samples to look at (information): P6: no samples on B$" "$t/g0r.txt" && grep -q "fewer launches than the plan) is information" "$t/g0r.txt"' "g0.sh run --quick: every launch ran: compare's fewer launches are information, the missing sample named (exit 0)"
STUB_ABAB_SESSION="failed=B-1/perf-start" g0q "$tb/rbstemsplus-app.zip" --quick --idle-wait 0
ok '[ "$rc" = 3 ]' "g0.sh run --quick: a launch that failed fails the rehearsal"
STUB_ABAB_SESSION="dur=75 B-1/perf-start" g0q "$tb/rbstemsplus-app.zip"
ok '[ "$rc" = 3 ]' "g0.sh run (a gate): compare's measurement failure stays the exit"
g0r --quick
ok '[ "$rc" = 2 ] && [ ! -s "$t/g0stub.log" ]' "g0.sh run --quick without a B: refused, nothing run"
# S5: P15 is information only: its failure never becomes the verdict
STUB_P15=/usr/bin/false g0r "$tb/rbstemsplus-app.zip"
ok '[ "$rc" = 0 ] && grep -q "P15: FAILED (information, not the verdict)" "$t/g0r.txt"' "g0.sh run: a failed P15 is information, the session's verdict stands"
# S4: abab.sh's idle guard is on by default (600 s): an unreadable idle time stops it before any launch
abenv env -u ABAB_IDLE_WAIT PERF_IDLE_CMD=/usr/bin/false "$ab" "$lb" "$tb/rbstemsplus-app.zip" -n 1 --scenarios perf-x
ok '[ "$rc" = 2 ] && grep -q "^idle_unreadable=.* before the first launch" "$lbdir/session.txt" && [ ! -s "$t/stub.log" ]' "abab.sh: the idle guard is on without ABAB_IDLE_WAIT"
rm -rf "$lbdir"

# thresholds.txt's absolute limits (@limit, the Windows app's): read, and a bad one refused
ok 'python3 -I "$root/tests/perf/rules.py" check | grep -q "rules for WB.*absolute limits"' "rules.py check: the Windows rows and the absolute limits read"
for bad in "@limit P1 mean 1s first WB" "@limit P1 median 1 first WB" "@limit P8.early share 1% Prepare WP" "@limit P8.early share>16700 1% Prepare WP" "@limit P8.early share>16700us 1ms Prepare WP" "@limit P1 median 1s first mac-B" "@limit P1 median 1s first WB extra"; do
  { cat "$root/tests/perf/thresholds.txt"; echo "$bad"; } > "$t/thr-bad.txt"
  ok '! python3 -I "$root/tests/perf/rules.py" check "$t/thr-bad.txt" >/dev/null' "rules.py refuses \"$bad\""
done
{ cat "$root/tests/perf/thresholds.txt"; echo "@limit P1 median 2s first WB"; } > "$t/thr-twice.txt"
ok '! python3 -I "$root/tests/perf/rules.py" check "$t/thr-twice.txt" >/dev/null' "rules.py refuses a limit given twice"
# the Windows scenario table (scenarios-win.tsv, app.md W10): every scenario script once, the
# Windows-only ones as win:<name>, each row's tag and verdict kind known
ok 'python3 -I - "$root" <<"PY"
import os, sys
root = sys.argv[1]
names = sorted(f[:-3] for f in os.listdir(os.path.join(root, "tests/harness/scenarios")) if f.endswith(".sh"))
rows = [l.rstrip("\n").split("\t") for l in open(os.path.join(root, "tests/harness/scenarios-win.tsv"), encoding="utf-8") if not l.startswith("#")]
head, rows = rows[0], rows[1:]
bad = [r for r in rows if len(r) != 5 or r[1] not in ("first", "Prepare") or r[2] not in ("run", "properties", "mac-only", "later") or not r[3] or not r[4]]
seen = [r[0] for r in rows]
# the tag of a row: the "# tag:" header of its script, or a tag of its rows in scenarios.tsv (drive-replug has both)
tags = {}
for n in names:
    for l in open(os.path.join(root, "tests/harness/scenarios", n + ".sh"), encoding="utf-8"):
        if l.startswith("# tag:"): tags.setdefault(n, set()).add(l.split(":", 1)[1].strip())
for l in open(os.path.join(root, "tests/harness/scenarios.tsv"), encoding="utf-8"):
    c = l.rstrip("\n").split("\t")
    if not l.startswith("#") and len(c) > 3: tags.setdefault(c[3], set()).add(c[2])
problems = {"bad rows": bad, "missing": [n for n in names if n not in seen], "twice": sorted({n for n in seen if seen.count(n) > 1}),
            "unknown": [n for n in seen if not n.startswith("win:") and n not in names],
            "tag unlike its script and scenarios.tsv": [r[0] + " " + r[1] for r in rows if len(r) > 1 and r[0] in names and r[1] not in tags.get(r[0], set())]}
for what, v in problems.items():
    if v: print("scenarios-win.tsv:", what, v)
sys.exit(1 if head != ["scenario", "tag", "windows", "per_os", "notes"] or any(problems.values()) else 0)
PY' "scenarios-win.tsv: every scenario once, its tag and verdict kind known"
# WB's plan (the archived first release, the Manage tab only): every @launches WB scenario a
# script the Windows app runs (scenarios-win.tsv's "run"), none that needs a later part
ok 'python3 -I - "$root" <<"PY"
import os, sys
root = sys.argv[1]
sys.path.insert(0, os.path.join(root, "tests/perf"))
import rules
plan = rules.parse(os.path.join(root, "tests/perf/thresholds.txt"))[1].get("WB", [])
verdict = {}
for l in open(os.path.join(root, "tests/harness/scenarios-win.tsv"), encoding="utf-8"):
    c = l.rstrip("\n").split("\t")
    if not l.startswith("#") and len(c) > 2: verdict[c[0]] = c[2]
bad = [e["scenario"] for e in plan if not os.path.isfile(os.path.join(root, "tests/harness/scenarios", e["scenario"] + ".sh"))
       or verdict.get(e["scenario"]) != "run"]
if bad: print("@launches WB: not a scenario the Windows app runs:", bad)
sys.exit(1 if bad or not plan else 0)
PY' "thresholds.txt: every @launches WB scenario a script scenarios-win.tsv runs"
# the Windows Prepare fixture's stand-in facts (win/shell/vm/prepare-fixture.ps1, ProbeFacts): each
# a facts.def fact of the kind it is written with (a str fact set as a flag would mislead the core)
ok 'python3 -I - "$root" <<"PY"
import os, re, sys
root = sys.argv[1]
kinds = dict(re.findall(r"^FACT\(FACT_(\w+),\s*(flag|i64|str),", open(os.path.join(root, "core/facts.def"), encoding="utf-8").read(), re.M))
lines = re.findall(r"fact\.(\w+)=(flag|i64|str):", open(os.path.join(root, "win/shell/vm/prepare-fixture.ps1"), encoding="utf-8").read())
bad = [f"{n}={k} (facts.def: {kinds.get(n)})" for n, k in lines if kinds.get(n) != k]
if bad: print("prepare-fixture.ps1:", bad)
sys.exit(1 if bad or len(lines) < 10 else 0)
PY' "prepare-fixture.ps1: every stand-in fact a facts.def fact of its kind"

# vm.sh up: every clone runs with its template's MAC address (one rekordbox seat), never beside the
# template or another VM with that MAC. A stub tart (TART, TART_HOME): its VMs are folders with a
# config.json and a state; clone gives the copy a new MAC and run does when the MAC is in use (as
# tart does), each call logged with the MAC the VM had then; ip fails (vm.sh stops there) unless
# VS_IP=1
vs="$t/vmsh"; mkdir -p "$vs"
cat > "$vs/tart" <<'STUB'
#!/bin/bash
V="$TART_HOME/vms"; L="$TART_HOME/../tart.log"
mac() { python3 -I -c 'import json, sys; print(json.load(open(sys.argv[1]))["macAddress"])' "$V/$1/config.json"; }
setmac() { python3 -I -c 'import json, sys; p = sys.argv[1]; d = json.load(open(p)); d["macAddress"] = sys.argv[2]; json.dump(d, open(p, "w"))' "$V/$1/config.json" "$2"; }
case "$1" in
  list) echo "Source Name Disk Size Accessed State"; for d in "$V"/*/; do n="$(basename "$d")"; echo "local $n 50 30 2 days ago $(cat "$d/state")"; done ;;
  clone) cp -R "$V/$2" "$V/$3"; echo stopped > "$V/$3/state"; setmac "$3" "0e:00:00:00:00:0$(ls "$V" | wc -l | tr -d ' ')"; echo "clone $2 $3" >> "$L" ;;
  run) for d in "$V"/*/; do n="$(basename "$d")"; [ "$n" != "$2" ] && [ "$(cat "$d/state")" = running ] && [ "$(mac "$n")" = "$(mac "$2")" ] && setmac "$2" 0e:ff:ff:ff:ff:ff; done
       [ -n "${VS_REGEN:-}" ] && setmac "$2" 0e:ff:ff:ff:ff:fe
       echo running > "$V/$2/state"; echo "run $2 $(mac "$2")" >> "$L" ;;
  ip) sleep 0.3; [ -n "${VS_IP:-}" ] && echo 192.0.2.1 ;;
  stop) echo stopped > "$V/$2/state"; echo "stop $2" >> "$L" ;;
  *) echo "tart $*?" >> "$L"; exit 1 ;;
esac
STUB
chmod +x "$vs/tart"
vsvm() {   # vsvm NAME MAC STATE: a VM of the stub
  mkdir -p "$vs/home/vms/$1"; echo "$3" > "$vs/home/vms/$1/state"
  printf '{"cpuCount": 8, "ecid": "E", "macAddress": "%s", "version": 1}' "$2" > "$vs/home/vms/$1/config.json"
}
vsup() {   # vsup CLONE [VAR=VALUE…]: vm.sh up CLONE on the stub, its exit status into rc
  local c="$1"; shift
  : > "$vs/tart.log"
  env "$@" TART="$vs/tart" TART_HOME="$vs/home" VM_STATE="$vs/state" "$root/tests/harness/vm.sh" up "$c" > "$vs/up.out" 2>&1; rc=$?
}
vsmac() { python3 -I -c 'import json, sys; print(json.load(open(sys.argv[1]))["macAddress"])' "$vs/home/vms/$1/config.json"; }
T=f2:29:15:0c:4b:bc
rm -rf "$vs/home"; vsvm rb26-template "$T" stopped; vsvm dj4-template 0e:12:34:56:78:9a running
vsup c1
ok '[ "$rc" = 1 ] && [ "$(cat "$vs/tart.log")" = "clone rb26-template c1
run c1 $T" ] && [ "$(vsmac c1)" = "$T" ] && grep -q "\"cpuCount\": 8" "$vs/home/vms/c1/config.json" && [ "$(vsmac rb26-template)" = "$T" ]' "vm.sh up: the clone gets its template's MAC address after tart clone and before its start (its other keys kept; another template's VM running beside it is fine)"
echo stopped > "$vs/home/vms/c1/state"; vsvm rb26-template "$T" running
vsup c2
ok '[ "$rc" = 3 ] && [ ! -s "$vs/tart.log" ] && [ ! -d "$vs/home/vms/c2" ] && grep -q "rb26-template runs with rb26-template.s MAC address $T: one VM of rb26-template at a time, never beside it" "$vs/up.out"' "vm.sh up: the template running: exit 3, nothing cloned or started"
vsvm rb26-template "$T" stopped; echo running > "$vs/home/vms/c1/state"
vsup c2
ok '[ "$rc" = 3 ] && [ ! -s "$vs/tart.log" ] && [ ! -d "$vs/home/vms/c2" ] && grep -q "c1 runs with" "$vs/up.out"' "vm.sh up: another clone with the template's MAC running: exit 3, nothing cloned or started"
echo stopped > "$vs/home/vms/c1/state"; vsvm c3 0e:aa:aa:aa:aa:aa stopped
vsup c3
ok '[ "$rc" = 1 ] && [ "$(cat "$vs/tart.log")" = "run c3 $T" ] && [ "$(vsmac c3)" = "$T" ]' "vm.sh up: a clone already there with another MAC address gets the template's before it starts"
echo stopped > "$vs/home/vms/c3/state"
vsup c1 VS_REGEN=1 VS_IP=1
ok '[ "$rc" = 3 ] && [ "$(cat "$vs/tart.log")" = "run c1 0e:ff:ff:ff:ff:fe
stop c1" ] && grep -q "c1 stopped: it runs with MAC address 0e:ff:ff:ff:ff:fe, not only rb26-template.s $T" "$vs/up.out"' "vm.sh up: tart gave the started clone another MAC address: stopped at once, exit 3 (before SSH)"
vsup rb26-template
ok '[ "$rc" = 1 ] && [ ! -s "$vs/tart.log" ] && grep -q "is the template: vm.sh runs clones only" "$vs/up.out"' "vm.sh up: the template itself never started or changed by vm.sh"

echo "selftest: $pass passed, $fail failed"
[ "$fail" = 0 ]

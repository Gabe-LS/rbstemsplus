#!/bin/bash
# Tests scripts/release.sh's performance gate (release.md §6 step 4b, §6.2; T27, owner decided OD11)
# in the sandbox of tests/release/sandbox.sh (fake repositories, a fake gh, a throwaway key, fake
# builds; nothing real is signed or uploaded, no network): from 1.5.1 a real run needs a passing record
# build/gates/<commit>/perf-mac.tsv (tests/perf/record.py) for this commit and the TEST payload in
# dist-test/, after the gate archive's checksums. Refused: no record, another commit's, another
# TEST payload's, a FAIL (with --accept-perf too), a session's failed measurement, a result line
# its rows don't give, BORDERLINE rows without --accept-perf, a gate archive that doesn't match
# its SHA256SUMS; --accept-perf with --plan, or with no reason before the next flag; a record
# that is a symbolic link or is replaced during the build. BORDERLINE with --accept-perf REASON goes
# through, said in the output, build/gates/<commit>/perf-accepted.txt (only once the draft is
# made) and the draft's notes. After the draft, the TEST build is archived as gate-archive
# tag-vX.Y.Z/ (step 10). tests/perf/record.py alone: a table with no "# compare:" line, a later
# pass after a failure, a perf.sh run downgraded to information, a verdict word it doesn't know
# exactly, an info-only table, a row whose verdict isn't its note's, no source line; perfstats.py
# gate --out and stats.py compare --tsv never leave an earlier table behind. Then
# tests/perf/archive-release.sh alone: WB, W0's worker and WP from a Windows TEST build, a
# baseline kept (never replaced), a tag archived with another payload.json refused, a tampered
# archive refused, a file that isn't dist-test/SHA256SUMS's or payload.json's refused, a Windows
# payload with no Windows zip refused, --wp on a zip without Prepare refused. For 1.5.0,
# check both release shapes without performance records and refuse --accept-perf.
#
#   tests/release/perf-gate-test.sh [RELEASE_SH]    run outside a sandbox: a pseudo-terminal
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
rel="$(cd "$(dirname "${1:-$root/scripts/release.sh}")" && pwd)/$(basename "${1:-$root/scripts/release.sh}")"
[ -f "$rel" ] || { echo "perf-gate-test: no release.sh at $rel" >&2; exit 2; }
. "$here/sandbox.sh"

# rec VERDICT [COMMIT] [PAYLOAD] [OVERALL]: HEAD's perf-mac.tsv from a compare table with P1 a
# pass and P3 VERDICT, for COMMIT (HEAD) and PAYLOAD (dist-test/payload.json); OVERALL: the
# table's "# compare:" line (pass)
rec() {
  local c; c="$(g -C "$dev" rev-parse HEAD)"
  { echo "# compare: ${4:-pass}"
    printf 'metric\tA\tB\tchange\tallowed\tverdict\tnA\tnB\n'
    printf 'P1\t100.000\t101.000\t+1.000\t20.000\tpass\t8\t8\n'
    printf 'P3\t200.000\t218.000\t+18.000\t20.000\t%s\t8\t8\n' "$1"; } > "$t/cmp.tsv"
  python3 -I "$dev/tests/perf/record.py" write mac "${2:-$c}" "${3:-$dev/dist-test/payload.json}" "$dev/build/gates/$c/perf-mac.tsv" "$t/cmp.tsv" > /dev/null \
    || bad "rec $1: record.py write failed"
}
has() { if grep -q -- "$2" "$t/$1.log"; then ok; else bad "$1: no '$2' in its output"; tail -5 "$t/$1.log" | sed 's/^/    /'; fi; }

# Replace the live TEST payload immediately after a successful G3 check when requested.
mv "$dev/tests/mac/g3.sh" "$dev/tests/mac/g3-real.sh"
cat > "$dev/tests/mac/g3.sh" <<'G3'
#!/bin/bash
set -e
"$(dirname "$0")/g3-real.sh" "$@"
if [ "$1" = check ] && [ -n "${NEXT_TEST_PAYLOAD:-}" ]; then
  cp "$NEXT_TEST_PAYLOAD" "$(dirname "$0")/../../dist-test/payload.json"
fi
G3
chmod 755 "$dev/tests/mac/g3.sh"
g -C "$dev" add tests/mac && g -C "$dev" commit -qm 'Stub a TEST payload swap after G3' && g -C "$dev" push -q origin main
# Fake the functional VM check. A build hook marks success through the Windows gates
# so the Windows cases can exercise step 4b without assembling release assets.
mkdir -p "$dev/tests/win"
cat > "$dev/tests/win/vm-run.sh" <<'VM'
#!/bin/bash
set -euo pipefail
[ "$1" = check ]
printf 'commit %s\npayload_sha256 %s\nresult pass\n' "$2" \
  "$(shasum -a 256 "$3/payload.json" | cut -c1-64)" > "build/gates/$2/vm-win.ok"
VM
chmod 755 "$dev/tests/win/vm-run.sh"
cp "$root/tests/win/gate-record.py" "$dev/tests/win/"
g -C "$dev" add tests/win/vm-run.sh tests/win/gate-record.py
hand_record() {
  python3 -I "$dev/tests/win/gate-record.py" write-by-hand "$c" "$dev/dist-test" --pass > /dev/null \
    || bad "could not write the VR2 fixture for $c"
}

mac_waiver='Owner decision 2026-11-19: Move the Mac performance test to 1.5.1.'
win_waiver='Owner decision 2026-11-19: defer Windows speed tests until 1.5.1.'
# Check the whole waiver file for the current TEST payload.
waived() {
  local run="$1" platform="$2" pp="$3" reason="$4"
  has "$run" "$platform performance records waived for v1.5.0. $reason"
  [ ! -e "$gates/perf-$pp.tsv" ] && printf 'tag v1.5.0\ncommit %s\npayload_sha256 %s\nreason %s\n' \
    "$c" "$(shasum -a 256 "$dev/dist-test/payload.json" | cut -c1-64)" "$reason" \
    | cmp -s - "$gates/perf-$pp-waived.txt" \
    && ok || bad "$run: incorrect $platform waiver record for this tag, commit and TEST payload"
}
waiver_plan() {
  grep -q '^Next: .*gates on it (G3);$' "$t/$1.log" \
    && ! grep -Eq 'perf-(mac|win)\.tsv' "$t/$1.log" \
    && ok || bad "$1: expected Next to ask for G3 without performance records"
}

commit_version 1.5.0
g -C "$dev" push -q origin main
c="$(g -C "$dev" rev-parse HEAD)"
gates="$dev/build/gates/$c"
rs pg-150-win-plan v1.5.0 --key "$key" --windows --plan
waiver_plan pg-150-win-plan
g3_record pass '"windows": {}'
hand_record
printf 'admin %064d\n' 0 > "$dev/dist-test/windows-release.sha256"
rm -f "$gates/perf-mac.tsv" "$gates/perf-win.tsv"
FAKE_BUILD_HOOK='echo reached the build after step 4b; exit 37' rs pg-150-win v1.5.0 --key "$key" --windows
has pg-150-win 'reached the build after step 4b'
waived pg-150-win Mac mac "$mac_waiver"
waived pg-150-win Windows win "$win_waiver"
# Start the Mac-only plan with neither the aborted public commit nor its waiver files.
g -C "$t/public" reset -q --hard origin/main
rm -f "$gates/perf-mac-waived.txt" "$gates/perf-win-waived.txt"
rs pg-150-mac-plan v1.5.0 --key "$key" --plan
waiver_plan pg-150-mac-plan
g3_record
rm -f "$gates/perf-mac.tsv" "$gates/perf-win.tsv"
FAKE_BUILD_HOOK='exit 37' rs pg-150-accept v1.5.0 --key "$key" --accept-perf "a reason"
refused pg-150-accept "--accept-perf cannot be used below 1.5.1 because the performance records are waived"
rs pg-150-mac v1.5.0 --key "$key"
if [ "$rs_rc" = 0 ]; then
  released pg-150-mac v1.5.0
  publish v1.5.0
else
  bad "pg-150-mac: release exited $rs_rc"
  tail -5 "$t/pg-150-mac.log" | sed 's/^/    /'
  # Drop the aborted fake build and public commit before checking the cutoff.
  g -C "$t/public" reset -q --hard origin/main
  rm -rf "$t/public/dist" "$t/public/build"
fi
waived pg-150-mac Mac mac "$mac_waiver"
[ ! -e "$gates/perf-win-waived.txt" ] && ok || bad "pg-150-mac: unexpected Windows waiver"

commit_version 1.5.1
g -C "$dev" push -q origin main
rs pg-151-win-plan v1.5.1 --key "$key" --windows --plan
c="$(g -C "$dev" rev-parse HEAD)"
gates="$dev/build/gates/$c"
has pg-151-win-plan "records build/gates/$c/perf-mac.tsv and perf-win.tsv"
g3_record pass '"windows": {}'
hand_record
printf 'admin %064d\n' 0 > "$dev/dist-test/windows-release.sha256"
rm -f "$gates/perf-mac.tsv"
FAKE_BUILD_HOOK='exit 37' rs pg-151-win-no-mac v1.5.1 --key "$key" --windows
refused pg-151-win-no-mac "no passing performance record (mac)"
rec pass
FAKE_BUILD_HOOK='exit 37' rs pg-151-no-win v1.5.1 --key "$key" --windows
refused pg-151-no-win "no passing performance record (win)"
[ ! -e "$gates/perf-mac-waived.txt" ] && [ ! -e "$gates/perf-win-waived.txt" ] \
  && ok || bad "pg-151-no-win: unexpected waiver record"
g -C "$t/public" reset -q --hard origin/main

rs pg-plan v1.5.1 --key "$key" --plan
g3_record
c="$(g -C "$dev" rev-parse HEAD)"
gates="$dev/build/gates/$c"

# 1. no Mac record at the cutoff, on a Mac-only release
rm -f "$gates/perf-mac.tsv"
# Use a fresh clone to also catch writes from the fetch and tree preparation.
mv "$t/public" "$t/preview-clone"
g clone -q "$t/remote/Gabe-LS/rbstemsplus.git" "$t/public"
public_state > "$t/public.before"
rs pg-none v1.5.1 --key "$key"
refused pg-none "no passing performance record (mac) for $c"
public_unchanged pg-none
has pg-none "no performance record"
(rs_rc=0; fail=0; refused pg-none "no passing performance record (mac)"; [ "$fail" = 1 ]) > "$t/pg-zero-status.log" \
  && ok || bad "pg-zero-status: refused accepted a zero exit status"

# 2. another commit's record, another TEST payload's
rec pass "$(printf '0%.0s' $(seq 40))"
rs pg-commit v1.5.1 --key "$key"
refused pg-commit "no passing performance record (mac)"
has pg-commit "for commit 0000000000000000000000000000000000000000, not $c"
echo '{"another": "TEST build"}' > "$t/other-payload.json"
rec pass "$c" "$t/other-payload.json"
rs pg-payload v1.5.1 --key "$key"
refused pg-payload "no passing performance record (mac)"
has pg-payload "for the TEST payload"
cp "$dev/dist-test/payload.json" "$t/g3-payload.json"
# The perf record matches the replacement. A build-hook stop keeps a broken release from drafting.
NEXT_TEST_PAYLOAD="$t/other-payload.json" FAKE_BUILD_HOOK='exit 37' rs pg-g3-payload v1.5.1 --key "$key"
refused pg-g3-payload "no passing performance record (mac)"
has pg-g3-payload "G3 tart run: passed for $c"
cp "$t/g3-payload.json" "$dev/dist-test/payload.json"

# 3. a FAIL: a regression, with --accept-perf too; a session's failed measurement; a result line
# that isn't its rows'
rec REGRESSION
rs pg-fail v1.5.1 --key "$key"
refused pg-fail "no passing performance record (mac)"
has pg-fail "FAIL: P3"
rs pg-fail-accept v1.5.1 --key "$key" --accept-perf "it looks fine"
refused pg-fail-accept "no passing performance record (mac)"
has pg-fail-accept "a FAIL is never accepted"
rec pass "" "" "MEASUREMENT FAILED: P5: no samples on B"
rs pg-measure v1.5.1 --key "$key"
refused pg-measure "no passing performance record (mac)"
has pg-measure "a session's measurement failed"
rec REGRESSION
sed -i '' 's/^result	fail$/result	pass/' "$gates/perf-mac.tsv"
rs pg-forged v1.5.1 --key "$key"
refused pg-forged "no passing performance record (mac)"
has pg-forged "its result line says pass, its rows and sources fail"

# 4. BORDERLINE without --accept-perf; --accept-perf on a plan run
rec "pass BORDERLINE"
rs pg-border v1.5.1 --key "$key"
refused pg-border "BORDERLINE performance rows"
has pg-border "BORDERLINE P3: 200.000 -> 218.000"
has pg-border "scripts/release.sh v1.5.1 --accept-perf \"REASON\""
rs pg-accept-plan v1.5.1 --key "$key" --accept-perf "a reason" --plan
refused pg-accept-plan "--accept-perf is for the real run"
# --accept-perf with no reason: the next flag is never taken as one
for f in --dry-run --plan --windows; do
  rs "pg-accept$f" v1.5.1 --key "$key" --accept-perf "$f"
  refused "pg-accept$f" "--accept-perf takes the reason (in quotes) right after it, not '$f'"
done
# a record that is a symbolic link; one replaced during the build (after step 4b checked it):
# refused, and no perf-accepted.txt is left by a run that made no draft
mv "$gates/perf-mac.tsv" "$t/perf-mac.real"; ln -s "$t/perf-mac.real" "$gates/perf-mac.tsv"
rs pg-symlink v1.5.1 --key "$key"
refused pg-symlink "perf-mac.tsv is a symbolic link"
rm "$gates/perf-mac.tsv"
rec pass; cp "$gates/perf-mac.tsv" "$t/perf-pass.tsv"
rec "pass BORDERLINE"
# The check-then-copy window itself can't be tested with this build hook.
FAKE_BUILD_HOOK="cp '$t/perf-pass.tsv' '$gates/perf-mac.tsv'" rs pg-swapped v1.5.1 --key "$key" --accept-perf "noise"
refused pg-swapped "perf-mac.tsv changed during the build (it isn't the record step 4b checked)"
[ ! -e "$gates/perf-accepted.txt" ] && ok || bad "pg-swapped: perf-accepted.txt written before the draft"

# 5. the gate archive's checksums, before any comparison: a file changed, then SHA256SUMS gone
rec pass
cp "$GATES_ARCHIVE/mac-A/toolchain.txt" "$t/toolchain.saved"
echo "changed" >> "$GATES_ARCHIVE/mac-A/toolchain.txt"
rs pg-sums v1.5.1 --key "$key"
refused pg-sums "the gate archive ($GATES_ARCHIVE) doesn't match its SHA256SUMS"
cp "$t/toolchain.saved" "$GATES_ARCHIVE/mac-A/toolchain.txt"
mv "$GATES_ARCHIVE/SHA256SUMS" "$t/sums.saved"
rs pg-nosums v1.5.1 --key "$key"
refused pg-nosums "doesn't match its SHA256SUMS, or has none"
mv "$t/sums.saved" "$GATES_ARCHIVE/SHA256SUMS"
cp "$GATES_ARCHIVE/SHA256SUMS" "$t/sums.saved"
echo 'malformed checksum line' >> "$GATES_ARCHIVE/SHA256SUMS"
(A="$GATES_ARCHIVE"; . "$root/tests/perf/archive.sh"; ! verify) > "$t/pg-strict.log" 2>&1 \
  && ok || bad "pg-strict: a malformed manifest passed verification"
mv "$t/sums.saved" "$GATES_ARCHIVE/SHA256SUMS"
mkdir -p "$GATES_ARCHIVE/run.partial" "$GATES_ARCHIVE/new folder"
touch "$GATES_ARCHIVE/mac-A/unlisted\\file name" "$GATES_ARCHIVE/.DS_Store" \
  "$GATES_ARCHIVE/run.partial/file" "$GATES_ARCHIVE/new folder/file"
(A="$GATES_ARCHIVE"; . "$root/tests/perf/archive.sh"; seal 'new folder'; verify) > "$t/pg-warn.log" 2>&1 \
  && grep -qxF 'seal: warning: mac-A/unlisted\file name was missing from the old SHA256SUMS' "$t/pg-warn.log" \
  && [ "$(grep -c 'seal: warning:' "$t/pg-warn.log")" = 1 ] \
  && ok || bad "pg-warn: seal did not warn only about the unlisted file"
rm -r "$GATES_ARCHIVE/run.partial" "$GATES_ARCHIVE/new folder"
rm "$GATES_ARCHIVE/mac-A/unlisted\\file name" "$GATES_ARCHIVE/.DS_Store"
reseal
[ ! -e "$GATES_ARCHIVE/tag-v1.5.1" ] && ok || bad "a refused run archived tag-v1.5.1"

# 6. BORDERLINE with --accept-perf: released, said in the output, the log and the draft's notes;
# the TEST build archived as tag-v1.5.1 with its test key pair, the archive sealed
rec "pass BORDERLINE"
rs pg-accept v1.5.1 --key "$key" --accept-perf "P3 is within noise on this host"
released pg-accept v1.5.1
has pg-accept "BORDERLINE accepted (--accept-perf): P3 (mac). Reason: P3 is within noise on this host"
grep -qx "reason P3 is within noise on this host" "$gates/perf-accepted.txt" && grep -qx "commit $c" "$gates/perf-accepted.txt" \
  && ok || bad "pg-accept: perf-accepted.txt doesn't hold the reason and the commit"
grep -q "Performance: BORDERLINE rows accepted .*P3 (mac). Reason: P3 is within noise on this host" "$FAKE_GH/notes" \
  && ok || bad "pg-accept: the draft's notes don't say the BORDERLINE accepted"
a="$GATES_ARCHIVE/tag-v1.5.1"
cmp -s "$a/payload.json" "$dev/dist-test/payload.json" && cmp -s "$a/test-keys/test.pub.pem" "$tkey/test.pub.pem" \
  && [ -f "$a/payload.json.sig" ] && [ -f "$a/rbstemsplus-app.zip" ] && [ -f "$a/toolchain.txt" ] \
  && ok || bad "pg-accept: tag-v1.5.1 isn't the TEST build with its test key pair"
(cd "$GATES_ARCHIVE" && shasum -a 256 -c --quiet SHA256SUMS) && grep -q " tag-v1.5.1/payload.json$" "$GATES_ARCHIVE/SHA256SUMS" \
  && ok || bad "pg-accept: the archive isn't sealed over tag-v1.5.1"
has pg-accept "archived tag-v1.5.1 in"
tag151="$(shasum -a 256 "$a/payload.json" | cut -c1-64)"
publish v1.5.1

# 7. the next release, a pass with --accept-perf: nothing accepted, nothing in the notes; archived
commit_version 1.5.2
g -C "$dev" push -q origin main 2> /dev/null
rs pg-152-plan v1.5.2 --key "$key" --plan
g3_record
rs pg-152 v1.5.2 --key "$key" --accept-perf "not needed"
released pg-152 v1.5.2
has pg-152 "no row is BORDERLINE: nothing accepted"
! grep -q "Performance:" "$FAKE_GH/notes" && ok || bad "pg-152: the notes speak of a BORDERLINE"
[ -d "$GATES_ARCHIVE/tag-v1.5.2" ] && [ "$(shasum -a 256 "$a/payload.json" | cut -c1-64)" = "$tag151" ] \
  && ok || bad "pg-152: tag-v1.5.2 not archived, or tag-v1.5.1 changed"

# record.py alone (what release.sh's step 4b reads): each refused by write or by check
rp() { python3 -I "$dev/tests/perf/record.py" "$@" > "$t/rp.log" 2>&1; }
c152="$(g -C "$dev" rev-parse HEAD)"; ps152="$(shasum -a 256 "$dev/dist-test/payload.json" | cut -c1-64)"
hdr='metric\tA\tB\tchange\tallowed\tverdict\tnA\tnB\n'
tbl() { local n="$1"; shift; printf "$@" > "$t/$n.tsv"; }
# rw NAME: write a record from NAME.tsv, then check it; rc of check (write's failure: 99)
rw() { rp write mac "$c152" "$dev/dist-test/payload.json" "$t/rec-$1.tsv" "$t/$1.tsv" || return 99; rp check "$t/rec-$1.tsv" mac "$c152" "$ps152"; }
tbl nosum "${hdr}P1\t1\t1\t+0\t1\tpass\t8\t8\n"
rw nosum; [ $? = 99 ] && grep -q "no '# compare:' summary line: not a gate session's table" "$t/rp.log" && ok || bad "record.py: a table with no summary line not refused"
tbl later "# compare: MEASUREMENT FAILED: P5: no samples\n${hdr}P1\t1\t1\t+0\t1\tpass\t8\t8\n# compare: pass\n"
rw later; [ $? = 1 ] && grep -q "a session's measurement failed" "$t/rp.log" && ok || bad "record.py: a later pass overrode a failure"
# a stub run of perf.sh: perfstats.py gate's table, downgraded to information (no gate)
tbl downgraded "# compare: pass\nmetric\tA\tB\tchange\tallowed\tresult\nP-H1.run\t1 ms\t1 ms\t0 ms\t20 ms\tpass\n# compare: information only (stubs: not a valid gate)\n"
rw downgraded; [ $? = 1 ] && ok || bad "record.py: a downgraded gate.tsv not refused"
tbl passenger "# compare: pass\n${hdr}P1\t1\t1\t+0\t1\tpassenger\t8\t8\n"
rw passenger; [ $? = 1 ] && grep -q "FAIL: P1" "$t/rp.log" && ok || bad "record.py: 'passenger' counted as a pass"
tbl infoonly "# compare: pass\n${hdr}P13\t1\t1\t+0\t-\tinfo\t8\t8\n"
rw infoonly; [ $? = 99 ] && grep -q "no gating row" "$t/rp.log" && ok || bad "record.py write: an info-only table not refused"
tbl good "# compare: pass\n${hdr}P1\t1\t1\t+0\t1\tpass\t8\t8\n"
rw good && ok || bad "record.py: a passing table refused"
sed 's/^\(row	P1	.*	\)pass	pass$/\1pass	REGRESSION/' "$t/rec-good.tsv" > "$t/rec-note.tsv"
rp check "$t/rec-note.tsv" mac "$c152" "$ps152"; [ $? = 1 ] && grep -q "its note 'REGRESSION' says fail" "$t/rp.log" && ok || bad "record.py check: a row whose note says otherwise not refused"
grep -v '^source' "$t/rec-good.tsv" > "$t/rec-nosrc.tsv"
rp check "$t/rec-nosrc.tsv" mac "$c152" "$ps152"; [ $? = 1 ] && grep -q "no source line" "$t/rp.log" && ok || bad "record.py check: a record with no source line not refused"
# perfstats.py gate --out and stats.py compare --tsv: an earlier table never survives a run that stops
printf 'P-H1.run\tA\t1\t1000000\nP-H1.run\tB\t1\t1000000\n' > "$t/samples.tsv"; : > "$t/no-rules.txt"
cp "$t/good.tsv" "$t/gate.tsv"
python3 -I "$root/tests/perf/perfstats.py" gate "$t/samples.tsv" --a A --b B --rules "$t/no-rules.txt" --out "$t/gate.tsv" > /dev/null 2>&1
[ $? = 3 ] && [ "$(head -c 31 "$t/gate.tsv")" = "# compare: MEASUREMENT FAILED: " ] && [ "$(wc -l < "$t/gate.tsv")" -eq 1 ] \
  && ok || bad "perfstats.py gate: the rules' failure left another table, or no summary line"
cp "$t/good.tsv" "$t/gate.tsv"
python3 -I "$root/tests/perf/perfstats.py" gate "$t/samples.tsv" --a A --b B --out "$t/gate.tsv" > /dev/null 2>&1
[ $? = 2 ] && [ ! -e "$t/gate.tsv" ] && ok || bad "perfstats.py gate: missing --rules left an earlier table"
cp "$t/good.tsv" "$t/gate.tsv"
python3 -I "$root/tests/perf/perfstats.py" gate "$t/samples.tsv" --a A --b B --rules "$root/tests/perf/thresholds.txt" --out "$t/gate.tsv" --typo x > /dev/null 2>&1
[ $? = 2 ] && [ ! -e "$t/gate.tsv" ] && ok || bad "perfstats.py gate: --typo left an earlier table or didn't exit 2"
printf '\xff' > "$t/bad-samples.tsv"
for samples in missing-samples.tsv bad-samples.tsv; do
  cp "$t/good.tsv" "$t/gate.tsv"
  python3 -I "$root/tests/perf/perfstats.py" gate "$t/$samples" --a A --b B --rules "$root/tests/perf/thresholds.txt" --out "$t/gate.tsv" > /dev/null 2>&1
  [ $? = 3 ] && [ "$(head -c 31 "$t/gate.tsv")" = "# compare: MEASUREMENT FAILED: " ] && [ "$(wc -l < "$t/gate.tsv")" -eq 1 ] \
    && ok || bad "perfstats.py gate: $samples didn't leave one MEASUREMENT FAILED line (3)"
done
cp "$t/good.tsv" "$t/gate.tsv"
printf 'P-H1.run\tB\t1\tnan\n' >> "$t/samples.tsv"
printf '%s\n' "# compare: MEASUREMENT FAILED: $t/samples.tsv: a non-finite sample: 'P-H1.run\tB\t1\tnan'" > "$t/nan.tsv"
python3 -I "$root/tests/perf/perfstats.py" gate "$t/samples.tsv" --a A --b B --rules "$root/tests/perf/thresholds.txt" --out "$t/gate.tsv" > /dev/null 2>&1
[ $? = 3 ] && cmp -s "$t/nan.tsv" "$t/gate.tsv" && ok || bad "perfstats.py gate: nan didn't leave one MEASUREMENT FAILED line (3)"
cp "$t/good.tsv" "$t/cmp-old.tsv"; mkdir -p "$t/empty-session"
python3 -I "$root/tests/perf/stats.py" compare "$t/empty-session" --tsv "$t/cmp-old.tsv" > /dev/null 2>&1
! cmp -s "$t/good.tsv" "$t/cmp-old.tsv" && ok || bad "stats.py compare: the earlier table left in place"

# 8. tests/perf/archive-release.sh alone: a Windows TEST build with Prepare (WB, W0's worker, WP)
d="$dev/dist-test"
# winbuild VERSION [EXTRA_JSON] [noprep|app|bridge]: a Windows TEST build of VERSION in dist-test/
# (its zip with the worker, and Prepare unless noprep), payload.json's windows block naming the zip
# and the bridge (app or bridge: that pin is wrong), SHA256SUMS, signed
winbuild() {
  local f app bridge
  for f in rbstemsplus-bridge-win-x64.dll bootstrap.ps1 NOTICE-windows.txt; do echo "test $f" > "$d/$f"; done
  python3 -I -c 'import sys, zipfile
with zipfile.ZipFile(sys.argv[1], "w") as z:
    z.writestr(zipfile.ZipInfo("admin/RB Stems Plus.exe"), b"the worker")
    if sys.argv[2] != "noprep":
        z.writestr(zipfile.ZipInfo("app/rbsp-prepare.exe"), b"the helper")' "$d/rbstemsplus-windows.zip" "${3:-}"
  app="$(shasum -a 256 "$d/rbstemsplus-windows.zip" | cut -c1-64)"
  bridge="$(shasum -a 256 "$d/rbstemsplus-bridge-win-x64.dll" | cut -c1-64)"
  case "${3:-}" in
    app) app="$(printf '%064d' 0)" ;;
    bridge) bridge="$(printf '%064d' 0)" ;;
  esac
  test_build_files "\"payload_version\": \"$1\"${2:+, $2}, \"windows\": {\"app\": {\"sha256\": \"$app\"}, \"bridge\": {\"sha256\": \"$bridge\"}}"
  test_signed
}
ar() { local n="$1"; shift; (cd "$dev" && tests/perf/archive-release.sh "$@") > "$t/$n.log" 2>&1; }
winbuild 1.3.9 "" noprep
ar ar-auto-noprep v1.3.9 --wp-if-present && [ -d "$GATES_ARCHIVE/tag-v1.3.9" ] && [ ! -e "$GATES_ARCHIVE/WP" ] \
  && ok || bad "ar-auto-noprep: archiving without Prepare failed or made WP"
winbuild 1.4.0
ar ar-win v1.4.0 --wb --wp --payload-sha "$(shasum -a 256 "$d/payload.json" | cut -c1-64)" && ok || { bad "ar-win: failed"; sed 's/^/    /' "$t/ar-win.log"; }
for n in tag-v1.4.0 WB WP; do
  cmp -s "$GATES_ARCHIVE/$n/rbstemsplus-windows.zip" "$d/rbstemsplus-windows.zip" && cmp -s "$GATES_ARCHIVE/$n/payload.json" "$d/payload.json" \
    && ok || bad "ar-win: $n doesn't hold the Windows TEST build"
done
arch_tag="$GATES_ARCHIVE/tag-v1.4.0"
grep -qxF "payload_sha256=$(shasum -a 256 "$arch_tag/payload.json" | cut -c1-64)" "$arch_tag/toolchain.txt" \
  && ok || bad "ar-win: toolchain.txt doesn't name the staged payload's SHA-256"
[ "$(cat "$GATES_ARCHIVE/W0/worker/RB Stems Plus.exe" 2>/dev/null)" = "the worker" ] && ok || bad "ar-win: W0/worker isn't the zip's worker"
(cd "$GATES_ARCHIVE" && shasum -a 256 -c --quiet SHA256SUMS) && grep -q " WP/payload.json$" "$GATES_ARCHIVE/SHA256SUMS" \
  && grep -q " W0/worker/RB Stems Plus.exe$" "$GATES_ARCHIVE/SHA256SUMS" && ok || bad "ar-win: the archive isn't sealed over WB, WP and W0"
# again: every folder kept; then another TEST build of 1.4.0: the baselines kept, the tag refused
touch "$GATES_ARCHIVE/WB/sealed\\file name"
(A="$GATES_ARCHIVE"; . "$root/tests/perf/archive.sh"; seal WB) || bad "ar-again: could not seal the added test fixture"
ar ar-again v1.4.0 --wb --wp && grep -q "tag-v1.4.0 is in the gate archive already, with this payload.json: kept" "$t/ar-again.log" \
  && grep -q "WB is in the gate archive already: kept" "$t/ar-again.log" && ok || bad "ar-again: not kept"
cp "$GATES_ARCHIVE/WB/payload.json" "$t/wb.saved"
winbuild 1.4.0 '"another": 1'
! ar ar-other v1.4.0 --wb && grep -q "tag-v1.4.0 is there with another payload.json: never replaced" "$t/ar-other.log" \
  && cmp -s "$GATES_ARCHIVE/WB/payload.json" "$t/wb.saved" && ok || bad "ar-other: not refused, or WB replaced"
# the payload release.sh's gates read; a version that isn't the tag's; --wb without Windows files
! ar ar-ps v1.4.1 --payload-sha "$(printf '1%.0s' $(seq 64))" && grep -q "isn't the TEST build the gates read" "$t/ar-ps.log" && ok || bad "ar-ps: not refused"
! ar ar-ver v1.4.1 && grep -q "dist-test/payload.json is version 1.4.0, not 1.4.1" "$t/ar-ver.log" && ok || bad "ar-ver: not refused"
# a Windows payload with no Windows zip: refused, never archived as a Mac-only build
winbuild 1.4.1; rm -f "$d/rbstemsplus-windows.zip"
! ar ar-nowinzip v1.4.1 && grep -q "dist-test/rbstemsplus-windows.zip is missing (the TEST build has Windows files)" "$t/ar-nowinzip.log" \
  && [ ! -e "$GATES_ARCHIVE/tag-v1.4.1" ] && ok || bad "ar-nowinzip: not refused"
test_build_files '"payload_version": "1.4.1"'; test_signed
! ar ar-nowin v1.4.1 --wb && grep -q "dist-test/ has none" "$t/ar-nowin.log" && [ ! -e "$GATES_ARCHIVE/tag-v1.4.1" ] && ok || bad "ar-nowin: not refused"
# a payload.json no archived key signed; a tampered archive
echo "not a signature" > "$d/payload.json.sig"
! ar ar-nokey v1.4.1 && grep -q "no test key pair here" "$t/ar-nokey.log" && ok || bad "ar-nokey: not refused"
test_signed
cp "$GATES_ARCHIVE/WP/toolchain.txt" "$t/wp-toolchain.saved"
echo "changed" >> "$GATES_ARCHIVE/WP/toolchain.txt"
! ar ar-tampered v1.4.1 && grep -q "doesn't match its SHA256SUMS: nothing archived" "$t/ar-tampered.log" && [ ! -e "$GATES_ARCHIVE/tag-v1.4.1" ] \
  && ok || bad "ar-tampered: not refused"
cp "$t/wp-toolchain.saved" "$GATES_ARCHIVE/WP/toolchain.txt"
# a file replaced after the build: not SHA256SUMS's; with SHA256SUMS made anew, not payload.json's
# (nothing archived, nothing staged left)
cp "$d/rbstemsplus-app.zip" "$t/app.saved"; echo "another app" > "$d/rbstemsplus-app.zip"
! ar ar-zip v1.4.1 && grep -q "tag-v1.4.1/rbstemsplus-app.zip isn't the one dist-test/SHA256SUMS lists" "$t/ar-zip.log" \
  && [ ! -e "$GATES_ARCHIVE/tag-v1.4.1" ] && [ ! -e "$GATES_ARCHIVE/tag-v1.4.1.partial" ] && ok || bad "ar-zip: not refused"
(cd "$d" && shasum -a 256 rbstemsplus-app.zip libonnxruntime.1.18.0.dylib bootstrap.sh payload.json > SHA256SUMS)
! ar ar-pin v1.4.1 && grep -q "tag-v1.4.1/rbstemsplus-app.zip isn't the file payload.json names (app.sha256" "$t/ar-pin.log" \
  && [ ! -e "$GATES_ARCHIVE/tag-v1.4.1.partial" ] && ok || bad "ar-pin: not refused"
cp "$t/app.saved" "$d/rbstemsplus-app.zip"
# each Windows file matches SHA256SUMS, but its pin in the signed payload is wrong
cp "$GATES_ARCHIVE/SHA256SUMS" "$t/archive-sums.saved"
for pin in app bridge; do
  winbuild 1.4.1 "" "$pin"
  ! ar "ar-win-$pin" v1.4.1 && grep -q "isn't the file payload.json names (windows.$pin.sha256" "$t/ar-win-$pin.log" \
    && [ ! -e "$GATES_ARCHIVE/tag-v1.4.1" ] && [ ! -e "$GATES_ARCHIVE/tag-v1.4.1.partial" ] \
    && cmp -s "$GATES_ARCHIVE/SHA256SUMS" "$t/archive-sums.saved" && ok || bad "ar-win-$pin: wrong Windows pin wasn't refused cleanly"
done
# --wp on a Windows zip without Prepare: refused (WP would be a baseline without it)
winbuild 1.4.2 "" noprep
! ar ar-noprep v1.4.2 --wp && grep -q "has no rbsp-prepare.exe" "$t/ar-noprep.log" && [ ! -e "$GATES_ARCHIVE/tag-v1.4.2" ] \
  && [ ! -e "$GATES_ARCHIVE/tag-v1.4.2.partial" ] && ok || bad "ar-noprep: not refused"

# A seal fails after the tag has moved. A retry may not seal additions to that folder.
cat > "$t/bin/mv" <<'MV'
#!/bin/bash
if [ "${FAIL_ARCHIVE_SEAL:-0}" = 1 ] && [ "${@: -1}" = "$GATES_ARCHIVE/SHA256SUMS" ]; then exit 1; fi
if [ -n "${ARCHIVE_RACE_TAG:-}" ]; then
  case "${@: -2:1}" in
    */"$ARCHIVE_RACE_TAG"|*/"$ARCHIVE_RACE_TAG.partial") mkdir "$GATES_ARCHIVE/$ARCHIVE_RACE_TAG" ;;
  esac
fi
exec /bin/mv "$@"
MV
chmod 755 "$t/bin/mv"
winbuild 1.4.3
! FAIL_ARCHIVE_SEAL=1 ar ar-unsealed v1.4.3 && grep -q "holds tag-v1.4.3 unsealed" "$t/ar-unsealed.log" \
  && [ -d "$GATES_ARCHIVE/tag-v1.4.3" ] && ok || bad "ar-unsealed: failed seal did not leave the moved tag"
touch "$GATES_ARCHIVE/tag-v1.4.3/added file"
! ar ar-unsealed-retry v1.4.3 && grep -qF 'tag-v1.4.3/added file' "$t/ar-unsealed-retry.log" \
  && cmp -s "$GATES_ARCHIVE/SHA256SUMS" "$t/archive-sums.saved" && ok || bad "ar-unsealed-retry: accepted an unsealed tag"
rm -r "$GATES_ARCHIVE/tag-v1.4.3"
cp "$t/archive-sums.saved" "$GATES_ARCHIVE/SHA256SUMS"
winbuild 1.4.0
for folder in WB WP W0/worker; do
  touch "$GATES_ARCHIVE/$folder/added file"
  ! ar ar-unlisted v1.4.0 --wb --wp && grep -qF "$folder/added file" "$t/ar-unlisted.log" \
    && cmp -s "$GATES_ARCHIVE/SHA256SUMS" "$t/archive-sums.saved" && ok || bad "ar-unlisted: accepted an unsealed file in $folder"
  rm "$GATES_ARCHIVE/$folder/added file"
  cp "$t/archive-sums.saved" "$GATES_ARCHIVE/SHA256SUMS"
done

# Another run's lock and staging files are left alone, and a destination that appears
# during the move must be left empty instead of receiving the staged directory.
winbuild 1.4.4
mkdir "$GATES_ARCHIVE/.archive-release.lock"
! ar ar-locked v1.4.4 && grep -q 'archive is locked' "$t/ar-locked.log" \
  && [ -d "$GATES_ARCHIVE/.archive-release.lock" ] && [ ! -e "$GATES_ARCHIVE/tag-v1.4.4" ] \
  && ok || bad "ar-locked: the lock did not stop archiving"
rm -rf "$GATES_ARCHIVE/tag-v1.4.4"
rmdir "$GATES_ARCHIVE/.archive-release.lock"
cp "$t/archive-sums.saved" "$GATES_ARCHIVE/SHA256SUMS"
mkdir "$GATES_ARCHIVE/tag-v1.4.4.partial"
touch "$GATES_ARCHIVE/tag-v1.4.4.partial/earlier-run"
! ARCHIVE_RACE_TAG=tag-v1.4.4 ar ar-move-race v1.4.4 && grep -q 'could not move' "$t/ar-move-race.log" \
  && [ -d "$GATES_ARCHIVE/tag-v1.4.4" ] && [ -z "$(ls -A "$GATES_ARCHIVE/tag-v1.4.4")" ] \
  && [ ! -e "$GATES_ARCHIVE/.archive-release.lock" ] && cmp -s "$GATES_ARCHIVE/SHA256SUMS" "$t/archive-sums.saved" \
  && ok || bad "ar-move-race: an existing destination did not stop the move cleanly"
[ -f "$GATES_ARCHIVE/tag-v1.4.4.partial/earlier-run" ] && ok || bad "ar-move-race: removed another run's staging folder"

# The archive changes after step 4b verified it. Step 10 fails with the draft already made.
publish v1.5.2
mkdir -p "$dev/docs/release-notes"
printf 'Release notes for the\npatch release.\n' > "$dev/docs/release-notes/1.5.3.md"
g -C "$dev" add docs/release-notes/1.5.3.md
commit_version 1.5.3
g -C "$dev" push -q origin main
rs pg-153-plan v1.5.3 --key "$key" --plan
g3_record
rec "pass BORDERLINE"
archive_rc=0
FAKE_BUILD_HOOK="echo changed >> '$GATES_ARCHIVE/mac-A/toolchain.txt'" \
  rs pg-archive-failed v1.5.3 --key "$key" --accept-perf "P3 has known host noise" || archive_rc=$?
has pg-archive-failed "the draft v1.5.3 is made, but the TEST build wasn't archived"
[ "$archive_rc" != 0 ] && [ "$archive_rc" = "$rs_rc" ] && [ -d "$FAKE_GH/releases/v1.5.3" ] \
  && [ ! -e "$GATES_ARCHIVE/tag-v1.5.3" ] && ok || bad "pg-archive-failed: expected a failing status and a draft without an archived tag"
(fail=0; refused pg-archive-failed "the draft v1.5.3 is made"; [ "$fail" = 1 ]) > "$t/pg-draft-status.log" \
  && ok || bad "pg-archive-failed: refused accepted a run that made a draft"
grep -q '^gh release create v1.5.3 .* --notes-file ' "$FAKE_GH/calls" \
  && grep -qx 'Release notes for the patch release\.' "$FAKE_GH/notes" \
  && grep -q 'Performance: BORDERLINE rows accepted .*P3 (mac). Reason: P3 has known host noise' "$FAKE_GH/notes" \
  && ok || bad "pg-notes-file: the notes file didn't retain its text and append the BORDERLINE acceptance"

cp "$t/toolchain.saved" "$GATES_ARCHIVE/mac-A/toolchain.txt"

echo "perf-gate-test: $pass passed, $fail failed"
[ "$fail" = 0 ]

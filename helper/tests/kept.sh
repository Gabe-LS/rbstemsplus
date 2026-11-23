#!/bin/bash
# The helper's kept sets and room, in a scratch folder (nothing else is touched):
# writing a set, preparing it again, refusals, sets whose date has passed, the 75% share,
# making room, and the estimate with the sets' manifests. Fake entries (zeros named like
# entries) fill the cache where a test needs it.
#
#   helper/tests/kept.sh [RBSP_PREPARE]     default build/helper/rbsp-prepare
#   helper/tests/kept.sh --windows [RBSP_PREPARE]   default build/helper-win/rbsp-prepare.exe
# Windows (Git Bash) needs RBSP_MODEL, RBSP_ORT, RBSP_REKORDBOX, RBSP_OTHER_DRIVE (e.g. E:),
# RBSP_UNC (e.g. //server/share), RBSP_KEPT_CHECK (rbsp-kept-check.exe), RBSP_KEYS_HELPER
# (the test build with --keys), and ffmpeg. RBSP_DECODE_DUMP defaults to the exe next to the helper.
#
# Needs our Demucs v4 model (RBSP_MODEL, default dist-test/stemsplus-model.onnx) and Pioneer's
# ONNX Runtime where the app keeps it (or RBSP_ORT); python3 (stdlib only) writes the music.
set -u
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
windows=0
if [ "${1:-}" = --windows ]; then windows=1; shift; fi
date_cmd=(date)
sed_in_place=(sed -i '')
dd_block=1m
manifest_rate=
py="${PY:-python3}"
sha=("${SHA:-shasum}" -a 256)
default_bin="$root/build/helper/rbsp-prepare"
other=/Volumes/nowhere
if [ "$windows" = 1 ]; then
  py="${PY:-python}"
  sha=("${SHA:-sha256sum}")
  sed_in_place=(sed -i)
  dd_block=1M
  manifest_rate='hz=44100 '
  default_bin="$root/build/helper-win/rbsp-prepare.exe"
  : "${RBSP_MODEL:?set RBSP_MODEL}" "${RBSP_ORT:?set RBSP_ORT}" "${RBSP_REKORDBOX:?set RBSP_REKORDBOX}"
  : "${RBSP_OTHER_DRIVE:?set RBSP_OTHER_DRIVE}" "${RBSP_UNC:?set RBSP_UNC}" "${RBSP_KEPT_CHECK:?set RBSP_KEPT_CHECK}"
  : "${RBSP_KEYS_HELPER:?set RBSP_KEYS_HELPER}"
  other="${RBSP_OTHER_DRIVE%/}/rbsp-kept-missing"
  sysdrive="${SYSTEMDRIVE:-${SystemDrive:-C:}}"
  [[ "$RBSP_OTHER_DRIVE" =~ ^[a-zA-Z]:/?$ &&
     "$(tr 'a-z' 'A-Z' <<< "${RBSP_OTHER_DRIVE:0:1}")" != "$(tr 'a-z' 'A-Z' <<< "${sysdrive:0:1}")" &&
     "$RBSP_UNC" =~ ^//[^/]+/[^/]+/?$ ]] || {
    echo 'set RBSP_OTHER_DRIVE to a second drive letter and RBSP_UNC to //server/share' >&2
    exit 1
  }
fi
bin="${1:-$default_bin}"
model="${RBSP_MODEL:-$root/dist-test/stemsplus-model.onnx}"
ort="${RBSP_ORT:-/Library/Application Support/rbstemsplus/ort/libonnxruntime.1.18.0.dylib}"
for f in "$bin" "$model" "$ort"; do [ -f "$f" ] || { echo "missing: $f"; exit 1; }; done
bin="$(cd "$(dirname "$bin")" && pwd -P)/$(basename "$bin")"   # absolute: some cases run from another folder
id="$("${sha[@]}" "$model" | cut -d' ' -f1)"
s="$(cd "$(mktemp -d "${TMPDIR:-/tmp}/rbsp-kept.XXXXXX")" && pwd -P)"
trap '[ "${KEEP:-}" = 1 ] && echo "scratch: $s" || rm -rf "$s"' EXIT
if [ "$windows" = 1 ]; then
  export MSYS2_ARG_CONV_EXCL='*'
  RBSP_DECODE_DUMP="${RBSP_DECODE_DUMP:-$(dirname "$bin")/rbsp-decode-dump.exe}"
  for var in bin model ort s RBSP_REKORDBOX RBSP_KEPT_CHECK RBSP_KEYS_HELPER RBSP_DECODE_DUMP; do
    native="$(cygpath -am "${!var}")" || exit 1
    printf -v "$var" '%s' "$native"
  done
  runner="$(cygpath -am "$here/kept-run.py")"
  export RBSP_TEST_RUN_ERRORS="$s/runner.errors"
  for f in "$RBSP_KEPT_CHECK" "$RBSP_KEYS_HELPER" "$RBSP_DECODE_DUMP"; do
    [ -f "$f" ] || { echo "missing: $f"; exit 1; }
  done
fi
passed=0 failures=0
expect() { if eval "$1"; then passed=$((passed + 1)); else failures=$((failures + 1)); echo "  FAIL $2"; fi; }
py() { "$py" -I "$@"; }
invoke() {
  if [ "$windows" = 1 ]; then
    py "$runner" -1 "$bin" --rekordbox "$RBSP_REKORDBOX" "$@"
  else
    "$bin" "$@"
  fi
}

# 30 s of music-like stereo noise (3 chunks), the same every run
py - "$s/a.wav" << 'EOF'
import math, random, struct, sys, wave
random.seed(7)
w = wave.open(sys.argv[1], "wb"); w.setnchannels(2); w.setsampwidth(2); w.setframerate(44100)
fr = bytearray()
for i in range(44100 * 30):
    t = i / 44100
    v = 0.3 * math.sin(2 * math.pi * 110 * t) * (1 + math.sin(2 * math.pi * 2 * t)) / 2 + 0.05 * (random.random() - 0.5)
    fr += struct.pack("<hh", int(v * 30000), int(v * 26000))
w.writeframes(bytes(fr)); w.close()
EOF

now="$("${date_cmd[@]}" +%s)"
c= kd= out=
start() { c="$s/$1/cache" kd="$s/$1/kept"; mkdir -p "$c"; }
run() { invoke --model "$model" --ort "$ort" --cache "$c" --kept-dir "$kd" "$@" 2> "$s/err"; }
last() { tail -1 <<< "$out"; }
fileline() { grep -m1 "\"file\"" <<< "$out"; }
field() { py -c 'import json,sys; d=json.loads(sys.argv[1]); sys.stdout.write(str(d.get(sys.argv[2], "")))' "$1" "$2"; }
totals() { py -c 'import json,sys; rows=list(map(json.loads,sys.stdin)); d=rows[-1]; assert d["estimate"] == sum("file" in r for r in rows) == sum(d[k] for k in ("ready","prepare","skipped","unchecked"))' <<< "$out"; }
lines() { tail -n +3 "$kd/$1.kept" | wc -l | tr -d ' '; }
key() { printf '%064x' "$1"; }
fake() { local k; k="$(key "$1")"; mkdir -p "$c/$id/${k:0:2}"; dd if=/dev/zero of="$c/$id/${k:0:2}/$k.flac" bs="$dd_block" count="$2" 2> /dev/null; touch -t "$3" "$c/$id/${k:0:2}/$k.flac"; }
there() { local k; k="$(key "$1")"; [ -f "$c/$id/${k:0:2}/$k.flac" ]; }
set_() { local f="$kd/$1" u="$2"; shift 2; mkdir -p "$kd"; { printf 'rbstemsplus-kept 1\nuntil %s\n' "$u"; for i in "$@"; do printf '%s %s\n' "$id" "$(key "$i")"; done; } > "$f"; }
stamp() {
  if [ "$windows" = 1 ]; then "${date_cmd[@]}" -d "$1 hours ago" +%Y%m%d%H%M
  else "${date_cmd[@]}" -v-"$1"H +%Y%m%d%H%M; fi
}

echo "== writing a set"
start write
out="$(run --keep friday "$s/a.wav")"
w="$(field "$(fileline)" written)" a="$(field "$(fileline)" already)"
expect '[ -f "$kd/friday.kept" ] && [ "$(head -1 "$kd/friday.kept")" = "rbstemsplus-kept 1" ] && [ "$w" -gt 0 ] && [ "$(lines friday)" = "$((w + a))" ]' \
  "a set lists every entry written: $w written, $(lines friday 2> /dev/null) lines"
u="$(sed -n 2p "$kd/friday.kept" | cut -d' ' -f2)"
expect '[ $((u - now - 864000)) -ge 0 ] && [ $((u - now - 864000)) -lt 120 ]' "kept for 10 days by default: until $u, now $now"
expect '[ "$(field "$(last)" kept)" = friday ] && [ "$(field "$(last)" until)" = "$u" ]' "the last line names the set and its date: $(last)"
missing=0
while read -r mm kk; do [ -f "$c/$mm/${kk:0:2}/$kk.flac" ] || missing=$((missing + 1)); done < <(tail -n +3 "$kd/friday.kept")
expect '[ $missing = 0 ]' "every listed entry is in the cache ($missing missing)"
expect '[ "$(ls -A "$kd" | tr "\n" " ")" = "friday.kept friday.manifest " ]' "the set and its manifest, no temporary file: $(ls -A "$kd" | tr '\n' ' ')"
if [ "$windows" = 1 ]; then
  expect '"$RBSP_KEPT_CHECK" "$kd/friday.kept" "$u" "$((w + a))"' "the Windows bridge parser reads the helper's set"
  mkdir "$s/key-check" "$s/decode"
  (cd "$s/key-check" && py "$runner" -1 "$RBSP_KEYS_HELPER" --keys --model "$model" --rekordbox "$RBSP_REKORDBOX" "$s/a.wav") > "$s/keys.out"
  rc=$?
  expect '[ $rc = 0 ] && [ -z "$(ls -A "$s/key-check")" ] && [ "$(awk -F "\t" '\''$1 == "key" && $5 == 0 { print $2 }'\'' "$s/keys.out" | sort -u)" = "$(tail -n +3 "$kd/friday.kept" | cut -d" " -f2 | sort)" ]' \
    "--keys produces exactly the non-silent kept keys and creates no files"
  py "$runner" -1 "$RBSP_DECODE_DUMP" "$RBSP_REKORDBOX" "$s/decode" "$s/a.wav" > "$s/decode.out"
  rc=$?
  expect '[ $rc = 0 ] && grep -q "^a.wav.*1 tracks.*1323000$" "$s/decode.out" && [ "$(wc -c < "$s/decode/a.wav.0.f32" | tr -d " ")" = 10584000 ]' \
    "decode_dump reports all 30 seconds and writes both channels"
fi
before="$(tail -n +3 "$kd/friday.kept")"
out="$(run --keep friday --until $((now + 3600)) "$s/a.wav")"
expect '[ "$(field "$(fileline)" written)" = 0 ] && [ "$(field "$(fileline)" already)" = "$((w + a))" ] && [ "$(tail -n +3 "$kd/friday.kept")" = "$before" ] && [ "$(sed -n 2p "$kd/friday.kept")" = "until $((now + 3600))" ]' \
  "prepared again: every entry already there, the same lines, the new date"
expect '[ "$(field "$(fileline)" how)" = "unchanged since it was prepared" ]' "an unchanged file isn't decoded again: $(fileline)"
out="$(run --no-keep "$s/a.wav")"
expect '[ "$(ls "$kd" | wc -l | tr -d " ")" = 2 ] && [ -z "$(field "$(last)" kept)" ]' "--no-keep writes no set"

echo "== the helper's tag (a platform prefix and a number: 1 the Mac's, w1 Windows')"
start tag
out="$(run --no-keep "$s/a.wav")"
entries=()
while IFS= read -r f; do entries+=("$f"); done < <(find "$c/$id" -name '*.flac' | sort)
# retag FILE TAG: the entry's RBSTEMS_HELPER comment set to TAG (the FLAC's VORBIS_COMMENT rebuilt)
retag() { py - "$1" "$2" << 'EOF'
import struct, sys
p, tag = sys.argv[1], sys.argv[2].encode().decode("unicode_escape").encode("latin-1")   # \x00: a NUL
b = open(p, "rb").read()
assert b[:4] == b"fLaC"
out, i = bytearray(b[:4]), 4
while True:
    h, n = b[i], int.from_bytes(b[i + 1:i + 4], "big")
    body = b[i + 4:i + 4 + n]
    if h & 0x7F == 4:
        vl = struct.unpack("<I", body[:4])[0]
        q = 4 + vl
        cnt = struct.unpack("<I", body[q:q + 4])[0]
        q += 4
        ents = []
        for _ in range(cnt):
            l = struct.unpack("<I", body[q:q + 4])[0]
            e = body[q + 4:q + 4 + l]
            q += 4 + l
            ents.append(b"RBSTEMS_HELPER=" + tag if e.startswith(b"RBSTEMS_HELPER=") else e)
        body = body[:4 + vl] + struct.pack("<I", cnt) + b"".join(struct.pack("<I", len(e)) + e for e in ents) + body[q:]
    out += bytes([h]) + len(body).to_bytes(3, "big") + body
    i += 4 + n
    if h & 0x80:
        break
open(p, "wb").write(out + b[i:])
EOF
}
tagof() { py -c 'import re, sys; m = re.search(rb"RBSTEMS_HELPER=([a-z0-9]*)", open(sys.argv[1], "rb").read(8192)); sys.stdout.write(m.group(1).decode() if m else "")' "$1"; }
if [ "${#entries[@]}" = 3 ]; then
  tag=1 foreign=w1 future=2 older=0
  if [ "$windows" = 1 ]; then tag=w1 foreign=1 future=w2 older=w0; fi
  retag "${entries[0]}" "$foreign"; retag "${entries[1]}" "$future"; retag "${entries[2]}" "$older"
  out="$(run --no-keep "$s/a.wav")"
  expect '[ "$(field "$(fileline)" written)" = 1 ] && [ "$(field "$(fileline)" already)" = 2 ] && [ "$(tagof "${entries[0]}")" = "$foreign" ] && [ "$(tagof "${entries[1]}")" = "$future" ] && [ "$(tagof "${entries[2]}")" = "$tag" ]' \
    "the other platform's tag and a later tag stay, an older tag is rewritten: $(fileline)"
  retag "${entries[0]}" "$(printf '1%.0s' $(seq 80))"; retag "${entries[1]}" '1\x00X'
  out="$(run --no-keep "$s/a.wav")"
  expect '[ "$(field "$(fileline)" written)" = 2 ] && [ "$(field "$(fileline)" already)" = 1 ] && [ "$(tagof "${entries[0]}")" = "$tag" ] && [ "$(tagof "${entries[1]}")" = "$tag" ]' \
    "a tag too long to read and one holding a NUL are rewritten: $(fileline)"
else expect false "3 entries to retag: ${#entries[@]}"; fi

echo "== refusals"
start refuse
run --keep friday --until $((now - 1)) "$s/a.wav" > /dev/null; rc=$?
expect '[ $rc = 2 ]' "a date that has passed is refused"
run --keep "fri day" "$s/a.wav" > /dev/null; rc=$?
expect '[ $rc = 1 ] && grep -q "1 to 64 letters" "$s/err"' "a name with a space is refused: $(cat "$s/err")"
for i in 1 2 3 4 5; do set_ s$i.kept $((now + 3600)); done
set_ old.kept $((now - 60))
run --keep friday "$s/a.wav" > /dev/null; rc=$?
expect '[ $rc = 1 ] && grep -q "5 playlists are kept already" "$s/err" && [ ! -f "$kd/friday.kept" ]' "a sixth set is refused: $(cat "$s/err")"
expect '[ ! -f "$kd/old.kept" ]' "a set whose date has passed is deleted"
rm "$kd/s5.kept"; printf 'not a set\n' > "$kd/bad.kept"
run --keep friday "$s/a.wav" > /dev/null; rc=$?
expect '[ $rc = 1 ] && [ -f "$kd/bad.kept" ]' "a file the bridge can't read still counts towards its 5 (and stays)"
rm "$kd/bad.kept"; set_ s5.kept $((now + 3600))
out="$(run --keep s1 --until $((now + 3600)) "$s/a.wav")"
expect '[ -f "$kd/s1.kept" ] && [ "$(lines s1)" -gt 0 ]' "5 sets, one of them prepared again: allowed"
# an empty --cache or --kept-dir is none, never the working folder
mkdir -p "$s/cwd"
(cd "$s/cwd" && invoke --model "$model" --ort "$ort" --cache "" --kept-dir "$kd" --keep x "$s/a.wav" > /dev/null 2> "$s/err"); rc=$?
expect '[ $rc = 1 ] && grep -q "no cache folder" "$s/err" && [ -z "$(ls -A "$s/cwd")" ]' "an empty --cache: no cache folder: $(cat "$s/err")"
(cd "$s/cwd" && invoke --model "$model" --ort "$ort" --cache "$c" --kept-dir "" --keep x "$s/a.wav" > /dev/null 2> "$s/err"); rc=$?
expect '[ $rc = 1 ] && grep -q "no home folder" "$s/err" && [ ! -e "$s/cwd/x.kept" ] && [ -z "$(ls -A "$s/cwd")" ]' "an empty --kept-dir with a set: no home folder: $(cat "$s/err")"
out="$(cd "$s/cwd" && invoke --estimate --model "$model" --cache "$c" --kept-dir "" "$s/a.wav" 2> "$s/err")"; rc=$?
expect '[ $rc = 0 ] && [ -n "$(field "$(last)" estimate)" ] && [ -z "$(ls -A "$s/cwd")" ]' "an empty --kept-dir without a set: estimated, nothing written there: $(last)"
# a kept sets' folder that can be listed but not searched: refused with its own error, not EIO
if [ "$windows" = 0 ]; then
  chmod 0644 "$kd"
  out="$(invoke --estimate --model "$model" --cache "$c" --kept-dir "$kd" "$s/a.wav" 2> "$s/err")"; rc=$?
  chmod 0755 "$kd"
  expect '[ $rc = 0 ] && [ "$(field "$(last)" refused)" = "the kept sets can'"'"'t be read: Permission denied" ]' "a kept sets' folder without search permission: refused, Permission denied: $(last)"
fi

# 0.1 GB: 107.4 MB; the share 80.5 MB; 90% 96.6 MB; room down to 85.9 MB
echo "== the share"
start share
for i in 1 2 3 4 5 6 7 8; do fake $i 10 "$(stamp 5)"; done
set_ other.kept $((now + 3600)) 1 2 3 4 5 6 7 8
out="$(run --keep friday --max-gb 0.1 "$s/a.wav")"
expect '[[ "$(field "$(last)" stopped)" == "prepared stems would take more than 75% of the cache"* ]] && [ "$(field "$(fileline)" written)" = 0 ] && [ "$(lines friday)" = 0 ]' \
  "past 75% with another set's 80 MB: stopped before writing: $(last)"
out="$(run --no-keep --max-gb 0.1 "$s/a.wav")"
expect '[ "$(field "$(fileline)" written)" -gt 0 ]' "without a set the share doesn't apply: $(head -1 <<< "$out")"

echo "== making room"
start room
fake 1 10 "$(stamp 9)"; fake 2 10 "$(stamp 8)"
for i in 3 4 5 6 7 8 9; do fake $i 10 "$(stamp $((10 - i + 2)))"; done     # 3 oldest of the rest
set_ other.kept $((now + 3600)) 1 2
out="$(run --keep friday --max-gb 0.1 "$s/a.wav")"
expect 'grep -q "\"room\":1," <<< "$out" && there 1 && there 2 && ! there 3 && there 4 && there 9 && [ "$(field "$(fileline)" written)" -gt 0 ] && [ -z "$(field "$(last)" stopped)" ]' \
  "over 90%: the least recently used entry no set keeps goes, kept ones stay, then it prepares: $(tr '\n' ' ' <<< "$out")"
start full
for i in 1 2 3 4 5 6 7 8 9; do fake $i 10 "$(stamp 5)"; done
set_ a.kept $((now + 3600)) 1 2 3 4 5; set_ b.kept $((now + 3600)) 6 7 8 9
out="$(run --no-keep --max-gb 0.1 "$s/a.wav")"
expect '[[ "$(field "$(last)" stopped)" == "the cache holds"* ]] && there 1 && there 9' "nothing to remove (every entry kept): stopped, nothing removed: $(last)"

echo "== the estimate and the manifest"
start est
est() { invoke --estimate --model "$model" --cache "$c" --kept-dir "$kd" "$@" 2> "$s/err"; }
of() { grep -m1 "\"file\":\"$1\"" <<< "$out"; }
out="$(est --keep e "$s/a.wav" "$s/none.wav" streaming:x)"
expect '[ "$(field "$(of "$s/a.wav")" result)" = prepare ] && [ "$(field "$(of "$s/a.wav")" entries)" = 3 ] && [ "$(field "$(of "$s/a.wav")" seconds)" = 30.0 ]' \
  "a file to prepare: 30 s, 3 entries: $(of "$s/a.wav")"
expect '[ "$(field "$(of "$s/none.wav")" result)" = skipped ] && ! grep -q "streaming" <<< "$out"' "a missing file is skipped; an input that isn't a local file gets no line"
expect 'totals && [ "$(field "$(last)" estimate)" = 2 ] && [ "$(field "$(last)" prepare)" = 1 ] && [ "$(field "$(last)" skipped)" = 1 ] && [ "$(field "$(last)" runs)" = 3 ] && [ "$(field "$(last)" fits)" = True ] && [ ! -e "$kd/e.kept" ] && [ ! -e "$c/$id" ]' \
  "the totals count only local files; nothing written: $(last)"
printf '%s\nstreaming:y\n%s\n' "$s/a.wav" "$s/none.wav" > "$s/list"
out="$(est --list "$s/list")"
expect 'totals && [ "$(field "$(last)" estimate)" = 2 ] && [ "$(field "$(last)" skipped)" = 1 ] && ! grep -q "streaming" <<< "$out"' \
  "--list: the same, the input that isn't a local file dropped: $(last)"
out="$(run --keep e "$s/a.wav")"
expect '[ "$(field "$(last)" runs)" = 3 ] && [ "$(sed -n 1p "$kd/e.manifest")" = "rbstemsplus-manifest 1" ] && [ "$(sed -n 3p "$kd/e.manifest")" = "model $id" ] && [ "$(grep -c "^file .* 1 3 ${manifest_rate}$s/a.wav\$" "$kd/e.manifest")" = 1 ] && [ "$(tail -n +7 "$kd/e.manifest" | sort)" = "$(tail -n +3 "$kd/e.kept" | cut -d" " -f2 | sort)" ]' \
  "a set's manifest: its file, complete, and the set's 3 entries: $(sed -n 5,6p "$kd/e.manifest" | tr '\n' ' ')"
out="$(est --keep e)"
expect '[ "$(field "$(of "$s/a.wav")" result)" = ready ] && [ "$(field "$(last)" ready)" = 1 ] && [ "$(field "$(last)" runs)" = 0 ] && [ "$(field "$(last)" need_gb)" = 0.0 ]' \
  "the set checked (its own files): ready: $(tr '\n' ' ' <<< "$out")"
out="$(est --keep e streaming:x)"
expect '[ "$(field "$(last)" estimate)" = 0 ] && ! grep -q "\"file\"" <<< "$out"' "only inputs that aren't local files: none estimated, not the set's own files: $(tr '\n' ' ' <<< "$out")"
printf 'streaming:y\n' > "$s/list2"; out="$(est --keep e --list "$s/list2")"
expect '[ "$(field "$(last)" estimate)" = 0 ] && ! grep -q "\"file\"" <<< "$out"' "--list: the same: $(tr '\n' ' ' <<< "$out")"
out="$(est --keep other "$s/a.wav")"
expect '[ "$(field "$(of "$s/a.wav")" result)" = ready ]' "another set's manifest counts: ready"
k1="$(sed -n 7p "$kd/e.manifest")"; rm "$c/$id/${k1:0:2}/$k1.flac"
out="$(est --keep e)"
expect '[ "$(field "$(of "$s/a.wav")" result)" = prepare ] && [ "$(field "$(of "$s/a.wav")" missing)" = 1 ] && [ "$(field "$(of "$s/a.wav")" entries)" = 1 ]' \
  "an entry gone from the cache: to prepare, 1 missing: $(of "$s/a.wav")"
out="$(run --keep e "$s/a.wav")"; out="$(est --keep e)"
expect '[ "$(field "$(of "$s/a.wav")" result)" = ready ]' "prepared again: ready"
out="$(est --quick --keep e "$s/a.wav" "$other/x.wav")"
expect '[ "$(field "$(of "$s/a.wav")" result)" = ready ] && [ "$(field "$(of "$other/x.wav")" result)" = prepare ] && [ "$(field "$(last)" unchecked)" = 0 ]' \
  "--quick: ready from its size and time; another drive's track no manifest has: to prepare, not read: $(last)"
touch "$s/a.wav"; out="$(est --keep e)"
expect '[ "$(field "$(of "$s/a.wav")" result)" = prepare ] && [ -z "$(field "$(of "$s/a.wav")" missing)" ] && [ "$(field "$(of "$s/a.wav")" entries)" = 3 ]' \
  "the file changed: all of it to prepare: $(of "$s/a.wav")"
out="$(est --quick --keep e)"
expect '[ "$(field "$(of "$s/a.wav")" result)" = prepare ]' "--quick: a changed file isn't ready"
"${sed_in_place[@]}" "s|^file \\([0-9]* [0-9]* 1 [0-9]*\\) ${manifest_rate}$s/a.wav\$|file \\1 ${manifest_rate}$other/a.wav|" "$kd/e.manifest"
out="$(est --quick --keep e)"
expect 'totals && [ "$(field "$(of "$other/a.wav")" result)" = unchecked ] && [ "$(field "$(last)" unchecked)" = 1 ]' "--quick: another drive's track prepared to its end: unchecked, not read"
if [ "$windows" = 1 ]; then
  cp "$kd/e.manifest" "$s/letter.manifest"
  unc="${RBSP_UNC%/}/rbsp-kept-missing/a.wav"
  py -c 'from pathlib import Path; import sys; p=Path(sys.argv[1]); p.write_bytes(p.read_bytes().replace(sys.argv[2].encode(),sys.argv[3].encode()))' "$kd/e.manifest" "$other/a.wav" "$unc"
  out="$(est --quick --keep e)"
  expect 'totals && [ "$(field "$(of "$unc")" result)" = unchecked ] && [ "$(field "$(last)" unchecked)" = 1 ]' "--quick also takes a complete UNC track from its manifest"
  mv "$s/letter.manifest" "$kd/e.manifest"
fi
out="$(invoke --quick --model "$model" --cache "$other/cache" --kept-dir "$kd" --keep e 2> "$s/err")"; rc=$?
expect '[ $rc = 0 ] && [ "$(field "$(of "$other/a.wav")" result)" = unchecked ] && [ "$(field "$(last)" cache_gb)" = 0.0 ]' \
  "--quick with the cache on another drive (not even there): not looked at, still checked: $(last) $(cat "$s/err")"
k1="$(sed -n 7p "$kd/e.manifest")"; rm "$c/$id/${k1:0:2}/$k1.flac"
out="$(est --quick --keep e)"
expect '[ "$(field "$(of "$other/a.wav")" result)" = prepare ] && [ "$(field "$(of "$other/a.wav")" missing)" = 1 ] && [ "$(field "$(last)" unchecked)" = 0 ]' \
  "--quick: another drive's track with an entry gone from this Mac's cache: to prepare, not read: $(of "$other/a.wav")"
rm -rf "$c/$id"
out="$(est --quick --keep e)"
expect '[ "$(field "$(of "$other/a.wav")" result)" = prepare ] && [ "$(field "$(of "$other/a.wav")" missing)" -gt 1 ]' \
  "--quick: the same after the saved stems are cleared: $(of "$other/a.wav")"
start est-share
for i in 1 2 3 4 5 6 7; do fake $i 10 "$(stamp 5)"; done
set_ other.kept $((now + 3600)) 1 2 3 4 5 6 7
out="$(est --keep e --max-gb 0.12 "$s/a.wav" "$s/a.wav" "$s/a.wav")"
expect '[ "$(field "$(last)" fits)" = False ] && [ "$(field "$(last)" fit_files)" = 2 ] && [ "$(field "$(last)" disk_ok)" = True ]' \
  "73 MB kept, 9 MB a file, a share of 97 MB: 2 of 3 fit: $(last)"
start est-stop
for i in 1 2 3 4 5 6 7 8; do fake $i 10 "$(stamp 5)"; done
set_ other.kept $((now + 3600)) 1 2 3 4 5 6 7 8
out="$(run --keep stopped --max-gb 0.1 "$s/a.wav")"; out="$(est --keep stopped)"
expect '[ "$(field "$(of "$s/a.wav")" result)" = prepare ] && grep -q "^file [0-9]* [0-9]* 0 [0-9]* ${manifest_rate}$s/a.wav\$" "$kd/stopped.manifest"' "a file stopped before its end isn't ready: $(of "$s/a.wav")"
"${sed_in_place[@]}" "s|^file \\([0-9]* [0-9]* [01] [0-9]*\\) ${manifest_rate}$s/a.wav\$|file \\1 ${manifest_rate}$other/a.wav|" "$kd/stopped.manifest"
out="$(est --quick --keep stopped)"
expect '[ "$(field "$(of "$other/a.wav")" result)" = prepare ]' "--quick: a track it can't read, stopped before its end: not ready ($(of "$other/a.wav"))"
set_ gone.kept $((now - 60)); printf 'x\n' > "$kd/gone.manifest"
for i in 1 2 3 4; do set_ s$i.kept $((now + 3600)); done
out="$(est --keep sixth "$s/a.wav")"
expect '[[ "$(field "$out" refused)" == "5 playlists are kept already"* ]] && [ ! -e "$kd/gone.kept" ] && [ ! -e "$kd/gone.manifest" ]' \
  "a sixth set is refused; a set past its date goes with its manifest: $out"

echo "== preparing again near the share"
start near
out="$(run --keep near "$s/a.wav")"
kb="$(field "$(last)" kept_gb)"
g="$(py -c 'import sys; sys.stdout.write("%.6f" % ((float(sys.argv[1]) * 2**30 + 2 * 2**20) / 0.75 / 2**30))' "$kb")"
out="$(run --keep near --max-gb "$g" "$s/a.wav")"
expect '[ -z "$(field "$(last)" stopped)" ] && [ "$(field "$(fileline)" how)" = "unchanged since it was prepared" ] && grep -q "^file [0-9]* [0-9]* 1 3 ${manifest_rate}$s/a.wav\$" "$kd/near.manifest"' \
  "a set 2 MB under its share, prepared again: its own entries don't count twice, it stays complete: $(last)"
rm "$kd/near.manifest"
out="$(run --keep near --max-gb "$g" "$s/a.wav")"
expect '[ -z "$(field "$(last)" stopped)" ] && [ "$(field "$(fileline)" already)" = 3 ] && grep -q "^file [0-9]* [0-9]* 1 3 ${manifest_rate}$s/a.wav\$" "$kd/near.manifest"' \
  "the same, decoded again (no manifest): entries already there pass the share: $(last)"

echo "== the share counts every entry once"
start share-new
out="$(run --no-keep "$s/a.wav")"
out="$(run --keep x --max-gb 0.008 "$s/a.wav")"
expect '[[ "$(field "$(last)" stopped)" == *"75%"* ]] && py -c "import sys; sys.exit(float(sys.argv[1]) > 0.006)" "$(field "$(last)" kept_gb)"' \
  "entries in the cache that no set keeps count against the share: $(last)"
start share-gone
out="$(run --keep y "$s/a.wav")"
kb="$(field "$(last)" kept_gb)"
rm -rf "${c:?}/$id"
out="$(run --keep y "$s/a.wav")"
expect '[ "$(field "$(last)" kept_gb)" = "$kb" ] && [ "$(field "$(fileline)" written)" -gt 0 ]' \
  "a set's entries that left the cache count when written again: $(field "$(last)" kept_gb) of $kb"
rm -rf "${c:?}/$id"
out="$(run --keep y --max-gb 0.0065 "$s/a.wav")"
expect '[[ "$(field "$(last)" stopped)" == *"75%"* ]]' "and so the share stops them: $(last)"

echo "== a track skipped this time stays prepared"
start carry
mkdir -p "$s/drive"
py -c 'import math, random, struct, sys, wave
random.seed(8)
w = wave.open(sys.argv[1], "wb"); w.setnchannels(2); w.setsampwidth(2); w.setframerate(44100)
w.writeframes(b"".join(struct.pack("<hh", int(9000 * math.sin(i / 7.0) + 900 * (random.random() - 0.5)), int(8000 * math.sin(i / 9.0))) for i in range(44100 * 15)))
w.close()' "$s/drive/b.wav"
out="$(run --keep carry "$s/a.wav" "$s/drive/b.wav")"
n="$(lines carry)"
mv "$s/drive" "$s/drive.away"
out="$(run --keep carry "$s/a.wav" "$s/drive/b.wav")"
expect '[ -z "$(field "$(last)" stopped)" ] && [ "$(field "$(of "$s/drive/b.wav")" result)" = skipped ] && grep -q "^file [0-9]* [0-9]* 1 [1-9] ${manifest_rate}$s/drive/b.wav\$" "$kd/carry.manifest" && [ "$(lines carry)" = "$n" ] && [ "$n" -gt 3 ]' \
  "its drive unplugged: skipped, but its record and entries stay in the set ($(lines carry) of $n lines)"
printf '%s\n%s\n' "$s/a.wav" "$s/drive/b.wav" > "$s/first.txt"
out="$(run --keep carry --list "$s/first.txt")"
expect '[ "$(field "$(of "$s/drive/b.wav")" result)" = skipped ] && [ "$(lines carry)" = "$n" ]' "the same with a list: $(lines carry) of $n lines"
# the cache's drive gone during the run: the list is read after the cache is checked, and opening
# the FIFO's other end returns once the helper has opened it, so the cache moves away just then
# (a helper that never opens it: the reader in the background lets the test go on, and fail)
gone_with_list() {
  local fifo="$s/list.$1.fifo" watchdog
  mkfifo "$fifo"
  "$bin" --model "$model" --ort "$ort" --cache "$c" --kept-dir "$kd" --keep carry --list "$fifo" > "$s/gone.out" 2> "$s/err" &
  pid=$!
  ( sleep 120; cat "$fifo" ) > /dev/null 2>&1 &
  watchdog=$!
  exec 3> "$fifo"
  mv "$c" "$c.away"
  printf '%s\n' "$2" >&3
  exec 3>&-
  wait "$pid"; rc=$?
  kill "$watchdog" 2> /dev/null; wait "$watchdog" 2> /dev/null
  out="$(cat "$s/gone.out")"
  mv "$c.away" "$c"
}
before="$(tail -n +3 "$kd/carry.kept")"; was="$(grep '^file' "$kd/carry.manifest" | sort)"
if [ "$windows" = 1 ]; then
  # A regular list keeps working across processes. Move the cache once the first run has closed it.
  mv "$s/drive.away" "$s/drive"
  mv "$c" "$c.away"
  run --chosen --keep carry --list "$s/first.txt" > "$s/gone.out"; rc=$?
  expect '[ $rc = 1 ] && grep -q "no cache folder" "$s/err" && [ ! -e "$c" ] && [ "$(tail -n +3 "$kd/carry.kept")" = "$before" ] && [ "$(grep "^file" "$kd/carry.manifest" | sort)" = "$was" ]' \
    "a moved chosen cache is refused with its kept set and manifest intact"
  c="$c.away"
  out="$(run --chosen --keep carry --list "$s/first.txt")"
  expect '[ "$(field "$(last)" done)" = 2 ] && [ "$(grep -c "unchanged since it was prepared" <<< "$out")" = 2 ] && [ "$(tail -n +3 "$kd/carry.kept")" = "$before" ]' \
    "opening the cache at its new path still reuses the manifest and entries"
else
gone_with_list 1 "$s/drive/b.wav"
expect '[ $rc = 0 ] && [[ "$(field "$(last)" stopped)" == *"cache folder"* ]] && [ "$(tail -n +3 "$kd/carry.kept")" = "$before" ] && [ "$(grep "^file" "$kd/carry.manifest" | sort)" = "$was" ]' \
  "the cache gone during the run: it stops, the set's entries and records stay as they were: $(last)"
gone_with_list 2 "$s/a.wav"
expect '[[ "$(field "$(last)" stopped)" == *"cache folder"* ]] && [ "$(tail -n +3 "$kd/carry.kept")" = "$before" ] && [ "$(grep "^file" "$kd/carry.manifest" | sort)" = "$was" ]' \
  "the same with the track still readable (music on the Mac, the cache on a drive): $(last)"
mv "$s/drive.away" "$s/drive"
start midway
out="$(run --keep m "$s/a.wav")"
rm -rf "${c:?}/$id"
"$bin" --model "$model" --ort "$ort" --cache "$c" --kept-dir "$kd" --keep m "$s/a.wav" > "$s/mid.out" 2> "$s/err" &
pid=$!
for i in $(seq 600); do grep -q '"chunk":1,' "$s/mid.out" 2> /dev/null && break; sleep 0.1; done
mv "$c" "$c.away"
wait "$pid"
out="$(cat "$s/mid.out")"
mv "$c.away" "$c"
expect '[[ "$(field "$(last)" stopped)" == *"cache folder"* ]] && grep -q "^file [0-9]* [0-9]* 1 3 ${manifest_rate}$s/a.wav\$" "$kd/m.manifest"' \
  "gone during a track: the reason says so, and its earlier record stays: $(last)"
fi

echo "== part of a playlist, and a set over the share"
start first-n
out="$(run --keep f "$s/a.wav" "$s/drive/b.wav")"
n="$(lines f)"
out="$(run --keep f --first 1 "$s/a.wav" "$s/drive/b.wav")"
expect '[ -z "$(field "$(last)" stopped)" ] && [ "$(field "$(last)" done)" = 1 ] && [ -z "$(of "$s/drive/b.wav")" ] && [ "$(lines f)" = "$n" ] && grep -q "^file [0-9]* [0-9]* 1 [1-9] ${manifest_rate}$s/drive/b.wav\$" "$kd/f.manifest"' \
  "--first runs its N and keeps the playlist's other tracks ($(lines f) of $n lines)"
out="$(run --keep f --first 1 "$s/a.wav" "$s/new.wav")"
expect '[ -z "$(field "$(last)" stopped)" ] && [ "$(lines f)" = 3 ] && ! grep -q "b.wav" "$kd/f.manifest" && ! grep -q "new.wav" "$kd/f.manifest"' \
  "--first lets go of a track that left the playlist ($(lines f) lines)"
out="$(run --keep f "$s/a.wav" "$s/drive/b.wav")"
"${sed_in_place[@]}" "s/^model .*/model $(key 5)/" "$kd/f.manifest"
out="$(run --keep f --first 1 "$s/a.wav" "$s/drive/b.wav")"
expect '[ -z "$(field "$(last)" stopped)" ] && [ "$(lines f)" = 3 ] && ! grep -q "b.wav" "$kd/f.manifest" && grep -q "^model $id\$" "$kd/f.manifest"' \
  "--first lets go of another model's set: entries and records together ($(lines f) lines)"
start over
out="$(run --keep o "$s/a.wav")"
out="$(est --keep o --max-gb 0.008 "$s/a.wav")"
expect '[ "$(field "$(of "$s/a.wav")" result)" = ready ] && [ "$(field "$(last)" fits)" = True ] && [ "$(field "$(last)" fit_files)" = 1 ]' \
  "a ready set over a lowered limit still fits (nothing to add): $(last)"
out="$(run --keep o --max-gb 0.008 "$s/a.wav")"
expect '[ -z "$(field "$(last)" stopped)" ] && [ "$(field "$(of "$s/a.wav")" result)" = prepared ]' "and it can be kept again: $(last)"
out="$(est --quick --keep o --max-gb 0.008 "$s/a.wav" "$s/drive/b.wav")"
expect '[ "$(field "$(of "$s/drive/b.wav")" result)" = prepare ] && [ "$(field "$(of "$s/drive/b.wav")" gb)" = 0.0 ] && [ "$(field "$(last)" fits)" = False ] && [ "$(field "$(last)" fit_files)" = 1 ]' \
  "a set over the limit with a track of unknown length to prepare doesn't fit: $(last)"

echo "== the share holds only what the playlist still has"
start departed
out="$(run --keep d "$s/a.wav")"
out="$(est --keep d --max-gb 0.016 "$s/drive/b.wav")"
expect '[ "$(field "$(last)" fits)" = True ] && [ "$(field "$(last)" fit_files)" = 1 ]' "a track that left the playlist doesn't hold the share: $(last)"
out="$(run --keep d --max-gb 0.016 "$s/drive/b.wav")"
expect '[ -z "$(field "$(last)" stopped)" ] && [ "$(field "$(of "$s/drive/b.wav")" result)" = prepared ] && ! grep -q "a.wav" "$kd/d.manifest"' "and the run gets through, letting it go: $(last)"
start switched
out="$(run --keep w "$s/a.wav")"
"${sed_in_place[@]}" "s/^model .*/model $(key 5)/" "$kd/w.manifest"
keys="$(tail -n +3 "$kd/w.kept" | sed "s/^[^ ]*/$(key 5)/")"
{ sed -n 1,2p "$kd/w.kept"; printf '%s\n' "$keys"; } > "$kd/w.tmp" && mv "$kd/w.tmp" "$kd/w.kept"
mv "$c/$id" "$c/$(key 5)"
out="$(est --keep w --max-gb 0.016 "$s/drive/b.wav")"
expect '[ "$(field "$(last)" fits)" = True ]' "another model's set doesn't hold the share: $(last)"
start retag
out="$(run --keep r "$s/a.wav")"
touch "$s/a.wav"
out="$(est --keep r --max-gb 0.016 "$s/a.wav")"
expect '[ "$(field "$(of "$s/a.wav")" result)" = prepare ] && [ "$(field "$(last)" fits)" = True ]' "a re-tagged track is counted once: $(last)"
out="$(run --keep r --max-gb 0.016 "$s/a.wav")"
expect '[ -z "$(field "$(last)" stopped)" ] && [ "$(field "$(fileline)" written)" = 0 ] && [ "$(lines r)" = 3 ]' "and the run finds its entries and writes nothing: $(last)"
start order
out="$(run --keep q "$s/drive/b.wav")"
out="$(est --keep q --max-gb 0.012 "$s/a.wav" "$s/drive/b.wav")"
expect '[ "$(field "$(of "$s/drive/b.wav")" result)" = ready ] && [ "$(field "$(last)" fits)" = False ] && [ "$(field "$(last)" fit_files)" = 0 ]' "a ready track further down still counts for the first tracks: $(last)"

py - "$s" << 'EOF2'
import math, random, struct, sys, wave
def music(path, secs, seed):
    random.seed(seed)
    w = wave.open(path, "wb"); w.setnchannels(2); w.setsampwidth(2); w.setframerate(44100)
    fr = bytearray()
    for i in range(44100 * secs):
        t = i / 44100
        v = 0.3 * math.sin(2 * math.pi * (110 + seed) * t) * (1 + math.sin(2 * math.pi * 2 * t)) / 2 + 0.05 * (random.random() - 0.5)
        fr += struct.pack("<hh", int(v * 30000), int(v * 26000))
    w.writeframes(bytes(fr)); w.close()
music(sys.argv[1] + "/n1.wav", 11, 1); music(sys.argv[1] + "/n2.wav", 30, 2); music(sys.argv[1] + "/n3.wav", 30, 3)
EOF2
start past-n
out="$(run --keep g "$s/n3.wav")"
touch "$s/n3.wav"
out="$(est --keep g --max-gb 0.0195 "$s/n1.wav" "$s/n2.wav" "$s/n3.wav")"
n="$(field "$(last)" fit_files)"
out2="$out"
out="$(run --keep g --max-gb 0.0195 --first "$n" "$s/n1.wav" "$s/n2.wav" "$s/n3.wav")"
expect '[ "$(field "$(tail -1 <<< "$out2")" fits)" = False ] && [ "$n" = 1 ] && [ -z "$(field "$(last)" stopped)" ] && grep -q "n3.wav" "$kd/g.manifest"' \
  "the first $n offered with a changed track past them: the run finishes and keeps it: $(last)"
start replaced
py - "$s" << 'EOF2'
import math, random, struct, sys, wave
def music(path, secs, seed):
    random.seed(seed)
    w = wave.open(path, "wb"); w.setnchannels(2); w.setsampwidth(2); w.setframerate(44100)
    fr = bytearray()
    for i in range(44100 * secs):
        t = i / 44100
        v = 0.3 * math.sin(2 * math.pi * (110 + seed) * t) * (1 + math.sin(2 * math.pi * 2 * t)) / 2 + 0.05 * (random.random() - 0.5)
        fr += struct.pack("<hh", int(v * 30000), int(v * 26000))
    w.writeframes(bytes(fr)); w.close()
music(sys.argv[1] + "/long.wav", 90, 4); music(sys.argv[1] + "/shorter.wav", 10, 5); music(sys.argv[1] + "/n4.wav", 30, 6)
EOF2
out="$(run --keep l "$s/long.wav")"
cp "$s/shorter.wav" "$s/long.wav"
out="$(est --keep l --max-gb 0.03 "$s/n4.wav" "$s/long.wav")"
fits="$(field "$(last)" fits)"
out="$(run --keep l --max-gb 0.03 "$s/n4.wav" "$s/long.wav")"
expect '[ "$fits" = True ] && [ -z "$(field "$(last)" stopped)" ]' "a track replaced by shorter audio: the estimate says it fits, and the run agrees: $(last)"
start moved
mkdir -p "$s/elsewhere"
cp "$s/n1.wav" "$s/n2.wav" "$s/elsewhere/"
out="$(run --keep v "$s/n1.wav" "$s/n2.wav")"
n="$(lines v)"
stop_run() {
  local chunk="$1" pid i
  shift
  if [ "$windows" = 1 ]; then
    py "$runner" "$chunk" "$bin" --rekordbox "$RBSP_REKORDBOX" --model "$model" --ort "$ort" --cache "$c" --kept-dir "$kd" "$@"
  else
    "$bin" --model "$model" --ort "$ort" --cache "$c" --kept-dir "$kd" "$@" > "$s/stop.out" &
    pid=$!
    for i in $(seq 600); do grep -q "\"chunk\":$chunk," "$s/stop.out" && break; sleep 0.1; done
    kill -TERM "$pid"; wait "$pid"; local rc=$?
    cat "$s/stop.out"
    return "$rc"
  fi
}
out="$(stop_run 0 --keep v "$s/n3.wav" "$s/elsewhere/n1.wav" "$s/elsewhere/n2.wav" 2> "$s/err")"
expect '[ "$(field "$(last)" stopped)" = stopped ] && [ "$(tail -n +3 "$kd/v.kept" | wc -l | tr -d " ")" -ge "$n" ]' "moved tracks, a run stopped before them: their entries stay in the set ($(lines v) of at least $n lines)"
start subtract
out="$(run --keep t "$s/n1.wav" "$s/n3.wav")"
touch "$s/n1.wav" "$s/n3.wav"
out="$(est --keep t --max-gb 0.018 "$s/n1.wav" "$s/n2.wav" "$s/n4.wav" "$s/n3.wav")"
first_one="$(field "$(last)" fit_files)"
expect '[ "$first_one" = 1 ]' "a changed track among the first N stops counting as past them: $(last)"
if [ "$windows" = 0 ]; then
  start cloud
  out="$(est --quick --keep z "$HOME/Library/Mobile Documents/com~apple~CloudDocs/rbsp-none.wav" "$HOME/Library/CloudStorage/rbsp-none/x.wav")"
  expect '[ "$(field "$(of "$HOME/Library/Mobile Documents/com~apple~CloudDocs/rbsp-none.wav")" result)" = prepare ] && [ "$(field "$(of "$HOME/Library/CloudStorage/rbsp-none/x.wav")" result)" = prepare ]' "--quick doesn't look into cloud folders: $(last)"
fi

echo "== this Mac's cache folder cleared"
start cleared
out="$(run --keep n "$s/a.wav")"
rm -rf "$c"
out="$(est --keep n "$s/a.wav")"
expect '[ "$(field "$(of "$s/a.wav")" result)" = prepare ] && [ -d "$c" ]' "a cleared cache folder is an empty one, made again: its tracks are to prepare: $(last)"
rm -rf "$c"
out="$(est --chosen --keep n "$s/a.wav")"; rc=$?
expect '[ $rc = 1 ] && [ ! -e "$c" ]' "a chosen location's is never made: the helper says it's gone (exit $rc)"
mkdir -p "$c"

echo "== stopping after the current chunk"
start stop
py -c 'import wave, sys
r = wave.open(sys.argv[1]); fr = r.readframes(r.getnframes())
w = wave.open(sys.argv[2], "wb"); w.setnchannels(2); w.setsampwidth(2); w.setframerate(44100)
for _ in range(3): w.writeframes(fr)
w.close()' "$s/a.wav" "$s/l.wav"
out="$(stop_run 1 --keep st "$s/l.wav" "$s/a.wav" 2> "$s/err")"; rc=$?
expect '[ $rc = 0 ] && [ "$(field "$(last)" stopped)" = stopped ] && [ "$(field "$(fileline)" result)" = stopped ] && ! grep -q "\"file\":\"$s/a.wav\"" <<< "$out"' \
  "stopped after its current chunk, the next file not started: $(last)"
expect 'grep -q "\"at\":0,\"files\":2,\"chunk\":0,\"chunks\":9}" <<< "$out" && [ "$(lines st)" -ge 1 ] && grep -q "^file [0-9]* [0-9]* 0 [0-9]* ${manifest_rate}$s/l.wav\$" "$kd/st.manifest"' \
  "progress lines; the set holds what was written, the file not complete"

echo "== the run path with stdin = /dev/null (as the Mac app starts it: Windows' stdin Stop isn't built here)"
if [ "$windows" = 0 ]; then
  start devnull
  out="$(run --keep dn "$s/a.wav" < /dev/null)"; rc=$?
  expect '[ $rc = 0 ] && [ "$(field "$(fileline)" result)" = prepared ] && [ -z "$(field "$(last)" stopped)" ] && [ "$(lines dn)" -ge 1 ]' \
    "a run whose stdin is at its end isn't stopped by it: $(last)"
fi

echo "== an AAC file with a short last packet, prepared for the first time"
# ffmpeg's encoder: two lengths of the same audio, which share every chunk but the last
if command -v ffmpeg > /dev/null; then
  start aac
  ffmpeg -v error -y -i "$s/a.wav" -t 29.9 -c:a aac -b:a 192k "$s/f.m4a"
  out="$(run --keep aac "$s/f.m4a")"
  w="$(field "$(fileline)" written)"
  expect '[ "$(field "$(fileline)" result)" = prepared ] && [ "$w" -gt 0 ] && [ "$(field "$(fileline)" already)" = 0 ] && [ "$(lines aac)" = "$w" ]' \
    "a chunk the lengths share counts once, none \"already there\": $(fileline)"
elif [ "$windows" = 1 ]; then expect false "the AAC case requires ffmpeg"
else echo "  (skipped: no ffmpeg)"; fi

if [ "$windows" = 1 ]; then
  expect '[ ! -e "$RBSP_TEST_RUN_ERRORS" ]' "all helper stdout is LF-only and the requested stop milestones were reached"
  expect 'py -c '\''from pathlib import Path; import sys; files=[p for p in Path(sys.argv[1]).rglob("*") if p.suffix in (".kept", ".manifest")]; assert files and all(b"\r" not in p.read_bytes() for p in files)'\'' "$s"' \
    "kept sets and manifests are LF-only"
fi

echo
echo "$passed passed, $failures failed"
[ "$failures" = 0 ]

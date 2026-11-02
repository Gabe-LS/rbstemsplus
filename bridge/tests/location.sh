# Sourced by run.sh: config.ini's shared fixtures and chosen locations (cache_dir, cache_id).
# Uses run.sh's helpers (fresh, harness, outcome, has, count, expect, config, layout_ok).

ID=0F8C2D3A-1B2C-4D5E-8F90-1A2B3C4D5E6F
OTHER_ID=1F8C2D3A-1B2C-4D5E-8F90-1A2B3C4D5E6F
FOLDER="RB Stems Plus Cache.noindex"
fixture_file="$here/../../tests/config-fixtures.txt"

statef() { echo "$th/Library/Logs/rbstemsplus/bridge-state"; }
state_is() { grep -qx "state=$1" "$(statef)" 2> /dev/null && grep -qx "reason=$2" "$(statef)"; }
marker() { printf 'id=%s\nformat=rbstems-v1\n' "${2:-$ID}" > "$1/.rbstemsplus-cache"; }
# a chosen location: our folder in $1 (made, with its marker, unless $2 = none) and config.ini naming it
chosen_at() {
  loc="$1/$FOLDER"
  mkdir -p "$1"
  if [ "${2:-}" != none ]; then mkdir -p "$loc"; marker "$loc"; fi
  config "cache_dir=\"$loc\"\ncache_id=$ID\n"
  hcache="$loc"
}
flacs() { find "$1" -name "*.flac" ! -name "._*" | wc -l | tr -d " "; }
newest_flac() { find "$1" -name "*.flac" ! -name "._*" -newer "$2" | head -1; }

# ---------------------------------------------------------------- config.ini: the shared fixtures

# Each fixture as the bridge reads it: the values from its "bridge loaded" line, the location from
# its log (the app's tests read the same file). Then a config.ini that can't be read.
fixtures() {
  point "$fake"
  local name expected w e g dd where dir got list ran=0 want
  list="$(python3 -I "$here/fixtures.py" list "$fixture_file")" || { fail "fixtures.py couldn't list $fixture_file"; return; }
  want="$(grep -vc '^#' "$fixture_file")"
  while IFS=$'\t' read -r name expected; do
    ran=$((ran + 1))
    e=1 g=20 dd=60 where= dir=
    for w in $expected; do
      case "$w" in
        enabled=*) e="${w#*=}" ;; max_gb=*) g="${w#*=}" ;; max_days=*) dd="${w#*=}" ;;
        location=*) where="${w#*=}" ;; dir=*) dir="${w#*=}"; dir="${dir//%20/ }" ;;
      esac
    done
    fresh "fx-$name"
    mkdir -p "$h/Library/Application Support/rbstemsplus"
    python3 -I "$here/fixtures.py" write "$fixture_file" "$name" "$h/Library/Application Support/rbstemsplus/config.ini" \
      || { fail "fixture $name: couldn't be written"; continue; }
    harness passthrough "$fake" > /dev/null 2>&1
    got="$(grep -o 'cache .*, rekordbox_model' "$(logf)" | head -1)"
    local values="enabled=$e, max_gb=$(printf %.1f "$g"), max_days=$(printf %g "$dd")"
    case "$where" in
      unreadable) expect 'has "ERROR config: config.ini can'"'"'t be read" && has "cache NONE, " && state_is unavailable config' "fixture $name: unreadable; got $got" ;;
      malformed) expect 'has "is set wrongly: nothing is cached" && has "cache NONE, $values"' "fixture $name: malformed; got $got" ;;
      chosen) expect 'has "cache $dir (chosen in Settings: " && has "), $values" && ! has "ERROR config"' "fixture $name: chosen $dir; got $got" ;;
      thismac) expect 'has "cache $th/Library/Caches/rbstemsplus, $values" && ! has "ERROR config"' "fixture $name: this Mac; got $got" ;;
      *) fail "fixture $name: no location in \"$expected\"" ;;
    esac
  done <<< "$list"
  expect '[ "$ran" = "$want" ]' "every config fixture ran: $ran of $want"
  # the marker fixtures, shared with the app (tests/marker-fixtures.txt): one chosen folder each
  if [ -n "$have_model" ]; then
    local mf="$here/../../tests/marker-fixtures.txt" verdict d="$scratch/marker-$arch" mran=0 mwant
    mwant="$(grep -vc '^#' "$mf")"
    point "$real"
    while IFS=$'\t' read -r name verdict; do
      mran=$((mran + 1))
      fresh "mk-$name"; anyvol=1; rm -rf "$d"; chosen_at "$d"
      python3 -I "$here/fixtures.py" write "$mf" "$name" "$loc/.rbstemsplus-cache" || { fail "marker fixture $name: couldn't be written"; continue; }
      harness run "$scratch/model/hdemucs.onnx" > /dev/null 2>&1
      if [ "$verdict" = valid ]; then expect 'state_is available ""' "marker fixture $name: valid, the location is used"
      else expect 'state_is unavailable marker' "marker fixture $name: refused (marker)"; fi
    done <<< "$(python3 -I "$here/fixtures.py" list "$mf")"
    expect '[ "$mran" = "$mwant" ]' "every marker fixture ran: $mran of $mwant"
    point "$fake"
  fi

  local c
  fresh cfg-folder; mkdir -p "$h/Library/Application Support/rbstemsplus/config.ini"
  harness passthrough "$fake" > /dev/null 2>&1
  expect 'has "config.ini can'"'"'t be read" && has "cache NONE, "' "a config.ini that is a folder can't be read: nothing is cached"
  fresh cfg-perm; config 'max_gb=3\n'; c="$h/Library/Application Support/rbstemsplus/config.ini"; chmod 000 "$c"
  harness passthrough "$fake" > /dev/null 2>&1
  chmod 644 "$c"
  expect 'has "config.ini can'"'"'t be read" && has "cache NONE, " && state_is unavailable config' "a config.ini without permission can't be read: nothing is cached"
  fresh cfg-none
  harness passthrough "$fake" > /dev/null 2>&1
  expect 'grep -qx "state=available" "$(statef)" && grep -qx "location=" "$(statef)" && grep -q "^pid=[0-9]*$" "$(statef)" && grep -q "^start=[1-9][0-9]*$" "$(statef)"' \
    "bridge-state: this Mac, available, with the process's pid and start"
}

# ---------------------------------------------------------------- a chosen location

location() {
  if [ -z "$have_model" ]; then skip "chosen locations (need Pioneer's library and python3)"; return; fi
  point "$real"
  local m="$scratch/model/hdemucs.onnx" d="$scratch/drive-$arch" out want sha v ino stamp
  sha="$(shasum -a 256 "$m" | cut -d' ' -f1)"

  fresh loc; anyvol=1; rm -rf "$d"; chosen_at "$d"
  out="$(outcome "$m" a wait a b c d wait | tr '\n' ' ')"
  want="a model stored 1 a cache b cache c model d model stored 3 "
  expect '[ "$out" = "$want" ]' "chosen, an empty folder: miss, write, hit, near in one run: got \"$out\", want \"$want\""
  expect '[ "$(flacs "$loc")" = 3 ] && layout_ok "$loc" && [ -d "$loc/$sha" ]' "chosen: the entries are in the chosen folder"
  expect '[ "$(flacs "$h/Library/Caches/rbstemsplus")" = 0 ] && ls "$h/Library/Caches/rbstemsplus"/model-*.sha256 > /dev/null 2>&1 && ! ls "$loc"/model-* > /dev/null 2>&1' \
    "chosen: nothing saved on this Mac; the model's checksum is remembered there, never on the drive"
  expect 'has "cache: using $loc (" && state_is available "" && grep -qxF "location=$loc" "$(statef)"' "chosen: in use, and bridge-state says so"
  expect '! has ERROR && ! has "not available"' "chosen: no ERROR"
  out="$(outcome "$m" a b | tr '\n' ' ')"
  expect '[ "$out" = "a cache b cache " ]' "chosen: a new process finds them: got \"$out\""

  fresh loc-missing; anyvol=1; rm -rf "$d"; chosen_at "$d" none
  out="$(outcome "$m" a sleep a | tr '\n' ' ')"
  expect '[ "$out" = "a model stored 0 a model " ] && [ ! -e "$loc" ] && [ -z "$(ls -A "$d")" ]' "chosen, its folder missing: the model runs, nothing is created: got \"$out\""
  expect 'state_is unavailable folder && has "not available (its folder is missing" && (has "summaries not loaded" || has ", not cached")' "chosen, its folder missing: unavailable (folder), logged"

  # not on an external volume (the real check, as in a release build): a path on this Mac's disk,
  # like a mount point left in /Volumes after the drive went, is "not connected"
  fresh loc-boot; rm -rf "$d"; chosen_at "$d"
  out="$(outcome "$m" a sleep | tr '\n' ' ')"
  expect '[ "$out" = "a model stored 0 " ] && state_is unavailable not-connected && [ "$(ls -A "$loc")" = .rbstemsplus-cache ]' \
    "chosen on this Mac's own disk: not connected, nothing written: got \"$out\""

  for v in wrong-id missing fifo link big format two-ids; do
    fresh "loc-marker-$v"; anyvol=1; rm -rf "$d"; chosen_at "$d"
    case $v in
      wrong-id) marker "$loc" "$OTHER_ID" ;;
      missing) rm "$loc/.rbstemsplus-cache" ;;
      fifo) rm "$loc/.rbstemsplus-cache"; mkfifo "$loc/.rbstemsplus-cache" ;;
      link) mv "$loc/.rbstemsplus-cache" "$d/marker"; ln -s "$d/marker" "$loc/.rbstemsplus-cache" ;;
      big) { printf 'id=%s\nformat=rbstems-v1\n' "$ID"; head -c 5000 /dev/zero | tr '\0' '#'; } > "$loc/.rbstemsplus-cache" ;;
      format) printf 'id=%s\nformat=rbstems-v2\n' "$ID" > "$loc/.rbstemsplus-cache" ;;
      two-ids) printf 'id=%s\nid=%s\nformat=rbstems-v1\n' "$ID" "$OTHER_ID" > "$loc/.rbstemsplus-cache" ;;
    esac
    out="$(outcome "$m" a sleep | tr '\n' ' ')"
    expect '[ "$out" = "a model stored 0 " ] && state_is unavailable marker' "chosen, marker $v: unavailable (marker), nothing written: got \"$out\""
  done
  fresh loc-marker-case; anyvol=1; rm -rf "$d"; chosen_at "$d"; marker "$loc" "$(tr A-F a-f <<< "$ID")"
  out="$(outcome "$m" a wait | tr '\n' ' ')"
  expect '[ "$out" = "a model stored 1 " ] && state_is available ""' "chosen: the marker's id compares ignoring case: got \"$out\""

  # a link on the way to our folder
  fresh loc-link; anyvol=1; rm -rf "$d"; mkdir -p "$d/real/$FOLDER"; marker "$d/real/$FOLDER"; ln -s "$d/real" "$d/via"
  config "cache_dir=\"$d/via/$FOLDER\"\ncache_id=$ID\n"; hcache="$d/real/$FOLDER"
  out="$(outcome "$m" a sleep | tr '\n' ' ')"
  expect '[ "$out" = "a model stored 0 " ] && state_is unavailable folder' "chosen, a link on the way to the folder: unavailable (folder): got \"$out\""
  fresh loc-folder-link; anyvol=1; rm -rf "$d"; mkdir -p "$d/real"; marker "$d/real"; ln -s "$d/real" "$d/$FOLDER"
  config "cache_dir=\"$d/$FOLDER\"\ncache_id=$ID\n"; hcache="$d/real"
  out="$(outcome "$m" a sleep | tr '\n' ' ')"
  expect '[ "$out" = "a model stored 0 " ] && state_is unavailable folder && [ "$(ls -A "$d/real")" = .rbstemsplus-cache ]' "chosen, the folder itself a link: unavailable, nothing written: got \"$out\""

  # the memos: a forged one on the drive is never read; a bad one in this Mac's folder is ignored
  fresh loc-memo; anyvol=1; rm -rf "$d"; chosen_at "$d"
  ino="$(stat -f %i "$m")" stamp="$(stat -f '%z %m' "$m")"
  printf '%s %s\n' "$stamp" "$(printf 'e%.0s' {1..64})" > "$loc/model-$ino.sha256"
  mkdir -p "$h/Library/Caches/rbstemsplus"
  printf '%s ../../../../%s\n' "$stamp" "$(printf 'x%.0s' {1..60})" > "$h/Library/Caches/rbstemsplus/model-$ino.sha256"
  out="$(outcome "$m" a wait | tr '\n' ' ')"
  expect '[ "$out" = "a model stored 1 " ] && [ -d "$loc/$sha" ] && [ "$(ls "$loc" | grep -cv "^model-")" = 1 ]' "a forged memo on the drive, a bad one on this Mac: the model is hashed: got \"$out\""
  expect 'grep -q " $sha$" "$h/Library/Caches/rbstemsplus/model-$ino.sha256"' "the bad memo is replaced with the real checksum"
}

# ---------------------------------------------------------------- what the drive holds

entries() {
  if [ -z "$have_model" ]; then skip "a chosen location's entries (need Pioneer's library and python3)"; return; fi
  point "$real"
  local m="$scratch/model/hdemucs.onnx" d="$scratch/drive-$arch" out t="$scratch/t-$arch" fa fd ka x o="$scratch/out-$arch" sha
  sha="$(shasum -a 256 "$m" | cut -d' ' -f1)"
  seed() {                                               # a and d stored; fa/fd their FLACs, ka a's key
    fresh "ent-$1"; anyvol=1; rm -rf "$d" "$o"; mkdir -p "$o"; chosen_at "$d"
    touch "$t"; sleep 1; outcome "$m" a wait > /dev/null; fa="$(newest_flac "$loc" "$t")"
    touch "$t"; sleep 1; outcome "$m" d wait > /dev/null; fd="$(newest_flac "$loc" "$t")"
    ka="$(basename "$fa" .flac)"; x="$(dirname "$fa")"
  }

  seed fifo; rm "$fa"; mkfifo "$fa"
  out="$(outcome "$m" a wait | tr '\n' ' ')"
  expect '[ "$out" = "a model stored 2 " ] && [ -f "$fa" ] && [ ! -p "$fa" ] && state_is available ""' "a FIFO at an entry's .flac: a miss (no hang), rewritten: got \"$out\""
  seed fifo-desc; rm "$x/$ka.desc"; mkfifo "$x/$ka.desc"
  out="$(outcome "$m" b a sleep | tr '\n' ' ')"
  expect '[ "$out" = "b model a cache stored 3 " ] && [ -f "$x/$ka.desc" ] && [ ! -p "$x/$ka.desc" ]' "a FIFO at an entry's .desc: no near hit, no hang; the hit rewrites it: got \"$out\""
  seed link; cp "$fd" "$o/x.flac"; rm "$fa"; ln -s "$o/x.flac" "$fa"
  out="$(outcome "$m" a wait | tr '\n' ' ')"
  expect '[ "$out" = "a model stored 2 " ] && [ -f "$fa" ] && [ ! -L "$fa" ] && cmp -s "$fd" "$o/x.flac"' "a link at an entry's .flac: a miss, replaced; its target untouched: got \"$out\""
  seed prefix-link; mv "$x" "$o/prefix"; ln -s "$o/prefix" "$x"
  out="$(outcome "$m" a sleep | tr '\n' ' ')"
  expect '[[ "$out" == "a model "* ]] && [ -L "$x" ] && [ "$(ls "$o/prefix" | wc -l | tr -d " ")" = 2 ] && state_is available ""' \
    "a link at a prefix folder: not entered, not written through: got \"$out\""
  seed model-link; mv "$loc/$sha" "$o/model"; ln -s "$o/model" "$loc/$sha"
  out="$(outcome "$m" a sleep | tr '\n' ' ')"
  expect '[[ "$out" == "a model "* ]] && [ -L "$loc/$sha" ] && [ "$(find "$o/model" -type f | wc -l | tr -d " ")" = 4 ]' "a link at the model folder: not entered: got \"$out\""
  seed upper; mv "$loc/$sha" "$loc/x"; mv "$loc/x" "$loc/$(tr a-f A-F <<< "$sha")"
  out="$(outcome "$m" a d sleep | tr '\n' ' ')"
  expect '[[ "$out" == "a model d model "* ]] && [ "$(find "$loc" -type f -name "*.flac" | wc -l | tr -d " ")" = 2 ] && has "only by ignoring case"' \
    "an upper-case model folder: not served, not written, left alone: got \"$out\""
  config "cache_dir=\"$loc\"\ncache_id=$ID\nmax_days=1\n"; touch -t "$(date -v-40d +%Y%m%d%H%M)" "$fa"
  outcome "$m" sleep > /dev/null
  expect '[ -f "$(find "$loc" -iname "$ka.flac")" ]' "an upper-case model folder: never pruned"
  seed zeros; head -c "$(stat -f %z "$fa")" /dev/zero > "$fa.z"; mv "$fa.z" "$fa"
  out="$(outcome "$m" a wait a | tr '\n' ' ')"
  expect '[ "$out" = "a model stored 2 a cache " ]' "a zero-filled .flac: a miss, rewritten, then a hit: got \"$out\""
  seed truncated; head -c 10 "$x/$ka.desc" > "$x/d.z"; mv "$x/d.z" "$x/$ka.desc"
  out="$(outcome "$m" b a sleep | tr '\n' ' ')"
  expect '[ "$out" = "b model a cache stored 3 " ] && [ "$(head -c 4 "$x/$ka.desc")" = RBD2 ] && [ "$(stat -f %z "$x/$ka.desc")" -gt 10 ]' \
    "a truncated .desc: no near hit; the exact hit rewrites it: got \"$out\""

  # a forged .desc can't loosen the near-hit bars: its residuals are capped at -10 dB
  seed forged-desc
  python3 -I - "$x/$ka.desc" << 'EOF'
import struct, sys
p = sys.argv[1]; b = bytearray(open(p, "rb").read()); fr = struct.unpack_from("<i", b, 4)[0]; n = 2 * fr; off = 16 + 4 * n
struct.pack_into("<2f", b, off, 50.0, 50.0)
struct.pack_into("<%df" % n, b, off + 8, *([50.0] * n))
open(p, "wb").write(b)
EOF
  out="$(outcome "$m" b | tr '\n' ' ')"
  expect '[ "$out" = "b cache " ] && has "(cached -10.0 -10.0, "' "a forged .desc with loose bars: capped at -10 dB: got \"$out\""

  # an exact hit whose stems don't add up to the chunk (d's stems under a's key) is refused
  seed garbage
  python3 -I - "$fd" "$fa" "$(basename "$fd" .flac)" "$ka" << 'EOF'
import sys
src, dst, kd, ka = sys.argv[1:]
b = open(src, "rb").read()
assert b.count(kd.encode()) == 1
open(dst, "wb").write(b.replace(kd.encode(), ka.encode()))
EOF
  out="$(outcome "$m" a wait a | tr '\n' ' ')"
  expect '[ "$out" = "a model stored 2 a cache " ] && has "refused: its stems don'"'"'t add up"' "an exact hit with someone else's stems: refused, a miss, rewritten: got \"$out\""

  # a track's silent end: an exact hit isn't judged under -60 dBFS
  fresh ent-quiet; anyvol=1; rm -rf "$d"; chosen_at "$d"
  local qd before after
  out="$(outcome "$m" gain=0.0001 a wait | tr '\n' ' ')$(outcome "$m" gain=0.0001 a sleep | tr '\n' ' ')"
  qd="$(find "$loc" -name "*.desc" ! -name "._*" | head -1)"; before="$(stat -f '%i %m' "$qd")"
  out="$out$(outcome "$m" gain=0.0001 a sleep | tr '\n' ' ')"; after="$(stat -f '%i %m' "$qd")"
  expect '[ "$out" = "a model stored 1 a cache stored 1 a cache stored 1 " ] && ! has refused' "a silent chunk from a chosen location: still a hit: got \"$out\""
  expect '[ "$before" = "$after" ]' "a silent chunk's summary isn't rewritten on every hit ($before, then $after)"
}

# ---------------------------------------------------------------- losing it

losses() {
  if [ -z "$have_model" ]; then skip "losing a chosen location (need Pioneer's library and python3)"; return; fi
  point "$real"
  local m="$scratch/model/hdemucs.onnx" d="$scratch/drive-$arch" out want fa t="$scratch/t-$arch"

  # live recovery: an unavailable location is checked again (every RBSTEMS_TEST_RECHECK_SECONDS
  # here, RECHECK_EVERY in use) and used again once it passes, without a restart
  fresh gone; anyvol=1; rm -rf "$d" "$d.away"; chosen_at "$d"
  out="$(RBSTEMS_TEST_RECHECK_SECONDS=1 outcome "$m" a wait "sh=mv '$d' '$d.away'" a d sleep "sh=[ ! -e '$d' ]" "sh=mv '$d.away' '$d'" "sh=sleep 3" a d wait | tr '\n' ' ')"
  want="a model stored 1 a model d model stored 0 a cache d model stored 2 "
  expect '[ "$out" = "$want" ]' "the folder goes mid-run: misses, nothing recreated; back in place, used again without a restart: got \"$out\", want \"$want\""
  expect 'state_is available "" && [ "$(count "not available (")" = 1 ] && [ "$(count "cache: back: using $loc (")" = 1 ] && [ "$(count "limits: ")" -ge 2 ]' \
    "the loss and the return are logged once each, the space counted again, bridge-state says available"
  fresh gone-slow; anyvol=1; rm -rf "$d" "$d.away"; chosen_at "$d"
  out="$(outcome "$m" a wait "sh=mv '$d' '$d.away'" a "sh=mv '$d.away' '$d'" a | tr '\n' ' ')"
  expect '[ "$out" = "a model stored 1 a model a model " ] && state_is unavailable folder' "back at once: not used before the next check (every 10 s): got \"$out\""
  out="$(outcome "$m" a | tr '\n' ' ')"
  expect '[ "$out" = "a cache " ] && state_is available ""' "a new process uses it: got \"$out\""

  fresh missing-at-launch; anyvol=1; rm -rf "$d" "$d.away"; chosen_at "$d"; mv "$d" "$d.away"
  out="$(RBSTEMS_TEST_RECHECK_SECONDS=1 outcome "$m" a "sh=mv '$d.away' '$d'" "sh=sleep 3" a wait a | tr '\n' ' ')"
  expect '[ "$out" = "a model a model stored 1 a cache " ] && state_is available "" && has "cache: back: using $loc ("' \
    "the folder missing when rekordbox opens, then there: used from the next check on, no restart: got \"$out\""

  fresh same-name; anyvol=1; rm -rf "$d" "$d.away"; chosen_at "$d"
  out="$(outcome "$m" a wait "sh=mv '$d' '$d.away' && cp -R '$d.away' '$d'" a | tr '\n' ' ')"
  expect '[ "$out" = "a model stored 1 a cache " ] && [ "$(count "not available (")" = 0 ] && has "cache: checked again: using $loc (" && state_is available ""' \
    "another folder at the same place, same marker: checked at once like at launch (not the folder pinned) and used: got \"$out\""

  fresh other-drive; anyvol=1; rm -rf "$d" "$d.away"; chosen_at "$d"
  out="$(RBSTEMS_TEST_RECHECK_SECONDS=1 outcome "$m" a wait "sh=mv '$d' '$d.away' && mkdir -p '$loc' && printf 'id=$OTHER_ID\nformat=rbstems-v1\n' > '$loc/.rbstemsplus-cache'" a "sh=sleep 3" a d sleep | tr '\n' ' ')"
  expect '[ "$out" = "a model stored 1 a model a model d model stored 0 " ] && state_is unavailable marker && [ "$(ls -A "$loc")" = .rbstemsplus-cache ] && has "is still unavailable, now because its folder'"'"'s marker"' \
    "another location's folder at the same place (another marker): never used, nothing written, the new reason logged: got \"$out\""

  fresh miss-ok; anyvol=1; rm -rf "$d"; chosen_at "$d"
  touch "$t"; sleep 1; outcome "$m" a wait > /dev/null; fa="$(newest_flac "$loc" "$t")"
  rm "$fa"
  out="$(outcome "$m" b d wait a wait | tr '\n' ' ')"
  expect '[ "$out" = "b model d model stored 2 a cache stored 2 " ] && state_is available "" && ! has "not available"' \
    "a near candidate gone, misses: the location stays available (ENOENT below it is a miss; a is then a near hit on b): got \"$out\""

  fresh cleared; anyvol=1; rm -rf "$d"; chosen_at "$d"
  outcome "$m" a wait > /dev/null
  find "$loc" -mindepth 1 -maxdepth 1 -type d -exec rm -rf {} +
  out="$(outcome "$m" a wait a | tr '\n' ' ')"
  expect '[ "$out" = "a model stored 1 a cache " ] && [ -f "$loc/.rbstemsplus-cache" ]' "after Clear (the folder and marker kept): a miss writes, then a hit: got \"$out\""

  fresh busy; anyvol=1; rm -rf "$d"; chosen_at "$d"; wdelay=1
  out="$(outcome "$m" a c d e f g | tr '\n' ' ')"
  expect '[ "$(count "miss ")" = 6 ] && [ "$(count "chunks are waiting to be saved")" = 1 ]' "the writer queue is bounded: past 4 chunks new ones aren't saved, logged once"

  # the index's ceiling: 1000 entries for a 0.1 GB limit
  fresh ceiling; anyvol=1; rm -rf "$d"; chosen_at "$d"
  config "cache_dir=\"$loc\"\ncache_id=$ID\nmax_gb=0.1\n"
  touch "$t"; sleep 1; outcome "$m" a wait > /dev/null; fa="$(newest_flac "$loc" "$t")"
  python3 -I - "${fa%.flac}.desc" << 'EOF'
import os, sys, hashlib
src = sys.argv[1]; b = open(src, "rb").read(); model = os.path.dirname(os.path.dirname(src))
for i in range(1100):
    k = hashlib.sha256(str(i).encode()).hexdigest()
    os.makedirs(os.path.join(model, k[:2]), exist_ok=True)
    open(os.path.join(model, k[:2], k + ".desc"), "wb").write(b)
EOF
  out="$(outcome "$m" a | tr '\n' ' ')"
  expect '[ "$out" = "a cache " ] && has "summaries: stopped at 1000" && has "summaries loaded: 1000 cached chunks"' "the index stops at its ceiling; exact hits still work: got \"$out\""
}

# ---------------------------------------------------------------- companions (exFAT's, FAT32's "._" files)

companions() {
  if [ -z "$have_model" ]; then skip "companion files (need Pioneer's library and python3)"; return; fi
  point "$real"
  local m="$scratch/model/hdemucs.onnx" d="$scratch/drive-$arch" fa ka x sha K2 old young f gone=() kept=()
  sha="$(shasum -a 256 "$m" | cut -d' ' -f1)" K2="ab$(printf '7%.0s' {1..62})"
  old="$(date -v-2H +%Y%m%d%H%M)" young="$(date +%Y%m%d%H%M)"
  fresh comp; anyvol=1; rm -rf "$d"; chosen_at "$d"
  outcome "$m" a wait > /dev/null
  fa="$(find "$loc" -name "*.flac" | head -1)" ka="$(basename "$fa" .flac)" x="$(dirname "$fa")"
  mkdir -p "$loc/$sha/ab"
  local keep=("$x/._$ka.flac" "$x/._$ka.desc" "$loc/$sha/._${ka:0:2}" "$loc/._$sha" "$loc/._.rbstemsplus-cache" "$loc/$sha/ab/._$K2.desc.young")
  local drop=("$loc/$sha/ab/._$K2.flac" "$loc/$sha/ab/._$K2.desc" "$loc/$sha/._cd" "$loc/._$(printf 'f%.0s' {1..64})" "$x/._$ka.flac.123.tmp")
  for f in "${keep[@]}" "${drop[@]}"; do head -c 4096 /dev/urandom > "$f"; touch -t "$old" "$f"; done
  mv "$loc/$sha/ab/._$K2.desc.young" "$loc/$sha/ab/._${K2/ab7/ab8}.desc"; touch -t "$young" "$loc/$sha/ab/._${K2/ab7/ab8}.desc"
  keep[5]="$loc/$sha/ab/._${K2/ab7/ab8}.desc"
  config "cache_dir=\"$loc\"\ncache_id=$ID\nmax_days=30\n"
  outcome "$m" sleep > /dev/null
  for f in "${keep[@]}"; do [ -f "$f" ] || gone+=("${f#$loc/}"); done
  for f in "${drop[@]}"; do [ ! -e "$f" ] || kept+=("${f#$loc/}"); done
  expect '[ ${#gone[@]} = 0 ]' "companions of what is there (and young orphans, and the marker's) are kept: gone ${gone[*]:-}"
  expect '[ ${#kept[@]} = 0 ]' "orphan companions over an hour old are deleted: left ${kept[*]:-}"
  touch -t "$(date -v-40d +%Y%m%d%H%M)" "$fa"
  outcome "$m" sleep > /dev/null
  expect '[ ! -e "$fa" ] && [ ! -e "$x/$ka.desc" ] && [ ! -e "$x/._$ka.flac" ] && [ ! -e "$x/._$ka.desc" ] && has "removed 1 unused for 30 days"' \
    "an entry unused for max_days goes with its companions"
}

# ---------------------------------------------------------------- real volumes (disk images)

images_ok=
attach_images() {
  [ -n "$have_model" ] || return
  local v name fs args mp img
  for v in "exfat|ExFAT|" "fat32|MS-DOS FAT32|" "apfs|APFS|" "hfs|HFS+|" "owners|APFS|-owners on"; do
    IFS='|' read -r name fs args <<< "$v"
    img="$scratch/img-$name.dmg" mp="$scratch/vol-$name"
    mkdir -p "$mp"
    if ! hdiutil create -quiet -size 300m -fs "$fs" -volname "RBT$(tr a-z A-Z <<< "${name:0:4}")" "$img" > /dev/null 2>&1 \
       || ! hdiutil attach -quiet -nobrowse $args -mountpoint "$mp" "$img" > /dev/null 2>&1; then
      echo "  disk images: hdiutil couldn't make or attach a $fs image"
      return
    fi
    mounts+=("$mp")
  done
  images_ok=1
}

images() {
  if [ -z "$images_ok" ]; then skip "chosen locations on disk images (need hdiutil, Pioneer's library and python3)"; return; fi
  point "$real"
  local m="$scratch/model/hdemucs.onnx" name mp out fa ka x
  for name in exfat fat32 apfs hfs owners; do
    mp="$scratch/vol-$name"
    fresh "img-$name"; rm -rf "$mp/DJ-$arch"; chosen_at "$mp/DJ-$arch"
    out="$(outcome "$m" a wait a b d wait | tr '\n' ' ')"
    expect '[ "$out" = "a model stored 1 a cache b cache d model stored 2 " ] && state_is available "" && has "cache: using $loc ("' \
      "$name: a chosen folder on the volume: miss, hit, near: got \"$out\""
    fa="$(find "$loc" -name "*.flac" ! -name "._*" | head -1)" ka="$(basename "$fa" .flac)" x="$(dirname "$fa")"
    case $name in
      exfat|fat32) expect '[ -f "$x/._$ka.flac" ] && [ "$(find "$loc" -name "._*.tmp" | wc -l | tr -d " ")" = 0 ]' "$name: entries have companions, temp files' don't stay" ;;
    esac
    config "cache_dir=\"$loc\"\ncache_id=$ID\nmax_days=30\n"
    touch -t "$(date -v-40d +%Y%m%d%H%M)" "$fa"
    outcome "$m" sleep > /dev/null
    expect '[ ! -e "$fa" ] && [ ! -e "$x/._$ka.flac" ] && has "removed 1 unused for 30 days" && ! has ERROR' "$name: the limits work, companions included"
  done

  mp="$scratch/vol-exfat"
  fresh img-detach; chosen_at "$mp/DJ-$arch-detach"
  out="$(RBSTEMS_TEST_RECHECK_SECONDS=1 outcome "$m" a wait "sh=hdiutil detach -quiet -force '$mp'" a d sleep "sh=[ ! -e '$loc' ]" "sh=sleep 2" \
         "sh=hdiutil attach -quiet -nobrowse -mountpoint '$mp' '$scratch/img-exfat.dmg' > /dev/null" "sh=sleep 3" a | tr '\n' ' ')"
  mount | grep -q " on $mp (" || hdiutil attach -quiet -nobrowse -mountpoint "$mp" "$scratch/img-exfat.dmg" > /dev/null 2>&1
  expect '[ "$out" = "a model stored 1 a model d model stored 0 a cache " ] && state_is available "" && has "cache: back: using $loc ("' \
    "exfat detached mid-run: misses, nothing recreated; attached again, used again without a restart: got \"$out\""
  out="$(outcome "$m" a | tr '\n' ' ')"
  expect '[ "$out" = "a cache " ]' "exfat attached again, a new process: hits: got \"$out\""
  # unplugged and plugged in again with no lookup meanwhile (the drive comes back with new device
  # numbers; an image attached again would get its old disk number back, so a spacer image takes
  # that one meanwhile): the next lookup checks it at once and hits, without waiting for a recheck
  fresh img-replug; loc="$mp/DJ-$arch-detach/$FOLDER"; hcache="$loc"; config "cache_dir=\"$loc\"\ncache_id=$ID\n"
  local spacer="$scratch/img-spacer.dmg" sdev
  [ -e "$spacer" ] || hdiutil create -quiet -size 1m -layout NONE "$spacer"
  out="$(outcome "$m" a "sh=hdiutil detach -quiet -force '$mp' && hdiutil attach -nomount '$spacer' > '$scratch/spacer-dev' && hdiutil attach -quiet -nobrowse -mountpoint '$mp' '$scratch/img-exfat.dmg' > /dev/null" a | tr '\n' ' ')"
  sdev="$(awk 'NR==1{print $1}' "$scratch/spacer-dev" 2>/dev/null)"; [ -n "$sdev" ] && hdiutil detach -quiet "$sdev"
  mount | grep -q " on $mp (" || hdiutil attach -quiet -nobrowse -mountpoint "$mp" "$scratch/img-exfat.dmg" > /dev/null 2>&1
  expect '[ "$out" = "a cache a cache " ] && state_is available "" && [ "$(count "not available (")" = 0 ] && has "cache: checked again: using $loc ("' \
    "exfat detached and attached again between two lookups: the next one hits at once, never unavailable: got \"$out\""
  hdiutil detach -quiet -force "$mp" && hdiutil attach -quiet -nobrowse -readonly -mountpoint "$mp" "$scratch/img-exfat.dmg" > /dev/null 2>&1
  fresh img-readonly; loc="$mp/DJ-$arch-detach/$FOLDER"; hcache="$loc"; config "cache_dir=\"$loc\"\ncache_id=$ID\n"
  out="$(RBSTEMS_TEST_RECHECK_SECONDS=1 outcome "$m" a sleep "sh=grep -qx state=unavailable '$(statef)' && grep -qx reason=read-only '$(statef)' || echo NOT-READ-ONLY" \
         "sh=hdiutil detach -quiet -force '$mp' && hdiutil attach -quiet -nobrowse -mountpoint '$mp' '$scratch/img-exfat.dmg' > /dev/null" "sh=sleep 3" a | tr '\n' ' ')"
  mount | grep -q " on $mp (" || hdiutil attach -quiet -nobrowse -mountpoint "$mp" "$scratch/img-exfat.dmg" > /dev/null 2>&1
  expect '[ "$out" = "a model stored 1 a cache " ] && state_is available ""' \
    "exfat attached read-only: unavailable (read-only), not served; mounted read-write again, used without a restart: got \"$out\""
}

# ---------------------------------------------------------------- faults at chosen moments (test hooks)

faults() {
  if [ -z "$have_model" ]; then skip "faults in a chosen location (need Pioneer's library and python3)"; return; fi
  point "$real"
  local m="$scratch/model/hdemucs.onnx" d="$scratch/drive-$arch" out total sum
  hx() { RBSTEMS_HARNESS_WAIT_FOR="Demucs session opened" outcome "$@"; }   # chunks before the index (or the check) is done

  # pending: while the location is being checked, rekordbox's chunks pass through, nothing is read or written
  fresh pending; anyvol=1; rm -rf "$d"; chosen_at "$d"
  out="$(RBSTEMS_TEST_VALIDATE_DELAY=3 hx "$m" a "sh=sleep 5" a wait | tr '\n' ' ')"
  expect '[ "$out" = "a model a model stored 1 " ] && [ "$(count "miss ")" = 1 ]' "pending: the first chunk passes through untouched, the next one (checked) is cached: got \"$out\""

  # an open that fails for want of descriptors is a miss, never a loss
  fresh emfile; anyvol=1; rm -rf "$d"; chosen_at "$d"
  outcome "$m" a wait > /dev/null
  out="$(RBSTEMS_TEST_HIT_OPEN_ERRNO=24 outcome "$m" a a | tr '\n' ' ')"
  expect '[[ "$out" == "a "*" a cache " ]] && state_is available "" && ! has "not available"' "EMFILE opening the folder: that chunk only, the location stays: got \"$out\""

  # write failures: ENOSPC is that entry only; EROFS means the drive turned read-only (a loss)
  fresh enospc; anyvol=1; rm -rf "$d"; chosen_at "$d"
  out="$(RBSTEMS_TEST_WRITE_ERRNO=28 outcome "$m" a sleep d sleep | tr '\n' ' ')"
  expect '[ "$out" = "a model stored 0 d model stored 0 " ] && has "No space left on device" && state_is available ""' "ENOSPC on a write: not saved, the location stays: got \"$out\""
  fresh erofs; anyvol=1; rm -rf "$d"; chosen_at "$d"
  out="$(RBSTEMS_TEST_WRITE_ERRNO=30 outcome "$m" a sleep d sleep | tr '\n' ' ')"
  expect '[ "$out" = "a model stored 0 d model stored 0 " ] && state_is unavailable read-only && [ "$(count "not available (")" = 1 ]' "EROFS on a write: the location is lost (read-only) until it's checked again: got \"$out\""

  # Stems Cache off: the chosen drive isn't touched at all
  fresh disabled; anyvol=1; rm -rf "$d"; chosen_at "$d"; config "cache_dir=\"$loc\"\ncache_id=$ID\nenabled=0\n"; rm "$loc/.rbstemsplus-cache"
  out="$(outcome "$m" a | tr '\n' ' ')"
  expect '[ "$out" = "a model " ] && state_is unavailable disabled && ! has "not available (" && [ -z "$(ls -A "$loc")" ]' "enabled=0: the drive is never checked, bridge-state says disabled: got \"$out\""

  # the total the limits count is the disk space of the entries' files (entry_size), companions included
  fresh recount; anyvol=1; rm -rf "$d"; chosen_at "$d"
  outcome "$m" a c d e wait > /dev/null
  head -c 5000 /dev/urandom > "$(dirname "$(find "$loc" -name "*.flac" | head -1)")/._$(basename "$(find "$loc" -name "*.flac" | head -1)")"
  outcome "$m" sleep > /dev/null
  total="$(grep -o '([0-9]* bytes on disk)' "$(logf)" | tail -1 | tr -dc 0-9)"
  sum="$(python3 -I -c '
import os, sys, re
t = 0
for r, ds, fs in os.walk(sys.argv[1]):
    for f in fs:
        if re.fullmatch(r"(\._)?[0-9a-f]{64}\.(flac|desc)", f): t += os.lstat(os.path.join(r, f)).st_blocks * 512
print(t)' "$loc")"
  expect '[ -n "$total" ] && [ "$total" = "$sum" ]' "the limits' total equals the entries' disk space, companions included ($total, counted $sum)"
}

# a drive detached in the middle of a write, a prune and an index load (the exFAT image)
midway() {
  if [ -z "$images_ok" ]; then skip "detaching mid-operation (need hdiutil, Pioneer's library and python3)"; return; fi
  point "$real"
  local m="$scratch/model/hdemucs.onnx" mp="$scratch/vol-exfat" img="$scratch/img-exfat.dmg" out v
  reattach() { hdiutil attach -quiet -nobrowse -mountpoint "$mp" "$img" > /dev/null 2>&1; }
  fresh mid-write; chosen_at "$mp/DJ-$arch-mid"
  out="$(RBSTEMS_TEST_WRITE_DELAY=3 outcome "$m" a "sh=sleep 1" "sh=hdiutil detach -quiet -force '$mp'" "sh=sleep 4" d sleep | tr '\n' ' ')"
  reattach
  expect '[[ "$out" == "a model "*"d model "* ]] && state_is unavailable not-connected && [ "$(count "not available (")" = 1 ]' "detached mid-write: no crash, the location lost once: got \"$out\""
  out="$(outcome "$m" a wait | tr '\n' ' ')"
  expect '[ "$out" = "a model stored 1 " ] && state_is available ""' "after a detach mid-write, a new run saves again: got \"$out\""
  for v in prune index; do
    fresh "mid-$v"; chosen_at "$mp/DJ-$arch-mid"
    if [ $v = prune ]; then out="$(RBSTEMS_TEST_PRUNE_DELAY=3 outcome "$m" "sh=sleep 1" "sh=hdiutil detach -quiet -force '$mp'" "sh=sleep 4" a | tr '\n' ' ')"
    else out="$(RBSTEMS_TEST_INDEX_DELAY=3 RBSTEMS_HARNESS_WAIT_FOR="Demucs session opened" outcome "$m" "sh=sleep 1" "sh=hdiutil detach -quiet -force '$mp'" "sh=sleep 4" a | tr '\n' ' ')"; fi
    reattach
    expect '[ "$out" = "a model " ] && grep -qx "state=unavailable" "$(statef)" && ! has "removed 1 unused"' "detached mid-$v: no crash, nothing evicted, the location lost: got \"$out\""
    out="$(outcome "$m" a | tr '\n' ' ')"
    expect '[ "$out" = "a cache " ] && state_is available ""' "after a detach mid-$v, a new run hits: got \"$out\""
  done
}

# ---------------------------------------------------------------- near hits off (the default)

# Without near_hits=1 only exact hits are served: no index is loaded, no candidate is read before a
# miss, a hit never reads the entry's .desc; the .desc files are still written (for a future
# pre-cache) and the limits still run at launch. near_hits=1 turns near hits on.
nearoff() {
  if [ -z "$have_model" ]; then skip "near hits off (needs Pioneer's library and python3)"; return; fi
  point "$real"
  local m="$scratch/model/hdemucs.onnx" c out d="$scratch/drive-$arch" fa
  fresh nearoff; near=; c="$h/Library/Caches/rbstemsplus"
  out="$(outcome "$m" a wait a b c d wait | tr '\n' ' ')"
  expect '[ "$out" = "a model stored 1 a cache b model c model d model stored 4 " ]' "near hits off: b (a's chunk shifted) is separated, not served from a: got \"$out\""
  expect 'has "near hits off (near_hits=0)" && has "summaries not loaded: near hits are off" && [ "$(grep -c "near [0-9a-f]* \(for\|rejected\)" "$(logf)")" = 0 ]' \
    "near hits off: logged at load, no index, no candidate checked"
  expect '[ "$(find "$c" -name "*.desc" | wc -l | tr -d " ")" = 4 ]' "near hits off: the .desc files are still written"
  fa="$(find "$c" -name "*.flac" | head -1)"
  rm "${fa%.flac}.desc"; mkfifo "${fa%.flac}.desc"
  config 'max_days=30\n'
  touch -t "$(date -v-40d +%Y%m%d%H%M)" "$(find "$c" -name "*.flac" | sed -n 2p)"
  out="$(outcome "$m" a b sleep | tr '\n' ' ')"
  expect '[[ "$out" == *"cache"* ]] && [ -p "${fa%.flac}.desc" ] && has "removed 1 unused for 30 days"' \
    "near hits off: a hit never reads its .desc (a FIFO there stays), and the limits still run at launch: got \"$out\""
  config 'near_hits=1\n'
  out="$(outcome "$m" b | tr '\n' ' ')"
  expect '[ "$out" = "b cache" ] || [ "$out" = "b cache " ]' "near_hits=1 in config.ini turns them on: got \"$out\""
  fresh nearoff-drive; near=; anyvol=1; rm -rf "$d"; chosen_at "$d"
  out="$(outcome "$m" a wait a b | tr '\n' ' ')"
  expect '[ "$out" = "a model stored 1 a cache b model " ] && has "summaries not loaded: near hits are off" && state_is available ""' \
    "near hits off on a drive: exact hits only, no index read from the drive: got \"$out\""
}

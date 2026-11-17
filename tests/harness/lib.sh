# The harness driver's verbs, sourced by tests/harness/run.sh in the tart VM (as root, over SSH).
# OS-neutral verbs, this file the Mac's driver (the private Windows plan's mac-core §2.3; the Windows
# driver is app.md §7's). The app runs in the logged-in user's GUI session; the OS-level verbs
# (launch, activation, Finder to the front) come from outside the app's process, the in-window
# verbs (clicks, tabs, sheet answers) go through the probe's command FIFO (app/Probe/Probe.swift).
#
# Scenario files (tests/harness/scenarios/*.sh) define setup (the facts before launch) and steps,
# and use only the verbs below.

H=/Users/Shared/rbsp-harness
APP="/Applications/RB Stems Plus.app"
BID=io.github.rbstemsplus.app
U="${HARNESS_USER:-dj}"
UID_="$(id -u "$U")"
UH="/Users/$U"
SUP="$UH/Library/Application Support/rbstemsplus"
LOGS="$UH/Library/Logs/rbstemsplus"
RB="/Applications/rekordbox 7/rekordbox.app"
RB_MOVED="/Users/Shared/rbsp-harness/rekordbox 7 (moved)"     # rekordbox-missing moves it here
RBLIB="$RB/Contents/Frameworks/libonnxruntime.1.18.0.dylib"
MODELDIR="$UH/Library/Application Support/Pioneer/rekordbox6/models/demucs3_model"
ROOTDIR="/Library/Application Support/rbstemsplus"
OUT="$H/out"
FAKE="$H/fake"
CONF_DIR="$UH/Library/Application Support/rbstemsplus-probe"
N=0
FAILS=0
MODE="${MODE:-trace}"
VERB="${VERB:-reopen}"

say() { echo "[$(date +%H:%M:%S)] $*" >&2; }
fail() { say "FAIL: $*"; FAILS=$((FAILS + 1)); echo "FAIL $*" >> "$RUN/result"; }
check() { if eval "$1"; then echo "ok $2" >> "$RUN/result"; else fail "$2"; fi; }
as_user() { launchctl asuser "$UID_" sudo -u "$U" -H -- "$@"; }
own() { chown -R "$U":staff "$@"; }

# ---- the app's state before launch (facts) -------------------------------------------------

# Everything back to the template's state: no app running, no folder of ours, rekordbox as
# Pioneer's installer left it (its pristine files were saved by `run.sh prepare-vm`).
reset_all() {
  pkill -x "RB Stems Plus" 2>/dev/null; pkill -x "RB Stems Plus Watcher" 2>/dev/null; pkill -f "rbsp-harness/fake-rekordbox" 2>/dev/null
  for _ in 1 2 3 4 5 6 7 8 9 10; do pgrep -x "RB Stems Plus" >/dev/null || break; sleep 0.3; done
  pkill -9 -x "RB Stems Plus" 2>/dev/null
  launchctl bootout "gui/$UID_/io.github.rbstemsplus.watcher" 2>/dev/null
  launchctl enable "gui/$UID_/io.github.rbstemsplus.watcher" 2>/dev/null     # watcher-off-line turns it off
  rm -f "$UH/Library/LaunchAgents/io.github.rbstemsplus.watcher.plist"
  rm -rf "$SUP" "$LOGS" "$UH/Library/Caches/rbstemsplus" "$UH/Library/Caches/rbsp-elsewhere" "$ROOTDIR" /etc/sudoers.d/rbsp-harness
  rm -rf "$UH/Desktop/RB Stems Plus report "*.zip
  # a stick an aborted USB scenario left mounted is unmounted (the next scenario meets none); a
  # mount point a pulled stick left is removed
  if stick_mounted; then diskutil unmount force "$STICKDIR" >/dev/null 2>&1 || true; fi
  stick_gone
  # rekordbox moved away (rekordbox-missing) goes back first
  if [ ! -e "/Applications/rekordbox 7" ] && [ -d "$RB_MOVED" ]; then mv "$RB_MOVED" "/Applications/rekordbox 7"; fi
  cp -p "$H/pristine/libonnxruntime.1.18.0.dylib" "$RBLIB"
  cp -p "$H/pristine/Info.plist" "$RB/Contents/Info.plist"
  # what a real Stems Cache install re-signs (install-cache-flow), only if it differs
  [ -f "$H/pristine/rekordbox" ] && ! cmp -s "$H/pristine/rekordbox" "$RB/Contents/MacOS/rekordbox" && cp -p "$H/pristine/rekordbox" "$RB/Contents/MacOS/rekordbox"
  [ -f "$H/pristine/CodeResources" ] && ! cmp -s "$H/pristine/CodeResources" "$RB/Contents/_CodeSignature/CodeResources" && cp -p "$H/pristine/CodeResources" "$RB/Contents/_CodeSignature/CodeResources"
  # macOS's privacy grants as a first launch meets them: App Management refused once (the state it
  # leaves after blocking), the Desktop and removable volumes allowed (fact_files_allowed)
  tcc_set kTCCServiceSystemPolicyAppBundles 0
  fact_files_allowed
  rm -rf "$MODELDIR" && ditto "$H/pristine/demucs3_model" "$MODELDIR" && own "$MODELDIR"
  rm -rf "$FAKE/script" "$FAKE/log" "$OUT"
  mkdir -p "$FAKE/script" "$FAKE/log" "$OUT" "$CONF_DIR"
  own "$FAKE" "$OUT" "$CONF_DIR"
  printf 'mode=%s\nout=%s\nhelper=%s\n' "$MODE" "$OUT" "$FAKE/fake-prepare" > "$CONF_DIR/probe.conf"
  own "$CONF_DIR"
}

# RB Stems Plus's own folder with its first-launch welcome already seen.
fact_welcomed() { mkdir -p "$SUP" && touch "$SUP/welcomed" && own "$SUP"; }
# what the user chose to install (the "installed" file)
fact_chosen() { mkdir -p "$SUP" && printf 'model=%s\ncache=%s\n' "$1" "$2" > "$SUP/installed" && own "$SUP"; }
# config.ini (the bridge's settings): fact_config "enabled=1" "max_gb=20" …
fact_config() { mkdir -p "$SUP" && printf '%s\n' "$@" > "$SUP/config.ini" && own "$SUP"; }
# Demucs v4 in rekordbox's model folder, as the lights see it (its size; Models.swift checks the
# checksum only when it saves, replaces or puts back a model)
fact_plus_on() { rm -f "$MODELDIR/hdemucs.onnx" && mkfile -n 308572524 "$MODELDIR/hdemucs.onnx" && own "$MODELDIR"; }
# rekordbox's STEMS Engine version ("" removes it: not downloaded yet)
fact_engine() { if [ -z "$1" ]; then rm -f "$MODELDIR/demucs3_ver.txt"; else printf '%s\n' "$1" > "$MODELDIR/demucs3_ver.txt"; own "$MODELDIR"; fi; }
# Stems Cache in rekordbox: the real bridge of this build (in the shared folder) and the root
# copy of Pioneer's ONNX Runtime, as the install's root script leaves them (not re-signed: the
# scenarios never start rekordbox)
fact_cache_on() {
  cp "$H/bridge/libonnxruntime.1.18.0.dylib" "$RBLIB"
  mkdir -p "$ROOTDIR/ort" && cp -p "$H/pristine/libonnxruntime.1.18.0.dylib" "$ROOTDIR/ort/libonnxruntime.1.18.0.dylib"
}
# a prepared playlist: fact_kept ID DAY FILES…, kept until 1 Jan 2030 + DAY days (a fixed date:
# the Status tab shows it, and traces recorded on different days must match)
fact_kept() {
  local id="$1" days="$2"; shift 2
  mkdir -p "$SUP/kept"
  printf 'rbstemsplus-kept 1\nuntil %s\n' "$(( 1893456000 + days * 86400 ))" > "$SUP/kept/$id.kept"
  : > "$SUP/kept/$id.manifest"
  for f in "$@"; do printf 'file %s\n' "$f" >> "$SUP/kept/$id.manifest"; done
  own "$SUP"
}
# rekordbox's version as its Info.plist says (Compat.swift's gates read it), e.g. 7.3.0.0001
fact_rekordbox_version() { /usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString $1" "$RB/Contents/Info.plist"; }
# the busy marker an interrupted action leaves (its process gone): fact_busy ID LABEL
fact_busy() { mkdir -p "$SUP" && printf '99999\n1\n%s\n%s\n' "$1" "$2" > "$SUP/busy" && own "$SUP"; }
# the watcher's "Reinstall Now" request, written AGE seconds ago (default now)
fact_request() {
  mkdir -p "$SUP" && printf 'reinstall\n' > "$SUP/request" && own "$SUP"
  [ -n "${1:-}" ] && touch -t "$(date -v-"$1"S +%Y%m%d%H%M.%S)" "$SUP/request"
  true
}
# ~/Library/Caches/rbstemsplus as a link (cacheIsLink)
fact_cache_link() { mkdir -p "$UH/Library/Caches/rbsp-elsewhere" && ln -sfn "$UH/Library/Caches/rbsp-elsewhere" "$UH/Library/Caches/rbstemsplus" && chown -h "$U":staff "$UH/Library/Caches/rbstemsplus"; }
# a process that rekordboxOpen() takes for rekordbox (pgrep -f on its path), in the VM only
fact_rekordbox_open() { (exec -a "$RB/Contents/MacOS/rekordbox rbsp-harness/fake-rekordbox" /bin/sleep 3600) >/dev/null 2>&1 & }
fact_rekordbox_closed() { pkill -f "rbsp-harness/fake-rekordbox" 2>/dev/null; true; }
# sudo without a password for the user (the admin runs go through; no password sheet)
fact_sudo_nopasswd() { printf '%s ALL=(ALL) NOPASSWD: ALL\n' "$U" > /etc/sudoers.d/rbsp-harness; chmod 440 /etc/sudoers.d/rbsp-harness; }
fact_sudo_password() { rm -f /etc/sudoers.d/rbsp-harness; as_user sudo -k 2>/dev/null; true; }
# This app's privacy grant for a TCC service in DJ's TCC.db, as System Settings leaves it: 2 (the
# user's Allow), 0 (refused: App Management leaves this after blocking the app), none (no row).
# The row names the installed app's code (its designated requirement), as the user's Allow does.
# DJ's tccd is restarted so it reads the change. The VM only (root over SSH, which has Full Disk
# Access in the template).
tcc_set() {
  local svc="$1" v="$2" db="$UH/Library/Application Support/com.apple.TCC/TCC.db" req
  if [ "$v" = none ]; then
    sqlite3 "$db" "delete from access where service='$svc' and client='$BID'" || fail "tcc: $svc not removed"
  else
    codesign -d -r- "$APP" 2>&1 | sed -n 's/^\(# \)\{0,1\}designated => //p' | csreq -r- -b "$H/app.req" || { fail "tcc: no code requirement"; return 1; }
    req="$(xxd -p "$H/app.req" | tr -d '\n')"
    sqlite3 "$db" "insert or replace into access (service, client, client_type, auth_value, auth_reason, auth_version, csreq, flags, indirect_object_identifier) values ('$svc', '$BID', 0, $v, 3, 1, X'$req', 0, 'UNUSED')" || fail "tcc: $svc not set"
  fi
  pkill -u "$U" -x tccd 2>/dev/null; sleep 0.5; true
}
# App Management (Privacy & Security): allow | deny | none
fact_app_management() { case "$1" in allow) tcc_set kTCCServiceSystemPolicyAppBundles 2 ;; deny) tcc_set kTCCServiceSystemPolicyAppBundles 0 ;; *) tcc_set kTCCServiceSystemPolicyAppBundles none ;; esac; }
# Files and Folders: the Desktop (Create Report saves its zip there) and removable volumes (a chosen
# drive): allowed, so macOS's own prompt never comes up over the window during a scenario
fact_files_allowed() { tcc_set kTCCServiceSystemPolicyDesktopFolder 2; tcc_set kTCCServiceSystemPolicyRemovableVolumes 2; }
# This app's rows in DJ's TCC.db (the complete uninstall resets them: tccutil)
tcc_rows() { sqlite3 "$UH/Library/Application Support/com.apple.TCC/TCC.db" "select count(*) from access where client='$BID'"; }
# rekordbox's folder moved out of /Applications (reset_all puts it back)
fact_rekordbox_moved() { mv "/Applications/rekordbox 7" "$RB_MOVED"; }
fact_rekordbox_back() { [ -d "$RB_MOVED" ] && mv "$RB_MOVED" "/Applications/rekordbox 7"; true; }
# Pioneer's own model saved by this app (originals/<sha256>.onnx and its STEMS Engine version), as
# Install Demucs v4 leaves it
fact_original_saved() {
  local sha; sha="$(shasum -a 256 "$H/pristine/demucs3_model/hdemucs.onnx" | cut -d' ' -f1)"
  mkdir -p "$SUP/originals" && cp "$H/pristine/demucs3_model/hdemucs.onnx" "$SUP/originals/$sha.onnx"
  printf '0002\n' > "$SUP/originals/$sha.engine"; own "$SUP"
}
# the bridge's bridge-state for the rekordbox running now (fact_rekordbox_open first): STATE
# (pending | available | unavailable), REASON, LOCATION ("" this Mac, else the chosen folder);
# replaced by a rename, as the bridge does
fact_bridge_state() {
  local pid start; pid="$(pgrep -f "rbsp-harness/fake-rekordbox" | head -1)"
  [ -n "$pid" ] || { fail "bridge-state: no rekordbox running"; return 1; }
  start="$(( $("$H/bin/vmtool" pidstart "$pid") / 1000000 ))"
  mkdir -p "$LOGS" && own "$LOGS"
  printf 'pid=%s\nstart=%s\nlocation=%s\nstate=%s\nreason=%s\n' "$pid" "$start" "${3:-}" "$1" "$2" > "$LOGS/.bridge-state.tmp"
  chown "$U":staff "$LOGS/.bridge-state.tmp" && mv -f "$LOGS/.bridge-state.tmp" "$LOGS/bridge-state"
}
# the payload.json (and its signature) the scenario's server serves, saved as a launch before
# downloaded it
fact_saved_payload() {
  mkdir -p "$SUP/payload"
  curl -fsS -o "$SUP/payload/payload.json" "http://192.168.64.1:8765/payload.json" \
    && curl -fsS -o "$SUP/payload/payload.json.sig" "http://192.168.64.1:8765/payload.json.sig" || fail "no payload.json from the server"
  own "$SUP"
}
# the app's log as 30 minutes of a session leave it (Create Report masks it: P16)
fact_long_log() {
  mkdir -p "$LOGS"
  local i
  for i in $(seq 1 1800); do
    printf '2030-01-01 %02d:%02d:%02d  prepare: estimate of playlist %d: 3 tracks, /Users/%s/Music/Harness/track-%d.mp3 ready\n' $(( i / 3600 )) $(( i / 60 % 60 )) $(( i % 60 )) "$i" "$U" "$i"
  done > "$LOGS/app.log"
  own "$LOGS"
}

# ---- the USB stick (vm.sh up --usb; the scenario's header says "# usb: yes") -------------------

STICK=RBSPSTICK
STICKDIR="/Volumes/$STICK"
STICKCACHE="$STICKDIR/RB Stems Plus Cache.noindex"     # our folder there (Location.swift: locationFolderName)
USBREQ="${USBREQ:-/Volumes/My Shared Files/xfer/usbreq}"      # vm.sh run names a new one per scenario
# the stick's volume is mounted at STICKDIR (a folder there alone isn't the stick: the mount point
# a pulled stick may leave; stick_gone removes such a leftover)
stick_mounted() { mount | grep -q " on $STICKDIR ("; }
stick_gone() { stick_mounted || { [ -d "$STICKDIR" ] && rm -rf "$STICKDIR"; }; true; }
# usb attach|detach: the stick plugged in or pulled out (no eject first: as a user pulls it), the
# host's answer awaited, then its volume mounted or gone
usb() {
  local id="u$(date +%s)$RANDOM" i
  printf '%s\n' "$1" > "$USBREQ/$id.req"
  for i in $(seq 1 200); do [ -e "$USBREQ/$id.done" ] && break; sleep 0.1; done
  [ -e "$USBREQ/$id.done" ] || { fail "usb $1: the host didn't answer"; return 1; }
  for i in $(seq 1 300); do
    if [ "$1" = attach ]; then stick_mounted && return 0; else stick_mounted || return 0; fi
    sleep 0.1
  done
  fail "usb $1: $STICKDIR $([ "$1" = attach ] && echo "didn't mount" || echo "is still there")"; return 1
}
# Stems Cache's chosen location on the stick, as Settings' Change… leaves it: our folder with its
# marker (a fixed id: traces of different runs match), config.ini's cache_dir and cache_id
STICK_ID=6F1C2D3E-4A5B-4C6D-8E9F-0A1B2C3D4E5F
fixture_drive() {
  stick_gone; stick_mounted || usb attach || return 1
  mdutil -i off "$STICKDIR" >/dev/null 2>&1         # no Spotlight indexing on the stick meanwhile
  rm -rf "$STICKCACHE" "$STICKDIR/filler"
  mkdir -p "$STICKCACHE"
  printf 'id=%s\nformat=rbstems-v1\n' "$STICK_ID" > "$STICKCACHE/.rbstemsplus-cache"
  chown -R "$U":staff "$STICKCACHE"
  mkdir -p "$SUP"
  { [ -f "$SUP/config.ini" ] && grep -v '^cache_' "$SUP/config.ini"; printf 'cache_dir="%s"\ncache_id=%s\n' "$STICKCACHE" "$STICK_ID"; } > "$SUP/config.ini.new"
  mv "$SUP/config.ini.new" "$SUP/config.ini"; own "$SUP"
}
# the stick filled up to LEFT MB free (cache-paused); "fill 0" removes the filler
fill_stick() {
  rm -f "$STICKDIR/filler"
  [ "$1" = 0 ] && return 0
  local free; free="$(df -m "$STICKDIR" | awk 'NR == 2 {print $4}')"
  [ "$free" -gt "$1" ] && mkfile "$(( free - $1 ))m" "$STICKDIR/filler"
  true
}

# ---- more processes of the app -----------------------------------------------------------------
# the probe's records of every copy of the app this run started, the first launched first
# (by when each started its record, oldest first: pids wrap around)
traces() { ls -tUr "$OUT"/app-*.jsonl 2>/dev/null; }
# the kernel's start time (µs since 1970) of each copy of the app now running, into $RUN/pids.txt
# (P14 joins an update's two processes through it)
record_pids() { local p; for p in $(pgrep -x "RB Stems Plus"); do echo "$p $("$H/bin/vmtool" pidstart "$p")" >> "$RUN/pids.txt"; done; }

# ---- the fake helper ---------------------------------------------------------------------------

# helper NAME < lines: the fake's script for one call (fake-prepare.c's header names them)
helper() { cat > "$FAKE/script/$1.jsonl"; own "$FAKE/script"; }
# the scenario lets a script waiting on "@wait NAME" go on
helper_go() { touch "$FAKE/script/$1"; own "$FAKE/script"; }
# waits until a script reached "@touch NAME"
helper_reached() {
  local i
  for i in $(seq 1 "$(( ${2:-10} * 50 ))"); do [ -e "$FAKE/script/$1" ] && return 0; sleep 0.02; done
  fail "the fake helper didn't reach $1"; return 1
}
# waits until the fake helper was started N times with the script NAME (its argv log)
helper_calls() {
  local i
  for i in $(seq 1 "$(( ${3:-10} * 50 ))"); do
    [ "$(calls_of "$1")" -ge "$2" ] && return 0; sleep 0.02
  done
  fail "the fake helper wasn't started $2 times with $1"; return 1
}
calls_of() { local c; c="$(grep -c "	$1 <- " "$FAKE/log/argv.log" 2>/dev/null)"; echo "${c:-0}"; }
# check_gated G: the Prepare tab in front checks Friday again (Check Again), its estimate held at
# helper_go G1 (estimate_friday G); returns once the fake helper is waiting there
check_gated() {
  local n; n="$(calls_of estimate-2)"
  estimate_friday "$1"
  tab Prepare; settle
  click prepare.checkBtn
  helper_calls estimate-2 $((n + 1))
}
# the playlists a scenario's library holds: id, parent, name, kind, tracks
playlist_line() { printf '{"playlist":"%s","parent":"%s","name":"%s","kind":"%s","tracks":%s}\n' "$@"; }
# a track of the fixture library: always a local file (never a streaming track)
track() { printf '/Users/%s/Music/Harness/%s.mp3' "$U" "$1"; }
# The G1 API spike (perf-spike*.sh): spike_on asks a core-linked TEST build for its spike table
# (app/Spike); spike_scripts ID:ROWS… writes perf-playlist-5000's fake-helper scripts for each
# playlist, the estimate at 1 line/ms for IDs ending in 51 (stats.py's "paced"), else at full speed
# The spike starts at the scenario's first `mark settled` (after the launch and the activation,
# which then have only the app's window); a select or scroll before it is refused.
spike_on() {
  printf 'spike=1\n' >> "$CONF_DIR/probe.conf"; own "$CONF_DIR"
  grep -aq 'core spike' "$APP/Contents/MacOS/RB Stems Plus" || fail "not a TEST build with the core's spike (app/Spike)"
}
spike_scripts() {
  local p id n est end
  est='{"file":"/Users/'"$U"'/Music/Harness/big-{n}.mp3","result":"prepare","seconds":240,"entries":22,"gb":0.06,"missing":0}'
  end='{"estimate":5000,"ready":0,"prepare":5000,"skipped":0,"entries":110000,"runs":110000,"time_s":275000,"need_gb":300,"kept_gb":0,"share_gb":150,"max_gb":200,"cache_gb":1,"free_gb":30,"fits":false,"fit_files":2500,"max_gb_needed":400,"disk_ok":false,"asking":0,"folders":[]}'
  for p in "$@"; do
    id="${p%%:*}" n="${p#*:}"
    echo "@repeat $n {\"file\":\"/Users/$U/Music/Harness/big-{n}.mp3\"}" | helper "tracks-$id"
    case "$id" in
      *51) printf '@pace 1\n@repeat %s %s\n%s\n' "$n" "$est" "$end" | helper "estimate-$id" ;;
      *) printf '@repeat %s %s\n%s\n' "$n" "$est" "$end" | helper "estimate-$id" ;;
    esac
  done
}

# ---- the fixture library (local files only) ------------------------------------------------------

# Stems Cache installed and on, with rekordbox's own model: the Prepare tab can check playlists
fixture_prepare_ready() { fact_welcomed; fact_cache_on; fact_chosen 0 1; fact_config "rekordbox_model=1" "max_gb=20"; fact_engine 0002; }
# Sets (folder 1) > Friday (2, 3 tracks) and Late (5, 1 track); Warm-up (3, 2 tracks); Smart (4, intelligent)
fixture_library() {
  {
    playlist_line 1 "" "Sets" folder 0
    playlist_line 2 1 "Friday" playlist 3
    playlist_line 5 1 "Late" playlist 1
    playlist_line 3 "" "Warm-up" playlist 2
    playlist_line 4 "" "Smart" intelligent 0
  } | helper playlists
  for t in a b c; do printf '{"file":"%s"}\n' "$(track "friday-$t")"; done | helper tracks-2
  for t in a b; do printf '{"file":"%s"}\n' "$(track "warmup-$t")"; done | helper tracks-3
  printf '{"file":"%s"}\n' "$(track late-a)" | helper tracks-5
}
ESTIMATE_END='"skipped":0,"entries":85,"runs":45,"time_s":112,"need_gb":0.13,"kept_gb":0.12,"share_gb":15,"max_gb":20,"cache_gb":1.2,"free_gb":30,"max_gb_needed":0.4,"disk_ok":true,"asking":0,"folders":[]'
# estimate_friday [GATE]: Friday's estimate (1 ready, 2 to prepare); with GATE, each line waits
# for helper_go GATE1…GATE4 (every intermediate frame recorded in order)
estimate_friday() {
  local g="${1:-}"
  {
    echo "@pace 60"                      # two frames or more per line (exact comparisons, mac-core §2.3)
    [ -n "$g" ] && echo "@wait ${g}1"
    printf '{"file":"%s","result":"ready","entries":40,"gb":0.12}\n' "$(track friday-a)"
    [ -n "$g" ] && echo "@wait ${g}2"
    printf '{"file":"%s","result":"prepare","seconds":312,"entries":28,"gb":0.08,"missing":0}\n' "$(track friday-b)"
    [ -n "$g" ] && echo "@wait ${g}3"
    printf '{"file":"%s","result":"prepare","seconds":185,"entries":17,"gb":0.05,"missing":0}\n' "$(track friday-c)"
    [ -n "$g" ] && echo "@wait ${g}4"
    printf '{"estimate":3,"ready":1,"prepare":2,"fits":true,"fit_files":3,%s}\n' "$ESTIMATE_END"
  } | helper estimate-2
}
# run_friday GATE: Friday's run, each step waiting for helper_go GATE1…GATE5; SIGTERM (Stop) ends it
# with the set written and "stopped"
run_friday() {
  local g="$1"
  {
    echo "@pace 60"
    echo "@wait ${g}1"
    printf '{"at":0,"files":3,"chunk":0,"chunks":2}\n'
    printf '{"file":"%s","result":"prepared","written":0,"already":40,"silent":0,"refused":0}\n' "$(track friday-a)"
    echo "@wait ${g}2"
    printf '{"at":1,"files":3,"chunk":0,"chunks":2}\n{"at":1,"files":3,"chunk":1,"chunks":2}\n'
    printf '{"file":"%s","result":"prepared","written":28,"already":0,"silent":0,"refused":0}\n' "$(track friday-b)"
    echo "@wait ${g}3"
    printf '{"at":2,"files":3,"chunk":0,"chunks":2}\n'
    echo "@wait ${g}4"
    printf '{"at":2,"files":3,"chunk":1,"chunks":2}\n'
    printf '{"file":"%s","result":"prepared","written":17,"already":0,"silent":0,"refused":0}\n' "$(track friday-c)"
    echo "@wait ${g}5"
    printf '{"done":3,"runs":45,"run_s":110,"kept":"2","until":1800000000,"kept_gb":0.25}\n'
    echo "@onterm"
    printf '{"file":"%s","result":"stopped","written":3,"already":0,"silent":0,"refused":0}\n' "$(track friday-c)"
    printf '{"done":2,"stopped":"stopped","runs":30,"run_s":80,"kept":"2","until":1800000000,"kept_gb":0.2}\n'
  } | helper run-2
}

# ---- the app -----------------------------------------------------------------------------------

# Opens the app the way a user does (Launch Services), then waits for its first drawn window.
# A second launch in one scenario (after a quit) waits for the new process's own record.
launch() {
  local i before; before="$(traces | wc -l | tr -d ' ')"
  as_user open "$APP" || { fail "open failed"; return 1; }
  wait_new_trace "$before" || return 1
  wait_event 15000 windowDrawn 1
}
# wait_new_trace N: a copy of the app beyond the N already recorded has started its probe; TRACE
# is its record
wait_new_trace() {
  local i
  for i in $(seq 1 "${2:-300}"); do
    if [ "$(traces | wc -l | tr -d ' ')" -gt "$1" ] && [ -p "$OUT/cmd" ]; then TRACE="$(traces | tail -1)"; LAST_DLG=0; return 0; fi    # the new process counts its dialogs from 0
    sleep 0.05
  done
  fail "the probe didn't start"; return 1
}

# cmd VERB ARGS…: one probe command, waited for. Returns its ack's text in $ACK.
cmd() {
  N=$((N + 1))
  # unique also from a subshell ($(count_of …) runs cmd in one, where N alone would repeat)
  local id="c$N.$RANDOM$RANDOM" i
  # the FIFO's reader is the probe; a writer that can't open it gives up after 10 s
  ( printf '%s %s\n' "$id" "$*" > "$OUT/cmd" ) & local w=$!
  for i in $(seq 1 200); do kill -0 $w 2>/dev/null || break; sleep 0.05; done
  if kill -0 $w 2>/dev/null; then kill $w 2>/dev/null; fail "cmd $*: the app isn't listening"; ACK=""; return 1; fi
  local limit=$(( ${CMD_WAIT_S:-30} * 50 ))
  for i in $(seq 1 $limit); do
    ACK="$(grep -m1 "^$id " "$OUT/acks" 2>/dev/null | cut -d' ' -f2-)"
    [ -n "$ACK" ] && break
    sleep 0.02
  done
  case "$ACK" in ok*) return 0 ;; *) [ -n "${QUIET_FAIL:-}" ] || fail "cmd $*: ${ACK:-no answer}"; return 1 ;; esac
}

mark() { cmd mark "$*"; }
click() { cmd click "$1"; }
menu() { cmd menu "$1"; }
# answer TITLE: clicks the modal dialog's button; next_dialog MS: a dialog newer than the one
# answered last is up (dialogs that follow each other with no turn between are told apart)
LAST_DLG=0
answer() { wait_dialog 10000 && LAST_DLG="$(count_of dialog)" && cmd answer "$1"; }
next_dialog() { wait_event "$1" dialog $((LAST_DLG + 1)) && wait_dialog "$1"; }
# answer_like PREFIX: the dialog's button whose title starts with PREFIX (a title that carries a
# size, "Clear Saved Stems (1.2 GB)"), as the last frame of the trace shows the dialog
answer_like() {
  local t
  wait_dialog 10000 && sleep 0.2
  t="$(grep -o '"app.modal":"[^"]*"' "$TRACE" | tail -1 | grep -o "[{,] $1[^,}*]*\|{$1[^,}*]*" | head -1 | sed 's/^[{,] *//; s/ (off)$//')"
  [ -n "$t" ] || { fail "no dialog button like $1"; return 1; }
  answer "$t"
}
tab() { cmd tab "$1"; }
select_playlist() { cmd select "$1"; }
# wait_for MS KEY=VALUE (or KEY!=VALUE): a frame shows it
wait_for() { CMD_WAIT_S=$(( $1 / 1000 + 5 )) cmd wait "$1" "$2"; }
wait_dialog() { CMD_WAIT_S=$(( $1 / 1000 + 5 )) cmd wait "$1" "app.modal!="; }
wait_no_dialog() { CMD_WAIT_S=$(( $1 / 1000 + 5 )) cmd wait "$1" "app.modal="; }
# wait_event MS NAME COUNT: the probe recorded the event (or timing point) COUNT times
wait_event() { CMD_WAIT_S=$(( $1 / 1000 + 5 )) cmd event "$1" "$2" "$3"; }
# nothing on screen changed for MS (default 400) within 15 s
settle() { CMD_WAIT_S=20 cmd quiet "${1:-400}" 15000; }
count_of() { cmd count "$1" >/dev/null && echo "${ACK#ok }"; }

quit_app() {
  cmd quit || true
  local i
  for i in $(seq 1 100); do pgrep -x "RB Stems Plus" >/dev/null || break; sleep 0.1; done
  if pgrep -x "RB Stems Plus" >/dev/null; then fail "the app didn't quit"; pkill -9 -x "RB Stems Plus"; fi
}

# ---- activation from outside the process -------------------------------------------------------

# Finder in front: the app inactive, as the user leaving it.
finder_front() {
  as_user open -a Finder
  wait_for 5000 "app.active=0"
}

# Coming back to the app by the scenario's verb ($VERB, run.sh runs each activation scenario with
# both), asserting the probe saw NSApp.isActive false -> true and didBecomeActive.
activate() {
  local before reopens after reopened
  before="$(count_of didBecomeActive)"; reopens="$(count_of reopen)"
  # counts compared only when both were read (an empty one is a failure, never a match)
  if [ -z "$before" ] || [ -z "$reopens" ]; then fail "activation ($VERB): the probe's counts couldn't be read"; return 1; fi
  case "$VERB" in
    reopen) as_user open "$APP" ;;                           # a reopen event too: the Dock-click path
    activate) as_user "$H/bin/vmtool" activate "$BID" >/dev/null ;;
    appleevent) as_user "$H/bin/vmtool" appleevent "$BID" >/dev/null ;;
    click) as_user "$H/bin/vmtool" click "$BID" >/dev/null ;;
  esac
  wait_event 5000 didBecomeActive $((before + 1)) || { fail "activation ($VERB): no didBecomeActive"; return 1; }
  wait_for 2000 "app.active=1" || return 1
  if [ "$VERB" = reopen ]; then
    # A modal dialog that came up on activation (a main-queue block's runModal) holds the reopen
    # event until it closes: today's behaviour, recorded, not a failure (the scenario's trace has it)
    if ! QUIET_FAIL=1 wait_event 3000 reopen $((reopens + 1)); then
      if QUIET_FAIL=1 wait_for 100 "app.modal="; then fail "reopen: no reopen callback"
      else echo "note: reopen callback held while a dialog is up" >> "$RUN/result"; fi
    fi
  else
    reopened="$(count_of reopen)"
    if [ -z "$reopened" ]; then fail "activate only ($VERB): the reopen count couldn't be read"
    elif [ "$reopened" != "$reopens" ]; then fail "activate only ($VERB): a reopen callback ran"; fi
  fi
  echo "ok activation $VERB" >> "$RUN/result"
}

# ---- measurements (perf scenarios, tests/perf) -------------------------------------------------

app_pid() { pgrep -x "RB Stems Plus" | head -1; }
# P10: the app's phys_footprint now, into $RUN/footprint-LABEL.txt
footprint_now() { /usr/bin/footprint -p "$(app_pid)" > "$RUN/footprint-$1.txt" 2>&1 || fail "footprint failed"; }
# P16.own: the app's rusage now (its own CPU, its reaped children's CPU and lifetimes), into
# $RUN/rusage-LABEL.txt
rusage_now() { "$H/bin/vmtool" rusage "$(app_pid)" > "$RUN/rusage-$1.txt" 2>&1 || fail "vmtool rusage failed"; }
# P12: the app's CPU while idle for SECONDS (top samples every 5 s), into $RUN/top.txt, or
# $RUN/top-LABEL.txt (perf-start's window after the start: P12.start)
top_idle() { top -l $(( $1 / 5 + 1 )) -s 5 -pid "$(app_pid)" -stats pid,cpu,threads,idlew > "$RUN/top${2:+-$2}.txt" 2>&1; }
# P11: one start of the watcher (its probe writes watcher-<pid>.json), timed by time -l too
watcher_start() {
  as_user /usr/bin/time -l "$APP/Contents/Helpers/RB Stems Plus Watcher.app/Contents/MacOS/RB Stems Plus Watcher" >> "$RUN/watcher-time.txt" 2>&1
  pgrep -x "RB Stems Plus Watcher" >/dev/null && fail "a watcher process remains after its rounds"
  true
}
# the perf records to disk (perf mode keeps them in memory until then)
flush() { cmd flush; }

miniaturize() { cmd miniaturize && wait_for 5000 "win.mini=1"; }
deminiaturize() { cmd deminiaturize && wait_for 5000 "win.mini=0"; }

#!/bin/bash
# The harness on the host: one tart VM (lane L1 owns tart; one VM at a time), the scenarios run
# in it, their records back here. Run outside the sandbox (tart, ssh).
#
#   tests/harness/vm.sh build                  the VM-side tools (vmtool, fake-prepare) into build/harness/vm/
#   tests/harness/vm.sh up [CLONE] [--usb]     clone rb26-template (once) and start it with its window
#                                              (the owner's preference), with the template's MAC
#                                              address (one rekordbox seat; never beside the template
#                                              or another VM with that MAC), the harness's SSH key installed,
#                                              DJ's screen saver off; --usb: booted by tests/vm-usb.swift
#                                              with a virtual USB stick ("# usb: yes" scenarios)
#   tests/harness/vm.sh push [APPZIP]          the harness, the TEST build (default dist-test/) and its
#                                              bridge into the VM; the app to /Applications
#   tests/harness/vm.sh run LABEL SCEN… [-n RUNS] [--verbs both|reopen|activate|appleevent|click]
#                                              each scenario RUNS times (default 1) into
#                                              build/harness/results/LABEL/<scenario>[@verb]/<run>/
#   tests/harness/vm.sh sh 'COMMAND'           a command in the VM, as admin
#   tests/harness/vm.sh down                   stop the VM (at the end of every session)
#   tests/harness/vm.sh delete                 stop and delete the clone (the task's end)
# The GUI user (dj) must be logged in: the template logs in automatically. If it doesn't, stop and
# ask the owner to log in (never work around the login).
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
TART="${TART:-$HOME/Applications/tart.app/Contents/MacOS/tart}"
TEMPLATE="${TEMPLATE:-rb26-template}"
VMS_DIR="${TART_HOME:-$HOME/.tart}/vms"
state="${VM_STATE:-$root/build/harness}"     # VM_STATE, TART, TART_HOME: the selftest's
CLONE="$(cat "$state/clone" 2>/dev/null || echo rbsp-harness)"
XFER="$state/xfer"
KEY="$state/ssh/id"
mkdir -p "$state" "$XFER"

die() { echo "vm.sh: $*" >&2; exit 1; }
ip() { "$TART" ip "$CLONE" --wait 180; }
SSH_OPTS=(-i "$KEY" -o IdentitiesOnly=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=10)
vssh() { ssh "${SSH_OPTS[@]}" "admin@$(ip)" "$@"; }
VX="/Users/admin/xfer"      # the shared folder in the VM (a link to /Volumes/My Shared Files/xfer)

# One computer, one rekordbox seat: rekordbox counts a computer by its identity, and a tart clone
# differs from its template only by the MAC address tart gives it (owner, 2026-11-17). The template
# is an activated computer: a clone with the template's MAC is that computer (no new seat), one
# with another MAC is a new one (rekordbox activates it on its own: a seat). So every clone runs
# with its template's MAC (config.json "macAddress", set after the clone and before each start),
# and two VMs with one MAC never run together: tart run would give the second one a new MAC. Exit 3:
# the MAC can't be held (nothing of rekordbox has run then; a VM started here is stopped first)
die_mac() { echo "vm.sh: $*" >&2; exit 3; }
mac_of() { python3 -I -c 'import json, sys; print(json.load(open(sys.argv[1]))["macAddress"].lower())' "$VMS_DIR/$1/config.json" 2>/dev/null || true; }
pin_mac() {   # pin_mac VM MAC: VM's config.json names MAC (its other keys kept)
  python3 -I -c 'import json, os, sys
p, mac = sys.argv[1], sys.argv[2]
d = json.load(open(p)); d["macAddress"] = mac
with open(p + ".pin", "w") as f: json.dump(d, f)
os.replace(p + ".pin", p)' "$VMS_DIR/$1/config.json" "$2"
}
# same_mac MAC EXCEPT: the running VMs (EXCEPT left out) whose config.json names MAC
same_mac() {
  local v
  for v in $("$TART" list | awk 'NR > 1 && $NF == "running" {print $2}'); do
    [ "$v" != "$2" ] && [ "$(mac_of "$v")" = "$1" ] && echo "$v"
  done
  true
}

case "${1:-}" in
build)
  mkdir -p "$state/vm"
  sdk="$(xcrun --sdk macosx --show-sdk-path)"
  xcrun swiftc -O -sdk "$sdk" -target arm64-apple-macos12.0 "$root/tests/harness/vmtool.swift" -o "$state/vm/vmtool"
  clang -O2 -Wall -target arm64-apple-macos12.0 -o "$state/vm/fake-prepare" "$root/tests/harness/fake-prepare.c"
  codesign -f -s - "$state/vm/vmtool" "$state/vm/fake-prepare"
  echo "built: $state/vm/{vmtool,fake-prepare}"
  ;;
up)
  usb=""
  [ "${2:-}" = --usb ] && { usb=1; shift; }
  [ "${3:-}" = --usb ] && usb=1
  [ -n "${2:-}" ] && { CLONE="$2"; echo "$CLONE" > "$state/clone"; }
  [ "$CLONE" != "$TEMPLATE" ] || die "$CLONE is the template: vm.sh runs clones only"
  tmac="$(mac_of "$TEMPLATE")"
  [[ "$tmac" =~ ^([0-9a-f]{2}:){5}[0-9a-f]{2}$ ]] || die_mac "no MAC address in $VMS_DIR/$TEMPLATE/config.json"
  running=no; "$TART" list | awk -v c="$CLONE" '$2 == c {print $NF}' | grep -qx running && running=yes
  if [ $running = no ]; then
    # one VM with the template's MAC at a time: neither the template nor another of its clones
    # running (checked before the clone too: a running template is never cloned)
    others="$(same_mac "$tmac" "$CLONE")"
    [ -z "$others" ] || die_mac "$(echo $others) runs with $TEMPLATE's MAC address $tmac: one VM of $TEMPLATE at a time, never beside it (stop it first)"
    "$TART" list | awk '{print $2}' | grep -qx "$CLONE" || "$TART" clone "$TEMPLATE" "$CLONE"
    [ "$(mac_of "$CLONE")" = "$tmac" ] || { pin_mac "$CLONE" "$tmac"; echo "$CLONE: $TEMPLATE's MAC address $tmac set"; }
    [ "$(mac_of "$CLONE")" = "$tmac" ] || die_mac "$CLONE's MAC address couldn't be set to $tmac"
  fi
  if [ -n "$usb" ]; then
    # --usb: booted by tests/vm-usb.swift instead of tart run (a window too), with a 2 GB virtual
    # USB stick macOS sees as external, plugged and pulled by the scenarios' "usb" verb
    if ! [ -f "$state/usb-pid" ] || ! kill -0 "$(cat "$state/usb-pid")" 2>/dev/null; then
      if [ ! -x "$state/vm/vmusb" ]; then
        mkdir -p "$state/vm"
        xcrun swiftc -swift-version 5 -O "$root/tests/vm-usb.swift" -o "$state/vm/vmusb"
        printf '<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n<plist version="1.0"><dict><key>com.apple.security.virtualization</key><true/></dict></plist>\n' > "$state/vm/vmusb.entitlements"
        codesign -f -s - --entitlements "$state/vm/vmusb.entitlements" "$state/vm/vmusb"
      fi
      # the stick's image lives in the shared folder: the VM formats it once through hdiutil
      [ -f "$XFER/usb.img" ] || { truncate -s 2g "$XFER/usb.img"; rm -f "$state/usb.formatted"; }
      rm -f "$state/usb-ctl"; mkfifo "$state/usb-ctl"
      nohup "$state/vm/vmusb" "$VMS_DIR/$CLONE" "$XFER" "$XFER/usb.img" "$state/usb-ctl" > "$state/vmusb.log" 2>&1 &
      echo $! > "$state/usb-pid"
    fi
  elif ! "$TART" list | awk -v c="$CLONE" '$2 == c {print $NF}' | grep -qx running; then
    nohup "$TART" run "$CLONE" --dir="xfer:$XFER" > "$state/tart-run.log" 2>&1 &
  fi
  ip >/dev/null
  # booted: still the template's MAC (tart run gives a new one when it finds the MAC in use), and
  # no other VM with it; else this VM stopped at once
  m="$(mac_of "$CLONE")"; others="$(same_mac "$tmac" "$CLONE")"
  if [ "$m" != "$tmac" ] || [ -n "$others" ]; then
    "$TART" stop "$CLONE" >/dev/null 2>&1
    die_mac "$CLONE stopped: it runs with MAC address $m, not only $TEMPLATE's $tmac${others:+ (also running with it: $(echo $others))}"
  fi
  if [ ! -f "$KEY" ]; then mkdir -p "$state/ssh"; ssh-keygen -q -t ed25519 -N '' -f "$KEY"; fi
  cp "$KEY.pub" "$XFER/harness.pub"
  if ! vssh true 2>/dev/null; then
    # the tart image's public default password, used once to install this key
    printf '#!/bin/sh\necho admin\n' > "$state/ssh/askpass"; chmod +x "$state/ssh/askpass"
    for _ in $(seq 1 30); do
      SSH_ASKPASS="$state/ssh/askpass" SSH_ASKPASS_REQUIRE=force ssh "${SSH_OPTS[@]}" -o PreferredAuthentications=password,keyboard-interactive "admin@$(ip)" \
        'mkdir -p ~/.ssh && cat "/Volumes/My Shared Files/xfer/harness.pub" >> ~/.ssh/authorized_keys && chmod 600 ~/.ssh/authorized_keys' 2>/dev/null && break
      sleep 5
    done
  fi
  vssh 'ln -sfn "/Volumes/My Shared Files/xfer" /Users/admin/xfer; sw_vers -productVersion'
  # and the guest's own network card has it (what rekordbox reads)
  gmac="$(vssh "ifconfig en0 | awk '/ether/ {print \$2}'" | tr 'A-F' 'a-f' || true)"
  [ "$gmac" = "$tmac" ] || { "$TART" stop "$CLONE" >/dev/null 2>&1; die_mac "$CLONE stopped: its en0 is ${gmac:-unknown}, not $TEMPLATE's MAC address $tmac"; }
  # the template logs dj in by itself, a little after boot
  for _ in $(seq 1 40); do user="$(vssh 'stat -f %Su /dev/console')"; [ "$user" = dj ] && break; sleep 3; done
  [ "$user" = dj ] || die "nobody is logged in as dj in the VM ($user): ask the owner to log in"
  # DJ's screen would lock after an hour idle (only the owner can unlock it): no screen saver
  vssh 'sudo launchctl asuser $(id -u dj) sudo -u dj defaults -currentHost write com.apple.screensaver idleTime -int 0'
  if [ -n "$usb" ] && [ ! -f "$state/usb.formatted" ]; then
    # formatted once, plugged in as the stick (an image formatted through the shared folder doesn't
    # keep its volume): macOS's "not readable" alert for the blank stick is dismissed at once
    # (UserNotificationCenter ended: it is DJ's alert, nothing else is open then), then pulled
    echo attach > "$state/usb-ctl"
    vssh 'for i in $(seq 1 100); do d=$(diskutil list external physical | sed -n "s|^/dev/\(disk[0-9]*\) (external, physical):|\1|p" | head -1); [ -n "$d" ] && break; sleep 0.2; done
          [ -n "$d" ] || exit 1
          sudo pkill -x UserNotificationCenter
          sudo diskutil eraseDisk JHFS+ RBSPSTICK GPT "$d" >/dev/null && sudo pkill -x UserNotificationCenter; sync; diskutil eject "$d" >/dev/null; true' \
      && sleep 1 && echo detach > "$state/usb-ctl" && touch "$state/usb.formatted"
  fi
  echo "up: $CLONE at $(ip) (MAC $tmac, $TEMPLATE's), dj logged in${usb:+, with the USB stick (unplugged)}"
  ;;
push)
  zip="${2:-$root/dist-test/rbstemsplus-app.zip}"
  [ -f "$zip" ] || die "no $zip: scripts/build.sh --test URL first"
  { [ -x "$state/vm/vmtool" ] && [ ! "$root/tests/harness/vmtool.swift" -nt "$state/vm/vmtool" ] \
      && [ ! "$root/tests/harness/fake-prepare.c" -nt "$state/vm/fake-prepare" ]; } || "$0" build
  # a new name each time: the shared folder caches a replaced file. In the VM, the fake helper is
  # removed before it is copied: an executable overwritten in place is killed (its old signature
  # is cached by the kernel)
  stamp="$(date +%s)"
  rm -rf "$XFER"/harness-* "$XFER"/app-*.zip; hx="$XFER/harness-$stamp"
  mkdir -p "$hx/bin" "$hx/bridge"
  cp -R "$root/tests/harness/lib.sh" "$root/tests/harness/run.sh" "$root/tests/harness/scenarios" "$hx/"
  cp "$state/vm/vmtool" "$hx/bin/"; cp "$state/vm/fake-prepare" "$hx/bin/"
  cp "$(dirname "$zip")/libonnxruntime.1.18.0.dylib" "$hx/bridge/" 2>/dev/null || echo "no bridge next to the zip: fact_cache_on unavailable"
  cp "$zip" "$XFER/app-$stamp.zip"
  vssh "sudo rm -rf /Users/Shared/rbsp-harness/{lib.sh,run.sh,scenarios,bin,bridge} && sudo mkdir -p /Users/Shared/rbsp-harness/fake && sudo ditto $VX/harness-$stamp /Users/Shared/rbsp-harness && sudo rm -f /Users/Shared/rbsp-harness/fake/fake-prepare && sudo cp /Users/Shared/rbsp-harness/bin/fake-prepare /Users/Shared/rbsp-harness/fake/ && sudo chmod -R a+rX /Users/Shared/rbsp-harness && sudo bash /Users/Shared/rbsp-harness/run.sh setup && sudo bash /Users/Shared/rbsp-harness/run.sh install-app $VX/app-$stamp.zip"
  shasum -a 256 "$zip" | cut -d' ' -f1 > "$state/pushed-app.sha256"
  ;;
install)
  # only the app (tests/perf/abab.sh alternates two builds between runs)
  zip="$2"; stamp="$(date +%s)$$"
  rm -f "$XFER"/app-*.zip; cp "$zip" "$XFER/app-$stamp.zip"
  vssh "sudo bash /Users/Shared/rbsp-harness/run.sh install-app $VX/app-$stamp.zip"
  shasum -a 256 "$zip" | cut -d' ' -f1 > "$state/pushed-app.sha256"
  ;;
run)
  label="$2"; shift 2
  runs=1; verbs=""; scen=()
  while [ $# -gt 0 ]; do
    case "$1" in -n) runs="$2"; shift 2 ;; --verbs) verbs="$2"; shift 2 ;; *) scen+=("$1"); shift ;; esac
  done
  rc=0
  # a locked screen keeps the app from becoming active: every activation would fail
  if vssh 'ioreg -n Root -d1 | grep -q "\"IOConsoleLocked\" = Yes"'; then
    die "DJ's screen is locked in the VM: ask the owner to unlock it (vm.sh wait-login waits for it)"
  fi
  # the scripts as they are now (the app stays as pushed)
  sx="sync-$(date +%s)$RANDOM"; rm -rf "$XFER"/sync-*; mkdir -p "$XFER/$sx"
  cp -R "$root/tests/harness/lib.sh" "$root/tests/harness/run.sh" "$root/tests/harness/scenarios" "$XFER/$sx/"
  vssh "sudo rm -rf /Users/Shared/rbsp-harness/scenarios && sudo ditto $VX/$sx /Users/Shared/rbsp-harness && sudo chmod -R a+rX /Users/Shared/rbsp-harness"
  for s in "${scen[@]}"; do
    f="$root/tests/harness/scenarios/$s.sh"
    [ -f "$f" ] || die "no scenario $s"
    v="$verbs"
    [ -z "$v" ] && v="$(sed -n 's/^# verbs: *//p' "$f" | head -1)"
    case "${v:-none}" in both) list="reopen activate" ;; none) list="reopen" ;; *) list="$v" ;; esac
    mode="$(sed -n 's/^# mode: *//p' "$f" | head -1)"
    # PERF_MODE replaces a perf scenario's mode (tests/perf/abab.sh: perf; the probe's perf-min stays)
    [ "${mode:-trace}" = perf ] && [ -n "${PERF_MODE:-}" ] && mode="$PERF_MODE"
    # "# server: DIR [--rate KB/s]": the TEST build's download server (its compiled-in URL) serves
    # DIR meanwhile ("dist-test" is the build under test's own folder: SERVER_SRC when set, as
    # abab.sh sets it per side); "# server: make NAME FLAGS… [--rate KB/s]": a folder made by
    # tests/harness/update-server.sh from SERVER_SRC (default dist-test/) with its test key
    # SERVER_KEY (default build/test-keys/test-key.pem). The server must be ours and serve that
    # folder (port 8765 free before, its payload.json fetched back), or the scenario doesn't run;
    # what it served (payload.json's and the app zip's sha256) goes into each run's build.txt.
    srv="$(sed -n 's/^# server: *//p' "$f" | head -1)"; spid=""; served=""
    if [ -n "$srv" ]; then
      read -r -a sw <<< "$srv"
      rate=(); flags=(); sdir=""
      if [ "${sw[0]}" = make ]; then
        k=2
        while [ $k -lt ${#sw[@]} ]; do
          if [ "${sw[k]}" = --rate ]; then rate=(--rate "${sw[k+1]}"); k=$((k + 2)); else flags+=("${sw[k]}"); k=$((k + 1)); fi
        done
        sdir="$state/servers/${sw[1]}"
        "$root/tests/harness/update-server.sh" "$sdir" "${SERVER_SRC:-$root/dist-test}" "${SERVER_KEY:-$root/build/test-keys/test-key.pem}" ${flags[@]+"${flags[@]}"} >/dev/null \
          || die "$s: its server folder couldn't be made (update-server.sh)"
      else
        if [ "${sw[0]}" = dist-test ]; then sdir="${SERVER_SRC:-$root/dist-test}"; else sdir="$root/${sw[0]}"; fi
        [ "${sw[1]:-}" = --rate ] && rate=(--rate "${sw[2]}")
      fi
      [ -f "$sdir/payload.json" ] || die "$s: no payload.json in $sdir"
      if curl -s -o /dev/null --max-time 2 http://192.168.64.1:8765/; then die "$s: something already serves 192.168.64.1:8765 (a g1-round.sh server?): stop it first"; fi
      python3 -I "$root/tests/harness/serve.py" "$sdir" ${rate[@]+"${rate[@]}"} > "$state/server.log" 2>&1 & spid=$!
      for _ in $(seq 1 50); do curl -s -o "$state/served-payload.json" --max-time 2 http://192.168.64.1:8765/payload.json && break; sleep 0.1; done
      kill -0 "$spid" 2>/dev/null || die "$s: the local server didn't start (see $state/server.log)"
      cmp -s "$state/served-payload.json" "$sdir/payload.json" || { kill "$spid"; die "$s: the server on 8765 doesn't serve $sdir"; }
      served="served=$sdir
served_payload_sha256=$(shasum -a 256 "$sdir/payload.json" | cut -d' ' -f1)
served_app_sha256=$(shasum -a 256 "$sdir/rbstemsplus-app.zip" 2>/dev/null | cut -d' ' -f1)"
    fi
    # "# usb: yes": the scenario plugs and pulls the stick (vm.sh up --usb): its requests come
    # through the shared folder (a new usbreq-* folder each time: the VM's view of the shared
    # folder keeps a folder that was replaced), answered here on vm-usb.swift's control FIFO
    upid=""; ureq=""
    if grep -q '^# usb: yes' "$f"; then
      [ -p "$state/usb-ctl" ] || die "$s needs the USB stick: tests/harness/vm.sh up CLONE --usb"
      rm -rf "$XFER"/usbreq-*; ureq="usbreq-$(date +%s)$RANDOM"; mkdir -p "$XFER/$ureq"
      (
        while sleep 0.2; do
          for r in "$XFER/$ureq"/*.req; do
            [ -e "$r" ] || continue
            n="$(grep -c '^usb: ' "$state/vmusb.log" 2>/dev/null || true)"
            cat "$r" > "$state/usb-ctl"
            for _ in $(seq 1 50); do [ "$(grep -c '^usb: ' "$state/vmusb.log" 2>/dev/null || true)" -gt "$n" ] && break; sleep 0.1; done
            tail -1 "$state/vmusb.log" > "${r%.req}.part" && mv "${r%.req}.part" "${r%.req}.done"; rm -f "$r"
          done
        done
      ) & upid=$!
    fi
    for verb in $list; do
      name="$s"; [ "${v:-none}" != none ] && name="$s@$verb"
      for i in $(seq 1 "$runs"); do
        dir="results/$label/$name/$i"
        xd="xr/$(date +%s)$RANDOM/$name/$i"; rm -rf "$state/$dir"; mkdir -p "$XFER/$xd"
        echo "== $name run $i"
        if vssh "sudo MODE=${mode:-trace} PERF_IDLE_S=${PERF_IDLE_S:-} PERF_P12_S=${PERF_P12_S:-} PERF_ROUNDS=${PERF_ROUNDS:-} USBREQ='/Volumes/My Shared Files/xfer/$ureq' bash /Users/Shared/rbsp-harness/run.sh scenario $s $VX/$xd $verb"; then echo "   passed"; else echo "   FAILED"; rc=1; fi
        mkdir -p "$state/$(dirname "$dir")"; cp -R "$XFER/$xd" "$state/$dir"
        { printf 'app_sha256=%s\nclone=%s\n' "$(cat "$state/pushed-app.sha256" 2>/dev/null)" "$CLONE"; if [ -n "$served" ]; then printf '%s\n' "$served"; fi; } > "$state/$dir/build.txt"
      done
    done
    [ -n "$spid" ] && kill "$spid" 2>/dev/null
    if [ -n "$upid" ]; then kill "$upid" 2>/dev/null; wait "$upid" 2>/dev/null || true; fi
  done
  exit $rc
  ;;
sh) shift; vssh "$@" ;;
wait-login)
  # waits up to MINUTES (default 30) for dj's GUI login (the owner's)
  # (and the screen unlocked); then DJ's screen saver is kept off, so it doesn't lock again
  for _ in $(seq 1 $(( ${2:-30} * 6 ))); do
    if [ "$(vssh 'stat -f %Su /dev/console' 2>/dev/null)" = dj ] && ! vssh 'ioreg -n Root -d1 | grep -q "\"IOConsoleLocked\" = Yes"'; then
      vssh 'sudo launchctl asuser $(id -u dj) sudo -u dj defaults -currentHost write com.apple.screensaver idleTime -int 0'
      echo "dj is logged in"; exit 0
    fi
    sleep 10
  done
  echo "dj didn't log in"; exit 1 ;;
down|delete)
  # a VM booted with --usb stops through vm-usb.swift's FIFO; tart stops the others
  if [ -f "$state/usb-pid" ] && kill -0 "$(cat "$state/usb-pid")" 2>/dev/null; then
    echo stop > "$state/usb-ctl"
    for _ in $(seq 1 60); do kill -0 "$(cat "$state/usb-pid")" 2>/dev/null || break; sleep 1; done
  fi
  rm -f "$state/usb-pid"
  "$TART" stop "$CLONE" 2>/dev/null || true
  if [ "$1" = delete ]; then "$TART" delete "$CLONE"; rm -f "$state/clone" "$XFER/usb.img" "$state/usb.formatted"; fi ;;
*) sed -n '2,21p' "$0"; exit 2 ;;
esac

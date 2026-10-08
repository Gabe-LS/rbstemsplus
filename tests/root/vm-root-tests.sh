#!/bin/bash
# The root scripts (app/Sources/Scripts.swift) on the REAL paths, run as root: for a throwaway VM
# clone only. It changes /Applications/rekordbox 7 on purpose (links, owners, a hidden or moved
# folder, runs stopped with kill -9) and puts it back after each case, but a bug here or in the
# scripts can leave rekordbox broken. Never run it on a Mac you use.
#
# In the VM (macOS 12 or later, an admin account):
#   1. rekordbox 7 installed by Pioneer's installer and opened once, then quit. Stems Cache not
#      installed: no /Library/Application Support/rbstemsplus.
#   2. Let the terminal app change other apps: System Settings > Privacy & Security > App
#      Management > Terminal on (root's writes into rekordbox.app are refused otherwise, as
#      they are for RB Stems Plus until the user allows it).
#   3. Copy in, from the developer Mac: build/test/rbsp-test ("make test") and
#      build/bridge/libonnxruntime.1.18.0.dylib ("make bridge"), e.g. to ~/rbsp/, and this file.
#   4. Run:
#        sudo bash vm-root-tests.sh --throwaway-vm ~/rbsp/rbsp-test ~/rbsp/libonnxruntime.1.18.0.dylib
#      About 30 minutes: every install re-signs rekordbox's 300 MB executable. Each check prints
#      "ok" or "FAIL", then a summary; the exit code is 1 if any failed. Everything printed is
#      also in /var/tmp/rbsp-vm-root-tests.log.
#   5. Throw the clone away (or keep it to look into a failure).
#
# A dry run of this file itself, as your own account, on a fake rekordbox in a new folder (no
# root, nothing real touched, needs Xcode's clang; the owner case is skipped):
#   tests/root/vm-root-tests.sh --fake "$TMPDIR/rbsp-fake" build/test/rbsp-test
#
# Checked after every case: rekordbox verifies as AlphaTheta's, or as our complete seal with a
# complete saved set, or (after a kill) at least its saved set is complete; the lock is gone. At
# the end: rekordbox.app is exactly what it was at the start (names, owners, modes, link
# targets, contents), the root folder is gone, and nothing else in /Library/Application Support
# changed.
set -u
PATH=/usr/bin:/bin:/usr/sbin:/sbin
say() { printf '%s\n' "$*"; }
die() { say "vm-root-tests: $*" >&2; exit 2; }
usage="usage: sudo $0 --throwaway-vm <rbsp-test> <bridge dylib>   or   $0 --fake <new folder> <rbsp-test>"

MODE="${1-}"; [ $# = 3 ] || die "$usage"
case "$MODE" in
  --throwaway-vm)
    TESTBIN="$2"; BRIDGE="$3"; FAKE=
    [ "$(id -u)" = 0 ] || die "run it with sudo (in a throwaway VM clone only)"
    WORK="$(mktemp -d /var/tmp/rbsp-vm-tests.XXXXXX)"; LOG=/var/tmp/rbsp-vm-root-tests.log;;
  --fake)
    FAKE="$2"; TESTBIN="$3"
    [ "$(id -u)" != 0 ] || die "the dry run is for your own account, not root"
    [ ! -e "$FAKE" ] || die "$FAKE exists: give a new folder"
    "$TESTBIN" --make-fake "$FAKE" || die "can't make the fake rekordbox"
    BRIDGE="$FAKE/parts/bridge.dylib"; WORK="$FAKE/work"; mkdir -p "$WORK"; LOG="$FAKE/log";;
  *) die "$usage";;
esac
[ -x "$TESTBIN" ] && [ -f "$BRIDGE" ] || die "$usage"
TESTBIN="$(cd "$(dirname "$TESTBIN")" && pwd)/$(basename "$TESTBIN")"
exec > >(tee -a "$LOG") 2>&1

if [ -n "$FAKE" ]; then
  RBDIR="$FAKE/Applications/rekordbox 7"; OUT="$FAKE/Library/rbstemsplus"; LOCK="$FAKE/run/rbstemsplus.lock"
  REQ='identifier "fake.alphatheta"'; RBPROC=rbsp-rekordbox; SUPPORT="$FAKE/Library"
  # test names for Pioneer's installer and updater, so a real process can't change a run
  INSTPROC=rbsp-installer; UPDPROC='rbsp-Upmgr rb'; INSTAPP="$FAKE/procs/Installer.app/Contents/MacOS/Installer"
else
  RBDIR="/Applications/rekordbox 7"; OUT="/Library/Application Support/rbstemsplus"; LOCK=/var/run/rbstemsplus.lock
  REQ='anchor apple generic and certificate leaf[subject.OU] = "6BRHGXQ6VU"'; RBPROC=rekordbox; SUPPORT="/Library/Application Support"
  INSTPROC=installer; UPDPROC='Upmgr rekordbox'; INSTAPP=
fi
INSTLINE="if running -x installer || running -x 'Upmgr rekordbox' || running -f '^/System/Library/CoreServices/Installer[.]app/'; then"
APP="$RBDIR/rekordbox.app"; C="$APP/Contents"; LIBF="$C/Frameworks/libonnxruntime.1.18.0.dylib"
THREE="MacOS/rekordbox _CodeSignature/CodeResources Frameworks/libonnxruntime.1.18.0.dylib"
BUNDLE=com.pioneerdj.rekordboxdj
TRACE="$WORK/trace"
sha() { shasum -a 256 "$1" 2>/dev/null | cut -d' ' -f1; }
VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$C/Info.plist" 2>/dev/null)" || die "no rekordbox at $APP"
SET="$OUT/pioneer/$VERSION"; ORTCOPY="$OUT/ort/libonnxruntime.1.18.0.dylib"
BSHA="$(sha "$BRIDGE")"
INSTALL="$("$TESTBIN" --print-script install)" && UNINSTALL="$("$TESTBIN" --print-script uninstall)" || die "$TESTBIN can't print the scripts"
SAY_LINE="say() { /usr/bin/printf '%s\n' \"\$*\" 2>/dev/null || true; }"

# MARK: the test body: the release text, with HOOKS (at|code: at a step's name or number, run
# that code) at its "# step:" points and EXTRA (more code, e.g. a codesign wrapper) after them; in
# --fake mode also the fake's paths, this account as "root", the fake signer and the fake process
# names (exactly as app/Tests does)

replace_once() {   # $1 text, $2 from, $3 to; fails unless $2 is there exactly once
  local t="$1" rest
  rest="${t#*"$2"}"
  [ "$rest" != "$t" ] || return 1
  case "$rest" in *"$2"*) return 1;; esac
  printf '%s' "${t%%"$2"*}$3$rest"
}
HOOKS=(); EXTRA=
body() {
  local b h hook
  if [ "$1" = install ]; then b="$INSTALL"; else b="$UNINSTALL"; fi
  hook="__n=0"$'\n'"__step() { __n=\$((__n+1)); printf '%s\\n' \"\$1\" >> '$TRACE'"
  for h in ${HOOKS[@]+"${HOOKS[@]}"}; do
    hook+=$'\n'"  if [ \"\$1\" = '${h%%|*}' ] || [ \"\$__n\" = '${h%%|*}' ]; then ${h#*|}; fi"
  done
  hook+=$'\n'"}"
  [ -z "$EXTRA" ] || hook+=$'\n'"$EXTRA"
  b="$(replace_once "$b" "$SAY_LINE" "$SAY_LINE"$'\n'"$hook")" || die "the $1 script no longer has: $SAY_LINE"
  b="${b//"# step: "/__step }"
  if [ -n "$FAKE" ]; then
    local from to
    while IFS='|' read -r from to; do
      b="$(replace_once "$b" "$from" "$to")" || die "the $1 script no longer has: $from"
    done <<EOF
RBDIR='/Applications/rekordbox 7'|RBDIR='$RBDIR'
OUT='/Library/Application Support/rbstemsplus'|OUT='$OUT'
LOCK='/var/run/rbstemsplus.lock'|LOCK='$LOCK'
OWNER=0:0|OWNER=$(id -u):$(id -g)
PIONEER='anchor apple generic and certificate leaf[subject.OU] = "6BRHGXQ6VU"'|PIONEER='$REQ'
if running -x rekordbox; then|if running -x '$RBPROC'; then
EOF
    b="$(replace_once "$b" "$INSTLINE" "if running -x '$INSTPROC' || running -x '$UPDPROC' || running -f '${INSTAPP//./[.]}'; then")" \
      || die "the $1 script no longer has: $INSTLINE"
  fi
  printf '%s' "$b"
}

# MARK: running the scripts (RC: exit code, TEXT: what they said)

RC=0; TEXT=
ort_sha() { if grep -aq 'rbstems bridge: ' "$LIBF" 2>/dev/null; then sha "$ORTCOPY"; else sha "$LIBF"; fi; }
report() { say "  > exit $RC: $(printf '%s' "$TEXT" | tr '\n' '|' | cut -c1-400)"; }
inst() {   # [force] [version] [bundle ID] [stdin]
  local b; b="$(body install)" || exit 2; rm -f "$TRACE"
  TEXT="$(/bin/bash -c "$b" rbsp install "${2-$VERSION}" "${3-$BUNDLE}" "$BSHA" "$(ort_sha)" "${1-0}" < "${4-$BRIDGE}" 2>&1)"; RC=$?; report
}
uninst() { local b; b="$(body uninstall)" || exit 2; rm -f "$TRACE"; TEXT="$(/bin/bash -c "$b" rbsp uninstall "$VERSION" "$BUNDLE" < /dev/null 2>&1)"; RC=$?; report; }
rmroot() { local b; b="$(body uninstall)" || exit 2; rm -f "$TRACE"; TEXT="$(/bin/bash -c "$b" rbsp remove-root-folder "" "" < /dev/null 2>&1)"; RC=$?; report; }
steps() { [ -f "$TRACE" ] && wc -l < "$TRACE" | tr -d ' ' || printf 0; }
laststep() { [ -f "$TRACE" ] && tail -n 1 "$TRACE"; }

# MARK: checks

N=0; FAILS=0
check() { N=$((N+1)); if [ "$1" = 0 ]; then say "ok   $2"; else say "FAIL $2"; FAILS=$((FAILS+1)); fi; }
pioneers() { codesign --verify --strict -R="$REQ" "$APP" >/dev/null 2>&1; }
ours() { grep -aq 'rbstems bridge: ' "$LIBF" 2>/dev/null && codesign --verify --deep --strict "$APP" >/dev/null 2>&1; }
set_ok() { [ -f "$SET/SHA256SUMS" ] && (cd "$SET" && shasum -a 256 -s -c SHA256SUMS) >/dev/null 2>&1; }
no_lock() { [ ! -e "$LOCK" ] && [ ! -L "$LOCK" ]; }
three() { local f; for f in $THREE; do sha "$C/$f"; done; }
safe() { pioneers || { ours && set_ok; } || set_ok; }
# (the dry run leaves out the group: as a user, cp -p can't keep the group the temporary folder gave)
manifest() {
  local fmt='%N %Su:%Sg %Lp %HT %Y'; [ -z "$FAKE" ] || fmt='%N %Su %Lp %HT %Y'
  (cd "$RBDIR" && find . -print0 | xargs -0 stat -f "$fmt" | sort && find . -type f -print0 | xargs -0 shasum -a 256 | sort)
}
summary() { say ""; say "$N checks, $FAILS failed. Log: $LOG"; [ "$FAILS" = 0 ]; }
# back to rekordbox as Pioneer's installer left it, without the root folder; stops the run if it can't
reset() {
  HOOKS=()
  if ! pioneers || [ -e "$OUT" ]; then uninst; fi
  if pioneers && [ ! -e "$OUT" ] && no_lock && [ "$(three)" = "$THREE0" ]; then return 0; fi
  check 1 "back to Pioneer's rekordbox after: $1"; summary; say "stopping: rekordbox isn't back to Pioneer's"; exit 1
}
fakeproc() {   # a process named $1 (or at the path $1)
  local exe="$WORK/procs/$1"; case "$1" in /*) exe="$1";; esac
  mkdir -p "$(dirname "$exe")"; cp "$TESTBIN" "$exe"; "$exe" --sleep 120 >/dev/null 2>&1 & FAKEPID=$!; sleep 1
}
linkswap() { mv "$1" "$1.rbsp-real" && ln -s "$(basename "$1").rbsp-real" "$1"; }
unlinkswap() { rm "$1" && mv "$1.rbsp-real" "$1"; }

# MARK: the start

say "rekordbox $VERSION at $APP; work folder $WORK"
pioneers || die "rekordbox doesn't verify as AlphaTheta's: start from Pioneer's installer"
[ ! -e "$OUT" ] || die "$OUT exists: uninstall Stems Cache first"
! pgrep -x "$RBPROC" >/dev/null || die "quit rekordbox first"
no_lock || die "$LOCK exists"
THREE0="$(three)"; manifest > "$WORK/manifest.start"; SUPPORT0="$(ls -a "$SUPPORT")"
ORT0="$(sha "$LIBF")"

# MARK: install and uninstall

inst; [ $RC = 0 ] && ours && set_ok && [ "$(sha "$ORTCOPY")" = "$ORT0" ] && no_lock; check $? "install: the bridge in, re-signed, Pioneer's files saved, its library in ort/"
[ "$(cd "$SET" && for f in $THREE; do sha "$f"; done)" = "$THREE0" ]; check $? "install: the saved set is Pioneer's three files"
inst; [ $RC = 0 ] && ours; check $? "install again: already in place"
inst 1; [ $RC = 0 ] && ours && set_ok; check $? "reinstall (force)"
uninst; [ $RC = 0 ] && pioneers && [ ! -e "$OUT" ] && [ "$(three)" = "$THREE0" ] && no_lock; check $? "uninstall: Pioneer's files back, the root folder removed"
uninst; [ $RC = 0 ] && pioneers; check $? "uninstall with nothing installed"
reset "install and uninstall"

# MARK: what the app checked must still be so

inst 0 1.0; [ $RC = 4 ] && pioneers && [ ! -e "$OUT" ]; check $? "another version than checked: nothing changed"
inst 0 ..; [ $RC = 4 ] && pioneers && [ ! -e "$OUT" ]; check $? "\"..\" as the version argument: nothing changed"
inst 0 "$VERSION" com.example.other; [ $RC = 10 ] && pioneers && [ ! -e "$OUT" ]; check $? "another bundle ID: refused"
cp -p "$C/Info.plist" "$WORK/Info.plist"
for v in .. "" "$VERSION/" "../../x"; do
  /usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString $v" "$C/Info.plist"
  inst 0 "$v"; [ $RC = 10 ] && [ ! -e "$OUT" ]; check $? "rekordbox's version \"$v\": refused"
done
cp -p "$WORK/Info.plist" "$C/Info.plist"
reset "version checks"

# MARK: links and permissions where Pioneer's installer leaves real, root-only folders and files

for p in "$RBDIR" "$APP" "$C" "$C/Frameworks" "$C/MacOS" "$C/_CodeSignature" "$LIBF" "$C/MacOS/rekordbox" "$C/_CodeSignature/CodeResources" "$C/Info.plist"; do
  name="${p#"$RBDIR"}"; name="rekordbox 7${name}"
  linkswap "$p" || { check 1 "make $name a link"; continue; }
  inst; [ $RC = 10 ] && [ ! -e "$OUT" ]; check $? "install: $name as a link: refused"
  uninst; [ $RC = 10 ]; check $? "uninstall: $name as a link: refused"
  unlinkswap "$p"; pioneers; check $? "($name put back)"
done
inst; [ $RC = 0 ] && ours; check $? "(installed, for the next cases)"
for p in "$C/Frameworks" "$LIBF"; do
  linkswap "$p"
  uninst; [ $RC = 10 ] && set_ok; check $? "uninstall with the bridge in: ${p#"$C/"} as a link: refused, the saved set stays"
  unlinkswap "$p"
done
reset "links"
for p in "$C/Frameworks" "$LIBF"; do
  mode="$(stat -f %Lp "$p")"; chmod g+w "$p"
  inst; [ $RC = 10 ] && pioneers; check $? "install: ${p#"$C/"} writable by its group: refused"
  chmod "$mode" "$p"
done
if [ -z "$FAKE" ]; then
  owner="$(stat -f %u:%g "$RBDIR")"; chown "${SUDO_UID:-501}" "$RBDIR"
  inst; [ $RC = 10 ] && pioneers; check $? "install: rekordbox 7 owned by a user: refused"
  chown "$owner" "$RBDIR"
  owner="$(stat -f %u:%g "$LIBF")"; chown "${SUDO_UID:-501}" "$LIBF"
  inst; [ $RC = 10 ] && pioneers; check $? "install: rekordbox's library owned by a user: refused"
  chown "$owner" "$LIBF"
fi
reset "permissions"

# MARK: rekordbox missing; the root folder alone

inst; HOOKS=("verified|exit 0"); uninst; HOOKS=()
pioneers && [ -d "$SET" ]; check $? "(Pioneer's rekordbox with a root folder left, for the next cases)"
rmroot; [ $RC = 4 ] && [ -d "$OUT" ]; check $? "remove-root-folder with rekordbox there: refused"
mv "$RBDIR" "$RBDIR.rbsp-hidden"
inst; [ $RC = 9 ]; check $? "install with rekordbox missing: its own exit code"
uninst; [ $RC = 9 ] && [ -d "$OUT" ]; check $? "uninstall with rekordbox missing: nothing removed"
rmroot; [ $RC = 0 ] && [ ! -e "$OUT" ] && [ -d "$RBDIR.rbsp-hidden/rekordbox.app" ]; check $? "remove-root-folder with rekordbox missing: only the root folder goes"
mv "$RBDIR.rbsp-hidden" "$RBDIR"
reset "rekordbox missing"

# MARK: leftovers of a stopped run

inst
for f in $THREE; do printf partial > "$C/$f.rbsp-restore"; done
printf partial > "$C/Frameworks/.bridge.new"; : > "$C/Frameworks/.rbsp-probe"; mkdir -p "$OUT/stage.AbC123" "$OUT/stage.toolong1"
uninst; [ $RC = 0 ] && pioneers && [ ! -e "$C/Frameworks/.bridge.new" ] && [ ! -e "$C/Frameworks/.rbsp-probe" ] && [ ! -e "$C/MacOS/rekordbox.rbsp-restore" ]
check $? "leftovers removed by exact name, then the uninstall works"
reset "leftovers"

# MARK: the lock, Pioneer's installer and updater, rekordbox open

mkdir -p "$(dirname "$LOCK")"
sleep 120 & holder=$!
mkdir "$LOCK"; printf '%s\n%s\n' "$holder" "$(ps -o lstart= -p "$holder")" > "$LOCK/owner"
inst; [ $RC = 7 ] && pioneers && [ -d "$LOCK" ]; check $? "lock held by a running process: refused, the lock stays"
kill "$holder"; wait "$holder" 2>/dev/null
inst; [ $RC = 0 ] && ours && no_lock; check $? "lock of a process that is gone: taken over, then released"
reset "lock"
mkdir "$LOCK"
inst; [ $RC = 7 ]; check $? "lock being taken right now (no owner yet): refused"
touch -t 202001010000 "$LOCK"
inst; [ $RC = 0 ] && no_lock; check $? "lock without owner from long ago: taken over"
reset "lock without owner"
for p in "$INSTPROC" "$UPDPROC" ${INSTAPP:+"$INSTAPP"} "$RBPROC"; do
  fakeproc "$p"
  inst; want=8; [ "$p" = "$RBPROC" ] && want=5
  [ $RC = $want ] && pioneers && no_lock; check $? "a process named $p running: refused (exit $want)"
  kill "$FAKEPID"; wait "$FAKEPID" 2>/dev/null
done

# MARK: what arrives on stdin

inst 0 "$VERSION" "$BUNDLE" /dev/zero; [ $RC = 4 ] && pioneers && ! ls -d "$OUT"/stage.?????? >/dev/null 2>&1; check $? "/dev/zero instead of the bridge: refused (32 MB cap)"
mkfifo "$WORK/fifo"; (printf abc > "$WORK/fifo" &)
inst 0 "$VERSION" "$BUNDLE" "$WORK/fifo"; [ $RC = 4 ] && pioneers; check $? "a FIFO with other bytes instead of the bridge: refused"
inst 0 "$VERSION" "$BUNDLE" /dev/null; [ $RC = 4 ] && pioneers; check $? "nothing on stdin: refused"
reset "stdin"

# MARK: a step fails after rekordbox was changed

for at in swapped signed; do
  HOOKS=("$at|exit 1"); inst; HOOKS=()
  [ $RC = 6 ] && pioneers && [ "$(three)" = "$THREE0" ] && no_lock; check $? "install fails after $at: rolled back"
done
inst; HOOKS=("restored|exit 1"); uninst; HOOKS=()
[ $RC = 6 ] && pioneers && set_ok; check $? "uninstall fails after the restore: tried again, the saved set stays"
reset "failures"
# the output closed half-way (the app quit)
HOOKS=("swapped|sleep 1" "signed|say failing; say more; exit 1")
b="$(body install)" || exit 2; HOOKS=(); rm -f "$TRACE"
/bin/bash -c "$b" rbsp install "$VERSION" "$BUNDLE" "$BSHA" "$(ort_sha)" 0 < "$BRIDGE" 2>&1 | head -c 1 >/dev/null; RC="${PIPESTATUS[0]}"
[ "$RC" = 6 ] && pioneers && [ "$(three)" = "$THREE0" ] && no_lock && [ "$(grep -c restored-file "$TRACE")" = 3 ]; check $? "output closed mid-run (EPIPE): the rollback finishes (exit $RC)"
reset "EPIPE"
# rekordbox's folder moved during the run: its files are put back where it went
HOOKS=("swapped|mv \"\$RBDIR\" \"\$RBDIR.moved\""); inst; HOOKS=()
[ $RC = 11 ] && [ ! -e "$RBDIR" ]; check $? "rekordbox moved during the install: reinstall rekordbox (exit 11)"
[ -d "$RBDIR.moved" ] && mv "$RBDIR.moved" "$RBDIR"
pioneers; check $? "(the moved rekordbox has Pioneer's files)"
reset "moved"
# rekordbox's folder renamed and replaced WHILE codesign runs, at each of its calls: codesign only
# gets the copy in the root folder, so nothing outside is written, and the run stops. Swapped in:
# a tree whose MacOS, _CodeSignature and Frameworks are links to a folder outside.
DECOY="$WORK/decoy/rekordbox 7"; OUTSIDE="$WORK/outside"
rm -rf "$WORK/decoy" "$OUTSIDE"; mkdir -p "$DECOY/rekordbox.app/Contents" "$OUTSIDE"
cp -p "$C/Info.plist" "$DECOY/rekordbox.app/Contents/Info.plist"
for d in MacOS _CodeSignature Frameworks; do
  mkdir -p "$OUTSIDE/$d"; printf 'outside\n' > "$OUTSIDE/$d/sentinel"; ln -s "$OUTSIDE/$d" "$DECOY/rekordbox.app/Contents/$d"
done
cp "$OUTSIDE/MacOS/sentinel" "$OUTSIDE/MacOS/rekordbox"; cp "$OUTSIDE/MacOS/sentinel" "$OUTSIDE/_CodeSignature/CodeResources"
cp "$OUTSIDE/MacOS/sentinel" "$OUTSIDE/Frameworks/libonnxruntime.1.18.0.dylib"
tree() { (cd "$1" && find . -print | LC_ALL=C sort && find . -type f -exec shasum -a 256 {} + | LC_ALL=C sort); }
OUTSIDE0="$(tree "$OUTSIDE")"; DECOY0="$(tree "$DECOY")"
cswrap() {   # $1: the codesign call at which rekordbox's folder is swapped (0: none)
  EXTRA="codesign() {
  local n p
  n=\$(( \$(cat '$WORK/cs-count' 2>/dev/null || printf 0) + 1 )); printf '%s\n' \"\$n\" > '$WORK/cs-count'
  printf '%s\n' \"\${@: -1}\" >> '$WORK/cs-paths'
  if [ \"\$n\" = '$1' ]; then
    command codesign \"\$@\" & p=\$!
    mv \"\$RBDIR\" \"\$RBDIR.moved\" && cp -R '$DECOY' \"\$RBDIR\"
    wait \"\$p\"; return
  fi
  command codesign \"\$@\"
}"
  rm -f "$WORK/cs-count" "$WORK/cs-paths"
}
paths_ok() { [ -s "$WORK/cs-paths" ] && ! grep -v "^$OUT/" "$WORK/cs-paths" >/dev/null; }
for act in install uninstall; do
  [ $act = uninstall ] && inst
  cswrap 0; if [ $act = install ]; then inst; else uninst; fi; EXTRA=
  calls="$(cat "$WORK/cs-count" 2>/dev/null || printf 0)"
  [ $RC = 0 ] && [ "$calls" -ge 3 ] && paths_ok; check $? "$act: codesign gets only paths in $OUT ($calls calls)"
  reset "$act, codesign counted"
  k=1
  while [ "$k" -le "$calls" ]; do
    [ $act = uninstall ] && inst
    cswrap "$k"; if [ $act = install ]; then inst; else uninst; fi; EXTRA=
    { [ $RC = 10 ] || [ $RC = 11 ]; } && [ "$(tree "$OUTSIDE")" = "$OUTSIDE0" ] && [ "$(tree "$RBDIR")" = "$DECOY0" ] && paths_ok
    check $? "$act: rekordbox swapped during codesign call $k: stopped, nothing outside written"
    if [ -d "$RBDIR.moved" ]; then rm -rf "$RBDIR"; mv "$RBDIR.moved" "$RBDIR"; fi
    safe; check $? "($act, swap at call $k: rekordbox is AlphaTheta's, or its saved set is complete)"
    reset "$act, swapped during codesign call $k"
    k=$((k + 1))
  done
done

# MARK: the saved set, and repairs

inst; rm "$SET/SHA256SUMS"
uninst; [ $RC = 0 ] && pioneers && [ ! -e "$OUT" ]; check $? "uninstall with a v1.0 set (no SHA256SUMS): Pioneer's files back"
inst; cp -p "$SET/MacOS/rekordbox" "$C/MacOS/rekordbox"
uninst; [ $RC = 0 ] && pioneers && [ ! -e "$OUT" ]; check $? "uninstall repairs a half-restored rekordbox"
inst; cp -p "$SET/Frameworks/libonnxruntime.1.18.0.dylib" "$LIBF"     # v1.0's restore order: the library first
uninst; [ $RC = 0 ] && pioneers; check $? "uninstall repairs rekordbox when the bridge is gone but it doesn't verify"
inst; cp -p "$SET/_CodeSignature/CodeResources" "$C/_CodeSignature/CodeResources"
inst 1; [ $RC = 0 ] && ours && set_ok; check $? "install repairs a half-changed rekordbox first"
reset "repairs"

# MARK: kill -9 after every step, then the same action again finishes

for act in install uninstall; do
  k=1
  while [ $k -lt 40 ]; do
    [ $act = uninstall ] && inst
    HOOKS=("$k|kill -9 \$\$")
    if [ $act = install ]; then inst; else uninst; fi
    HOOKS=()
    if [ $RC != 137 ]; then [ $RC = 0 ]; check $? "$act: the sweep reached the end after $((k - 1)) steps"; break; fi
    at="$(laststep)"
    safe; check $? "$act killed at step $k ($at): rekordbox is AlphaTheta's, or its saved set is complete"
    if [ $act = install ]; then inst; [ $RC = 0 ] && ours && set_ok && no_lock
    else uninst; [ $RC = 0 ] && pioneers && [ ! -e "$OUT" ] && no_lock; fi
    check $? "$act killed at step $k ($at), run again: finished"
    reset "$act killed at step $k"
    k=$((k + 1))
  done
done
HOOKS=("signed|exit 1"); inst; HOOKS=(); n="$(steps)"
reset "rollback count"
HOOKS=("signed|exit 1" "$((n - 1))|kill -9 \$\$"); inst; HOOKS=()
[ $RC = 137 ] && set_ok; check $? "install killed during its rollback ($(laststep))"
inst; [ $RC = 0 ] && ours && set_ok; check $? "then install again: repaired and installed"
reset "killed during the rollback"

# MARK: the end: rekordbox exactly as at the start, nothing of ours left

manifest > "$WORK/manifest.end"
diff "$WORK/manifest.start" "$WORK/manifest.end"; check $? "rekordbox.app is exactly as at the start (names, owners, modes, links, contents)"
[ "$(ls -a "$SUPPORT")" = "$SUPPORT0" ] && [ ! -e "$OUT" ] && no_lock; check $? "the root folder and the lock are gone, nothing else changed next to them"
summary

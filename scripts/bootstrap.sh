#!/bin/bash
# RB Stems Plus installer: puts "RB Stems Plus.app" in /Applications, signs it on this Mac, opens it.
# Everything else happens in the app. Started by pasting into Terminal:
#
#   curl -fsSL https://github.com/Gabe-LS/rbstemsplus/releases/latest/download/bootstrap.sh | bash
#
# What it does, in order:
#   1. checks that what it would replace in /Applications is RB Stems Plus: a real app folder
#      (not a link) with its bundle ID; anything else there stops it, untouched;
#   2. downloads payload.json and its signature, payload.json.sig, and stops unless macOS's own
#      openssl verifies the signature with one of the public keys below (the app's keys), and
#      unless no key in it is given twice (readers disagree on which one counts);
#   3. stops if payload.json is older than this script's own version (PAYLOAD_MINIMUM);
#   4. reads the app zip's name and SHA-256 from payload.json, downloads the zip and refuses to
#      go on unless its SHA-256 matches;
#   5. unpacks it with ditto into a temporary folder and checks it is RB Stems Plus, with no link
#      or special file inside;
#   6. quits a running RB Stems Plus (not its background watcher, RB Stems Plus Watcher, nothing
#      else), writes the app's busy marker (so its watcher and update leave the app alone),
#      replaces the app in /Applications, signs it ad hoc on this Mac (hardened runtime: nothing
#      can inject code into it; the watcher helper inside it first, then the app), re-registers
#      it with macOS and opens it. The app then registers its watcher again for the new code.
# No password and no sudo: /Applications is writable by admin accounts. curl sets no quarantine,
# so Gatekeeper doesn't stop the app.
#
# The limit: this script itself is trusted as pasted, over GitHub's HTTPS. The signature
# protects what it downloads, not the script.
#
# Test override: RBSTEMSPLUS_BASE_URL replaces the release URL, e.g. a local test server:
#   curl -fsSL http://192.168.64.1:8765/bootstrap.sh | RBSTEMSPLUS_BASE_URL=http://192.168.64.1:8765/ bash
set -euo pipefail

# The release the app is downloaded from (one GitHub Release per version). Must match
# downloadBase in app/Sources/Paths.swift.
BASE_URL="https://github.com/Gabe-LS/rbstemsplus/releases/latest/download/"
BUNDLE_ID="io.github.rbstemsplus.app"
APP_NAME="RB Stems Plus"
APP="/Applications/$APP_NAME.app"
# The background watcher: a helper app inside the app (app/Sources/Paths.swift, watcherBundleID
# and watcherHelperPath). Never quit here.
WATCHER_ID="io.github.rbstemsplus.watcher"
WATCHER_HELPER="Contents/Helpers/RB Stems Plus Watcher.app"
# This script's staging names, next to the app. Not the app's own update's (.RB Stems Plus.update.app
# and .RB Stems Plus.old.app, app/Sources/Paths.swift): the two never use each other's.
NEW="/Applications/.$APP_NAME.new.app"
OLD="/Applications/.$APP_NAME.bootstrap-old.app"
# The public keys payload.json's signature is checked with (base64 of their DER, as in
# keys/*.pub.pem): written in by scripts/build.sh, the release key and the backup key (a test
# build: its test key). Either one is enough. Empty here: run only the copy a build made.
SIGNING_KEYS=""
# The oldest payload_version this script installs: its own release's version (VERSION), written
# in by scripts/build.sh; the app has the same floor, compiled in. A signed older release can't be
# served to it to undo a fix. Empty here: run only the copy a build made.
PAYLOAD_MINIMUM=""
# The app's busy marker (app/Sources/State.swift): "<pid>\n<start>\n", the writer's pid and its
# start time in seconds since 1970, then (the app's only) the action's id and label.
BUSY="$HOME/Library/Application Support/rbstemsplus/busy"
tmp=""; MARKED=0; OWN_START=""; SAVED_BUSY=""

fail() { echo "RB Stems Plus wasn't installed: $*" >&2; exit 1; }

# Whether payload.json ($1) carries a valid signature ($2: DER, ECDSA P-256 with SHA-256) by one
# of the public keys ($4...: base64 DER), checked with macOS's own openssl. $3: a folder for the
# key files. An empty or missing signature, or no key, is no.
verify_payload() {
  local json="$1" sig="$2" dir="$3" key n=0
  shift 3
  [ -f "$json" ] && [ -s "$sig" ] || return 1
  for key in "$@"; do
    [[ "$key" =~ ^[A-Za-z0-9+/]+=*$ ]] || continue
    n=$((n + 1))
    { echo "-----BEGIN PUBLIC KEY-----"; echo "$key" | /usr/bin/fold -w 64; echo "-----END PUBLIC KEY-----"; } > "$dir/key$n.pem"
    if /usr/bin/openssl dgst -sha256 -verify "$dir/key$n.pem" -signature "$sig" "$json" > /dev/null 2>&1; then
      return 0
    fi
  done
  return 1
}

# Whether the file $1 is one JSON object in which no object, at any level, has the same key
# twice, and every key is printable ASCII without escapes: the app's own check (plainJSONKeys in
# app/Sources/Payload.swift). JSON readers disagree about a key given twice (plutil, used below,
# takes the last; the app the first), so payload.json must have none. Values are only skipped:
# plutil checks them. macOS's own awk, byte by byte.
plain_json() {
  LC_ALL=C /usr/bin/awk '
    function ws() { while (i <= n && index(" \t\n\r", substr(s, i, 1)) > 0) i++ }
    function str(   start, c) {
      if (substr(s, i, 1) != "\"") return 0
      i++; start = i
      while (i <= n) {
        c = substr(s, i, 1)
        if (c == "\"") { STR = substr(s, start, i - start); i++; return 1 }
        if (c == "\\") { i += 2; continue }
        if (c < " ") return 0
        i++
      }
      return 0
    }
    function val(depth,   c, id, start) {
      ws()
      if (depth >= 32 || i > n) return 0
      c = substr(s, i, 1)
      if (c == "{") {
        i++; id = ++objects; ws()
        if (substr(s, i, 1) == "}") { i++; return 1 }
        while (1) {
          ws()
          if (!str() || STR == "" || STR ~ /[^ -~]/ || index(STR, "\\") > 0 || ((id, STR) in keys)) return 0
          keys[id, STR] = 1
          ws(); if (substr(s, i, 1) != ":") return 0
          i++
          if (!val(depth + 1)) return 0
          ws(); c = substr(s, i, 1); i++
          if (c == ",") continue
          return c == "}"
        }
      }
      if (c == "[") {
        i++; ws()
        if (substr(s, i, 1) == "]") { i++; return 1 }
        while (1) {
          if (!val(depth + 1)) return 0
          ws(); c = substr(s, i, 1); i++
          if (c == ",") continue
          return c == "]"
        }
      }
      if (c == "\"") return str()
      start = i
      while (i <= n && index("+-.0123456789Eaeflnrstu", substr(s, i, 1)) > 0) i++
      return i > start
    }
    { s = s $0 "\n" }
    END {
      n = length(s); i = 1; ws()
      if (substr(s, i, 1) != "{" || !val(0)) exit 1
      ws(); exit (i > n ? 0 : 1)
    }' "$1"
}

# Whether version $1 is older than version $2 (digits and dots: 1.0.9 is older than 1.0.10). Yes
# also when either isn't such a version, so the caller refuses it.
older_version() {
  local IFS=. i
  [[ "$1" =~ ^[0-9]{1,9}(\.[0-9]{1,9}){0,9}$ ]] && [[ "$2" =~ ^[0-9]{1,9}(\.[0-9]{1,9}){0,9}$ ]] || return 0
  local -a a=($1) b=($2)
  for ((i = 0; i < ${#a[@]} || i < ${#b[@]}; i++)); do
    if ((10#${a[i]:-0} < 10#${b[i]:-0})); then return 0; fi
    if ((10#${a[i]:-0} > 10#${b[i]:-0})); then return 1; fi
  done
  return 1
}

# Whether $1 is RB Stems Plus: a real folder (not a link) whose Info.plist has our bundle ID.
# Only such a folder is ever replaced or deleted.
is_ours() {
  [ -d "$1" ] && [ ! -L "$1" ] || return 1
  [ "$(plutil -extract CFBundleIdentifier raw -o - "$1/Contents/Info.plist" 2>/dev/null)" = "$BUNDLE_ID" ]
}

# Stops, touching nothing, if anything at the places this script replaces or deletes isn't
# RB Stems Plus. Checked before the download and again right before the install. The two
# staging names are hidden (Finder shows them after Command-Shift-Period); ours there are
# removed by the install itself.
check_places() {
  local p
  for p in "$APP" "$NEW" "$OLD"; do
    if [ -e "$p" ] || [ -L "$p" ]; then
      is_ours "$p" && continue
      if [ "$p" = "$APP" ]; then
        fail "$p isn't RB Stems Plus (or is a link), so it was left as it is. Nothing was changed. Move it out of the Applications folder, then paste the install command again."
      fi
      fail "$p isn't RB Stems Plus (or is a link), so it was left as it is. Nothing was changed. In Finder, open the Applications folder and press Command-Shift-Period to show hidden items, move \"${p##*/}\" out of it, then paste the install command again."
    fi
  done
}

# When process $1 started, in whole seconds since 1970 (the kernel's p_starttime, as the app
# records it); nothing if there is no such process. In UTC, so no hour is ambiguous.
started() {
  set -- $(TZ=UTC0 LC_ALL=C /bin/ps -p "$1" -o lstart= 2>/dev/null)
  [ $# -eq 5 ] && TZ=UTC0 LC_ALL=C /bin/date -j -f '%a %b %e %T %Y' "$*" +%s 2>/dev/null
}

# Stops, changing nothing, while RB Stems Plus is working: its busy marker's writer still runs
# (the app during an action, or its update replacing the app), i.e. a process with the marker's
# pid that started when the marker says. A marker in 1.0's format has no start time: its writer
# is a process with that pid that had started when the file was written; one that got the pid
# later doesn't count (as in the app). Checked whether or not the app is open, before the
# download and again right before the install.
check_busy() {
  local pid="" start="" now=""
  [ -f "$BUSY" ] || return 0
  { IFS= read -r pid || true; IFS= read -r start || true; } < "$BUSY" 2>/dev/null || return 0
  pid="${pid//[[:space:]]/}"; start="${start//[[:space:]]/}"
  [[ "$pid" =~ ^[0-9]{1,9}$ ]] && [ "$pid" != 0 ] || return 0
  now="$(started "$pid")" || now=""
  [ -n "$now" ] || return 0
  if [[ "$start" =~ ^[0-9]{1,12}$ ]]; then
    [ "$now" = "$start" ] || return 0
  else
    [ "$now" -le "$(stat -f %m "$BUSY" 2>/dev/null || printf 0)" ] || return 0
  fi
  fail "RB Stems Plus is changing rekordbox or updating itself. Nothing was changed. Wait until it has finished, then paste the install command again."
}

# While this script replaces and signs the app, the busy marker names it (its pid and start time,
# as the app writes them), so the app's watcher and its update leave the app alone. The marker
# that was there (the app's, left by a run that quit half-way: its next launch offers to finish
# that action) is kept: its action goes into this script's marker, and unmark_busy puts it back
# exactly as it was. If this script is stopped, its marker names a process that is gone, with
# the same action. Without a start time (ps failed), no marker is written.
mark_busy() {
  local l1="" l2="" l3="" l4="" action=""
  OWN_START="$(started $$)" || OWN_START=""
  [[ "$OWN_START" =~ ^[0-9]+$ ]] || return 0
  mkdir -p "${BUSY%/*}" 2>/dev/null || return 0
  if [ -f "$BUSY" ] && [ ! -L "$BUSY" ]; then
    SAVED_BUSY="$(cat "$BUSY" 2>/dev/null; printf x)"; SAVED_BUSY="${SAVED_BUSY%x}"
    { IFS= read -r l1 || true; IFS= read -r l2 || true; IFS= read -r l3 || true; IFS= read -r l4 || true; } < "$BUSY" 2>/dev/null || true
    if [[ "${l2//[[:space:]]/}" =~ ^[0-9]+$ ]]; then [ -z "$l3" ] || action="$l3"$'\n'"$l4"$'\n'
    else [ -z "$l2" ] || action="$l2"$'\n'"$l3"$'\n'; fi
  fi
  if printf '%s\n%s\n%s' "$$" "$OWN_START" "$action" > "$BUSY.$$" && mv -f "$BUSY.$$" "$BUSY"; then MARKED=1
  else rm -f "$BUSY.$$" 2>/dev/null || true; fi
}

# The marker as it was before mark_busy (or none), if it is still this script's.
unmark_busy() {
  local l1="" l2=""
  [ "$MARKED" = 1 ] || return 0
  MARKED=0
  { IFS= read -r l1 || true; IFS= read -r l2 || true; } < "$BUSY" 2>/dev/null || return 0
  [ "$l1" = "$$" ] && [ "$l2" = "$OWN_START" ] || return 0
  if [ -n "$SAVED_BUSY" ]; then
    { printf '%s' "$SAVED_BUSY" > "$BUSY.$$" && mv -f "$BUSY.$$" "$BUSY"; } || rm -f "$BUSY.$$" 2>/dev/null || true
  else
    rm -f "$BUSY" 2>/dev/null || true
  fi
}

# At exit, however it exits: the marker as it was, and no temporary files.
cleanup() {
  unmark_busy
  [ -z "$tmp" ] || rm -rf "$tmp"
}

# The pids of the running RB Stems Plus window app. The background watcher is left alone: it
# keeps running the old code until it exits. It is the helper app RB Stems Plus Watcher (its own
# bundle ID and process name), or, in 1.0, the app's own executable with --watch.
app_pids() {
  local pid id
  for pid in $(pgrep -x "$APP_NAME" || true); do
    id="$(lsappinfo info -only bundleID "$pid" 2>/dev/null)" || id=""
    case "$id" in *"\"$WATCHER_ID\""*) continue ;; esac
    case "$id" in *"\"$BUNDLE_ID\""*) ;; *) continue ;; esac
    case " $(ps -o args= -p "$pid" 2>/dev/null) " in *" --watch "*) continue ;; esac
    echo "$pid"
  done
}

# Signs the app $1 ad hoc with the hardened runtime, inside out: the watcher helper first (if
# the app has one), then the app, whose signature seals the helper's (as scripts/build-app.sh).
sign_app() {
  if [ -d "$1/$WATCHER_HELPER" ]; then codesign -f -s - --options runtime "$1/$WATCHER_HELPER" || return 1; fi
  codesign -f -s - --options runtime "$1"
}

# Quits a running RB Stems Plus, unless it is changing rekordbox right now.
quit_app() {
  local pids pid i
  pids="$(app_pids)"
  [ -n "$pids" ] || return 0
  echo "Quitting RB Stems Plus…"
  # shellcheck disable=SC2086
  kill -TERM $pids 2>/dev/null || true
  for i in 1 2 3 4 5 6 7 8 9 10; do
    [ -n "$(app_pids)" ] || return 0
    sleep 1
  done
  fail "RB Stems Plus didn't quit. Nothing was changed. Quit it, then paste the install command again."
}

main() {
  local base="${RBSTEMSPLUS_BASE_URL:-$BASE_URL}"
  base="${base%/}/"
  local zip sha got id odd pv
  # -q: ignore ~/.curlrc (it could turn off certificate checks); HTTPS only, also after
  # redirects, except for a test server given in RBSTEMSPLUS_BASE_URL
  local curl_safe=(-q --proto =https --proto-redir =https)
  [ -z "${RBSTEMSPLUS_BASE_URL:-}" ] || curl_safe=(-q --proto =http,https --proto-redir =http,https)

  [ "$(uname -s)" = Darwin ] || fail "it needs a Mac."
  [ "$(sw_vers -productVersion | cut -d. -f1)" -ge 12 ] || fail "it needs macOS 12 or later."
  [ -w /Applications ] || fail "only an administrator can add apps to the Applications folder. Log in as an administrator, then paste the install command again."
  [ -n "$SIGNING_KEYS" ] && [ -n "$PAYLOAD_MINIMUM" ] \
    || fail "this copy of the install command has no signing keys or version. Use the install command from RB Stems Plus's README."
  check_places
  check_busy

  tmp="$(mktemp -d "${TMPDIR:-/tmp}/rbstemsplus.XXXXXX")"
  trap cleanup EXIT

  echo "Downloading RB Stems Plus…"
  curl "${curl_safe[@]}" -fsSL --retry 2 -o "$tmp/payload.json" "${base}payload.json" \
    || fail "couldn't download ${base}payload.json. Nothing was changed. Check your internet connection, then paste the install command again."
  curl "${curl_safe[@]}" -fsSL --retry 2 -o "$tmp/payload.json.sig" "${base}payload.json.sig" \
    || fail "couldn't download ${base}payload.json.sig. Nothing was changed. Check your internet connection, then paste the install command again."
  # shellcheck disable=SC2086
  verify_payload "$tmp/payload.json" "$tmp/payload.json.sig" "$tmp" $SIGNING_KEYS \
    || fail "the download couldn't be verified (payload.json isn't signed by RB Stems Plus). Nothing was changed. Try again later."
  plain_json "$tmp/payload.json" \
    || fail "the release's payload.json isn't valid (a key given twice, or not plain). Nothing was changed. Try again later."
  pv="$(plutil -extract payload_version raw -o - "$tmp/payload.json" 2>/dev/null)" || pv=""
  ! older_version "$pv" "$PAYLOAD_MINIMUM" \
    || fail "the release's payload (${pv:-no version}) is older than this install command ($PAYLOAD_MINIMUM). Nothing was changed. Try again later."
  zip="$(plutil -extract app.zip raw -o - "$tmp/payload.json" 2>/dev/null)" || zip=""
  sha="$(plutil -extract app.sha256 raw -o - "$tmp/payload.json" 2>/dev/null)" || sha=""
  case "$zip" in ""|*/*|.*) fail "the release's payload.json has no valid app file name. Nothing was changed. Try again later." ;; esac
  [[ "$sha" =~ ^[0-9a-f]{64}$ ]] || fail "the release's payload.json has no valid app checksum. Nothing was changed. Try again later."

  curl "${curl_safe[@]}" -fSL --retry 2 --progress-bar -o "$tmp/$zip" "${base}$zip" \
    || fail "couldn't download ${base}$zip. Nothing was changed. Check your internet connection, then paste the install command again."
  got="$(shasum -a 256 "$tmp/$zip" | cut -d' ' -f1)"
  [ "$got" = "$sha" ] || fail "the download is damaged or isn't the published one (checksum mismatch). Nothing was changed. Try again later."

  mkdir "$tmp/x"
  ditto -x -k "$tmp/$zip" "$tmp/x" || fail "couldn't unpack the download. Nothing was changed. If the disk is full, free up some space, then paste the install command again."
  [ -d "$tmp/x/$APP_NAME.app" ] || fail "the download has no $APP_NAME.app. Nothing was changed. Try again later."
  id="$(plutil -extract CFBundleIdentifier raw -o - "$tmp/x/$APP_NAME.app/Contents/Info.plist" 2>/dev/null)" || id=""
  [ "$id" = "$BUNDLE_ID" ] || fail "the download isn't RB Stems Plus. Nothing was changed. Try again later."
  # the app has no links: one could lead outside it once installed
  odd="$(find "$tmp/x" ! -type f ! -type d -print -quit)" || fail "couldn't check the download. Nothing was changed. Paste the install command again."
  [ -z "$odd" ] || fail "the download contains a link or special file (${odd#"$tmp/x/"}). Nothing was changed. Try again later."

  check_places
  check_busy
  quit_app
  mark_busy
  echo "Installing RB Stems Plus in the Applications folder…"
  # the new app goes next to the old one first, then takes its place; on any failure the old
  # app stays (or is put back) and nothing is left half-copied
  local new="$NEW" old="$OLD"
  rm -rf "$new" "$old"
  ditto "$tmp/x/$APP_NAME.app" "$new" || { rm -rf "$new"; fail "couldn't copy the app to the Applications folder. Nothing was changed. If the disk is full, free up some space, then paste the install command again."; }
  if [ -e "$APP" ]; then
    mv "$APP" "$old" || { rm -rf "$new"; fail "couldn't replace the old RB Stems Plus. Nothing was changed. Paste the install command again."; }
  fi
  if ! mv "$new" "$APP"; then
    [ ! -e "$old" ] || mv "$old" "$APP"
    rm -rf "$new"; fail "couldn't put the app in the Applications folder. Nothing was changed. Paste the install command again."
  fi
  rm -rf "$old" || true
  # the new app is in place: a failure from here on can't leave the old one, so each says what to do
  sign_app "$APP" 2>/dev/null \
    || fail "couldn't sign the app on this Mac. Paste the install command again."
  # make macOS re-read the app (icon, name): replacing an app in place keeps the old record. Not
  # needed to open it, so a failure only goes on.
  /System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "$APP" || true
  touch "$APP" || true
  unmark_busy
  if open "$APP"; then
    echo "RB Stems Plus is in your Applications folder and open. You can close Terminal."
  else
    echo "RB Stems Plus is in your Applications folder, but it didn't open. Open it from the Applications folder. You can close Terminal."
  fi
}

# Everything runs from main, called on the last line: a download cut short runs nothing, and no
# command can read the rest of this script from curl's pipe.
main "$@" < /dev/null

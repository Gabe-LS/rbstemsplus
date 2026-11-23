#!/bin/bash
# A release's TEST build into the gate archive (release.md §6.1, §6.2: R4.I4, J14; T27), as
# scripts/release.sh does after it made the draft, or by hand when that step failed:
#
#   tests/perf/archive-release.sh vX.Y.Z [--wb] [--wp | --wp-if-present] [--payload-sha SHA256]
#
# The archive is ${GATES_ARCHIVE:-<checkout>/gates-archive}; its SHA256SUMS is checked first (a
# mismatch stops: nothing in it is ever rebuilt) and sealed anew at the end (tests/perf/archive.sh).
#   tag-vX.Y.Z/   dist-test/'s release files (the Mac app zip, the bridge, payload.json and its
#                 signature, bootstrap.sh, and the four Windows files when there are some), the
#                 test key pair that signed payload.json (build/test-keys/, or the earlier tag-*/
#                 test-keys/ a carried TEST build was signed with: the pair whose public half
#                 verifies payload.json.sig), build/test-build.log when there is one, and
#                 toolchain.txt. A later release's TEST build carries files from it, and G3 starts
#                 from it (docs/RELEASING.md §6). Already there with this payload.json: kept as it
#                 is; with another: refused (never replaced).
#   --wb          the first release with Windows files: WB/ (the Windows TEST files, the
#                 Windows app's baseline for the first-tagged metrics) and W0/worker/ (the
#                 worker, admin/RB Stems Plus.exe from that zip: the worker's W0, J13/J14)
#   --wp          the first release whose Windows zip ships Prepare (rbsp-prepare.exe): WP/
#                 (the baseline of the Prepare-tagged metrics)
#   --wp-if-present  release.sh uses this when the previous release didn't ship Prepare
# A baseline already there is kept, never replaced (said). --payload-sha: dist-test/payload.json
# must have this SHA-256 (the TEST build release.sh's gates read). The files are pinned before
# anything is moved in: dist-test/SHA256SUMS (scripts/build.sh) must list this payload.json, and
# the tag folder, staged once (or the one kept), must match it file by file and match
# payload.json's app, bridge (and windows.app, windows.bridge) SHA-256s, with payload.json.sig
# verifying; WB, WP and W0/worker are made from that folder. Windows files: payload.json has a
# windows block.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
die() { echo "archive-release.sh: $*" >&2; exit 1; }
usage() { echo "usage: $0 vX.Y.Z [--wb] [--wp | --wp-if-present] [--payload-sha SHA256]" >&2; exit 2; }
[ $# -ge 1 ] || usage
tag="$1"; shift
[[ "$tag" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "the tag must be vX.Y.Z, not '$tag'"
wb=0; wp=0; want_ps=""
while [ $# -gt 0 ]; do
  case "$1" in
    --wb) wb=1; shift ;;
    --wp) wp=1; shift ;;
    --wp-if-present) wp=auto; shift ;;
    --payload-sha) [ $# -ge 2 ] || usage; want_ps="$2"; shift 2 ;;
    *) usage ;;
  esac
done
A="${GATES_ARCHIVE:-$root/gates-archive}"
T="$root/dist-test"
. "$root/tests/perf/archive.sh"
[ -d "$A" ] || die "no gate archive at $A: point GATES_ARCHIVE at it (it is git-ignored: the main checkout's gates-archive/)"
lock="$A/.archive-release.lock"
mkdir "$lock" || die "the archive is locked or $lock can't be created"
trap 'if [ -n "${staging:-}" ]; then rm -rf "$staging"; fi; rmdir "$lock"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
verify || die "the gate archive ($A) doesn't match its SHA256SUMS: nothing archived (never rebuild a baseline)"
staging="$(mktemp -d "$A/.archive-release.partial.XXXXXX")" || die "can't stage files under $A"

MAC_FILES="rbstemsplus-app.zip libonnxruntime.1.18.0.dylib payload.json payload.json.sig bootstrap.sh"
WIN_FILES="rbstemsplus-windows.zip rbstemsplus-bridge-win-x64.dll bootstrap.ps1 NOTICE-windows.txt"
for f in $MAC_FILES SHA256SUMS; do [ -f "$T/$f" ] && [ ! -L "$T/$f" ] || die "dist-test/$f is missing: dist-test/ must hold $tag's TEST build"; done
# dist-test/SHA256SUMS, written by scripts/build.sh after payload.json: the TEST build's files
sums="$staging/SHA256SUMS"
pjs="$staging/payload.json"
cp "$T/SHA256SUMS" "$sums"
cp "$T/payload.json" "$pjs" && touch -r "$T/payload.json" "$pjs"
ps="$(shasum -a 256 "$pjs" | cut -d' ' -f1)"
[ -z "$want_ps" ] || [ "$ps" = "$want_ps" ] || die "dist-test/payload.json isn't the TEST build the gates read (${want_ps:0:12}): it is ${ps:0:12}"
sumof() { awk -v f="$1" '{ n = $0; sub(/^[0-9a-f]+ [ *]/, "", n) } n == f { print $1 }' "$sums"; }
[ "$(sumof payload.json)" = "$ps" ] || die "dist-test/SHA256SUMS doesn't list this payload.json (${ps:0:12}): it isn't the TEST build's"
v="$(plutil -extract payload_version raw -o - "$pjs" 2>/dev/null || true)"
[ -z "$v" ] || [ "v$v" = "$tag" ] || die "dist-test/payload.json is version $v, not ${tag#v}"
# pj KEY.KEY: a string in the payload.json snapshot, or nothing
pj() { python3 -I -c 'import json, sys
v = json.load(open(sys.argv[1]))
for k in sys.argv[2].split("."):
    v = v.get(k) if isinstance(v, dict) else None
print(v if isinstance(v, str) else "")' "$pjs" "$1" 2>/dev/null || true; }
haswin=0
python3 -I -c 'import json, sys; sys.exit(0 if isinstance(json.load(open(sys.argv[1])).get("windows"), dict) else 1)' "$pjs" 2>/dev/null && haswin=1
if [ "$haswin" = 1 ]; then
  for f in $WIN_FILES; do [ -f "$T/$f" ] && [ ! -L "$T/$f" ] || die "dist-test/$f is missing (the TEST build has Windows files)"; done
fi
[ "$wb$wp" = 00 ] || [ "$haswin" = 1 ] || die "Windows baseline options need Windows TEST files, and dist-test/ has none"

# the key pair that signed dist-test/payload.json
keys=""
for k in "$root/build/test-keys" "$A"/tag-*/test-keys; do
  [ -f "$k/test.pub.pem" ] && [ -f "$k/test-key.pem" ] || continue
  if /usr/bin/openssl dgst -sha256 -verify "$k/test.pub.pem" -signature "$T/payload.json.sig" "$pjs" > /dev/null 2>&1; then keys="$k"; break; fi
done
[ -n "$keys" ] || die "no test key pair here (build/test-keys/, $A/tag-*/test-keys/) verifies dist-test/payload.json.sig: archive it by hand (docs/RELEASING.md §6)"

toolchain() {   # toolchain.txt: what built it
  echo "tag=$tag commit=$(plutil -extract commit raw -o - "$pjs" 2>/dev/null || echo "?")"
  echo "built=$(stat -f %Sm -t %Y-%m-%dT%H:%M:%S "$pjs")"
  echo "payload_sha256=$ps"
  echo "swiftc=$(xcrun swiftc --version 2>&1 | head -1 || echo "?")"
  echo "xcode=$(xcodebuild -version 2>/dev/null | tr '\n' ' ' || echo "?")"
  echo "macos=$(sw_vers -productVersion 2>/dev/null || echo "?")"
}
check_kept() {   # an earlier seal may have failed after a folder was moved in
  local extra
  extra="$( (cd "$A" && archive_files "$1") | unlisted)" || die "couldn't check the manifest coverage of $A/$1"
  [ -z "$extra" ] || die "$A/$1 has files missing from SHA256SUMS; the archive needs a human:"$'\n'"$extra"
}
# stage NAME SRC FILES...: a new folder A/NAME from SRC's FILES, staged under this run's folder
# (seal leaves it out), or kept when it is there
made=""
stage() {
  local name="$1" src="$2" f; shift 2
  if [ -e "$A/$name" ]; then
    check_kept "$name"
    echo "archive-release.sh: $name is in the gate archive already: kept (a baseline is never replaced)"; return 0
  fi
  mkdir -p "$staging/$name"
  for f in "$@"; do cp -p "$src/$f" "$staging/$name/"; done
  toolchain > "$staging/$name/toolchain.txt"
  made="$made $name"
}

d="$A/tag-$tag"
# shellcheck disable=SC2086
files="$MAC_FILES $( [ "$haswin" = 0 ] || echo $WIN_FILES )"
if [ -e "$d" ]; then
  cmp -s "$d/payload.json" "$pjs" || die "$d is there with another payload.json: never replaced. If this release's TEST build is another one, the archive needs a human"
  check_kept "tag-$tag"
  echo "archive-release.sh: tag-$tag is in the gate archive already, with this payload.json: kept"
  src="$d"
else
  # shellcheck disable=SC2086
  stage "tag-$tag" "$T" $files
  src="$staging/tag-$tag"
  mkdir -p "$src/test-keys"
  cp -p "$keys/test-key.pem" "$keys/test.pub.pem" "$src/test-keys/"
  [ ! -f "$root/build/test-build.log" ] || cp -p "$root/build/test-build.log" "$src/build.log"
fi
# the tag folder's files are the TEST build's: dist-test/SHA256SUMS, payload.json's SHA-256s, the
# signature; everything after this is made from that folder, never from dist-test/ again
for f in $files; do
  [ "$f" != payload.json.sig ] || continue
  [ -f "$src/$f" ] && [ ! -L "$src/$f" ] && [ -n "$(sumof "$f")" ] && [ "$(shasum -a 256 "$src/$f" | cut -d' ' -f1)" = "$(sumof "$f")" ] \
    || die "tag-$tag/$f isn't the one dist-test/SHA256SUMS lists: nothing archived (the TEST build changed, or isn't whole)"
done
cmp -s "$src/payload.json" "$pjs" || die "tag-$tag/payload.json isn't the snapshot: nothing archived"
pins="app.sha256=rbstemsplus-app.zip bridge.sha256=libonnxruntime.1.18.0.dylib"
[ "$haswin" = 0 ] || pins="$pins windows.app.sha256=rbstemsplus-windows.zip windows.bridge.sha256=rbstemsplus-bridge-win-x64.dll"
for p in $pins; do
  want="$(pj "${p%%=*}")"
  [ -n "$want" ] && [ "$(shasum -a 256 "$src/${p#*=}" | cut -d' ' -f1)" = "$want" ] \
    || die "tag-$tag/${p#*=} isn't the file payload.json names (${p%%=*} ${want:-missing}): nothing archived"
done
/usr/bin/openssl dgst -sha256 -verify "$src/test-keys/test.pub.pem" -signature "$src/payload.json.sig" "$src/payload.json" > /dev/null 2>&1 \
  || die "tag-$tag/payload.json.sig doesn't verify with its test key (tag-$tag/test-keys/): nothing archived"
if [ "$haswin" = 1 ]; then
  # Read the entries in the checked copy, including their CRCs, before deciding on WP.
  prep_rc=0
  python3 -I -c 'import sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as z:
    prepare = False
    for i in z.infolist():
        if i.filename.endswith("/"):
            continue
        if any(ord(c) < 0x20 or ord(c) == 0x7f for c in i.filename):
            sys.exit("a zip entry name holds a control character")
        if i.filename == "rbsp-prepare.exe" or i.filename.endswith("/rbsp-prepare.exe"):
            prepare = True
        with z.open(i) as entry:
            while entry.read(1024 * 1024):
                pass
    sys.exit(0 if prepare else 3)' "$src/rbstemsplus-windows.zip" \
    || prep_rc=$?
  case "$prep_rc" in
    0) [ "$wp" != auto ] || wp=1 ;;
    3) [ "$wp" != 1 ] || die "--wp, but tag-$tag/rbstemsplus-windows.zip has no rbsp-prepare.exe: nothing archived" ;;
    *) die "tag-$tag/rbstemsplus-windows.zip can't be read as a zip: nothing archived" ;;
  esac
fi
if [ "$wb" = 1 ]; then
  # shellcheck disable=SC2086
  stage WB "$src" $WIN_FILES payload.json payload.json.sig
  if [ -e "$A/W0/worker" ]; then
    check_kept W0/worker
    echo "archive-release.sh: W0/worker is in the gate archive already: kept (a baseline is never replaced)"
  else
    mkdir -p "$staging/W0/worker"
    python3 -I -c 'import sys, zipfile
z = zipfile.ZipFile(sys.argv[1]); n = [i for i in z.namelist() if i == "admin/RB Stems Plus.exe"]
if len(n) != 1:
    sys.exit("the zip has no single admin/RB Stems Plus.exe")
open(sys.argv[2], "wb").write(z.read(n[0]))' "$src/rbstemsplus-windows.zip" "$staging/W0/worker/RB Stems Plus.exe" \
      || die "tag-$tag/rbstemsplus-windows.zip's worker (admin/RB Stems Plus.exe) couldn't be read: nothing archived"
    toolchain > "$staging/W0/worker/toolchain.txt"
    made="$made W0/worker"
  fi
fi
if [ "$wp" = 1 ]; then
  # shellcheck disable=SC2086
  stage WP "$src" $WIN_FILES payload.json payload.json.sig
fi
for n in $made; do
  parent="$A/$(dirname "$n")"
  mkdir -p "$parent"
  # Move into the parent so an existing directory is a collision, never a container.
  mv -n "$staging/$n" "$parent/" && [ ! -e "$staging/$n" ] || die "could not move $n into $parent without replacing an existing destination"
done
# shellcheck disable=SC2086
seal $made || die "the archive's SHA256SUMS couldn't be sealed: $A holds$made unsealed"
verify || die "the archive doesn't match the SHA256SUMS just sealed"
echo "archive-release.sh: archived${made:- nothing new} in $A (SHA256SUMS sealed). Copy it to the second disk: tests/perf/b0.sh copy DEST"

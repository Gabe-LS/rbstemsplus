#!/bin/bash
# Build the bootstrap and update suites' fixtures for the checkout's planned TEST build.
# Monday 09:35, from the release checkout after release.sh --plan
# Use the chosen B1 host for TEST_HOST.
#   TEST_HOST=<B1 host>
#   TEST_PORT=8767
#   tests/win/suites/build-update-fixtures.sh --build-rc --test http://192.168.64.1:8765/ --win-test "https://${TEST_HOST}:${TEST_PORT}"
# Monday 10:00, in the same shell
#   tests/win/suites/build-update-fixtures.sh 1.4.0
#   tests/win/vm-run.sh stage --build
#   mkdir -p build/win/dist-test
#   cp -R build/update-fixtures/dist-test/. build/win/dist-test/
#   VMRUN_HTTP_BIND="$TEST_HOST" VMRUN_HTTP_PORT="$TEST_PORT" tests/win/vm-run.sh run "clean Windows" update --record
# --build-rc uses build/gates/<HEAD>/plan and records its TEST inputs for fixture generation.
# Output: build/update-fixtures/dist-test (must be absent). OLD_VERSION plus one patch must be
# below HEAD's VERSION. The older app uses the RC host's /current; the RC uses the server root.
# Use --local OLD_VERSION for the standalone Parallels experiment that rebuilds both apps.
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd -P)"
die() { echo "build-update-fixtures.sh: $*" >&2; exit 2; }
usage() {
  die 'usage: build-update-fixtures.sh [--local] OLD_VERSION | --build-rc --test URL --win-test URL'
}
sha() { shasum -a 256 "$1" | cut -d' ' -f1; }
head="$(git -C "$root" rev-parse HEAD)"
version="$(git -C "$root" show "$head:VERSION" | tr -d '[:space:]')"
plan="$root/build/gates/$head/plan"
inputs="${plan%/*}/test-build-inputs"
candidate="$root/dist-test"

if [ "${1:-}" = --build-rc ]; then
  [ "$#" = 5 ] && [ "$2" = --test ] && [ -n "$3" ] && [ "$4" = --win-test ] || usage
  mac_url="$3"
  win_url="${5%/}"
  [[ "$win_url" =~ ^https://[A-Za-z0-9.-]+(:[0-9]+)?$ ]] || usage
  [ -f "$plan" ] || die "no release plan at $plan"
  plan_sha="$(sha "$plan")"
  archive="${GATES_ARCHIVE:-$root/gates-archive}"
  case "$archive" in /*) ;; *) archive="$PWD/$archive" ;; esac
  rm -f "$inputs"
  GATES_ARCHIVE="$archive" "$root/scripts/build.sh" --test "$mac_url" --win-test "$win_url" --plan "$plan"
  [ "$(sha "$plan")" = "$plan_sha" ] || die 'release plan changed during the TEST build'
  [ -f "$candidate/payload.json" ] && [ -f "$candidate/payload.json.sig" ] || die "no TEST build at $candidate"
  payload_sha="$(sha "$candidate/payload.json")"
  part="$(mktemp "$inputs.XXXXXX")"
  trap 'rm -f "$part"' EXIT
  printf '%s\n' "$mac_url" "$win_url" "$archive" "$plan_sha" "$payload_sha" > "$part"
  mv -f "$part" "$inputs"
  echo "Recorded RC TEST build inputs in $inputs"
  exit 0
fi

local_build=0
if [ "${1:-}" = --local ]; then local_build=1; shift; fi
[ "$#" = 1 ] && [[ "$1" != -* ]] || usage
old="$1"
if [ "$local_build" = 1 ]; then
  mac_url=http://10.211.55.2:8766/current/
  win_url=https://10.211.55.2:8766/current
else
  [ -f "$inputs" ] || die 'no RC TEST build inputs; run --build-rc first'
  {
    IFS= read -r mac_url && IFS= read -r win_url && IFS= read -r archive &&
      IFS= read -r plan_sha && IFS= read -r payload_sha
  } < "$inputs" || die "incomplete TEST build inputs at $inputs"
  [ -f "$plan" ] && [ "$(sha "$plan")" = "$plan_sha" ] || die 'release plan changed; run --build-rc again'
  [ -f "$candidate/payload.json" ] && [ "$(sha "$candidate/payload.json")" = "$payload_sha" ] \
    || die 'RC payload changed; run --build-rc again'
  [ -f "$candidate/payload.json.sig" ] || die "no TEST build at $candidate"
  grep -qx 'windows\.app new' "$plan" || die 'the update suite needs a plan with a new Windows app'
  win_url="$win_url/current"
fi

carried="$(python3 -I - "$old" "$version" <<'PY'
import re
import sys

def version(s):
    if not re.fullmatch(r'(0|[1-9][0-9]*)(\.(0|[1-9][0-9]*)){2}', s):
        sys.exit(f'expected x.y.z, got {s!r}')
    return tuple(map(int, s.split('.')))

major, minor, patch = version(sys.argv[1])
carried = (major, minor, patch + 1)
if carried >= version(sys.argv[2]):
    sys.exit('need OLD_VERSION < carried version < HEAD VERSION')
print('.'.join(map(str, carried)))
PY
)" || die 'choose a lower OLD_VERSION with a patch version in between'

parent="$root/build/update-fixtures"
mkdir -p "$parent"
out="$parent/dist-test"
[ ! -e "$out" ] || die "$out already exists"
scratch="$(mktemp -d "$parent/.build.XXXXXX")"
trap 'rm -rf "$scratch"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
repo="$scratch/source"
fixture="$scratch/dist-test"
git -c core.hooksPath=/dev/null clone -q --shared --no-checkout "$root" "$repo"
git -C "$repo" -c core.hooksPath=/dev/null checkout -q --detach "$head"
grep -q '^function Test-RbspAppVersion(' "$repo/scripts/bootstrap.ps1" ||
  die "HEAD's bootstrap has no app version check"
# The clone builds with this checkout's pinned dependencies and its test keys: a fresh clone has
# no build/, and a test-keys/ of its own would sign with a new key and make a new TLS CA, while
# vm-run.sh serves HTTPS with this checkout's build/test-keys/win-tls certificate.
mkdir -p "$repo/build"
for d in blocksrt dl dotnet dotnet-home flac flaui helper-win-deps llvm-mingw nuget pocketfft pyrekordbox sqlcipher test-keys; do
  [ ! -e "$root/build/$d" ] || ln -s "$(cd "$root/build/$d" && pwd -P)" "$repo/build/$d"
done

if [ "$local_build" = 1 ]; then
  "$repo/scripts/build.sh" --test "$mac_url" --windows --win-test "$win_url"
  mv "$repo/dist-test" "$fixture"
else
  cp "$plan" "$scratch/rc.plan"
  [ "$(sha "$scratch/rc.plan")" = "$plan_sha" ] || die 'release plan changed during fixture generation'
  mkdir "$fixture"
  for f in "$candidate"/*; do [ ! -f "$f" ] || cp -p "$f" "$fixture/"; done
  [ "$(sha "$fixture/payload.json")" = "$payload_sha" ] || die 'RC payload changed during the copy'
fi
mkdir "$fixture/new"
# Keep a whole copy of the candidate at the root for bootstrap and at new/ for update.
for f in "$fixture"/*; do [ ! -f "$f" ] || cp -p "$f" "$fixture/new/"; done

# Build a genuine older app and change only the stub's inert resource for UP3. The build
# stamps the app from VERSION. The committed release source and its stub are left alone.
printf '%s\n' "$old" > "$repo/VERSION"
printf '\n4101 RCDATA BEGIN "RBSP old TEST update fixture" END\n' >> "$repo/win/c/stub.rc"
git -C "$repo" -c core.hooksPath=/dev/null add VERSION win/c/stub.rc
git -C "$repo" -c core.hooksPath=/dev/null -c user.name='RBSP TEST fixtures' \
  -c user.email=fixtures@example.invalid commit -qm "Stamp the old Windows TEST fixture $old"
stage_args=()
if [ "$local_build" = 1 ]; then
  RBSTEMSPLUS_MODEL="${RBSTEMSPLUS_MODEL:-$fixture/stemsplus-model.onnx}" \
    "$repo/scripts/build.sh" --test "$mac_url" --windows --win-test "$win_url"
else
  # The synthetic release builds every file at its older version. Keep the RC plan's
  # previous release so build.sh uses the same TEST inputs from the gate archive.
  old_plan="$scratch/old.plan"
  old_head="$(git -C "$repo" rev-parse HEAD)"
  awk -v commit="$old_head" -v version="$old" '
    $1 ~ /^#/ { next }
    $1 == "commit" { print $1, commit; next }
    $1 == "version" { print $1, version; next }
    $1 ~ /^(mac|windows)\./ { print $1, "new"; next }
    { print }
  ' "$scratch/rc.plan" > "$old_plan"
  RBSTEMSPLUS_MODEL="${RBSTEMSPLUS_MODEL:-$fixture/stemsplus-model.onnx}" \
    GATES_ARCHIVE="$archive" "$repo/scripts/build.sh" --test "$mac_url" --win-test "$win_url" --plan "$old_plan"
  previous="$(awk '$1 == "previous" { print $2 }' "$old_plan")"
  if [ "$previous" != none ]; then
    stage_args=(--key "$archive/tag-$previous/test-keys")
  fi
fi
mv "$repo/dist-test" "$fixture/a"
"$repo/tests/win/suites/stage-dist.sh" "$scratch" ${stage_args[@]+"${stage_args[@]}"}

python3 -I - "$fixture" "$head" "$old" "$carried" "$version" <<'PY'
import json
from pathlib import Path
import sys

root, head, old, carried, new = sys.argv[1:]
root = Path(root)

def release(folder, payload_version, app_version):
    d = root / folder
    with (d / 'payload.json').open() as f:
        payload = json.load(f)
    app = payload['windows']['app']
    if (payload['payload_version'], app['version']) != (payload_version, app_version):
        sys.exit(f'{folder}: unexpected payload or Windows app version')
    if not (d / 'bootstrap.ps1').is_file() or not (d / app['zip']).is_file():
        sys.exit(f'{folder}: bootstrap or Windows zip missing')
    return payload

a = release('a', old, old)
c = release('carried', carried, old)
n = release('new', new, new)
if release('.', new, new) != n or n['commit'] != head:
    sys.exit('new/ and bootstrap root must be the HEAD candidate')
if any(a['windows']['app']['stub_sha256'][arch] == n['windows']['app']['stub_sha256'][arch]
       for arch in ('arm64', 'x64')):
    sys.exit('a/ and new/ must have different stubs on both architectures')
c['payload_version'] = old
if a != c:
    sys.exit('carried/ must differ from a/ only in payload_version')

with (root / 'bad-version' / 'payload.json').open() as f:
    bad = json.load(f)
if bad['windows']['app']['version'] == new:
    sys.exit('bad-version/ must name another Windows app version')
bad['windows']['app']['version'] = new
if bad != n:
    sys.exit('bad-version/ must differ from the candidate only in windows.app.version')
PY
# rename into an absent path, even when two fixture builders finish together
python3 -I - "$fixture" "$out" <<'PY'
import os
import sys
os.rename(sys.argv[1], sys.argv[2])
PY
echo "Windows TEST fixtures: a=$old carried=$carried new=$version and bad-version in $out"

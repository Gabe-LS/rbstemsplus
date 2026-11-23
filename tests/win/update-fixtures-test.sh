#!/bin/bash
# Offline fixture builder tests. The build and stage commands produce small stand-ins.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
t="$(mktemp -d "${TMPDIR:-/tmp}/rbsp-update-fixtures.XXXXXX")"
trap 'rm -rf "$t"' EXIT
mkdir -p "$t"/{scripts,win/c,tests/win/suites}
builder="$t/tests/win/suites/build-update-fixtures.sh"
cp "${1:-$here/suites/build-update-fixtures.sh}" "$builder"
chmod +x "$builder"
printf 'build/\ndist-test/\n' > "$t/.gitignore"
echo 1.4.0 > "$t/VERSION"
echo 'constant stub' > "$t/win/c/stub.rc"
echo 'function Test-RbspAppVersion() {}' > "$t/scripts/bootstrap.ps1"
cat > "$t/scripts/build.sh" <<'SH'
#!/bin/bash
set -eu
cd "$(dirname "$0")/.."
[ -z "$(git status --porcelain)" ]
mkdir dist-test
v="$(cat VERSION)"
if [ "$v" = 1.3.0 ]; then
  model="${RBSTEMSPLUS_MODEL:-dist/stemsplus-model.onnx}"
  if [ ! -f "$model" ]; then
    echo 'FAIL BSR9: the old build cannot find the model in its scratch checkout' >&2
    exit 1
  fi
  cp "$model" dist-test/stemsplus-model.onnx
else
  printf 'RC model fixture\n' > dist-test/stemsplus-model.onnx
fi
printf '%s %s\n' "$v" "$*" >> "$FIXTURE_BUILD_LOG"
printf '%s %s\n' "$v" "${GATES_ARCHIVE:-}" >> "$FIXTURE_BUILD_LOG.archive"
while [ "$#" -gt 0 ]; do
  case "$1" in
    --plan)
      grep -qxF "commit $(git rev-parse HEAD)" "$2"
      grep -qxF "version $v" "$2"
      cp "$2" "$FIXTURE_BUILD_LOG.$v.plan"
      shift 2 ;;
    --windows) shift ;;
    --test|--win-test) shift 2 ;;
    *) exit 2 ;;
  esac
done
stub="$(shasum -a 256 win/c/stub.rc | cut -d' ' -f1)"
printf '{"commit":"%s","payload_version":"%s","windows":{"app":{"version":"%s","zip":"windows.zip","stub_sha256":{"x64":"%s","arm64":"%s"}}}}\n' \
  "$(git rev-parse HEAD)" "$v" "$v" "$stub" "$stub" > dist-test/payload.json
cp scripts/bootstrap.ps1 dist-test/
printf 'Windows zip for %s, build %s\n' "$v" "$(wc -l < "$FIXTURE_BUILD_LOG")" > dist-test/windows.zip
printf 'fixture signature for %s\n' "$v" > dist-test/payload.json.sig
SH
cat > "$t/tests/win/suites/stage-dist.sh" <<'SH'
#!/bin/bash
set -eu
printf '%s\n' "${*:2}" >> "$FIXTURE_STAGE_LOG"
cd "$1/dist-test"
mkdir bad-version
cp -R a carried
cp bootstrap.ps1 windows.zip bad-version/
sed 's/"payload_version":"1.3.0"/"payload_version":"1.3.1"/' a/payload.json > carried/payload.json
sed 's/"version":"1.4.0"/"version":"0.0.0"/' payload.json > bad-version/payload.json
SH
chmod +x "$t/scripts/build.sh" "$t/tests/win/suites/stage-dist.sh"
git -C "$t" init -q
git -C "$t" add .
git -C "$t" -c user.name=Fixture -c user.email=fixture@example.invalid -c core.hooksPath=/dev/null commit -qm fixture
head="$(git -C "$t" rev-parse HEAD)"
cd -P "$t"
t="$PWD"
export FIXTURE_BUILD_LOG="$t/build/build.log" FIXTURE_STAGE_LOG="$t/build/stage.log"
plan="build/gates/$head/plan"
mkdir -p "$(dirname "$plan")"
cat > "$plan" <<EOF
plan 1
commit $head
version 1.4.0
previous v1.2.0 aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
mac.app new
mac.bridge carry $head bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb
windows.app new
windows.stub.x64 new
windows.stub.arm64 new
windows.admin new
windows.bridge new
EOF
mac_url=http://127.0.0.1:4785/
win_url=https://127.0.0.1:4786
archive='build/previous TEST builds'

reject() {
  local reason="$1"
  shift
  if "$builder" "$@" > build/refused.log 2>&1 || ! grep -qF "$reason" build/refused.log; then
    cat build/refused.log
    echo "FAIL: expected refusal mentioning $reason" >&2
    exit 1
  fi
}
reject 'TEST build inputs' 1.3.0
[ ! -e "$FIXTURE_BUILD_LOG" ]
echo 'PASS B2: missing RC inputs refuse a build without --local'

GATES_ARCHIVE="$archive" "$builder" --build-rc --test "$mac_url" --win-test "$win_url/"
cmp "$plan" "$FIXTURE_BUILD_LOG.1.4.0.plan"
grep -qFx "1.4.0 --test $mac_url --win-test $win_url --plan $t/$plan" "$FIXTURE_BUILD_LOG"
echo 'PASS B2: the RC build uses the release plan and supplied TEST URLs'
cp "$plan" build/original-plan
echo '# changed after the build' >> "$plan"
reject 'release plan changed; run --build-rc again' 1.3.0
mv build/original-plan "$plan"
cp dist-test/payload.json build/original-payload
echo ' ' >> dist-test/payload.json
reject 'RC payload changed; run --build-rc again' 1.3.0
mv build/original-payload dist-test/payload.json
[ "$(wc -l < "$FIXTURE_BUILD_LOG")" -eq 1 ]
echo 'PASS B2: changed plan or payload refuses fixture generation before a build'

env -u RBSTEMSPLUS_MODEL "$builder" 1.3.0
d="$t/build/update-fixtures/dist-test"
cmp "$d/stemsplus-model.onnx" "$d/a/stemsplus-model.onnx"
echo 'PASS BSR9: a/ uses the copied RC model when RBSTEMSPLUS_MODEL is unset'
for f in dist-test/*; do
  cmp "$f" "$d/${f##*/}"
  cmp "$f" "$d/new/${f##*/}"
done
[ "$(wc -l < "$FIXTURE_BUILD_LOG")" -eq 2 ]
echo 'PASS B2: root and new/ reuse the exact RC build and only a/ is built'

grep -qF "1.3.0 --test $mac_url --win-test $win_url/current --plan " "$FIXTURE_BUILD_LOG"
grep -qFx 'version 1.3.0' "$FIXTURE_BUILD_LOG.1.3.0.plan"
grep -qFx 'mac.bridge new' "$FIXTURE_BUILD_LOG.1.3.0.plan"
grep -qFx "$(sed -n '/^previous /p' "$plan")" "$FIXTURE_BUILD_LOG.1.3.0.plan"
for v in 1.4.0 1.3.0; do
  grep -qFx "$v $t/$archive" "$FIXTURE_BUILD_LOG.archive"
done
grep -qFx -- "--key $t/$archive/tag-v1.2.0/test-keys" "$FIXTURE_STAGE_LOG"
echo 'PASS B2: a/ uses the RC host and previous release, with fresh fixture build decisions'

cmp "$d/a/windows.zip" "$d/carried/windows.zip"
cmp "$d/windows.zip" "$d/bad-version/windows.zip"
rm -rf "$d"
rm -f "${plan%/*}/test-build-inputs"
env -u RBSTEMSPLUS_MODEL "$builder" --local 1.3.0
cmp "$d/stemsplus-model.onnx" "$d/a/stemsplus-model.onnx"
echo 'PASS BSR9: local a/ uses the freshly built RC model when RBSTEMSPLUS_MODEL is unset'
for v in 1.4.0 1.3.0; do
  grep -qFx "$v --test http://10.211.55.2:8766/current/ --windows --win-test https://10.211.55.2:8766/current" "$FIXTURE_BUILD_LOG"
done
cmp "$d/payload.json" "$d/new/payload.json"
[ "$(git -C "$t" rev-parse HEAD)" = "$head" ]
[ -z "$(git -C "$t" status --porcelain)" ]
echo 'PASS IN3/IN5: old, carried, candidate and bad-version fixtures from clean, isolated builds'

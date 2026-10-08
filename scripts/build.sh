#!/bin/bash
# Builds every release asset into dist/:
#   rbstemsplus-app.zip           the app (scripts/build-app.sh)
#   libonnxruntime.1.18.0.dylib   the universal bridge (bridge/build.sh)
#   stemsplus-model.onnx          the model, copied from RBSTEMSPLUS_MODEL (default: the local
#                                 experiments file below) or already in dist/; never built here
#   bootstrap.sh                  the pasted installer (scripts/bootstrap.sh, with the public
#                                 signing keys written in)
#   NOTICE, LICENSE               the licences (also inside the app)
#   payload.json                  versions, file names, SHA-256s, the compatible versions, and
#                                 the commit it was built from
#   SHA256SUMS                    shasum -a 256 of all of the above
# payload.json.sig, its signature, is not made here for a release: scripts/release.sh, which runs
# this build, signs it and uploads the release as a draft (docs/RELEASING.md). The app and the
# bootstrap refuse a payload.json without a valid one.
#
#   scripts/build.sh               a release, into dist/: only from a commit (no uncommitted or
#                                  untracked file), with keys/release.pub.pem and
#                                  keys/backup.pub.pem compiled in
#   scripts/build.sh --test URL    a test build into dist-test/: the app downloads from URL (a
#                                  local server serving dist-test/) instead of the GitHub release,
#                                  and trusts only a test key (build/test-keys/, made here once),
#                                  which also signs dist-test/payload.json
#
# Safe to run again: each step replaces its own output. Stops at the first problem with a message.
# Needs Xcode (actool with Icon Composer support) and whatever bridge/build.sh needs.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
dist="$root/dist"
test_base=""
if [ "${1:-}" = "--test" ]; then
  [ $# -eq 2 ] || { echo "usage: $0 [--test URL]" >&2; exit 2; }
  test_base="$2"; dist="$root/dist-test"
elif [ $# -ne 0 ]; then
  echo "usage: $0 [--test URL]" >&2; exit 2
elif [ -n "${RBSTEMSPLUS_TEST_BASE_URL:-}${RBSTEMSPLUS_TEST_KEY:-}${RBSTEMSPLUS_SCREENSHOTS:-}" ]; then
  echo "build.sh: RBSTEMSPLUS_TEST_BASE_URL, RBSTEMSPLUS_TEST_KEY or RBSTEMSPLUS_SCREENSHOTS is set: unset it for a release, or use --test URL" >&2; exit 1
fi
version="$(tr -d '[:space:]' < "$root/VERSION")"

die() { echo "build.sh: $*" >&2; exit 1; }

# A release is built only from a commit, which payload.json names (scripts/release.sh checks it is
# the commit it releases). A test build may have changes (its commit then ends in "-dirty").
commit="$(git -C "$root" rev-parse HEAD)" || die "not a git checkout"
changes="$(git -C "$root" status --porcelain)" || die "git status failed"
if [ -z "$test_base" ]; then
  [ -z "$changes" ] || die "uncommitted or untracked files (git status): a release is built only from a commit"
  for k in release backup; do
    [ -f "$root/keys/$k.pub.pem" ] \
      || die "keys/$k.pub.pem is missing: a release needs both public signing keys (scripts/make-signing-keys.sh, docs/RELEASING.md)"
  done
elif [ -n "$changes" ]; then
  commit="$commit-dirty"
fi

# --- edit for each release -------------------------------------------------------------------
# The model is the same export for every release; its SHA-256 is also ourModel in
# app/Sources/Paths.swift (and names the cache folder). A new export means a new hash in both.
model_sha256="0cc50d877629fda906562d08236b32a3e9934e64c4ea8f3ab7fc43f40976e772"
model_size=308572524
# The SHA-256s of every earlier model export (space-separated): payload.json's previous_models,
# so RB Stems Plus still knows them as its own and never saves one as rekordbox's.
previous_models=""
# payload.json's "compatible" block: rekordbox versions Stems Cache may be installed on, the
# ONNX Runtime the bridge expects, the STEMS Engine versions Stems Plus fits.
compat_rekordbox="7.2.17 7.2.18 7.2.19"
compat_ort_prefix="1.18."
compat_stems_engine="0002"
# ---------------------------------------------------------------------------------------------

model_src="${RBSTEMSPLUS_MODEL:-$root/../Library Organizer/experiments/rekordbox-stems/runs/htdemucs_rb_v4xt.onnx}"
app_zip="rbstemsplus-app.zip"
bridge="libonnxruntime.1.18.0.dylib"
model="stemsplus-model.onnx"

sha256() { shasum -a 256 "$1" | cut -d' ' -f1; }
size() { stat -f %z "$1"; }

[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "VERSION must be x.y.z, not '$version'"
for h in $previous_models; do [[ "$h" =~ ^[0-9a-f]{64}$ ]] || die "previous_models: '$h' isn't a SHA-256"; done
grep -qs "$model_sha256" "$root"/app/Sources/*.swift \
  || die "the model SHA-256 here isn't in app/Sources (ourModel in Paths.swift); make them agree"
mkdir -p "$dist"

# A public key file's DER in base64 (one line), as the app and the bootstrap hold it.
pubkey_b64() {
  local text
  text="$(/usr/bin/openssl ec -pubin -in "$1" -noout -text 2>/dev/null)" && grep -q 'ASN1 OID: prime256v1' <<< "$text" || return 1
  /usr/bin/openssl ec -pubin -in "$1" -outform DER 2>/dev/null | /usr/bin/base64 | tr -d '\n'
}
echo "== signing keys"
test_keys="$root/build/test-keys"
test_b64=""
if [ -n "$test_base" ]; then
  # the test key: made once, kept in build/ (never in git), trusted only by test builds
  if [ ! -f "$test_keys/test-key.pem" ] || [ ! -f "$test_keys/test.pub.pem" ]; then
    mkdir -p "$test_keys"
    (umask 077 && /usr/bin/openssl ecparam -name prime256v1 -genkey -noout -out "$test_keys/test-key.pem") \
      || die "couldn't make the test key"
    /usr/bin/openssl ec -in "$test_keys/test-key.pem" -pubout -out "$test_keys/test.pub.pem" 2>/dev/null \
      || die "couldn't make the test public key"
    echo "made a test signing key in build/test-keys/"
  fi
  test_b64="$(pubkey_b64 "$test_keys/test.pub.pem")" || die "build/test-keys/test.pub.pem isn't a P-256 public key"
  trusted="$test_b64"
else
  release_b64="$(pubkey_b64 "$root/keys/release.pub.pem")" || die "keys/release.pub.pem isn't a P-256 public key"
  backup_b64="$(pubkey_b64 "$root/keys/backup.pub.pem")" || die "keys/backup.pub.pem isn't a P-256 public key"
  [ "$release_b64" != "$backup_b64" ] || die "keys/release.pub.pem and keys/backup.pub.pem are the same key"
  [ ! -f "$test_keys/test.pub.pem" ] || test_b64="$(pubkey_b64 "$test_keys/test.pub.pem")" || test_b64=""
  trusted="$release_b64 $backup_b64"
fi

echo "== app"
RBSTEMSPLUS_TEST_BASE_URL="$test_base" RBSTEMSPLUS_TEST_KEY="${test_base:+$test_keys/test.pub.pem}" \
  "$root/scripts/build-app.sh" "$dist" || die "the app didn't build (see above)"
[ -f "$dist/$app_zip" ] || die "scripts/build-app.sh made no $dist/$app_zip"
app_exe="$root/build/app/RB Stems Plus.app/Contents/MacOS/RB Stems Plus"
if [ -n "$test_base" ]; then
  grep -aq "TEST BUILD" "$app_exe" || die "the test app doesn't say TEST BUILD"
else
  ! grep -aq "TEST BUILD" "$app_exe" || die "the app is a TEST BUILD: not packaging it as a release"
  [ -z "$test_b64" ] || ! grep -aqF "$test_b64" "$app_exe" || die "the app trusts the test key: not packaging it as a release"
fi
for k in $trusted; do grep -aqF "$k" "$app_exe" || die "the app doesn't hold the signing key $k"; done

echo "== bridge"
rm -rf "$root/build/bridge"
"$root/bridge/build.sh" "$root/build/bridge" || die "the bridge didn't build (see above)"
[ -f "$root/build/bridge/$bridge" ] || die "bridge/build.sh made no build/bridge/$bridge"
archs="$(lipo -archs "$root/build/bridge/$bridge")"
[[ " $archs " == *" arm64 "* && " $archs " == *" x86_64 "* ]] \
  || die "the bridge must be universal (arm64 x86_64), it is: $archs"
grep -aq "rbstems bridge: " "$root/build/bridge/$bridge" || die "the bridge lacks its marker string"
deps="$(otool -L "$root/build/bridge/$bridge")" || die "otool couldn't read the bridge"
[[ "$deps" != *"/opt/homebrew"* && "$deps" != *"/usr/local/"* ]] \
  || die "the bridge links to Homebrew; it must link libFLAC statically"
cp "$root/build/bridge/$bridge" "$dist/$bridge"

echo "== model"
if [ -f "$dist/$model" ] && [ "$(size "$dist/$model")" = "$model_size" ] \
   && [ "$(sha256 "$dist/$model")" = "$model_sha256" ]; then
  echo "$dist/$model is already the right model"
elif [ -n "$test_base" ] && [ -f "$root/dist/$model" ] && [ "$(size "$root/dist/$model")" = "$model_size" ] \
   && [ "$(sha256 "$root/dist/$model")" = "$model_sha256" ]; then
  ln -f "$root/dist/$model" "$dist/$model" 2>/dev/null || cp "$root/dist/$model" "$dist/$model"
  echo "took the model from dist/"
elif [ -f "$model_src" ]; then
  [ "$(size "$model_src")" = "$model_size" ] || die "$model_src is not $model_size bytes"
  [ "$(sha256 "$model_src")" = "$model_sha256" ] || die "$model_src doesn't have SHA-256 $model_sha256"
  cp "$model_src" "$dist/$model.part" && mv "$dist/$model.part" "$dist/$model"
  echo "copied the model from $model_src"
else
  die "no model: put it at dist/$model (SHA-256 $model_sha256) or set RBSTEMSPLUS_MODEL"
fi

echo "== bootstrap, NOTICE, LICENSE"
bash -n "$root/scripts/bootstrap.sh"
# the same keys as the app, written into its one SIGNING_KEYS="" line
[ "$(grep -c '^SIGNING_KEYS=""$' "$root/scripts/bootstrap.sh")" = 1 ] || die "scripts/bootstrap.sh needs exactly one SIGNING_KEYS=\"\" line"
sed "s|^SIGNING_KEYS=\"\"\$|SIGNING_KEYS=\"$trusted\"|" "$root/scripts/bootstrap.sh" > "$dist/bootstrap.sh"
chmod 755 "$dist/bootstrap.sh"
grep -qxF "SIGNING_KEYS=\"$trusted\"" "$dist/bootstrap.sh" || die "the keys didn't go into bootstrap.sh"
# its version, the oldest payload.json it installs (the app has it compiled in), into its one
# PAYLOAD_MINIMUM="" line
[ "$(grep -c '^PAYLOAD_MINIMUM=""$' "$dist/bootstrap.sh")" = 1 ] || die "scripts/bootstrap.sh needs exactly one PAYLOAD_MINIMUM=\"\" line"
sed -i '' "s|^PAYLOAD_MINIMUM=\"\"\$|PAYLOAD_MINIMUM=\"$version\"|" "$dist/bootstrap.sh"
grep -qxF "PAYLOAD_MINIMUM=\"$version\"" "$dist/bootstrap.sh" || die "the version didn't go into bootstrap.sh"
[ -n "$test_base" ] || [ -z "$test_b64" ] || ! grep -qF "$test_b64" "$dist/bootstrap.sh" || die "bootstrap.sh trusts the test key"
cp "$root/NOTICE" "$root/LICENSE" "$dist/"

echo "== payload.json"
json_list() { local out="" v; for v in $1; do out+="${out:+, }\"$v\""; done; echo "[$out]"; }
cat > "$dist/payload.json" <<EOF
{
  "payload_version": "$version",
  "commit": "$commit",
  "app": {"version": "$version", "zip": "$app_zip", "sha256": "$(sha256 "$dist/$app_zip")"},
  "bridge": {"file": "$bridge", "sha256": "$(sha256 "$dist/$bridge")"},
  "model": {"file": "$model", "sha256": "$model_sha256", "size": $model_size},
  "previous_models": $(json_list "$previous_models"),
  "compatible": {
    "rekordbox": $(json_list "$compat_rekordbox"),
    "ort_version_prefix": "$compat_ort_prefix",
    "stems_engine": $(json_list "$compat_stems_engine")
  }
}
EOF
plutil -convert xml1 -o /dev/null "$dist/payload.json" || die "payload.json isn't valid JSON"

echo "== SHA256SUMS"
(cd "$dist" && shasum -a 256 "$app_zip" "$bridge" "$model" bootstrap.sh NOTICE LICENSE payload.json > SHA256SUMS)
cat "$dist/SHA256SUMS"

rm -f "$dist/payload.json.sig"
if [ -n "$test_base" ]; then
  echo "== payload.json.sig (the test key)"
  /usr/bin/openssl dgst -sha256 -sign "$test_keys/test-key.pem" -out "$dist/payload.json.sig" "$dist/payload.json" \
    || die "couldn't sign payload.json with the test key"
  /usr/bin/openssl dgst -sha256 -verify "$test_keys/test.pub.pem" -signature "$dist/payload.json.sig" "$dist/payload.json" > /dev/null \
    || die "the test signature doesn't verify"
fi
echo "Built RB Stems Plus $version ($commit) into $dist${test_base:+ (TEST BUILD, downloads from $test_base, signed with the test key)}"
[ -n "$test_base" ] || echo "Not signed: scripts/release.sh signs and uploads a release (docs/RELEASING.md)."


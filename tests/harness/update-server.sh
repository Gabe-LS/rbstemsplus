#!/bin/bash
# A folder for the TEST builds' local download server (tests/harness/vm.sh serves it on
# 192.168.64.1:8765 while a scenario whose header says "# server: make NAME FLAGS…" runs): a
# copy of a TEST build's files with payload.json changed as FLAGS say and signed again with that
# build's test key (never a release key). On the host.
#
#   tests/harness/update-server.sh OUT SRC KEY [FLAGS…]
#     SRC   a TEST build's folder: rbstemsplus-app.zip, the bridge, payload.json and the model
#           (dist-test/ of the build pushed into the VM)
#     KEY   the test key that build trusts (build/test-keys/test-key.pem)
#   FLAGS
#     --app-version V    the app zip repacked with CFBundleShortVersionString V: a newer RB Stems
#                        Plus for the in-app update (update-flow, P14). V "next": the source app's
#                        own version with its patch number plus one (1.3.0 -> 1.3.1, 1.4.0 ->
#                        1.4.1), so the update works from whichever build is under test. The
#                        repack breaks the app's signature on purpose: the update script signs
#                        the copy it stages
#     --app-zip ZIP      ZIP as the app (G1: a core-linked TEST build), at its own version
#     --bridge-variant   the bridge with one byte appended: another checksum than the one in
#                        rekordbox, so the app offers the Stems Cache update (cache-update-offer)
#     --bad-sig          payload.json changed after it was signed: it doesn't verify (install-unverified)
# The model is linked, not copied (300 MB).
set -euo pipefail
out="$1"; src="$2"; key="$3"; shift 3
die() { echo "update-server.sh: $*" >&2; exit 1; }
for f in rbstemsplus-app.zip libonnxruntime.1.18.0.dylib payload.json stemsplus-model.onnx; do [ -f "$src/$f" ] || die "no $src/$f"; done
[ -f "$key" ] || die "no test key $key"
# never a release key: a key whose public half is one of keys/*.pub.pem is refused
pub="$(/usr/bin/openssl ec -in "$key" -pubout 2>/dev/null)" || die "$key isn't an EC private key"
for k in "${RBSP_RELEASE_KEYS:-$(cd "$(dirname "$0")/../.." && pwd)/keys}"/*.pub.pem; do      # RBSP_RELEASE_KEYS: the selftest's
  [ -f "$k" ] && [ "$pub" = "$(cat "$k")" ] && die "$key is the release signing key ($k): test keys only"
done
appv=""; appzip=""; variant=""; badsig=""
while [ $# -gt 0 ]; do
  case "$1" in
    --app-version) appv="$2"; shift 2 ;;
    --app-zip) appzip="$2"; shift 2 ;;
    --bridge-variant) variant=1; shift ;;
    --bad-sig) badsig=1; shift ;;
    *) die "unknown flag $1" ;;
  esac
done
rm -rf "$out"; mkdir -p "$out"
cp "$src/libonnxruntime.1.18.0.dylib" "$src/payload.json" "$out/"
ln -s "$src/stemsplus-model.onnx" "$out/stemsplus-model.onnx"
tmp="$(mktemp -d "${TMPDIR:-/tmp}/rbsp-server.XXXXXX")"; trap 'rm -rf "$tmp"' EXIT
if [ -n "$appzip" ]; then
  cp "$appzip" "$out/rbstemsplus-app.zip"
elif [ -n "$appv" ]; then
  ditto -x -k "$src/rbstemsplus-app.zip" "$tmp/x"
  if [ "$appv" = next ]; then
    cur="$(plutil -extract CFBundleShortVersionString raw "$tmp/x/RB Stems Plus.app/Contents/Info.plist")"
    [[ "$cur" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)$ ]] || die "the app's version $cur isn't x.y.z"
    appv="${BASH_REMATCH[1]}.${BASH_REMATCH[2]}.$(( BASH_REMATCH[3] + 1 ))"
  fi
  plutil -replace CFBundleShortVersionString -string "$appv" "$tmp/x/RB Stems Plus.app/Contents/Info.plist"
  (cd "$tmp/x" && ditto -c -k --sequesterRsrc --keepParent "RB Stems Plus.app" "$out/rbstemsplus-app.zip")
else
  cp "$src/rbstemsplus-app.zip" "$out/"
fi
[ -n "$variant" ] && printf '\0' >> "$out/libonnxruntime.1.18.0.dylib"
# the app's version as the zip holds it
ditto -x -k "$out/rbstemsplus-app.zip" "$tmp/v"
version="$(plutil -extract CFBundleShortVersionString raw "$tmp/v/RB Stems Plus.app/Contents/Info.plist")"
python3 -I - "$out/payload.json" "$version" "$(shasum -a 256 "$out/rbstemsplus-app.zip" | cut -d' ' -f1)" \
  "$(shasum -a 256 "$out/libonnxruntime.1.18.0.dylib" | cut -d' ' -f1)" <<'EOF'
import json, sys
p, version, app_sha, bridge_sha = sys.argv[1:5]
m = json.load(open(p))
m["app"]["version"] = version
m["app"]["sha256"] = app_sha
m["bridge"]["sha256"] = bridge_sha
# the payload's own version follows the app's when the app is newer (as a release's does)
parts = lambda v: [int(x) for x in v.split(".")]
if parts(version) > parts(m["payload_version"]):
    m["payload_version"] = version
open(p, "w").write(json.dumps(m, indent=2) + "\n")
EOF
/usr/bin/openssl dgst -sha256 -sign "$key" -out "$out/payload.json.sig" "$out/payload.json"
[ -n "$badsig" ] && printf ' ' >> "$out/payload.json"
echo "server folder $out: app $version$( [ -n "$variant" ] && echo ", another bridge")$( [ -n "$badsig" ] && echo ", payload.json not verifying")"

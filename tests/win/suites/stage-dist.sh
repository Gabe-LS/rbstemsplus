#!/bin/bash
# The bootstrap suite's fixture releases, made from the share's TEST build dist-test/
# (bootstrap.ps1, payload.json, its .sig, the Windows zip):
#  - the faults (release.md section 8): three copies with one byte appended to the zip
#    (fault-zip/), to payload.json (fault-json/) or to its signature (fault-sig/). Each must be
#    refused with nothing changed (bootstrap/faults.ps1);
#  - bad-version/ (part B3): a copy whose payload.json names another windows.app.version than the
#    zip's app, at or below payload_version, signed again with this TEST build's test key. Test-RbspAppVersion
#    must refuse it (bootstrap/version-refused.ps1). The key is used only when its public half
#    verifies dist-test/payload.json.sig (so it is this build's), and never when it is one of
#    keys/*.pub.pem (a release key); else bad-version/ isn't made, said (that step SKIPs);
#  - carried/ (update.suite, release.md 1.3): when the share has update.suite's first release
#    dist-test/a/, a copy of it whose payload.json names the next payload_version (its last
#    number plus one) and nothing else changed, so its windows.app.version is a/'s, signed again
#    with the test key that signed a/ (the same rules); a/'s other files as they are. Without a
#    signed a/ or its key, carried/ is left as it is, said.
#   tests/win/suites/stage-dist.sh [SHARE] [--key DIR]... [--dist BUILD]
#       SHARE: default the main checkout's build/win. DIR: a test key pair (test-key.pem,
#       test.pub.pem), tried in order; default this checkout's build/test-keys, then the main
#       checkout's. BUILD: the TEST build folder the fixtures are made from and in (default
#       SHARE/dist-test; another one is what vm-run.sh run --serve BUILD serves). Exit 0 when the
#       faults were made (bad-version/ and carried/, or the reason either wasn't, said), 2 when
#       the TEST build or a key is unusable.
set -euo pipefail
here="$(cd "$(dirname "$0")/../../.." && pwd)"
main="$(cd "$(git -C "$here" rev-parse --path-format=absolute --git-common-dir)/.." && pwd)"
share="" keys=() dist=""
while [ $# -gt 0 ]; do
  case "$1" in
    --key) [ $# -ge 2 ] || { echo "stage-dist.sh: --key needs a folder" >&2; exit 2; }; keys+=("$2"); shift 2 ;;
    --dist) [ $# -ge 2 ] || { echo "stage-dist.sh: --dist needs a folder" >&2; exit 2; }; dist="$2"; shift 2 ;;
    *) [ -z "$share" ] || { echo "usage: stage-dist.sh [SHARE] [--key DIR]... [--dist BUILD]" >&2; exit 2; }; share="$1"; shift ;;
  esac
done
share="${share:-$main/build/win}"
[ ${#keys[@]} -gt 0 ] || keys=("$here/build/test-keys" "$main/build/test-keys")
release_keys="${STAGE_DIST_RELEASE_KEYS:-$here/keys}"   # (the offline tests point it elsewhere)
stop() { echo "stage-dist.sh: $*" >&2; exit 2; }
d="${dist:-$share/dist-test}"
[ -f "$d/payload.json" ] && [ -f "$d/payload.json.sig" ] || stop "no TEST build in $d"
zip="$(python3 -I -c 'import json, sys; print(json.load(open(sys.argv[1]))["windows"]["app"]["zip"])' "$d/payload.json")" \
  || stop "$d/payload.json has no windows.app.zip"
case "$zip" in */*|..*|"") stop "the zip's name isn't plain: $zip" ;; esac
[ -f "$d/$zip" ] || stop "no $d/$zip"
for f in zip json sig; do
  o="$d/fault-$f"
  rm -rf "$o"
  mkdir -p "$o"
  for x in bootstrap.ps1 payload.json payload.json.sig "$zip"; do [ ! -f "$d/$x" ] || cp "$d/$x" "$o/"; done
  case "$f" in zip) t="$o/$zip" ;; json) t="$o/payload.json" ;; sig) t="$o/payload.json.sig" ;; esac
  printf 'X' >> "$t"
  echo "staged $o: one byte appended to $(basename "$t")"
done

# ---- bad-version/
# pub_b64 PEM: a P-256 public key's DER, base64 (nothing, exit 1, for anything else)
pub_b64() {
  /usr/bin/openssl ec -pubin -in "$1" -noout -text 2> /dev/null | grep -q 'ASN1 OID: prime256v1' || return 1
  /usr/bin/openssl ec -pubin -in "$1" -outform DER 2> /dev/null | /usr/bin/base64 | tr -d '\n'
}
# signer DIR WHAT: the first test key pair that signed DIR/payload.json (WHAT; none: "", said),
# never a release key's
signer() {
  local k p r
  for k in "${keys[@]}"; do
    [ -f "$k/test-key.pem" ] && [ -f "$k/test.pub.pem" ] || continue
    p="$(pub_b64 "$k/test.pub.pem")" || stop "$k/test.pub.pem isn't a P-256 public key"
    for r in "$release_keys"/*.pub.pem; do
      [ -f "$r" ] || continue
      [ "$(pub_b64 "$r" || true)" != "$p" ] || stop "$k is $(basename "$r")'s key pair: a release key never signs a fixture"
    done
    if /usr/bin/openssl dgst -sha256 -verify "$k/test.pub.pem" -signature "$1/payload.json.sig" "$1/payload.json" > /dev/null 2>&1; then key="$k"; return 0; fi
    echo "stage-dist.sh: the test key in $k didn't sign $2"
  done
}
o="$d/bad-version"
rm -rf "$o"
key=""
signer "$d" "this TEST build"
if [ -z "$key" ]; then
  echo "bad-version/ not made: no test key pair that signed $d/payload.json (the version-refused step SKIPs)"
else
mkdir -p "$o"
for x in bootstrap.ps1 "$zip"; do [ ! -f "$d/$x" ] || cp "$d/$x" "$o/"; done
old_new="$(python3 -I -c '
import json, re, sys
src, dst = sys.argv[1], sys.argv[2]
p = json.load(open(src))
old = str(p["windows"]["app"]["version"])
if not re.fullmatch(r"[0-9]{1,9}(\.[0-9]{1,9}){2}", old):
    sys.exit("windows.app.version %r isn'"'"'t x.y.z" % old)
# Bootstrap compares numeric parts against payload_version, then checks the app version text.
p["windows"]["app"]["version"] = "0.0.00" if old == "0.0.0" else "0.0.0"
with open(dst, "w") as f:
    json.dump(p, f, indent=2)
    f.write("\n")
print(old, p["windows"]["app"]["version"])' "$d/payload.json" "$o/payload.json")" || stop "couldn't write $o/payload.json"
/usr/bin/openssl dgst -sha256 -sign "$key/test-key.pem" -out "$o/payload.json.sig" "$o/payload.json" || stop "couldn't sign $o/payload.json with the test key"
/usr/bin/openssl dgst -sha256 -verify "$key/test.pub.pem" -signature "$o/payload.json.sig" "$o/payload.json" > /dev/null || stop "the bad-version signature doesn't verify"
echo "staged $o: windows.app.version ${old_new% *} -> ${old_new#* }, signed with the test key in $key"
fi

# ---- carried/ (from update.suite's a/)
a="$d/a"; o="$d/carried"
if [ ! -f "$a/payload.json" ] || [ ! -f "$a/payload.json.sig" ]; then
  echo "carried/ not made: no signed update.suite release $a (carried/ left as it is)"
  exit 0
fi
key=""
signer "$a" "update.suite's a/"
if [ -z "$key" ]; then
  echo "carried/ not made: no test key pair that signed $a/payload.json (carried/ left as it is)"
  exit 0
fi
rm -rf "$o"
mkdir -p "$o"
for x in "$a"/*; do
  [ -f "$x" ] && [ ! -L "$x" ] || continue
  case "$(basename "$x")" in payload.json|payload.json.sig) continue ;; esac
  cp -c "$x" "$o/" 2> /dev/null || cp "$x" "$o/" || stop "couldn't copy $x to $o"
done
old_new="$(python3 -I -c '
import json, sys
src, dst = sys.argv[1], sys.argv[2]
p = json.load(open(src))
old = str(p["payload_version"])
parts = old.split(".")
if len(parts) != 3 or not all(x.isdigit() for x in parts):
    sys.exit("payload_version %r isn'"'"'t x.y.z" % old)
if not isinstance(p.get("windows", {}).get("app", {}).get("version"), str):
    sys.exit("no windows.app.version")
parts[-1] = str(int(parts[-1]) + 1)
p["payload_version"] = ".".join(parts)
with open(dst, "w") as f:
    json.dump(p, f, indent=2)
    f.write("\n")
print(old, p["payload_version"], p["windows"]["app"]["version"])' "$a/payload.json" "$o/payload.json")" || stop "couldn't write $o/payload.json"
/usr/bin/openssl dgst -sha256 -sign "$key/test-key.pem" -out "$o/payload.json.sig" "$o/payload.json" || stop "couldn't sign $o/payload.json with the test key"
/usr/bin/openssl dgst -sha256 -verify "$key/test.pub.pem" -signature "$o/payload.json.sig" "$o/payload.json" > /dev/null || stop "the carried signature doesn't verify"
read -r was now app <<< "$old_new"
echo "staged $o: payload_version $was -> $now, windows.app.version $app (a/'s), signed with the test key in $key"

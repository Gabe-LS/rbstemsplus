#!/bin/bash
# multiversion.suite's served folder, made from a TEST build whose Windows list holds 7.2.19 and
# 7.2.20 (scripts/build.sh --test http://10.211.55.2:8766/ --win-test https://10.211.55.2:8766/current
# --windows): the app's own downloads (the update check, the bridge) come from current/, which the
# suite's switch steps fill from one of two signed test manifests (mv-spec.md section 5):
#   m1/  windows.compatible holds 7.2.19 only, with its binding (payload_version as built)
#   m2/  the build's own list, 7.2.19 + 7.2.20 with both bindings and the code digest, at the next
#        payload_version (a refresh the app accepts over m1)
# The root (what the one-liner installs) is the build with m1's payload.json, so the app starts
# from M1 as well. Each payload.json is signed again with the test key that signed the build.
# The multi-version worker refuses a payload below its compiled minimum (win/admin/main.c
# ADMIN_COMPILED_MINIMUM, 1.5.0) while VERSION is still lower: m1's payload_version is then
# raised to that minimum (windows.app.version kept, as update.suite's carried/ does) and m2's is
# the next one.
#   tests/win/suites/multiversion/make-dist.sh BUILD [OUT]
#       BUILD: the TEST build folder (dist-test/ of the checkout that built it). OUT: default the
#       main checkout's build/win/dist-mv (the share); replaced whole. Then
#       tests/win/vm-run.sh run "<snapshot (b)>" multiversion --serve build/win/dist-mv
set -euo pipefail
here="$(cd "$(dirname "$0")/../../../.." && pwd)"
main="$(cd "$(git -C "$here" rev-parse --path-format=absolute --git-common-dir)/.." && pwd)"
build="${1:?usage: make-dist.sh BUILD [OUT]}"
out="${2:-$main/build/win/dist-mv}"
stop() { echo "make-dist.sh: $*" >&2; exit 2; }
[ -f "$build/payload.json" ] && [ -f "$build/payload.json.sig" ] || stop "no signed TEST build in $build"
key=""
for k in "$here/build/test-keys" "$main/build/test-keys"; do
  [ -f "$k/test-key.pem" ] || continue
  if /usr/bin/openssl dgst -sha256 -verify "$k/test.pub.pem" -signature "$build/payload.json.sig" "$build/payload.json" > /dev/null 2>&1; then key="$k"; break; fi
done
[ -n "$key" ] || stop "no test key pair here signed $build/payload.json"
for r in "$here"/keys/*.pub.pem; do
  [ -f "$r" ] || continue
  ! cmp -s "$r" "$key/test.pub.pem" || stop "$key is a release key pair"
done
bridge="$(python3 -I -c 'import json,sys; print(json.load(open(sys.argv[1]))["windows"]["bridge"]["file"])' "$build/payload.json")"
case "$bridge" in */*|..*|"") stop "the bridge's name isn't plain: $bridge" ;; esac
minimum="$(sed -n 's/^#define ADMIN_COMPILED_MINIMUM "\([0-9.]*\)".*/\1/p' "$here/win/admin/main.c")"
[[ "$minimum" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || stop "no ADMIN_COMPILED_MINIMUM in win/admin/main.c"
# current/ is the VM's (a switch step's copy, root's on the Mac): kept, the next switch replaces it
if [ -d "$out" ]; then find "$out" -mindepth 1 -maxdepth 1 ! -name current -exec rm -rf {} +; fi
mkdir -p "$out/m1" "$out/m2"
for f in "$build"/*; do
  [ -f "$f" ] || continue
  case "$(basename "$f")" in payload.json|payload.json.sig) continue ;; esac
  cp -c "$f" "$out/" 2> /dev/null || cp "$f" "$out/"
done
python3 -I - "$build/payload.json" "$out" "$minimum" <<'EOF'
import json, sys
src, out, minimum = sys.argv[1], sys.argv[2], sys.argv[3]
def num(v): return tuple(int(x) for x in str(v).split("."))
p = json.load(open(src))
c = p["windows"]["compatible"]
if "7.2.19" not in c["rekordbox"] or "7.2.20" not in c["rekordbox"]:
    sys.exit("the build's Windows list isn't 7.2.19 + 7.2.20: %r" % c["rekordbox"])
i = c["rekordbox"].index("7.2.19")
m1 = json.loads(json.dumps(p))
if num(m1["payload_version"]) < num(minimum):
    m1["payload_version"] = minimum
k = m1["windows"]["compatible"]
k["rekordbox"] = ["7.2.19"]
k["ort_sha256"] = [c["ort_sha256"][i]]
k["ort"] = [b for b in c["ort"] if b["rekordbox"] == "7.2.19"]
m2 = json.loads(json.dumps(p))
parts = str(m1["payload_version"]).split(".")
parts[-1] = str(int(parts[-1]) + 1)
m2["payload_version"] = ".".join(parts)
for name, m in (("m1", m1), ("m2", m2)):
    with open("%s/%s/payload.json" % (out, name), "w") as f:
        json.dump(m, f, indent=2)
        f.write("\n")
    print("%s: payload_version %s, windows list %s" % (name, m["payload_version"], m["windows"]["compatible"]["rekordbox"]))
EOF
for m in m1 m2; do
  /usr/bin/openssl dgst -sha256 -sign "$key/test-key.pem" -out "$out/$m/payload.json.sig" "$out/$m/payload.json"
  /usr/bin/openssl dgst -sha256 -verify "$key/test.pub.pem" -signature "$out/$m/payload.json.sig" "$out/$m/payload.json" > /dev/null || stop "$m's signature doesn't verify"
  cp "$out/$bridge" "$out/$m/"
  [ ! -f "$out/test-ca.pem" ] || cp "$out/test-ca.pem" "$out/$m/"
done
cp "$out/m1/payload.json" "$out/m1/payload.json.sig" "$out/"
echo "made $out (root and m1/: M1; m2/: M2), signed with the test key in $key"

#!/bin/bash
# A served copy of a TEST build whose payload_version is at least the worker's compiled minimum
# (win/admin/main.c ADMIN_COMPILED_MINIMUM): while VERSION is below it, the multi-version worker
# refuses the build's own payload (exit 14, "Update RB Stems Plus first"), so worker.suite,
# stems-cache-uac.suite, uninstall-uac.suite and the other suites that install Stems Cache from
# dist-test/ need this raise, as multiversion/make-dist.sh's m1/ and update.suite's carried/ do:
# payload_version raised, windows.app.version and every other field kept, signed again with the
# test key that signed BUILD (never a release key). Then stage-dist.sh's fixture releases are
# made in OUT.
#   tests/win/suites/raise-payload.sh BUILD [OUT]
#       BUILD: the TEST build folder (dist-test/ of the checkout that built it). OUT: default the
#       main checkout's build/win/dist-test (the share); replaced whole. A BUILD already at or
#       above the minimum is copied unchanged (said).
set -euo pipefail
here="$(cd "$(dirname "$0")/../../.." && pwd)"
main="$(cd "$(git -C "$here" rev-parse --path-format=absolute --git-common-dir)/.." && pwd)"
build="${1:?usage: raise-payload.sh BUILD [OUT]}"
out="${2:-$main/build/win/dist-test}"
stop() { echo "raise-payload.sh: $*" >&2; exit 2; }
[ -f "$build/payload.json" ] && [ -f "$build/payload.json.sig" ] || stop "no signed TEST build in $build"
[ "$(cd "$build" && pwd -P)" != "$(mkdir -p "$out" && cd "$out" && pwd -P)" ] || stop "BUILD and OUT are the same folder"
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
minimum="$(sed -n 's/^#define ADMIN_COMPILED_MINIMUM "\([0-9.]*\)".*/\1/p' "$here/win/admin/main.c")"
[[ "$minimum" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || stop "no ADMIN_COMPILED_MINIMUM in win/admin/main.c"
# current/ (a switch step's copy, root's on the Mac) is kept, as make-dist.sh does
find "$out" -mindepth 1 -maxdepth 1 ! -name current -exec rm -rf {} +
for f in "$build"/*; do
  [ -f "$f" ] || continue
  cp -c "$f" "$out/" 2> /dev/null || cp "$f" "$out/"
done
raised="$(python3 -I - "$out/payload.json" "$minimum" <<'EOF'
import json, sys
path, minimum = sys.argv[1], sys.argv[2]
def num(v): return tuple(int(x) for x in str(v).split("."))
p = json.load(open(path))
if num(p["payload_version"]) >= num(minimum):
    print("")
    sys.exit(0)
p["payload_version"] = minimum
with open(path, "w") as f:
    json.dump(p, f, indent=2)
    f.write("\n")
print(minimum)
EOF
)"
if [ -n "$raised" ]; then
  /usr/bin/openssl dgst -sha256 -sign "$key/test-key.pem" -out "$out/payload.json.sig" "$out/payload.json"
  /usr/bin/openssl dgst -sha256 -verify "$key/test.pub.pem" -signature "$out/payload.json.sig" "$out/payload.json" > /dev/null || stop "the signature doesn't verify"
  echo "raise-payload.sh: $out: payload_version raised to $raised, signed with the test key in $key"
else
  echo "raise-payload.sh: $out: payload_version already at or above $minimum, copied unchanged"
fi
"$here/tests/win/suites/stage-dist.sh" "$(dirname "$out")" --dist "$out" --key "$key"

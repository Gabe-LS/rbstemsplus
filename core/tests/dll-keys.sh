#!/bin/sh
# core/build.sh dll's payload keys (review NA8): a release build refuses keys/release or
# keys/backup when it is build/test-keys' test key, and refuses the two being one key; --test-key
# is taken only with --test-hooks; a good pair and a test build's key are written. Runs
# core/build.sh in a scratch copy of the tree (core/build.sh, keys/, build/test-keys/) with keys made
# here, so the repository's keys are never touched. A refusal stops before anything is fetched or
# compiled, so no toolchain is needed. Exits 1 on a failure.
#
#   core/tests/dll-keys.sh
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
t="$(mktemp -d "${TMPDIR:-/tmp}/rbsp-dll-keys.XXXXXX")"
trap 'rm -rf "${t:?}"' EXIT
fails=0
mkdir -p "$t/core" "$t/keys" "$t/build/test-keys" "$t/scripts"
cp "$root/core/build.sh" "$t/core/build.sh"
key() {   # NAME: a new P-256 public key at $t/k/NAME.pub.pem
  mkdir -p "$t/k"
  /usr/bin/openssl ecparam -name prime256v1 -genkey -noout -out "$t/k/$1.key" 2> /dev/null
  /usr/bin/openssl ec -in "$t/k/$1.key" -pubout -out "$t/k/$1.pub.pem" 2> /dev/null
}
key a
key b
key test
# no toolchain here: a build that gets past the keys stops at once, after writing them
printf '#!/bin/sh\necho "fake toolchain: stop" >&2\nexit 9\n' > "$t/scripts/win-deps.sh"
cp "$t/scripts/win-deps.sh" "$t/scripts/win-cc.sh"
chmod +x "$t/scripts/win-deps.sh" "$t/scripts/win-cc.sh" "$t/core/build.sh"
cp "$t/k/test.pub.pem" "$t/build/test-keys/test.pub.pem"

# CASE NAME RELEASE BACKUP WANT (refused: the words it says; written: the key names written) ARGS...
case_() {
  name="$1" rel="$2" bak="$3" want="$4"
  shift 4
  cp "$t/k/$rel.pub.pem" "$t/keys/release.pub.pem"
  cp "$t/k/$bak.pub.pem" "$t/keys/backup.pub.pem"
  rm -rf "$t/out"
  set +e
  "$t/core/build.sh" dll "$t/out" "$@" > "$t/log" 2>&1
  x=$?
  set -e
  case "$want" in
    written:*)
      names="${want#written:}"
      # the file the case writes, never the other one: a test build's under testhooks/ only
      if [ "$names" = test ]; then keys="$t/out/testhooks/core_native_keys.c" other="$t/out/core_native_keys.c"
      else keys="$t/out/core_native_keys.c" other="$t/out/testhooks/core_native_keys.c"; fi
      ok=1 count=0
      [ -f "$keys" ] && [ ! -f "$other" ] || ok=0
      for n in $names; do
        count=$((count + 1))
        # the key the case meant, as build.sh encodes it (DER, base64, one line)
        case "$n" in release) pem="$t/k/$rel.pub.pem" ;; backup) pem="$t/k/$bak.pub.pem" ;; *) pem="$t/k/$n.pub.pem" ;; esac
        b64="$(/usr/bin/openssl ec -pubin -in "$pem" -outform DER 2> /dev/null | /usr/bin/base64 | tr -d '\n')"
        grep -qF "{\"$n\", \"$b64\"}," "$keys" 2> /dev/null || ok=0
      done
      [ "$(grep -c '^    {"' "$keys" 2> /dev/null)" = "$count" ] && grep -q "native_n_key_texts = $count;" "$keys" 2> /dev/null || ok=0
      if [ "$ok" = 1 ] && grep -q "fake toolchain: stop" "$t/log"; then echo "ok   $name"
      else cat "$t/log"; echo "FAIL $name: not exactly the keys $names, with their values, in $keys (exit $x)"; fails=$((fails + 1)); fi
      ;;
    *)
      if [ "$x" != 0 ] && grep -q "$want" "$t/log" && ! grep -q "fake toolchain" "$t/log" && [ ! -f "$t/out/core_native_keys.c" ] \
         && [ ! -f "$t/out/testhooks/core_native_keys.c" ]; then echo "ok   $name"
      else cat "$t/log"; echo "FAIL $name: not refused with \"$want\" (exit $x)"; fails=$((fails + 1)); fi
      ;;
  esac
}

case_ "a release with its two keys: written" a b "written:release backup"
case_ "a release whose release key is the test key: refused" test b "keys/release.pub.pem is the test key"
case_ "a release whose backup key is the test key: refused" a test "keys/backup.pub.pem is the test key"
case_ "a release whose two keys are one: refused" a a "are the same key"
case_ "--test-key without --test-hooks: refused" a b "usage:" --test-key "$t/k/test.pub.pem"
case_ "--test-key without --test-hooks (empty first argument): refused" a b "usage:" "" --test-key "$t/k/test.pub.pem"
case_ "--test-hooks --test-key: the test key alone" a b "written:test" --test-hooks --test-key "$t/k/test.pub.pem"
case_ "--test-hooks --test-key with no key: refused" a b "usage:" --test-hooks --test-key

[ "$fails" = 0 ] && echo "dll-keys: all passed" || { echo "dll-keys: $fails failed"; exit 1; }

#!/bin/sh
# Exercise build-win.sh's compile and link argument handling without running dependency builds.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
mkdir -p "$root/build"
temp="$(mktemp -d "$root/build/helper build.XXXXXX")"
trap 'rm -rf "${temp:?}"' EXIT
TMPDIR="$temp"
scratch="$(mktemp -d "$TMPDIR/rbsp-helper-win.XXXXXX")"
obj="$scratch/obj" alac="$scratch/alac" pf="$scratch/pocketfft" rt="$scratch/blocksrt"
flacd="$scratch/flac" ltcd="$scratch/ltc" sqlcd="$scratch/other-include"
mkdir -p "$obj" "$alac" "$pf" "$rt" "$flacd/include" "$flacd/lib" "$ltcd/lib" "$sqlcd" "$scratch/dummykey"
touch "$obj/pocketfft.o" "$obj/sqlite3.o" "$obj/rbsp.res.o" "$obj/alac_two words.o" "$obj/blocks_runtime.o" "$obj/blocks_data.o" \
      "$flacd/lib/libFLAC.a" "$ltcd/lib/libtomcrypt.a"
mkdir -p "$scratch/src/helper" "$scratch/src/common" "$scratch/src/bridge"

cc="$scratch/test-cc"
cat > "$cc" <<'SH'
#!/bin/sh
set -eu
test "$1" = x64
shift
while [ "$#" -gt 0 ]; do
  case "$1" in
    -I*) test -d "${1#-I}" || { echo "missing include directory: $1" >&2; exit 1; } ;;
    -o) shift; : > "$1" ;;
    *.o|*.a) test -f "$1" || { echo "missing link input: $1" >&2; exit 1; } ;;
    -Wl,-Map=*) : > "${1#-Wl,-Map=}" ;;
    -*|*.c) ;;
    *) echo "unexpected compiler argument: $1" >&2; exit 1 ;;
  esac
  shift
done
SH
chmod +x "$cc"
WARN=-Wall STRICT=-Wpedantic LIBS=-lntdll
fail() { echo "FAIL $*" >&2; exit 1; }
# Source just the production build function, with a stand-in compiler and empty fixtures.
eval "$(sed -n '/^build() {$/,/^}$/p' "$here/../build-win.sh")"
build "$scratch/src" "$scratch/out"
test -e "$scratch/out/rbsp-prepare.exe"
test -e "$scratch/out/rbsp-decode-dump.exe"
test -e "$scratch/out/rbsp-win-stop-test.exe"
test -e "$scratch/out/rbsp-kept-check.exe"

# Exercise the production math-import gate with synthetic import listings.
out="$scratch/out"
math_gate="$(sed -n '/^for e in rbsp-decode-dump.exe rbsp-prepare.exe; do$/,/^done$/p' "$here/../build-win.sh")"
test -n "$math_gate"
pe_dump() {
  printf 'DLL Name: api-ms-win-crt-math-l1-1-0.dll\n  0 pow\n  0 sin\n'
  if [ "${2##*/}" = rbsp-prepare.exe ]; then
    if [ "${cos_elsewhere:-}" = 1 ]; then printf 'DLL Name: api-ms-win-crt-runtime-l1-1-0.dll\n'; fi
    printf '  0 cos\n'
  fi
}
(eval "$math_gate")
cos_elsewhere=1
if (eval "$math_gate") > "$scratch/math.log" 2>&1 ||
   ! grep -q 'rbsp-prepare.exe:.*expected.*cos pow sin' "$scratch/math.log"; then
  fail "the math gate did not reject cos from another DLL with the expected diagnostic"
fi

# Expand the resource source with the defines passed to windres, without producing a PE file.
(
  eval "$(sed -n '/^build_resource() {$/,/^}$/p' "$here/../build-win.sh")"
  resource_cc() {
    test "$1 $2 $3" = '--tool x64 windres' || return 1
    shift 3
    clang -E -P -x c "$1" "$2" "$3" "$4" "$5" "$6" > "$scratch/version.rc"
  }
  here="$root/helper" cc=resource_cc version=7.8.9
  build_resource
  for want in 'FILEVERSION 7,8,9,0' 'PRODUCTVERSION 7,8,9,0' \
              'VALUE "ProductName", "RB Stems Plus"' 'VALUE "ProductVersion", "7.8.9"'; do
    grep -qF "$want" "$scratch/version.rc" || fail "the stamped resource is missing $want"
  done
)
echo "build-win-host: 4 passed, 0 failed"

#!/bin/sh
# The Windows prepare helper's build, on the Mac (helper.md §1 "Build", task T3), parallel to
# helper/build.sh: x86_64 only (helper.md §0 finding 5: it loads rekordbox's x64 DLLs, also on
# ARM64 Windows), with the pinned llvm-mingw (scripts/win-cc.sh: clang, lld, mingw-w64, UCRT) and
# nothing else: no MSVC, clang-cl, xwin or Windows SDK. Its own downloads go through
# scripts/deps.sh (deps.lock); two come through the existing pinned scripts, each checking its own
# sha256: the toolchain (scripts/win-cc.sh --fetch, also a deps.lock row) and pocketfft
# (scripts/win-deps.sh, not in deps.lock yet: release.md T1-T6). helper/build.sh and
# bridge/build.sh, the Mac's, are left as they are.
#
#   helper/build-win.sh [OUT]          into OUT (default build/helper-win)
#
# The helper and the parts that exist for Windows, as the helper links them, with the helper's gates:
#   OUT/rbsp-prepare.exe           the prepare helper (T12, helper.md §3): prepare.c and everything it
#                                  links, model.c on Pioneer's onnxruntime.dll (the app's kept copy,
#                                  --ort; never shipped or fetched: its C API header is the bridge's,
#                                  bridge/onnxruntime_c_api.h), the CPU by default, DirectML with
#                                  --dml; kept.c and prepare.c with clang blocks (-fblocks) and
#                                  BlocksRuntime (scripts/win-deps.sh blocksrt, the bridge's pin) linked
#                                  statically. The release zip's app\rbsp-prepare.exe
#                                  (scripts/build-windows.sh)
#   OUT/rbsp-decode-dump.exe       the decoders as the helper runs them (decode.c's Windows path:
#                                  pcm.c, m4a.c, alac.c, and mp3.c on rekordbox's libmpg123.dll):
#                                  helper/tests/decode_dump.c, for the VM's ground truth (T2, T17):
#                                  rbsp-decode-dump.exe "C:\Program Files\rekordbox\rekordbox 7.2.19" OUTDIR FILE...
#   OUT/rbsp-aac-mf-probe.exe      aac_mf.c's decodes, 11 per file (async, blocking at 8 stack
#                                  shifts), for Q3 (T10: helper/tests/aac_mf_probe.c)
#   OUT/rbsp-win-plat-test.exe     T4's and T7's calls (helper/tests/win-plat.c), with
#                                  OUT/rbsp-planted.dll for its DLL-search check
#   OUT/rbsp-win-library-test.exe  SQLCipher with LibTomCrypt and CNG's PBKDF2 (helper/sqlc_kdf.h,
#                                  P-H5), and library.c with the library key (helper/tests/win-library.c)
#   OUT/rbsp-win-rules-test.exe    the track rules on library.c and manifest.c (T13; library.c with a
#                                  dummy key, never rekordbox's: helper/tests/win-rules.c)
#   OUT/rbsp-win-readable-test.exe readable.c with its test gate (T14 and the UNC ruling:
#                                  helper/tests/win-readable.c)
#   OUT/rbsp-win-stop-test.exe     the Stop on stdin, Pause by suspending (T15: helper/tests/win-stop.c)
#   OUT/rbsp-kept-check.exe        check kept.sh output with the bridge's exact parser (T16)
# Each exe has the helper's manifest (helper/win/rbsp.manifest: UTF-8 code page, long paths) and
# calls plat_exe_start first (SetDefaultDllDirectories(System32), binary standard streams).
#
# The dependencies are built into build/helper-win-deps/, each with a stamp (its deps.lock sha256,
# its recipe, the toolchain's pin) and built again when the stamp differs:
# libFLAC (bridge/build.sh's pin, --disable-multithreading, FLAC__NO_DLL), LibTomCrypt 1.18.2,
# SQLCipher 4.19.0's amalgamation (made on this Mac, compiled for Windows with
# SQLCIPHER_CRYPTO_LIBTOMCRYPT: CommonCrypto doesn't exist there), Apple's ALAC files.
# rekordbox's library key: as helper/build.sh gets it (pyrekordbox's two files, or
# RBSP_LIBRARY_KEY_FILE), checked against helper/build.sh's LIBKEY_SHA256 and fetched at build
# time only (never in the repository), written into libkey.h in helper/build.sh's encoding, which
# library.c decodes (checked on this Mac with CommonCrypto); never printed. That encoding hides
# nothing: the key is public, and the owner dropped hiding it on Windows (2026-11-12).
#
# Gates (the build stops on the first that fails):
#  - every download's sha256 and every component's licence files (deps.sh);
#  - Media Foundation in the pinned llvm-mingw (Q14, for T10): mfapi.h, mfidl.h, mfreadwrite.h,
#    mferror.h, mfobjects.h, libmfplat.a and libmfreadwrite.a there, exporting what aac_mf.c will
#    call, and a translation unit using them compiles; libmfuuid.a is linked into nothing;
#    helper/mf_guids.h's GUIDs (aac_mf.c's own) equal the headers' (defined with INITGUID);
#  - libFLAC and LibTomCrypt with no pthread_, _Unwind_ or __cxa_ symbol (I52);
#  - our sources with -Wall -Wextra -Werror;
#  - each exe: PE32+ x86-64, a console program, ASLR (high entropy), NX; imports only KERNEL32,
#    ntdll, ADVAPI32, SHELL32, ole32 and api-ms-win-crt-* (never bcrypt, I31, nor mfplat or
#    mfreadwrite, aac_mf.c's: loaded by their full System32 paths at run time), no delay-loaded
#    import; SetDefaultDllDirectories among them; the manifest in it; no pthread_/_Unwind_/__cxa_ symbol; libmingwex members only from a reviewed list (no
#    Cephes-derived code, release.md §9); no libmfuuid.a in its link map;
#  - sin and pow (the resampler's) and, in rbsp-prepare.exe, cos imported from
#    api-ms-win-crt-math-l1-1-0.dll, the OS's UCRT, and defined by nothing linked in (helper.md §0
#    finding 4, re-checked here);
#  - built twice (the second time from a copy of the sources under another path, with a space):
#    the same bytes.
#
# Two test variants of every exe, never a release (scripts/build-windows.sh refuses both):
#   helper/build-win.sh --test-timing OUT   the performance timing lines on stderr (helper.md §2a,
#                                           as helper/build.sh --test-timing; Windows' track lines
#                                           add power= and ep=)
#   RBSP_RESEARCH=1 helper/build-win.sh OUT rbsp-prepare.exe with --keys (T1's research flag, for
#                                           the T16/T17 key checks) and the unproved format switches
# A build with neither has no "rbsp-timing" and no "--keys" in rbsp-prepare.exe (gated).
# --version X.Y.Z sets the resource stamp (default: VERSION).
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
cc="$root/scripts/win-cc.sh"
deps="$root/scripts/deps.sh"
usage() { echo "usage: $0 [--test-timing] [--version X.Y.Z] [OUT]" >&2; exit 2; }
timing="" research="" version=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    --test-timing) timing=-DRBSP_TEST_TIMING; shift ;;
    --version) [ "$#" -ge 2 ] && [ -n "$2" ] || usage; version="$2"; shift 2 ;;
    --*) usage ;;
    *) break ;;
  esac
done
[ "$#" -le 1 ] || usage
[ -z "$timing" ] || [ "$#" -eq 1 ] || usage
version="${version:-$(tr -d '[:space:]' < "$root/VERSION")}"
case "$version" in *[!0-9.]*) usage ;; esac
printf '%s\n' "$version" | grep -qxE '[0-9]+\.[0-9]+\.[0-9]+' || usage
if [ "${RBSP_RESEARCH:-}" = 1 ]; then research=-DRBSP_RESEARCH; echo "RESEARCH BUILD (--keys)"; fi
out="${1:-$root/build/helper-win}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
cache="$root/build/helper-win-deps"
fail() { echo "helper/build-win.sh: $*" >&2; exit 1; }
jobs="$(sysctl -n hw.ncpu 2> /dev/null || echo 4)"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/rbsp-helper-win.XXXXXX")"   # holds the key while the build runs
trap 'rm -rf "${scratch:?}"' EXIT
tc="$root/build/llvm-mingw"
"$cc" --fetch
nm_() { "$cc" --tool x64 llvm-nm "$@"; }
pe_dump() { "$cc" --tool x64 llvm-objdump "$@"; }   # the pinned llvm-objdump, never the host's
# libtool and makefiles can't take the repository's path (spaces): the tools through wrappers here
printf '#!/bin/sh\nexec "%s" x64 -Wno-unused-command-line-argument "$@"\n' "$cc" > "$scratch/cc"
for t in ar ranlib nm; do printf '#!/bin/sh\nexec "%s/bin/llvm-%s" "$@"\n' "$tc" "$t" > "$scratch/$t"; done
chmod +x "$scratch/cc" "$scratch/ar" "$scratch/ranlib" "$scratch/nm"
no_runtime_syms() {   # FILE WHAT: no pthread_/_Unwind_/__cxa_ symbol in it (I52)
  n="$(nm_ "$1" 2> /dev/null | grep -cE 'pthread_|_Unwind_|__cxa_' || true)"
  [ "$n" = 0 ] || fail "$2 refers to $n pthread_/_Unwind_/__cxa_ symbols"
}

# ---------------------------------------------------------------- Media Foundation (Q14, for T10)

inc="$tc/generic-w64-mingw32/include" lib="$tc/x86_64-w64-mingw32/lib"
for h in mfapi.h mfidl.h mfreadwrite.h mferror.h mfobjects.h; do [ -f "$inc/$h" ] || fail "llvm-mingw has no $h"; done
for l in libmfplat.a libmfreadwrite.a; do [ -f "$lib/$l" ] || fail "llvm-mingw has no $l"; done
for s in MFStartup MFShutdown MFCreateAttributes MFCreateMediaType MFAllocateWorkQueue MFUnlockWorkQueue MFCreateAsyncResult \
  MFInvokeCallback MFPutWorkItem; do
  nm_ "$lib/libmfplat.a" 2> /dev/null | grep -q " __imp_$s\$" || fail "libmfplat.a doesn't export $s"
done
nm_ "$lib/libmfreadwrite.a" 2> /dev/null | grep -q ' __imp_MFCreateSourceReaderFromByteStream$' \
  || fail "libmfreadwrite.a doesn't export MFCreateSourceReaderFromByteStream"
cat > "$scratch/mf.c" << 'C'
#define COBJMACROS
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
/* what aac_mf.c will use (T10): declared and complete in these headers */
HRESULT mf_check(IMFByteStream *bs, IMFAttributes *a, IMFSourceReader **r, IMFMediaType *t, IMFSample *s);
HRESULT mf_check(IMFByteStream *bs, IMFAttributes *a, IMFSourceReader **r, IMFMediaType *t, IMFSample *s) {
    DWORD q = 0;
    HRESULT h = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (SUCCEEDED(h)) h = MFAllocateWorkQueue(&q);
    if (SUCCEEDED(h)) h = MFCreateSourceReaderFromByteStream(bs, a, r);
    if (SUCCEEDED(h)) h = IMFSourceReader_SetCurrentMediaType(*r, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, t);
    if (SUCCEEDED(h)) h = IMFSourceReader_ReadSample(*r, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, NULL, NULL, NULL, NULL);
    IMFMediaBuffer *b = NULL;
    if (SUCCEEDED(h)) h = IMFSample_ConvertToContiguousBuffer(s, &b);
    MFUnlockWorkQueue(q);
    MFShutdown();
    return h == MF_E_INVALIDREQUEST ? E_FAIL : h;
}
C
(cd "$scratch" && "$cc" x64 -Wall -Wextra -Werror -Wno-unused-command-line-argument -c mf.c -o mf.o) > "$scratch/mf.log" 2>&1 \
  || { cat "$scratch/mf.log" >&2; fail "Media Foundation's headers don't compile what aac_mf.c needs"; }
# aac_mf.c's own GUIDs (helper/mf_guids.h; libmfuuid.a is never linked) against the headers': each
# header GUID defined with INITGUID and ours side by side in one object, their 16 bytes compared
guids="$(sed -n 's/^[[:space:]]*X(\([A-Za-z_][A-Za-z_0-9]*\),.*/\1/p' "$here/mf_guids.h")"
cat > "$scratch/guids-count.c" << 'C'
#include "mf_guids.h"
#define X(...) rb_mf_guid_row
RBMF_GUIDS(X)
C
rows="$("$cc" x64 -Wno-unused-command-line-argument -E -P -I"$here" "$scratch/guids-count.c")" \
  || fail "helper/mf_guids.h: its GUID list couldn't be preprocessed"
[ -n "$guids" ] && [ "$(echo "$guids" | wc -w)" -eq "$(echo "$rows" | wc -w)" ] \
  || fail "helper/mf_guids.h: its GUID list doesn't match the preprocessed row count"
cat > "$scratch/guids.c" << 'C'
#define INITGUID
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include "mf_guids.h"
#define X(n, l, w1, w2, b0, b1, b2, b3, b4, b5, b6, b7) \
    __attribute__((section(".rdata$rb_" #n))) const GUID rb_##n = {l, w1, w2, {b0, b1, b2, b3, b4, b5, b6, b7}}; \
    const GUID *const rb_their_##n = &n;
RBMF_GUIDS(X)
C
(cd "$scratch" && "$cc" x64 -Wall -Wextra -Werror -Wno-unused-command-line-argument -Wno-missing-field-initializers -I"$here" \
  -c guids.c -o guids.o) > "$scratch/guids.log" 2>&1 || { cat "$scratch/guids.log" >&2; fail "helper/mf_guids.h's check doesn't compile"; }
gbytes() { pe_dump -s -j "$1" "$scratch/guids.o" | sed -n 's/^ 0000 \([0-9a-f ]*\).*/\1/p' | tr -d ' ' | cut -c1-32; }
for g in $guids; do
  theirs="$(gbytes ".rdata\$$g")" ours="$(gbytes ".rdata\$rb_$g")"
  [ ${#theirs} = 32 ] || fail "llvm-mingw's headers don't define $g"
  [ "$theirs" = "$ours" ] || fail "helper/mf_guids.h: $g is $ours, the headers say $theirs"
done

# ---------------------------------------------------------------- the dependencies

unpack() {   # NAME: the download unpacked into $scratch/NAME, its path printed
  t="$("$deps" fetch "$1")"
  mkdir -p "$scratch/$1"
  case "$t" in *.tar.xz) tar -xJf "$t" -C "$scratch/$1" ;; *.tar.gz) tar -xzf "$t" -C "$scratch/$1" ;; *) fail "$t: not a tarball" ;; esac
  printf '%s\n' "$scratch/$1"
}
FLAC_VERSION="$("$deps" get flac version)"
SQLCIPHER_VERSION="$("$deps" get sqlcipher version)"
LTC_VERSION="$("$deps" get libtomcrypt version)"
flacd="$cache/flac-$FLAC_VERSION-x64"
ltcd="$cache/libtomcrypt-$LTC_VERSION-x64"
sqlcd="$cache/sqlcipher-$SQLCIPHER_VERSION"
# Each cached build carries a stamp: the download's sha256 from deps.lock, the recipe below, the
# toolchain's pin and scripts/win-cc.sh's own sha256 (its flags compile libFLAC and LibTomCrypt).
# A cache whose stamp differs is built again, so a changed flag, lock row, toolchain or wrapper
# never reuses an old build (a release build starts from an empty cache anyway).
TC_PIN="$("$deps" get llvm-mingw sha256) win-cc.sh $(shasum -a 256 "$cc" | cut -d' ' -f1)"
# libtool can't tell lld and llvm-ar from MSVC's tools for a mingw host by itself: the overrides
FLAC_CFG="--host=x86_64-w64-mingw32 --enable-static --disable-shared --disable-ogg --disable-programs --disable-examples
  --disable-cpplibs --disable-doxygen-docs --disable-version-from-git --disable-multithreading
  am_cv_ar_interface=ar lt_cv_ar_interface=ar lt_cv_prog_gnu_ld=yes lt_cv_sys_max_cmd_len=8192"
FLAC_CFLAGS="-O2 -DFLAC__NO_DLL -D__USE_MINGW_ANSI_STDIO=0"
LTC_CFLAGS="-O2 -w"
SQLC_CFG="--with-tempstore=yes --disable-tcl --disable-shared"
stamp() { printf 'sha256 %s\nrecipe %s\ntoolchain %s\n' "$("$deps" get "$1" sha256)" "$(echo $2)" "$TC_PIN"; }
fresh() { [ -f "$1/.stamp" ] && [ "$(cat "$1/.stamp")" = "$2" ]; }   # DIR STAMP

# libFLAC as the Windows bridge builds it (bridge/win/build.sh), with the UCRT's printf and strtod
want="$(stamp flac "configure $FLAC_CFG CFLAGS=$FLAC_CFLAGS")"
if ! fresh "$flacd" "$want"; then
  d="$(unpack flac)"
  "$deps" licence flac.lib "$d"
  (
    cd "$d/flac-$FLAC_VERSION"
    # configure and make expand these as command words.
    export PATH="$scratch:$PATH"
    # shellcheck disable=SC2086
    ./configure --quiet $FLAC_CFG CC=cc AR=ar RANLIB=ranlib NM=nm \
      CFLAGS="$FLAC_CFLAGS"
    make -s -j"$jobs" -C src/libFLAC
  ) > "$scratch/flac-build.log" 2>&1 || { tail -30 "$scratch/flac-build.log" >&2; fail "libFLAC's Windows build"; }
  rm -rf "$flacd"
  mkdir -p "$flacd/lib" "$flacd/include/FLAC"
  cp "$d/flac-$FLAC_VERSION/src/libFLAC/.libs/libFLAC.a" "$flacd/lib/"
  cp "$d/flac-$FLAC_VERSION/include/FLAC/"*.h "$flacd/include/FLAC/"
  printf '%s\n' "$want" > "$flacd/.stamp"
fi
no_runtime_syms "$flacd/lib/libFLAC.a" "libFLAC for Windows (--disable-multithreading?)"

# LibTomCrypt: its own makefile, the whole library (the link takes only what SQLCipher calls:
# AES, SHA-1/256/512, HMAC, PBKDF2, CBC, Fortuna, rng_get_bytes' CryptGenRandom)
want="$(stamp libtomcrypt "make -f makefile.unix CFLAGS=$LTC_CFLAGS libtomcrypt.a")"
if ! fresh "$ltcd" "$want"; then
  d="$(unpack libtomcrypt)"
  "$deps" licence libtomcrypt.lib "$d"
  (
    cd "$d/libtomcrypt-$LTC_VERSION"
    export PATH="$scratch:$PATH"
    make -s -j"$jobs" -f makefile.unix CC=cc AR=ar ARFLAGS=r RANLIB=ranlib \
      CFLAGS="$LTC_CFLAGS" EXTRALIBS= libtomcrypt.a
  ) > "$scratch/ltc-build.log" 2>&1 || { tail -30 "$scratch/ltc-build.log" >&2; fail "LibTomCrypt's Windows build"; }
  rm -rf "$ltcd"
  mkdir -p "$ltcd/lib"
  cp "$d/libtomcrypt-$LTC_VERSION/libtomcrypt.a" "$ltcd/lib/"
  cp -R "$d/libtomcrypt-$LTC_VERSION/src/headers" "$ltcd/include"
  printf '%s\n' "$want" > "$ltcd/.stamp"
fi
no_runtime_syms "$ltcd/lib/libtomcrypt.a" "LibTomCrypt for Windows"

# SQLCipher's amalgamation, made on this Mac (as helper/build.sh makes it), compiled below
want="$(stamp sqlcipher "configure $SQLC_CFG CC=clang; make sqlite3.c")"
if ! fresh "$sqlcd" "$want"; then
  d="$(unpack sqlcipher)"
  "$deps" licence sqlcipher.lib "$d"
  (
    cd "$d/sqlcipher-$SQLCIPHER_VERSION"
    # shellcheck disable=SC2086
    ./configure $SQLC_CFG CC=clang
    make sqlite3.c
  ) > "$scratch/sqlc-build.log" 2>&1 || { tail -30 "$scratch/sqlc-build.log" >&2; fail "SQLCipher's amalgamation"; }
  rm -rf "$sqlcd"
  mkdir -p "$sqlcd"
  cp "$d/sqlcipher-$SQLCIPHER_VERSION/sqlite3.c" "$d/sqlcipher-$SQLCIPHER_VERSION/sqlite3.h" "$sqlcd/"
  printf '%s\n' "$want" > "$sqlcd/.stamp"
fi

# Apple's ALAC files, unmodified (deps.lock's group alac); the C++ decoder class is helper/alac.c
alac="$scratch/alac"
"$deps" group alac "$alac"
"$deps" licence alac.decoder "$alac"

# ---------------------------------------------------------------- the library key

LIBKEY_SHA256="$(sed -n 's/^LIBKEY_SHA256=//p' "$here/build.sh")"
[ -n "$LIBKEY_SHA256" ] || fail "no LIBKEY_SHA256 in helper/build.sh"
keyfile="${RBSP_LIBRARY_KEY_FILE:-}"
if [ -n "$keyfile" ]; then
  [ -r "$keyfile" ] || fail "RBSP_LIBRARY_KEY_FILE: $keyfile can't be read"
else
  db_py="$("$deps" fetch pyrekordbox-database)"
  utils_py="$("$deps" fetch pyrekordbox-utils)"
  "$deps" licence pyrekordbox-database.key "$(dirname "$("$deps" fetch pyrekordbox-licence)")"
  keyfile="$scratch/libkey.raw"
  # helper/build.sh's reading, word for word: base85, XOR with BLOB_KEY, zlib; literals only
  python3 -I - "$db_py" "$utils_py" > "$keyfile" << 'PY' || fail "couldn't read the key from pyrekordbox's files"
import ast, base64, re, sys, zlib
def literal(path, name):
    m = re.search(r'^' + name + r'\s*=\s*(b"[^"]*")', open(path, encoding="utf-8").read(), re.M)
    if not m:
        sys.exit(path + ": no " + name)
    return ast.literal_eval(m.group(1))
blob, k = literal(sys.argv[1], "BLOB"), literal(sys.argv[2], "BLOB_KEY")
data = base64.b85decode(blob)
sys.stdout.buffer.write(zlib.decompress(bytes(b ^ k[i % len(k)] for i, b in enumerate(data))))
PY
fi
if [ "$(tr -d '\r\n' < "$keyfile" | shasum -a 256 | cut -d' ' -f1)" != "$LIBKEY_SHA256" ]; then
  fail "$keyfile doesn't hold rekordbox's library key (sha256 differs)"
fi
obj="$scratch/obj"
mkdir -p "$obj"
# helper/build.sh's encoding: byte i stored as key[i] + 0x5a + 13i (library.c undoes it)
tr -d '\r\n' < "$keyfile" | od -An -tu1 -v | awk '
  BEGIN { n = 0 }    # numeric from the start: an unset n indexes b[""], not b[0]
  { for (f = 1; f <= NF; f++) { b[n] = ($f + 90 + 13 * n) % 256; n++ } }
  END { printf "static const unsigned char LIBKEY[] = {"; for (i = 0; i < n; i++) printf "%s%d", i ? "," : "", b[i];
        printf "};\n#define LIBKEY_LEN %d\n", n }' > "$obj/libkey.h"
# the encoded bytes must give the key back (its sha256), with library.c's decoding, on this Mac
cat > "$scratch/keycheck.c" << 'C'
#include <stdio.h>
#include <CommonCrypto/CommonDigest.h>
#include "libkey.h"
int main(void) {
    unsigned char k[LIBKEY_LEN], h[32];
    for (int i = 0; i < LIBKEY_LEN; i++) k[i] = (unsigned char)(LIBKEY[i] - 0x5a - 13 * i);
    CC_SHA256(k, LIBKEY_LEN, h);
    for (int i = 0; i < 32; i++) printf("%02x", h[i]);
    printf("\n");
    return 0;
}
C
clang -w -I"$obj" "$scratch/keycheck.c" -o "$scratch/keycheck"
[ "$("$scratch/keycheck")" = "$LIBKEY_SHA256" ] || fail "the encoded key doesn't decode to the key: the encoding and library.c's differ"
grep -q 'LIBKEY\[i\] - 0x5a - 13 \* i' "$here/library.c" || fail "library.c's decoding isn't the one checked here"
rm -f "$scratch/keycheck" "$scratch/keycheck.c" "$scratch/libkey.raw"

# ---------------------------------------------------------------- compile and link

# the repository's path has spaces, which compile lines split on: the dependencies by a link here
ln -s "$cache" "$scratch/deps"
flacd="$scratch/deps/flac-$FLAC_VERSION-x64" ltcd="$scratch/deps/libtomcrypt-$LTC_VERSION-x64" sqlcd="$scratch/deps/sqlcipher-$SQLCIPHER_VERSION"
# pocketfft: plat_win.c's inverse FFT (scripts/win-deps.sh's pin, the bridge's), linked with it
pf="$scratch/pocketfft"
cp -R "$("$root/scripts/win-deps.sh" pocketfft)" "$pf"
# BlocksRuntime (the blocks of kept.c and prepare.c, plat_dir_each_b): the bridge's pin and recipe
# (bridge/win/build.sh), its own Windows branch's warnings off
rt="$scratch/blocksrt"
cp -R "$("$root/scripts/win-deps.sh" blocksrt)" "$rt"

SQLC_DEFS="-DSQLITE_HAS_CODEC -DSQLCIPHER_CRYPTO_LIBTOMCRYPT -DSQLITE_TEMP_STORE=2 -DSQLITE_THREADSAFE=1
  -DSQLITE_EXTRA_INIT=sqlcipher_extra_init -DSQLITE_EXTRA_SHUTDOWN=sqlcipher_extra_shutdown
  -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DQS=0"
# the UCRT's printf (an OS component, C99's formats), not mingw-w64's own: -std=c11 turns that on,
# and it links gdtoa and mingw_pformat from libmingwex
WARN="-Wall -Wextra -Werror -Wno-unused-command-line-argument -D__USE_MINGW_ANSI_STDIO=0${timing:+ $timing}"
STRICT="-Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wformat=2"   # common/'s, as common/tests/win.sh
LIBS="-lntdll -ladvapi32 -lshell32 -lole32"
# shellcheck disable=SC2086
# the amalgamation with its PBKDF2 hook (helper/sqlc_kdf.c includes sqlite3.c): CNG's derivation
"$scratch/cc" -w -D__USE_MINGW_ANSI_STDIO=0 $(echo $SQLC_DEFS) -I"$ltcd/include" -I"$sqlcd" -I"$here" -c "$here/sqlc_kdf.c" -o "$obj/sqlite3.o" \
  || fail "SQLCipher for Windows"
# the dummy library key the rules test's library.c gets (never rekordbox's: win-rules.c's fixture)
mkdir -p "$scratch/dummykey"
printf 'static const unsigned char LIBKEY[] = {0};\n#define LIBKEY_LEN 1\n' > "$scratch/dummykey/libkey.h"
"$scratch/cc" -w -I"$pf" -c "$pf/pocketfft.c" -o "$obj/pocketfft.o" || fail "pocketfft for Windows"
for r in runtime data; do
  "$scratch/cc" -w -Wno-incompatible-pointer-types -I"$rt" -c "$rt/$r.c" -o "$obj/blocks_$r.o" || fail "BlocksRuntime's $r.c"
done
build_resource() {
  v1="${version%%.*}"; rest="${version#*.}"; v2="${rest%%.*}"; v3="${rest#*.}"
  "$cc" --tool x64 windres -I"$here/win" -D "RBSP_VERSION_NUM=$v1,$v2,$v3,0" -D "RBSP_VERSION=$version" \
    "$here/win/rbsp.rc" -O coff -o "$obj/rbsp.res.o" || fail "the manifest and version resources"
}
build_resource
# Apple's ALACBitUtilities.c and dp_dec.c: -fwrapv, as its predictor shifts and overflows signed
# values on a corrupt packet
for f in "$alac"/*.c; do "$scratch/cc" -w -fwrapv -c "$f" -o "$obj/alac_$(basename "$f" .c).o" || fail "$f"; done
# the planted DLL that rbsp-win-plat-test.exe must not find by name
printf 'int rbsp_planted(void);\nint rbsp_planted(void) { return 1; }\n' > "$scratch/planted.c"
"$scratch/cc" -shared "$scratch/planted.c" -o "$out/rbsp-planted.dll"

# build SRC OUTDIR: our sources from SRC (a copy of the repository's helper/ and common/) into
# OUTDIR's exes, relative names only (the repository's path has spaces), each linked with a map
build() {
  src="$1" o="$2"
  mkdir -p "$o/obj"
  sh "$root/helper/tests/kept-parser.sh" > "$o/obj/kept-parser.inc" || return 1
  (
    cd "$src" || exit 1
    set -- -Ihelper -Icommon -Ibridge -I"$o/obj" -I"$obj" -I"$sqlcd" -I"$flacd/include" -I"$alac" -I"$pf" -I"$rt"
    for f in common/plat_win.c common/plat_helper.c common/plat_helper_win.c common/path_rules.c common/entries.c helper/big_plat.c helper/decode.c \
      helper/pcm.c helper/m4a.c helper/alac.c helper/mp3.c helper/resample_win.c helper/library.c helper/kdf_win.c helper/manifest.c \
      helper/readable.c helper/stdin_stop.c helper/aac_mf.c helper/prepare.c helper/model.c helper/entry.c helper/kept.c \
      helper/tests/aac_mf_probe.c helper/tests/decode_dump.c helper/tests/win-plat.c helper/tests/win-library.c \
      helper/tests/win-rules.c helper/tests/win-readable.c helper/tests/win-stop.c helper/tests/kept-check.c; do
      x=""
      case "$f" in
        helper/resample_win.c) x="-ffp-contract=off${research:+ $research}" ;; helper/decode.c | helper/aac_mf.c) x="${research:+$research}" ;; helper/pcm.c | helper/entry.c) x="-DFLAC__NO_DLL" ;; common/plat_win.c) x="-DPLAT_NO_BLOCKS" ;;
        common/plat_helper* | common/path_rules.c) x="$STRICT" ;; helper/kept.c) x="-fblocks" ;; helper/prepare.c) x="-fblocks${research:+ $research}" ;;
      esac
      # shellcheck disable=SC2086
      "$cc" x64 $WARN $x "$@" -c "$f" -o "$o/obj/$(basename "$f" .c).o" || exit 1
    done
    # plat_win.c again with its block forms (plat_dir_each_b: kept.c's and prepare.c's), for the helper only
    # shellcheck disable=SC2086
    "$cc" x64 $WARN -fblocks "$@" -c common/plat_win.c -o "$o/obj/plat_win_blocks.o" || exit 1
    # library.c again with the dummy key, for the rules test
    # shellcheck disable=SC2086
    "$cc" x64 $WARN -I"$scratch/dummykey" "$@" -c helper/library.c -o "$o/obj/library_dummykey.o" || exit 1
    # readable.c again with its test gate (RBSP_READABLE_TEST), for the readable test only
    # shellcheck disable=SC2086
    "$cc" x64 $WARN -DRBSP_READABLE_TEST "$@" -c helper/readable.c -o "$o/obj/readable_gate.o" || exit 1
  ) > "$o/cc.log" 2>&1 || { cat "$o/cc.log" >&2; fail "the compile failed"; }
  set -- "$o/obj/plat_win.o" "$o/obj/plat_helper.o" "$o/obj/plat_helper_win.o" "$o/obj/path_rules.o" "$obj/pocketfft.o"
  link() {   # EXE OBJECTS...
    e="$1"; shift
    # shellcheck disable=SC2086
    "$cc" x64 -Wno-unused-command-line-argument "$@" "$obj/rbsp.res.o" $LIBS -Wl,-Map="$o/$e.map" -o "$o/$e" 2> "$o/$e.link.log" \
      || { cat "$o/$e.link.log" >&2; fail "linking $e"; }
  }
  # the helper: on plat_win.c's block forms, with BlocksRuntime (its four block symbols' LNK4217
  # warnings are the only ones expected, as the bridge's)
  link rbsp-prepare.exe -Wl,--undefined=rb_resample "$o/obj/prepare.o" "$o/obj/model.o" "$o/obj/entry.o" "$o/obj/kept.o" \
    "$o/obj/manifest.o" "$o/obj/library.o" "$o/obj/kdf_win.o" "$obj/sqlite3.o" "$o/obj/readable.o" "$o/obj/stdin_stop.o" \
    "$o/obj/decode.o" "$o/obj/aac_mf.o" "$o/obj/pcm.o" "$o/obj/m4a.o" "$o/obj/alac.o" "$obj"/alac_*.o "$o/obj/mp3.o" \
    "$o/obj/resample_win.o" "$o/obj/big_plat.o" "$o/obj/entries.o" "$o/obj/plat_win_blocks.o" "$o/obj/plat_helper.o" \
    "$o/obj/plat_helper_win.o" "$o/obj/path_rules.o" "$obj/pocketfft.o" "$obj/blocks_runtime.o" "$obj/blocks_data.o" \
    "$flacd/lib/libFLAC.a" "$ltcd/lib/libtomcrypt.a"
  # the decode tool, with rekordbox for Windows' resampler (resample_win.c)
  link rbsp-decode-dump.exe -Wl,--undefined=rb_resample "$o/obj/decode_dump.o" "$o/obj/decode.o" "$o/obj/aac_mf.o" "$o/obj/pcm.o" \
    "$o/obj/m4a.o" "$o/obj/alac.o" "$obj"/alac_*.o "$o/obj/mp3.o" "$o/obj/resample_win.o" "$o/obj/big_plat.o" \
    "$o/obj/readable.o" "$@" "$flacd/lib/libFLAC.a"
  link rbsp-aac-mf-probe.exe "$o/obj/aac_mf_probe.o" "$o/obj/aac_mf.o" "$o/obj/big_plat.o" "$o/obj/readable.o" "$@"
  link rbsp-win-plat-test.exe "$o/obj/win-plat.o" "$o/obj/big_plat.o" "$@"
  link rbsp-win-library-test.exe "$o/obj/win-library.o" "$o/obj/library.o" "$o/obj/kdf_win.o" "$obj/sqlite3.o" "$@" "$ltcd/lib/libtomcrypt.a"
  link rbsp-win-rules-test.exe "$o/obj/win-rules.o" "$o/obj/library_dummykey.o" "$o/obj/manifest.o" "$o/obj/kdf_win.o" "$obj/sqlite3.o" \
    "$@" "$ltcd/lib/libtomcrypt.a"
  link rbsp-win-readable-test.exe "$o/obj/win-readable.o" "$o/obj/readable_gate.o" "$@"
  link rbsp-win-stop-test.exe "$o/obj/win-stop.o" "$o/obj/stdin_stop.o" "$@"
  link rbsp-kept-check.exe "$o/obj/kept-check.o" "$o/obj/entries.o" "$@"
}
EXES="rbsp-prepare.exe rbsp-decode-dump.exe rbsp-aac-mf-probe.exe rbsp-win-plat-test.exe rbsp-win-library-test.exe rbsp-win-rules-test.exe rbsp-win-readable-test.exe rbsp-win-stop-test.exe rbsp-kept-check.exe"
copy_src() {   # DIR: helper/ and common/ copied there
  mkdir -p "$1/helper/tests" "$1/helper/win" "$1/common" "$1/bridge"
  cp "$root/bridge/onnxruntime_c_api.h" "$1/bridge/"    # model.c's ORT 1.18 C API (vendored, MIT)
  cp "$here"/*.c "$here"/*.h "$1/helper/"
  cp "$here"/tests/*.c "$1/helper/tests/"
  cp "$root"/common/*.c "$root"/common/*.h "$1/common/"
  mkdir -p "$1/common/tests" && cp "$root"/common/tests/check.h "$1/common/tests/"
}
copy_src "$scratch/src"
build "$scratch/src" "$scratch/first"
# the same sources and version from another folder, with a space in its path: the same bytes
copy_src "$scratch/copy of src"
build "$scratch/copy of src" "$scratch/second"
for e in $EXES; do
  cmp -s "$scratch/first/$e" "$scratch/second/$e" || fail "$e isn't reproducible: it differs when built from a copy of the sources"
  cp "$scratch/first/$e" "$scratch/first/$e.map" "$out/"
done
# the helper's link warnings: none but LNK4217 on the four block ABI symbols (BlocksRuntime linked
# in), as the bridge's
log="$scratch/first/rbsp-prepare.exe.link.log"
warn="$(grep -c 'warning' "$log" || true)"
l4217="$(grep -cE 'locally defined symbol imported: (_NSConcreteGlobalBlock|_NSConcreteStackBlock|_Block_object_assign|_Block_object_dispose) ' "$log" || true)"
[ "$warn" = "$l4217" ] || { cat "$log" >&2; fail "rbsp-prepare.exe: link warnings other than the four block symbols' LNK4217"; }

# ---------------------------------------------------------------- gates

# libmingwex members linked in: a reviewed allowlist, per exe (no Cephes-derived code, release.md
# §9): the three Control Flow Guard ones, and the two an exe's start-up code brings (CRT_fp10: the
# x87 control word; __mingw_setusermatherr: the _matherr hook), neither a math function; and
# mingw-w64's own vsnprintf, which libFLAC's share/compat.h calls by name (__mingw_vsnprintf) on
# mingw: mingw_vsnprintf, mingw_pformat and David Gay's gdtoa (gdtoa, dmisc, gmisc, misc), under the
# runtime licence file's gdtoa notice (win/NOTICE.txt), not Cephes
for e in $EXES; do
  x="$out/$e"
  hdr="$(pe_dump -p "$x")"
  echo "$hdr" | grep -q '^Magic.*020b' || fail "$e: not PE32+"
  pe_dump -f "$x" | grep -qi 'file format coff-x86-64' || fail "$e: not x86-64"
  echo "$hdr" | grep -qE '^Subsystem[[:space:]]+0+3([[:space:]]|$)' || fail "$e: not a console program"
  dc="$(echo "$hdr" | sed -n 's/^DllCharacteristics[[:space:]]*//p' | cut -c1-8)"
  [ $((0x$dc & 0x160)) = $((0x160)) ] || fail "$e: DllCharacteristics $dc lacks HIGH_ENTROPY_VA, DYNAMIC_BASE or NX_COMPAT"
  dlls="$(echo "$hdr" | sed -n 's/.*DLL Name: //p' | sort -u | tr '\n' ' ')"
  extra="$(echo "$dlls" | tr ' ' '\n' | grep -v -i -e '^$' -e '^kernel32\.dll$' -e '^advapi32\.dll$' -e '^ntdll\.dll$' -e '^shell32\.dll$' \
    -e '^ole32\.dll$' -e '^api-ms-win-crt-' || true)"
  [ -z "$extra" ] || fail "$e: imports outside the allowlist: $extra"
  echo "$hdr" | grep -qE '^Entry d 0+ 0+ Delay Import Directory$' || fail "$e: a delay-loaded import"
  echo "$hdr" | grep -qE '[[:space:]]SetDefaultDllDirectories$' || fail "$e: doesn't call SetDefaultDllDirectories (plat_exe_start)"
  grep -aqF '<activeCodePage xmlns="http://schemas.microsoft.com/SMI/2019/WindowsSettings">UTF-8</activeCodePage>' "$x" \
    || fail "$e: the manifest (UTF-8 code page) isn't in it"
  syms="$(nm_ "$x" 2> /dev/null | grep -cE 'pthread_|_Unwind_|__cxa_' || true)"
  mapped="$(grep -cE 'pthread_|_Unwind_|__cxa_' "$out/$e.map" || true)"
  [ "$syms" = 0 ] && [ "$mapped" = 0 ] || fail "$e: pthread_/_Unwind_/__cxa_ symbols linked in ($syms in the symbols, $mapped in the map)"
  if grep -q 'libmfuuid' "$out/$e.map"; then fail "$e: libmfuuid.a is linked in (owner decided: never)"; fi
  mx="$(grep -oE 'lib64_libmingwex_a-[A-Za-z0-9_]+\.o' "$out/$e.map" | sort -u | tr '\n' ' ')"
  bad="$(echo "$mx" | tr ' ' '\n' | grep -v -e '^$' -e '^lib64_libmingwex_a-guard_dispatch\.o$' -e '^lib64_libmingwex_a-loadcfg\.o$' \
    -e '^lib64_libmingwex_a-mingw_cfguard_support\.o$' -e '^lib64_libmingwex_a-CRT_fp10\.o$' \
    -e '^lib64_libmingwex_a-__mingw_setusermatherr\.o$' -e '^lib64_libmingwex_a-mingw_vsnprintf\.o$' \
    -e '^lib64_libmingwex_a-mingw_pformat\.o$' -e '^lib64_libmingwex_a-gdtoa\.o$' -e '^lib64_libmingwex_a-dmisc\.o$' \
    -e '^lib64_libmingwex_a-gmisc\.o$' -e '^lib64_libmingwex_a-misc\.o$' || true)"
  [ -z "$bad" ] || fail "$e: libmingwex members outside the reviewed list (no Cephes, release.md §9): $bad"
done
# These functions must come from the OS's UCRT math set (helper.md §0 4).
for e in rbsp-decode-dump.exe rbsp-prepare.exe; do
  expected="pow sin "
  if [ "$e" = rbsp-prepare.exe ]; then expected="cos pow sin "; fi
  hdr="$(pe_dump -p "$out/$e")"
  math="$(echo "$hdr" | awk '/DLL Name: /{d=$3} d == "api-ms-win-crt-math-l1-1-0.dll" && $NF ~ /^(cos|pow|sin)$/ {print $NF}' | sort | tr '\n' ' ')"
  [ "$math" = "$expected" ] || fail "$e: expected ${expected% } from api-ms-win-crt-math-l1-1-0.dll (found ${math% })"
done
# (imported means nothing linked in defines them: the linker takes a definition before an import
# library's; and the libmingwex list above has no math member)

# the test variants say so in the helper's bytes; a build that is neither has none of theirs
if [ -n "$timing" ]; then grep -aqF 'rbsp-timing track' "$out/rbsp-prepare.exe" || fail "the timing build has no timing lines"
elif grep -aqF 'rbsp-timing' "$out/rbsp-prepare.exe"; then fail "rbsp-prepare.exe has the timing lines (a test build's)"; fi
if [ -n "$research" ]; then grep -aqF -- '--keys' "$out/rbsp-prepare.exe" || fail "the research build has no --keys"
elif grep -aqF -- '--keys' "$out/rbsp-prepare.exe"; then fail "rbsp-prepare.exe has --keys (a research build's)"; fi
for flag in --test-unproved-aac --test-unproved-mp3 --test-unproved-rates 'AAC MF type change:' 'Candidate resampler table:'; do
  if [ -n "$research" ]; then grep -aqF -- "$flag" "$out/rbsp-prepare.exe" || fail "the research build has no $flag"
  elif grep -aqF -- "$flag" "$out/rbsp-prepare.exe"; then fail "rbsp-prepare.exe has $flag (a research build's)"; fi
done

printf 'built into %s (x64)%s:\n' "$out" "${timing:+ TEST: timing lines}${research:+ RESEARCH: --keys}"
for e in $EXES rbsp-planted.dll; do printf '  %s  %s\n' "$(shasum -a 256 "$out/$e" | cut -d' ' -f1)" "$e"; done
echo "gates passed: pinned deps and licences, Media Foundation (Q14) and its GUIDs, imports, manifest, SetDefaultDllDirectories, no runtime or libmfuuid, libmingwex list, key checked, math imports from the UCRT, reproducible"

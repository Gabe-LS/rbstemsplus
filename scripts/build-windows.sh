#!/bin/bash
# The Windows release files (release.md §3.1, T12; T5/T7's first cut), built on the Mac with the
# pinned llvm-mingw and .NET SDK, into OUT:
#   rbstemsplus-windows.zip            the app folder, in the release layout (win/zip-contents.txt,
#                                      the zip's exact file list, is its gate): RB Stems Plus.exe and
#                                      its .exe.config (win/shell/build.sh), x64\ and arm64\rbsp-core.dll
#                                      (core/build.sh dll), rbsp-watch-x64.exe and rbsp-watch-arm64.exe
#                                      (the watcher, as win/c/tests/watch.sh builds its shipped exes),
#                                      rbsp-prepare.exe (the prepare helper, x64 also on ARM64:
#                                      helper/build-win.sh, its gates; the shell's paths.helper),
#                                      admin\RB Stems Plus.exe (win/admin/build.sh), stub\x64\ and
#                                      stub\arm64\RB Stems Plus.exe (win/c/stub.c), NOTICE,
#                                      NOTICE-windows.txt (win/NOTICE.txt) and LICENSE
#   rbstemsplus-bridge-win-x64.dll     the bridge (bridge/win/build.sh's onnxruntime.dll)
#   windows.env                        what scripts/build.sh writes into payload.json's windows
#                                      block: the app's version, the zip's, the worker's, the two
#                                      stubs' and the bridge's SHA-256
# build.sh runs it only with a windows block (--windows, or a plan's windows.app new). Alone:
#
#   scripts/build-windows.sh OUT                                 a release's: keys/release and
#                                                                keys/backup compiled in
#   scripts/build-windows.sh --test URL --test-key PEM OUT       a TEST build: the app downloads
#                                                                from URL (https://, the TEST
#                                                                server) and trusts PEM's key alone
#                                                                (the core, the worker, the watcher);
#                                                                never a release key
#   ... --stamp VERSION                                          the app stamped VERSION instead of
#                                                                VERSION's (the exe, core DLLs,
#                                                                watchers, prepare helper; release.sh's carry
#                                                                decision builds the previous
#                                                                windows.app.version, release.md 1.3
#                                                                "As built (T13)"); a TEST build may
#                                                                say next (VERSION's patch plus one:
#                                                                the update scenarios' newer zip,
#                                                                WIN_NEXT_APP_ZIP). The worker and
#                                                                the stubs are the same bytes at
#                                                                any stamp. --app-version is the
#                                                                same option (its older name)
#   ... --carry-file ENTRY=PATH                                  the zip's ENTRY is PATH's bytes, a
#                                                                previous release's file carried
#                                                                into a new zip (R4.I83): ENTRY is
#                                                                stub/x64/RB Stems Plus.exe,
#                                                                stub/arm64/RB Stems Plus.exe or
#                                                                admin/RB Stems Plus.exe; repeatable
#
# The zip is deterministic (sorted names, one fixed time, deflate), as the updater's unzip.c reads
# it. Version resources (release.md §3.2, read by scripts/win-version.py): the shell, core DLLs,
# watchers and prepare helper carry the version (ProductVersion, FILEVERSION X.Y.Z.0); the worker
# and stubs have a constant resource with no version (I83); the bridge has none. Every PE's link map and symbols:
# no pthread_, _Unwind_ or __cxa_, no libmfuuid.a (I52, linkgate). Every shipped file's SHA-256
# goes into build/win.sha256 (build/win-test.sha256 for a TEST build); --check-repro builds it all
# again from scratch in another folder and requires the same bytes for every file. rbsp-prepare.exe
# ships from 1.5.0 (owner 2026-11-17: Prepare on Windows in 1.5.0).
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
die() { echo "build-windows.sh: $*" >&2; exit 1; }
usage() { echo "usage: $0 [--test URL --test-key PEM] [--stamp VERSION] [--carry-file ENTRY=PATH]... [--check-repro] OUT" >&2; exit 2; }
test_base="" test_key="" app_version="" check_repro=""
carry_files=()
while [ $# -gt 1 ]; do
  case "$1" in
    --check-repro) check_repro=1; shift ;;
    --test) test_base="$2"; shift 2 ;;
    --test-key) test_key="$2"; shift 2 ;;
    --stamp|--app-version) app_version="$2"; shift 2 ;;
    --carry-file) carry_files+=("$2"); shift 2 ;;
    *) usage ;;
  esac
done
[ $# -eq 1 ] || usage
out="$1"
if [ -n "$test_base$test_key" ]; then
  [ -n "$test_base" ] && [ -n "$test_key" ] || usage
  case "$test_base" in https://*) ;; *) die "--test URL must be https:// (the core downloads over HTTPS only)" ;; esac
  [ -f "$test_key" ] || die "no test key $test_key"
  test_key="$(cd "$(dirname "$test_key")" && pwd)/$(basename "$test_key")"
fi
# a P-256 public key as the 91-byte uncompressed SPKI sig.c takes (sig_spki_p256), whatever point
# form the PEM holds, in base64; anything else fails
pubkey_b64() {
  local text n
  text="$(/usr/bin/openssl ec -pubin -in "$1" -noout -text 2>/dev/null)" && printf '%s\n' "$text" | grep -q 'ASN1 OID: prime256v1' || return 1
  n="$(/usr/bin/openssl ec -pubin -in "$1" -conv_form uncompressed -outform DER 2>/dev/null | wc -c | tr -d ' ')"
  [ "$n" = 91 ] || return 1
  /usr/bin/openssl ec -pubin -in "$1" -conv_form uncompressed -outform DER 2>/dev/null | /usr/bin/base64 | tr -d '\n'
}
if [ -n "$test_key" ]; then
  # never a release key: its public half, in either point form, isn't one of keys/*.pub.pem
  pub="$(pubkey_b64 "$test_key")" || die "$test_key isn't a P-256 public key"
  for k in "$root"/keys/*.pub.pem; do
    [ -f "$k" ] && [ "$pub" = "$(pubkey_b64 "$k" || true)" ] && die "$test_key is a release key ($k): test keys only"
  done
fi
version="$(tr -d '[:space:]' < "$root/VERSION")"
if [ "$app_version" = next ]; then
  [ -n "$test_base" ] || die "--stamp next is a TEST build's only (the update scenarios' newer zip)"
  # VERSION's patch number plus one (1.4.0 -> 1.4.1), as tests/harness/update-server.sh's "next"
  [[ "$version" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)$ ]] || die "VERSION must be x.y.z, not '$version'"
  app_version="${BASH_REMATCH[1]}.${BASH_REMATCH[2]}.$(( BASH_REMATCH[3] + 1 ))"
fi
version="${app_version:-$version}"
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "the version must be x.y.z, not '$version'"
# the carried entries: the three files that go per file (R4.I83), each once, from a readable file
carried=()
for c in ${carry_files[@]+"${carry_files[@]}"}; do
  entry="${c%%=*}"; path="${c#*=}"
  [ "$entry" != "$c" ] && [ -n "$path" ] || die "--carry-file takes ENTRY=PATH, not '$c'"
  case "$entry" in "stub/x64/RB Stems Plus.exe"|"stub/arm64/RB Stems Plus.exe"|"admin/RB Stems Plus.exe") ;;
    *) die "--carry-file: '$entry' isn't carried per file (only the stubs and the worker, R4.I83)" ;; esac
  case " ${carried[*]+${carried[*]}} " in *" ${entry// /_} "*) die "--carry-file: $entry twice" ;; esac
  [ -f "$path" ] || die "--carry-file: no file $path"
  carried+=("${entry// /_}")
done
mkdir -p "$out"
out="$(cd "$out" && pwd)"
# the work folder and the hash list (RBSP_WIN_WORK and RBSP_WIN_SUMS: --check-repro's second build)
work="${RBSP_WIN_WORK:-$root/build/windows${test_base:+-test}}"
sums="${RBSP_WIN_SUMS:-$root/build/win${test_base:+-test}.sha256}"
# a carried file never lies in a folder a build empties (this one's, the other kind's, the repro's)
phys() { python3 -I -c 'import os, sys; print(os.path.realpath(sys.argv[1]))' "$1"; }   # every symlink, the last part's too
b="$(phys "$root/build/x" || printf '%s\n' "$root/build/x")"; b="${b%/x}"; w="$(phys "$work/x" || printf '%s\n' "$work/x")"; w="${w%/x}"
for c in ${carry_files[@]+"${carry_files[@]}"}; do
  p="$(phys "${c#*=}")" || die "--carry-file: can't resolve ${c#*=}"
  case "$p" in "$b"/windows/*|"$b"/windows-test/*|"$b"/windows-repro*/*|"$w"/*)
    die "--carry-file: ${c#*=} lies in a folder the build empties ($p): copy it elsewhere first" ;; esac
done
rm -rf "$work"; mkdir -p "$work"
wcc="$root/scripts/win-cc.sh"
sha256() { shasum -a 256 "$1" | cut -d' ' -f1; }
WARN="-Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wformat=2 -Wformat-nonliteral -Werror"
# imports of PE ($2, the allowlist as an ERE): KERNEL32 among them, nothing else
gate() {
  local imports bad
  imports="$("$wcc" --tool "$1" llvm-objdump -p "$2" | sed -n 's/^ *DLL Name: //p' | sort -u)"
  printf '%s\n' "$imports" | grep -qx 'KERNEL32.dll' || die "$2: no KERNEL32.dll in its imports"
  bad="$(printf '%s\n' "$imports" | grep -viE "^($3)\$" || true)"
  [ -z "$bad" ] || die "$2 imports $(echo $bad)"
}
# no threads or C++ runtime linked in statically (I52, release.md §3.3): a static library doesn't
# show in the import table, so the link map and the symbols refuse pthread_*, _Unwind_* and
# __cxa_*, and the map refuses libmfuuid.a (the helper's Media Foundation GUIDs are its own)
linkgate() {   # ARCH PE MAP
  local n syms
  [ -f "$3" ] || die "$2: no link map ($3)"
  n="$(grep -cE 'pthread_|_Unwind_|__cxa_' "$3" || true)"
  [ "$n" = 0 ] || die "$2's link map names pthread_/_Unwind_/__cxa_ $n times"
  if grep -q 'libmfuuid\.a' "$3"; then die "$2 links libmfuuid.a"; fi
  syms="$("$wcc" --tool "$1" llvm-nm "$2")" || die "$2: llvm-nm failed"
  n="$(printf '%s\n' "$syms" | grep -cE 'pthread_|_Unwind_|__cxa_' || true)"
  [ "$n" = 0 ] || die "$2 has $n pthread_/_Unwind_/__cxa_ symbols"
}

echo "== the shell and the core DLLs (win/shell/build.sh)"
# another version than VERSION's goes to the exe and the core DLLs (--stamp)
stamp=()
[ "$version" = "$(tr -d '[:space:]' < "$root/VERSION")" ] || stamp=(--app-version "$version")
if [ -n "$test_base" ]; then
  "$root/win/shell/build.sh" --no-test --test-app --test-base "$test_base" --test-key "$test_key" ${stamp[@]+"${stamp[@]}"} "$work/shell"
  app="$work/shell/app-test"
else
  # The standard C# shell shows Status and Prepare and runs the helper staged below.
  "$root/win/shell/build.sh" --no-test ${stamp[@]+"${stamp[@]}"} "$work/shell"
  app="$work/shell/app"
fi

# the core DLLs the app folder got (the TEST app's are the core's TEST build), with their link maps
coredir="$work/shell/core"
[ -z "$test_base" ] || coredir="$work/shell/core-test/testhooks"
for a in x64 arm64; do
  cmp -s "$coredir/$a/rbsp-core.dll" "$app/$a/rbsp-core.dll" || die "$app/$a/rbsp-core.dll isn't $coredir's"
  linkgate "$a" "$app/$a/rbsp-core.dll" "$coredir/$a/rbsp-core.dll.map"
done

echo "== the worker (win/admin/build.sh)"
"$root/win/admin/build.sh" "$work/admin" ${test_key:+--test-key "$test_key"}
linkgate x64 "$work/admin/rbsp-admin.exe" "$work/admin/rbsp-admin.exe.map"
if [ -n "$test_base" ]; then
  # and the release worker from the same commit: its hash (OUT/windows-release.sha256) is what
  # release.sh holds the release's worker to (TT1, BP1); the TEST zip has the TEST worker
  "$root/win/admin/build.sh" "$work/admin-release"
  linkgate x64 "$work/admin-release/rbsp-admin.exe" "$work/admin-release/rbsp-admin.exe.map"
fi

echo "== the bridge (bridge/win/build.sh: the release's, in TEST builds too)"
"$root/bridge/win/build.sh" "$work/bridge"
[ -f "$work/bridge/onnxruntime.dll" ] || die "bridge/win/build.sh made no onnxruntime.dll"
linkgate x64 "$work/bridge/onnxruntime.dll" "$work/bridge/onnxruntime.dll.map"

echo "== the prepare helper (helper/build-win.sh: x64, its own gates)"
# never a research helper (--keys) in a zip; build-win.sh's own gate refuses a release-flagged
# build with --keys or the timing lines
[ "${RBSP_RESEARCH:-}" != 1 ] || die "RBSP_RESEARCH=1 is set: a research helper never ships"
"$root/helper/build-win.sh" --version "$version" "$work/helper"
[ -f "$work/helper/rbsp-prepare.exe" ] || die "helper/build-win.sh made no rbsp-prepare.exe"
linkgate x64 "$work/helper/rbsp-prepare.exe" "$work/helper/rbsp-prepare.exe.map"

echo "== the start stubs (win/c/stub.c)"
for a in x64 arm64; do
  mkdir -p "$work/stub/$a"
  # The fixed resources include the icon shared with the WPF app (I83).
  cp "$root/win/c/stub.rc" "$root/win/c/stub.manifest" "$root/win/shell/RbStemsPlus.ico" "$work/stub/$a/"
  (cd "$work/stub/$a" && "$wcc" --tool "$a" windres stub.rc -O coff -o stub.res.o)
  # shellcheck disable=SC2086
  (cd "$root" && "$wcc" "$a" $WARN -Wno-unused-command-line-argument -municode -mwindows win/c/stub.c win/c/stub_rules.c \
     "$work/stub/$a/stub.res.o" -Wl,-Map="$work/stub/$a/stub.map" -o "$work/stub/$a/RB Stems Plus.exe")
  gate "$a" "$work/stub/$a/RB Stems Plus.exe" 'KERNEL32\.dll|USER32\.dll|api-ms-win-crt-[a-z0-9-]+\.dll'
  linkgate "$a" "$work/stub/$a/RB Stems Plus.exe" "$work/stub/$a/stub.map"
  python3 -I "$root/scripts/win-icon-check.py" "$work/stub/$a/RB Stems Plus.exe"
done

echo "== the watchers (win/c/watch_win.c, as win/c/tests/watch.sh builds the shipped ones)"
keysc="$work/watch_keys.c"
{
  echo "/* Written by scripts/build-windows.sh: the keys the watcher checks payload.json with. */"
  echo "#include \"watch.h\""
  echo "const sig_key_text watch_compiled_keys[] = {"
  if [ -n "$test_key" ]; then
    echo "    {\"test\", \"$pub\"},"
    echo "};"
    echo "const int watch_n_compiled_keys = 1;"
  else
    for name in release backup; do
      [ -f "$root/keys/$name.pub.pem" ] || die "keys/$name.pub.pem is missing"
      k="$(pubkey_b64 "$root/keys/$name.pub.pem")" || die "keys/$name.pub.pem isn't a P-256 public key"
      echo "    {\"$name\", \"$k\"},"
    done
    echo "};"
    echo "const int watch_n_compiled_keys = 2;"
  fi
} > "$keysc"
pf="$("$root/scripts/win-deps.sh" pocketfft)"
v1="${version%%.*}"; rest="${version#*.}"; v2="${rest%%.*}"; v3="${rest#*.}"
# The prepared playlist check shares the app's facts and helper process.
PREPARE="win/c/inventory.c win/c/receipt.c win/c/pe_code.c win/c/host_facts.c win/c/native_facts.c win/c/update.c win/c/update_rules.c win/c/unzip.c
  win/c/native_location.c win/c/native_config.c win/c/volumes.c win/c/volumes_win.c common/cfg_ini.c common/path_rules.c
  common/marker.c win/c/helper_proc.c win/c/helper_proc_win.c win/c/admin.c win/c/admin_proto.c"
CORESRC="$("$root/core/build.sh" srcs)"
for a in x64 arm64; do
  d="$work/watch-$a"; mkdir -p "$d"
  for s in $CORESRC; do
    # shellcheck disable=SC2086
    (cd "$root" && "$wcc" "$a" $WARN -Wno-unused-command-line-argument -DCORE_WIN_POLICY -Icore -c "core/$s" -o "$d/core-${s%.c}.o")
  done
  # shellcheck disable=SC2086
  (cd "$root/common" && "$wcc" "$a" $WARN -Wno-unused-command-line-argument -DPLAT_NO_BLOCKS -I"$pf" -c plat_win.c -o "$d/core-plat_win.o")
  "$wcc" "$a" -w -c -I"$pf" "$pf/pocketfft.c" -o "$d/core-pocketfft.o"
  "$wcc" --tool "$a" llvm-ar rcs "$d/libcore.a" "$d"/core-*.o
  printf '#define V1 %s\n#define V2 %s\n#define V3 %s\n#define VSTR "%s"\n' "$v1" "$v2" "$v3" "$version" > "$d/watch_version.h"
  cp "$root/win/c/watch.rc" "$root/win/c/watch.manifest" "$d/"
  (cd "$d" && "$wcc" --tool "$a" windres watch.rc -O coff -o watch.res.o)
  # shellcheck disable=SC2086
  (cd "$root" && "$wcc" "$a" $WARN -Wno-unused-command-line-argument -municode -mwindows -DCORE_WIN_POLICY "-DRBSP_VERSION=\"$version\"" -Icore -Icommon -Iwin/c \
     $PREPARE win/c/watch_win.c win/c/watch.c win/c/watch_facts.c win/c/dismissed.c win/c/compat.c win/c/compat_win.c win/c/payload.c win/c/payload_store.c win/c/fs_write.c \
     win/c/sig.c win/c/sig_bcrypt.c "$keysc" "$d/watch.res.o" "$d/libcore.a" \
     -lcomctl32 -lshell32 -lole32 -luuid -ladvapi32 -luser32 -lntdll -Wl,-Map="$d/rbsp-watch-$a.exe.map" -o "$d/rbsp-watch-$a.exe")
  sh "$root/win/c/tests/watch-map.sh" release "$d/rbsp-watch-$a.exe.map"
  gate "$a" "$d/rbsp-watch-$a.exe" 'KERNEL32\.dll|ADVAPI32\.dll|USER32\.dll|SHELL32\.dll|ole32\.dll|COMCTL32\.dll|ntdll\.dll|api-ms-win-crt-[a-z0-9-]+\.dll'
  linkgate "$a" "$d/rbsp-watch-$a.exe" "$d/rbsp-watch-$a.exe.map"
done

echo "== the zip"
stage="$work/zip"; rm -rf "$stage"; mkdir -p "$stage/x64" "$stage/arm64" "$stage/admin" "$stage/stub/x64" "$stage/stub/arm64"
cp "$app/RB Stems Plus.exe" "$app/RB Stems Plus.exe.config" "$stage/"
for a in x64 arm64; do
  cp "$app/$a/rbsp-core.dll" "$stage/$a/"
  cp "$work/watch-$a/rbsp-watch-$a.exe" "$stage/"
  cp "$work/stub/$a/RB Stems Plus.exe" "$stage/stub/$a/"
done
cp "$work/admin/rbsp-admin.exe" "$stage/admin/RB Stems Plus.exe"
cp "$work/helper/rbsp-prepare.exe" "$stage/"
cp "$root/NOTICE" "$root/LICENSE" "$stage/"
cp "$root/win/NOTICE.txt" "$stage/NOTICE-windows.txt"
# a carried stub or worker: the previous release's bytes in place of this build's
for c in ${carry_files[@]+"${carry_files[@]}"}; do
  entry="${c%%=*}"; path="${c#*=}"
  if cmp -s "$path" "$stage/$entry"; then echo "carried $entry (the same bytes as this build's)"
  else echo "carried $entry from $path (this build's differs)"; fi
  cp "$path" "$stage/$entry"
done
# the exact contents (the fixture; R4.I82)
(cd "$stage" && find . -type f | sed 's|^\./||' | LC_ALL=C sort) > "$work/zip-contents.txt"
diff -u "$root/win/zip-contents.txt" "$work/zip-contents.txt" || die "the zip's contents aren't win/zip-contents.txt's"
# every staged native PE, carried or built, gated again as it goes in the zip (release.md §3.3):
# its machine, its imports (static and delay-load), and the worker's marker, manifest and keys
# (the AnyCPU shell's machine is win/shell/build.sh's pe_machine check)
has() {   # FILE TEXT [utf16]: the bytes hold TEXT (as UTF-16LE with utf16)
  python3 -I -c 'import sys; t = sys.argv[2].encode("utf-16-le" if sys.argv[3] else "ascii"); sys.exit(0 if t in open(sys.argv[1], "rb").read() else 1)' "$1" "$2" "${3:-}"
}
for f in "admin/RB Stems Plus.exe" "stub/x64/RB Stems Plus.exe" x64/rbsp-core.dll rbsp-watch-x64.exe rbsp-prepare.exe \
         "stub/arm64/RB Stems Plus.exe" arm64/rbsp-core.dll rbsp-watch-arm64.exe; do
  case "$f" in *arm64*) a=arm64 m=coff-arm64 ;; *) a=x64 m=coff-x86-64 ;; esac
  fmt="$("$wcc" --tool "$a" llvm-objdump -f "$stage/$f")" || die "$f: llvm-objdump failed"
  printf '%s\n' "$fmt" | grep -q "file format $m\$" || die "$f isn't $m: $(printf '%s\n' "$fmt" | sed -n 's/.*file format //p')"
done
for a in x64 arm64; do
  gate "$a" "$stage/stub/$a/RB Stems Plus.exe" 'KERNEL32\.dll|USER32\.dll|api-ms-win-crt-[a-z0-9-]+\.dll'
done
worker="$stage/admin/RB Stems Plus.exe"
# the worker's imports as win/admin/tests/run.sh reads them (the import and delay-load tables):
# KERNEL32, ADVAPI32, ntdll and api-ms-win-crt-* (app.md §5.5's KnownDLLs rule)
wi="$("$wcc" --tool x64 llvm-readobj --coff-imports "$worker")" || die "the worker: llvm-readobj failed"
wi="$(printf '%s\n' "$wi" | sed -n 's/^ *Name: \(.*\.dll\)$/\1/p' | sort -u)"
printf '%s\n' "$wi" | grep -qx 'KERNEL32.dll' || die "the worker has no KERNEL32.dll in its imports"
bad="$(printf '%s\n' "$wi" | grep -viE '^(KERNEL32|ADVAPI32|ntdll)\.dll$|^api-ms-win-crt-[a-z0-9-]+\.dll$' || true)"
[ -z "$bad" ] || die "the worker imports $(echo $bad)"
has "$worker" 'rbsp-admin worker: proto=2 minimum=' || die "the worker has no rbsp-admin marker"
has "$worker" requireAdministrator || die "the worker has no requireAdministrator manifest"
if [ -n "$test_key" ]; then
  has "$worker" RBSP_TEST_PROGRAMS utf16 || die "the TEST worker can't move its root (no RBSP_TEST_PROGRAMS)"
  has "$worker" RBSP_TEST_WORKER_HOLD utf16 || die "the TEST worker can't be held"
  has "$worker" RBSP_TEST_WORKER_MIDHOLD utf16 || die "the TEST worker can't be held mid-work"
  has "$worker" RBSP_TEST_WORKER_MIDHOLD_TARGET || die "the TEST worker can't hold one chosen swap"
  has "$worker" RBSP_TEST_WORKER_FAIL_TARGET || die "the TEST worker can't fail one chosen version"
  has "$worker" "$pub" || die "the TEST worker doesn't trust $test_key"
else
  if has "$worker" RBSP_TEST_PROGRAMS utf16; then die "the release worker reads RBSP_TEST_PROGRAMS (a TEST build's)"; fi
  if has "$worker" RBSP_TEST_WORKER_HOLD utf16; then die "the release worker reads RBSP_TEST_WORKER_HOLD (a TEST build's)"; fi
  if has "$worker" RBSP_TEST_WORKER_MIDHOLD utf16; then die "the release worker reads RBSP_TEST_WORKER_MIDHOLD (a TEST build's)"; fi
  # the two target names are read as ANSI (GetEnvironmentVariableA): their ASCII bytes
  if has "$worker" RBSP_TEST_WORKER_MIDHOLD_TARGET; then die "the release worker reads RBSP_TEST_WORKER_MIDHOLD_TARGET (a TEST build's)"; fi
  if has "$worker" RBSP_TEST_WORKER_FAIL_TARGET; then die "the release worker reads RBSP_TEST_WORKER_FAIL_TARGET (a TEST build's)"; fi
  for name in release backup; do
    k="$(pubkey_b64 "$root/keys/$name.pub.pem")" || die "keys/$name.pub.pem isn't a P-256 public key"
    has "$worker" "$k" || die "the release worker doesn't trust keys/$name.pub.pem"
  done
fi
# a TEST build says it in its exe; a release names neither the TEST hooks nor a test key
exe="$stage/RB Stems Plus.exe"
# grep -c reads the whole input (grep -q would stop tr early: a SIGPIPE pipefail counts as a failure)
names() { LC_ALL=C tr -d '\000' < "$1" | LC_ALL=C grep -a -c -E "$2" > /dev/null; }
if [ -n "$test_base" ]; then
  names "$exe" RBSP_SHELL_TEST || die "the TEST exe isn't a TEST build"
  names "$exe" RbspTestUpdateBase || die "the TEST exe has no download base"
elif names "$exe" 'RBSP_SHELL_TEST|RbspTestUpdateBase|core_test_update_base'; then
  die "the release exe carries TEST hooks"
fi
# the version resources (release.md §3.2): the shell, core DLLs, watchers and prepare helper stamped with
# the version; the worker and the stubs a constant one (no version, I83); the bridge none
vres() { python3 -I "$root/scripts/win-version.py" "$1"; }
for f in "RB Stems Plus.exe" x64/rbsp-core.dll arm64/rbsp-core.dll rbsp-watch-x64.exe rbsp-watch-arm64.exe rbsp-prepare.exe; do
  vr="$(vres "$stage/$f")"
  for want in "fixed.file=$version.0" "fixed.product=$version.0" "string.ProductVersion=$version" "string.ProductName=RB Stems Plus"; do
    printf '%s\n' "$vr" | grep -qxF "$want" || die "$f's version resource has no $want"
  done
done
for f in "admin/RB Stems Plus.exe" "stub/x64/RB Stems Plus.exe" "stub/arm64/RB Stems Plus.exe"; do
  vr="$(vres "$stage/$f")"
  printf '%s\n' "$vr" | grep -qxF "fixed.file=0.0.0.0" && printf '%s\n' "$vr" | grep -qxF "string.ProductName=RB Stems Plus" \
    || die "$f has no constant version resource"
  if printf '%s\n' "$vr" | grep -q '^string\.\(ProductVersion\|FileVersion\)='; then die "$f's version resource names a version (I83)"; fi
done
[ "$(vres "$work/bridge/onnxruntime.dll")" = none ] || die "the bridge has a version resource"
rm -f "$out/rbstemsplus-windows.zip"
SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "$root" log -1 --format=%ct 2>/dev/null || echo 315532800)}" \
  python3 -I - "$stage" "$out/rbstemsplus-windows.zip" <<'PY'
import os, sys, time, zipfile
src, dst = sys.argv[1], sys.argv[2]
t = time.gmtime(max(315532800, int(os.environ["SOURCE_DATE_EPOCH"])))[:6]
names = sorted(os.path.relpath(os.path.join(d, f), src).replace(os.sep, "/") for d, _, fs in os.walk(src) for f in fs)
with zipfile.ZipFile(dst + ".part", "w") as z:
    for n in names:
        i = zipfile.ZipInfo(n, t)
        i.compress_type = zipfile.ZIP_DEFLATED
        i.external_attr = 0o644 << 16
        z.writestr(i, open(os.path.join(src, n), "rb").read())
os.replace(dst + ".part", dst)
PY
cp "$work/bridge/onnxruntime.dll" "$out/rbstemsplus-bridge-win-x64.dll"
rm -f "$out/windows-release.sha256"
[ -z "$test_base" ] || printf 'admin %s\n' "$(sha256 "$work/admin-release/rbsp-admin.exe")" > "$out/windows-release.sha256"
{
  echo "win_app_version=$version"
  echo "win_zip_sha256=$(sha256 "$out/rbstemsplus-windows.zip")"
  echo "win_admin_sha256=$(sha256 "$stage/admin/RB Stems Plus.exe")"
  echo "win_stub_x64_sha256=$(sha256 "$stage/stub/x64/RB Stems Plus.exe")"
  echo "win_stub_arm64_sha256=$(sha256 "$stage/stub/arm64/RB Stems Plus.exe")"
  echo "win_bridge_sha256=$(sha256 "$out/rbstemsplus-bridge-win-x64.dll")"
} > "$out/windows.env"
# every shipped file's SHA-256 (release.md §3.3): the zip's files, the zip and the bridge
{
  (cd "$stage" && find . -type f | sed 's|^\./||' | LC_ALL=C sort | while IFS= read -r f; do printf '%s  zip/%s\n' "$(sha256 "$f")" "$f"; done)
  printf '%s  %s\n' "$(sha256 "$out/rbstemsplus-windows.zip")" rbstemsplus-windows.zip
  printf '%s  %s\n' "$(sha256 "$out/rbstemsplus-bridge-win-x64.dll")" rbstemsplus-bridge-win-x64.dll
  [ -z "$test_base" ] || printf '%s  %s\n' "$(sha256 "$work/admin-release/rbsp-admin.exe")" "release/admin/RB Stems Plus.exe"
} > "$sums"
if [ -n "$check_repro" ]; then
  # the same build again from scratch in another folder: every file the same bytes
  echo "== --check-repro: the same build again, in another folder"
  r="$root/build/windows-repro${test_base:+-test}"
  rm -rf "$r"; mkdir -p "$r/out"
  again=()
  [ -z "$test_base" ] || again+=(--test "$test_base" --test-key "$test_key")
  [ -z "$app_version" ] || again+=(--stamp "$version")
  for c in ${carry_files[@]+"${carry_files[@]}"}; do again+=(--carry-file "$c"); done
  RBSP_WIN_WORK="$r/work" RBSP_WIN_SUMS="$r/win.sha256" "$0" ${again[@]+"${again[@]}"} "$r/out" > "$r/build.log" 2>&1 \
    || { tail -30 "$r/build.log" >&2; die "--check-repro: the second build failed ($r/build.log)"; }
  diff -u "$sums" "$r/win.sha256" || die "--check-repro: the second build's files differ (above)"
  diff -r "$stage" "$r/work/zip" > /dev/null || die "--check-repro: the zip folders differ"
  for f in rbstemsplus-windows.zip rbstemsplus-bridge-win-x64.dll windows.env ${test_base:+windows-release.sha256}; do
    cmp -s "$out/$f" "$r/out/$f" || die "--check-repro: $f differs"
  done
  echo "--check-repro: two builds, the same bytes ($(wc -l < "$sums" | tr -d ' ') files, $sums)"
  rm -rf "$r"
fi
echo "built the Windows files $version into $out${test_base:+ (TEST BUILD, downloads from $test_base, trusts $test_key alone)}"

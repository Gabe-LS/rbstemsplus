#!/bin/sh
# win/c's tests on the Mac (app.md §7: no VM), with the core's warnings (C11, -Wpedantic, -Werror):
#  - the Mac-native tests (main.c and the *_test.c files): sig.c, payload.c, payload_store.c and
#    compat.c, built for this Mac against the test crypto shim (sig_mac.c, Security.framework),
#    plat_mac.c and a fake compat_os, plain (-O2) and with ASan+UBSan; compat.c's PE reader on
#    small exes the pinned toolchain builds here (tests/pe/: rekordbox.exe's and the ORT's
#    version resources, and none), x64 and arm64. They read the shared fixtures from today's
#    Swift (gen-fixtures.sh), made again when app/Sources or the generator changed. The updater
#    (update.c, unzip.c, update_rules.c, stub_rules.c) runs on a fake network and a fake OS, on an
#    app zip and the app's PE fixture (tests/pe/app.rc), and unzip.c on zips made by Python's
#    zipfile, Info-ZIP's zip and ditto (gen-zips.py); the plain build once more with its scratch on
#    a case-sensitive APFS image (hdiutil, -nobrowse; detached after its pass, and again in the
#    EXIT trap if that failed), for REMOVE_LOCATION's
#    cases that only run there;
#  - the fuzz driver (fuzz.c) on the DER, key, manifest, PE, zip, inflate and update record
#    parsers, with ASan+UBSan;
#  - every win/c source compiled for Windows x64 and arm64 with the pinned llvm-mingw and the
#    release flags (scripts/win-cc.sh), CORE_WIN_POLICY as the core DLL builds;
#  - sig.c, sig_bcrypt.c, payload.c and the updater's and stub's pure rules linked into a Windows
#    x64 and arm64 test exe (wintest.c, no plat_win.c yet), and its import table checked: KERNEL32
#    and the UCRT's api-ms-win-crt-* only; bcrypt.dll is never imported (it is loaded from System32
#    at run time);
#  - compat_win.c linked into a gate exe per architecture (tests/gate_stub.c, no -l flags): its
#    imports KERNEL32, ADVAPI32 and api-ms-win-crt-* only (wintrust and crypt32 are loaded at run
#    time); update_win.c with plat_win.c (tests/gate_update.c): the core DLL's allowlist, KERNEL32,
#    ADVAPI32, USER32, ntdll, SHELL32, ole32 and the UCRT
#    (winhttp.dll is loaded at run time); the start stub (stub.c, -municode -mwindows): KERNEL32,
#    USER32 and the UCRT only (app.md §7, J21). An import list without KERNEL32.dll (llvm-objdump
#    failing) fails the gate;
#  - no win/c object refers to bcrypt's, wintrust's, crypt32's, version.dll's or winhttp.dll's
#    functions at all, imported or not (llvm-nm -u, its own failure a failure);
#  - the app's own platform calls (W6a): app.sh.
#
#   win/c/tests/run.sh [OUT]       builds into OUT (default build/winc); exits 1 on a failure
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${1:-$root/build/winc}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
STD="-std=c11"
WARN="-Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wformat=2 -Wformat-nonliteral -Werror"
MAC="-mmacosx-version-min=14.0"   # test binaries only: the shim's RFC 4754 algorithm is macOS 14+
scratch="$(mktemp -d "${TMPDIR:-/tmp}/rbsp-winc.XXXXXX")"
cs_attached=0   # the case-sensitive image below: detached before the scratch goes
# set -e can't cut this short (review CS1): a detach that fails keeps the scratch, said
cleanup() {
  if [ "$cs_attached" = 1 ] && ! hdiutil detach -quiet -force "$scratch/cs" > /dev/null 2>&1; then
    echo "run.sh: couldn't detach $scratch/cs; kept $scratch (cs.dmg inside)" >&2
    return
  fi
  chmod -R u+w "$scratch" 2>/dev/null || :
  rm -rf "${scratch:?}"
}
trap cleanup EXIT
fails=0
cd "$root"   # relative names from here on: the repository's path has spaces

# the shared fixtures, again when the Swift they come from is newer, or when the last ones were
# partial (Swift couldn't write config.ini in a sandbox: review S44)
fx="$out/fixtures.txt"
if [ ! -f "$fx" ] || [ -f "$out/fixtures.partial" ] \
   || [ -n "$(find app/Sources win/c/tests/fixtures.swift win/c/tests/gen-fixtures.sh VERSION -newer "$fx" 2>/dev/null | head -1)" ]; then
  win/c/tests/gen-fixtures.sh "$out" > "$out/gen-fixtures.log" 2>&1 || { tail -20 "$out/gen-fixtures.log"; echo "FAIL the fixtures from today's Swift"; exit 1; }
fi

# the PE fixtures: a tiny exe with rekordbox.exe's version resource, the ORT's, and none
WCC="scripts/win-cc.sh"
pe="$out/pe"
mkdir -p "$pe"
for a in x64 arm64; do
  for rc in rekordbox ort app notapp; do
    "$WCC" --tool "$a" windres "win/c/tests/pe/$rc.rc" -O coff -o "$pe/$rc-$a.res.o" > "$out/pe.log" 2>&1 \
      && "$WCC" "$a" -Wno-unused-command-line-argument win/c/tests/pe/tiny.c "$pe/$rc-$a.res.o" -o "$pe/$rc-$a.exe" >> "$out/pe.log" 2>&1 \
      || { cat "$out/pe.log"; echo "FAIL the PE fixture $rc ($a)"; exit 1; }
  done
  "$WCC" "$a" -Wno-unused-command-line-argument win/c/tests/pe/tiny.c -o "$pe/plain-$a.exe" > "$out/pe.log" 2>&1 \
    || { cat "$out/pe.log"; echo "FAIL the PE fixture plain ($a)"; exit 1; }
done

# the zips from other tools for unzip.c (Python's zipfile, Info-ZIP's zip, ditto), again when the
# generator changed
zips="$out/zips"
if [ ! -f "$zips/raw.deflate" ] || [ win/c/tests/gen-zips.py -nt "$zips/raw.deflate" ]; then
  python3 -I win/c/tests/gen-zips.py "$zips" > "$out/gen-zips.log" 2>&1 || { cat "$out/gen-zips.log"; echo "FAIL the zip fixtures"; exit 1; }
fi

# the release zip's names, which unzip_test.c runs through unzip_name_ok (review BM2)
export RBSP_ZIP_CONTENTS="$root/win/zip-contents.txt"

# models.c's built-in model is Paths.swift's ourModel
ours_swift="$(sed -n 's/^let ourModel = "\([0-9a-f]*\)"$/\1/p' app/Sources/Paths.swift)"
ours_c="$(sed -n 's/^#define MODELS_OURS_BUILTIN "\([0-9a-f]*\)"$/\1/p' win/c/models.h)"
if [ -z "$ours_swift" ] || [ "$ours_swift" != "$ours_c" ]; then echo "FAIL models.h's MODELS_OURS_BUILTIN isn't Paths.swift's ourModel"; fails=$((fails + 1)); fi

# the pure modules and their tests; the core's JSON reader and arena as the core builds them, and its
# busy marker's rules (state.c, with text.c and helper_lines.c: native_leftover's)
WINC="win/c/sig.c win/c/payload.c win/c/update_rules.c win/c/stub_rules.c"
STORE="win/c/payload_store.c win/c/fs_write.c win/c/compat.c win/c/unzip.c win/c/update.c win/c/sha_memo.c win/c/native_facts.c win/c/native_steps.c win/c/native_config.c win/c/models.c win/c/shell.c common/marker.c win/c/volumes.c win/c/afterexit.c win/c/host_facts.c win/c/native_location.c common/cfg_ini.c common/path_rules.c common/plat_mac.c"
CORE="core/json.c core/arena.c core/state.c core/text.c core/helper_lines.c core/words.c"
TESTS="win/c/tests/main.c win/c/tests/fixtures_test.c win/c/tests/sig_test.c win/c/tests/payload_test.c win/c/tests/store_test.c win/c/tests/compat_test.c
  win/c/tests/update_rules_test.c win/c/tests/unzip_test.c win/c/tests/update_test.c win/c/tests/sha_memo_test.c win/c/tests/native_facts_test.c win/c/tests/native_steps_test.c win/c/tests/models_test.c win/c/tests/models_fixtures_test.c win/c/tests/afterexit_test.c win/c/tests/host_facts_test.c win/c/tests/native_location_test.c win/c/tests/sig_mac.c"
FW="-framework Security -framework CoreFoundation -framework Accelerate"
for v in plain asan; do
  case "$v" in
    plain) san="-O2" ;;
    asan) san="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined" ;;
  esac
  # shellcheck disable=SC2086
  clang $STD $WARN $MAC $san -DCOMPAT_TEST_HOOKS -DUPDATE_TEST_HOOKS -DNATIVE_STEPS_TEST_HOOKS -DMODELS_TEST_HOOKS -DNATIVE_LOCATION_TEST_HOOKS -Icore -Icommon $WINC $STORE $CORE $TESTS $FW -o "$out/winc-test-$v"
  d="$scratch/$v"; mkdir -p "$d"
  if "$out/winc-test-$v" "$fx" "$d" "$pe" "$zips" > "$out/winc-test-$v.log" 2>&1; then echo "ok   win/c tests ($v): $(tail -1 "$out/winc-test-$v.log")"
  else tail -30 "$out/winc-test-$v.log"; echo "FAIL win/c tests ($v)"; fails=$((fails + 1)); fi
  # fs_write.c built again with its flush replaced by a stub that can fail (tests/fs_write_fault.c,
  # review AW3): the error comes back, the old file stays, no temp file is left. Only fs_write.c
  # takes the -D: plat_mac.c's real plat_fsync stays what the stub calls
  # shellcheck disable=SC2086
  if clang $STD $WARN $MAC $san -Dplat_fsync=fs_test_fsync -Icore -Icommon -c win/c/fs_write.c -o "$out/fs_write_fault-$v.o" > "$out/fs-write-fault-$v.log" 2>&1 \
     && clang $STD $WARN $MAC $san -Icore -Icommon win/c/tests/fs_write_fault.c common/plat_mac.c "$out/fs_write_fault-$v.o" $FW -o "$out/fs-write-fault-$v" >> "$out/fs-write-fault-$v.log" 2>&1 \
     && "$out/fs-write-fault-$v" "$d" >> "$out/fs-write-fault-$v.log" 2>&1; then
    echo "ok   fs_write.c's failed flush ($v): $(tail -1 "$out/fs-write-fault-$v.log")"
  else tail -20 "$out/fs-write-fault-$v.log"; echo "FAIL fs_write.c's failed flush ($v)"; fails=$((fails + 1)); fi
  # the mapped view's flush failing (tests/flush_fault.c, review WF6): fs_write.c, models.c and
  # shell.c with plat_map_flush renamed to the test's stub (plat.h declares it for this build)
  # shellcheck disable=SC2086
  if clang $STD $WARN $MAC $san -Dplat_map_flush=test_map_flush -Icore -Icommon win/c/tests/flush_fault.c win/c/fs_write.c win/c/models.c win/c/shell.c \
       common/plat_mac.c $FW -o "$out/flush-fault-$v" > "$out/flush-fault-$v.log" 2>&1 \
     && "$out/flush-fault-$v" "$d" >> "$out/flush-fault-$v.log" 2>&1; then
    echo "ok   a mapped view's failed flush ($v): $(tail -1 "$out/flush-fault-$v.log")"
  else tail -20 "$out/flush-fault-$v.log"; echo "FAIL a mapped view's failed flush ($v)"; fails=$((fails + 1)); fi
done

# the plain tests once more with their scratch on a case-sensitive APFS image (review RF2's Fix B):
# REMOVE_LOCATION's case-sensitive cases (MS6's other file in another case, MT2 and RF1's hard
# links there) run only on such a volume. hdiutil is the OS's; the image is detached after its
# pass (a detach that fails is a failure), and again in the EXIT trap if that failed. An image
# that can't be made or attached is a failure, not a skip.
cs_mnt="$scratch/cs"
mkdir -p "$cs_mnt"
if hdiutil create -quiet -size 256m -fs "Case-sensitive APFS" -volname rbsp-winc-cs "$scratch/cs.dmg" > "$out/winc-test-cs.log" 2>&1 \
   && hdiutil attach -quiet -nobrowse -mountpoint "$cs_mnt" "$scratch/cs.dmg" >> "$out/winc-test-cs.log" 2>&1; then
  cs_attached=1
  mkdir -p "$cs_mnt/s"
  if "$out/winc-test-plain" "$fx" "$cs_mnt/s" "$pe" "$zips" > "$out/winc-test-cs.log" 2>&1 && ! grep -q "scratch drive ignores case" "$out/winc-test-cs.log"; then
    echo "ok   win/c tests (plain, case-sensitive APFS): $(tail -1 "$out/winc-test-cs.log")"
  else tail -30 "$out/winc-test-cs.log"; echo "FAIL win/c tests (plain, case-sensitive APFS)"; fails=$((fails + 1)); fi
  if hdiutil detach -quiet "$cs_mnt" >> "$out/winc-test-cs.log" 2>&1 \
     || hdiutil detach -quiet -force "$cs_mnt" >> "$out/winc-test-cs.log" 2>&1; then cs_attached=0
  else echo "FAIL the case-sensitive image didn't detach: $cs_mnt ($scratch/cs.dmg)"; fails=$((fails + 1)); fi
else
  cat "$out/winc-test-cs.log"; echo "FAIL win/c tests (plain, case-sensitive APFS): the image couldn't be made or attached (hdiutil; outside a sandbox)"; fails=$((fails + 1))
fi

# every source for Windows, both architectures, as the core DLL builds (CORE_WIN_POLICY); none
# refers to bcrypt's, wintrust's, crypt32's or version.dll's functions (they are loaded at run time,
# or not used: the version resource is read as data)
for a in x64 arm64; do
  mkdir -p "$out/win-$a"
  for s in win/c/*.c; do
    o="$out/win-$a/$(basename "$s" .c).o"
    # shellcheck disable=SC2086
    if ! "$WCC" "$a" $WARN -Wno-unused-command-line-argument -DCORE_WIN_POLICY -Icore -Icommon -c "$s" -o "$o" > "$out/win-$a.log" 2>&1; then
      cat "$out/win-$a.log"; echo "FAIL $s for Windows $a"; fails=$((fails + 1))
    elif ! "$WCC" --tool "$a" llvm-nm -u "$o" > "$o.nm" 2>&1; then
      cat "$o.nm"; echo "FAIL llvm-nm on $s ($a)"; fails=$((fails + 1))
    elif grep -E '(__imp_)?(BCrypt|WinVerifyTrust|WTHelper|Cert[A-Z]|Crypt[A-Z]|GetFileVersionInfo|VerQuery|WinHttp)' "$o.nm"; then
      echo "FAIL $s for Windows $a refers to a crypto, version.dll or winhttp.dll function"; fails=$((fails + 1))
    # shellcheck disable=SC2086
    elif ! "$WCC" "$a" $WARN -Wno-unused-command-line-argument -DCORE_WIN_POLICY -DRBSP_CORE_PLAT_SHIM_H -include common/plat.h \
         -Icore -Icommon -c "$s" -o /dev/null > "$out/win-$a.log" 2>&1; then
      # the core on common/plat.h instead of its shim (common/tests/run.sh's WINCORE build): the same
      cat "$out/win-$a.log"; echo "FAIL $s for Windows $a on plat.h"; fails=$((fails + 1))
    fi
  done
done
[ "$fails" = 0 ] && echo "ok   win/c compiles for Windows (x64, arm64; the core on its shim and on plat.h)"

# the fuzz driver, ASan+UBSan
# shellcheck disable=SC2086
clang $STD $WARN $MAC -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined -Icore \
  -Icommon $WINC $CORE win/c/compat.c win/c/unzip.c common/plat_mac.c win/c/tests/fuzz.c win/c/tests/sig_mac.c $FW -o "$out/winc-fuzz"
if "$out/winc-fuzz" "${FUZZ_ITERATIONS:-20000}" 0 "$pe/rekordbox-x64.exe" "$zips" > "$out/winc-fuzz.log" 2>&1; then echo "ok   win/c fuzz: $(sed -n 's/^fuzz: //p' "$out/winc-fuzz.log" | tr '\n' ';')"
else tail -20 "$out/winc-fuzz.log"; echo "FAIL win/c fuzz"; fails=$((fails + 1)); fi

# import gate: ARCH EXE ALLOWED (an extended regex of DLL names); KERNEL32.dll must be among them
# (an empty list is llvm-objdump failing, not a clean exe)
gate() {
  imports="$("$WCC" --tool "$1" llvm-objdump -p "$2" | sed -n 's/^ *DLL Name: //p' | sort -u | tr '\n' ' ')"
  # shellcheck disable=SC2086
  bad="$(printf '%s\n' $imports | grep -viE "^($3)\$" || true)"
  if ! printf '%s\n' $imports | grep -qx 'KERNEL32.dll'; then echo "FAIL $(basename "$2") ($1): no KERNEL32.dll in its imports: $imports"; fails=$((fails + 1))
  elif [ -n "$bad" ]; then echo "FAIL $(basename "$2") ($1) imports $bad"; fails=$((fails + 1))
  else echo "ok   $(basename "$2") ($1) imports: $imports"; fi
}

# the Windows test exe: the shared fixtures with the real BCrypt (run in the VM later); its
# imports: KERNEL32 and api-ms-win-crt-* only, never bcrypt (loaded from System32 at run time).
# compat_win.c's gate exe: KERNEL32, ADVAPI32 and api-ms-win-crt-* (wintrust and crypt32 are
# loaded from System32 at run time).
for a in x64 arm64; do
  exe="$out/win-$a/winc-wintest.exe"
  # shellcheck disable=SC2086
  if "$WCC" "$a" $WARN -Wno-unused-command-line-argument -DCORE_WIN_POLICY -Icore -Icommon \
       $WINC win/c/sig_bcrypt.c $CORE win/c/native_config.c win/c/afterexit_self_win.c win/c/tests/wintest.c win/c/tests/fixtures_test.c \
       win/c/tests/update_rules_test.c win/c/tests/afterexit_wintest.c -o "$exe" > "$out/wintest-$a.log" 2>&1; then
    gate "$a" "$exe" 'KERNEL32\.dll|api-ms-win-crt-[a-z0-9-]+\.dll'
  else
    cat "$out/wintest-$a.log"; echo "FAIL winc-wintest.exe ($a) doesn't build"; fails=$((fails + 1))
  fi
  exe="$out/win-$a/winc-compat-gate.exe"
  # shellcheck disable=SC2086
  if "$WCC" "$a" $WARN -Wno-unused-command-line-argument -DCORE_WIN_POLICY -Icore -Icommon \
       win/c/compat_win.c win/c/tests/gate_stub.c -o "$exe" > "$out/gate-$a.log" 2>&1; then
    gate "$a" "$exe" 'KERNEL32\.dll|ADVAPI32\.dll|api-ms-win-crt-[a-z0-9-]+\.dll'
  else
    cat "$out/gate-$a.log"; echo "FAIL winc-compat-gate.exe ($a) doesn't build"; fails=$((fails + 1))
  fi
  # update_win.c with plat_win.c (its tree's deletion, T3b) as the core DLL links them: the core
  # DLL's allowlist (app.md §7, I81), plat_win.c's own imports (ntdll, SHELL32, ole32) included
  exe="$out/win-$a/winc-update-gate.exe"
  pf="$(scripts/win-deps.sh pocketfft)"
  # shellcheck disable=SC2086
  if "$WCC" "$a" -w -c -I"$pf" "$pf/pocketfft.c" -o "$out/win-$a/pocketfft.o" > "$out/gate-update-$a.log" 2>&1 \
     && "$WCC" "$a" $WARN -Wno-unused-command-line-argument -DCORE_WIN_POLICY -DPLAT_NO_BLOCKS -Icore -Icommon -I"$pf" \
       win/c/update_win.c win/c/update_rules.c win/c/payload.c win/c/sig.c win/c/sig_bcrypt.c common/plat_win.c $CORE \
       win/c/tests/gate_update.c "$out/win-$a/pocketfft.o" -lntdll -lshell32 -lole32 -o "$exe" >> "$out/gate-update-$a.log" 2>&1; then
    gate "$a" "$exe" 'KERNEL32\.dll|ADVAPI32\.dll|USER32\.dll|ntdll\.dll|SHELL32\.dll|ole32\.dll|api-ms-win-crt-[a-z0-9-]+\.dll'
  else
    cat "$out/gate-update-$a.log"; echo "FAIL winc-update-gate.exe ($a) doesn't build"; fails=$((fails + 1))
  fi
  # the start stub as it ships: its own program, the GUI subsystem, the narrowest allowlist (J21)
  exe="$out/win-$a/RB Stems Plus.exe"
  # shellcheck disable=SC2086
  if "$WCC" "$a" $WARN -Wno-unused-command-line-argument -municode -mwindows win/c/stub.c win/c/stub_rules.c -o "$exe" \
       > "$out/stub-$a.log" 2>&1; then
    gate "$a" "$exe" 'KERNEL32\.dll|USER32\.dll|api-ms-win-crt-[a-z0-9-]+\.dll'
    echo "     the stub ($a): $(wc -c < "$exe" | tr -d ' ') bytes"
  else
    cat "$out/stub-$a.log"; echo "FAIL the stub ($a) doesn't build"; fails=$((fails + 1))
  fi
done

# the watcher (watch.sh): its decisions on the Mac, rbsp-watch.exe for both architectures and its gate
sh win/c/tests/watch.sh "$out" "$pe" || fails=$((fails + 1))

# the admin worker and the app's admin side (W4, W5): their own script
win/admin/tests/run.sh "$out/admin" "$pe" || fails=$((fails + 1))

# the app's own platform calls (W6a: volumes, lifecycle, report, CriticalWork): their own script
sh win/c/tests/app.sh "$out" || fails=$((fails + 1))

notes="$(grep -h "^note:" "$out"/winc-test-plain.log 2>/dev/null | sort -u | tr "\n" " ")"
[ "$fails" = 0 ] && echo "win/c: all passed${notes:+ ($notes)}" || { echo "win/c: $fails failed"; exit 1; }

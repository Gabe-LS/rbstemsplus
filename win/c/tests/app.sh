#!/bin/sh
# The app's own platform calls (W6a: volumes.c, lifecycle.c, report.c, report_build.c, shell.c;
# app.md §5.8-5.10, §7); run.sh calls it, with run.sh's warnings:
#  - tests/app_test.c on the Mac: the rules with fakes, the size walk and the WER walk on a scratch
#    tree through plat_mac.c, Create Report's whole zip on a scratch tree (read back with
#    unzip.c), CriticalWork's threads, a seeded fuzz of the text parsers; plain (-O2) and with
#    ASan+UBSan;
#  - winc-app-wintest.exe per architecture (tests/app_wintest.c with the *_win.c files, plat_win.c
#    and pocketfft), the VM's run of every Windows call here, and its imports within the core
#    DLL's allowlist: KERNEL32, ADVAPI32, USER32, SHELL32, ole32, ntdll and api-ms-win-crt-* only
#    (app.md §7, I81; cfgmgr32 and wevtapi are loaded from System32 at run time, never imported).
#
#   win/c/tests/app.sh OUT     OUT: run.sh's build folder. Exits 1 on a failure.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="$(cd "$1" && pwd)"
STD="-std=c11"
WARN="-Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wformat=2 -Wformat-nonliteral -Werror"
MAC="-mmacosx-version-min=14.0"
WCC="scripts/win-cc.sh"
FW="-framework CoreFoundation -framework Accelerate"
fails=0
cd "$root"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/rbsp-app.XXXXXX")"
trap 'chmod -R u+w "$scratch" 2>/dev/null; rm -rf "${scratch:?}"' EXIT

APP="win/c/volumes.c win/c/lifecycle.c win/c/report.c win/c/report_build.c win/c/fs_write.c win/c/unzip.c win/c/shell.c core/mask.c core/arena.c"
APPWIN="win/c/volumes_win.c win/c/lifecycle_win.c win/c/osversion_win.c win/c/report_win.c"
for v in plain asan; do
  case "$v" in
    plain) san="-O2" ;;
    asan) san="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined" ;;
  esac
  # shellcheck disable=SC2086
  if clang $STD $WARN $MAC $san -Icore -Icommon $APP common/plat_mac.c win/c/tests/app_test.c $FW -o "$out/winc-app-test-$v" \
       > "$out/app-build-$v.log" 2>&1; then
    d="$scratch/$v"; mkdir -p "$d"
    if "$out/winc-app-test-$v" "$d" "${FUZZ_ITERATIONS:-20000}" "$root/win/c" > "$out/winc-app-test-$v.log" 2>&1; then
      echo "ok   the app's platform calls ($v): $(tail -1 "$out/winc-app-test-$v.log")"
    else tail -30 "$out/winc-app-test-$v.log"; echo "FAIL the app's platform calls ($v)"; fails=$((fails + 1)); fi
  else
    cat "$out/app-build-$v.log"; echo "FAIL the app's platform calls ($v) don't build"; fails=$((fails + 1))
  fi
done

pf="$(scripts/win-deps.sh pocketfft)"
for a in x64 arm64; do
  o="$out/win-$a"
  mkdir -p "$o"
  exe="$o/winc-app-wintest.exe"
  # shellcheck disable=SC2086
  if "$WCC" "$a" -w -c -I"$pf" "$pf/pocketfft.c" -o "$o/app-pocketfft.o" > "$out/app-wintest-$a.log" 2>&1 \
     && "$WCC" "$a" $WARN -Wno-unused-command-line-argument -DPLAT_NO_BLOCKS -Icore -Icommon -I"$pf" $APP $APPWIN common/plat_win.c \
          win/c/tests/app_wintest.c win/c/notify_win.c "$o/app-pocketfft.o" -lntdll -lole32 -o "$exe" >> "$out/app-wintest-$a.log" 2>&1; then
    imports="$("$WCC" --tool "$a" llvm-objdump -p "$exe" | sed -n 's/^ *DLL Name: //p' | sort -u | tr '\n' ' ')"
    # shellcheck disable=SC2086
    bad="$(printf '%s\n' $imports | grep -viE '^(KERNEL32|ADVAPI32|USER32|SHELL32|ole32|ntdll)\.dll$|^api-ms-win-crt-[a-z0-9-]+\.dll$' || true)"
    # shellcheck disable=SC2086
    if ! printf '%s\n' $imports | grep -qx 'KERNEL32.dll'; then
      echo "FAIL winc-app-wintest.exe ($a): no KERNEL32.dll in its imports: $imports"; fails=$((fails + 1))
    elif [ -n "$bad" ]; then echo "FAIL winc-app-wintest.exe ($a) imports $bad"; fails=$((fails + 1))
    else echo "ok   winc-app-wintest.exe ($a) imports: $imports"; fi
  else
    cat "$out/app-wintest-$a.log"; echo "FAIL winc-app-wintest.exe ($a) doesn't build"; fails=$((fails + 1))
  fi
  if "$WCC" "$a" $WARN -Wno-unused-command-line-argument -DPLAT_NO_BLOCKS -Icommon -I"$pf" \
      win/c/tests/memo_stamp_wintest.c win/c/sha_memo.c common/plat_win.c "$o/app-pocketfft.o" \
      -lntdll -lole32 -o "$o/memo-stamp-wintest.exe" > "$out/memo-stamp-$a.log" 2>&1; then
    echo "ok   memo-stamp-wintest.exe ($a) builds"
  else
    cat "$out/memo-stamp-$a.log"; echo "FAIL memo-stamp-wintest.exe ($a) doesn't build"; fails=$((fails + 1))
  fi
done

[ "$fails" = 0 ] || { echo "the app's platform calls: $fails failed"; exit 1; }

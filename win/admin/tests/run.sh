#!/bin/sh
# The admin worker's and the app's admin side's tests on the Mac (app.md §7: no VM), with the
# core's warnings (C11, -Wpedantic, -Werror); win/c/tests/run.sh runs this after its own:
#  - the Mac-native tests (main.c and the *_test.c files): admin_proto.c, the worker's runs
#    (worker.c) on real files in a scratch tree through plat_mac.c, compat.c and payload.c with a
#    fake OS (fake_os.c), and admin.c and helper_proc.c (the app's side), plain (-O2) and with
#    ASan+UBSan, and again as a test worker builds them (-DADMIN_TEST_HOLD: its mid-work hold's
#    points, worker.h);
#  - admin.c built as released (hold_release.c): a bin\ worker with no hold is refused (NT1);
#  - the fuzz driver (fuzz.c) on the command line, the pipe's lines, the marker and the worker
#    choice, with ASan+UBSan;
#  - rbsp-admin.exe for Windows x64 (win/admin/build.sh: the release flags, the manifest, a
#    constant version resource with no version in it, I83) and its import gate (app.md §5.5's KnownDLLs rule: KERNEL32, ADVAPI32,
#    ntdll and api-ms-win-crt-*; never bcrypt, wintrust, crypt32, version, SHELL32 or ole32, in the
#    import or the delay-load table); two builds with different SOURCE_DATE_EPOCH and VERSION byte
#    for byte the same (J18, release.md T7/T13); the marker in its bytes;
#  - the app's side (admin_win.c, helper_proc_win.c) linked for Windows x64 and arm64, its imports
#    within the core DLL's allowlist (app.md §7).
#
#   win/admin/tests/run.sh [OUT] [PE]   OUT: default build/admin; PE: win/c's PE fixtures
#                                       (default build/winc/pe); exits 1 on a failure
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${1:-$root/build/admin}"
pe="${2:-$root/build/winc/pe}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
pe="$(cd "$pe" && pwd)"
STD="-std=c11"
WARN="-Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wformat=2 -Wformat-nonliteral -Werror"
MAC="-mmacosx-version-min=14.0"
fails=0
cd "$root"

SRC="win/c/receipt.c win/c/inventory.c win/c/admin_proto.c win/c/admin.c win/c/helper_proc.c win/c/native_admin.c win/admin/worker.c win/c/sig.c win/c/payload.c win/c/payload_store.c win/c/fs_write.c win/c/compat.c common/plat_mac.c core/json.c core/arena.c"
TESTS="win/c/tests/sig_mac.c win/admin/tests/main.c win/admin/tests/proto_test.c win/admin/tests/worker_test.c win/admin/tests/fake_os.c win/admin/tests/client_test.c win/admin/tests/receipt_test.c win/admin/tests/fake_pe_code.c"
FW="-framework Security -framework CoreFoundation -framework Accelerate"
for v in plain asan hold; do
  case "$v" in
    plain) san="-O2" ;;
    asan) san="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined" ;;
    hold) san="-O2 -DADMIN_TEST_HOLD" ;;
  esac
  # shellcheck disable=SC2086
  clang $STD $WARN $MAC $san -DCOMPAT_TEST_HOOKS -DADMIN_TEST_HOOKS -Icore -Icommon -Iwin/c $SRC $TESTS $FW -o "$out/admin-test-$v"
  d="$out/scratch-$v"
  rm -rf "$d"; mkdir -p "$d"
  if "$out/admin-test-$v" "$d" "$pe" > "$out/admin-test-$v.log" 2>&1; then echo "ok   win/admin tests ($v): $(tail -1 "$out/admin-test-$v.log")"
  else tail -40 "$out/admin-test-$v.log"; echo "FAIL win/admin tests ($v)"; fails=$((fails + 1)); fi
done

# admin.c as released (no ADMIN_TEST_HOOKS): a bin\ worker with no hold is refused (review NT1)
# shellcheck disable=SC2086
clang $STD $WARN $MAC -O2 -Icore -Icommon -Iwin/c win/c/admin_proto.c win/c/admin.c win/c/payload.c win/c/sig.c win/c/tests/sig_mac.c \
  common/plat_mac.c core/json.c core/arena.c win/admin/tests/hold_release.c $FW -o "$out/hold-release"
rm -rf "$out/scratch-hold"; mkdir -p "$out/scratch-hold"
if "$out/hold-release" "$out/scratch-hold" > "$out/hold-release.log" 2>&1; then echo "ok   win/admin as released: $(tail -1 "$out/hold-release.log")"
else cat "$out/hold-release.log"; echo "FAIL win/admin as released: a bin\\ worker with no hold was elevated"; fails=$((fails + 1)); fi

# admin.c's write_file with its flushes observed and failing (write_fault.c includes admin.c with
# plat_map_flush, plat_unmap and plat_fsync renamed to its stubs): map, write, flush the view,
# unmap, flush the file; either flush's error comes back and the file is removed. Plain and ASan
for v in plain asan; do
  case "$v" in
    plain) san="-O2" ;;
    asan) san="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined" ;;
  esac
  rm -rf "$out/scratch-write-$v"; mkdir -p "$out/scratch-write-$v"
  # shellcheck disable=SC2086
  if clang $STD $WARN $MAC $san -Icore -Icommon -Iwin/c win/admin/tests/write_fault.c win/c/admin_proto.c win/c/payload.c win/c/sig.c win/c/tests/sig_mac.c \
       common/plat_mac.c core/json.c core/arena.c $FW -o "$out/write-fault-$v" > "$out/write-fault-$v.log" 2>&1 \
     && "$out/write-fault-$v" "$out/scratch-write-$v" >> "$out/write-fault-$v.log" 2>&1; then echo "ok   admin.c's flushed writes ($v): $(tail -1 "$out/write-fault-$v.log")"
  else tail -20 "$out/write-fault-$v.log"; echo "FAIL admin.c's flushed writes ($v)"; fails=$((fails + 1)); fi
  # worker.c's write_new the same way (worker_fault.c includes worker.c with the stubs): the
  # whole view flushed after its bytes, unmapped, the file flushed, closed; either flush failing
  # removes the file (review AF4/WF4). Also checks failed and truncated directory listings.
  rm -rf "$out/scratch-wfault-$v"; mkdir -p "$out/scratch-wfault-$v"
  # shellcheck disable=SC2086
  if clang $STD $WARN $MAC $san -Icore -Icommon -Iwin/c -Iwin/admin win/admin/tests/worker_fault.c win/admin/tests/fake_os.c win/admin/tests/fake_pe_code.c win/c/receipt.c win/c/inventory.c win/c/admin_proto.c win/c/compat.c win/c/payload.c win/c/payload_store.c \
       win/c/fs_write.c win/c/sig.c win/c/tests/sig_mac.c common/plat_mac.c core/json.c core/arena.c $FW -o "$out/worker-fault-$v" > "$out/worker-fault-$v.log" 2>&1 \
     && "$out/worker-fault-$v" "$out/scratch-wfault-$v" >> "$out/worker-fault-$v.log" 2>&1; then echo "ok   worker fault tests ($v): $(tail -1 "$out/worker-fault-$v.log")"
  else tail -20 "$out/worker-fault-$v.log"; echo "FAIL worker fault tests ($v)"; fails=$((fails + 1)); fi
done

# the fuzz driver, ASan+UBSan
# shellcheck disable=SC2086
clang $STD $WARN $MAC -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined -Icore -Icommon -Iwin/c \
  win/c/admin_proto.c win/c/admin.c win/c/helper_proc.c win/admin/worker.c win/c/inventory.c win/c/receipt.c win/c/pe_code.c win/c/compat.c win/c/payload.c win/c/payload_store.c win/c/fs_write.c win/c/sig.c win/c/tests/sig_mac.c \
  common/plat_mac.c core/json.c core/arena.c win/admin/tests/fuzz.c $FW -o "$out/admin-fuzz"
if "$out/admin-fuzz" "${FUZZ_ITERATIONS:-20000}" 0 > "$out/admin-fuzz.log" 2>&1; then echo "ok   win/admin fuzz: $(sed -n 's/^fuzz: //p' "$out/admin-fuzz.log" | tr '\n' ';')"
else tail -20 "$out/admin-fuzz.log"; echo "FAIL win/admin fuzz"; fails=$((fails + 1)); fi

# rbsp-admin.exe: the release build (keys/), a test build (a throwaway key), the same bytes from
# two builds with different SOURCE_DATE_EPOCH and VERSION (VERSION is never read: J18)
key="$out/test-key"
mkdir -p "$key"
[ -f "$key/test.pub.pem" ] || { /usr/bin/openssl ecparam -name prime256v1 -genkey -noout -out "$key/test.pem" 2>/dev/null \
  && /usr/bin/openssl ec -in "$key/test.pem" -pubout -out "$key/test.pub.pem" 2>/dev/null; }
if SOURCE_DATE_EPOCH=1 win/admin/build.sh "$out/rel1" > "$out/build.log" 2>&1 \
   && SOURCE_DATE_EPOCH=2 VERSION=9.9.9 win/admin/build.sh "$out/rel2" >> "$out/build.log" 2>&1 \
   && win/admin/build.sh "$out/test" --test-key "$key/test.pub.pem" >> "$out/build.log" 2>&1; then
  if cmp -s "$out/rel1/rbsp-admin.exe" "$out/rel2/rbsp-admin.exe"; then echo "ok   rbsp-admin.exe: two builds, the same bytes ($(shasum -a 256 "$out/rel1/rbsp-admin.exe" | cut -c1-12))"
  else echo "FAIL rbsp-admin.exe: two builds differ"; fails=$((fails + 1)); fi
  for exe in "$out/rel1/rbsp-admin.exe" "$out/test/rbsp-admin.exe"; do
    # the import gate (app.md §5.5): every DLL in the import and delay-load tables
    dlls="$(scripts/win-cc.sh --tool x64 llvm-readobj --coff-imports "$exe" | sed -n 's/^ *Name: \(.*\.dll\)$/\1/p' | sort -u | tr '\n' ' ')"
    # shellcheck disable=SC2086
    bad="$(printf '%s\n' $dlls | grep -viE '^(KERNEL32|ADVAPI32|ntdll)\.dll$|^api-ms-win-crt-[a-z0-9-]+\.dll$' || true)"
    # shellcheck disable=SC2086
    if ! printf '%s\n' $dlls | grep -qx 'KERNEL32.dll'; then echo "FAIL $exe: no KERNEL32.dll among its imports ($dlls)"; fails=$((fails + 1))
    elif [ -n "$bad" ]; then echo "FAIL $exe imports $bad"; fails=$((fails + 1))
    else echo "ok   $(basename "$(dirname "$exe")")/rbsp-admin.exe imports: $dlls"; fi
    if ! grep -aq 'rbsp-admin worker: proto=2 minimum=' "$exe"; then echo "FAIL $exe: no marker"; fails=$((fails + 1)); fi
    if ! grep -aq 'requireAdministrator' "$exe"; then echo "FAIL $exe: no requireAdministrator manifest"; fails=$((fails + 1)); fi
  done
  # Program Files (its root's and rekordbox's place) movable in the test build only: the shared
  # rule's override (its variable's name, UTF-16, in the bytes), and the rule's key in both
  has16() { python3 -I -c 'import sys; sys.exit(0 if sys.argv[2].encode("utf-16-le") in open(sys.argv[1], "rb").read() else 1)' "$1" "$2"; }
  if has16 "$out/rel1/rbsp-admin.exe" RBSP_TEST_PROGRAMS; then echo "FAIL the release worker can move its root"; fails=$((fails + 1)); fi
  if ! has16 "$out/test/rbsp-admin.exe" RBSP_TEST_PROGRAMS; then echo "FAIL the test worker can't move its root"; fails=$((fails + 1)); fi
  # the VM's holds (stems-cache-uac's sign-out cases) in the test worker only: before the work
  # (RBSP_TEST_WORKER_HOLD: WC1, WC2) and mid-work (RBSP_TEST_WORKER_MIDHOLD: WC3, WC10), their
  # names (UTF-16 and ASCII) and their lines nowhere in the release worker
  for name in RBSP_TEST_WORKER_HOLD RBSP_TEST_WORKER_MIDHOLD; do
    if has16 "$out/rel1/rbsp-admin.exe" "$name" || grep -aq "$name" "$out/rel1/rbsp-admin.exe"; then echo "FAIL the release worker can be held ($name)"; fails=$((fails + 1)); fi
    if ! has16 "$out/test/rbsp-admin.exe" "$name"; then echo "FAIL the test worker can't be held ($name)"; fails=$((fails + 1)); fi
  done
  if grep -aq 'test build: ' "$out/rel1/rbsp-admin.exe"; then echo "FAIL the release worker has a test build's line"; fails=$((fails + 1)); fi
  if ! grep -aq 'test build: held %d s mid-work' "$out/test/rbsp-admin.exe" || ! grep -aq 'bridge swapped into rekordbox' "$out/test/rbsp-admin.exe"; then
    echo "FAIL the test worker has no mid-work hold"; fails=$((fails + 1)); fi
  if grep -aq 'bridge swapped into rekordbox' "$out/rel1/rbsp-admin.exe"; then echo "FAIL the release worker has the mid-work hold's points"; fails=$((fails + 1)); fi
  for exe in "$out/rel1/rbsp-admin.exe" "$out/test/rbsp-admin.exe"; do
    if ! has16 "$exe" 'SOFTWARE\Microsoft\Windows\CurrentVersion' || ! has16 "$exe" ProgramFilesDir; then
      echo "FAIL $exe doesn't read ProgramFilesDir"; fails=$((fails + 1)); fi
    if grep -aq 'C:\\Program Files' "$exe" || has16 "$exe" 'C:\Program Files'; then echo "FAIL $exe names C:\\Program Files"; fails=$((fails + 1)); fi
  done
else
  tail -30 "$out/build.log"; echo "FAIL rbsp-admin.exe doesn't build"; fails=$((fails + 1))
fi

# the app's admin side (admin_win.c, helper_proc_win.c) linked for Windows, both architectures:
# its imports within the core DLL's allowlist (app.md §7: KERNEL32, ADVAPI32, SHELL32, ole32,
# ntdll, api-ms-win-crt-*; bcrypt only at run time)
pf="$(scripts/win-deps.sh pocketfft)"
for a in x64 arm64; do
  g="$out/gate-$a"
  mkdir -p "$g"
  # shellcheck disable=SC2086
  if scripts/win-cc.sh "$a" -w -c -I"$pf" "$pf/pocketfft.c" -o "$g/pocketfft.o" > "$g/cc.log" 2>&1 \
     && scripts/win-cc.sh "$a" $WARN -Wno-unused-command-line-argument -ffunction-sections -Wl,--gc-sections -DCORE_WIN_POLICY -DPLAT_NO_BLOCKS \
          -Icore -Icommon -Iwin/c -I"$pf" win/admin/tests/client_gate.c win/c/admin.c win/c/admin_win.c win/c/admin_proto.c win/c/helper_proc.c \
          win/c/helper_proc_win.c win/c/payload.c win/c/sig.c win/c/sig_bcrypt.c core/json.c core/arena.c common/plat_win.c "$g/pocketfft.o" \
          -lntdll -ladvapi32 -lshell32 -lole32 -o "$g/admin-client-gate.exe" >> "$g/cc.log" 2>&1; then
    dlls="$(scripts/win-cc.sh --tool "$a" llvm-readobj --coff-imports "$g/admin-client-gate.exe" | sed -n 's/^ *Name: \(.*\.dll\)$/\1/p' | sort -u | tr '\n' ' ')"
    # shellcheck disable=SC2086
    bad="$(printf '%s\n' $dlls | grep -viE '^(KERNEL32|ADVAPI32|SHELL32|ole32|ntdll)\.dll$|^api-ms-win-crt-[a-z0-9-]+\.dll$' || true)"
    if [ -n "$bad" ]; then echo "FAIL the app's admin side ($a) imports $bad"; fails=$((fails + 1))
    else echo "ok   the app's admin side ($a) imports: $dlls"; fi
  else
    cat "$g/cc.log"; echo "FAIL the app's admin side doesn't link for Windows $a"; fails=$((fails + 1))
  fi
done

# a cancelled overlapped connect is waited for (GetOverlappedResult, bWait TRUE) before the
# OVERLAPPED on admin_win.c's stack goes: CancelIo's contract (audit b-a1b-F1). Each CancelIo has
# that wait on the same line or within its next three lines. Every early exit between the
# connect and its normal completion must also have waited, unless the connect never started.
# Strip comments while keeping line numbers for the wait checks.
stripped="$(mktemp)"
trap 'rm -f "$stripped"' EXIT
awk '
  {
    line = $0; code = ""
    while (line != "") {
      if (in_comment) {
        end = index(line, "*/")
        if (!end) break
        line = substr(line, end + 2); in_comment = 0; continue
      }
      block = index(line, "/*"); slash = index(line, "//")
      if (slash && (!block || slash < block)) { code = code substr(line, 1, slash - 1); break }
      if (!block) { code = code line; break }
      code = code substr(line, 1, block - 1) " "
      line = substr(line, block + 2); in_comment = 1
    }
    print code
  }
' win/c/admin_win.c > "$stripped"
if awk '
  /ConnectNamedPipe\(srv, &ov\)/ { connect = NR }
  /GetOverlappedResult\(srv, &ov, &dummy, FALSE\)/ { complete = NR }
  /CancelIo\(/ { n++; c = NR }
  /GetOverlappedResult\(srv, &ov, &dummy, TRUE\)/ { waited = NR; if (c && NR <= c + 3) c = 0 }
  c && NR == c + 3 { bad = 1 }
  connect && !complete && /goto out;/ && !/e != ERROR_IO_PENDING/ && (waited < connect || NR - waited > 3) { bad = 1 }
  END { exit bad || c || !n || !connect || !complete }
' "$stripped"; then
  echo "ok   admin_win.c: each CancelIo and early exit waits for the pending connect"
else
  echo "FAIL admin_win.c: an early exit or CancelIo can leave a pending connect (its OVERLAPPED on the stack)"; fails=$((fails + 1))
fi

[ "$fails" = 0 ] && echo "win/admin: all passed" || { echo "win/admin: $fails failed"; exit 1; }

#!/bin/sh
# Builds the shared C core (core/*.c, C11, no third-party code). mac-core §5.
#
#   core/build.sh lib OUT [--test-hooks [--broken-selftest]]
#                                         the Mac archives the app links (scripts/build-app.sh links
#                                         libcore.a into every release, M7's switch-over; a TEST
#                                         build the --test-hooks one): OUT/<arch>/libcore.a for arm64 and
#                                         x86_64, -O2, macOS 12, -fvisibility=hidden (the app exports
#                                         nothing of the core), no CORE_WIN_POLICY, no asserts. The
#                                         release gate runs on each: no core_test_ symbol, every
#                                         defined external symbol private, no debug marker. Every
#                                         compile has -Werror=unguarded-availability-new (an API newer
#                                         than macOS 12 without a check fails) and the flags are
#                                         printed ("core cflags: ...": release.md §6.1's gate greps it).
#                                         --test-hooks: with core.h's CORE_TEST_HOOKS, for TEST_BUILD
#                                         apps only (the G1 API spike), under its own name and
#                                         objects: OUT/<arch>/libcore-testhooks.a. Never for a release.
#                                         --broken-selftest (after --test-hooks): core_selftest()
#                                         fails (9), for G1.4's refused-swap test only:
#                                         OUT/<arch>/libcore-testhooks-broken.a.
#                                         Next to them, OUT/<name>.inputs: the repository files that
#                                         reach the archives' bytes, from the compiles' clang
#                                         depfiles (scripts/mac-inputs.py deps) plus core/build.sh,
#                                         one per line, sorted (review SO7: the Mac app's input list
#                                         takes these instead of all of core/, so a Windows-only or
#                                         test-only change isn't a Mac input).
#   core/build.sh test OUT SAN OS         the unit tests: OUT/core-test-SAN-OS, this Mac's arch, with
#                                         the frozen Swift's vectors (OUT/vectors_swift.h, made by
#                                         core/tests/gen-vectors.sh when it is missing or older than
#                                         the frozen Swift (core/tests/swift/oracle: app/Sources at
#                                         56d7c11 plus freeze-time fixes), the vectors'
#                                         Swift or gen-vectors.sh).
#                                         SAN: plain (-O2) and asanrel (ASan + UBSan), both as released:
#                                         no CORE_DEBUG, so the release behaviour of violations is
#                                         tested (tsanrel: TSan on those paths too); asan (ASan +
#                                         UBSan) and tsan, with CORE_DEBUG
#                                         (violations abort; the leak check runs).
#                                         OS: mac (as shipped: no CORE_WIN_POLICY) or win (the host
#                                         test build: CORE_WIN_POLICY, both OS columns).
#   core/build.sh fuzz OUT OS             core/tests/fuzz.c with ASan and UBSan, CORE_DEBUG, for mac
#                                         (the config as shipped) or win: OUT/core-fuzz-OS
#   core/build.sh swift OUT               core/tests/smoke.swift with core.h as its bridging header
#                                         and the generated CoreIds.swift, linked with
#                                         lib --test-hooks: OUT/core-smoke
#   core/build.sh ids OUT                 the shells' generated ids from the .def tables:
#                                         OUT/CoreIds.cs (CORE_WIN_POLICY, as the DLL) for the WPF
#                                         shell (W8a), OUT/CoreIds.swift (the Mac build) for Swift,
#                                         and OUT/CoreIds.swift.inputs: the repository files the
#                                         Mac generator was built from (its depfile: gen_ids.c, the
#                                         .def tables, the headers) plus core/build.sh
#   core/build.sh dll OUT [--test-hooks [--test-key PEM]]
#                                         the Windows core (app.md §3): OUT/x64/rbsp-core.dll and
#                                         OUT/arm64/rbsp-core.dll, llvm-mingw through
#                                         scripts/win-cc.sh (its release flags), CORE_WIN_POLICY,
#                                         a version resource (win/c/core.rc) stamped with
#                                         RBSP_WIN_VERSION, else VERSION, and checked,
#                                         with the "native" entries' Windows code ($DLL_WINC,
#                                         CORE_NATIVE_WIN: win/c's modules, common/plat_win.c and
#                                         pinned pocketfft, scripts/win-deps.sh), exports by
#                                         CORE_API only. The gate: KERNEL32 imported, and nothing
#                                         beyond the core DLL's allowlist (app.md §7, I81:
#                                         KERNEL32, ADVAPI32, USER32, SHELL32, ole32, comctl32,
#                                         ntdll, api-ms-win-crt-*); no export but core_*; in the
#                                         release DLL, no core_test_ export and no debug marker
#                                         (as lib's).
#                                         --test-hooks: with CORE_TEST_HOOKS, for TEST builds only
#                                         (the W0 spike), in its own folder (the file name is the
#                                         same): OUT/testhooks/<arch>/rbsp-core.dll; Program Files
#                                         movable by RBSP_TEST_PROGRAMS (PLAT_TEST_PROGRAMS), which
#                                         the release DLL never reads; core_test_update_base, the
#                                         TEST build's download server (update_shell.h), exported.
#                                         The payload keys compiled in: keys/release and
#                                         keys/backup (refused when either is build/test-keys'
#                                         test key or both are one key, as build-app.sh refuses);
#                                         --test-key PEM (with --test-hooks only): that key alone.
#   core/build.sh tables OUT              OUT/mask_tables.h, the masking's Unicode tables, from this
#                                         Mac's ICU and CoreFoundation (core/tools/gen_mask_tables.c,
#                                         -licucore): what core/mask_tables.h, committed, must be
#                                         (make core-test compares them)
#   core/build.sh srcs                    prints the core's sources (relative to core/), one line
#   core/build.sh tests                   prints the unit tests' sources and entry points, the same
#   core/build.sh vectors OUT             OUT/vectors_swift.h, the frozen Swift's vectors, when the
#                                         core has a generator for them (core/tests/gen-vectors.sh;
#                                         again when stale, as test's); nothing otherwise.
#                                         common/tests/run.sh builds the core's
#                                         tests on common/plat.h from these three, so its lists
#                                         can't drift from the core's.
#
# Needs Xcode's command line tools, nothing else (dll: the pinned llvm-mingw, fetched by win-cc.sh).
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
MIN_MACOS=12.0
STD="-std=c11"
WARN="-Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wformat=2 -Wformat-nonliteral -Werror"
SRCS="arena core dialog dialog_texts dump flow flows helper_lines ids json lights mask menus model prepare_tab schema selftest state status_tab text view watcher words"
WIN_SRCS="win native"   # Windows-only (CORE_WIN_POLICY): in the test, fuzz and dll builds, never in lib's Mac archive
# the dll build only: the "native" backend and the win/c and common/ code it calls (repository paths)
DLL_WINC="win/c/core_native win/c/core_native_admin win/c/native_admin win/c/core_native_steps win/c/native_steps win/c/update_win win/c/helper_proc win/c/helper_proc_win win/c/admin win/c/admin_win
  win/c/admin_proto win/c/update win/c/update_rules win/c/unzip win/c/compat win/c/compat_win win/c/payload win/c/payload_store win/c/fs_write win/c/sig
  win/c/sig_bcrypt win/c/sha_memo win/c/native_facts win/c/native_config win/c/models common/marker win/c/volumes win/c/core_native_life win/c/lifecycle win/c/lifecycle_win win/c/osversion_win win/c/shell win/c/afterexit win/c/afterexit_win win/c/afterexit_self_win win/c/host_facts win/c/native_location common/cfg_ini common/path_rules common/plat_win"
TESTS="tests/unit.c tests/vectors.c tests/pure.c tests/dfuzz.c tests/flows_test.c tests/view_test.c tests/win_texts.c tests/win_front.c"      # the unit tests' sources, relative to core/

usage() { echo "usage: $0 lib OUT [--test-hooks [--broken-selftest]] | dll OUT [--test-hooks [--test-key PEM]] | test OUT plain|asan|asanrel|tsan|tsanrel mac|win | fuzz OUT mac|win | swift OUT | ids OUT | tables OUT | srcs | tests | vectors OUT" >&2; exit 2; }
# relative names, compiled from core/ (the repository's path has spaces)
srcs() { for s in $SRCS $WIN_SRCS; do printf '%s.c ' "$s"; done; }
# the DLL's own Windows code for the "native" entries (API-V1-NOTES §11), relative to core/
winc() { for s in $DLL_WINC; do printf '../%s.c ' "$s"; done; }
# the frozen Swift's vectors (core/tests/gen-vectors.sh) in $out, made once per build folder
# ... and again when a frozen Swift source, the vectors' Swift or gen-vectors.sh is newer than it (Q26)
vectors() {
  stale=
  if [ -f "$out/vectors_swift.h" ]; then
    for f in "$here"/tests/swift/oracle/*.swift "$here/tests/swift/vectors.swift" "$here/tests/gen-vectors.sh"; do
      [ "$f" -nt "$out/vectors_swift.h" ] && { stale=1; break; }
    done
  else stale=1; fi
  [ -z "$stale" ] || "$here/tests/gen-vectors.sh" "$out" > /dev/null
}
case "${1:-}" in
  srcs) [ $# -eq 1 ] || usage; srcs; echo; exit 0 ;;
  tests) [ $# -eq 1 ] || usage; echo "$TESTS"; exit 0 ;;
esac
[ $# -ge 2 ] || usage
mode="$1"
mkdir -p "$2"
out="$(cd "$2" && pwd)"
shift 2

case "$mode" in
lib)
  hooks= name=libcore objdir=obj
  case "${1:-}" in --test-hooks) hooks="-DCORE_TEST_HOOKS" name=libcore-testhooks objdir=obj-testhooks ;; "") ;; *) usage ;; esac
  case "${2:-}" in
    --broken-selftest) [ -n "$hooks" ] || usage; hooks="$hooks -DCORE_SELFTEST_BROKEN" name=libcore-testhooks-broken objdir=obj-testhooks-broken ;;
    "") ;; *) usage ;;
  esac
  avail="-Werror=unguarded-availability-new"
  # one set of flags: what is printed is exactly what every compile gets (<arch> replaced)
  # -MD: each object's clang depfile beside it (obj/<src>.d), the input list's source; it doesn't
  # change the object's bytes
  cflags="$STD $WARN -O2 -target <arch>-apple-macos$MIN_MACOS -fvisibility=hidden -fno-common -DNDEBUG -MD $avail${hooks:+ $hooks}"
  echo "core cflags: $cflags"
  for arch in arm64 x86_64; do
    obj="$out/$arch/$objdir"
    rm -rf "$obj"
    mkdir -p "$obj"
    arch_cflags="$(printf '%s' "$cflags" | sed "s/<arch>/$arch/")"
    for s in $SRCS; do
      # no -g and a fixed working directory: the objects carry no build path (reproducible)
      # shellcheck disable=SC2086
      (cd "$here" && clang $arch_cflags -c "$s.c" -o "$obj/$s.o")
    done
    lib="$out/$arch/$name.a"
    rm -f "$lib"
    # shellcheck disable=SC2046
    (cd "$obj" && xcrun libtool -static -D -no_warning_for_no_symbols -o "$lib" $(for s in $SRCS; do printf '%s.o ' "$s"; done))
    if [ -z "$hooks" ]; then
      # the release gate (C38)
      if nm -m "$lib" | grep -q '_core_test_'; then echo "core/build.sh: $lib has test hooks" >&2; exit 1; fi
      if nm -m "$lib" | grep -v '(undefined)' | grep ' external ' | grep -qv 'private external'; then
        echo "core/build.sh: $lib exports a symbol that isn't private" >&2; exit 1
      fi
      if strings "$lib" | grep -q 'RBSP-CORE-DEBUG-BUILD'; then echo "core/build.sh: $lib is a debug build" >&2; exit 1; fi
      # the core's probe timing points (core_test_set_mark's names) are the test builds' only: add each new one here
      if strings -a "$lib" | grep -q 'trigger\.actionEnd'; then echo "core/build.sh: $lib has the probe's timing points" >&2; exit 1; fi
    fi
    echo "built $lib${hooks:+ ($hooks)}"
  done
  # the archives' input list (review SO7): both architectures' depfiles, and this script
  # (the parser's own status checked, and an empty list refused: the list from before stays then)
  deps="$(python3 -I "$here/../scripts/mac-inputs.py" deps --cwd "$here" "$here/.." "$out"/*/"$objdir"/*.d)" \
    || { echo "core/build.sh: the depfiles of $name couldn't be read" >&2; exit 1; }
  [ -n "$deps" ] || { echo "core/build.sh: no input list for $name" >&2; exit 1; }
  printf '%s\ncore/build.sh\n' "$deps" | LC_ALL=C sort -u > "$out/$name.inputs.tmp"
  mv "$out/$name.inputs.tmp" "$out/$name.inputs"
  echo "wrote $out/$name.inputs"
  ;;
test)
  [ $# -eq 2 ] || usage
  case "$1" in
    plain) san="-O2" ;;
    asan) san="-O1 -g -DCORE_DEBUG -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined" ;;
    asanrel) san="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined" ;;
    tsan) san="-O1 -g -DCORE_DEBUG -fsanitize=thread" ;;
    tsanrel) san="-O1 -g -fsanitize=thread" ;;
    *) usage ;;
  esac
  case "$2" in mac) os= ;; win) os="-DCORE_WIN_POLICY" ;; *) usage ;; esac
  bin="$out/core-test-$1-$2"
  vectors
  # shellcheck disable=SC2046
  (cd "$here" && clang $STD $WARN $san -DCORE_TEST_HOOKS $os -I. -I"$out" $(srcs) $TESTS -lpthread -o "$bin")
  echo "built $bin"
  ;;
fuzz)
  [ $# -eq 1 ] || usage
  case "$1" in mac) os= ;; win) os="-DCORE_WIN_POLICY" ;; *) usage ;; esac
  # shellcheck disable=SC2046
  (cd "$here" && clang $STD $WARN -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined \
    -DCORE_DEBUG -DCORE_TEST_HOOKS $os -I. $(srcs) tests/fuzz.c tests/dfuzz.c -lpthread -o "$out/core-fuzz-$1")
  echo "built $out/core-fuzz-$1 (ASan and UBSan)"
  ;;
swift)
  # core.h as the Swift shell imports it (the bridging header), the core linked statically
  [ $# -eq 0 ] || usage
  "$0" lib "$out/lib" --test-hooks
  "$0" ids "$out"
  arch="$(uname -m)"
  # top-level code beside another file: it must be called main.swift
  mkdir -p "$out/smoke"
  cp "$here/tests/smoke.swift" "$out/smoke/main.swift"
  (cd "$here" && xcrun swiftc -sdk "$(xcrun --sdk macosx --show-sdk-path)" -target "$arch-apple-macos$MIN_MACOS" \
    -Xcc -DCORE_TEST_HOOKS -import-objc-header core.h "$out/smoke/main.swift" "$out/CoreIds.swift" \
    "$out/lib/$arch/libcore-testhooks.a" -o "$out/core-smoke")
  echo "built $out/core-smoke"
  ;;
ids)
  [ $# -eq 0 ] || usage
  (cd "$here" && clang $STD $WARN -O0 -DCORE_WIN_POLICY -I. tools/gen_ids.c -o "$out/gen_ids_win")
  (cd "$here" && clang $STD $WARN -O0 -I. -MD -MF "$out/gen_ids_mac.d" tools/gen_ids.c -o "$out/gen_ids_mac")
  "$out/gen_ids_win" cs > "$out/CoreIds.cs"
  "$out/gen_ids_mac" swift > "$out/CoreIds.swift"
  # CoreIds.swift's input list (review SO7): the Mac generator's depfile, and this script
  deps="$(python3 -I "$here/../scripts/mac-inputs.py" deps --cwd "$here" "$here/.." "$out/gen_ids_mac.d")" \
    || { echo "core/build.sh: the depfiles of CoreIds.swift couldn't be read" >&2; exit 1; }
  [ -n "$deps" ] || { echo "core/build.sh: no input list for CoreIds.swift" >&2; exit 1; }
  printf '%s\ncore/build.sh\n' "$deps" | LC_ALL=C sort -u > "$out/CoreIds.swift.inputs.tmp"
  mv "$out/CoreIds.swift.inputs.tmp" "$out/CoreIds.swift.inputs"
  echo "wrote $out/CoreIds.cs, $out/CoreIds.swift and $out/CoreIds.swift.inputs"
  ;;
dll)
  hooks= dir="$out" test_key=
  case "${1:-}" in --test-hooks) hooks="-DCORE_TEST_HOOKS -DPLAT_TEST_PROGRAMS" dir="$out/testhooks" ;; "") ;; *) usage ;; esac
  case "${2:-}" in
    --test-key) [ -n "$hooks" ] && [ -n "${3:-}" ] || usage; test_key="$3" ;;   # a test build's key, never a release's
    "") ;; *) usage ;;
  esac
  # the public keys payload.json's signature is checked with (core_native_admin.h's
  # native_key_texts): keys/release and keys/backup, with build-app.sh's refusals (the test key,
  # the two the same key); with --test-key (test builds only), that key alone, named "test"
  # as the 91-byte uncompressed SPKI sig.c takes (sig_spki_p256), whatever point form the PEM
  # holds; anything else fails
  pubkey_b64() {
    text="$(/usr/bin/openssl ec -pubin -in "$1" -noout -text 2>/dev/null)" && printf '%s\n' "$text" | grep -q 'ASN1 OID: prime256v1' || return 1
    [ "$(/usr/bin/openssl ec -pubin -in "$1" -conv_form uncompressed -outform DER 2>/dev/null | wc -c | tr -d ' ')" = 91 ] || return 1
    /usr/bin/openssl ec -pubin -in "$1" -conv_form uncompressed -outform DER 2>/dev/null | /usr/bin/base64 | tr -d '\n'
  }
  mkdir -p "$dir"
  {
    echo "/* Written by core/build.sh dll: the public keys payload.json's signature is checked with. */"
    echo "#include \"sig.h\""
    echo "const sig_key_text native_key_texts[] = {"
    if [ -n "$test_key" ]; then
      k="$(pubkey_b64 "$test_key")" || { echo "core/build.sh: $test_key isn't a P-256 public key" >&2; exit 1; }
      echo "    {\"test\", \"$k\"},"
      echo "};"
      echo "const int native_n_key_texts = 1;"
    else
      prev=
      for name in release backup; do
        f="$here/../keys/$name.pub.pem"
        [ -f "$f" ] || { echo "core/build.sh: keys/$name.pub.pem is missing" >&2; exit 1; }
        k="$(pubkey_b64 "$f")" || { echo "core/build.sh: keys/$name.pub.pem isn't a P-256 public key" >&2; exit 1; }
        t="$here/../build/test-keys/test.pub.pem"
        if [ -f "$t" ] && [ "$k" = "$(pubkey_b64 "$t")" ]; then
          echo "core/build.sh: keys/$name.pub.pem is the test key: not building with it" >&2; exit 1
        fi
        [ "$k" != "$prev" ] || { echo "core/build.sh: keys/release.pub.pem and keys/backup.pub.pem are the same key" >&2; exit 1; }
        prev="$k"
        echo "    {\"$name\", \"$k\"},"
      done
      echo "};"
      echo "const int native_n_key_texts = 2;"
    fi
  } > "$dir/core_native_keys.c.part"
  mv -f "$dir/core_native_keys.c.part" "$dir/core_native_keys.c"
  # after the keys: a refused key stops the build before anything is fetched or compiled
  wcc="$here/../scripts/win-cc.sh"
  pf="$("$here/../scripts/win-deps.sh" pocketfft)"
  # the version resource (win/c/core.rc, release.md §3.2): RBSP_WIN_VERSION (win/shell/build.sh's
  # --app-version, a carry-forward's previous version) or VERSION
  ver="${RBSP_WIN_VERSION:-$(tr -d '[:space:]' < "$here/../VERSION")}"
  expr "$ver" : '[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*$' > /dev/null || { echo "core/build.sh: the version must be x.y.z, not '$ver'" >&2; exit 1; }
  v1="${ver%%.*}"; rest="${ver#*.}"; v2="${rest%%.*}"; v3="${rest#*.}"
  printf '#define V1 %s\n#define V2 %s\n#define V3 %s\n#define VSTR "%s"\n' "$v1" "$v2" "$v3" "$ver" > "$dir/core_version.h"
  cp "$here/../win/c/core.rc" "$dir/core.rc"
  for arch in x64 arm64; do
    mkdir -p "$dir/$arch"
    dll="$dir/$arch/rbsp-core.dll"
    (cd "$dir" && "$wcc" --tool "$arch" windres core.rc -O coff -o "$arch/core.res.o")
    "$wcc" "$arch" -w -c -I"$pf" "$pf/pocketfft.c" -o "$dir/$arch/pocketfft.o"
    # shellcheck disable=SC2046,SC2086
    (cd "$here" && "$wcc" "$arch" $WARN -Wno-unused-command-line-argument -DNDEBUG -DCORE_WIN_POLICY -DCORE_BUILD_DLL -DCORE_NATIVE_WIN \
      -DPLAT_NO_BLOCKS $hooks -I. -I../common -I../win/c -I"$pf" -shared $(srcs) $(winc) "$dir/core_native_keys.c" "$dir/$arch/pocketfft.o" \
      "$dir/$arch/core.res.o" -lntdll -lshell32 -lole32 -Wl,-Map="$dll.map" -o "$dll")
    dump="$("$wcc" --tool "$arch" llvm-objdump -p "$dll")"
    imports="$(printf '%s\n' "$dump" | sed -n 's/^ *DLL Name: //p' | tr 'A-Z' 'a-z' | sort -u)"
    # shellcheck disable=SC2086
    case " $(printf '%s ' $imports)" in *" kernel32.dll "*) ;; *) echo "core/build.sh: $dll: no KERNEL32.dll in its imports" >&2; exit 1 ;; esac
    # the core DLL's allowlist (app.md §7, I81); bcrypt, wintrust, crypt32, winhttp, wevtapi and
    # cfgmgr32 are loaded from System32 at run time, never imported
    for i in $imports; do
      case "$i" in kernel32.dll|advapi32.dll|user32.dll|shell32.dll|ole32.dll|comctl32.dll|ntdll.dll|api-ms-win-crt-*.dll) ;;
        *) echo "core/build.sh: $dll imports $i" >&2; exit 1 ;; esac
    done
    # the core's entry points only (CORE_API): nothing of win/c or common/ is exported
    if printf '%s\n' "$dump" | sed -n '/^Export Table:/,/^$/p' | grep -E '^ +[0-9]+ +0x' | grep -vE ' core_[a-z0-9_]+$' | grep -q .; then
      echo "core/build.sh: $dll exports something that isn't core_*" >&2; exit 1
    fi
    # its version resource: the version stamped, as a DLL
    vr="$(python3 -I "$here/../scripts/win-version.py" "$dll")"
    for want in "fixed.file=$ver.0" "fixed.product=$ver.0" fixed.type=2 "string.ProductVersion=$ver" "string.ProductName=RB Stems Plus"; do
      printf '%s\n' "$vr" | grep -qxF "$want" || { echo "core/build.sh: $dll's version resource has no $want" >&2; exit 1; }
    done
    # core_win.h's entry points (the WPF shell's one copy, process start time, CriticalWork and cheap
    # facts) and the stub copy's after-exit (update_shell.h, release.md T11) are exported
    for e in core_win_one_take core_win_one_window core_win_one_window_close core_win_one_close core_win_process_start \
             core_win_critical_running core_win_critical_wait core_win_critical_close core_win_after_exit core_win_cheap_facts core_win_warm_facts; do
      printf '%s\n' "$dump" | sed -n '/^Export Table:/,/^$/p' | grep -qE " $e\$" || { echo "core/build.sh: $dll doesn't export $e" >&2; exit 1; }
    done
    # a test-hooks DLL takes the harness's fake helper (core_win.h); a release one can't (below)
    if [ -n "$hooks" ] && ! printf '%s\n' "$dump" | sed -n '/^Export Table:/,/^$/p' | grep -qE " core_test_win_fake_helper\$"; then
      echo "core/build.sh: $dll doesn't export core_test_win_fake_helper" >&2; exit 1
    fi
    if [ -z "$hooks" ]; then
      # the release gate, as lib's (C38)
      if printf '%s\n' "$dump" | grep -q 'core_test_'; then echo "core/build.sh: $dll exports test hooks" >&2; exit 1; fi
      if strings -a "$dll" | grep -q 'RBSP-CORE-DEBUG-BUILD'; then echo "core/build.sh: $dll is a debug build" >&2; exit 1; fi
      if strings -a "$dll" | grep -q 'trigger\.actionEnd'; then echo "core/build.sh: $dll has the probe's timing points" >&2; exit 1; fi
      # Program Files from HKLM only (plat_program_files): the test override is the test build's,
      # and no hard-coded C:\Program Files (UTF-8 or UTF-16) is left (review PF6)
      if python3 -I -c 'import sys; sys.exit(0 if "RBSP_TEST_PROGRAMS".encode("utf-16-le") in open(sys.argv[1], "rb").read() else 1)' "$dll"; then
        echo "core/build.sh: $dll reads RBSP_TEST_PROGRAMS" >&2; exit 1
      fi
      if python3 -I -c 'import sys; b = open(sys.argv[1], "rb").read().lower(); w = "c:\\program files"; sys.exit(0 if w.encode() in b or w.encode("utf-16-le") in b else 1)' "$dll"; then
        echo "core/build.sh: $dll names C:\\Program Files" >&2; exit 1
      fi
    elif ! printf '%s\n' "$dump" | sed -n '/^Export Table:/,/^$/p' | grep -qE ' core_test_update_base$'; then
      # a TEST DLL takes its build's download server (update_shell.h, release.md T12)
      echo "core/build.sh: $dll doesn't export core_test_update_base" >&2; exit 1
    fi
    echo "built $dll${hooks:+ (CORE_TEST_HOOKS)}"
  done
  ;;
tables)
  [ $# -eq 0 ] || usage
  (cd "$here" && clang $STD $WARN -O1 tools/gen_mask_tables.c -licucore -framework CoreFoundation -o "$out/gen_mask_tables")
  "$out/gen_mask_tables" > "$out/mask_tables.h"
  echo "wrote $out/mask_tables.h"
  ;;
vectors)
  [ $# -eq 0 ] || usage
  if [ -x "$here/tests/gen-vectors.sh" ]; then vectors; fi
  ;;
*) usage ;;
esac

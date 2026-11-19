#!/bin/bash
# Builds every release asset into dist/:
#   rbstemsplus-app.zip           the app (scripts/build-app.sh)
#   libonnxruntime.1.18.0.dylib   the universal bridge (bridge/build.sh)
#   stemsplus-model.onnx          the model, copied from RBSTEMSPLUS_MODEL (default: the local
#                                 experiments file below) or already in dist/; never built here
#   bootstrap.sh                  the pasted installer (scripts/bootstrap.sh, with the public
#                                 signing keys written in)
#   NOTICE, LICENSE               the licences (also inside the app)
#   payload.json                  versions, file names, SHA-256s, the compatible versions, and
#                                 the commit it was built from
#   SHA256SUMS                    shasum -a 256 of all of the above
# payload.json.sig, its signature, is not made here for a release: scripts/release.sh, which runs
# this build, signs it and uploads the release as a draft (docs/RELEASING.md). The app and the
# bootstrap refuse a payload.json without a valid one.
#
#   scripts/build.sh               a release, into dist/: only from a commit (no uncommitted or
#                                  untracked file), with keys/release.pub.pem and
#                                  keys/backup.pub.pem compiled in
#   scripts/build.sh --test URL    a test build into dist-test/: the app downloads from URL (a
#                                  local server serving dist-test/) instead of the GitHub release,
#                                  and trusts only a test key (build/test-keys/, made here once),
#                                  which also signs dist-test/payload.json
#   ... --plan FILE [--carry DIR]  in the shape of a release plan (scripts/release.sh --plan
#                                  writes it, build/gates/<commit>/plan; release.md §1.3): each
#                                  Mac file the plan carries is the previous release's, byte for
#                                  byte, with its version and commit in payload.json, and only the
#                                  others are built. The carried files come from DIR: for a release,
#                                  the previous release's verified files (scripts/release.sh
#                                  passes them); for a test build (--test URL --plan FILE), the
#                                  previous release's TEST build in the gate archive by default,
#                                  ${GATES_ARCHIVE:-gates-archive}/tag-<its tag>/ (its dist-test
#                                  files and the test key pair that signed them, test-keys/;
#                                  docs/RELEASING.md archives each release's), checked against the
#                                  archive's SHA256SUMS. A TEST build that carries a file is signed
#                                  with that folder's test key, never with build/test-keys/ (which
#                                  isn't made or read then), so the carried app trusts it. One that
#                                  carries nothing (a previous release named) is signed with the
#                                  gate archive's test key as well (tag-<its tag>/test-keys/, the
#                                  same pair in every tag-*/), as G3 updates the archived TEST
#                                  builds to it. A test plan must be this commit's, with no
#                                  uncommitted change.
#   ... --windows                  also the Windows files (release.md §3.1, T12: scripts/build-windows.sh
#                                  into the same folder) and payload.json's windows block: the app
#                                  zip, the bridge, the worker's and the stubs' SHA-256, the
#                                  Windows compatible lists, resampler_proved. Without it (and
#                                  without a plan's windows.app new) no Windows file is built and
#                                  the payload has no windows block (I81: a Mac fix never waits for
#                                  the Windows build). With --test, --win-test WURL is the Windows
#                                  TEST app's download server (https://: the core downloads over
#                                  HTTPS only; the Parallels host, e.g. https://10.211.55.2:8766):
#                                  the shell hands it to the core's TEST DLL (core_test_update_base),
#                                  and a test TLS CA (name-constrained to WURL's host) and its
#                                  server certificate are kept in build/test-keys/win-tls/: the
#                                  certificate is remade when under 30 days are left, the CA too
#                                  when the host changes or the CA runs out (test-ca.pem, the CA's
#                                  certificate, into dist-test/ for the VM to trust). The Windows
#                                  TEST build trusts the same test key as the Mac's
#   ... --test URL --broken-selftest-test
#                                  the same with RBSTEMSPLUS_TEST_BROKEN_SELFTEST=1: an app whose
#                                  core self-test fails, for G1.4's refused-swap test only. Without
#                                  this flag the variable is refused, so a leaked one never makes
#                                  a gate build whose every --swap fails.
#
# payload.json's app.commit and bridge.commit name the commit that built each Mac file: this one,
# or the carried file's own (release.md §1.2).
#
# Safe to run again: each step replaces its own output. Stops at the first problem with a message.
# Needs Xcode (actool with Icon Composer support) and whatever bridge/build.sh needs.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
dist="$root/dist"
test_base=""
plan=""
carry=""; archive_keys=""
broken_flag=()
windows=""
win_test=""
usage() { echo "usage: $0 [--test URL [--win-test WURL] [--broken-selftest-test]] [--windows] [--plan FILE [--carry DIR]]" >&2; exit 2; }
while [ $# -gt 0 ]; do
  case "$1" in
    --test) [ $# -ge 2 ] && [ -z "$test_base" ] || usage; test_base="$2"; dist="$root/dist-test"; shift 2 ;;
    --plan) [ $# -ge 2 ] && [ -z "$plan" ] || usage; plan="$2"; shift 2 ;;
    --carry) [ $# -ge 2 ] && [ -z "$carry" ] || usage; carry="$2"; shift 2 ;;
    --broken-selftest-test) [ ${#broken_flag[@]} -eq 0 ] || usage; broken_flag=(--broken-selftest-test); shift ;;
    --windows) [ -z "$windows" ] || usage; windows=1; shift ;;
    --win-test) [ $# -ge 2 ] && [ -z "$win_test" ] || usage; win_test="$2"; shift 2 ;;
    *) usage ;;
  esac
done
[ -z "$carry" ] || [ -n "$plan" ] || usage
# --broken-selftest-test only with --test
[ ${#broken_flag[@]} -eq 0 ] || [ -n "$test_base" ] || usage
[ -n "$test_base" ] || [ -z "$plan" ] || [ -n "$carry" ] || [ ! -f "$plan" ] || [ "$(grep -cE '^(mac\.app|mac\.bridge|windows\.[a-z0-9.]+) carry ' "$plan")" = 0 ] \
  || { echo "build.sh: the plan carries a file: a release build needs --carry DIR (scripts/release.sh passes it)" >&2; exit 2; }
if [ -z "$test_base" ] && [ -n "${RBSTEMSPLUS_TEST_BASE_URL:-}${RBSTEMSPLUS_TEST_KEY:-}${RBSTEMSPLUS_SCREENSHOTS:-}${RBSTEMS_DUMP_DIR:-}${RBSTEMSPLUS_TEST_BROKEN_SELFTEST:-}${RBSTEMSPLUS_TEST_WIN_ORT_SHA256:-}" ]; then
  echo "build.sh: RBSTEMSPLUS_TEST_BASE_URL, RBSTEMSPLUS_TEST_KEY, RBSTEMSPLUS_SCREENSHOTS, RBSTEMS_DUMP_DIR, RBSTEMSPLUS_TEST_BROKEN_SELFTEST or RBSTEMSPLUS_TEST_WIN_ORT_SHA256 is set: unset it for a release, or use --test URL" >&2; exit 1
fi
# the Windows TEST server only for a TEST build's Windows files
[ -z "$win_test" ] || [ -n "$test_base" ] || usage
# the broken self-test app only when asked for by name (review Q30)
if [ -n "${RBSTEMSPLUS_TEST_BROKEN_SELFTEST:-}" ] && [ ${#broken_flag[@]} -eq 0 ]; then
  echo "build.sh: RBSTEMSPLUS_TEST_BROKEN_SELFTEST is set: unset it, or pass --broken-selftest-test (tests/mac/swap-selftest.sh only)" >&2; exit 1
fi
# the app's own version comes from VERSION, or from the plan for a carried app
[ -z "${RBSTEMSPLUS_APP_VERSION:-}" ] || { echo "build.sh: RBSTEMSPLUS_APP_VERSION is set: unset it (a carried app comes from --plan)" >&2; exit 1; }
# the helper's research build (helper/build.sh) is never in any app, test builds included
[ -z "${RBSP_RESEARCH:-}" ] || { echo "build.sh: RBSP_RESEARCH is set: unset it (research builds of the helper are never packaged)" >&2; exit 1; }
version="$(tr -d '[:space:]' < "$root/VERSION")"

die() { echo "build.sh: $*" >&2; exit 1; }

# A release is built only from a commit, which payload.json names (scripts/release.sh checks it is
# the commit it releases). A test build may have changes (its commit then ends in "-dirty").
commit="$(git -C "$root" rev-parse HEAD)" || die "not a git checkout"
changes="$(git -C "$root" status --porcelain)" || die "git status failed"
if [ -z "$test_base" ]; then
  [ -z "$changes" ] || die "uncommitted or untracked files (git status): a release is built only from a commit"
  for k in release backup; do
    [ -f "$root/keys/$k.pub.pem" ] \
      || die "keys/$k.pub.pem is missing: a release needs both public signing keys (scripts/make-signing-keys.sh, docs/RELEASING.md)"
  done
elif [ -n "$changes" ]; then
  commit="$commit-dirty"
fi

# --- edit for each release -------------------------------------------------------------------
# The model is the same export for every release; its SHA-256 is also ourModel in
# app/Sources/Paths.swift (and names the cache folder). A new export means a new hash in both.
model_sha256="0cc50d877629fda906562d08236b32a3e9934e64c4ea8f3ab7fc43f40976e772"
model_size=308572524
# The SHA-256s of every earlier model export (space-separated): payload.json's previous_models,
# so RB Stems Plus still knows them as its own and never saves one as rekordbox's.
previous_models=""
# payload.json's "compatible" block: rekordbox versions Stems Cache may be installed on, the
# ONNX Runtime the bridge expects, the STEMS Engine versions our Demucs v4 model fits.
compat_rekordbox="7.2.17 7.2.18 7.2.19 7.2.20"
compat_ort_prefix="1.18."
compat_stems_engine="0002"
# The windows block's (release.md §1.2): the rekordbox versions Stems Cache may be installed on
# under Windows, the SHA-256 of AlphaTheta's onnxruntime.dll in each (the bridge's real library:
# the VM's C:\rbtest\pristine\onnxruntime.dll, read by tests/harness/win's setup), the STEMS
# Engine versions, and the builds the resampler proof covers ("PRODUCT_VERSION:REKORDBOX_SHA256",
# none until helper.md T11 proves one). A --windows build refuses an empty ort list; a TEST build
# may take RBSTEMSPLUS_TEST_WIN_ORT_SHA256 instead (another copy's, for a version not listed yet).
# 7.2.19's onnxruntime.dll: the same SHA-256 on the VM and the owner's PC (2026-11-16).
compat_win_rekordbox="7.2.19"
compat_win_ort_sha256="e7a702e13bd3e487705204d518c96f1f116aa25b0ac584741838e20bc1ff6e9f"
compat_win_stems_engine="0002"
resampler_proved=""
# ---------------------------------------------------------------------------------------------

model_src="${RBSTEMSPLUS_MODEL:-$root/../Library Organizer/experiments/rekordbox-stems/runs/htdemucs_rb_v4xt.onnx}"
app_zip="rbstemsplus-app.zip"
bridge="libonnxruntime.1.18.0.dylib"
model="stemsplus-model.onnx"

sha256() { shasum -a 256 "$1" | cut -d' ' -f1; }
size() { stat -f %z "$1"; }

[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "VERSION must be x.y.z, not '$version'"

# The plan (scripts/release.sh --plan): each Mac file new (built here, from this commit) or carried
# (the previous release's bytes, its version and commit). Lines: "plan 1", "commit SHA" (the
# release commit), "version X.Y.Z", "previous TAG PAYLOAD_SHA256" (or "previous none"),
# "mac.app new" or "mac.app carry VERSION COMMIT ZIP_SHA256", "mac.bridge new" or "mac.bridge
# carry COMMIT SHA256", "windows.app none" and "windows.bridge none" (no Windows files) or the
# Windows lines of release.md §1.3 "As built (T13)": "windows.app new|carry VERSION ZIP_SHA256",
# "windows.stub.x64", "windows.stub.arm64" and "windows.admin" each "new|carry SHA256",
# "windows.bridge new|carry SHA256" (scripts/windows-assets.sh reads and checks them); "#" lines are
# comments.
app_plan=new; app_carry_version=""; app_carry_commit=""; app_carry_sha=""
bridge_plan=new; bridge_carry_commit=""; bridge_carry_sha=""
if [ -n "$plan" ]; then
  [ -f "$plan" ] || die "no plan at $plan"
  plan_format=""; plan_commit=""; plan_version=""; prev_tag=""; seen=""; plan_windows=""; plan_wfiles=""; plan_wcarry=""
  while read -r k a b c d rest; do
    case "$k" in ''|'#'*) continue ;; esac
    [ -z "${rest:-}" ] || die "$plan: too many fields on the line $k"
    case " $seen " in *" $k "*) die "$plan: $k twice" ;; esac
    seen="$seen $k"
    case "$k $a" in
      "plan "*) plan_format="$a" ;;
      "commit "*) plan_commit="$a" ;;
      "version "*) plan_version="$a" ;;
      "previous "*) prev_tag="$a" ;;
      "mac.app new"|"mac.bridge new") [ -z "$b" ] || die "$plan: $k new takes nothing more" ;;
      "mac.app carry") app_plan=carry; app_carry_version="$b"; app_carry_commit="$c"; app_carry_sha="$d" ;;
      "mac.bridge carry") bridge_plan=carry; bridge_carry_commit="$b"; bridge_carry_sha="$c"; [ -z "$d" ] || die "$plan: mac.bridge carry takes a commit and a SHA-256" ;;
      "windows.app none"|"windows.bridge none") ;;
      "windows.app new"|"windows.bridge new"|"windows.app carry"|"windows.bridge carry")
        plan_windows="$plan_windows $k"; [ "$a" = new ] || plan_wcarry=1 ;;
      "windows.stub.x64 "*|"windows.stub.arm64 "*|"windows.admin "*)
        plan_wfiles="$plan_wfiles $k"; [ "$a" != carry ] || plan_wcarry=1 ;;
      "windows."*) die "$plan: the line '$k $a' isn't a plan's" ;;
      *) die "$plan: the line '$k $a' isn't a plan's" ;;
    esac
  done < "$plan"
  for k in plan commit version previous mac.app mac.bridge windows.app windows.bridge; do
    case " $seen " in *" $k "*) ;; *) die "$plan has no $k line" ;; esac
  done
  [ "$plan_format" = 1 ] || die "$plan: plan format '$plan_format', not 1"
  # the Windows files go together: both new or carried (the windows block, with the stubs' and the
  # worker's lines), or both none (no stub or worker line then)
  case "$plan_windows" in
    "") [ -z "$plan_wfiles" ] || die "$plan: windows.stub/windows.admin lines without Windows files" ;;
    " windows.app windows.bridge"|" windows.bridge windows.app") windows=1 ;;
    *) die "$plan: windows.app and windows.bridge are both new or carried, or both none" ;;
  esac
  [ "$plan_version" = "$version" ] || die "$plan is for version $plan_version, VERSION is $version"
  [[ "$plan_commit" =~ ^[0-9a-f]{40}$ ]] || die "$plan: the commit '$plan_commit' isn't a commit"
  if [ -n "$test_base" ]; then
    # a TEST build in the plan's shape is the plan's commit; the release builds a public copy of it
    [ "$plan_commit" = "$commit" ] || die "$plan is for $plan_commit, not this commit ($commit): run scripts/release.sh --plan here"
  fi
  if [ "$app_plan" = carry ]; then
    [[ "$app_carry_version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ && "$app_carry_commit" =~ ^[0-9a-f]{40}$ && "$app_carry_sha" =~ ^[0-9a-f]{64}$ ]] \
      || die "$plan: mac.app carry needs a version, a commit and a SHA-256"
  fi
  if [ "$bridge_plan" = carry ]; then
    [[ "$bridge_carry_commit" =~ ^[0-9a-f]{40}$ && "$bridge_carry_sha" =~ ^[0-9a-f]{64}$ ]] || die "$plan: mac.bridge carry needs a commit and a SHA-256"
  fi
  if [ "$app_plan$bridge_plan" != newnew ] || [ -n "$plan_wcarry" ]; then
    [ "$prev_tag" != none ] && [[ "$prev_tag" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "$plan carries a file, but names no previous release"
    if [ -z "$carry" ]; then
      # the previous release's TEST build, from the gate archive (its checksums first)
      archive="${GATES_ARCHIVE:-$root/gates-archive}"
      [ -f "$archive/SHA256SUMS" ] || die "no gate archive at $archive (or no SHA256SUMS in it): the previous release's TEST build is kept there"
      (cd "$archive" && shasum -a 256 -c --quiet SHA256SUMS) || die "the gate archive doesn't match its SHA256SUMS: stop (never rebuild what it keeps)"
      carry="$archive/tag-$prev_tag"
    fi
    [ -d "$carry" ] || die "no folder $carry with the previous release's files ($prev_tag)"
    carry="$(cd "$carry" && pwd)"
  elif [ -n "$test_base" ] && [ "$prev_tag" != none ]; then
    # a TEST build that carries nothing is G3's too: tests/mac/g3.sh updates the gate archive's
    # TEST builds of earlier releases to it, and they trust only the test key archived with them
    # (one pair in every tag-*/test-keys/), so it is signed with that key, not build/test-keys/
    archive="${GATES_ARCHIVE:-$root/gates-archive}"
    [ -f "$archive/SHA256SUMS" ] || die "no gate archive at $archive (or no SHA256SUMS in it): a TEST build in a plan's shape is signed with its test key (G3 updates its TEST builds)"
    (cd "$archive" && shasum -a 256 -c --quiet SHA256SUMS) || die "the gate archive doesn't match its SHA256SUMS: stop (never rebuild what it keeps)"
    archive_keys="$(cd "$archive" && pwd)/tag-$prev_tag/test-keys"
    [ -f "$archive_keys/test-key.pem" ] && [ -f "$archive_keys/test.pub.pem" ] \
      || die "no test key pair in $archive_keys (test-key.pem, test.pub.pem): the previous release's TEST build is archived with the key that signed it"
    for k in "$archive"/tag-*/test-keys/test.pub.pem; do
      cmp -s "$k" "$archive_keys/test.pub.pem" || die "$k isn't $prev_tag's test key: the gate archive's TEST builds don't trust one key, so G3 can't update them all to this build"
    done
  fi
fi
for h in $previous_models; do [[ "$h" =~ ^[0-9a-f]{64}$ ]] || die "previous_models: '$h' isn't a SHA-256"; done
if [ -n "$windows" ]; then
  [ -z "$test_base" ] || [ -n "$win_test" ] || die "--test with --windows needs --win-test WURL: the Windows TEST app's https:// server"
  [ -z "$win_test" ] || [[ "$win_test" =~ ^https://[A-Za-z0-9.-]+(:[0-9]+)?$ ]] || die "--win-test must be https://HOST[:PORT], not '$win_test'"
  win_ort="$compat_win_ort_sha256"
  [ -z "$test_base" ] || win_ort="${RBSTEMSPLUS_TEST_WIN_ORT_SHA256:-$win_ort}"
  [ -n "$win_ort" ] || die "compat_win_ort_sha256 is empty: AlphaTheta's onnxruntime.dll SHA-256 for each listed rekordbox version (release.md §1.2)"
  for h in $win_ort; do [[ "$h" =~ ^[0-9a-f]{64}$ ]] || die "compat_win_ort_sha256: '$h' isn't a SHA-256"; done
  for r in $resampler_proved; do [[ "$r" =~ ^[0-9]+(\.[0-9]+)+:[0-9a-f]{64}$ ]] || die "resampler_proved: '$r' isn't PRODUCT_VERSION:SHA256"; done
fi
grep -qs "$model_sha256" "$root"/app/Sources/*.swift \
  || die "the model SHA-256 here isn't in app/Sources (ourModel in Paths.swift); make them agree"
mkdir -p "$dist"

# A public key file's DER in base64 (one line), as the app and the bootstrap hold it.
pubkey_b64() {
  local text
  text="$(/usr/bin/openssl ec -pubin -in "$1" -noout -text 2>/dev/null)" && grep -q 'ASN1 OID: prime256v1' <<< "$text" || return 1
  /usr/bin/openssl ec -pubin -in "$1" -outform DER 2>/dev/null | /usr/bin/base64 | tr -d '\n'
}
echo "== signing keys"
test_keys="$root/build/test-keys"
test_b64=""
if [ -n "$test_base" ] && { [ "$app_plan$bridge_plan" != newnew ] || [ -n "${plan_wcarry:-}" ]; }; then
  # a TEST build that carries a file: the key pair archived with the previous release's TEST
  # build (its app trusts only that key), never build/test-keys/
  test_keys="$carry/test-keys"
  [ -f "$test_keys/test-key.pem" ] && [ -f "$test_keys/test.pub.pem" ] \
    || die "no test key pair in $test_keys (test-key.pem, test.pub.pem): the previous release's TEST build is archived with the key that signed it"
  echo "the test key of $carry (the carried TEST build's)"
  test_b64="$(pubkey_b64 "$test_keys/test.pub.pem")" || die "$test_keys/test.pub.pem isn't a P-256 public key"
  trusted="$test_b64"
elif [ -n "$test_base" ] && [ -n "$archive_keys" ]; then
  # a TEST build in a plan's shape that carries nothing: the gate archive's test key (above)
  test_keys="$archive_keys"
  echo "the test key of the gate archive's TEST builds ($test_keys)"
  test_b64="$(pubkey_b64 "$test_keys/test.pub.pem")" || die "$test_keys/test.pub.pem isn't a P-256 public key"
  trusted="$test_b64"
elif [ -n "$test_base" ]; then
  # the test key: made once, kept in build/ (never in git), trusted only by test builds
  if [ ! -f "$test_keys/test-key.pem" ] || [ ! -f "$test_keys/test.pub.pem" ]; then
    mkdir -p "$test_keys"
    (umask 077 && /usr/bin/openssl ecparam -name prime256v1 -genkey -noout -out "$test_keys/test-key.pem") \
      || die "couldn't make the test key"
    /usr/bin/openssl ec -in "$test_keys/test-key.pem" -pubout -out "$test_keys/test.pub.pem" 2>/dev/null \
      || die "couldn't make the test public key"
    echo "made a test signing key in build/test-keys/"
  fi
  test_b64="$(pubkey_b64 "$test_keys/test.pub.pem")" || die "build/test-keys/test.pub.pem isn't a P-256 public key"
  trusted="$test_b64"
else
  release_b64="$(pubkey_b64 "$root/keys/release.pub.pem")" || die "keys/release.pub.pem isn't a P-256 public key"
  backup_b64="$(pubkey_b64 "$root/keys/backup.pub.pem")" || die "keys/backup.pub.pem isn't a P-256 public key"
  [ "$release_b64" != "$backup_b64" ] || die "keys/release.pub.pem and keys/backup.pub.pem are the same key"
  [ ! -f "$test_keys/test.pub.pem" ] || test_b64="$(pubkey_b64 "$test_keys/test.pub.pem")" || test_b64=""
  trusted="$release_b64 $backup_b64"
fi

echo "== app"
if [ "$app_plan" = new ]; then
  RBSTEMSPLUS_TEST_BASE_URL="$test_base" RBSTEMSPLUS_TEST_KEY="${test_base:+$test_keys/test.pub.pem}" \
    "$root/scripts/build-app.sh" "$dist" ${broken_flag[@]+"${broken_flag[@]}"} || die "the app didn't build (see above)"
  [ -f "$dist/$app_zip" ] || die "scripts/build-app.sh made no $dist/$app_zip"
  app_bundle="$root/build/app/RB Stems Plus.app"
  app_version="$version"; app_commit="$commit"
else
  # carried: the previous release's zip byte for byte (a TEST build: the previous release's TEST
  # zip, which its own payload.json names), never rebuilt
  [ ${#broken_flag[@]} -eq 0 ] || die "--broken-selftest-test needs a new app, and the plan carries the app"
  [ -f "$carry/$app_zip" ] || die "no $app_zip in $carry"
  if [ -n "$test_base" ]; then
    [ -f "$carry/payload.json" ] || die "no payload.json in $carry (the previous TEST build's)"
    want="$(plutil -extract app.sha256 raw -o - "$carry/payload.json" 2>/dev/null)" || die "$carry/payload.json has no app.sha256"
    [ "$(plutil -extract app.version raw -o - "$carry/payload.json" 2>/dev/null)" = "$app_carry_version" ] \
      || die "$carry/payload.json's app isn't version $app_carry_version, the plan's"
  else
    want="$app_carry_sha"
  fi
  [ "$(sha256 "$carry/$app_zip")" = "$want" ] || die "$carry/$app_zip doesn't have the SHA-256 $want"
  rm -f "$dist/$app_zip"
  cp "$carry/$app_zip" "$dist/$app_zip"
  rm -rf "$root/build/carried-app"; mkdir -p "$root/build/carried-app"
  ditto -x -k "$dist/$app_zip" "$root/build/carried-app" || die "couldn't unpack the carried $app_zip"
  app_bundle="$root/build/carried-app/RB Stems Plus.app"
  [ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$app_bundle/Contents/Info.plist" 2>/dev/null)" = "$app_carry_version" ] \
    || die "the carried app isn't version $app_carry_version"
  app_version="$app_carry_version"; app_commit="$app_carry_commit"
  echo "carried the app $app_version (built from $app_commit) from $carry"
fi
app_exe="$app_bundle/Contents/MacOS/RB Stems Plus"
if [ -n "$test_base" ]; then
  grep -aq "TEST BUILD" "$app_exe" || die "the test app doesn't say TEST BUILD"
else
  ! grep -aq "TEST BUILD" "$app_exe" || die "the app is a TEST BUILD: not packaging it as a release"
  # the harness's probe compiles into test builds only (a check: the bytes don't change)
  "$root/tests/harness/check-release.sh" "$app_bundle" || die "the app carries the harness's probe: not packaging it as a release"
  [ -z "$test_b64" ] || ! grep -aqF "$test_b64" "$app_exe" || die "the app trusts the test key: not packaging it as a release"
fi
for k in $trusted; do grep -aqF "$k" "$app_exe" || die "the app doesn't hold the signing key $k"; done

echo "== bridge"
if [ "$bridge_plan" = new ]; then
  rm -rf "$root/build/bridge"
  "$root/bridge/build.sh" "$root/build/bridge" || die "the bridge didn't build (see above)"
  [ -f "$root/build/bridge/$bridge" ] || die "bridge/build.sh made no build/bridge/$bridge"
  rm -f "$dist/$bridge"
  cp "$root/build/bridge/$bridge" "$dist/$bridge"
  bridge_commit="$commit"
else
  # carried: the previous release's bridge byte for byte (a TEST build's bridge is the release's)
  [ -f "$carry/$bridge" ] || die "no $bridge in $carry"
  [ "$(sha256 "$carry/$bridge")" = "$bridge_carry_sha" ] || die "$carry/$bridge doesn't have the SHA-256 $bridge_carry_sha, the plan's"
  rm -f "$dist/$bridge"
  cp "$carry/$bridge" "$dist/$bridge"
  bridge_commit="$bridge_carry_commit"
  echo "carried the bridge (built from $bridge_commit) from $carry"
fi
archs="$(lipo -archs "$dist/$bridge")"
[[ " $archs " == *" arm64 "* && " $archs " == *" x86_64 "* ]] \
  || die "the bridge must be universal (arm64 x86_64), it is: $archs"
grep -aq "rbstems bridge: " "$dist/$bridge" || die "the bridge lacks its marker string"
deps="$(otool -L "$dist/$bridge")" || die "otool couldn't read the bridge"
[[ "$deps" != *"/opt/homebrew"* && "$deps" != *"/usr/local/"* ]] \
  || die "the bridge links to Homebrew; it must link libFLAC statically"

echo "== Windows"
win_zip="rbstemsplus-windows.zip"
win_bridge="rbstemsplus-bridge-win-x64.dll"
win_assets=("$win_zip" "$win_bridge" bootstrap.ps1 NOTICE-windows.txt)
for f in "${win_assets[@]}" windows.env windows-release.sha256 test-ca.pem; do rm -f "$dist/$f"; done
if [ -n "$windows" ]; then
  # the files new or carried as the plan says (all new without one), bootstrap.ps1 with this
  # build's keys and version, NOTICE-windows.txt, and windows.env (scripts/windows-assets.sh)
  wa=(--version "$version" --keys "$trusted")
  [ -z "$plan" ] || wa+=(--plan "$plan")
  [ -z "$carry" ] || wa+=(--carry "$carry")
  [ -z "$test_base" ] || wa+=(--test "$win_test" --test-key "$test_keys/test.pub.pem")
  "$root/scripts/windows-assets.sh" "${wa[@]}" "$dist" || die "the Windows files didn't build or carry (see above)"
  # a release's bootstrap.ps1 never trusts the test key (its PEs compile in only keys/*.pub.pem)
  [ -n "$test_base" ] || [ -z "$test_b64" ] || ! grep -qF "$test_b64" "$dist/bootstrap.ps1" || die "bootstrap.ps1 trusts the test key"
  if [ -n "$test_base" ]; then
    # the Windows TEST server's TLS: a test CA and its server certificate for WURL's host
    # (build/test-keys/win-tls/ always, even when the test key is the carry's: never in the sealed
    # carry archive, and never a release's), the CA's certificate beside the files for the VM
    tls="$root/build/test-keys/win-tls"; host="${win_test#https://}"; host="${host%%:*}"
    if [[ "$host" =~ ^[0-9.]+$ ]]; then san="IP:$host"; nc="IP:$host/255.255.255.255"; else san="DNS:$host"; nc="DNS:$host"; fi
    fresh() { /usr/bin/openssl x509 -checkend 2592000 -noout -in "$1" > /dev/null 2>&1; }   # 30 days left
    catext="$(/usr/bin/openssl x509 -in "$tls/ca.pem" -noout -text 2>/dev/null || true)"
    # the CA: remade only for another host, a CA running out, or one made before its constraints
    if [ ! -f "$tls/ca-key.pem" ] || [ "$(cat "$tls/host" 2>/dev/null)" != "$host" ] || ! fresh "$tls/ca.pem" \
       || ! printf '%s\n' "$catext" | grep -q 'X509v3 Name Constraints: critical' \
       || ! printf '%s\n' "$catext" | grep -q 'CA:TRUE, pathlen:0'; then
      rm -rf "$tls"; mkdir -p "$tls"
      # named-curve P-256 keys (ecparam: LibreSSL's -newkey ec writes explicit parameters, which
      # strict verifiers refuse), SHA-256 signatures, a CA marked as one that signs no CA and no
      # name but WURL's host, the server's SAN and AKI
      printf '[req]\ndistinguished_name=dn\n[dn]\n[ca]\nbasicConstraints=critical,CA:TRUE,pathlen:0\nnameConstraints=critical,permitted;%s\nkeyUsage=critical,keyCertSign,cRLSign\nsubjectKeyIdentifier=hash\n' "$nc" > "$tls/ca.cnf"
      (umask 077 && /usr/bin/openssl ecparam -name prime256v1 -genkey -noout -out "$tls/ca-key.pem" \
       && /usr/bin/openssl req -x509 -sha256 -new -key "$tls/ca-key.pem" -days 3650 -subj "/CN=RB Stems Plus TEST CA" \
            -config "$tls/ca.cnf" -extensions ca -out "$tls/ca.pem") || die "couldn't make the Windows TEST server's TLS CA"
      printf '%s\n' "$host" > "$tls/host"
      echo "made the Windows TEST server's TLS CA for $host in build/test-keys/win-tls/"
    fi
    # the server's certificate: remade with the CA, or when under 30 days are left
    if [ ! -f "$tls/server.pem" ] || ! fresh "$tls/server.pem" \
       || ! /usr/bin/openssl verify -CAfile "$tls/ca.pem" "$tls/server.pem" > /dev/null 2>&1; then
      printf 'subjectAltName=%s\nextendedKeyUsage=serverAuth\nbasicConstraints=CA:FALSE\nkeyUsage=critical,digitalSignature\nauthorityKeyIdentifier=keyid\n' "$san" > "$tls/ext"
      (umask 077 && /usr/bin/openssl ecparam -name prime256v1 -genkey -noout -out "$tls/server-key.pem" \
       && /usr/bin/openssl req -new -key "$tls/server-key.pem" -subj "/CN=$host" -config "$tls/ca.cnf" -out "$tls/server.csr" \
       && /usr/bin/openssl x509 -req -sha256 -in "$tls/server.csr" -CA "$tls/ca.pem" -CAkey "$tls/ca-key.pem" -CAcreateserial \
            -days 825 -extfile "$tls/ext" -out "$tls/server.pem" 2>/dev/null \
       && /usr/bin/openssl verify -CAfile "$tls/ca.pem" "$tls/server.pem" > /dev/null) || die "couldn't make the Windows TEST server's TLS certificate"
      echo "made the Windows TEST server's TLS certificate for $host in build/test-keys/win-tls/"
    fi
    cp "$tls/ca.pem" "$dist/test-ca.pem"
  fi
  # shellcheck disable=SC1091
  . "$dist/windows.env"
  [[ "$win_app_version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "windows.env's win_app_version isn't a version"
  for v in win_zip_sha256 win_admin_sha256 win_stub_x64_sha256 win_stub_arm64_sha256 win_bridge_sha256; do
    [[ "${!v}" =~ ^[0-9a-f]{64}$ ]] || die "windows.env's $v isn't a SHA-256"
  done
  rm -f "$dist/windows.env"
else
  echo "no Windows files (no --windows, no windows block)"
fi

echo "== model"
if [ -f "$dist/$model" ] && [ "$(size "$dist/$model")" = "$model_size" ] \
   && [ "$(sha256 "$dist/$model")" = "$model_sha256" ]; then
  echo "$dist/$model is already the right model"
elif [ -n "$test_base" ] && [ -f "$root/dist/$model" ] && [ "$(size "$root/dist/$model")" = "$model_size" ] \
   && [ "$(sha256 "$root/dist/$model")" = "$model_sha256" ]; then
  ln -f "$root/dist/$model" "$dist/$model" 2>/dev/null || cp "$root/dist/$model" "$dist/$model"
  echo "took the model from dist/"
elif [ -f "$model_src" ]; then
  [ "$(size "$model_src")" = "$model_size" ] || die "$model_src is not $model_size bytes"
  [ "$(sha256 "$model_src")" = "$model_sha256" ] || die "$model_src doesn't have SHA-256 $model_sha256"
  cp "$model_src" "$dist/$model.part" && mv "$dist/$model.part" "$dist/$model"
  echo "copied the model from $model_src"
else
  die "no model: put it at dist/$model (SHA-256 $model_sha256) or set RBSTEMSPLUS_MODEL"
fi

echo "== bootstrap, NOTICE, LICENSE"
bash -n "$root/scripts/bootstrap.sh"
# the same keys as the app, written into its one SIGNING_KEYS="" line
[ "$(grep -c '^SIGNING_KEYS=""$' "$root/scripts/bootstrap.sh")" = 1 ] || die "scripts/bootstrap.sh needs exactly one SIGNING_KEYS=\"\" line"
sed "s|^SIGNING_KEYS=\"\"\$|SIGNING_KEYS=\"$trusted\"|" "$root/scripts/bootstrap.sh" > "$dist/bootstrap.sh"
chmod 755 "$dist/bootstrap.sh"
grep -qxF "SIGNING_KEYS=\"$trusted\"" "$dist/bootstrap.sh" || die "the keys didn't go into bootstrap.sh"
# its version, the oldest payload.json it installs (the app has it compiled in), into its one
# PAYLOAD_MINIMUM="" line
[ "$(grep -c '^PAYLOAD_MINIMUM=""$' "$dist/bootstrap.sh")" = 1 ] || die "scripts/bootstrap.sh needs exactly one PAYLOAD_MINIMUM=\"\" line"
sed -i '' "s|^PAYLOAD_MINIMUM=\"\"\$|PAYLOAD_MINIMUM=\"$version\"|" "$dist/bootstrap.sh"
grep -qxF "PAYLOAD_MINIMUM=\"$version\"" "$dist/bootstrap.sh" || die "the version didn't go into bootstrap.sh"
[ -n "$test_base" ] || [ -z "$test_b64" ] || ! grep -qF "$test_b64" "$dist/bootstrap.sh" || die "bootstrap.sh trusts the test key"
cp "$root/NOTICE" "$root/LICENSE" "$dist/"

echo "== payload.json"
json_list() { local out="" v; for v in $1; do out+="${out:+, }\"$v\""; done; echo "[$out]"; }
windows_json=""
if [ -n "$windows" ]; then
  proved=""; for r in $resampler_proved; do proved+="${proved:+, }{\"product_version\": \"${r%%:*}\", \"rekordbox_sha256\": \"${r#*:}\"}"; done
  windows_json=$(cat <<EOF
,
  "windows": {
    "app": {"version": "$win_app_version", "zip": "$win_zip", "sha256": "$win_zip_sha256",
            "admin_sha256": "$win_admin_sha256",
            "stub_sha256": {"x64": "$win_stub_x64_sha256", "arm64": "$win_stub_arm64_sha256"}},
    "bridge": {"file": "$win_bridge", "sha256": "$win_bridge_sha256"},
    "compatible": {
      "rekordbox": $(json_list "$compat_win_rekordbox"),
      "ort_sha256": $(json_list "$win_ort"),
      "stems_engine": $(json_list "$compat_win_stems_engine")
    },
    "resampler_proved": [$proved]
  }
EOF
)
fi
cat > "$dist/payload.json" <<EOF
{
  "payload_version": "$version",
  "commit": "$commit",
  "app": {"version": "$app_version", "commit": "$app_commit", "zip": "$app_zip", "sha256": "$(sha256 "$dist/$app_zip")"},
  "bridge": {"file": "$bridge", "commit": "$bridge_commit", "sha256": "$(sha256 "$dist/$bridge")"},
  "model": {"file": "$model", "sha256": "$model_sha256", "size": $model_size},
  "previous_models": $(json_list "$previous_models"),
  "compatible": {
    "rekordbox": $(json_list "$compat_rekordbox"),
    "ort_version_prefix": "$compat_ort_prefix",
    "stems_engine": $(json_list "$compat_stems_engine")
  }$windows_json
}
EOF
plutil -convert xml1 -o /dev/null "$dist/payload.json" || die "payload.json isn't valid JSON"

echo "== SHA256SUMS"
win_files=(); [ -z "$windows" ] || win_files=("${win_assets[@]}")
(cd "$dist" && shasum -a 256 "$app_zip" "$bridge" "$model" ${win_files[@]+"${win_files[@]}"} bootstrap.sh NOTICE LICENSE payload.json > SHA256SUMS)
cat "$dist/SHA256SUMS"

rm -f "$dist/payload.json.sig"
if [ -n "$test_base" ]; then
  echo "== payload.json.sig (the test key)"
  /usr/bin/openssl dgst -sha256 -sign "$test_keys/test-key.pem" -out "$dist/payload.json.sig" "$dist/payload.json" \
    || die "couldn't sign payload.json with the test key"
  /usr/bin/openssl dgst -sha256 -verify "$test_keys/test.pub.pem" -signature "$dist/payload.json.sig" "$dist/payload.json" > /dev/null \
    || die "the test signature doesn't verify"
fi
echo "Built RB Stems Plus $version ($commit) into $dist${test_base:+ (TEST BUILD, downloads from $test_base, signed with the test key)}"
[ -z "$plan" ] || echo "In the shape of $plan: the Mac app $app_plan ($app_version, from $app_commit), the Mac bridge $bridge_plan (from $bridge_commit)"
[ -z "$windows" ] || echo "With the Windows files $win_app_version${win_test:+ (TEST BUILD, downloads from $win_test)}"
[ -n "$test_base" ] || echo "Not signed: scripts/release.sh signs and uploads a release (docs/RELEASING.md)."


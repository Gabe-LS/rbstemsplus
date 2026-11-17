#!/bin/bash
# The Windows release assets in a plan's shape (release.md §1.3 "As built (T13)", T12), for
# scripts/build.sh: into DIST, rbstemsplus-windows.zip, rbstemsplus-bridge-win-x64.dll,
# bootstrap.ps1 and NOTICE-windows.txt, and DIST/windows.env with what payload.json's windows
# block names, each SHA-256 of the very bytes shipped (win_app_version, win_zip_sha256,
# win_admin_sha256, win_stub_x64_sha256, win_stub_arm64_sha256, win_bridge_sha256).
#
#   scripts/windows-assets.sh --version V --keys "KEY..." [--plan FILE [--carry DIR]]
#                             [--test URL --test-key PEM] DIST
#
# --version: this release's VERSION (bootstrap.ps1's PayloadMinimum, and the app's when new).
# --keys: the public keys payload.json is signed with, base64 DER, space-separated (bootstrap.ps1's
# SigningKeys, the same as the Mac app's and bootstrap.sh's).
# --plan: release.sh's plan; its Windows lines, each new or carried:
#   windows.app new | carry VERSION ZIP_SHA256        the zip, byte for byte the previous one's
#   windows.stub.x64 | windows.stub.arm64 | windows.admin new | carry SHA256
#                                                     in a new zip, the previous zip's entry
#                                                     (stub/x64, stub/arm64, admin RB Stems Plus.exe)
#                                                     through build-windows.sh --carry-file (R4.I83);
#                                                     a carried zip carries all three (efdc278's
#                                                     plan_lines write them as carry, never new),
#                                                     each SHA-256 the carried zip's own entry's
#   windows.bridge new | carry SHA256                 the bridge, the previous one's bytes
# Without a plan (build.sh --windows), everything is new. A carried file comes from DIR with the
# payload.json that names it: for a release, release.sh's verified copy of the previous release;
# for a TEST build, the archived TEST build. Every carried file's SHA-256 is checked against that
# payload.json and against the plan, before anything is built. Only two of a TEST archive's files
# are its own, not the plan's: the zip (its app is a TEST build) and the worker in it (it trusts
# the test key); its stubs and its bridge are the release's bytes, the plan's (BP2). A carried stub or
# worker is taken out of the previous zip into a fresh folder of its own (mktemp -d, never under
# build/windows*, which build-windows.sh empties: TB5) and found again in the shipped zip, so the
# windows.env computed from the shipped files names the plan's SHA-256 for every carried file.
# --test URL --test-key PEM: the Windows TEST build (scripts/build-windows.sh --test). With a new
# worker, DIST/windows-release.sha256 too ("admin SHA256": the release worker the same commit
# builds, which release.sh holds the release's to; TT1). None otherwise.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
die() { echo "windows-assets.sh: $*" >&2; exit 1; }
usage() { echo "usage: $0 --version V --keys \"KEY...\" [--plan FILE [--carry DIR]] [--test URL --test-key PEM] DIST" >&2; exit 2; }
version="" keys="" plan="" carry="" test_base="" test_key=""
while [ $# -gt 1 ]; do
  case "$1" in
    --version) version="$2"; shift 2 ;;
    --keys) keys="$2"; shift 2 ;;
    --plan) plan="$2"; shift 2 ;;
    --carry) carry="$2"; shift 2 ;;
    --test) test_base="$2"; shift 2 ;;
    --test-key) test_key="$2"; shift 2 ;;
    *) usage ;;
  esac
done
[ $# -eq 1 ] || usage
dist="$1"
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "--version must be x.y.z, not '$version'"
[[ "$keys" =~ ^[A-Za-z0-9+/=]+( [A-Za-z0-9+/=]+)*$ ]] || die "--keys must be base64 keys, space-separated"
[ -z "$test_base$test_key" ] || { [ -n "$test_base" ] && [ -n "$test_key" ]; } || usage
[ -z "$carry" ] || [ -n "$plan" ] || usage
mkdir -p "$dist"; dist="$(cd "$dist" && pwd)"
zip="rbstemsplus-windows.zip" bridge="rbstemsplus-bridge-win-x64.dll"
sha256() { shasum -a 256 "$1" | cut -d' ' -f1; }
# an entry's SHA-256 in a zip (none: empty)
entry_sha() { python3 -I -c 'import hashlib, sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
print(hashlib.sha256(z.read(sys.argv[2])).hexdigest() if sys.argv[2] in z.namelist() else "")' "$1" "$2"; }
entry_get() { python3 -I -c 'import sys, zipfile; open(sys.argv[3], "wb").write(zipfile.ZipFile(sys.argv[1]).read(sys.argv[2]))' "$1" "$2" "$3"; }

# ---------------------------------------------------------------- the plan's Windows lines
app=new app_v="" app_sha="" br=new br_sha=""
declare -a ent_key=(stub.x64 stub.arm64 admin)
declare -a ent_name=("stub/x64/RB Stems Plus.exe" "stub/arm64/RB Stems Plus.exe" "admin/RB Stems Plus.exe")
ent_how=(new new new) ent_sha=("" "" "")
if [ -n "$plan" ]; then
  [ -f "$plan" ] || die "no plan at $plan"
  seen=""
  while read -r k a b c rest; do
    case "$k" in windows.*) ;; *) continue ;; esac
    [ -z "${rest:-}" ] || die "$plan: too many fields on the line $k"
    case " $seen " in *" $k "*) die "$plan: $k twice" ;; esac
    seen="$seen $k"
    case "$k $a" in
      "windows.app new"|"windows.bridge new") [ -z "$b" ] || die "$plan: $k new takes nothing more" ;;
      "windows.app carry") app=carry; app_v="$b"; app_sha="$c"
        [[ "$app_v" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ && "$app_sha" =~ ^[0-9a-f]{64}$ ]] || die "$plan: windows.app carry takes a version and a SHA-256" ;;
      "windows.bridge carry") br=carry; br_sha="$b"
        [[ "$br_sha" =~ ^[0-9a-f]{64}$ && -z "$c" ]] || die "$plan: windows.bridge carry takes a SHA-256" ;;
      "windows.stub.x64 "*|"windows.stub.arm64 "*|"windows.admin "*)
        i=0; while [ "${ent_key[$i]}" != "${k#windows.}" ]; do i=$((i + 1)); done
        case "$a" in
          new) [ -z "$b" ] || die "$plan: $k new takes nothing more" ;;
          carry) [[ "$b" =~ ^[0-9a-f]{64}$ && -z "$c" ]] || die "$plan: $k carry takes a SHA-256"; ent_how[$i]=carry; ent_sha[$i]="$b" ;;
          *) die "$plan: $k is new or carry, not '$a'" ;;
        esac ;;
      "windows.app none"|"windows.bridge none") die "$plan: no Windows files in the plan (none): build.sh doesn't call windows-assets.sh then" ;;
      *) die "$plan: the line '$k $a' isn't a plan's" ;;
    esac
  done < "$plan"
  for k in windows.app windows.bridge windows.stub.x64 windows.stub.arm64 windows.admin; do
    case " $seen " in *" $k "*) ;; *) die "$plan has no $k line" ;; esac
  done
  if [ "$app" = carry ]; then
    for i in 0 1 2; do [ "${ent_how[$i]}" = carry ] || die "$plan carries the zip but not windows.${ent_key[$i]}: a carried zip carries its stubs and worker"; done
  fi
fi
carrying=""; [ "$app$br${ent_how[*]}" = "newnewnew new new" ] || carrying=1

# ---------------------------------------------------------------- the carried files, checked first
if [ -n "$carrying" ]; then
  [ -n "$carry" ] && [ -d "$carry" ] || die "the plan carries a Windows file: --carry DIR with the previous release's files"
  carry="$(cd "$carry" && pwd)"
  [ -f "$carry/payload.json" ] || die "no payload.json in $carry: a carried Windows file comes with the payload that names it"
  pv="$(python3 -I "$root/scripts/win-carry.py" prev "$carry/payload.json")" || die "$carry/payload.json has no readable windows block to carry from"
  eval "$pv"
  [ -f "$carry/$zip" ] || die "no $zip in $carry"
  [ "$(sha256 "$carry/$zip")" = "$prev_win_zip_sha256" ] || die "$carry/$zip isn't the SHA-256 its payload.json names"
  # the previous zip's own entries are what its payload names
  for e in "0:$prev_win_stub_x64_sha256" "1:$prev_win_stub_arm64_sha256" "2:$prev_win_admin_sha256"; do
    [ "$(entry_sha "$carry/$zip" "${ent_name[${e%%:*}]}")" = "${e#*:}" ] || die "$carry/$zip's ${ent_name[${e%%:*}]} isn't the SHA-256 its payload.json names"
  done
  # one entry per name as the updater compares them (unzip.c): same_name ignores ASCII case and a
  # trailing '/', and file_is_folder_of refuses a file that is another entry's folder; each of the
  # three entries once
  python3 -I -c 'import sys, zipfile
n = zipfile.ZipFile(sys.argv[1]).namelist()
u = [x.encode().upper() for x in n]
k = [x[:-1] if x.endswith(b"/") else x for x in u]
f = [x for x in u if not x.endswith(b"/")]
sys.exit(0 if len(set(k)) == len(k) and not any(y.startswith(x + b"/") for x in f for y in u)
         and all(n.count(e) == 1 for e in sys.argv[2:]) else 1)' "$carry/$zip" "${ent_name[@]}" \
    || die "$carry/$zip has duplicate or case-colliding entry names"
  # the plan names the previous release's bytes; an archived TEST build's are the same but for
  # its zip and its worker (its payload.json's, checked above)
  if [ "$app" = carry ]; then
    [ "$prev_win_app_version" = "$app_v" ] || die "$carry/payload.json's Windows app is $prev_win_app_version, the plan's $app_v"
    [ -n "$test_base" ] || [ "$prev_win_zip_sha256" = "$app_sha" ] || die "$carry/$zip isn't the plan's ($app_sha)"
  fi
  for i in 0 1 2; do
    [ "${ent_how[$i]}" = carry ] || continue
    [ -z "$test_base" ] || [ "$i" != 2 ] || continue      # the TEST worker has the test key
    [ "$(entry_sha "$carry/$zip" "${ent_name[$i]}")" = "${ent_sha[$i]}" ] || die "$carry/$zip's ${ent_name[$i]} isn't the plan's (${ent_sha[$i]})"
  done
  if [ "$br" = carry ]; then
    [ -f "$carry/$bridge" ] || die "no $bridge in $carry"
    want="$br_sha"
    [ "$prev_win_bridge_sha256" = "$want" ] || die "$carry/payload.json's Windows bridge isn't the plan's ($br_sha)"
    [ "$(sha256 "$carry/$bridge")" = "$want" ] || die "$carry/$bridge doesn't have the SHA-256 $want"
  fi
fi

# ---------------------------------------------------------------- build what is new
rm -f "$dist/$zip" "$dist/$bridge" "$dist/bootstrap.ps1" "$dist/NOTICE-windows.txt" "$dist/windows.env" "$dist/windows-release.sha256"
work="$(mktemp -d "${TMPDIR:-/tmp}/rbsp-windows-assets.XXXXXX")" || die "no temporary folder"
trap 'rm -rf "${work:?}"' EXIT
mkdir -p "$work/carried" "$work/out"
if [ "$app$br" != carrycarry ]; then
  args=()
  [ -z "$test_base" ] || args+=(--test "$test_base" --test-key "$test_key")
  if [ "$app" = new ]; then
    for i in 0 1 2; do
      [ "${ent_how[$i]}" = carry ] || continue
      # the entry checked above (the payload's, the plan's), into the fresh folder
      f="$work/carried/${ent_key[$i]}"
      entry_get "$carry/$zip" "${ent_name[$i]}" "$f"
      args+=(--carry-file "${ent_name[$i]}=$f")
    done
  fi
  "$root/scripts/build-windows.sh" ${args[@]+"${args[@]}"} "$work/out" || die "the Windows files didn't build (see above)"
  [ -f "$work/out/$zip" ] && [ -f "$work/out/$bridge" ] || die "scripts/build-windows.sh made no $zip or $bridge"
  # a TEST build with a new worker: the release worker's hash, for release.sh (TT1)
  if [ -n "$test_base" ] && [ "${ent_how[2]}" = new ]; then
    grep -qxE 'admin [0-9a-f]{64}' "$work/out/windows-release.sha256" 2> /dev/null && [ "$(grep -c '' "$work/out/windows-release.sha256")" = 1 ] \
      || die "scripts/build-windows.sh --test made no windows-release.sha256 (admin SHA256: the release worker)"
    cp "$work/out/windows-release.sha256" "$dist/windows-release.sha256"
  fi
fi
if [ "$app" = carry ]; then
  cp "$carry/$zip" "$dist/$zip"; win_app_version="$prev_win_app_version"
  echo "Windows app     carried: $win_app_version from $carry"
else
  cp "$work/out/$zip" "$dist/$zip"; win_app_version="$version"
  echo "Windows app     new: $win_app_version (x64 stub ${ent_how[0]}, arm64 stub ${ent_how[1]}, worker ${ent_how[2]})"
fi
if [ "$br" = carry ]; then cp "$carry/$bridge" "$dist/$bridge"; echo "Windows bridge  carried from $carry"
else cp "$work/out/$bridge" "$dist/$bridge"; echo "Windows bridge  new"; fi
# a carried stub or worker is the previous file in the shipped zip
for i in 0 1 2; do
  [ "${ent_how[$i]}" != carry ] || [ "$(entry_sha "$dist/$zip" "${ent_name[$i]}")" = "$(entry_sha "$carry/$zip" "${ent_name[$i]}")" ] \
    || die "the shipped $zip's ${ent_name[$i]} isn't the carried one"
done

# ---------------------------------------------------------------- bootstrap.ps1, NOTICE-windows.txt
src="$root/scripts/bootstrap.ps1"
[ "$(grep -c "^\$SigningKeys = ''\$" "$src")" = 1 ] && [ "$(grep -c "^\$PayloadMinimum = ''\$" "$src")" = 1 ] \
  || die "scripts/bootstrap.ps1 needs exactly one \$SigningKeys = '' and one \$PayloadMinimum = '' line"
sed -e "s|^\$SigningKeys = ''\$|\$SigningKeys = '$keys'|" -e "s|^\$PayloadMinimum = ''\$|\$PayloadMinimum = '$version'|" "$src" > "$dist/bootstrap.ps1"
grep -qxF "\$SigningKeys = '$keys'" "$dist/bootstrap.ps1" && grep -qxF "\$PayloadMinimum = '$version'" "$dist/bootstrap.ps1" \
  || die "the keys or the version didn't go into bootstrap.ps1"
# pure ASCII, no BOM (release.md §3.3): irm | iex reads it as text
python3 -I -c 'import sys; b = open(sys.argv[1], "rb").read(); sys.exit(0 if b.isascii() and not b.startswith(b"\xef\xbb\xbf") else 1)' "$dist/bootstrap.ps1" \
  || die "bootstrap.ps1 isn't pure ASCII without a BOM"
cp "$root/win/NOTICE.txt" "$dist/NOTICE-windows.txt"

# ---------------------------------------------------------------- what the windows block names
{
  echo "win_app_version=$win_app_version"
  echo "win_zip_sha256=$(sha256 "$dist/$zip")"
  echo "win_admin_sha256=$(entry_sha "$dist/$zip" "admin/RB Stems Plus.exe")"
  echo "win_stub_x64_sha256=$(entry_sha "$dist/$zip" "stub/x64/RB Stems Plus.exe")"
  echo "win_stub_arm64_sha256=$(entry_sha "$dist/$zip" "stub/arm64/RB Stems Plus.exe")"
  echo "win_bridge_sha256=$(sha256 "$dist/$bridge")"
} > "$dist/windows.env"
while IFS='=' read -r k v; do
  [ "$k" = win_app_version ] || [[ "$v" =~ ^[0-9a-f]{64}$ ]] || die "the shipped files have no $k (a zip without its stubs or worker?)"
done < "$dist/windows.env"
# so windows.env names the plan's SHA-256 for every carried file (a TEST build's zip and worker:
# the archive's own): the carried zip and bridge were checked against the plan and copied, a
# carried entry against the plan and found again in the shipped zip (above)

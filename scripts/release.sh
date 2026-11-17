#!/bin/bash
# Builds a release on this Mac, signs it, and uploads it to GitHub as a DRAFT. GitHub builds
# nothing. It never publishes: publishing stays a manual step (docs/RELEASING.md). Run it
# yourself, in Terminal, once the TEST build of the same commit has passed the VM tests.
#
# Two repositories: this one, the private Gabe-LS/rbstemsplus-dev, where all work happens, and
# the public Gabe-LS/rbstemsplus, which only receives releases. A release copies this commit's
# files, minus the paths in .publicignore, into a local clone of the public repository as one
# commit "RB Stems Plus X.Y.Z" by Gabe-LS, then builds, signs and uploads the draft from that
# public commit (payload.json names it). The public history holds only these release commits.
#
#   scripts/release.sh vX.Y.Z                   signed with the everyday key
#   scripts/release.sh vX.Y.Z --key PATH        with the everyday key in PATH
#   scripts/release.sh vX.Y.Z --backup-key      with the backup key, pasted from the password
#                                               manager (not shown, never written to disk)
#   scripts/release.sh vX.Y.Z --public PATH     the public clone at PATH (default: git config
#                                               rbstemsplus.publicClone, else the folder
#                                               "RB Stems Plus (public)" next to this one)
#   scripts/release.sh vX.Y.Z --dry-run         only steps 1, 3 and 4, with nothing committed,
#                                               and step 5's carry decisions: shows what would
#                                               be published and changes nothing (no key or
#                                               Terminal; gh only reads the latest release; it
#                                               fetches the public clone's origin and builds the
#                                               Mac files in a scratch folder)
#   scripts/release.sh vX.Y.Z --plan            the plan, before any gate (release.md §1.3): the
#                                               checks of steps 1-4 with nothing committed (no
#                                               key or Terminal), then the carry decisions, written
#                                               to build/gates/<commit>/plan. The TEST build for
#                                               the gates is then made in that shape
#                                               (scripts/build.sh --test URL --plan FILE), and a
#                                               real run stops when its decisions differ
#   scripts/release.sh vX.Y.Z --branch NAME     a maintenance release from the branch NAME (say
#                                               maint/1.3) instead of main: HEAD must be on
#                                               origin/NAME; every other check stays
#   scripts/release.sh vX.Y.Z --mac=auto|keep|new  the Mac app and bridge (default auto): each
#                                               carried forward byte for byte when this commit
#                                               builds the same files, or when no Mac input
#                                               changed since the commit that built them (a
#                                               toolchain's drift only), else new. keep: both
#                                               carried even though the build differs (a Windows
#                                               release where shared code moved but nothing
#                                               changed for Mac users). new: a new Mac app.
#                                               From main after a release from a branch, auto
#                                               is refused: keep, or new with the owner's
#                                               go-ahead (step 2). The same flag goes to --plan
#                                               and to the real run
#   scripts/release.sh vX.Y.Z --windows         the first release with Windows files (R4.I97):
#                                               the windows block and the Windows assets. From
#                                               then on every release has them (a latest release
#                                               with the block turns this on by itself). The
#                                               same flag goes to --plan and to the real run
#
# Once: set the public identity's email, your GitHub noreply address (GitHub › Settings › Emails),
#   git config rbstemsplus.publicEmail 12345678+Gabe-LS@users.noreply.github.com
# and clone the public repository next to this one:
#   git clone https://github.com/Gabe-LS/rbstemsplus.git "../RB Stems Plus (public)"
# The global git identity is never used for the public repository.
#
# In order, stopping at the first problem:
#   1. VERSION is X.Y.Z; no uncommitted or untracked file; the key file is this account's alone;
#      in Terminal; the plan of this commit exists (build/gates/<commit>/plan, from --plan);
#   2. (gh signed in, network) HEAD is on origin/main (origin/NAME with --branch NAME); the tag
#      vX.Y.Z, here or on origin, points at HEAD if it exists; GitHub has no release vX.Y.Z yet;
#      when the latest release's payload.json has a "windows" block, this commit has
#      scripts/build-windows.sh (release.md T12) to build or carry its Windows files; from main, when the newest vA.B.C tag on origin isn't on origin/main's
#      first-parent chain (a release from a branch came last; a merge of the branch into main
#      doesn't change that), main is pinned to that release's Mac app: --mac=keep carries it
#      forward, --mac=new (the owner's go-ahead) makes a new one, and auto is refused;
#   3. the public clone: rbstemsplus.publicEmail is a GitHub noreply address of Gabe-LS; the
#      clone's origin is Gabe-LS/rbstemsplus (and this repository's isn't); it is on main, clean,
#      and at its origin/main, or one commit ahead with this very release (an earlier run);
#      X.Y.Z is newer than every other vA.B.C tag there (the apps refuse an older payload.json);
#   4. the public copy: HEAD's files (git archive) minus .publicignore's paths and .publicignore;
#      refused if a listed path is missing from HEAD or would still be published, if any
#      published file mentions a listed file by name, or holds this account's full or short name
#      or this repository's git name or email; the copy must be HEAD's files exactly (modes and
#      contents), minus the private ones. It is committed in the public clone as "RB Stems Plus
#      X.Y.Z", author and committer Gabe-LS <publicEmail>, unsigned, on top of origin/main, and
#      main is moved to it there (nothing is pushed yet); a tag vX.Y.Z there must point at it;
#   5. the carry decisions (release.md §1.3), the same code as --plan's: the latest release's
#      payload.json, verified with this commit's public keys, names the previous Mac app and
#      bridge (their SHA-256s checked once downloaded, their commits in app.commit and
#      bridge.commit, else its top-level commit). With --mac=auto, the Mac app is built again in a
#      scratch folder from the public copy, stamped with the previous app.version, and compared
#      with the previous zip (scripts/bundle-compare.sh): the same files, or the same code with
#      the set-aside asset catalog's inputs unchanged, carry it; else, when no Mac app input
#      changed since its commit (the lists the build writes, scripts/mac-inputs.py, against the
#      public history), it is carried too (the toolchain's drift, printed); else it is new. The
#      bridge the same way, by its SHA-256. They must be the plan's, or it stops (the gates ran
#      on another shape). G3's tart run must have passed for this commit, the TEST payload in
#      dist-test/ and this plan (tests/mac/g3.sh check: build/gates/<commit>/g3-mac.ok, every
#      required scenario; release.md §6.1), with --branch too. With Windows files (step 4b,
#      release.md §6, I76, R4.I94): the VM record build/gates/<commit>/vm-win.ok
#      (tests/win/vm-run.sh check, for the TEST payload in dist-test/) whenever the Windows app,
#      a stub, the worker or the Windows bridge is new, or the TEST payload's lists (windows
#      compatible and resampler_proved, the model, previous_models) differ from the previous
#      release's. The Windows decisions (release.md §1.3): the Windows app built again from the
#      public copy stamped with the previous windows.app.version (scripts/build-windows.sh
#      --stamp) and compared with the previous zip file by file (SHA-256s): the same files
#      carry the zip; else a new zip, in which each start stub (x64, arm64) and the worker are
#      still carried one by one when their own SHA-256 is the previous stub_sha256 and
#      admin_sha256 (R4.I83); the Windows bridge carried when its SHA-256 is the previous one.
#      Then scripts/build.sh, from the
#      public clone, builds every asset into its dist/ in that shape (a new app stamped with
#      X.Y.Z; a carried file copied byte for byte), with its own checks (a clean tree, the commit
#      in payload.json, both public keys, the hardened runtime, libFLAC's checksum, the model's);
#   6. dist/: no test key ships: this checkout's build/test-keys key and every gate-archive
#      tag's are none of the public commit's keys/*.pub.pem (as points) and aren't in
#      bootstrap.sh, bootstrap.ps1 or the app's executable (any point form); payload.json names
#      the public commit and X.Y.Z, the plan's app version, and each Mac file's commit and
#      SHA-256 (a carried file's are the previous release's, and so are its bytes); with Windows
#      files, its windows block names the plan's windows.app.version, the zip's, the stubs', the
#      worker's and the bridge's SHA-256s (a carried one's are the previous release's, and so
#      are its bytes; a carried stub or worker is that entry of the zip), and its lists are the
#      TEST payload's; without, no windows block and no Windows asset; SHA256SUMS matches the
#      files;
#   7. payload.json is signed into dist/payload.json.sig (openssl asks for the everyday key's
#      passphrase itself: never on a command line, in the environment or in a log), and the
#      signature is checked with the public commit's keys/release.pub.pem and keys/backup.pub.pem;
#   8. you type "yes"; then the tag is made here if missing (annotated) and pushed if origin
#      doesn't have it; the public tag is made (annotated, by Gabe-LS) and the public main and
#      tag are pushed together;
#   9. gh creates the DRAFT release vX.Y.Z on Gabe-LS/rbstemsplus with the nine files, thirteen
#      with Windows files (it fails
#      if the release exists; nothing is ever replaced), with docs/release-notes/X.Y.Z.md as its
#      notes when there is one (unwrapped by scripts/unwrap-notes.py: GitHub keeps line breaks), and the draft is checked: its files' names and sizes, and its
#      payload.json and payload.json.sig are the ones made here.
#
# Needs: Xcode 26+ (scripts/build.sh), gh signed in with access to both repositories (it never
# signs in itself), git, network, and the model (scripts/build.sh says where it looks; the copy in
# this repository's dist/ is used when the public clone has none).
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
REPO="Gabe-LS/rbstemsplus"
PUBLIC_NAME="Gabe-LS"
# what a release holds; payload.json.sig is made here
ASSETS="rbstemsplus-app.zip libonnxruntime.1.18.0.dylib stemsplus-model.onnx bootstrap.sh NOTICE LICENSE payload.json SHA256SUMS"
# ... and from the first release with a windows block on (R4.I97)
WIN_ASSETS="rbstemsplus-windows.zip rbstemsplus-bridge-win-x64.dll bootstrap.ps1 NOTICE-windows.txt"
WIN_ZIP=rbstemsplus-windows.zip
WIN_BRIDGE=rbstemsplus-bridge-win-x64.dll
# the zip's entries carried one by one (R4.I83): the stubs and the worker
WIN_STUB_X64="stub/x64/RB Stems Plus.exe"
WIN_STUB_ARM64="stub/arm64/RB Stems Plus.exe"
WIN_ADMIN="admin/RB Stems Plus.exe"
# the Windows build's download caches under build/ (as the Mac side's flac, sqlcipher and
# pyrekordbox): dl (llvm-mingw's and the .NET SDK's tarballs, deps.sh's records), nuget (the
# packages restored in locked mode), pocketfft, blocksrt, flac. Never the unpacked llvm-mingw/ or
# dotnet/: the build unpacks them again from dl's tarballs once their pinned SHA-256s check out
WIN_CACHES="dl nuget pocketfft blocksrt flac"

die() { echo "release.sh: $*" >&2; exit 1; }
usage() { echo "usage: $0 vX.Y.Z [--key PATH | --backup-key] [--public PATH] [--branch NAME] [--mac=auto|keep|new] [--windows] [--dry-run | --plan]" >&2; exit 2; }

[ $# -ge 1 ] || usage
tag="$1"; shift
key="$HOME/.rbstemsplus-signing/release-key.pem"
backup=0
dry=0
plan=0
pub=""
branch=main
mac=auto
windows_flag=0
while [ $# -gt 0 ]; do
  case "$1" in
    --key) [ $# -ge 2 ] || usage; key="$2"; shift 2 ;;
    --backup-key) backup=1; shift ;;
    --public) [ $# -ge 2 ] || usage; pub="$2"; shift 2 ;;
    --branch) [ $# -ge 2 ] || usage; branch="$2"; shift 2 ;;
    --mac=auto|--mac=keep|--mac=new) mac="${1#--mac=}"; shift ;;
    --mac=*) die "--mac takes auto, keep or new, not '${1#--mac=}'" ;;
    --windows) windows_flag=1; shift ;;
    --dry-run) dry=1; shift ;;
    --plan) plan=1; shift ;;
    *) usage ;;
  esac
done
[ "$dry$plan" != 11 ] || die "--dry-run and --plan are two different runs: give one"
# the real run's step 1 needs the plan, written by --plan before any gate (release.md §6)
plan_dir="$root/build/gates"
[[ "$tag" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "the tag must be vX.Y.Z, not '$tag'"
version="${tag#v}"
git check-ref-format --branch "$branch" > /dev/null 2>&1 && [[ "$branch" != -* ]] || die "--branch: '$branch' isn't a branch name"

# ver_gt A B: true when the version A (x.y.z) is newer than B
ver_gt() {
  local IFS=. i
  local -a a=($1) b=($2)
  for i in 0 1 2; do
    [ "$((10#${a[i]:-0}))" -eq "$((10#${b[i]:-0}))" ] || { [ "$((10#${a[i]:-0}))" -gt "$((10#${b[i]:-0}))" ]; return; }
  done
  return 1
}
# newest_tag: of the "<sha> refs/tags/vA.B.C[^{}]" lines on stdin (git ls-remote's or
# show-ref's), every vA.B.C but $tag, prints the newest as "vA.B.C <commit>" (the commit an
# annotated tag points at), or nothing
newest_tag() {
  awk '{ t = $2; sub(/^refs\/tags\//, "", t)
         if (t ~ /\^\{\}$/) { sub(/\^\{\}$/, "", t); p[t] = $1 } else if (!(t in q)) q[t] = $1 }
       END { for (t in q) print t, (t in p ? p[t] : q[t]) }' | {
    t=""; sha=""; best=""; best_sha=""     # a subshell: nothing leaks
    while read -r t sha; do
      [[ "$t" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] && [ "$t" != "$tag" ] || continue
      if [ -z "$best" ] || ver_gt "${t#v}" "${best#v}"; then best="$t"; best_sha="$sha"; fi
    done
    [ -z "$best" ] || echo "$best $best_sha"
  }
}

# ------------------------------------------------------------------ 1. here

[ "$(tr -d '[:space:]' < "$root/VERSION")" = "$version" ] || die "VERSION says $(tr -d '[:space:]' < "$root/VERSION"), not $version"
changes="$(git -C "$root" status --porcelain)" || die "not a git checkout"
if [ "$dry" = 1 ]; then
  [ -z "$changes" ] || echo "NOTE: uncommitted or untracked files: the dry run copies HEAD, without them"
else
  [ -z "$changes" ] || die "uncommitted or untracked files (git status): a release is built only from a commit"
fi
commit="$(git -C "$root" rev-parse HEAD)"
plan_file="$plan_dir/$commit/plan"
if [ "$dry$plan" = 00 ]; then
  if [ "$backup" = 0 ]; then
    [ -f "$key" ] && [ ! -L "$key" ] || die "no signing key at $key (scripts/make-signing-keys.sh, or --key PATH, or --backup-key)"
    [ -O "$key" ] || die "$key isn't this account's"
    case "$(stat -f %Lp "$key")" in 600|400) ;; *) die "$key can be read by other accounts: chmod 600 it" ;; esac
  fi
  [ -t 0 ] && [ -t 1 ] || die "run it yourself in Terminal: it asks for the key's passphrase and for a confirmation"
  [ -f "$plan_file" ] || die "no plan for $commit ($plan_file): run scripts/release.sh $tag --plan first (with the same --mac and --branch), before the gates"
fi

tmp="$(mktemp -d "${TMPDIR:-/tmp}/rbsp-release.XXXXXX")"
trap 'rm -rf "${tmp:?}"' EXIT

# the public repository's URLs: https://github.com/Gabe-LS/rbstemsplus(.git), git@github.com:…,
# or a local path ending in Gabe-LS/rbstemsplus(.git)
is_public_url() { [[ "$1" =~ [:/]Gabe-LS/rbstemsplus(\.git)?/?$ ]]; }
dev_url="$(git -C "$root" remote get-url origin 2>/dev/null || true)"
! is_public_url "$dev_url" || die "this repository's origin is the public $REPO: run release.sh in the private development repository"

# ------------------------------------------------------------------ 2. GitHub

tag_here=0; remote=""
if [ "$dry" = 0 ]; then
  command -v gh > /dev/null || die "gh isn't installed"
  command -v python3 > /dev/null || die "python3 isn't installed (it unwraps the release notes)"
  gh auth status > /dev/null 2>&1 || die "gh isn't signed in: run gh auth login yourself first"
  echo "== the commit"
  git -C "$root" fetch -q origin "+refs/heads/$branch:refs/remotes/origin/$branch" || die "couldn't fetch origin $branch"
  if ! git -C "$root" merge-base --is-ancestor "$commit" "origin/$branch"; then
    [ "$branch" != main ] || die "HEAD ($commit) isn't on origin/main: push it first (a maintenance release from a branch: --branch NAME)"
    die "HEAD ($commit) isn't on origin/$branch: push it first"
  fi
  if [ "$branch" = main ]; then
    # the pin: when a release from a branch came last, main's next one carries that Mac app
    # forward (--mac=keep) unless the owner allows a new one (--mac=new); auto doesn't choose
    origin_tags="$(git -C "$root" ls-remote --tags origin)" || die "couldn't read origin's tags"
    last="$(newest_tag <<< "$origin_tags")"
    last_tag="${last%% *}"; last_sha="${last#* }"
    if [ -n "$last" ] && ver_gt "$version" "${last_tag#v}" && [ "$mac" = auto ]; then
      # main's own releases are on its first-parent chain; a branch release merged into main
      # (never do that: cherry-pick) is not, so a merge doesn't lift the pin. A file, not a pipe:
      # grep -q ending early would fail rev-list under pipefail.
      git -C "$root" rev-list --first-parent origin/main > "$tmp/first-parent" || die "couldn't list origin/main's commits"
      if ! grep -qx "$last_sha" "$tmp/first-parent"; then
        die "the newest release, $last_tag, came from a branch (its tag isn't on origin/main): main is pinned to that Mac app. Run again with --mac=keep to carry it forward, or, with the owner's go-ahead for a new Mac app, with --mac=new"
      fi
    fi
  fi
  if git -C "$root" rev-parse -q --verify "refs/tags/$tag" > /dev/null; then
    [ "$(git -C "$root" rev-parse "$tag^{commit}")" = "$commit" ] || die "the tag $tag here points at $(git -C "$root" rev-parse "$tag^{commit}"), not HEAD"
    tag_here=1
  fi
  # the commit an annotated tag points at ("^{}"), else a lightweight tag's
  remote="$(git -C "$root" ls-remote --tags origin "refs/tags/$tag" "refs/tags/$tag^{}" \
            | awk -v a="refs/tags/$tag^{}" -v b="refs/tags/$tag" '$2 == a { p = $1 } $2 == b { q = $1 } END { print (p != "" ? p : q) }')" \
    || die "couldn't read origin's tags"
  [ -z "$remote" ] || [ "$remote" = "$commit" ] || die "the tag $tag on origin points at $remote, not HEAD ($commit)"
  if out="$(gh release view "$tag" --repo "$REPO" --json isDraft 2>&1)"; then
    die "GitHub has a release $tag already: never replaced. To make it again, delete that draft by hand first."
  fi
  case "$out" in *"not found"*) ;; *) die "couldn't ask GitHub about $tag: $out" ;; esac
  # after a release with a "windows" block every release has one (R4.I97): this commit must
  # build or carry the Windows files, or every Windows install would break
  if out="$(gh release download --repo "$REPO" --pattern payload.json --dir "$tmp/latest" 2>&1)"; then
    # exit 3: a "windows" block; any other failure: not a JSON object (unreadable, so not checked)
    rc=0
    python3 -I -c 'import json,sys; d=json.load(open(sys.argv[1])); sys.exit(3 if isinstance(d,dict) and "windows" in d else 0 if isinstance(d,dict) else 1)' \
      "$tmp/latest/payload.json" > /dev/null 2>&1 || rc=$?
    [ "$rc" != 3 ] || git -C "$root" cat-file -e "$commit:scripts/build-windows.sh" 2>/dev/null \
      || die "the latest release's payload.json has a \"windows\" block, and $commit has no scripts/build-windows.sh to build or carry its Windows files (release.md T12)"
    [ "$rc" = 0 ] || [ "$rc" = 3 ] || die "couldn't read the latest release's payload.json as a JSON object, so its \"windows\" block can't be checked"
  else
    case "$out" in *"not found"*) ;; *) die "couldn't download the latest release's payload.json: $out" ;; esac
  fi
  echo "$commit: on origin/$branch, VERSION $version, no release $tag yet"
elif git -C "$root" rev-parse -q --verify "refs/remotes/origin/$branch" > /dev/null; then
  if git -C "$root" merge-base --is-ancestor "$commit" "origin/$branch"; then
    echo "HEAD is on origin/$branch as last fetched (a real run fetches it again)"
  else
    echo "NOTE: HEAD isn't on origin/$branch as last fetched: a real run stops unless it is pushed"
  fi
else
  echo "NOTE: no origin/$branch here (never fetched): a real run fetches it and stops unless HEAD is on it"
fi
[ "$dry" = 0 ] || echo "NOTE: not checked in a dry run: main's pin and the latest release's \"windows\" block (a real run checks both)"

# ------------------------------------------------------------------ 3. the public clone

echo "== the public clone"
email="$(git -C "$root" config --get rbstemsplus.publicEmail || true)"
[ -n "$email" ] || die "git config rbstemsplus.publicEmail isn't set. Set it once to your GitHub noreply address (GitHub › Settings › Emails): git config rbstemsplus.publicEmail 12345678+$PUBLIC_NAME@users.noreply.github.com"
[[ "$email" =~ ^([0-9]+\+)?Gabe-LS@users\.noreply\.github\.com$ ]] \
  || die "rbstemsplus.publicEmail is $email: it must be $PUBLIC_NAME's GitHub noreply address (…+$PUBLIC_NAME@users.noreply.github.com)"
[ -n "$pub" ] || pub="$(git -C "$root" config --get rbstemsplus.publicClone || true)"
[ -n "$pub" ] || pub="$(dirname "$root")/RB Stems Plus (public)"
[ -d "$pub" ] || die "no public clone at $pub: git clone https://github.com/$REPO.git \"$pub\" (or --public PATH, or git config rbstemsplus.publicClone PATH)"
pub="$(cd "$pub" && pwd -P)"
[ "$(git -C "$pub" rev-parse --show-toplevel 2>/dev/null)" = "$pub" ] || die "$pub isn't the top of a git clone"
[ "$pub" != "$(cd "$root" && pwd -P)" ] || die "the public clone can't be this repository"
pub_git="$(git -C "$pub" rev-parse --absolute-git-dir)"
pub_url="$(git -C "$pub" remote get-url origin 2>/dev/null)" || die "$pub has no origin"
is_public_url "$pub_url" || die "$pub's origin is $pub_url, not $REPO"
[ "$(git -C "$pub" symbolic-ref -q --short HEAD || true)" = main ] || die "$pub isn't on its branch main: git -C \"$pub\" switch main"
[ -z "$(git -C "$pub" status --porcelain)" ] || die "$pub has uncommitted or untracked files"
git -C "$pub" fetch -q --tags origin || die "couldn't fetch $pub's origin"
# the order of versions: every app that has seen a newer payload.json refuses an older one
pub_last="$( { git -C "$pub" show-ref --tags || true; } | newest_tag)"
pub_last="${pub_last%% *}"
[ -z "$pub_last" ] || ver_gt "$version" "${pub_last#v}" || die "$version isn't newer than $pub_last, the newest release in $REPO"
base="$(git -C "$pub" rev-parse -q --verify refs/remotes/origin/main || true)"
if [ -z "$base" ] && [ -n "$(git -C "$pub" ls-remote --heads origin)" ]; then
  die "$pub's origin has branches, but no main"
fi
head="$(git -C "$pub" rev-parse -q --verify HEAD || true)"

# ------------------------------------------------------------------ 4. the public copy

echo "== the public copy of $commit"
git -C "$root" cat-file -e "$commit:.publicignore" 2>/dev/null || die "$commit has no .publicignore: a release needs the list of private paths"
private=()
while IFS= read -r line || [ -n "$line" ]; do
  line="${line%"${line##*[![:space:]]}"}"                     # trailing spaces
  case "$line" in ''|'#'*) continue ;; esac
  line="${line%/}"
  [[ "$line" != /* && "$line" != *..* && "$line" != *'*'* ]] || die ".publicignore: '$line' must be a plain path inside the repository"
  git -C "$root" cat-file -e "$commit:$line" 2>/dev/null || die ".publicignore lists $line, which isn't in $commit: fix the list"
  private+=("$line")
done < <(git -C "$root" show "$commit:.publicignore")
[ "${#private[@]}" -gt 0 ] || die ".publicignore lists nothing"
stage="$tmp/public"
mkdir "$stage"
git -C "$root" archive --format=tar "$commit" | tar -x -f - -C "$stage" || die "couldn't export $commit"
rm -f "$stage/.publicignore"
for p in "${private[@]}"; do
  rm -rf "${stage:?}/$p"
  [ ! -e "$stage/$p" ] && [ ! -L "$stage/$p" ] || die "$p is still in the public copy"
done
# no mention of a private file by name (and by its capitals name without the extension), or of
# a private folder by its path
names=()
for p in "${private[@]}"; do
  if [ "$(git -C "$root" cat-file -t "$commit:$p")" = tree ]; then names+=(-e "$p"); continue; fi
  b="$(basename "$p")"; names+=(-e "$b")
  stem="${b%.*}"
  if [ "$stem" != "$b" ] && [ "${#stem}" -ge 5 ] && [[ ! "$stem" =~ [a-z] ]]; then names+=(-e "$stem"); fi
  found="$(cd "$stage" && find . -name "$b" | sed 's|^\./||')"
  [ -z "$found" ] || die "a file named like the private $p would be published: $found"
done
# search: the public copy's text files holding any of the strings given (grep's options): true if
# some do, listed in $hits
search() {
  local rc=0
  hits="$(cd "$stage" && grep -rIlF "$@" . | sed 's|^\./||' | tr '\n' ' ')" || rc=$?
  [ "$rc" -le 1 ] || die "couldn't search the public copy"
  [ -n "$hits" ]
}
if search -w "${names[@]}"; then
  die "these files mention a private file (.publicignore) and would be published: $hits"
fi
# no name or email of the developer's own identity
idents=()
for v in "$(id -F 2>/dev/null || true)" "$(id -un)" "$(git -C "$root" config --get user.name || true)"; do
  [ "${#v}" -ge 5 ] && [ "$(tr '[:upper:]' '[:lower:]' <<< "$v")" != "$(tr '[:upper:]' '[:lower:]' <<< "$PUBLIC_NAME")" ] && idents+=(-e "$v")
done
v="$(git -C "$root" config --get user.email || true)"
[[ "$v" != *@* || "$v" == "$email" ]] || idents+=(-e "$v")
if [ "${#idents[@]}" -gt 0 ] && search -i "${idents[@]}"; then
  die "these files hold the developer's own name or email and would be published: $hits"
fi
# the copy, as a tree in the public clone (its own index, nothing checked out)
export GIT_INDEX_FILE="$tmp/index"
git --git-dir="$pub_git" --work-tree="$stage" -C "$stage" add -A -f . || die "couldn't add the public copy"
tree="$(git --git-dir="$pub_git" --work-tree="$stage" write-tree)" || die "couldn't write the public tree"
unset GIT_INDEX_FILE
# exactly HEAD's files (mode, contents, path), minus the private ones
is_private() { local p; for p in "${private[@]}" .publicignore; do [ "$1" = "$p" ] || [[ "$1" == "$p/"* ]] && return 0; done; return 1; }
# (-z: NUL-separated and never quoted, so is_private sees a non-ASCII name as it is; compared
# through their SHA-256s, as a shell variable can't hold a NUL)
want="$(git -C "$root" ls-tree -r -z "$commit" | while IFS= read -r -d '' l; do is_private "${l#*$'\t'}" || printf '%s\0' "$l"; done | shasum -a 256)"
[ "$want" = "$(git --git-dir="$pub_git" ls-tree -r -z "$tree" | shasum -a 256)" ] || die "the public copy isn't HEAD's files minus the private ones (a .gitattributes export rule?)"
count="$(git --git-dir="$pub_git" ls-tree -r --name-only "$tree" | wc -l | tr -d ' ')"
echo "$count files; left out: .publicignore ${private[*]}"
msg="RB Stems Plus $version"
ident="$PUBLIC_NAME <$email>"
# an earlier run's commit of this very release, not pushed or pushed but not released
reuse=""
parent="$([ -z "$head" ] || git -C "$pub" rev-parse -q --verify "$head^" || true)"
if [ -n "$head" ] && { [ "$head" = "$base" ] || [ "$parent" = "$base" ]; } \
   && [ "$(git -C "$pub" rev-parse "$head^{tree}")" = "$tree" ] \
   && [ "$(git -C "$pub" log -1 --format=%B "$head" | sed '/^$/d')" = "$msg" ] \
   && [ "$(git -C "$pub" log -1 --format='%an <%ae>|%cn <%ce>' "$head")" = "$ident|$ident" ]; then
  reuse="$head"
elif [ "$head" != "$base" ]; then
  die "$pub's main has commits that aren't on its origin/main and aren't this release's: fix the clone by hand"
fi

# ------------------------------------------------------------------ the carry decisions
# (release.md §1.3): one code for --dry-run, --plan and step 5. Each Mac file is new or carried
# (the previous release's bytes, with its version and the commit that built it).

# prev_get: the latest release's payload.json and signature into $tmp/prev, verified with this
# commit's public keys, then its Mac app zip and bridge, checked against its SHA-256s. Sets
# prev_tag ("none" with no release yet), prev_sum (the payload's SHA-256) and prev_*.
prev_get() {
  local out k signer="" fields
  prev_tag=none; prev_sum=""; prev_win=0
  rm -rf "$tmp/prev"; mkdir -p "$tmp/prev"
  if ! out="$(gh release download --repo "$REPO" --pattern payload.json --pattern payload.json.sig --dir "$tmp/prev" 2>&1)"; then
    case "$out" in *"not found"*) return 0 ;; esac
    die "couldn't download the latest release's payload.json and its signature: $out"
  fi
  for k in release backup; do
    git -C "$root" show "$commit:keys/$k.pub.pem" > "$tmp/prev-$k.pub.pem" 2>/dev/null || die "$commit has no keys/$k.pub.pem"
    if /usr/bin/openssl dgst -sha256 -verify "$tmp/prev-$k.pub.pem" -signature "$tmp/prev/payload.json.sig" "$tmp/prev/payload.json" > /dev/null 2>&1; then
      signer="$k"; break
    fi
  done
  [ -n "$signer" ] || die "the latest release's payload.json doesn't verify with this commit's keys/*.pub.pem: nothing is carried from an unverified payload"
  # its fields; a missing app.commit or bridge.commit (every payload before 1.4.0) falls back to
  # the top-level commit (release.md §1.2)
  fields="$(python3 -I - "$tmp/prev/payload.json" <<'PY'
import json, re, sys
d = json.load(open(sys.argv[1]))
ver, sha, com = re.compile(r"\d+\.\d+\.\d+"), re.compile(r"[0-9a-f]{64}"), re.compile(r"[0-9a-f]{40}")
def need(v, rx, what):
    if not isinstance(v, str) or not rx.fullmatch(v):
        sys.exit("%s is %r" % (what, v))
    return v
app, bridge = d.get("app") or {}, d.get("bridge") or {}
top = need(d.get("commit"), com, "commit")
out = [need(d.get("payload_version"), ver, "payload_version"), top,
       need(app.get("version"), ver, "app.version"), need(app.get("commit", top), com, "app.commit"),
       need(app.get("zip"), re.compile(r"rbstemsplus-app\.zip"), "app.zip"), need(app.get("sha256"), sha, "app.sha256"),
       need(bridge.get("file"), re.compile(r"libonnxruntime\.1\.18\.0\.dylib"), "bridge.file"),
       need(bridge.get("commit", top), com, "bridge.commit"), need(bridge.get("sha256"), sha, "bridge.sha256")]
print(" ".join(out))
PY
)" || die "the latest release's payload.json can't be read for the carry decisions (above)"
  read -r prev_version prev_commit prev_app_version prev_app_commit prev_app_zip prev_app_sha \
          prev_bridge_file prev_bridge_commit prev_bridge_sha <<< "$fields"
  prev_tag="v$prev_version"
  prev_sum="$(shasum -a 256 "$tmp/prev/payload.json" | cut -d' ' -f1)"
  ! ver_gt "$prev_app_version" "$version" || die "the latest release's app.version, $prev_app_version, is newer than $version"
  gh release download "$prev_tag" --repo "$REPO" --pattern "$prev_app_zip" --pattern "$prev_bridge_file" --dir "$tmp/prev" > /dev/null 2>&1 \
    || die "couldn't download $prev_app_zip and $prev_bridge_file of $prev_tag"
  [ "$(shasum -a 256 "$tmp/prev/$prev_app_zip" | cut -d' ' -f1)" = "$prev_app_sha" ] || die "$prev_tag's $prev_app_zip doesn't have the SHA-256 its payload.json names"
  [ "$(shasum -a 256 "$tmp/prev/$prev_bridge_file" | cut -d' ' -f1)" = "$prev_bridge_sha" ] || die "$prev_tag's $prev_bridge_file doesn't have the SHA-256 its payload.json names"
  echo "the previous release: $prev_tag (payload.json signed with the $signer key; Mac app $prev_app_version from $prev_app_commit, bridge from $prev_bridge_commit)"
  # its windows block (release.md §1.2), when it has one: the Windows app's version, the zip's,
  # the stubs', the worker's and the bridge's SHA-256s; the two files downloaded and checked
  fields="$(python3 -I - "$tmp/prev/payload.json" <<'PY'
import json, re, sys
d = json.load(open(sys.argv[1]))
if "windows" not in d:
    print("none"); sys.exit(0)
ver, sha = re.compile(r"\d+\.\d+\.\d+"), re.compile(r"[0-9a-f]{64}")
def need(v, rx, what):
    if not isinstance(v, str) or not rx.fullmatch(v):
        sys.exit("windows.%s is %r" % (what, v))
    return v
w = d["windows"] if isinstance(d["windows"], dict) else sys.exit("windows isn't an object")
app, bridge = w.get("app") or {}, w.get("bridge") or {}
stub = app.get("stub_sha256") or {}
print(" ".join([need(app.get("version"), ver, "app.version"), need(app.get("zip"), re.compile(r"rbstemsplus-windows\.zip"), "app.zip"),
                need(app.get("sha256"), sha, "app.sha256"), need(app.get("admin_sha256"), sha, "app.admin_sha256"),
                need(stub.get("x64"), sha, "app.stub_sha256.x64"), need(stub.get("arm64"), sha, "app.stub_sha256.arm64"),
                need(bridge.get("file"), re.compile(r"rbstemsplus-bridge-win-x64\.dll"), "bridge.file"),
                need(bridge.get("sha256"), sha, "bridge.sha256")]))
PY
)" || die "the latest release's windows block can't be read for the carry decisions (above)"
  if [ "$fields" != none ]; then
    prev_win=1
    read -r prev_win_version prev_win_zip prev_win_sha prev_win_admin prev_win_stub_x64 prev_win_stub_arm64 \
            prev_win_bridge_file prev_win_bridge_sha <<< "$fields"
    ! ver_gt "$prev_win_version" "$version" || die "the latest release's windows.app.version, $prev_win_version, is newer than $version"
    gh release download "$prev_tag" --repo "$REPO" --pattern "$prev_win_zip" --pattern "$prev_win_bridge_file" --dir "$tmp/prev" > /dev/null 2>&1 \
      || die "couldn't download $prev_win_zip and $prev_win_bridge_file of $prev_tag"
    [ "$(shasum -a 256 "$tmp/prev/$prev_win_zip" | cut -d' ' -f1)" = "$prev_win_sha" ] || die "$prev_tag's $prev_win_zip doesn't have the SHA-256 its payload.json names"
    [ "$(shasum -a 256 "$tmp/prev/$prev_win_bridge_file" | cut -d' ' -f1)" = "$prev_win_bridge_sha" ] || die "$prev_tag's $prev_win_bridge_file doesn't have the SHA-256 its payload.json names"
    # the stubs and the worker the zip holds are the ones its payload names
    zip_entries "$tmp/prev/$prev_win_zip" > "$tmp/prev/win-entries" || die "$prev_tag's $prev_win_zip can't be read as a zip"
    [ "$(entry_sha "$tmp/prev/win-entries" "$WIN_STUB_X64")" = "$prev_win_stub_x64" ] && [ "$(entry_sha "$tmp/prev/win-entries" "$WIN_STUB_ARM64")" = "$prev_win_stub_arm64" ] \
      && [ "$(entry_sha "$tmp/prev/win-entries" "$WIN_ADMIN")" = "$prev_win_admin" ] \
      || die "$prev_tag's $prev_win_zip doesn't hold the stubs and the worker its payload.json names"
    echo "  its Windows app $prev_win_version, the Windows bridge ${prev_win_bridge_sha:0:12}"
  fi
}

# zip_entries ZIP: "SHA256 NAME" per file entry of a zip, sorted by name (python's zipfile)
zip_entries() {
  python3 -I - "$1" <<'PY'
import hashlib, sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as z:
    # each entry by its own ZipInfo (two of one name are two rows, each its own bytes), in name
    # order; a name with a control character could forge a row: refused (review TR2)
    for i in sorted((i for i in z.infolist() if not i.filename.endswith("/")), key=lambda i: i.filename):
        if any(ord(c) < 0x20 or ord(c) == 0x7f for c in i.filename):
            sys.exit("a zip entry's name holds a control character")
        print(hashlib.sha256(z.read(i)).hexdigest(), i.filename)
PY
}
# entry_sha LIST NAME: an entry's SHA-256 from zip_entries' list, or nothing
entry_sha() { awk -v n="$2" '{ s = $1; sub(/^[^ ]+ /, "") } $0 == n { print s; exit }' "$1"; }
# gated FILE: the parts of a payload.json whose change needs a VM run (R4.I94), as one line:
# windows.compatible, windows.resampler_proved, model, previous_models
gated() {
  python3 -I -c 'import json, sys
d = json.load(open(sys.argv[1])); w = d.get("windows") or {}
print(json.dumps([w.get("compatible"), w.get("resampler_proved"), d.get("model"), d.get("previous_models")], sort_keys=True))' "$1"
}

# changed_inputs FROM LIST...: the paths on the input lists that changed in the public history
# from the commit FROM to this release's public tree, one per line (none: nothing printed)
changed_inputs() {
  local from="$1" rc=0; shift
  git --git-dir="$pub_git" diff-tree -r -z --name-only --no-renames "$from" "$tree" > "$tmp/diff-names" \
    || die "couldn't compare $from with the public tree"
  python3 -I "$root/scripts/mac-inputs.py" changed "$@" < "$tmp/diff-names" || rc=$?
  [ "$rc" -le 1 ] || die "couldn't read the input lists $*"
}

# decide: the carry decisions: the Mac app and bridge (decide_mac), the Windows files
# (decide_windows)
decide() {
  decide_mac
  decide_windows
}

# decide_mac: the Mac decisions into dec_app, dec_bridge (new or carry) with app_why, bridge_why
decide_mac() {
  local d="$tmp/decide" c rc f list
  dec_app=new; dec_bridge=new
  prev_get
  if [ "$prev_tag" = none ]; then
    app_why="no release yet"; bridge_why="no release yet"; return 0
  fi
  for c in "$prev_app_commit" "$prev_bridge_commit"; do
    git --git-dir="$pub_git" cat-file -e "$c^{commit}" 2>/dev/null \
      || die "$c, which built a Mac file of $prev_tag, isn't in the public clone $pub: its inputs can't be compared"
  done
  if [ "$mac" = keep ]; then
    dec_app=carry; app_why="--mac=keep (carried even if this commit builds other bytes)"
    dec_bridge=carry; bridge_why="--mac=keep"
    return 0
  fi
  # the public files, as step 5 builds them, in a scratch folder with this checkout's download
  # caches (the builds check their SHA-256s)
  rm -rf "$d"; mkdir -p "$d/src/build"
  (cd "$stage" && tar -cf - .) | (cd "$d/src" && tar -xf -) || die "couldn't copy the public files to $d"
  for c in flac sqlcipher pyrekordbox; do
    [ ! -d "$root/build/$c" ] || cp -R "$root/build/$c" "$d/src/build/$c"
  done
  if [ "$mac" = new ]; then
    app_why="--mac=new"
  else
    echo "== the Mac app, stamped $prev_app_version, built again from the public copy (log: $d/app.log)"
    (cd "$d/src" && env -u RBSTEMSPLUS_TEST_BASE_URL -u RBSTEMSPLUS_TEST_KEY -u RBSTEMSPLUS_SCREENSHOTS -u RBSP_RESEARCH -u RBSTEMSPLUS_TEST_BROKEN_SELFTEST \
       RBSTEMSPLUS_APP_VERSION="$prev_app_version" scripts/build-app.sh "$d/out") > "$d/app.log" 2>&1 \
      || { tail -30 "$d/app.log"; die "the Mac app didn't build from the public copy (above)"; }
    rc=0
    "$root/scripts/bundle-compare.sh" "$d/out/rbstemsplus-app.zip" "$tmp/prev/$prev_app_zip" > "$d/compare.log" 2>&1 || rc=$?
    [ "$rc" -le 2 ] || { cat "$d/compare.log"; die "the bundle compare failed (above)"; }
    [ "$rc" = 0 ] || sed 's/^/  /' "$d/compare.log"
    changed_inputs "$prev_app_commit" "$d/src/build/app/rbstemsplus-app.zip.inputs" > "$d/app.changed"
    if [ "$rc" = 0 ]; then
      dec_app=carry; app_why="the same files as $prev_tag's zip"
    elif [ "$rc" = 1 ]; then
      # R10's rule: the same code; each set-aside asset catalog's inputs must be unchanged
      : > "$d/setaside.changed"
      while IFS= read -r f; do
        list="$d/src/build/app/$(basename "$f").inputs"
        [ -s "$list" ] || { echo "$f (no input list)" >> "$d/setaside.changed"; continue; }
        changed_inputs "$prev_app_commit" "$list" >> "$d/setaside.changed"
      done < <(sed -n 's/^set-aside: //p' "$d/compare.log")
      if [ ! -s "$d/setaside.changed" ]; then
        dec_app=carry; app_why="the same code as $prev_tag's zip (only the asset catalog, signatures and seal differ), and the asset catalog's inputs unchanged since $prev_app_commit"
      fi
    fi
    if [ "$dec_app" = new ]; then
      if [ ! -s "$d/app.changed" ]; then
        dec_app=carry; app_why="the files differ (above), but no Mac app input changed since $prev_app_commit: the toolchain's drift only"
      else
        app_why="Mac app inputs changed since $prev_app_commit: $(tr '\n' ' ' < "$d/app.changed")"
      fi
    fi
  fi
  echo "== the Mac bridge, built again from the public copy (log: $d/bridge.log)"
  (cd "$d/src" && bridge/build.sh "$d/bridge") > "$d/bridge.log" 2>&1 \
    || { tail -30 "$d/bridge.log"; die "the Mac bridge didn't build from the public copy (above)"; }
  if [ "$(shasum -a 256 "$d/bridge/$prev_bridge_file" | cut -d' ' -f1)" = "$prev_bridge_sha" ]; then
    dec_bridge=carry; bridge_why="the same bytes as $prev_tag's"
  else
    changed_inputs "$prev_bridge_commit" "$d/bridge/$prev_bridge_file.inputs" > "$d/bridge.changed"
    if [ ! -s "$d/bridge.changed" ]; then
      dec_bridge=carry; bridge_why="other bytes, but no bridge input changed since $prev_bridge_commit: the toolchain's drift only"
    else
      bridge_why="bridge inputs changed since $prev_bridge_commit: $(tr '\n' ' ' < "$d/bridge.changed")"
    fi
  fi
}

# decide_windows: win (1: this release has Windows files: --windows, or a previous release with
# a windows block), and with it dec_win (the zip), dec_stub_x64, dec_stub_arm64, dec_admin (the
# entries carried one by one, R4.I83) and dec_wbridge, each new or carry, with their whys
decide_windows() {
  local d="$tmp/decide-win" built bsha
  win=0; dec_win=none; dec_stub_x64=none; dec_stub_arm64=none; dec_admin=none; dec_wbridge=none
  win_why=""; wbridge_why=""
  if [ "$windows_flag" = 0 ] && [ "$prev_win" = 0 ]; then return 0; fi
  win=1; dec_win=new; dec_stub_x64=new; dec_stub_arm64=new; dec_admin=new; dec_wbridge=new
  if [ "$prev_win" = 0 ]; then
    win_why="the first release with Windows files (--windows)"; wbridge_why="the first Windows bridge"
    return 0
  fi
  [ -f "$stage/scripts/build-windows.sh" ] || die "$prev_tag has Windows files and this commit has no scripts/build-windows.sh to build or carry them (release.md T12)"
  rm -rf "$d"; mkdir -p "$d/src/build"
  (cd "$stage" && tar -cf - .) | (cd "$d/src" && tar -xf -) || die "couldn't copy the public files to $d"
  # this checkout's download caches, copied as the Mac side's are: the builds check each pinned
  # SHA-256 on use and download only what is missing or doesn't match, as the Mac builds do
  local c missing=""
  for c in $WIN_CACHES; do
    if [ -d "$root/build/$c" ]; then cp -R "$root/build/$c" "$d/src/build/$c" || die "couldn't copy build/$c to $d"
    else missing="$missing build/$c"; fi
  done
  [ -z "$missing" ] || echo "== not in this checkout:$missing: scripts/build-windows.sh downloads them (each pinned SHA-256 checked), as the Mac builds do"
  echo "== the Windows files, the app stamped $prev_win_version, built again from the public copy (log: $d/win.log)"
  (cd "$d/src" && scripts/build-windows.sh --stamp "$prev_win_version" "$d/out") > "$d/win.log" 2>&1 \
    || { tail -30 "$d/win.log"; die "the Windows files didn't build from the public copy (above)"; }
  [ -f "$d/out/$WIN_ZIP" ] && [ -f "$d/out/$WIN_BRIDGE" ] || die "scripts/build-windows.sh made no $WIN_ZIP or $WIN_BRIDGE"
  zip_entries "$d/out/$WIN_ZIP" > "$d/built-entries" || die "the built $WIN_ZIP can't be read as a zip"
  if cmp -s "$d/built-entries" "$tmp/prev/win-entries"; then
    dec_win=carry; dec_stub_x64=carry; dec_stub_arm64=carry; dec_admin=carry
    win_why="the same files as $prev_tag's zip"
  else
    win_why="files differ from $prev_tag's zip: $( { diff "$tmp/prev/win-entries" "$d/built-entries" || true; } | sed -n 's/^[<>] [0-9a-f]* //p' | LC_ALL=C sort -u | tr '\n' ',' | sed 's/,$//')"
    built="$(entry_sha "$d/built-entries" "$WIN_STUB_X64")"; [ "$built" != "$prev_win_stub_x64" ] || dec_stub_x64=carry
    built="$(entry_sha "$d/built-entries" "$WIN_STUB_ARM64")"; [ "$built" != "$prev_win_stub_arm64" ] || dec_stub_arm64=carry
    built="$(entry_sha "$d/built-entries" "$WIN_ADMIN")"; [ "$built" != "$prev_win_admin" ] || dec_admin=carry
  fi
  bsha="$(shasum -a 256 "$d/out/$WIN_BRIDGE" | cut -d' ' -f1)"
  if [ "$bsha" = "$prev_win_bridge_sha" ]; then dec_wbridge=carry; wbridge_why="the same bytes as $prev_tag's"
  else wbridge_why="other bytes than $prev_tag's"; fi
}

# the record step 4b asks for (I76, R4.I94): a VM run whenever a Windows file is new (vm_why)
vm_needed() {
  vm_why=""
  [ "$win" = 1 ] || return 1
  local k
  for k in "app:$dec_win" "x64 stub:$dec_stub_x64" "arm64 stub:$dec_stub_arm64" "worker:$dec_admin" "bridge:$dec_wbridge"; do
    [ "${k#*:}" != new ] || vm_why="${vm_why:+$vm_why, }the Windows ${k%%:*} is new"
  done
  [ -n "$vm_why" ]
}

# plan_lines: the decisions as build/gates/<commit>/plan holds them (scripts/build.sh --plan
# reads them); "#" lines say why and aren't compared
plan_lines() {
  echo "# scripts/release.sh $tag --plan (--mac=$mac): the carry decisions of $commit (release.md §1.3)"
  echo "plan 1"
  echo "commit $commit"
  echo "version $version"
  if [ "$prev_tag" = none ]; then echo "previous none"; else echo "previous $prev_tag $prev_sum"; fi
  echo "# mac.app: $app_why"
  if [ "$dec_app" = carry ]; then echo "mac.app carry $prev_app_version $prev_app_commit $prev_app_sha"; else echo "mac.app new"; fi
  echo "# mac.bridge: $bridge_why"
  if [ "$dec_bridge" = carry ]; then echo "mac.bridge carry $prev_bridge_commit $prev_bridge_sha"; else echo "mac.bridge new"; fi
  if [ "$win" = 0 ]; then
    echo "# windows: no Windows files (no --windows, and no windows block in the previous release)"
    echo "windows.app none"
    echo "windows.bridge none"
    return 0
  fi
  echo "# windows.app: $win_why"
  if [ "$dec_win" = carry ]; then echo "windows.app carry $prev_win_version $prev_win_sha"; else echo "windows.app new"; fi
  # the zip's stubs and worker, carried one by one into a new zip (R4.I83)
  if [ "$dec_stub_x64" = carry ]; then echo "windows.stub.x64 carry $prev_win_stub_x64"; else echo "windows.stub.x64 new"; fi
  if [ "$dec_stub_arm64" = carry ]; then echo "windows.stub.arm64 carry $prev_win_stub_arm64"; else echo "windows.stub.arm64 new"; fi
  if [ "$dec_admin" = carry ]; then echo "windows.admin carry $prev_win_admin"; else echo "windows.admin new"; fi
  echo "# windows.bridge: $wbridge_why"
  if [ "$dec_wbridge" = carry ]; then echo "windows.bridge carry $prev_win_bridge_sha"; else echo "windows.bridge new"; fi
}
print_decisions() {
  echo "The carry decisions (--mac=$mac; previous release: $prev_tag):"
  if [ "$dec_app" = carry ]; then echo "  Mac app         carried: $prev_app_version, built from $prev_app_commit ($app_why)"
  else echo "  Mac app         new: $version ($app_why)"; fi
  if [ "$dec_bridge" = carry ]; then echo "  Mac bridge      carried: built from $prev_bridge_commit ($bridge_why)"
  else echo "  Mac bridge      new ($bridge_why)"; fi
  if [ "$win" = 0 ]; then
    echo "  Windows app     none (no Windows files: no --windows, and none in the previous release)"
    echo "  Windows bridge  none"
    return 0
  fi
  if [ "$dec_win" = carry ]; then echo "  Windows app     carried: $prev_win_version ($win_why)"
  else echo "  Windows app     new: $version ($win_why)"; fi
  echo "    x64 stub      $dec_stub_x64; arm64 stub $dec_stub_arm64; worker $dec_admin"
  if [ "$dec_wbridge" = carry ]; then echo "  Windows bridge  carried ($wbridge_why)"; else echo "  Windows bridge  new ($wbridge_why)"; fi
  if vm_needed; then echo "  VM record       vm-win.ok needed: $vm_why"
  else echo "  VM record       vm-win.ok needed only if the TEST payload's lists differ from $prev_tag's (R4.I94: the real run checks)"; fi
  echo "  Windows assets  $WIN_ASSETS"
}

if [ "$dry$plan" != 00 ]; then
  echo
  if [ -n "$reuse" ]; then
    echo "The public clone already has this release's commit, $reuse."
  else
    echo "Would commit in $pub, on top of ${base:-nothing (the first commit)}:"
    echo "  \"$msg\", author and committer $ident"
    echo "  changes to the public files:"
    if [ -n "$base" ]; then git --git-dir="$pub_git" diff-tree -r --stat "$base" "$tree" | sed 's/^/    /'
    else git --git-dir="$pub_git" ls-tree -r --name-only "$tree" | sed 's/^/    new: /'; fi
  fi
  echo "== the carry decisions"
  if [ "$dry" = 1 ] && { ! command -v gh > /dev/null || ! gh auth status > /dev/null 2>&1; }; then
    echo "NOTE: gh isn't installed or signed in: the carry decisions (they read the latest release) aren't shown"
  else
    decide
    print_decisions
  fi
  if [ "$plan" = 1 ]; then
    mkdir -p "$(dirname "$plan_file")"
    [ ! -f "$plan_file" ] || echo "NOTE: replacing the earlier plan of this commit: gate records made for it no longer count"
    plan_lines > "$plan_file.part" && mv -f "$plan_file.part" "$plan_file" || die "couldn't write $plan_file"
    echo "Plan written: $plan_file"
    echo "Next: the TEST build in its shape, scripts/build.sh --test URL --plan $plan_file, and the gates on it;"
    echo "then the release with the same flags: scripts/release.sh $tag$( [ "$mac" = auto ] || echo " --mac=$mac")$( [ "$branch" = main ] || echo " --branch $branch")$( [ "$windows_flag" = 0 ] || echo " --windows")"
    if [ "$win" = 1 ] && vm_needed; then echo "The VM record is needed (step 4b): $vm_why: run each suite in tests/win/suites/release.list with --record (tests/win/vm-run.sh run SNAPSHOT SUITE --record) on that TEST build"; fi
    echo "Plan run: nothing was committed, tagged, pushed or uploaded."
  else
    echo "Dry run: nothing was committed, tagged, pushed or uploaded (the Mac files were built only in a scratch folder)."
  fi
  exit 0
fi

if [ -n "$reuse" ]; then
  pub_commit="$reuse"
  echo "the public clone has this release's commit from an earlier run: $pub_commit"
else
  pub_commit="$(GIT_AUTHOR_NAME="$PUBLIC_NAME" GIT_AUTHOR_EMAIL="$email" GIT_COMMITTER_NAME="$PUBLIC_NAME" GIT_COMMITTER_EMAIL="$email" \
                git --git-dir="$pub_git" commit-tree --no-gpg-sign "$tree" ${base:+-p "$base"} -m "$msg")" || die "couldn't commit in $pub"
  git -C "$pub" checkout -q -B main "$pub_commit" || die "couldn't move $pub's main to $pub_commit"
  echo "committed $pub_commit in $pub (not pushed)"
fi
[ "$(git -C "$pub" log -1 --format='%an <%ae>|%cn <%ce>' "$pub_commit")" = "$ident|$ident" ] || die "the public commit isn't by $ident"
pub_tag_here=0
if git -C "$pub" rev-parse -q --verify "refs/tags/$tag" > /dev/null; then
  [ "$(git -C "$pub" rev-parse "$tag^{commit}")" = "$pub_commit" ] || die "the tag $tag in $pub points at $(git -C "$pub" rev-parse "$tag^{commit}"), not $pub_commit"
  [ "$(git -C "$pub" for-each-ref --format='%(taggername) %(taggeremail)' "refs/tags/$tag")" = "$ident" ] \
    || die "the tag $tag in $pub isn't an annotated tag by $ident: delete it (git -C \"$pub\" tag -d $tag), then run this again"
  pub_tag_here=1
fi
pub_remote="$(git -C "$pub" ls-remote --tags origin "refs/tags/$tag" "refs/tags/$tag^{}" \
              | awk -v a="refs/tags/$tag^{}" -v b="refs/tags/$tag" '$2 == a { p = $1 } $2 == b { q = $1 } END { print (p != "" ? p : q) }')" \
  || die "couldn't read the public origin's tags"
[ -z "$pub_remote" ] || [ "$pub_remote" = "$pub_commit" ] || die "the tag $tag on $REPO points at $pub_remote, not $pub_commit"

# ------------------------------------------------------------------ 5. build

echo "== the carry decisions"
decide
print_decisions
plan_lines > "$tmp/plan"
grep -v '^#' "$plan_file" > "$tmp/plan.want" || true
grep -v '^#' "$tmp/plan" > "$tmp/plan.got" || true
if ! cmp -s "$tmp/plan.want" "$tmp/plan.got"; then
  diff "$tmp/plan.want" "$tmp/plan.got" | sed 's/^/  /' || true
  die "the carry decisions differ from the plan's ($plan_file; < the plan, > now): the gates ran on another shape. Make the plan again (--plan, with the same --mac), then the TEST build and the gates in its shape"
fi
echo "the same decisions as the plan ($plan_file)"

# G3 (release.md §6.1, T26): the tart run's record of this commit, for the TEST payload in
# dist-test/ and this plan, every required scenario passed (from main and with --branch alike)
echo "== the G3 tart run (tests/mac/g3.sh check)"
"$root/tests/mac/g3.sh" check "$commit" "$root/dist-test" \
  || die "no passing G3 tart run for $commit: tests/mac/g3.sh run on its TEST build (scripts/build.sh --test URL --plan $plan_file), then run this again"

# step 4b (release.md §6, I76, R4.I94): the VM record, when a Windows file is new or the TEST
# payload's lists aren't the previous release's
assets="$ASSETS"
test_ps=""
if [ "$win" = 1 ]; then
  assets="$ASSETS $WIN_ASSETS"
  [ -f "$root/dist-test/payload.json" ] && python3 -I -c 'import json, sys; sys.exit(0 if isinstance(json.load(open(sys.argv[1])).get("windows"), dict) else 1)' "$root/dist-test/payload.json" 2>/dev/null \
    || die "dist-test/payload.json (the TEST build) has no windows block: make the TEST build in this plan's shape (scripts/build.sh --test URL --plan $plan_file)"
  # the TEST payload the VM record is checked against; step 6 binds the release's Windows
  # binaries to it (review TT1)
  test_ps="$(shasum -a 256 "$root/dist-test/payload.json" | cut -d' ' -f1)"
  # a new worker: the release worker the TEST build made (review TT1, BM1), bound in step 6
  test_rel=""; test_rel_admin=""
  if [ "$dec_admin" = new ]; then
    rel="$root/dist-test/windows-release.sha256"
    [ -f "$rel" ] && [ ! -L "$rel" ] && grep -qxE 'admin [0-9a-f]{64}' "$rel" && [ "$(grep -c '' "$rel")" = 1 ] \
      || die "dist-test/windows-release.sha256 (admin SHA256: the release worker the TEST build made) is missing or not one such line: make the TEST build in this plan's shape (scripts/build.sh --test URL --plan $plan_file)"
    test_rel="$(shasum -a 256 "$rel" | cut -d' ' -f1)"; test_rel_admin="$(sed -n 's/^admin //p' "$rel")"
  fi
  vm_needed || true
  if [ "$prev_win" = 1 ] && [ "$(gated "$root/dist-test/payload.json")" != "$(gated "$tmp/prev/payload.json")" ]; then
    vm_why="${vm_why:+$vm_why, }the TEST payload's lists (windows compatible or resampler_proved, the model, previous_models) differ from $prev_tag's"
  fi
  if [ -n "$vm_why" ]; then
    echo "== the VM record (tests/win/vm-run.sh check): $vm_why"
    [ -x "$root/tests/win/vm-run.sh" ] || die "no tests/win/vm-run.sh to check the VM record with"
    # the check reads and writes the gates folder these name, not the build/gates/ read below
    # (review RV2); VMRUN_SHARE only moves the lock case 14 checks
    [ -z "${VMRUN_ROOT:-}" ] && [ -z "${VMRUN_GATES:-}" ] \
      || die "VMRUN_ROOT or VMRUN_GATES is set (a test variable): unset it, then run this again"
    [ -z "${VMRUN_SHARE:-}" ] || echo "== VMRUN_SHARE is set: the lock checked is $VMRUN_SHARE's"
    # an old vm-win.ok is no answer: only what this check writes is read
    vm_ok="$plan_dir/$commit/vm-win.ok"; rm -f "$vm_ok"
    # exit 1: no passing record (said); exit 2: it couldn't check, a VM run holding the share's lock
    vm_rc=0
    "$root/tests/win/vm-run.sh" check "$commit" "$root/dist-test" || vm_rc=$?
    [ "$vm_rc" != 2 ] \
      || die "tests/win/vm-run.sh check couldn't check the VM record (exit 2): a VM run holds the share's lock (or the lock couldn't be taken, said above); let that run end, then run this again"
    [ "$vm_rc" = 0 ] \
      || die "no passing VM run for $commit (build/gates/$commit/vm-win/): run each suite in tests/win/suites/release.list with --record (tests/win/vm-run.sh run SNAPSHOT SUITE --record) on its TEST build, then run this again"
    # what the check wrote is for this commit and the TEST payload step 6 binds to, not only its exit 0
    grep -qx "commit $commit" "$vm_ok" 2>/dev/null && grep -qx "payload_sha256 $test_ps" "$vm_ok" && grep -qx "result pass" "$vm_ok" \
      || die "tests/win/vm-run.sh check passed, but $vm_ok isn't a pass for $commit and dist-test/payload.json ($test_ps): run the check again"
    # what step 6 checks again before signing (review RV1)
    cp "$vm_ok" "$tmp/vm-win.ok.4b"
  else
    echo "== the VM record: not needed (every Windows file carried, the lists unchanged)"
  fi
fi

echo "== build (scripts/build.sh, in $pub)"
dist="$pub/dist"
# the model and libFLAC's tarball, from this repository when the public clone has none
# (scripts/build.sh and bridge/build.sh check both against their checksums)
if [ ! -f "$dist/stemsplus-model.onnx" ] && [ -f "$root/dist/stemsplus-model.onnx" ]; then
  mkdir -p "$dist"
  ln "$root/dist/stemsplus-model.onnx" "$dist/stemsplus-model.onnx" 2>/dev/null || cp "$root/dist/stemsplus-model.onnx" "$dist/stemsplus-model.onnx"
fi
for t in "$root"/build/flac/flac-*.tar.xz; do
  [ -f "$t" ] && [ ! -f "$pub/build/flac/$(basename "$t")" ] || continue
  mkdir -p "$pub/build/flac" && cp "$t" "$pub/build/flac/"
done
# with Windows files, the Windows build's download caches too, as the carry decision's (review
# TT9): this checkout's copied over the public clone's (each file is a pinned download the build
# checks), never the unpacked llvm-mingw/ or dotnet/; one this checkout lacks is said, and the
# build downloads it (its pin checked), as the Mac builds do
if [ "$win" = 1 ]; then
  missing=""
  for c in $WIN_CACHES; do
    if [ -d "$root/build/$c" ]; then
      mkdir -p "$pub/build/$c" && cp -R "$root/build/$c/." "$pub/build/$c/" || die "couldn't copy build/$c to $pub/build"
    else missing="$missing build/$c"; fi
  done
  [ -z "$missing" ] || echo "== not in this checkout:$missing: the public clone's build downloads them (each pinned SHA-256 checked), as the Mac builds do"
fi
# in the plan's shape: a carried file is the previous release's, from its verified download
if [ "$prev_tag" = none ]; then
  "$pub/scripts/build.sh" --plan "$tmp/plan" || die "the build failed (see above)"
else
  "$pub/scripts/build.sh" --plan "$tmp/plan" --carry "$tmp/prev" || die "the build failed (see above)"
fi

# ------------------------------------------------------------------ 6. what was built

echo "== dist/ (in $pub)"
p() { plutil -extract "$1" raw -o - "$dist/payload.json" 2>/dev/null || echo "(missing)"; }
for f in $assets; do [ -f "$dist/$f" ] && [ ! -L "$dist/$f" ] || die "dist/$f is missing"; done
# no test key ships (review BM3): build.sh's own guards read build/test-keys/ in the public clone,
# where there is none. Here: this checkout's test key and every gate-archive tag's (the keys
# TEST builds were signed with), each first against the public commit's keys/*.pub.pem (a release
# build trusts only those, and build-windows.sh turns them into the uncompressed point: so the
# point, not the PEM's bytes, is compared), then against bootstrap.sh, bootstrap.ps1 and the app's
# executable in rbstemsplus-app.zip, which hold a key as its DER in base64, in any point form
tk_b64() {
  local text form b
  text="$(/usr/bin/openssl ec -pubin -in "$1" -noout -text 2>/dev/null)" && grep -q 'ASN1 OID: prime256v1' <<< "$text" || return 1
  for form in uncompressed compressed hybrid; do
    b="$(/usr/bin/openssl ec -pubin -in "$1" -conv_form "$form" -outform DER 2>/dev/null | /usr/bin/base64 | tr -d '\n')"
    [ -n "$b" ] || return 1
    echo "$b"
  done
}
python3 -I -c '
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
exe = [i for i in z.infolist() if i.filename.count("/") == 3 and i.filename.endswith(".app/Contents/MacOS/RB Stems Plus")]
if len(exe) != 1:
    sys.exit(1)
open(sys.argv[2], "wb").write(z.read(exe[0]))' "$dist/rbstemsplus-app.zip" "$tmp/app-exe" 2>/dev/null \
  || die "dist/rbstemsplus-app.zip has no single app executable (RB Stems Plus.app/Contents/MacOS/RB Stems Plus) to check for a test key"
for n in release backup; do
  git -C "$pub" show "$pub_commit:keys/$n.pub.pem" > "$tmp/tk-$n.pub.pem" 2>/dev/null && tk_b64 "$tmp/tk-$n.pub.pem" > /dev/null \
    || die "keys/$n.pub.pem at the public commit isn't a P-256 public key"
done
for k in "$root/build/test-keys/test.pub.pem" "${GATES_ARCHIVE:-$root/gates-archive}"/tag-*/test-keys/test.pub.pem; do
  [ -f "$k" ] || continue
  kb="$(tk_b64 "$k")" || die "$k isn't a P-256 public key: the test-key check can't run"
  for n in release backup; do
    [ "${kb%%$'\n'*}" != "$(tk_b64 "$tmp/tk-$n.pub.pem" | head -1)" ] || die "keys/$n.pub.pem is the test key $k: not releasing it"
  done
  for f in "$dist/bootstrap.sh" "$dist/bootstrap.ps1" "$tmp/app-exe"; do
    [ -f "$f" ] || continue
    ! grep -aqF "$kb" "$f" \
      || die "$([ "$f" = "$tmp/app-exe" ] && echo "the app in dist/rbstemsplus-app.zip" || echo "dist/${f##*/}") trusts the test key $k: not releasing it"
  done
done
[ "$(p commit)" = "$pub_commit" ] || die "dist/payload.json names commit $(p commit), not the public commit ($pub_commit)"
want_app_version="$version"; want_app_commit="$pub_commit"; want_bridge_commit="$pub_commit"
if [ "$dec_app" = carry ]; then
  want_app_version="$prev_app_version"; want_app_commit="$prev_app_commit"
  [ "$(p app.sha256)" = "$prev_app_sha" ] && cmp -s "$dist/rbstemsplus-app.zip" "$tmp/prev/$prev_app_zip" \
    || die "dist/rbstemsplus-app.zip isn't $prev_tag's, which the plan carries"
fi
if [ "$dec_bridge" = carry ]; then
  want_bridge_commit="$prev_bridge_commit"
  [ "$(p bridge.sha256)" = "$prev_bridge_sha" ] && cmp -s "$dist/libonnxruntime.1.18.0.dylib" "$tmp/prev/$prev_bridge_file" \
    || die "dist/libonnxruntime.1.18.0.dylib isn't $prev_tag's, which the plan carries"
fi
[ "$(p payload_version)" = "$version" ] && [ "$(p app.version)" = "$want_app_version" ] \
  || die "dist/payload.json isn't version $version with the app $want_app_version"
[ "$(p app.commit)" = "$want_app_commit" ] && [ "$(p bridge.commit)" = "$want_bridge_commit" ] \
  || die "dist/payload.json's app.commit or bridge.commit isn't $want_app_commit and $want_bridge_commit"
# the windows block (release.md §1.2, §6 step 6), or none at all
haswin="$(python3 -I -c 'import json, sys; print(1 if "windows" in json.load(open(sys.argv[1])) else 0)' "$dist/payload.json" 2>/dev/null || echo "?")"
if [ "$win" = 0 ]; then
  [ "$haswin" = 0 ] || die "dist/payload.json has a windows block, which the plan doesn't"
  for f in $WIN_ASSETS; do [ ! -e "$dist/$f" ] || die "dist/$f is there, but this release has no Windows files"; done
else
  [ "$haswin" = 1 ] || die "dist/payload.json has no windows block (this release has Windows files)"
  for f in $WIN_ASSETS; do [ -f "$dist/$f" ] && [ ! -L "$dist/$f" ] || die "dist/$f is missing"; done
  want_win_version="$version"
  if [ "$dec_win" = carry ]; then
    want_win_version="$prev_win_version"
    cmp -s "$dist/$WIN_ZIP" "$tmp/prev/$prev_win_zip" || die "dist/$WIN_ZIP isn't $prev_tag's, which the plan carries"
  fi
  [ "$(p windows.app.version)" = "$want_win_version" ] && [ "$(p windows.app.zip)" = "$WIN_ZIP" ] \
    && [ "$(p windows.app.sha256)" = "$(shasum -a 256 "$dist/$WIN_ZIP" | cut -d' ' -f1)" ] \
    || die "dist/payload.json's windows.app isn't version $want_win_version with $WIN_ZIP's SHA-256"
  zip_entries "$dist/$WIN_ZIP" > "$tmp/dist-win-entries" || die "dist/$WIN_ZIP can't be read as a zip"
  for e in "x64:$WIN_STUB_X64:$dec_stub_x64:${prev_win_stub_x64:-}" "arm64:$WIN_STUB_ARM64:$dec_stub_arm64:${prev_win_stub_arm64:-}" \
           "admin:$WIN_ADMIN:$dec_admin:${prev_win_admin:-}"; do
    IFS=: read -r k name how prevsha <<< "$e"
    if [ "$k" = admin ]; then got="$(p windows.app.admin_sha256)"; else got="$(p "windows.app.stub_sha256.$k")"; fi
    # exactly one entry of that name, and a hash that is one (review TT2)
    [ "$(awk -v n="$name" '{ sub(/^[^ ]+ /, "") } $0 == n { c++ } END { print c + 0 }' "$tmp/dist-win-entries")" = 1 ] \
      || die "dist/$WIN_ZIP doesn't hold exactly one $name"
    [[ "$got" =~ ^[0-9a-f]{64}$ ]] || die "dist/payload.json's SHA-256 of $name isn't 64 hex digits"
    [ "$got" = "$(entry_sha "$tmp/dist-win-entries" "$name")" ] || die "dist/payload.json's SHA-256 of $name isn't the zip's"
    [ "$how" != carry ] || [ "$got" = "$prevsha" ] || die "dist/$WIN_ZIP's $name isn't $prev_tag's, which the plan carries"
  done
  # and no two entries one name as the updater compares them (unzip.c, review BR1): same_name folds
  # ASCII case and drops a trailing '/', and file_is_folder_of refuses a file that is another
  # entry's folder (the three entries' exact names were each counted once above)
  python3 -I -c 'import sys, zipfile
n = zipfile.ZipFile(sys.argv[1]).namelist()
u = [x.encode().upper() for x in n]
k = [x[:-1] if x.endswith(b"/") else x for x in u]
f = [x for x in u if not x.endswith(b"/")]
sys.exit(0 if len(set(k)) == len(k) and not any(y.startswith(x + b"/") for x in f for y in u) else 1)' "$dist/$WIN_ZIP" \
    || die "dist/$WIN_ZIP has entry names that collide as the updater compares them (case, a trailing '/', a file that is a folder)"
  [ "$(p windows.bridge.file)" = "$WIN_BRIDGE" ] && [ "$(p windows.bridge.sha256)" = "$(shasum -a 256 "$dist/$WIN_BRIDGE" | cut -d' ' -f1)" ] \
    || die "dist/payload.json's windows.bridge isn't $WIN_BRIDGE with its SHA-256"
  [ "$dec_wbridge" != carry ] || cmp -s "$dist/$WIN_BRIDGE" "$tmp/prev/$prev_win_bridge_file" || die "dist/$WIN_BRIDGE isn't $prev_tag's, which the plan carries"
  # the TEST payload step 4b read, on every Windows release (review TR1): the VM decision and
  # the lists were taken on that very file
  [ "$(shasum -a 256 "$root/dist-test/payload.json" | cut -d' ' -f1)" = "$test_ps" ] \
    || die "dist-test/payload.json changed after step 4b read it (the VM decision and the lists were taken on that file)"
  # the lists the gates ran on (R4.I94): the TEST payload's
  [ "$(gated "$dist/payload.json")" = "$(gated "$root/dist-test/payload.json")" ] \
    || die "dist/payload.json's lists (windows compatible or resampler_proved, the model, previous_models) aren't the TEST build's: the gates ran on others"
  # a new worker is the release worker the TEST build made beside its test-key one (review TT1,
  # BM1): the reference step 4b read, unchanged, names dist's worker
  if [ "$dec_admin" = new ]; then
    [ -f "$root/dist-test/windows-release.sha256" ] && [ ! -L "$root/dist-test/windows-release.sha256" ] && [ "$(shasum -a 256 "$root/dist-test/windows-release.sha256" | cut -d' ' -f1)" = "$test_rel" ] \
      || die "dist-test/windows-release.sha256 changed after step 4b read it"
    [ "$(p windows.app.admin_sha256)" = "$test_rel_admin" ] \
      || die "dist/payload.json's windows.app.admin_sha256 isn't the release worker the TEST build made"
  fi
  # what the VM ran (review TT1): the bridge and both stubs are the TEST build's bytes (they hold
  # no test key or URL); the worker is bound above
  if [ -n "$vm_why" ]; then
    tp() { plutil -extract "$1" raw -o - "$root/dist-test/payload.json" 2>/dev/null || echo "(missing)"; }
    for k in windows.bridge.sha256 windows.app.stub_sha256.x64 windows.app.stub_sha256.arm64; do
      [ "$(p "$k")" = "$(tp "$k")" ] || die "dist/payload.json's $k isn't the TEST build's, which the VM ran"
    done
  fi
fi
(cd "$dist" && shasum -a 256 -s -c SHA256SUMS) || die "dist/SHA256SUMS doesn't match the files"
listed="$(awk '{print $2}' "$dist/SHA256SUMS" | LC_ALL=C sort | tr '\n' ' ')"
[ "$listed" = "$(printf '%s\n' $assets | grep -vx SHA256SUMS | LC_ALL=C sort | tr '\n' ' ')" ] || die "dist/SHA256SUMS lists: $listed"
[ "$(git -C "$pub" rev-parse HEAD)" = "$pub_commit" ] && [ -z "$(git -C "$pub" status --porcelain)" ] || die "the public clone changed during the build"
[ "$(git -C "$root" rev-parse HEAD)" = "$commit" ] && [ -z "$(git -C "$root" status --porcelain)" ] || die "the checkout changed during the build"
# the VM verdict again, right before signing (review RV1): a --record run that failed during the
# build leaves no pass, and the summary must be the very one step 4b read
if [ "$win" = 1 ] && [ -n "${vm_why:-}" ]; then
  echo "== the VM record again (tests/win/vm-run.sh check)"
  vm_rc=0
  "$root/tests/win/vm-run.sh" check "$commit" "$root/dist-test" || vm_rc=$?
  [ "$vm_rc" != 2 ] \
    || die "tests/win/vm-run.sh check couldn't check the VM record (exit 2): a VM run holds the share's lock (or the lock couldn't be taken, said above); let that run end, then run this again"
  [ "$vm_rc" = 0 ] && cmp -s "$vm_ok" "$tmp/vm-win.ok.4b" \
    || die "the VM record changed during the build (tests/win/vm-run.sh check exit $vm_rc, or $vm_ok isn't the one step 4b read): run each suite in tests/win/suites/release.list with --record on the TEST build again, then run this again"
fi

# ------------------------------------------------------------------ 7. the signature

echo "== signing payload.json"
sig="$dist/payload.json.sig"
rm -f "$sig"
if [ "$backup" = 1 ]; then
  echo "Paste the backup private key from your password manager, then press Return."
  echo "It isn't shown, and it is never written to disk."
  pem="" line="" cr=$'\r'
  while IFS= read -rs line; do
    line="${line%"$cr"}"
    [ -n "$line" ] || { [ -n "$pem" ] && break; continue; }
    pem="$pem$line"$'\n'
    case "$line" in "-----END "*) break ;; esac
  done
  while IFS= read -rs -t 1 line; do :; done      # the Return pressed after the paste, and anything after it
  echo
  # through a pipe only: printf is a shell builtin, so the key is in no command line
  printf '%s' "$pem" | /usr/bin/openssl dgst -sha256 -sign /dev/stdin -out "$sig" "$dist/payload.json" 2>/dev/null \
    || { pem=""; rm -f "$sig"; die "that isn't a usable private key"; }
  pem=""
else
  echo "openssl asks for the everyday key's passphrase:"
  /usr/bin/openssl dgst -sha256 -sign "$key" -out "$sig" "$dist/payload.json" || { rm -f "$sig"; die "not signed (wrong passphrase?)"; }
fi
signer=""
for k in release backup; do
  git -C "$pub" show "$pub_commit:keys/$k.pub.pem" > "$tmp/$k.pub.pem" 2>/dev/null || die "$pub_commit has no keys/$k.pub.pem"
  if /usr/bin/openssl dgst -sha256 -verify "$tmp/$k.pub.pem" -signature "$sig" "$dist/payload.json" > /dev/null 2>&1; then signer="$k"; break; fi
done
[ -n "$signer" ] || { rm -f "$sig"; die "the signature doesn't verify with keys/*.pub.pem at the public commit: that key isn't one of them"; }
echo "signed with the $signer key"

# ------------------------------------------------------------------ 8. the tags and the push

echo
echo "Release $tag: commit $commit here (on $branch), published as $pub_commit in $REPO ($count files, by $ident), signed with the $signer key."
if [ -f "$root/dist-test/payload.json" ]; then
  tested="$(plutil -extract commit raw -o - "$root/dist-test/payload.json" 2>/dev/null || echo "?")"
  [ "$tested" = "$commit" ] && echo "dist-test/ (the TEST build) is from the same commit." \
    || echo "NOTE: dist-test/ (the TEST build) is from $tested, not this commit."
fi
print_decisions
echo "The TEST build of this commit must have passed the VM tests (docs/RELEASING.md), built in this plan's shape."
[ "$branch" = main ] || echo "A release from $branch: its G3 tart run (from 1.0.0, 1.3.0 and the 1.3.x) was checked above."
if [ "$tag_here" = 0 ]; then echo "The tag $tag is made here (annotated)."; fi
if [ -z "$remote" ]; then echo "The tag $tag is pushed to origin."; fi
if [ "$pub_tag_here" = 0 ]; then echo "The tag $tag is made in the public clone (annotated, by $PUBLIC_NAME)."; fi
echo "The public main ($pub_commit) and the tag $tag are pushed to $pub_url."
echo "Then a DRAFT release $tag is created on $REPO with these files. Nothing is published."
read -r -p "Type yes to go on: " answer
[ "$answer" = yes ] || die "stopped: nothing was pushed or uploaded (dist/ stays, with its signature; the public clone keeps its unpushed commit)"
if [ "$tag_here" = 0 ]; then
  git -C "$root" tag -a "$tag" -m "RB Stems Plus $version" "$commit" || die "couldn't make the tag $tag"
fi
if [ -z "$remote" ]; then
  git -C "$root" push origin "refs/tags/$tag" || die "couldn't push the tag $tag"
fi
if [ "$pub_tag_here" = 0 ]; then
  GIT_COMMITTER_NAME="$PUBLIC_NAME" GIT_COMMITTER_EMAIL="$email" \
    git -C "$pub" -c tag.gpgSign=false tag -a "$tag" -m "RB Stems Plus $version" "$pub_commit" || die "couldn't make the tag $tag in $pub"
fi
[ "$(git -C "$pub" for-each-ref --format='%(taggername) %(taggeremail)' "refs/tags/$tag")" = "$ident" ] \
  || die "the public tag $tag isn't an annotated tag by $ident"
git -C "$pub" push -q --atomic origin "$pub_commit:refs/heads/main" "refs/tags/$tag" || die "couldn't push the public main and tag $tag"

# ------------------------------------------------------------------ 9. the draft

echo "== the draft"
files=(); for f in $assets payload.json.sig; do files+=("$dist/$f"); done
notes=(--notes "Built and signed on the developer's Mac from $pub_commit ($signer key). Write the release notes, then publish by hand (docs/RELEASING.md).")
# GitHub keeps every line break of a release's notes as a <br>: the file, wrapped like every
# Markdown file here, goes up unwrapped (one line per paragraph and list item)
if [ -f "$pub/docs/release-notes/$version.md" ]; then
  python3 -I "$pub/scripts/unwrap-notes.py" "$pub/docs/release-notes/$version.md" > "$tmp/notes.md" \
    && [ -s "$tmp/notes.md" ] || die "couldn't unwrap docs/release-notes/$version.md"
  notes=(--notes-file "$tmp/notes.md")
fi
gh release create "$tag" "${files[@]}" --repo "$REPO" --draft --verify-tag --title "RB Stems Plus $version" "${notes[@]}" \
  || die "couldn't create the draft (if it was created half-way, delete it by hand, then run this again)"
[ "$(gh release view "$tag" --repo "$REPO" --json isDraft -q .isDraft)" = true ] || die "$tag isn't a draft"
want="$(for f in $assets payload.json.sig; do printf '%s %s\n' "$f" "$(stat -f %z "$dist/$f")"; done | LC_ALL=C sort)"
got="$(gh release view "$tag" --repo "$REPO" --json assets -q '.assets[] | "\(.name) \(.size)"' | LC_ALL=C sort)"
[ "$got" = "$want" ] || die "the draft's files aren't the ones built here: $(tr '\n' ' ' <<< "$got")"
gh release download "$tag" --repo "$REPO" --dir "$tmp/check" --pattern payload.json --pattern payload.json.sig \
  || die "couldn't download payload.json and its signature back"
cmp -s "$tmp/check/payload.json" "$dist/payload.json" && cmp -s "$tmp/check/payload.json.sig" "$sig" \
  || die "the draft's payload.json or payload.json.sig isn't the one made here"

echo
echo "Draft $tag is on $REPO, signed ($signer key), built from $pub_commit. It is NOT published."
echo "Next (docs/RELEASING.md): check the release notes, publish the draft by hand, then run the"
echo "real install command on a clean VM clone."

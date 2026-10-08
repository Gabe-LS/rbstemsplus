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
#   scripts/release.sh vX.Y.Z --dry-run         only steps 1, 3 and 4, with nothing committed:
#                                               shows what would be published and changes
#                                               nothing (no key, gh, Terminal or GitHub; it
#                                               fetches the public clone's origin)
#
# Once: set the public identity's email, your GitHub noreply address (GitHub › Settings › Emails),
#   git config rbstemsplus.publicEmail 12345678+Gabe-LS@users.noreply.github.com
# and clone the public repository next to this one:
#   git clone https://github.com/Gabe-LS/rbstemsplus.git "../RB Stems Plus (public)"
# The global git identity is never used for the public repository.
#
# In order, stopping at the first problem:
#   1. VERSION is X.Y.Z; no uncommitted or untracked file; the key file is this account's alone;
#      in Terminal;
#   2. (gh signed in, network) HEAD is on origin/main; the tag vX.Y.Z, here or on origin, points
#      at HEAD if it exists; GitHub has no release vX.Y.Z yet;
#   3. the public clone: rbstemsplus.publicEmail is a GitHub noreply address of Gabe-LS; the
#      clone's origin is Gabe-LS/rbstemsplus (and this repository's isn't); it is on main, clean,
#      and at its origin/main, or one commit ahead with this very release (an earlier run);
#   4. the public copy: HEAD's files (git archive) minus .publicignore's paths and .publicignore;
#      refused if a listed path is missing from HEAD or would still be published, if any
#      published file mentions a listed file by name, or holds this account's full or short name
#      or this repository's git name or email; the copy must be HEAD's files exactly (modes and
#      contents), minus the private ones. It is committed in the public clone as "RB Stems Plus
#      X.Y.Z", author and committer Gabe-LS <publicEmail>, unsigned, on top of origin/main, and
#      main is moved to it there (nothing is pushed yet); a tag vX.Y.Z there must point at it;
#   5. scripts/build.sh, from the public clone, builds every asset into its dist/, with its own
#      checks (a clean tree, the commit in payload.json, both public keys and no test key in the
#      app and the bootstrap, the hardened runtime, libFLAC's checksum, the model's);
#   6. dist/: payload.json names the public commit and X.Y.Z, SHA256SUMS matches the files;
#   7. payload.json is signed into dist/payload.json.sig (openssl asks for the everyday key's
#      passphrase itself: never on a command line, in the environment or in a log), and the
#      signature is checked with the public commit's keys/release.pub.pem and keys/backup.pub.pem;
#   8. you type "yes"; then the tag is made here if missing (annotated) and pushed if origin
#      doesn't have it; the public tag is made (annotated, by Gabe-LS) and the public main and
#      tag are pushed together;
#   9. gh creates the DRAFT release vX.Y.Z on Gabe-LS/rbstemsplus with the nine files (it fails
#      if the release exists; nothing is ever replaced), with docs/release-notes/X.Y.Z.md as its
#      notes when there is one, and the draft is checked: its files' names and sizes, and its
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

die() { echo "release.sh: $*" >&2; exit 1; }
usage() { echo "usage: $0 vX.Y.Z [--key PATH | --backup-key] [--public PATH] [--dry-run]" >&2; exit 2; }

[ $# -ge 1 ] || usage
tag="$1"; shift
key="$HOME/.rbstemsplus-signing/release-key.pem"
backup=0
dry=0
pub=""
while [ $# -gt 0 ]; do
  case "$1" in
    --key) [ $# -ge 2 ] || usage; key="$2"; shift 2 ;;
    --backup-key) backup=1; shift ;;
    --public) [ $# -ge 2 ] || usage; pub="$2"; shift 2 ;;
    --dry-run) dry=1; shift ;;
    *) usage ;;
  esac
done
[[ "$tag" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "the tag must be vX.Y.Z, not '$tag'"
version="${tag#v}"

# ------------------------------------------------------------------ 1. here

[ "$(tr -d '[:space:]' < "$root/VERSION")" = "$version" ] || die "VERSION says $(tr -d '[:space:]' < "$root/VERSION"), not $version"
changes="$(git -C "$root" status --porcelain)" || die "not a git checkout"
if [ "$dry" = 1 ]; then
  [ -z "$changes" ] || echo "NOTE: uncommitted or untracked files: the dry run copies HEAD, without them"
else
  [ -z "$changes" ] || die "uncommitted or untracked files (git status): a release is built only from a commit"
fi
commit="$(git -C "$root" rev-parse HEAD)"
if [ "$dry" = 0 ]; then
  if [ "$backup" = 0 ]; then
    [ -f "$key" ] && [ ! -L "$key" ] || die "no signing key at $key (scripts/make-signing-keys.sh, or --key PATH, or --backup-key)"
    [ -O "$key" ] || die "$key isn't this account's"
    case "$(stat -f %Lp "$key")" in 600|400) ;; *) die "$key can be read by other accounts: chmod 600 it" ;; esac
  fi
  [ -t 0 ] && [ -t 1 ] || die "run it yourself in Terminal: it asks for the key's passphrase and for a confirmation"
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
  gh auth status > /dev/null 2>&1 || die "gh isn't signed in: run gh auth login yourself first"
  echo "== the commit"
  git -C "$root" fetch -q origin main || die "couldn't fetch origin main"
  git -C "$root" merge-base --is-ancestor "$commit" origin/main || die "HEAD ($commit) isn't on origin/main: push it first"
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
  echo "$commit: on origin/main, VERSION $version, no release $tag yet"
fi

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
want="$(git -C "$root" ls-tree -r "$commit" | while IFS= read -r l; do is_private "${l#*$'\t'}" || printf '%s\n' "$l"; done)"
[ "$want" = "$(git --git-dir="$pub_git" ls-tree -r "$tree")" ] || die "the public copy isn't HEAD's files minus the private ones (a .gitattributes export rule?)"
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

if [ "$dry" = 1 ]; then
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
  echo "Dry run: nothing was committed, tagged, pushed, built or uploaded."
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
"$pub/scripts/build.sh" || die "the build failed (see above)"

# ------------------------------------------------------------------ 6. what was built

echo "== dist/ (in $pub)"
p() { plutil -extract "$1" raw -o - "$dist/payload.json" 2>/dev/null || echo "(missing)"; }
for f in $ASSETS; do [ -f "$dist/$f" ] && [ ! -L "$dist/$f" ] || die "dist/$f is missing"; done
[ "$(p commit)" = "$pub_commit" ] || die "dist/payload.json names commit $(p commit), not the public commit ($pub_commit)"
[ "$(p payload_version)" = "$version" ] && [ "$(p app.version)" = "$version" ] || die "dist/payload.json isn't version $version"
(cd "$dist" && shasum -a 256 -s -c SHA256SUMS) || die "dist/SHA256SUMS doesn't match the files"
listed="$(awk '{print $2}' "$dist/SHA256SUMS" | LC_ALL=C sort | tr '\n' ' ')"
[ "$listed" = "$(printf '%s\n' $ASSETS | grep -vx SHA256SUMS | LC_ALL=C sort | tr '\n' ' ')" ] || die "dist/SHA256SUMS lists: $listed"
[ "$(git -C "$pub" rev-parse HEAD)" = "$pub_commit" ] && [ -z "$(git -C "$pub" status --porcelain)" ] || die "the public clone changed during the build"
[ "$(git -C "$root" rev-parse HEAD)" = "$commit" ] && [ -z "$(git -C "$root" status --porcelain)" ] || die "the checkout changed during the build"

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
echo "Release $tag: commit $commit here, published as $pub_commit in $REPO ($count files, by $ident), signed with the $signer key."
if [ -f "$root/dist-test/payload.json" ]; then
  tested="$(plutil -extract commit raw -o - "$root/dist-test/payload.json" 2>/dev/null || echo "?")"
  [ "$tested" = "$commit" ] && echo "dist-test/ (the TEST build) is from the same commit." \
    || echo "NOTE: dist-test/ (the TEST build) is from $tested, not this commit."
fi
echo "The TEST build of this commit must have passed the VM tests (docs/RELEASING.md)."
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
files=(); for f in $ASSETS payload.json.sig; do files+=("$dist/$f"); done
notes=(--notes "Built and signed on the developer's Mac from $pub_commit ($signer key). Write the release notes, then publish by hand (docs/RELEASING.md).")
if [ -f "$pub/docs/release-notes/$version.md" ]; then notes=(--notes-file "$pub/docs/release-notes/$version.md"); fi
gh release create "$tag" "${files[@]}" --repo "$REPO" --draft --verify-tag --title "RB Stems Plus $version" "${notes[@]}" \
  || die "couldn't create the draft (if it was created half-way, delete it by hand, then run this again)"
[ "$(gh release view "$tag" --repo "$REPO" --json isDraft -q .isDraft)" = true ] || die "$tag isn't a draft"
want="$(for f in $ASSETS payload.json.sig; do printf '%s %s\n' "$f" "$(stat -f %z "$dist/$f")"; done | LC_ALL=C sort)"
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

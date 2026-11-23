#!/bin/sh
# deps.lock's reader and gate (release.md §9.1): the one way a Windows build downloads.
#
#   scripts/deps.sh fetch NAME          download record NAME into build/dl/NAME/ once, kept only
#                                       when its SHA-256 matches (no network when it already
#                                       does); prints the file's path
#   scripts/deps.sh group GROUP DIR     every download record of that group, copied into DIR
#   scripts/deps.sh licence NAME DIR    component record NAME's licence files under DIR (the
#                                       unpacked download, or the group's folder) match their
#                                       sha256s: "licence changed: re-read it" otherwise
#   scripts/deps.sh get NAME KEY        one value of a record
#   scripts/deps.sh check               offline (make check): the lock's syntax, the licence
#                                       allowlist, the notices named, no banned name, the Mac's
#                                       pins equal to their rows, and no other download in the
#                                       builds that use this script
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
lock="${RBSP_DEPS_LOCK:-$root/deps.lock}"
die() { echo "deps.sh: $*" >&2; exit 1; }
[ -f "$lock" ] || die "no $lock"

# get NAME KEY: the value of KEY in stanza [NAME] (empty when absent)
get() {
  awk -v want="[$1]" -v key="$2" '
    /^[[:space:]]*#/ { next }
    /^\[/ { in_ = ($0 == want); next }
    in_ && index($0, "=") {
      k = $0; sub(/[[:space:]]*=.*/, "", k)
      if (k == key) { v = $0; sub(/^[^=]*=[[:space:]]*/, "", v); sub(/[[:space:]]+$/, "", v); print v; exit }
    }' "$lock"
}
names() { sed -n 's/^\[\([^]]*\)\]$/\1/p' "$lock"; }
sha() { shasum -a 256 "$1" | cut -d' ' -f1; }

fetch() {
  n="$1"
  url="$(get "$n" url)" sum="$(get "$n" sha256)"
  [ -n "$url" ] && [ -n "$sum" ] || die "$n: no download record with a url and a sha256 in deps.lock"
  case "$url" in https://*) ;; *) die "$n: not an https url: $url" ;; esac
  d="$root/build/dl/$n"
  f="$d/$(basename "$url")"
  if [ ! -f "$f" ] || [ "$(sha "$f")" != "$sum" ]; then
    echo "downloading $url" >&2
    mkdir -p "$d"
    # A refused fetch mustn't remove another fetch's partial download.
    part="$(mktemp "$f.part.XXXXXX")" || die "$n: download failed: $url"
    trap 'rm -f "$part"' EXIT
    # netguard: bounded (connect timeout, stall limit), retried when transient, reported when not
    h="${url#https://}"; h="${h%%/*}"
    "$root/tests/harness/netguard.sh" --kind download --host "$h" --watch "$part" -- curl -fsSL -o "$part" "$url" \
      || die "$n: download failed: $url"
    got="$(sha "$part")"
    [ "$got" = "$sum" ] || die "$n: sha256 $got, expected $sum ($url)"
    chmod 0644 "$part"   # mktemp makes it 0600; the download kept curl's umask mode before
    mv -f "$part" "$f"
    trap - EXIT
  fi
  printf '%s\n' "$f"
}

licence() {
  n="$1" dir="$2"
  files="$(get "$n" licence-file)" sums="$(get "$n" licence-sha256)"
  [ -n "$files" ] || die "$n: no licence-file"
  i=1
  for f in $(printf '%s' "$files" | tr ',' ' '); do
    want="$(printf '%s' "$sums" | tr ',' '\n' | sed -n "${i}p" | tr -d ' ')"
    [ -f "$dir/$f" ] || die "$n: licence file $f missing from $dir: licence changed: re-read it (release.md §9)"
    [ "$(sha "$dir/$f")" = "$want" ] || die "$n: $f's sha256 differs: licence changed: re-read it (release.md §9)"
    i=$((i + 1))
  done
}

# the licences any row may have, and those only a build tool may (release.md §9.1 rule 3)
ANY='MIT BSD-2-Clause BSD-3-Clause ISC Zlib Apache-2.0 ZPL-2.1 Unlicense PSF-2.0 LicenseRef-mingw-w64-runtime-notices'
BUILD_ONLY='LGPL-2.1-or-later GPL-2.0-or-later GPL-3.0-or-later'

check() {
  bad=0
  err() { echo "deps.lock: $*" >&2; bad=1; }
  # stanza syntax: every line a comment, blank, [name] or key = value
  awk '!/^[[:space:]]*(#|$)/ && !/^\[[a-z0-9.-]+\]$/ && !/^[a-z0-9-]+ = ./ { print "line " NR ": " $0; e = 1 } END { exit e }' "$lock" \
    || err "lines that are no comment, [name] or key = value (above)"
  dup="$(names | sort | uniq -d)"
  [ -z "$dup" ] || err "names used twice: $dup"
  for n in $(names); do
    lower="$(printf '%s %s' "$n" "$(get "$n" url)" | tr 'A-Z' 'a-z')"
    for b in xwin msvc 'windows sdk' windowssdk microsoft.ai.directml wine; do
      case "$lower" in *"$b"*) err "[$n]: a banned name ($b)" ;; esac
    done
    case "$n" in
      *.*)                              # a component record
        for f in $(get "$n" from | tr ',' ' ') $(get "$n" licence-from); do
          [ -n "$(get "$f" sha256)" ] || err "[$n]: from/licence-from names $f, which isn't a download record"
        done
        use="$(get "$n" use)"
        case "$use" in ships | links | build | test) ;; *) err "[$n]: use = '$use'" ;; esac
        for t in $(get "$n" spdx | sed 's/WITH LLVM-exception//g; s/[()]//g'); do
          case "$t" in AND | OR) continue ;; LicenseRef-*-PublicDomain) continue ;; esac
          ok=0
          for a in $ANY; do [ "$t" = "$a" ] && ok=1; done
          if [ "$use" = build ]; then for a in $BUILD_ONLY; do [ "$t" = "$a" ] && ok=1; done; fi
          [ $ok = 1 ] || err "[$n]: licence $t isn't allowed in a '$use' row (release.md §9.1 rule 3)"
        done
        nf="$(get "$n" licence-file | tr ',' '\n' | grep -c . || true)"
        ns="$(get "$n" licence-sha256 | tr ',' '\n' | grep -cE '^ *[0-9a-f]{64} *$' || true)"
        [ "$nf" -ge 1 ] && [ "$nf" = "$ns" ] || err "[$n]: $nf licence files, $ns licence sha256s"
        notice="$(get "$n" notice)"
        case "$notice" in
          "none ("*)
            if [ "$use" = ships ] || [ "$use" = links ]; then
              case "$(get "$n" spdx)" in
                LicenseRef-*-PublicDomain | Unlicense | "Apache-2.0 WITH LLVM-exception") ;;
                *) err "[$n]: a '$use' row needs a notice" ;;
              esac
            fi ;;
          "NOTICE: "*) h="${notice#NOTICE: }"; grep -qxF "$h" "$root/NOTICE" || err "[$n]: NOTICE has no heading '$h'" ;;
          "win/NOTICE.txt: "*) h="${notice#win/NOTICE.txt: }"; grep -qxF "$h" "$root/win/NOTICE.txt" 2> /dev/null || err "[$n]: win/NOTICE.txt has no heading '$h'" ;;
          *) err "[$n]: notice = '$notice'" ;;
        esac ;;
      *)                                # a download record
        case "$(get "$n" kind)" in archive | file) ;; *) err "[$n]: kind = '$(get "$n" kind)'" ;; esac
        case "$(get "$n" url)" in https://*) ;; *) err "[$n]: url isn't https" ;; esac
        printf '%s' "$(get "$n" sha256)" | grep -qxE '[0-9a-f]{64}' || err "[$n]: sha256 isn't 64 hex digits"
        case "$(get "$n" platform)" in mac | windows | both) ;; *) err "[$n]: platform = '$(get "$n" platform)'" ;; esac ;;
    esac
  done
  # the Mac's pins, where they stay, equal to their rows
  pin() { sed -n "s/^$2=//p" "$root/$1"; }
  same() { [ "$1" = "$2" ] || err "$3: deps.lock says '$1', the script '$2'"; }
  same "$(get flac version)" "$(pin bridge/build.sh FLAC_VERSION)" "flac version (bridge/build.sh)"
  same "$(get flac sha256)" "$(pin bridge/build.sh FLAC_SHA256)" "flac sha256 (bridge/build.sh)"
  same "$(get sqlcipher version)" "$(pin helper/build.sh SQLCIPHER_VERSION)" "sqlcipher version (helper/build.sh)"
  same "$(get sqlcipher sha256)" "$(pin helper/build.sh SQLCIPHER_SHA256)" "sqlcipher sha256 (helper/build.sh)"
  same "$(get pyrekordbox-database version)" "$(pin helper/build.sh PYREKORDBOX_COMMIT)" "pyrekordbox commit (helper/build.sh)"
  same "$(get pyrekordbox-utils version)" "$(pin helper/build.sh PYREKORDBOX_COMMIT)" "pyrekordbox commit (helper/build.sh)"
  same "$(get pyrekordbox-database sha256)" "$(pin helper/build.sh PYREKORDBOX_DATABASE_SHA256)" "pyrekordbox database.py (helper/build.sh)"
  same "$(get pyrekordbox-utils sha256)" "$(pin helper/build.sh PYREKORDBOX_UTILS_SHA256)" "pyrekordbox utils.py (helper/build.sh)"
  same "$(get llvm-mingw version)" "$(pin scripts/win-cc.sh LLVM_MINGW_VERSION | cut -d' ' -f1)" "llvm-mingw version (scripts/win-cc.sh)"
  same "$(get llvm-mingw sha256)" "$(pin scripts/win-cc.sh LLVM_MINGW_SHA256)" "llvm-mingw sha256 (scripts/win-cc.sh)"
  # one way in: the builds on this script download nothing themselves (rule 1)
  for s in helper/build-win.sh; do
    if grep -nE '(^|[^a-z_])(curl|wget|git clone|dotnet tool|nuget)( |$)' "$root/$s" | grep -v '^[0-9]*:[[:space:]]*#'; then
      err "$s downloads without scripts/deps.sh (above)"
    fi
  done
  [ "$bad" = 0 ] || exit 1
  echo "deps.lock: $(names | grep -vc '\.') downloads, $(names | grep -c '\.') components: the gate's offline checks pass"
}

case "${1:-}" in
  fetch) [ $# = 2 ] || die "usage: fetch NAME"; fetch "$2" ;;
  group)
    [ $# = 3 ] || die "usage: group GROUP DIR"
    mkdir -p "$3"
    found=0
    for n in $(names); do
      [ "$(get "$n" group)" = "$2" ] || continue
      p="$(fetch "$n")"
      cp -f "$p" "$3/"
      found=1
    done
    [ $found = 1 ] || die "no download record of group $2" ;;
  licence) [ $# = 3 ] || die "usage: licence NAME DIR"; licence "$2" "$3" ;;
  get) [ $# = 3 ] || die "usage: get NAME KEY"; get "$2" "$3" ;;
  check) check ;;
  *) sed -n '2,15p' "$0" >&2; exit 2 ;;
esac

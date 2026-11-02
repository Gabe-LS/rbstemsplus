#!/bin/zsh
# The measurements of exFAT's and FAT32's "._" companions that need no special hardware, on disk
# images in a scratch folder (attached with hdiutil: run outside a sandbox). Prints its findings;
# nothing is written outside the scratch folder. ENTRIES=DIR measures real saved entries (default
# ~/Library/Caches/rbstemsplus, read only) copied onto an exFAT image with 128 KB clusters, as the
# owner's drive has.
set -u
here=${0:A:h}
S=$(mktemp -d "${TMPDIR:-/tmp}/rbsp-measure.XXXXXX"); S=$(cd $S && pwd -P)
mounts=()
trap 'for m in $mounts; do hdiutil detach -quiet -force $m; done; rm -rf $S' EXIT
clang -O1 -o $S/fsops $here/fsops.c || exit 1
xcrun swiftc -O -o $S/removeitem $here/removeitem.swift 2> /dev/null || exit 1

attach() {   # name fs args...
  local n=$1 fs=$2; shift 2
  hdiutil create -quiet -size 2g -type SPARSE -fs "$fs" "$@" -volname "M$n" $S/$n.sparseimage || return 1
  mkdir -p $S/m-$n && hdiutil attach -quiet -nobrowse -mountpoint $S/m-$n $S/$n.sparseimage || return 1
  mounts+=($S/m-$n)
}
attach exfat ExFAT -fsargs "-b 131072" || { echo "hdiutil failed"; exit 1; }
attach fat32 "MS-DOS FAT32" || exit 1
attach apfs APFS || exit 1

print "== companions (._ files) under the bridge's and the app's calls"
for n in exfat fat32 apfs; do
  print -- "-- $n ($(diskutil info $S/m-$n | awk -F': *' '/Allocation Block Size/ {print $2}'))"
  $S/fsops $S/m-$n/ops
  $S/removeitem $S/m-$n
done

print "\n== disk space per saved entry (st_blocks of .flac, .desc and their ._ files), exFAT 128 KB clusters"
src=${ENTRIES:-$HOME/Library/Caches/rbstemsplus}
python3 -I - $src $S/m-exfat/RB\ Stems\ Plus\ Cache.noindex $S/m-apfs/c << 'PY'
import os, sys, glob
src, dests = sys.argv[1], sys.argv[2:]
entries = sorted(glob.glob(os.path.join(src, "[0-9a-f]" * 2 + "*", "??", "*.flac")))
entries = [e for e in entries if len(os.path.basename(os.path.dirname(os.path.dirname(e)))) == 64]
if not entries: sys.exit("  no saved entries in " + src)
for dest in dests:
    flac = total = 0
    for e in entries:
        key = os.path.basename(e)[:-5]; m = os.path.basename(os.path.dirname(os.path.dirname(e)))
        d = os.path.join(dest, m, key[:2]); os.makedirs(d, exist_ok=True)
        for ext in (".flac", ".desc"):
            s = e[:-5] + ext
            if not os.path.exists(s): continue
            tmp = os.path.join(d, key + ext + ".1.tmp")       # as the bridge: a temp file, then a rename
            with open(s, "rb") as i, open(tmp, "xb") as o: o.write(i.read())
            os.rename(tmp, os.path.join(d, key + ext))
        for name in (key + ".flac", key + ".desc", "._" + key + ".flac", "._" + key + ".desc"):
            p = os.path.join(d, name)
            if os.path.exists(p): total += os.lstat(p).st_blocks * 512
        flac += os.path.getsize(os.path.join(d, key + ".flac"))
    print("  %-50s %d entries, FLAC %.1f MB, on disk %.1f MB: %+.1f%% (%.0f KB per entry over the FLAC)"
          % (dest.split("/m-")[1], len(entries), flac / 2**20, total / 2**20, 100 * (total - flac) / flac, (total - flac) / len(entries) / 1024))
PY

print "\n== st_dev and st_ino of a folder over 5 detach/attach cycles and 10 minutes"
for n in exfat fat32; do
  d=$S/m-$n/ino; mkdir -p $d; first="$(stat -f '%d %i' $d)"; same=1
  for i in 1 2 3 4 5; do
    hdiutil detach -quiet $S/m-$n && hdiutil attach -quiet -nobrowse -mountpoint $S/m-$n $S/$n.sparseimage
    [ "$(stat -f '%d %i' $d)" = "$first" ] || { same=0; print "  $n: changed after cycle $i: $first -> $(stat -f '%d %i' $d)"; }
  done
  print "  $n: dev ino $first; after 5 cycles: $([ $same = 1 ] && echo unchanged || echo CHANGED)"
done
sleep 600
for n in exfat fat32; do print "  $n after 10 minutes: $(stat -f '%d %i' $S/m-$n/ino)"; done

print "\n== what DiskArbitration says about this Mac's disks (Q10)"
for v in / $S/m-exfat; do
  diskutil info $v | awk -F': *' -v v=$v '/Device Location|Protocol|Removable Media|Solid State|^ *Internal/ {gsub(/^ +/, "", $1); printf "  %s: %s = %s\n", v, $1, $2}'
done

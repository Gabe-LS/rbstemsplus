#!/bin/bash
# Usage: gen-formats.sh SOURCE_V1 NEW_OUTPUT_DIR
# SOURCE_V1 is the generated corpus from helper/tests/perf.sh corpus.
set -euo pipefail
export LC_ALL=C

die() { printf '%s\n' "$*" >&2; exit 1; }
[ "$#" -eq 2 ] || die "usage: $0 SOURCE_V1 NEW_OUTPUT_DIR"
for tool in ffmpeg ffprobe afconvert flac python3 shasum; do
  command -v "$tool" >/dev/null || die "missing $tool"
done

src="$(cd "$1" && pwd -P)"
sources=(09-flac-16.flac 10-flac-24-48k.flac 11-flac-24-96k.flac 12-wav-16.wav
         13-wav-24-48k.wav 14-wav-f32.wav 16-aiff-16.aif 17-aiff-24-48k.aif)
for file in "${sources[@]}"; do
  [ -f "$src/$file" ] || die "missing v1 source: $src/$file"
done

out="$2"
[ ! -e "$out" ] && [ ! -L "$out" ] || die "output already exists: $out"
parent="$(cd "$(dirname "$out")" && pwd -P)"
out="$parent/$(basename "$out")"
tmp="$(mktemp -d "$parent/.formats.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT
mkdir "$tmp/files"
rows="$tmp/files/rows.tsv"
printf 'row\tfile\tcodec\trate_hz\tchannels\tdepth\tscope\tsource\tstart_s\tscale\n' > "$rows"
(cd "$src" && shasum -a 256 "${sources[@]}") > "$tmp/files/SOURCE-SHA256SUMS"
{
  ffmpeg -version | sed -n '1p'
  ffprobe -version | sed -n '1p'
  flac --version
  sw_vers
} > "$tmp/files/TOOLS"

ff=(ffmpeg -nostdin -hide_banner -loglevel error -y -filter_threads 1)
bitexact=(-threads:a 1 -fflags +bitexact -flags:a +bitexact -map_metadata -1 -map_chapters -1)
serial=0

emit() {
  local scope="$1" recipe="$2" ext="$3" rate="$4" channels="$5" depth="$6"
  local row="fmt-$recipe-$rate-${channels}ch-$depth-$ext"
  local dest="$tmp/files/$row.$ext" pcm="$tmp/source.wav"
  local source start scale filter layout codec profile='' tag='' width='' ffmux afmux brand=''
  local -a opts=()
  case "$ext" in
    m4a) ffmux=ipod; afmux=m4af; brand='M4A ' ;;
    mp4) ffmux=mp4; afmux=mp4f; brand=isom ;;
    mp3) ffmux=mp3 ;;
    flac|fla) ffmux=flac ;;
    wav) ffmux=wav ;;
    aif|aiff|aifc) ffmux=aiff ;;
  esac

  # Different starts and levels keep rows distinct even after resampling or a mono fold.
  source="${sources[serial % ${#sources[@]}]}"
  start=$((1 + serial / ${#sources[@]} % 12))
  printf -v scale '0.%04d' "$((5100 + 7 * serial))"
  serial=$((serial + 1))
  filter="volume=$scale"
  case "$channels" in
    1) filter+=",pan=mono|c0=0.58*c0+0.42*c1" ;;
    2) ;;
    *)
      case "$channels" in
        3) layout=3.0 ;; 4) layout=quad ;; 5) layout=5.0 ;;
        6) layout=5.1 ;; 7) layout=6.1 ;; 8) layout=7.1 ;;
      esac
      local -a extra=('0.45*c0+0.25*c1' '0.3*c1' '0.4*c1' '0.35*c0' '0.6*c0-0.2*c1' '0.6*c1-0.2*c0')
      filter+=",pan=$layout|c0=c0|c1=c1"
      local i
      for ((i = 2; i < channels; i++)); do filter+="|c$i=${extra[i - 2]}"; done
      ;;
  esac

  case "$recipe" in
    aac-lc|aac-lc-vbr)
      codec=aac; profile=LC
      if [ "$recipe" = aac-lc-vbr ]; then opts=(-q:a 2)
      elif [ "$channels" -eq 1 ]; then opts=(-b:a 64k)
      elif [ "$channels" -eq 2 ]; then opts=(-b:a 96k)
      else opts=(-b:a 192k)
      fi
      opts+=(-profile:a aac_low)
      ;;
    aac-he|aac-hev2)
      codec=aac
      local afcodec=aach
      profile=HE-AAC
      if [ "$recipe" = aac-hev2 ]; then afcodec=aacp; profile=HE-AACv2; fi
      if [ "$ext" = mp4 ]; then brand=mp42; fi
      "${ff[@]}" -ss "$start" -i "$src/$source" -t 30 -af "$filter" -ar "$rate" -c:a pcm_s32le "${bitexact[@]}" "$pcm"
      afconvert -f "$afmux" -d "$afcodec" -b 32000 -s 0 "$pcm" "$dest"
      ;;
    mp3-cbr|mp3-vbr)
      codec=mp3
      if [ "$recipe" = mp3-vbr ]; then opts=(-q:a 4)
      elif [ "$rate" -lt 32000 ]; then opts=(-b:a 64k)
      else opts=(-b:a 128k)
      fi
      opts+=(-write_xing 1 -id3v2_version 0)
      ;;
    alac)
      codec=alac; width="${depth#s}"
      local alac_flag
      case "$width" in 16) alac_flag=1 ;; 20) alac_flag=2 ;; 24) alac_flag=3 ;; 32) alac_flag=4 ;; esac
      if [ "$ext" = mp4 ]; then brand=mp42; fi
      "${ff[@]}" -ss "$start" -i "$src/$source" -t 30 -af "$filter" -ar "$rate" -c:a pcm_s32le "${bitexact[@]}" "$pcm"
      afconvert -f "$afmux" -d "alac/$alac_flag" "$pcm" "$dest"
      ;;
    flac)
      codec=flac; width="${depth#s}"
      case "$width" in
        8)
          # ffmpeg's FLAC encoder promotes 8-bit input to 16-bit.
          "${ff[@]}" -ss "$start" -i "$src/$source" -t 30 -af "$filter" -ar "$rate" -c:a pcm_s8 -f s8 "${bitexact[@]}" "$tmp/source.raw"
          flac --silent --force-raw-format --endian=little --sign=signed --channels="$channels" \
            --bps=8 --sample-rate="$rate" --no-padding --no-seektable -o "$dest" "$tmp/source.raw"
          ;;
        16) opts=(-sample_fmt s16) ;;
        24|32) opts=(-sample_fmt s32 -bits_per_raw_sample "$width" -strict experimental) ;;
      esac
      ;;
    wav-pcm|wav-rf64)
      codec="pcm_${depth}le"
      if [ "$depth" = u8 ]; then codec=pcm_u8; fi
      if [ "$recipe" = wav-rf64 ]; then opts=(-rf64 always); fi
      ;;
    wav-float) codec="pcm_${depth}le" ;;
    aiff) codec="pcm_${depth}be"; if [ "$depth" = s8 ]; then codec=pcm_s8; fi ;;
    aifc-sowt) codec=pcm_s16le; tag=sowt ;;
    aifc-none|aifc-twos|aifc-in24|aifc-in32|aifc-float)
      local affmt="BEI${depth#s}"
      codec="pcm_${depth}be"
      case "$recipe" in
        aifc-none) affmt=I8; codec=pcm_s8; tag=NONE ;;
        aifc-twos) tag=twos ;;
        aifc-in24) tag=in24 ;;
        aifc-in32) tag=in32 ;;
        aifc-float) affmt="BEF${depth#f}"; tag="fl${depth#f}" ;;
      esac
      "${ff[@]}" -ss "$start" -i "$src/$source" -t 30 -af "$filter" -ar "$rate" -c:a pcm_f64le "${bitexact[@]}" "$pcm"
      afconvert -f AIFC -d "$affmt" "$pcm" "$dest"
      ;;
  esac

  if [ ! -f "$dest" ]; then
    local encoder="$codec"
    if [ "$codec" = mp3 ]; then encoder=libmp3lame; fi
    "${ff[@]}" -ss "$start" -i "$src/$source" -t 30 -af "$filter" -ar "$rate" \
      -c:a "$encoder" ${opts[@]+"${opts[@]}"} "${bitexact[@]}" -f "$ffmux" "$dest"
  fi

  # Fail on encoder substitutions (especially HE-AAC, ALAC/FLAC width and AIFF-C tags).
  local info
  info="$(ffprobe -v error -select_streams a:0 \
    -show_entries stream=codec_name,profile,sample_rate,channels,bits_per_raw_sample,codec_tag_string,duration:format_tags=major_brand \
    -of default=noprint_wrappers=1 "$dest")"
  local property
  for property in "codec_name=$codec" "sample_rate=$rate" "channels=$channels" \
    "${profile:+profile=$profile}" "${width:+bits_per_raw_sample=$width}" "${tag:+codec_tag_string=$tag}" \
    "${brand:+TAG:major_brand=$brand}"; do
    [ -z "$property" ] || grep -Fxq "$property" <<< "$info" || die "$row: expected $property; got $info"
  done
  awk -F= '$1 == "duration" && $2 >= 29 && $2 <= 31 { ok=1 } END { exit !ok }' <<< "$info" ||
    die "$row: expected about 30 s; got $info"
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$row" "${dest##*/}" "$codec" "$rate" "$channels" "$depth" "$scope" "$source" "$start" "$scale" >> "$rows"
}

# Published MP3 rates, both documented encoding modes, plus MPEG-2.5 discovery controls.
for rate in 8000 11025 12000 16000 22050 24000 32000 44100 48000; do
  scope=published
  if [ "$rate" -lt 16000 ]; then scope=discovery; fi
  for ch in 1 2; do
    for recipe in mp3-cbr mp3-vbr; do emit "$scope" "$recipe" mp3 "$rate" "$ch" lossy; done
  done
done

for ext in m4a mp4; do
  for rate in 8000 11025 12000 16000 22050 24000 32000 44100 48000 64000 88200 96000; do
    case "$rate" in 16000|22050|24000|32000|44100|48000) scope=published ;; *) scope=discovery ;; esac
    for ch in 1 2; do emit "$scope" aac-lc "$ext" "$rate" "$ch" lossy; done
  done
  for ch in 1 2; do emit published aac-lc-vbr "$ext" 44100 "$ch" lossy; done
  for rate in 16000 22050 24000 32000 44100 48000; do emit discovery aac-he "$ext" "$rate" 2 lossy; done
  for rate in 32000 44100 48000; do emit discovery aac-hev2 "$ext" "$rate" 2 lossy; done
done
emit discovery aac-lc m4a 44100 6 lossy

# Lossless dimensions. Extra rates exercise the resampler, including the documented Mac
# upsampling cases. The public guide does not specify channel counts for these codecs.
for rate in 16000 22050 24000 32000 44100 48000 88200 96000 176400 192000; do
  for ch in 1 2; do
    for depth in s16 s24; do
      case "$rate" in 44100|48000|88200|96000|192000) scope=published ;; *) scope=discovery ;; esac
      emit "$scope" wav-pcm wav "$rate" "$ch" "$depth"
      for ext in aif aiff; do emit "$scope" aiff "$ext" "$rate" "$ch" "$depth"; done
      for ext in flac fla; do emit "$scope" flac "$ext" "$rate" "$ch" "$depth"; done
      emit "$scope" alac m4a "$rate" "$ch" "$depth"
    done
    for depth in u8 s32; do emit discovery wav-pcm wav "$rate" "$ch" "$depth"; done
    for depth in f32 f64; do emit discovery wav-float wav "$rate" "$ch" "$depth"; done
    for depth in s8 s32; do
      emit discovery flac flac "$rate" "$ch" "$depth"
      emit discovery aiff aiff "$rate" "$ch" "$depth"
    done
    for depth in s20 s32; do emit discovery alac m4a "$rate" "$ch" "$depth"; done
  done
done

for ch in 1 2; do
  for rate in 44100 48000; do
    for depth in u8 s16 s24 s32; do emit discovery wav-rf64 wav "$rate" "$ch" "$depth"; done
    emit discovery aifc-none aiff "$rate" "$ch" s8
    emit discovery aifc-twos aiff "$rate" "$ch" s16
    emit discovery aifc-sowt aiff "$rate" "$ch" s16
    emit discovery aifc-in24 aiff "$rate" "$ch" s24
    emit discovery aifc-in32 aiff "$rate" "$ch" s32
    for depth in f32 f64; do emit discovery aifc-float aiff "$rate" "$ch" "$depth"; done
  done
done
emit discovery aifc-sowt aifc 44100 2 s16
emit discovery alac mp4 44100 2 s24
for ch in 3 4 5 6 7 8; do
  emit discovery wav-pcm wav 44100 "$ch" s16
  emit discovery aiff aiff 44100 "$ch" s16
  emit discovery flac flac 44100 "$ch" s16
  emit discovery alac m4a 44100 "$ch" s16
done
# The Mac odd-corpus round observed stems at 8 and 12 kHz FLAC as well.
for rate in 8000 12000; do
  for ch in 1 2; do
    for depth in s16 s24; do
      for ext in flac fla; do emit discovery flac "$ext" "$rate" "$ch" "$depth"; done
    done
    for depth in s8 s32; do emit discovery flac flac "$rate" "$ch" "$depth"; done
  done
done

# afconvert stamps the movie, track and media headers with the wall clock. Clear only those
# timestamps, leaving the sample tables, priming metadata and encoded packets as Apple wrote them.
python3 -I - "$tmp/files" <<'PY'
from collections import Counter
from pathlib import Path
from struct import unpack
import sys


def clear_dates(stream, start, end, seen):
    pos = start
    while pos < end:
        stream.seek(pos)
        size, kind = unpack(">I4s", stream.read(8))
        if size < 8 or size > end - pos:
            raise ValueError(f"invalid box {kind!r} at {pos}")
        if kind in (b"moov", b"trak", b"mdia"):
            clear_dates(stream, pos + 8, pos + size, seen)
        elif kind in (b"mvhd", b"tkhd", b"mdhd"):
            version = stream.read(1)[0]
            if version not in (0, 1) or size < 12 + 8 * (version + 1):
                raise ValueError(f"invalid header {kind!r} at {pos}")
            stream.seek(pos + 12)
            stream.write(bytes(8 * (version + 1)))
            seen[kind] += 1
        pos += size


root = Path(sys.argv[1])
for file in sorted((*root.glob("fmt-alac-*"), *root.glob("fmt-aac-he*"))):
    seen = Counter()
    with file.open("r+b") as media:
        clear_dates(media, 0, file.stat().st_size, seen)
    if seen != {b"mvhd": 1, b"tkhd": 1, b"mdhd": 1}:
        raise ValueError(f"{file.name}: expected one movie, track and media header")
PY

# Kit row names are case-insensitively unique. Hash only media, in filename order.
awk -F '\t' 'NR > 1 && seen[tolower($1)]++ { print "duplicate row: " $1 > "/dev/stderr"; bad=1 }
  END { exit bad }' "$rows"
(
  cd "$tmp/files"
  shasum -a 256 fmt-* > SHA256SUMS
)
# A concurrent run must never move its files into a destination another run has published.
python3 -I -c 'import os, sys; os.rename(sys.argv[1], sys.argv[2])' "$tmp/files" "$out"
printf 'formats: %d verified audio files in %s\n' "$serial" "$out"
